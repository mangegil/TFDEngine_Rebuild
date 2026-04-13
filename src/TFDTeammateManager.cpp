#include "TFDTeammateManager.h"
#include "TFDActor.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <string_view>
#include <limits>
#include <cmath>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "TFDDefeatMonitor.h"
#include "TFDFlowController.h"
#include "TFDSettings.h"
#include "TFDHostilityController.h"
#include "TFDTame.h"

namespace
{
    namespace RestoreInternal
    {
        using Clock = std::chrono::steady_clock;

        struct QuestCache
        {
            bool resolved{ false };
            RE::TESQuest* quest{ nullptr };
            std::array<RE::BGSRefAlias*, 6> teammateAliases{};
        };

        enum class RestoreResult
        {
            kRestored,
            kRetryLater,
            kFailed
        };

        struct RestorePassStats
        {
            std::size_t total{ 0 };
            std::size_t restored{ 0 };
            std::size_t retryLater{ 0 };
            std::size_t failed{ 0 };
        };

        inline QuestCache g_cache{};
        inline std::mutex g_lock{};
        inline std::atomic_bool g_restoreQueued{ false };
        inline std::atomic_uint64_t g_restoreGeneration{ 0 };

        constexpr double kRestoreCompanionHours = 3.0;
        constexpr std::size_t kRestoreRetryCount = 5;
        constexpr auto kRestoreRetryDelay = std::chrono::milliseconds(1500);
        constexpr auto kRestoreTaskTimeout = std::chrono::seconds(10);
        constexpr const char* kCreatureTeammateAssignEvent = "TFDCreatureTeammateAssign";
        constexpr const char* kTameUnassignEvent = "TFDTameUnassign";

        RE::PlayerCharacter* Player()
        {
            return RE::PlayerCharacter::GetSingleton();
        }

        double NowSec()
        {
            static const auto t0 = Clock::now();
            return std::chrono::duration<double>(Clock::now() - t0).count();
        }

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

        void ResolveQuest()
        {
            if (g_cache.resolved) {
                return;
            }

            g_cache.resolved = true;
            g_cache.quest = RE::TESForm::LookupByEditorID<RE::TESQuest>("TFDCreatureTeammateQuest");
            if (!g_cache.quest) {
                spdlog::warn("[TFD][TeammateManager] quest TFDCreatureTeammateQuest not found");
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
                        const auto index = static_cast<std::size_t>(slot - 1);
                        g_cache.teammateAliases[index] = refAlias;
                    }
                } catch (...) {
                }
            }

            std::size_t found = 0;
            for (auto* alias : g_cache.teammateAliases) {
                if (alias) {
                    ++found;
                }
            }

