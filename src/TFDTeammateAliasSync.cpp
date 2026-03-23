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
                return;
            }

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
                        g_registry.teammateAliases[slot - 1] = refAlias;
                    }
                } catch (...) {
                }
            }

            std::size_t found = 0;
            for (auto* alias : g_registry.teammateAliases) {
                if (alias) {
                    ++found;
                }
            }
            spdlog::info("[TFD][TeammateAlias] registry resolved quest={:08X} aliases={}",
                g_registry.quest ? g_registry.quest->GetFormID() : 0u, found);
        }

        static bool IsValidTeammate(RE::Actor* actor)
        {
            return actor && actor != Player() && !actor->IsDisabled() && !actor->IsDead() && actor->IsPlayerTeammate();
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
            TFD::ActorScan::Rescan(useRadius, true);
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
            if (!g_registry.quest) {
                return;
            }

            auto desired = CollectNearbyPlayerTeammates(8000.0f);
            std::unordered_set<RE::FormID> keepIDs;
            for (auto* actor : desired) {
                if (actor) {
                    keepIDs.insert(actor->GetFormID());
                }
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
                if (!IsValidTeammate(current) || !keepIDs.contains(current->GetFormID())) {
                    spdlog::info("[TFD][TeammateAlias] clear alias='{}' actor={:08X}", alias->aliasName.c_str(), current->GetFormID());
                    WriteAlias(alias, nullptr);
                    continue;
                }
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
