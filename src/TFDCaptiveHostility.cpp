#include "TFDCaptiveHostility.h"

#include <unordered_map>
#include <mutex>
#include <chrono>

#include <spdlog/spdlog.h>

#include "TFDSettings.h"
#include "TFDFactionMask.h"
#include "TFDActorScan.h"
#include "TFDDefeatMonitor.h"

namespace TFD::CaptiveHostility
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        std::mutex gMutex;

        // allowlist = FormList (EditorID: TFDAllowedAggressorFactions)
        RE::BGSListForm* gAllowList = nullptr;
        bool gTriedResolve = false;

        struct Entry
        {
            float origAgg{ 0.0f };
            bool hasOrig{ false };

            // Step 1: stop combat hanya sekali (tanpa sheath spam).
            bool didStopCombat{ false };
        };

        // cache original aggression per actor handle
        std::unordered_map<std::uint32_t, Entry> gCache;  // key: native handle

        Clock::time_point gNextTick{};
        const std::chrono::milliseconds gInterval{ 500 };

        // Kalau kamu mau “hanya yang bandel banget”, set 1.0f (jadi cuma 2/3).
        // Tapi untuk pre combat greet yang stabil, biasanya 0.0f lebih aman (clamp 1/2/3).
        constexpr float minAggToClamp = 0.0f;

        static void ResolveAllowList()
        {
            if (gAllowList || gTriedResolve) {
                return;
            }
            gTriedResolve = true;

            gAllowList = RE::TESForm::LookupByEditorID<RE::BGSListForm>(TFD::FactionMask::kAllowListEditorId);
            if (!gAllowList) {
                spdlog::warn("[TFD][CaptiveHostility] allowlist missing (EditorID='{}')", TFD::FactionMask::kAllowListEditorId);
                return;
            }

            spdlog::info("[TFD][CaptiveHostility] allowlist resolved -> {:08X} ({} entries)",
                gAllowList->GetFormID(), gAllowList->forms.size());
        }

        static bool IsAllowlistedActor(RE::Actor* a)
        {
            if (!a || !gAllowList) {
                return false;
            }

            for (auto* f : gAllowList->forms) {
                auto* fac = f ? f->As<RE::TESFaction>() : nullptr;
                if (!fac) {
                    continue;
                }
                if (a->IsInFaction(fac)) {
                    return true;
                }
            }

            return false;
        }

        static void ApplyAggressionZero(RE::Actor* a)
        {
            if (!a) {
                return;
            }

            auto* avo = a->AsActorValueOwner();
            if (!avo) {
                return;
            }

            const std::uint32_t h = a->GetHandle().native_handle();

            auto [it, inserted] = gCache.emplace(h, Entry{});
            auto& e = it->second;

            // snapshot sekali
            if (!e.hasOrig) {
                e.origAgg = avo->GetActorValue(RE::ActorValue::kAggression);
                e.hasOrig = true;
            }

            // Step 1: fokus cuma bikin Aggression = 0 (jangan sheath spam)
            const float cur = avo->GetActorValue(RE::ActorValue::kAggression);
            if (cur > minAggToClamp) {
                avo->SetActorValue(RE::ActorValue::kAggression, 0.0f);
            }

            // Optional: stop combat sekali supaya langsung “adem” tanpa loop.
            if (!e.didStopCombat && a->IsInCombat()) {
                a->StopCombat();
                a->EvaluatePackage(true, false);
                e.didStopCombat = true;
            }

            if (inserted) {
                spdlog::info("[TFD][CaptiveHostility] clamp aggression actor={:08X} handle={} origAgg={}",
                    a->GetFormID(), h, e.origAgg);
            }
        }

        static void RestoreAll()
        {
            if (gCache.empty()) {
                return;
            }

            std::int32_t restored = 0;

            for (auto& it : gCache) {
                const auto h = it.first;
                const Entry& e = it.second;

                auto sp = RE::Actor::LookupByHandle(h);
                auto* a = sp.get();
                if (!a) {
                    continue;
                }

                auto* avo = a->AsActorValueOwner();
                if (!avo) {
                    continue;
                }

                if (e.hasOrig) {
                    avo->SetActorValue(RE::ActorValue::kAggression, e.origAgg);
                    restored++;
                }

                a->EvaluatePackage(true, false);
            }

            spdlog::info("[TFD][CaptiveHostility] restored aggression for {} actor(s)", restored);
            gCache.clear();
        }
    }

    void Tick()
    {
        std::scoped_lock lk(gMutex);

        // kalau mod dimatiin: pastiin clean
        if (!TFD::Settings::GetEnabled()) {
            RestoreAll();
            return;
        }

        if (TFD::DefeatMonitor::IsPlayerBleedHoldTargetBlocked()) {
            RestoreAll();
            return;
        }

        // Patch 1B: captive hostility hanya boleh aktif saat runtime benar-benar berada di fase Captive.
        // Jangan lagi pakai FactionMask::IsActive(), karena classifier faction juga bisa hidup
        // pada bleed/generic pleasure path dan itu bukan captive mode.
        const bool captive = TFD::DefeatMonitor::IsCaptivePhase();
        if (!captive) {
            RestoreAll();
            return;
        }

        ResolveAllowList();
        if (!gAllowList || gAllowList->forms.empty()) {
            return;
        }

        const auto now = Clock::now();
        if (now < gNextTick) {
            return;
        }
        gNextTick = now + gInterval;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }

        // scan dekat player
        const float radius = TFD::Settings::GetSweepRadius();
        TFD::ActorScan::Rescan(radius, true);

        const auto count = TFD::ActorScan::GetCount();
        for (std::int32_t i = 0; i < count; i++) {
            auto e = TFD::ActorScan::GetEntry(i);
            auto* a = e.actor.get().get();
            if (!a || a == player) {
                continue;
            }
            if (a->IsDisabled() || a->IsDead()) {
                continue;
            }

            // FILTER UTAMA: cuma faction2 yang ada di formlist
            if (!IsAllowlistedActor(a)) {
                continue;
            }

            // biar nggak ganggu NPC yg udah netral, kita clamp kalau dia hostile / lagi combat
            if (!e.hostile && !e.inCombat) {
                continue;
            }

            ApplyAggressionZero(a);
        }
    }

    void Reset()
    {
        std::scoped_lock lk(gMutex);
        RestoreAll();
        gAllowList = nullptr;
        gTriedResolve = false;
        gNextTick = {};
    }
}