            spdlog::info("[TFD][TeammateManager] restore quest resolved quest={:08X} aliases={}",
                g_cache.quest ? g_cache.quest->GetFormID() : 0u,
                found);
        }

        bool IsAliasRestoreCandidate(RE::Actor* actor)
        {
            return actor && actor != Player() && !actor->IsDead() && !actor->IsDisabled();
        }

        RestoreResult RestoreActor(RE::Actor* actor)
        {
            if (!IsAliasRestoreCandidate(actor)) {
                return RestoreResult::kFailed;
            }

            if (TFD::Tame::IsCompanion(actor)) {
                SendAssignEvent(actor);
                if (actor->Is3DLoaded()) {
                    actor->EvaluatePackage();
                }

                spdlog::info("[TFD][TeammateManager] actor already companion actor={:08X} reprime=1", actor->GetFormID());
                return RestoreResult::kRestored;
            }

            if (!actor->Is3DLoaded()) {
                spdlog::info("[TFD][TeammateManager] actor not ready yet actor={:08X} loaded=0", actor->GetFormID());
                return RestoreResult::kRetryLater;
            }

            if (TFD::Tame::HasActiveSession(actor)) {
                const bool promoted = TFD::Tame::PromoteToCompanion(actor, kRestoreCompanionHours);
                if (!promoted) {
                    spdlog::warn("[TFD][TeammateManager] promote existing failed actor={:08X}", actor->GetFormID());
                    return RestoreResult::kRetryLater;
                }

                SendUnassignTameEvent(actor);
                SendAssignEvent(actor);
                actor->EvaluatePackage();

                spdlog::info("[TFD][TeammateManager] promote existing actor={:08X} ok=1", actor->GetFormID());
                return RestoreResult::kRestored;
            }

            auto* player = Player();
            if (!player) {
                return RestoreResult::kRetryLater;
            }

            const auto started = TFD::Tame::BeginSession(player, actor, NowSec(), false, false);
            if (!started.has_value()) {
                spdlog::warn("[TFD][TeammateManager] begin tame failed actor={:08X}", actor->GetFormID());
                return RestoreResult::kRetryLater;
            }

            const bool promoted = TFD::Tame::PromoteToCompanion(actor, kRestoreCompanionHours);
            if (!promoted) {
                TFD::Tame::Release(actor, TFD::Tame::ReleaseReason::Generic);
                spdlog::warn("[TFD][TeammateManager] promote failed after restore begin actor={:08X}", actor->GetFormID());
                return RestoreResult::kRetryLater;
            }

            SendUnassignTameEvent(actor);
            SendAssignEvent(actor);
            actor->EvaluatePackage();

            spdlog::info("[TFD][TeammateManager] restored actor={:08X} name='{}' hours={:.2f} reprime=1",
                actor->GetFormID(),
                actor->GetName() ? actor->GetName() : "",
                kRestoreCompanionHours);
            return RestoreResult::kRestored;
        }

        RestorePassStats RestorePass()
        {
            std::scoped_lock lock(g_lock);
            ResolveQuest();

            RestorePassStats stats{};
            if (!g_cache.quest) {
                return stats;
            }

            for (auto* alias : g_cache.teammateAliases) {
                if (!alias) {
                    continue;
                }

                auto* actor = alias->GetActorReference();
                if (!actor) {
                    continue;
                }

                ++stats.total;

                switch (RestoreActor(actor)) {
                case RestoreResult::kRestored:
                    ++stats.restored;
                    break;
                case RestoreResult::kRetryLater:
                    ++stats.retryLater;
                    break;
                case RestoreResult::kFailed:
                default:
                    ++stats.failed;
                    break;
                }
            }

            return stats;
        }

        void QueueRestoreAfterLoad()
        {
            const std::uint64_t generation = g_restoreGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
            g_restoreQueued.store(true, std::memory_order_release);

            std::thread([generation]() {
                for (std::size_t attempt = 1; attempt <= kRestoreRetryCount; ++attempt) {
                    std::this_thread::sleep_for(kRestoreRetryDelay);

                    if (g_restoreGeneration.load(std::memory_order_acquire) != generation) {
                        return;
                    }

                    auto* task = SKSE::GetTaskInterface();
                    if (!task) {
                        continue;
                    }

                    auto promise = std::make_shared<std::promise<RestorePassStats>>();
                    auto future = promise->get_future();

                    task->AddTask([promise]() {
                        try {
                            promise->set_value(RestorePass());
                        } catch (...) {
                            try {
                                promise->set_exception(std::current_exception());
                            } catch (...) {
                            }
                        }
                    });

                    if (future.wait_for(kRestoreTaskTimeout) != std::future_status::ready) {
                        spdlog::warn("[TFD][TeammateManager] restore pass timeout attempt={}", attempt);
                        continue;
                    }

                    RestorePassStats stats{};
                    try {
                        stats = future.get();
                    } catch (...) {
                        spdlog::warn("[TFD][TeammateManager] restore pass exception attempt={}", attempt);
                        continue;
                    }

                    spdlog::info("[TFD][TeammateManager] restore pass attempt={} total={} restored={} retryLater={} failed={}",
                        attempt,
                        stats.total,
                        stats.restored,
                        stats.retryLater,
                        stats.failed);

                    if (stats.retryLater == 0) {
                        break;
                    }
                }

                if (g_restoreGeneration.load(std::memory_order_acquire) == generation) {
                    g_restoreQueued.store(false, std::memory_order_release);
                }
            }).detach();
        }
    }

    namespace AliasInternal
    {
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

        inline RegistryCache g_registry{};
        inline std::atomic_bool g_installed{ false };
        inline std::atomic_bool g_running{ false };
        inline std::atomic_bool g_tickPending{ false };
        inline std::thread g_worker{};
        inline std::mutex g_syncLock{};

        RE::PlayerCharacter* Player()
        {
            return RE::PlayerCharacter::GetSingleton();
        }

        void ResolveRegistry()
        {
            if (g_registry.resolved) {
                return;
            }
            g_registry.resolved = true;
            g_registry.quest = RE::TESForm::LookupByEditorID<RE::TESQuest>("TFDPlayerTeammateQuest");
            if (!g_registry.quest) {
                spdlog::warn("[TFD][TeammateManager] quest TFDPlayerTeammateQuest not found");
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
                        if (slot >= 1 && slot <= static_cast<int>(g_registry.teammateAliases.size())) {
                            const auto index = static_cast<std::size_t>(slot - 1);
                            g_registry.teammateAliases[index] = refAlias;
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
                spdlog::warn("[TFD][TeammateManager] faction TFDTeammateFaction not found");
            }

            std::size_t found = 0;
            for (auto* alias : g_registry.teammateAliases) {
                if (alias) {
                    ++found;
                }
            }
            spdlog::info("[TFD][TeammateManager] alias registry resolved quest={:08X} aliases={} faction={:08X} currentFollowerFaction={:08X} playerFollowerFaction={:08X} teammateState={:08X}",
                g_registry.quest ? g_registry.quest->GetFormID() : 0u,
                found,
                g_registry.teammateFaction ? g_registry.teammateFaction->GetFormID() : 0u,
                g_registry.currentFollowerFaction ? g_registry.currentFollowerFaction->GetFormID() : 0u,
                g_registry.playerFollowerFaction ? g_registry.playerFollowerFaction->GetFormID() : 0u,
                g_registry.teammateStateGlobal ? g_registry.teammateStateGlobal->GetFormID() : 0u);
        }

        bool IsValidTeammate(RE::Actor* actor)
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

        bool IsCombatCapableTeammate(RE::Actor* actor)
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

        int ComputeTeammateStateValue(const std::vector<RE::Actor*>& teammates)
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

        void WriteTeammateState(int value)
        {
            ResolveRegistry();
            if (!g_registry.teammateStateGlobal) {
                return;
            }

            g_registry.teammateStateGlobal->value = static_cast<float>(value);
        }

        void SyncTeammateFaction(RE::Actor* actor, bool shouldHaveFaction)
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
                    spdlog::info("[TFD][TeammateManager] add faction actor={:08X} faction={:08X}",
                        actor->GetFormID(),
                        g_registry.teammateFaction->GetFormID());
                }
            } else if (hasFaction) {
                actor->RemoveFromFaction(g_registry.teammateFaction);
                actor->EvaluatePackage(false, true);
                actor->EvaluatePackage(true, true);
                spdlog::info("[TFD][TeammateManager] remove faction actor={:08X} faction={:08X}",
                    actor->GetFormID(),
                    g_registry.teammateFaction->GetFormID());
            }
        }

        void WriteAlias(RE::BGSRefAlias* alias, RE::Actor* actor)
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

        std::vector<RE::Actor*> CollectNearbyPlayerTeammates(float radius)
        {
            std::vector<RE::Actor*> out;
            auto* player = Player();
            if (!player) {
                return out;
            }

            const float useRadius = (std::max)(radius, 6000.0f);
            TFD::Actor::Scan::Rescan(useRadius, false);
            const auto count = TFD::Actor::Scan::GetCount();
            std::unordered_set<RE::FormID> seen;
            out.reserve(8);

            for (int i = 0; i < count; ++i) {
                auto entry = TFD::Actor::Scan::GetEntry(i);
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

        void SyncAliasesImpl()
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
                    spdlog::info("[TFD][TeammateManager] clear alias='{}' actor={:08X} reason=invalid",
                        alias->aliasName.c_str(), current->GetFormID());
                    SyncTeammateFaction(current, false);
                    WriteAlias(alias, nullptr);
                    continue;
                }

                SyncTeammateFaction(current, true);
                remaining.erase(std::remove_if(remaining.begin(), remaining.end(), [&](RE::Actor* actor) {
                    return actor == current || (actor && current && actor->GetFormID() == current->GetFormID());
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
                spdlog::info("[TFD][TeammateManager] fill alias='{}' actor={:08X} name='{}'",
                    alias->aliasName.c_str(),
                    actor ? actor->GetFormID() : 0u,
                    actor && actor->GetName() ? actor->GetName() : "");
            }
        }

        void TickUI()
        {
            SyncAliasesImpl();
            g_tickPending.store(false, std::memory_order_release);
        }

        void WorkerLoop()
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
}


namespace TFD::TeammateManager::BridgeInternal
{
        constexpr const char* kDefeatedHumanoidRecruitEvent = "TFDDefeatedHumanoidRecruit";
        constexpr const char* kHumanoidTeammateAssignEvent = "TFDHumanoidTeammateAssign";
        constexpr double kDefeatedReentrySuppressSeconds = 6.0;

        inline RuntimeProviders g_runtimeProviders{};

        class DefeatedRecruitEventSink final : public RE::BSTEventSink<SKSE::ModCallbackEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* ev, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
            {
                if (!ev) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                std::string_view name(ev->eventName);
                if (name.empty() || name != kDefeatedHumanoidRecruitEvent) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                auto* actor = g_runtimeProviders.resolvePendingDefeatedDialogueTarget ? g_runtimeProviders.resolvePendingDefeatedDialogueTarget() : nullptr;
                if (!actor) {
                    spdlog::warn("[TFD][TeammateManager] defeated humanoid recruit event ignored reason=no_pending_target");
                    return RE::BSEventNotifyControl::kContinue;
                }
                const bool ok = TFD::TeammateManager::RecruitDefeatedHumanoidAsTeammate(actor);
                spdlog::info("[TFD][TeammateManager] defeated humanoid recruit event actor={} ok={}", static_cast<std::uint32_t>(actor->GetFormID()), ok ? 1 : 0);
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        inline DefeatedRecruitEventSink g_defeatedRecruitEventSink{};

        bool HasFollowerAnchorFaction(RE::Actor* actor)
        {
            if (!actor || actor->IsDisabled()) {
                return false;
            }
            AliasInternal::ResolveRegistry();
            if (AliasInternal::g_registry.currentFollowerFaction && actor->IsInFaction(AliasInternal::g_registry.currentFollowerFaction)) {
                return true;
            }
            if (AliasInternal::g_registry.playerFollowerFaction && actor->IsInFaction(AliasInternal::g_registry.playerFollowerFaction)) {
                return true;
            }
            return false;
        }

        std::vector<RE::Actor*> CollectRegisteredActors()
        {
            AliasInternal::ResolveRegistry();
            std::vector<RE::Actor*> out;
            out.reserve(AliasInternal::g_registry.teammateAliases.size());
            for (auto* alias : AliasInternal::g_registry.teammateAliases) {
                if (!alias) {
                    continue;
                }
                auto* actor = alias->GetActorReference();
                if (!actor || actor->IsDisabled()) {
                    continue;
                }
                out.push_back(actor);
            }
            return out;
        }

        float Distance3D(const RE::NiPoint3& a, const RE::NiPoint3& b)
        {
            const float dx = a.x - b.x;
            const float dy = a.y - b.y;
            const float dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }
    }

namespace TFD::TeammateManager
{
    void Install()
    {
        if (AliasInternal::g_installed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        if (auto* msg = SKSE::GetMessagingInterface()) {
            msg->RegisterListener([](SKSE::MessagingInterface::Message* message) {
                if (!message) {
                    return;
                }

                if (message->type == SKSE::MessagingInterface::kPreLoadGame) {
                    RestoreInternal::g_restoreGeneration.fetch_add(1, std::memory_order_acq_rel);
                    RestoreInternal::g_restoreQueued.store(false, std::memory_order_release);
                    return;
                }

                if (message->type == SKSE::MessagingInterface::kPostLoadGame) {
                    RestoreInternal::QueueRestoreAfterLoad();
                }
            });
        } else {
            spdlog::warn("[TFD][TeammateManager] MessagingInterface null");
        }

        if (auto* src = SKSE::GetModCallbackEventSource()) {
            src->AddEventSink(&BridgeInternal::g_defeatedRecruitEventSink);
        }

        AliasInternal::g_running.store(true, std::memory_order_release);
        AliasInternal::g_tickPending.store(false, std::memory_order_release);
        AliasInternal::g_worker = std::thread([]() { AliasInternal::WorkerLoop(); });
        spdlog::info("[TFD][TeammateManager] installed");
    }

    void Shutdown()
    {
        if (!AliasInternal::g_installed.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        AliasInternal::g_running.store(false, std::memory_order_release);
        if (AliasInternal::g_worker.joinable()) {
            AliasInternal::g_worker.join();
        }
        AliasInternal::g_tickPending.store(false, std::memory_order_release);
        if (auto* src = SKSE::GetModCallbackEventSource()) {
            src->RemoveEventSink(&BridgeInternal::g_defeatedRecruitEventSink);
        }
        BridgeInternal::g_runtimeProviders = {};
        spdlog::info("[TFD][TeammateManager] shutdown");
    }

    void SyncNow()
    {
        AliasInternal::SyncAliasesImpl();
    }
    std::size_t RestoreNow()
    {
        return RestoreInternal::RestorePass().restored;
    }

    void InstallRuntimeProviders(RuntimeProviders providers)
    {
        BridgeInternal::g_runtimeProviders = std::move(providers);
    }

    void ResetRuntimeProviders()
    {
        BridgeInternal::g_runtimeProviders = {};
    }

    bool IsActiveFollowerActor(RE::Actor* actor)
    {
        return AliasInternal::IsValidTeammate(actor);
    }

    std::vector<RE::Actor*> CollectRegisteredTeammates()
    {
        return BridgeInternal::CollectRegisteredActors();
    }

    std::vector<RE::Actor*> CollectKnownTeammates(float radius)
    {
        auto* player = AliasInternal::Player();
        std::vector<RE::Actor*> out;
        if (!player) {
            return out;
        }

        std::unordered_set<RE::FormID> seen;
        const float maxRadius = radius > 0.0f ? (std::max)(radius, 5000.0f) : 5000.0f;

        for (auto* actor : CollectRegisteredTeammates()) {
            if (!actor || actor == player || actor->IsDisabled()) {
                continue;
            }
            if (radius > 0.0f) {
                const float dist = BridgeInternal::Distance3D(actor->GetPosition(), player->GetPosition());
                if (dist > maxRadius) {
                    continue;
                }
            }
            seen.insert(actor->GetFormID());
            out.push_back(actor);
        }

        TFD::Actor::Scan::Rescan(maxRadius, false);
        const auto count = TFD::Actor::Scan::GetCount();
        auto* pCell = player->GetParentCell();
        for (int i = 0; i < count; ++i) {
            auto entry = TFD::Actor::Scan::GetEntry(i);
            auto sp = entry.actor.get();
            auto* actor = sp.get();
            if (!actor || actor == player || actor->IsDisabled()) {
                continue;
            }
            if (actor->GetParentCell() != pCell) {
                continue;
            }
            if (entry.dist > maxRadius) {
                continue;
            }
            if (!actor->IsPlayerTeammate() && !BridgeInternal::HasFollowerAnchorFaction(actor)) {
                continue;
            }
            if (!seen.insert(actor->GetFormID()).second) {
                continue;
            }
            out.push_back(actor);
        }

        return out;
    }

    FollowerResolution ResolveFollowerCandidates(float radius)
    {
        FollowerResolution result{};
        auto* player = AliasInternal::Player();
        if (!player) {
            return result;
        }

        float bestStandingDist = std::numeric_limits<float>::max();
        float bestDownedDist = std::numeric_limits<float>::max();
        for (auto* actor : CollectRegisteredTeammates()) {
            if (!actor || actor == player) {
                continue;
            }
            const float dist = BridgeInternal::Distance3D(actor->GetPosition(), player->GetPosition());
            if (radius > 0.0f && dist > (std::max)(radius, 5000.0f)) {
                continue;
            }
            const bool downed = TFD::DefeatMonitor::IsThresholdDownedActor(actor);
            if (!downed) {
                if (dist < bestStandingDist) {
                    bestStandingDist = dist;
                    result.standing = actor;
                }
            } else if (dist < bestDownedDist) {
                bestDownedDist = dist;
                result.downed = actor;
            }
        }
        return result;
    }

    std::vector<RE::Actor*> CollectStandingFollowers(float radius)
    {
        std::vector<RE::Actor*> out;
        for (auto* actor : CollectKnownTeammates(radius)) {
            if (!actor || actor->IsDead() || actor->IsDisabled()) {
                continue;
            }
            if (TFD::DefeatMonitor::IsThresholdDownedActor(actor)) {
                continue;
            }
            out.push_back(actor);
        }
        return out;
    }

    void RecoverVictoryTeammates()
    {
        for (auto* actor : CollectRegisteredTeammates()) {
            if (!actor || actor->IsDead() || actor->IsDisabled()) {
                continue;
            }
            if (actor->IsInCombat()) {
                actor->StopCombat();
            }
            if (auto* process = RE::ProcessLists::GetSingleton()) {
                process->StopCombatAndAlarmOnActor(actor, false);
            }
            if (actor->IsWeaponDrawn()) {
                actor->DrawWeaponMagicHands(false);
            }

            const bool isLockedAlly = BridgeInternal::g_runtimeProviders.hasAllyBleedLock ? BridgeInternal::g_runtimeProviders.hasAllyBleedLock(actor) : false;
            const bool isBleedingOut = BridgeInternal::g_runtimeProviders.isBleedingOutActor ? BridgeInternal::g_runtimeProviders.isBleedingOutActor(actor) : false;
            const bool isDown = isLockedAlly || isBleedingOut || TFD::DefeatMonitor::IsThresholdDownedActor(actor);
            if (!isDown) {
                continue;
            }
            if (BridgeInternal::g_runtimeProviders.restoreActorHealthToSafePct) {
                BridgeInternal::g_runtimeProviders.restoreActorHealthToSafePct(actor,
                    TFD::Settings::GetAllyDownedThresholdPct(),
                    0.12f,
                    0.58f,
                    0.92f,
                    45.0f,
                    "victory_teammate_recover");
            }
        }
    }

    bool RecruitDefeatedHumanoidAsTeammate(RE::Actor* actor)
    {
        if (!actor) {
            return false;
        }
        if (BridgeInternal::g_runtimeProviders.isDialogueCapableDefeatedEnemy && !BridgeInternal::g_runtimeProviders.isDialogueCapableDefeatedEnemy(actor)) {
            return false;
        }
        if (BridgeInternal::g_runtimeProviders.getDefeatedEnemyRemainingSeconds && BridgeInternal::g_runtimeProviders.getDefeatedEnemyRemainingSeconds(actor) <= 0.0) {
            return false;
        }
        if (!TFD::FlowController::QueueBridgeModEvent(BridgeInternal::kHumanoidTeammateAssignEvent, actor)) {
            spdlog::warn("[TFD][TeammateManager] defeated humanoid recruit failed actor={:08X} reason=bridge_assign_failed", actor->GetFormID());
            return false;
        }
        if (BridgeInternal::g_runtimeProviders.suppressDefeatedReentry) {
            BridgeInternal::g_runtimeProviders.suppressDefeatedReentry(actor, BridgeInternal::kDefeatedReentrySuppressSeconds, "defeated_humanoid_recruit");
        }
        if (BridgeInternal::g_runtimeProviders.releaseBleedLock) {
            BridgeInternal::g_runtimeProviders.releaseBleedLock(actor, "defeated_humanoid_recruit", true);
        }
        if (BridgeInternal::g_runtimeProviders.restoreActorHealthToSafePct) {
            BridgeInternal::g_runtimeProviders.restoreActorHealthToSafePct(actor,
                TFD::Settings::GetEnemyDownedThresholdPct(),
                0.12f,
                0.58f,
                0.92f,
                45.0f,
                "defeated_humanoid_recruit");
        }
        if (actor->IsInCombat()) {
            actor->StopCombat();
        }
        actor->DrawWeaponMagicHands(false);
        if (BridgeInternal::g_runtimeProviders.clearPendingDefeatedDialogueTarget) {
            BridgeInternal::g_runtimeProviders.clearPendingDefeatedDialogueTarget();
        }
        spdlog::info("[TFD][TeammateManager] defeated humanoid recruit actor={:08X}", actor->GetFormID());
        return true;
    }

    bool IsCreatureCompanion(RE::Actor* actor)
    {
        return TFD::Tame::IsCompanion(actor);
    }

    double GetRemainingCreatureCompanionHours(RE::Actor* actor)
    {
        return actor ? TFD::Tame::GetRemainingCompanionHours(actor) : 0.0;
    }

    bool ReleaseCreatureCompanion(RE::Actor* actor)
    {
        return actor ? TFD::Tame::Release(actor, TFD::Tame::ReleaseReason::Generic) : false;
    }
}
