#include "TFDCompanionRestore.h"

#include "TFDPacify.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

namespace TFD::CompanionRestore
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        struct QuestCache
        {
            bool resolved{ false };
            RE::TESQuest* quest{ nullptr };
            std::array<RE::BGSRefAlias*, 6> teammateAliases{};
        };

        QuestCache g_cache{};
        std::mutex g_lock{};
        std::atomic_bool g_restoreQueued{ false };

        constexpr double kRestoreCompanionHours = 3.0;
        constexpr const char* kCreatureTeammateAssignEvent = "TFDCreatureTeammateAssign";
        constexpr const char* kTameUnassignEvent = "TFDTameUnassign";

        void SendUnassignTameEvent(RE::Actor* actor)
        {
            if (!actor) {
                return;
            }

            auto* src = SKSE::GetModCallbackEventSource();
            if (!src) {
                return;
            }

            SKSE::ModCallbackEvent e(kTameUnassignEvent, "", 0.0f, actor);
            src->SendEvent(&e);
        }

        void SendAssignEvent(RE::Actor* actor)
        {
            if (!actor) {
                return;
            }

            auto* src = SKSE::GetModCallbackEventSource();
            if (!src) {
                return;
            }

            SKSE::ModCallbackEvent e(kCreatureTeammateAssignEvent, "", 0.0f, actor);
            src->SendEvent(&e);
        }

        double NowSec()
        {
            static const auto t0 = Clock::now();
            return std::chrono::duration<double>(Clock::now() - t0).count();
        }

        RE::PlayerCharacter* Player()
        {
            return RE::PlayerCharacter::GetSingleton();
        }

        void ResolveQuest()
        {
            if (g_cache.resolved) {
                return;
            }

            g_cache.resolved = true;
            g_cache.quest = RE::TESForm::LookupByEditorID<RE::TESQuest>("TFDCreatureTeammateQuest");
            if (!g_cache.quest) {
                spdlog::warn("[TFD][CompanionRestore] quest TFDCreatureTeammateQuest not found");
                return;
            }

            for (auto* baseAlias : g_cache.quest->aliases) {
                auto* refAlias = skyrim_cast<RE::BGSRefAlias*>(baseAlias);
                if (!refAlias) {
                    continue;
                }

                const std::string aliasName = refAlias->aliasName.c_str();
                if (aliasName.rfind("Teammate", 0) != 0 || aliasName.size() < 10) {
                    continue;
                }

                try {
                    const int slot = std::stoi(aliasName.substr(9));
                    if (slot >= 1 && slot <= static_cast<int>(g_cache.teammateAliases.size())) {
                        g_cache.teammateAliases[slot - 1] = refAlias;
                    }
                }
                catch (...) {
                }
            }

            std::size_t found = 0;
            for (auto* alias : g_cache.teammateAliases) {
                if (alias) {
                    ++found;
                }
            }

            spdlog::info("[TFD][CompanionRestore] quest resolved quest={:08X} aliases={}",
                g_cache.quest ? g_cache.quest->GetFormID() : 0u, found);
        }

        bool IsRestorableCompanion(RE::Actor* actor)
        {
            return actor &&
                actor != Player() &&
                !actor->IsDead() &&
                !actor->IsDisabled() &&
                actor->Is3DLoaded() &&
                actor->IsPlayerTeammate();
        }

        bool RestoreActor(RE::Actor* actor)
        {
            if (!IsRestorableCompanion(actor)) {
                return false;
            }

            if (TFD::Pacify::IsCompanion(actor)) {
                return false;
            }

            if (TFD::Pacify::HasActiveTameSession(actor)) {
                const bool promoted = TFD::Pacify::PromoteActiveTameToCompanion(actor, kRestoreCompanionHours);
                if (promoted) {
                    SendUnassignTameEvent(actor);
                    SendAssignEvent(actor);
                    actor->EvaluatePackage();
                }
                spdlog::info("[TFD][CompanionRestore] promote existing actor={:08X} ok={}", actor->GetFormID(), promoted ? 1 : 0);
                return promoted;
            }

            auto* player = Player();
            if (!player) {
                return false;
            }

            const auto started = TFD::Pacify::BeginTameSession(player, actor, NowSec(), false, false);
            if (!started) {
                spdlog::warn("[TFD][CompanionRestore] begin tame failed actor={:08X}", actor->GetFormID());
                return false;
            }

            const bool promoted = TFD::Pacify::PromoteActiveTameToCompanion(actor, kRestoreCompanionHours);
            if (!promoted) {
                TFD::Pacify::ReleaseActiveTameActor(actor, TFD::Pacify::ReleaseReason::Generic);
                spdlog::warn("[TFD][CompanionRestore] promote failed after restore begin actor={:08X}", actor->GetFormID());
                return false;
            }

            SendUnassignTameEvent(actor);
            SendAssignEvent(actor);
            actor->EvaluatePackage();

            spdlog::info("[TFD][CompanionRestore] restored actor={:08X} name='{}' hours={:.2f} reprime=1",
                actor->GetFormID(), actor->GetName() ? actor->GetName() : "", kRestoreCompanionHours);
            return true;
        }

        void QueueRestoreAfterLoad()
        {
            if (g_restoreQueued.exchange(true, std::memory_order_acq_rel)) {
                return;
            }

            std::thread([]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([]() {
                        const auto restored = RestoreNow();
                        spdlog::info("[TFD][CompanionRestore] delayed post-load restore restored={}", restored);
                        g_restoreQueued.store(false, std::memory_order_release);
                        });
                }
                else {
                    g_restoreQueued.store(false, std::memory_order_release);
                }
                }).detach();
        }
    }

    std::size_t RestoreNow()
    {
        std::scoped_lock lock(g_lock);
        ResolveQuest();
        if (!g_cache.quest) {
            return 0;
        }

        std::size_t restored = 0;
        for (auto* alias : g_cache.teammateAliases) {
            if (!alias) {
                continue;
            }

            auto* actor = alias->GetActorReference();
            if (!actor) {
                continue;
            }

            if (RestoreActor(actor)) {
                ++restored;
            }
        }

        return restored;
    }

    void OnSkseMessage(SKSE::MessagingInterface::Message* m)
    {
        if (!m) {
            return;
        }

        if (m->type == SKSE::MessagingInterface::kPreLoadGame) {
            g_restoreQueued.store(false, std::memory_order_release);
            return;
        }

        if (m->type == SKSE::MessagingInterface::kPostLoadGame ||
            m->type == SKSE::MessagingInterface::kNewGame) {
            QueueRestoreAfterLoad();
        }
    }
}
