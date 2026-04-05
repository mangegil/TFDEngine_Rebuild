#include "TFDTeammateAliasSync.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include "RE/B/BSAtomic.h"
#include "TFDActorScan.h"
#include "TFDDefeatMonitor.h"

namespace TFD::TeammateAliasSync
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        struct RegistryCache
        {
            bool resolved{ false };
            RE::TESQuest* quest{ nullptr };
            std::array<RE::BGSRefAlias*, 10> teammateAliases{};
            RE::TESFaction* teammateFaction{ nullptr };
            RE::TESFaction* currentFollowerFaction{ nullptr };
            RE::TESFaction* playerFollowerFaction{ nullptr };
            RE::TESGlobal* teammateStateGlobal{ nullptr };
        };

        RegistryCache g_registry{};
        std::atomic_bool g_installed{ false };
        std::atomic_bool g_running{ false };
        std::atomic_bool g_tickPending{ false };
        std::thread g_worker{};
        std::mutex g_syncLock{};

        static RE::PlayerCharacter* Player()
        {
            return RE::PlayerCharacter::GetSingleton();
        }

        static void ResolveRegistry()
        {
            if (g_registry.resolved) {
                return;
            }
            g_registry.resolved = true;
            g_registry.quest = RE::TESForm::LookupByEditorID<RE::TESQuest>("TFDPlayerTeammateQuest");
            if (!g_registry.quest) {
                spdlog::warn("[TFD][TeammateAlias] quest TFDPlayerTeammateQuest not found");
            }

            if (g_registry.quest) {
                for (auto* baseAlias : g_registry.quest->aliases) {
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
                    if (slot >= 1 && slot <= 10) {
                        g_registry.teammateAliases[static_cast<std::array<RE::BGSRefAlias*, 10Ui64>::size_type>(slot) - 1] = refAlias;
                    }
                } catch (...) {
                }
                }
            }

            g_registry.teammateFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDTeammateFaction");
            g_registry.currentFollowerFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>("CurrentFollowerFaction");
            g_registry.playerFollowerFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>("PlayerFollowerFaction");
            g_registry.teammateStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDTeammateState");
            if (!g_registry.teammateFaction) {
                spdlog::warn("[TFD][TeammateAlias] faction TFDTeammateFaction not found");
            }

            std::size_t found = 0;
            for (auto* alias : g_registry.teammateAliases) {
                if (alias) {
                    ++found;
                }
            }
            spdlog::info("[TFD][TeammateAlias] registry resolved quest={:08X} aliases={} faction={:08X} currentFollowerFaction={:08X} playerFollowerFaction={:08X} teammateState={:08X}",
                g_registry.quest ? g_registry.quest->GetFormID() : 0u,
                found,
                g_registry.teammateFaction ? g_registry.teammateFaction->GetFormID() : 0u,
                g_registry.currentFollowerFaction ? g_registry.currentFollowerFaction->GetFormID() : 0u,
                g_registry.playerFollowerFaction ? g_registry.playerFollowerFaction->GetFormID() : 0u,
                g_registry.teammateStateGlobal ? g_registry.teammateStateGlobal->GetFormID() : 0u);
        }

        static bool IsValidTeammate(RE::Actor* actor)
        {
            ResolveRegistry();
            if (!actor || actor == Player() || actor->IsDisabled() || actor->IsDead()) {
                return false;
            }
            if (actor->IsPlayerTeammate()) {
                return true;
            }
            if (g_registry.teammateFaction && actor->IsInFaction(g_registry.teammateFaction)) {
                return true;
            }
            if (g_registry.currentFollowerFaction && actor->IsInFaction(g_registry.currentFollowerFaction)) {
                return true;
            }
            if (g_registry.playerFollowerFaction && actor->IsInFaction(g_registry.playerFollowerFaction)) {
                return true;
            }
            return false;
        }

        static bool IsCombatCapableTeammate(RE::Actor* actor)
        {
            if (!IsValidTeammate(actor)) {
                return false;
            }

            if (TFD::DefeatMonitor::IsThresholdDownedActor(actor)) {
                return false;
            }

            const auto boolFlags = actor->GetActorRuntimeData().boolFlags;
            if (boolFlags.all(RE::Actor::BOOL_FLAGS::kIsInKillMove)) {
                return false;
            }

            return true;
        }

        static int ComputeTeammateStateValue(const std::vector<RE::Actor*>& teammates)
        {
            int activeCount = 0;
            for (auto* actor : teammates) {
                if (!IsCombatCapableTeammate(actor)) {
                    continue;
                }

                ++activeCount;
                if (activeCount > 1) {
                    return 2;
                }
            }

            return activeCount == 1 ? 1 : 0;
        }

        static void WriteTeammateState(int value)
        {
            ResolveRegistry();
            if (!g_registry.teammateStateGlobal) {
                return;
            }

            g_registry.teammateStateGlobal->value = static_cast<float>(value);
        }

        static void SyncTeammateFaction(RE::Actor* actor, bool shouldHaveFaction)
        {
            if (!actor || !g_registry.teammateFaction) {
                return;
            }

            const bool hasFaction = actor->IsInFaction(g_registry.teammateFaction);
            if (shouldHaveFaction) {
                if (!hasFaction) {
                    actor->AddToFaction(g_registry.teammateFaction, 0);
                    actor->EvaluatePackage(false, true);
                    actor->EvaluatePackage(true, true);
                    spdlog::info("[TFD][TeammateAlias] add faction actor={:08X} faction={:08X}",
                        actor->GetFormID(),
                        g_registry.teammateFaction->GetFormID());
                }
            } else if (hasFaction) {
                actor->RemoveFromFaction(g_registry.teammateFaction);
                actor->EvaluatePackage(false, true);
                actor->EvaluatePackage(true, true);
                spdlog::info("[TFD][TeammateAlias] remove faction actor={:08X} faction={:08X}",
                    actor->GetFormID(),
                    g_registry.teammateFaction->GetFormID());
            }
        }

        static void WriteAlias(RE::BGSRefAlias* alias, RE::Actor* actor)
        {
            if (!g_registry.quest || !alias) {
                return;
            }
            RE::ObjectRefHandle handle{};
            if (actor) {
                handle = actor->CreateRefHandle();
            }
            RE::BSWriteLockGuard lock(g_registry.quest->aliasAccessLock);
            auto it = g_registry.quest->refAliasMap.find(alias->aliasID);
            if (actor) {
                if (it != g_registry.quest->refAliasMap.end()) {
                    it->second = handle;
                } else {
                    g_registry.quest->refAliasMap.insert({ alias->aliasID, handle });
                }
            } else {
                if (it != g_registry.quest->refAliasMap.end()) {
                    g_registry.quest->refAliasMap.erase(it);
                }
            }
        }

        static std::vector<RE::Actor*> CollectNearbyPlayerTeammates(float radius)
        {
            std::vector<RE::Actor*> out;
            auto* player = Player();
            if (!player) {
                return out;
            }

            const float useRadius = (std::max)(radius, 6000.0f);
            TFD::ActorScan::Rescan(useRadius, false);
            const auto count = TFD::ActorScan::GetCount();
            std::unordered_set<RE::FormID> seen;
            out.reserve(8);

            for (int i = 0; i < count; ++i) {
                auto entry = TFD::ActorScan::GetEntry(i);
                auto actorSP = entry.actor.get();
                auto* actor = actorSP.get();
                if (!IsValidTeammate(actor)) {
                    continue;
                }
                if (entry.dist > useRadius) {
                    continue;
                }
                if (!seen.insert(actor->GetFormID()).second) {
                    continue;
                }
                out.push_back(actor);
            }

            return out;
        }

        static void SyncAliasesImpl()
        {
            std::scoped_lock lock(g_syncLock);
            ResolveRegistry();

            auto desired = CollectNearbyPlayerTeammates(8000.0f);
            const int teammateState = ComputeTeammateStateValue(desired);
            WriteTeammateState(teammateState);

            if (!g_registry.quest) {
                return;
            }

            std::vector<RE::Actor*> remaining;
            remaining.reserve(desired.size());
            for (auto* actor : desired) {
                if (actor) {
                    remaining.push_back(actor);
                }
            }

            for (auto* alias : g_registry.teammateAliases) {
                if (!alias) {
                    continue;
                }
                auto* current = alias->GetActorReference();
                if (!current) {
                    continue;
                }

                if (!IsValidTeammate(current)) {
                    spdlog::info("[TFD][TeammateAlias] clear alias='{}' actor={:08X} reason=invalid",
                        alias->aliasName.c_str(), current->GetFormID());
                    SyncTeammateFaction(current, false);
                    WriteAlias(alias, nullptr);
                    continue;
                }

                SyncTeammateFaction(current, true);

                // Keep already-registered valid teammates sticky even if the current scan
                // temporarily misses them. This avoids clear/fill churn for freshly promoted
                // creature companions whose follow state can take a moment to settle.
                remaining.erase(std::remove_if(remaining.begin(), remaining.end(), [&](RE::Actor* a) {
                    return a == current || (a && current && a->GetFormID() == current->GetFormID());
                }), remaining.end());
            }

            for (auto* alias : g_registry.teammateAliases) {
                if (!alias || remaining.empty()) {
                    continue;
                }
                auto* current = alias->GetActorReference();
                if (current && IsValidTeammate(current)) {
                    continue;
                }
                auto* actor = remaining.front();
                remaining.erase(remaining.begin());
                WriteAlias(alias, actor);
                SyncTeammateFaction(actor, true);
                spdlog::info("[TFD][TeammateAlias] fill alias='{}' actor={:08X} name='{}'",
                    alias->aliasName.c_str(), actor ? actor->GetFormID() : 0u,
                    actor && actor->GetName() ? actor->GetName() : "");
            }
        }

        static void TickUI()
        {
            SyncAliasesImpl();
            g_tickPending.store(false, std::memory_order_release);
        }

        static void WorkerLoop()
        {
            while (g_running.load(std::memory_order_acquire)) {
                if (!g_tickPending.exchange(true, std::memory_order_acq_rel)) {
                    if (auto* task = SKSE::GetTaskInterface()) {
                        task->AddTask([]() { TickUI(); });
                    } else {
                        g_tickPending.store(false, std::memory_order_release);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
        }
    }

    void Install()
    {
        if (g_installed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        g_running.store(true, std::memory_order_release);
        g_tickPending.store(false, std::memory_order_release);
        g_worker = std::thread([]() { WorkerLoop(); });
        spdlog::info("[TFD][TeammateAlias] installed");
    }

    void Shutdown()
    {
        if (!g_installed.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        g_running.store(false, std::memory_order_release);
        if (g_worker.joinable()) {
            g_worker.join();
        }
        g_tickPending.store(false, std::memory_order_release);
        spdlog::info("[TFD][TeammateAlias] shutdown");
    }

    void SyncNow()
    {
        SyncAliasesImpl();
    }
}
