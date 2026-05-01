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
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "TFDFlowController.h"
#include "TFDSettings.h"
#include "TFDHostilityController.h"
#include "TFDTame.h"
#include "TFDRecruit.h"

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
                    const int slot = std::stoi(aliasName.substr(8));
                    if (slot >= 1 && slot <= static_cast<int>(g_cache.teammateAliases.size())) {
                        const auto index = static_cast<std::size_t>(slot - 1);
                        g_cache.teammateAliases[index] = refAlias;
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
                        }
                        catch (...) {
                            try {
                                promise->set_exception(std::current_exception());
                            }
                            catch (...) {
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
                    }
                    catch (...) {
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
            RE::TESFaction* truceTeammateFaction{ nullptr };
            RE::TESFaction* currentFollowerFaction{ nullptr };
            RE::TESFaction* playerFollowerFaction{ nullptr };
            RE::TESGlobal* teammateStateGlobal{ nullptr };
            RE::TESGlobal* recruitSlotsFreeGlobal{ nullptr };
        };

        inline RegistryCache g_registry{};
        inline std::atomic_bool g_installed{ false };
        inline std::atomic_bool g_running{ false };
        inline std::atomic_bool g_tickPending{ false };
        inline std::thread g_worker{};
        inline std::mutex g_syncLock{};

        inline std::unordered_set<RE::FormID> g_knownConvertedTeammates{};
        inline std::unordered_map<RE::FormID, std::uint32_t> g_invalidAliasStrikes{};
        inline int g_lastRecruitSlotsFreeWritten{ -999 };
        inline int g_lastTeammateStateWritten{ -999 };

        constexpr std::uint32_t kConvertedAliasInvalidGraceTicks = 12;

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
                        const int slot = std::stoi(aliasName.substr(8));
                        if (slot >= 1 && slot <= static_cast<int>(g_registry.teammateAliases.size())) {
                            const auto index = static_cast<std::size_t>(slot - 1);
                            g_registry.teammateAliases[index] = refAlias;
                        }
                    }
                    catch (...) {
                    }
                }
            }

            g_registry.teammateFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDTeammateFaction");
            g_registry.truceTeammateFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDTruceTeammateFaction");
            g_registry.currentFollowerFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>("CurrentFollowerFaction");
            g_registry.playerFollowerFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>("PlayerFollowerFaction");
            g_registry.teammateStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDTeammateState");
            g_registry.recruitSlotsFreeGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDRecruitSlotsFree");
            if (!g_registry.teammateFaction) {
                spdlog::warn("[TFD][TeammateManager] faction TFDTeammateFaction not found");
            }
            if (!g_registry.truceTeammateFaction) {
                spdlog::warn("[TFD][TeammateManager] faction TFDTruceTeammateFaction not found");
            }

            std::size_t found = 0;
            for (auto* alias : g_registry.teammateAliases) {
                if (alias) {
                    ++found;
                }
            }
            spdlog::info("[TFD][TeammateManager] alias registry resolved quest={:08X} aliases={} tfdTeammateFaction={:08X} truceTeammateFaction={:08X} currentFollowerFaction={:08X} playerFollowerFaction={:08X} teammateState={:08X} recruitSlotsFree={:08X}",
                g_registry.quest ? g_registry.quest->GetFormID() : 0u,
                found,
                g_registry.teammateFaction ? g_registry.teammateFaction->GetFormID() : 0u,
                g_registry.truceTeammateFaction ? g_registry.truceTeammateFaction->GetFormID() : 0u,
                g_registry.currentFollowerFaction ? g_registry.currentFollowerFaction->GetFormID() : 0u,
                g_registry.playerFollowerFaction ? g_registry.playerFollowerFaction->GetFormID() : 0u,
                g_registry.teammateStateGlobal ? g_registry.teammateStateGlobal->GetFormID() : 0u,
                g_registry.recruitSlotsFreeGlobal ? g_registry.recruitSlotsFreeGlobal->GetFormID() : 0u);
        }

        RE::Actor* ResolveCombatTarget(RE::Actor* actor)
        {
            if (!actor) {
                return nullptr;
            }

            auto sp = actor->GetActorRuntimeData().currentCombatTarget.get();
            return sp.get();
        }

        bool HasFollowerAnchorFaction(RE::Actor* actor)
        {
            ResolveRegistry();
            if (!actor || actor->IsDisabled() || actor->IsDead()) {
                return false;
            }
            if (g_registry.currentFollowerFaction && actor->IsInFaction(g_registry.currentFollowerFaction)) {
                return true;
            }
            if (g_registry.playerFollowerFaction && actor->IsInFaction(g_registry.playerFollowerFaction)) {
                return true;
            }
            return false;
        }

        bool IsTFDConvertedTeammate(RE::Actor* actor)
        {
            ResolveRegistry();
            if (!actor || actor == Player() || actor->IsDisabled() || actor->IsDead()) {
                return false;
            }

            if (g_registry.teammateFaction && actor->IsInFaction(g_registry.teammateFaction)) {
                return true;
            }
            if (g_registry.truceTeammateFaction && actor->IsInFaction(g_registry.truceTeammateFaction)) {
                return true;
            }

            return false;
        }

        bool IsPermanentTFDConvertedTeammate(RE::Actor* actor)
        {
            ResolveRegistry();
            return actor && g_registry.teammateFaction && actor->IsInFaction(g_registry.teammateFaction);
        }

        bool HasAnyTFDTeammateFactionNow(RE::Actor* actor)
        {
            return IsTFDConvertedTeammate(actor);
        }

        void RememberConvertedTeammate(RE::Actor* actor, const char* reason)
        {
            if (!actor || actor == Player()) {
                return;
            }

            const auto actorId = actor->GetFormID();
            const bool inserted = g_knownConvertedTeammates.insert(actorId).second;
            if (inserted) {
                spdlog::info(
                    "[TFD][TeammateManager] remember converted teammate actor={:08X} reason={}",
                    actorId,
                    reason ? reason : "unknown");
            }
        }

        void ForgetConvertedTeammate(RE::Actor* actor, const char* reason)
        {
            if (!actor) {
                return;
            }

            const auto actorId = actor->GetFormID();
            g_knownConvertedTeammates.erase(actorId);
            g_invalidAliasStrikes.erase(actorId);
            spdlog::info(
                "[TFD][TeammateManager] forget converted teammate actor={:08X} reason={}",
                actorId,
                reason ? reason : "unknown");
        }

        bool WasKnownConvertedTeammate(RE::Actor* actor)
        {
            return actor && g_knownConvertedTeammates.find(actor->GetFormID()) != g_knownConvertedTeammates.end();
        }

        void ResetInvalidAliasStrike(RE::Actor* actor)
        {
            if (!actor) {
                return;
            }
            g_invalidAliasStrikes.erase(actor->GetFormID());
        }

        std::int32_t FactionRank(RE::Actor* actor, RE::TESFaction* faction)
        {
            if (!actor || !faction) {
                return -2;
            }
            return actor->GetFactionRank(faction, false);
        }

        bool HasPlayerHostility(RE::Actor* actor)
        {
            auto* player = Player();
            if (!actor || !player || actor == player) {
                return false;
            }

            auto* actorTarget = ResolveCombatTarget(actor);
            auto* playerTarget = ResolveCombatTarget(player);
            if (actorTarget == player || playerTarget == actor) {
                return true;
            }

            return actor->IsHostileToActor(player);
        }

        bool IsNaturalPlayerSideTeammate(RE::Actor* actor)
        {
            ResolveRegistry();
            if (!actor || actor == Player() || actor->IsDisabled() || actor->IsDead()) {
                return false;
            }

            // R26: Papyrus can temporarily call SetPlayerTeammate(True) while a
            // PreCombat recruit transaction is still pending. Do not let that
            // temporary state enter the teammate alias as a natural follower,
            // otherwise the alias package stack is built before TFDTeammateFaction
            // exists and the final follow package can fail to take over.
            if (TFD::Recruit::IsRecruitCommitPending(actor)) {
                return false;
            }

            if (IsTFDConvertedTeammate(actor)) {
                return false;
            }

            const bool hasNaturalTeammateState = actor->IsPlayerTeammate() || HasFollowerAnchorFaction(actor);
            if (!hasNaturalTeammateState) {
                return false;
            }

            // Natural vanilla/framework followers are recognition-only.
            // They may enter the alias registry only when they are not hostile to the player.
            return !HasPlayerHostility(actor);
        }

        bool IsValidTeammate(RE::Actor* actor)
        {
            return IsTFDConvertedTeammate(actor) || IsNaturalPlayerSideTeammate(actor);
        }

        bool IsCombatCapableTeammate(RE::Actor* actor)
        {
            if (!IsValidTeammate(actor)) {
                return false;
            }

            if (TFD::Actor::IsDownByHealthThreshold(actor, TFD::Settings::GetAllyDownedThresholdPct())) {
                return false;
            }

            const auto boolFlags = actor->GetActorRuntimeData().boolFlags;
            if (boolFlags.all(RE::Actor::BOOL_FLAGS::kIsInKillMove)) {
                return false;
            }

            return true;
        }

        bool IsKnownConvertedAliasActor(RE::Actor* actor);
        bool IsHardInvalidConvertedAliasActor(RE::Actor* actor);

        bool IsAliasUsableForNewRecruitUnsafe(RE::BGSRefAlias* alias)
        {
            if (!alias) {
                return false;
            }

            auto* current = alias->GetActorReference();
            if (!current) {
                return true;
            }

            if (IsValidTeammate(current)) {
                return false;
            }

            if (TFD::Recruit::IsRecruitCommitPending(current)) {
                return false;
            }

            // A temporarily invalid converted teammate still owns its alias until
            // the hard-invalid cleanup path releases it. Do not report that alias
            // as free, or PreCombat can over-admit crowd participants and later hit no_slot.
            if (IsKnownConvertedAliasActor(current) && !IsHardInvalidConvertedAliasActor(current)) {
                return false;
            }

            return true;
        }

        int CountOccupiedRecruitSlotsFromAliasesUnsafe()
        {
            ResolveRegistry();

            int occupied = 0;
            for (auto* alias : g_registry.teammateAliases) {
                if (!alias) {
                    continue;
                }

                auto* actor = alias->GetActorReference();
                if (!actor) {
                    continue;
                }

                if (IsValidTeammate(actor) ||
                    TFD::Recruit::IsRecruitCommitPending(actor) ||
                    (IsKnownConvertedAliasActor(actor) && !IsHardInvalidConvertedAliasActor(actor))) {
                    ++occupied;
                }
            }

            return occupied;
        }

        int ComputeRecruitSlotsFreeUnsafe()
        {
            ResolveRegistry();

            int freeSlots = 0;
            for (auto* alias : g_registry.teammateAliases) {
                if (IsAliasUsableForNewRecruitUnsafe(alias)) {
                    ++freeSlots;
                }
            }

            return std::clamp(freeSlots, 0, static_cast<int>(g_registry.teammateAliases.size()));
        }

        int ComputeTeammateStateValue(const std::vector<RE::Actor*>& teammates, int recruitSlotsFree)
        {
            if (recruitSlotsFree <= 0) {
                return 3;
            }

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

        void WriteRecruitSlotsFree(int value)
        {
            ResolveRegistry();
            if (!g_registry.recruitSlotsFreeGlobal) {
                return;
            }

            const int maxSlots = static_cast<int>(g_registry.teammateAliases.size());
            g_registry.recruitSlotsFreeGlobal->value = static_cast<float>(std::clamp(value, 0, maxSlots));
        }

        void RefreshRecruitCapacityGlobalsUnsafe(const char* reason)
        {
            ResolveRegistry();

            std::vector<RE::Actor*> registeredStateActors;
            registeredStateActors.reserve(g_registry.teammateAliases.size());
            for (auto* alias : g_registry.teammateAliases) {
                if (!alias) {
                    continue;
                }

                auto* actor = alias->GetActorReference();
                if (!actor || !IsValidTeammate(actor)) {
                    continue;
                }

                registeredStateActors.push_back(actor);
            }

            const int freeSlots = ComputeRecruitSlotsFreeUnsafe();
            const int teammateState = ComputeTeammateStateValue(registeredStateActors, freeSlots);
            WriteRecruitSlotsFree(freeSlots);
            WriteTeammateState(teammateState);

            const bool changed = freeSlots != g_lastRecruitSlotsFreeWritten || teammateState != g_lastTeammateStateWritten;
            g_lastRecruitSlotsFreeWritten = freeSlots;
            g_lastTeammateStateWritten = teammateState;

            if (changed || (reason && std::string_view(reason) != "sync_aliases")) {
                spdlog::info(
                    "[TFD][TeammateManager] recruit capacity refresh slotsFree={} teammateState={} registered={} reason={}",
                    freeSlots,
                    teammateState,
                    static_cast<unsigned>(registeredStateActors.size()),
                    reason ? reason : "unknown");
            }
        }

        void SyncTeammateFaction(RE::Actor* actor, bool shouldHaveFaction)
        {
            ResolveRegistry();
            if (!actor) {
                return;
            }

            // R11: this manager owns alias recognition only.
            // It must not grant or remove TFDTeammateFaction for natural vanilla/framework followers.
            // TFDTeammateFaction / TFDTruceTeammateFaction are owned by TFDRecruit and temporary TFD follow flows.
            if (!shouldHaveFaction) {
                spdlog::info("[TFD][TeammateManager] alias side effects skipped actor={:08X} reason=alias_clear_no_faction_mutation",
                    actor->GetFormID());
                return;
            }

            if (IsPermanentTFDConvertedTeammate(actor)) {
                auto* player = Player();
                auto* actorTarget = ResolveCombatTarget(actor);
                auto* playerTarget = ResolveCombatTarget(player);
                const bool actorTargetsPlayer = player && actorTarget == player;
                const bool playerTargetsActor = playerTarget == actor;
                const bool activeCombatConflict =
                    actor->IsInCombat() ||
                    actorTargetsPlayer ||
                    playerTargetsActor ||
                    TFD::Recruit::HasKnownHostileSourceFaction(actor);

                // R29: Do not run the heavy recruit commit path on every teammate
                // alias refresh for raw-only stale hostility. Some converted bandits
                // keep returning IsHostileToActor(Player)=true with no combat target
                // and no active hostile faction. Repeated StopCombat/UpdateCombat/
                // EvaluatePackage calls interrupt the alias follow package and make
                // the actor appear "stuck" at its old package. Only resettle when
                // there is a real combat/faction conflict to clean up.
                if (!activeCombatConflict) {
                    if (TFD::Recruit::IsRawHostileToPlayer(actor, player)) {
                        spdlog::info(
                            "[TFD][TeammateManager] skip heavy alias resettle actor={:08X} reason=raw_only_no_target_no_known_hostile",
                            actor->GetFormID());
                    }
                    return;
                }

                TFD::Recruit::CommitOptions recruitOptions{};
                recruitOptions.sourceFlow = TFD::Recruit::SourceFlow::Teammate;
                recruitOptions.reason = "converted_teammate_alias_refresh";
                recruitOptions.quarantineHostileFactions = true;
                recruitOptions.clearCombat = true;
                recruitOptions.evaluatePackage = true;
                recruitOptions.detailedLog = false;
                recruitOptions.throttleObserve = true;
                recruitOptions.ensurePacifyAlliance = true;
                recruitOptions.applyRuntimeProfile = true;
                TFD::Recruit::CommitRecruit(actor, player, recruitOptions);
                return;
            }

            if (IsTFDConvertedTeammate(actor)) {
                spdlog::info("[TFD][TeammateManager] alias side effects skipped actor={:08X} reason=temporary_tfd_teammate",
                    actor->GetFormID());
                return;
            }

            spdlog::info("[TFD][TeammateManager] alias recognition actor={:08X} type=natural_teammate no_tfd_faction_mutation=1",
                actor->GetFormID());
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
                }
                else {
                    g_registry.quest->refAliasMap.insert({ alias->aliasID, handle });
                }
            }
            else {
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
            auto snapshot = TFD::Actor::BuildSnapshot(useRadius, false);
            std::unordered_set<RE::FormID> seen;
            out.reserve(8);

            for (const auto& info : snapshot.actors) {
                auto* actor = info.get();
                if (!IsValidTeammate(actor)) {
                    continue;
                }
                if (info.dist > useRadius) {
                    continue;
                }
                if (!seen.insert(actor->GetFormID()).second) {
                    continue;
                }
                out.push_back(actor);
            }

            return out;
        }

        bool SameActor(RE::Actor* lhs, RE::Actor* rhs)
        {
            return lhs && rhs && lhs->GetFormID() == rhs->GetFormID();
        }

        bool QueueHumanoidTeammateAssignEvent(RE::Actor* actor, const char* reason)
        {
            if (!actor) {
                return false;
            }

            const bool queued = TFD::FlowController::QueueBridgeModEvent(
                "TFDHumanoidTeammateAssign",
                actor,
                reason ? reason : "teammate_manager_register_now",
                0.0f);

            spdlog::info(
                "[TFD][TeammateManager] queue humanoid assign actor={:08X} reason={} ok={}",
                actor->GetFormID(),
                reason ? reason : "unknown",
                queued ? 1 : 0);

            return queued;
        }

        float DistanceToPlayer(RE::Actor* actor, RE::PlayerCharacter* player)
        {
            if (!actor || !player) {
                return 0.0f;
            }

            const auto a = actor->GetPosition();
            const auto b = player->GetPosition();
            const float dx = a.x - b.x;
            const float dy = a.y - b.y;
            const float dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        void LogInvalidAliasDiagnostic(RE::BGSRefAlias* alias, RE::Actor* actor, const char* action, const char* reason, std::uint32_t strikes)
        {
            ResolveRegistry();
            auto* player = Player();
            auto* actorTarget = ResolveCombatTarget(actor);
            auto* playerTarget = ResolveCombatTarget(player);

            const bool hasActor = actor != nullptr;
            const bool dead = actor && actor->IsDead();
            const bool disabled = actor && actor->IsDisabled();
            const bool loaded = actor && actor->Is3DLoaded();
            const bool playerTeammate = actor && actor->IsPlayerTeammate();
            const bool currentFollower = actor && g_registry.currentFollowerFaction && actor->IsInFaction(g_registry.currentFollowerFaction);
            const bool playerFollower = actor && g_registry.playerFollowerFaction && actor->IsInFaction(g_registry.playerFollowerFaction);
            const bool pendingRecruit = actor && TFD::Recruit::IsRecruitCommitPending(actor);
            const bool inCombat = actor && actor->IsInCombat();
            const bool rawHostile = actor && TFD::Recruit::IsRawHostileToPlayer(actor, player);
            const bool knownConverted = actor && WasKnownConvertedTeammate(actor);
            const bool nowConverted = actor && HasAnyTFDTeammateFactionNow(actor);
            const bool knownHostileFaction = actor && TFD::Recruit::HasKnownHostileSourceFaction(actor);
            const float dist = (actor && player) ? DistanceToPlayer(actor, player) : 0.0f;

            spdlog::info(
                "[TFD][TeammateManager] invalid alias diag alias='{}' actor={:08X} action={} reason={} strikes={} maxGrace={} hasActor={} dead={} disabled={} loaded={} tfdRank={} truceRank={} knownConverted={} nowConverted={} playerTeammate={} currentFollower={} playerFollower={} pendingRecruit={} inCombat={} actorTarget={:08X} playerTarget={:08X} rawHostile={} hostileFaction={} dist={:.1f}",
                alias ? alias->aliasName.c_str() : "",
                actor ? actor->GetFormID() : 0u,
                action ? action : "unknown",
                reason ? reason : "unknown",
                strikes,
                kConvertedAliasInvalidGraceTicks,
                hasActor ? 1 : 0,
                dead ? 1 : 0,
                disabled ? 1 : 0,
                loaded ? 1 : 0,
                FactionRank(actor, g_registry.teammateFaction),
                FactionRank(actor, g_registry.truceTeammateFaction),
                knownConverted ? 1 : 0,
                nowConverted ? 1 : 0,
                playerTeammate ? 1 : 0,
                currentFollower ? 1 : 0,
                playerFollower ? 1 : 0,
                pendingRecruit ? 1 : 0,
                inCombat ? 1 : 0,
                actorTarget ? actorTarget->GetFormID() : 0u,
                playerTarget ? playerTarget->GetFormID() : 0u,
                rawHostile ? 1 : 0,
                knownHostileFaction ? 1 : 0,
                dist);
        }

        bool IsKnownConvertedAliasActor(RE::Actor* actor)
        {
            if (!actor || actor == Player()) {
                return false;
            }

            if (HasAnyTFDTeammateFactionNow(actor)) {
                RememberConvertedTeammate(actor, "known_alias_current_marker");
                return true;
            }

            return WasKnownConvertedTeammate(actor);
        }

        bool IsHardInvalidConvertedAliasActor(RE::Actor* actor)
        {
            if (!actor || actor == Player()) {
                return true;
            }
            return actor->IsDead() || actor->IsDisabled();
        }

        bool ShouldPreserveInvalidConvertedAlias(RE::BGSRefAlias* alias, RE::Actor* actor, const char* reason)
        {
            if (!IsKnownConvertedAliasActor(actor)) {
                LogInvalidAliasDiagnostic(alias, actor, "clear_not_converted", reason, 0);
                return false;
            }

            if (IsHardInvalidConvertedAliasActor(actor)) {
                LogInvalidAliasDiagnostic(alias, actor, "clear_hard_invalid", reason, 0);
                ForgetConvertedTeammate(actor, "hard_invalid_alias_clear");
                return false;
            }

            const auto actorId = actor->GetFormID();
            const std::uint32_t strikes = ++g_invalidAliasStrikes[actorId];
            const bool preserve = strikes <= kConvertedAliasInvalidGraceTicks;

            LogInvalidAliasDiagnostic(alias, actor, preserve ? "preserve_converted_alias" : "clear_grace_expired", reason, strikes);

            if (!preserve) {
                g_invalidAliasStrikes.erase(actorId);
                return false;
            }

            QueueHumanoidTeammateAssignEvent(actor, reason ? reason : "preserve_invalid_converted_alias");
            if (actor->Is3DLoaded()) {
                actor->EvaluatePackage();
            }
            return true;
        }

        std::vector<RE::Actor*> CollectRegisteredConvertedTeammatesUnsafe()
        {
            ResolveRegistry();

            std::vector<RE::Actor*> out;
            out.reserve(g_registry.teammateAliases.size());

            auto* player = Player();
            std::unordered_set<RE::FormID> seen;
            for (auto* alias : g_registry.teammateAliases) {
                if (!alias) {
                    continue;
                }

                auto* actor = alias->GetActorReference();
                if (!actor || actor == player || actor->IsDead() || actor->IsDisabled()) {
                    continue;
                }

                if (!IsTFDConvertedTeammate(actor)) {
                    continue;
                }

                RememberConvertedTeammate(actor, "collect_registered_converted");
                ResetInvalidAliasStrike(actor);

                if (!seen.insert(actor->GetFormID()).second) {
                    continue;
                }

                out.push_back(actor);
            }

            return out;
        }

        std::size_t CatchupRegisteredHumanoidTeammatesAfterLoad(const char* reason, std::size_t attempt)
        {
            std::scoped_lock lock(g_syncLock);
            ResolveRegistry();

            auto* player = Player();
            auto* playerCell = player ? player->GetParentCell() : nullptr;
            if (!player || !playerCell) {
                return 0;
            }

            auto actors = CollectRegisteredConvertedTeammatesUnsafe();
            if (actors.empty()) {
                return 0;
            }

            constexpr float kEvaluateDistance = 2500.0f;
            constexpr float kHardFarDistance = 12000.0f;
            constexpr float kWrongCellFarDistance = 6000.0f;
            constexpr float kMoveOffsetBase = 768.0f;
            constexpr float kMoveOffsetStep = 160.0f;
            constexpr float kMoveBackOffset = -768.0f;

            std::size_t touched = 0;
            for (std::size_t i = 0; i < actors.size(); ++i) {
                auto* actor = actors[i];
                if (!actor || actor == player || actor->IsDead() || actor->IsDisabled()) {
                    continue;
                }

                auto* actorCell = actor->GetParentCell();
                const bool wrongCell = actorCell && actorCell != playerCell;
                const bool unloaded = !actor->Is3DLoaded();
                const float dist = DistanceToPlayer(actor, player);
                const bool far = dist > kHardFarDistance;
                const bool wrongCellFar = wrongCell && dist > kWrongCellFarDistance;

                bool moved = false;
                bool evaluated = false;
                bool assigned = false;

                // R30: do not rubber-band followers during ordinary running.
                // Move only on the final post-loading pass, and only if the actor
                // is still unloaded, in a different cell and far away, or extremely far.
                const bool allowEmergencyMove = attempt >= 3;
                const bool shouldMove = allowEmergencyMove && (unloaded || far || wrongCellFar);


                if (actor->IsInCombat()) {
                    actor->StopCombat();
                }
                if (auto* process = RE::ProcessLists::GetSingleton()) {
                    process->StopCombatAndAlarmOnActor(actor, false);
                }
                if (actor->IsWeaponDrawn()) {
                    actor->DrawWeaponMagicHands(false);
                }

                if (shouldMove) {
                    actor->MoveTo(player);
                    auto pos = player->GetPosition();
                    const float side = (i % 2 == 0) ? 1.0f : -1.0f;
                    pos.x += side * (kMoveOffsetBase + static_cast<float>(i) * kMoveOffsetStep);
                    pos.y += kMoveBackOffset;
                    actor->SetPosition(pos, true);
                    moved = true;
                }

                TFD::Recruit::CommitOptions options{};
                options.sourceFlow = TFD::Recruit::SourceFlow::Teammate;
                options.reason = reason ? reason : "post_load_humanoid_catchup";
                options.quarantineHostileFactions = true;
                options.clearCombat = true;
                options.evaluatePackage = false;
                options.detailedLog = false;
                options.throttleObserve = true;
                options.ensurePacifyAlliance = true;
                options.applyRuntimeProfile = true;
                TFD::Recruit::CommitRecruit(actor, player, options);

                assigned = QueueHumanoidTeammateAssignEvent(actor, reason ? reason : "post_load_humanoid_catchup");

                const bool shouldEvaluateForPackageRepair = moved || wrongCell || dist > kEvaluateDistance || !actor->Is3DLoaded();
                if (shouldEvaluateForPackageRepair) {
                    actor->EvaluatePackage();
                    evaluated = true;
                }

                spdlog::info(
                    "[TFD][TeammateManager] humanoid post-load catchup actor={:08X} reason={} attempt={} moved={} assigned={} evaluated={} wrongCell={} unloaded={} dist={:.1f}",
                    actor->GetFormID(),
                    reason ? reason : "post_load_humanoid_catchup",
                    attempt,
                    moved ? 1 : 0,
                    assigned ? 1 : 0,
                    evaluated ? 1 : 0,
                    wrongCell ? 1 : 0,
                    unloaded ? 1 : 0,
                    dist);

                ++touched;
            }

            return touched;
        }

        void QueueHumanoidTeammateCatchupAfterLoad(const char* reason)
        {
            const std::string reasonText = reason && reason[0] ? reason : "post_load_humanoid_catchup";

            std::thread([reasonText]() {
                constexpr std::array delays{
                    std::chrono::milliseconds(1500),
                    std::chrono::milliseconds(4500),
                    std::chrono::milliseconds(9000)
                };

                for (std::size_t i = 0; i < delays.size(); ++i) {
                    std::this_thread::sleep_for(delays[i]);

                    auto* task = SKSE::GetTaskInterface();
                    if (!task) {
                        continue;
                    }

                    task->AddTask([reasonText, attempt = i + 1]() {
                        const auto count = CatchupRegisteredHumanoidTeammatesAfterLoad(reasonText.c_str(), attempt);
                        if (count > 0) {
                            spdlog::info(
                                "[TFD][TeammateManager] humanoid post-load catchup pass reason={} attempt={} count={}",
                                reasonText,
                                attempt,
                                count);
                        }
                    });
                }
            }).detach();
        }

        bool RegisterOrRefreshAliasForActor(RE::Actor* actor, const char* reason)
        {
            std::scoped_lock lock(g_syncLock);
            ResolveRegistry();

            if (!g_registry.quest || !actor || actor == Player() || actor->IsDisabled() || actor->IsDead()) {
                spdlog::warn(
                    "[TFD][TeammateManager] register now failed actor={:08X} reason={} detail=invalid_input",
                    actor ? actor->GetFormID() : 0u,
                    reason ? reason : "unknown");
                return false;
            }

            if (!IsValidTeammate(actor)) {
                const bool pendingRecruit = TFD::Recruit::IsRecruitCommitPending(actor);
                spdlog::warn(
                    "[TFD][TeammateManager] register now rejected actor={:08X} reason={} detail={} pendingRecruit={}",
                    actor->GetFormID(),
                    reason ? reason : "unknown",
                    pendingRecruit ? "recruit_commit_pending_no_alias_pre_marker" : "not_valid_teammate",
                    pendingRecruit ? 1 : 0);
                return false;
            }

            if (IsTFDConvertedTeammate(actor)) {
                RememberConvertedTeammate(actor, reason ? reason : "register_now_valid_converted");
                ResetInvalidAliasStrike(actor);
            }

            RE::BGSRefAlias* emptyAlias = nullptr;
            RE::BGSRefAlias* recyclableAlias = nullptr;
            RE::Actor* recyclableActor = nullptr;

            for (auto* alias : g_registry.teammateAliases) {
                if (!alias) {
                    continue;
                }

                auto* current = alias->GetActorReference();
                if (SameActor(current, actor)) {
                    if (IsTFDConvertedTeammate(actor)) {
                        RememberConvertedTeammate(actor, reason ? reason : "register_now_refresh_converted");
                        ResetInvalidAliasStrike(actor);
                    }
                    SyncTeammateFaction(actor, true);
                    QueueHumanoidTeammateAssignEvent(actor, reason ? reason : "register_now_refresh");
                    if (actor->Is3DLoaded()) {
                        actor->EvaluatePackage();
                    }
                    spdlog::info(
                        "[TFD][TeammateManager] register now refresh alias='{}' actor={:08X} reason={}",
                        alias->aliasName.c_str(),
                        actor->GetFormID(),
                        reason ? reason : "unknown");
                    RefreshRecruitCapacityGlobalsUnsafe(reason ? reason : "register_now_refresh");
                    return true;
                }

                if (!current) {
                    if (!emptyAlias) {
                        emptyAlias = alias;
                    }
                    continue;
                }

                if (!recyclableAlias && !IsValidTeammate(current) && !TFD::Recruit::IsRecruitCommitPending(current)) {
                    if (IsKnownConvertedAliasActor(current) && !IsHardInvalidConvertedAliasActor(current)) {
                        continue;
                    }
                    recyclableAlias = alias;
                    recyclableActor = current;
                }
            }

            auto* targetAlias = emptyAlias ? emptyAlias : recyclableAlias;
            if (!targetAlias) {
                spdlog::warn(
                    "[TFD][TeammateManager] register now failed actor={:08X} reason={} detail=no_slot",
                    actor->GetFormID(),
                    reason ? reason : "unknown");
                RefreshRecruitCapacityGlobalsUnsafe(reason ? reason : "register_now_no_slot");
                QueueHumanoidTeammateAssignEvent(actor, reason ? reason : "register_now_no_slot_bridge_only");
                return false;
            }

            if (recyclableActor) {
                spdlog::info(
                    "[TFD][TeammateManager] register now recycle alias='{}' oldActor={:08X} newActor={:08X} reason={}",
                    targetAlias->aliasName.c_str(),
                    recyclableActor->GetFormID(),
                    actor->GetFormID(),
                    reason ? reason : "unknown");
                SyncTeammateFaction(recyclableActor, false);
            }

            WriteAlias(targetAlias, actor);
            if (IsTFDConvertedTeammate(actor)) {
                RememberConvertedTeammate(actor, reason ? reason : "register_now_fill_converted");
                ResetInvalidAliasStrike(actor);
            }
            SyncTeammateFaction(actor, true);
            QueueHumanoidTeammateAssignEvent(actor, reason ? reason : "register_now_assign");
            if (actor->Is3DLoaded()) {
                actor->EvaluatePackage();
            }

            spdlog::info(
                "[TFD][TeammateManager] register now fill alias='{}' actor={:08X} name='{}' reason={}",
                targetAlias->aliasName.c_str(),
                actor->GetFormID(),
                actor->GetName() ? actor->GetName() : "",
                reason ? reason : "unknown");

            RefreshRecruitCapacityGlobalsUnsafe(reason ? reason : "register_now_fill");
            return true;
        }

        void SyncAliasesImpl()
        {
            std::scoped_lock lock(g_syncLock);
            ResolveRegistry();

            auto desired = CollectNearbyPlayerTeammates(8000.0f);

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
                    if (TFD::Recruit::IsRecruitCommitPending(current)) {
                        spdlog::info(
                            "[TFD][TeammateManager] defer clear alias='{}' actor={:08X} reason=recruit_commit_pending",
                            alias->aliasName.c_str(),
                            current->GetFormID());
                        continue;
                    }

                    if (ShouldPreserveInvalidConvertedAlias(alias, current, "sync_alias_invalid")) {
                        continue;
                    }

                    spdlog::info("[TFD][TeammateManager] clear alias='{}' actor={:08X} reason=invalid",
                        alias->aliasName.c_str(), current->GetFormID());
                    SyncTeammateFaction(current, false);
                    WriteAlias(alias, nullptr);
                    continue;
                }

                if (IsTFDConvertedTeammate(current)) {
                    RememberConvertedTeammate(current, "sync_alias_valid_converted");
                    ResetInvalidAliasStrike(current);
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
                if (current && IsKnownConvertedAliasActor(current) && !IsHardInvalidConvertedAliasActor(current)) {
                    continue;
                }
                auto* actor = remaining.front();
                remaining.erase(remaining.begin());
                WriteAlias(alias, actor);
                if (IsTFDConvertedTeammate(actor)) {
                    RememberConvertedTeammate(actor, "sync_fill_converted");
                    ResetInvalidAliasStrike(actor);
                }
                SyncTeammateFaction(actor, true);
                spdlog::info("[TFD][TeammateManager] fill alias='{}' actor={:08X} name='{}'",
                    alias->aliasName.c_str(),
                    actor ? actor->GetFormID() : 0u,
                    actor && actor->GetName() ? actor->GetName() : "");
            }
            RefreshRecruitCapacityGlobalsUnsafe("sync_aliases");

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
                    }
                    else {
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
            if (!actor || actor->IsDead() || actor->IsDisabled()) {
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
        }
        else {
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

    std::size_t GetMaxRecruitSlots()
    {
        AliasInternal::ResolveRegistry();
        return AliasInternal::g_registry.teammateAliases.size();
    }

    std::size_t GetRecruitSlotsFree()
    {
        std::scoped_lock lock(AliasInternal::g_syncLock);
        AliasInternal::ResolveRegistry();
        return static_cast<std::size_t>(AliasInternal::ComputeRecruitSlotsFreeUnsafe());
    }

    void RefreshRecruitCapacityGlobals(const char* reason)
    {
        std::scoped_lock lock(AliasInternal::g_syncLock);
        AliasInternal::RefreshRecruitCapacityGlobalsUnsafe(reason ? reason : "external_refresh");
    }

    bool RegisterOrRefreshTeammateNow(RE::Actor* actor, const char* reason)
    {
        return AliasInternal::RegisterOrRefreshAliasForActor(actor, reason);
    }
    void QueueHumanoidTeammateCatchupAfterLoad(const char* reason)
    {
        AliasInternal::QueueHumanoidTeammateCatchupAfterLoad(reason);
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

        auto snapshot = TFD::Actor::BuildSnapshot(maxRadius, false);
        auto* pCell = player->GetParentCell();
        for (const auto& info : snapshot.actors) {
            auto* actor = info.get();
            if (!actor || actor == player || actor->IsDisabled()) {
                continue;
            }
            if (actor->GetParentCell() != pCell) {
                continue;
            }
            if (info.dist > maxRadius) {
                continue;
            }
            if (!AliasInternal::IsValidTeammate(actor)) {
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
            const bool downed = TFD::Actor::IsDownByHealthThreshold(actor, TFD::Settings::GetAllyDownedThresholdPct());
            if (!downed) {
                if (dist < bestStandingDist) {
                    bestStandingDist = dist;
                    result.standing = actor;
                }
            }
            else if (dist < bestDownedDist) {
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
            if (TFD::Actor::IsDownByHealthThreshold(actor, TFD::Settings::GetAllyDownedThresholdPct())) {
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
            const bool isDown = isLockedAlly || isBleedingOut || TFD::Actor::IsDownByHealthThreshold(actor, TFD::Settings::GetAllyDownedThresholdPct());
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

    bool ReviveDownedAlly(RE::Actor* actor, float targetHealthPct)
    {
        return BridgeInternal::g_runtimeProviders.reviveDownedAlly ? BridgeInternal::g_runtimeProviders.reviveDownedAlly(actor, targetHealthPct) : false;
    }

    void SetPendingDefeatedDialogueTarget(RE::Actor* actor)
    {
        if (BridgeInternal::g_runtimeProviders.setPendingDefeatedDialogueTarget) {
            BridgeInternal::g_runtimeProviders.setPendingDefeatedDialogueTarget(actor);
        }
    }

    bool RecruitDefeatedHumanoidAsTeammate(RE::Actor* actor)
    {
        if (!actor) {
            return false;
        }
        if (actor->IsDisabled() || actor->IsDead()) {
            spdlog::warn(
                "[TFD][TeammateManager] defeated humanoid recruit failed actor={:08X} reason=invalid_actor",
                actor ? actor->GetFormID() : 0u);
            return false;
        }
        if (BridgeInternal::g_runtimeProviders.isDialogueCapableDefeatedEnemy && !BridgeInternal::g_runtimeProviders.isDialogueCapableDefeatedEnemy(actor)) {
            spdlog::warn(
                "[TFD][TeammateManager] defeated humanoid recruit failed actor={:08X} reason=not_dialogue_capable_defeated",
                actor->GetFormID());
            return false;
        }
        if (BridgeInternal::g_runtimeProviders.getDefeatedEnemyRemainingSeconds && BridgeInternal::g_runtimeProviders.getDefeatedEnemyRemainingSeconds(actor) <= 0.0) {
            spdlog::warn(
                "[TFD][TeammateManager] defeated humanoid recruit failed actor={:08X} reason=defeated_window_expired",
                actor->GetFormID());
            return false;
        }

        AliasInternal::ResolveRegistry();
        AliasInternal::RefreshRecruitCapacityGlobalsUnsafe("defeated_humanoid_recruit_preflight");
        if (!AliasInternal::IsValidTeammate(actor) && AliasInternal::ComputeRecruitSlotsFreeUnsafe() <= 0) {
            spdlog::warn(
                "[TFD][TeammateManager] defeated humanoid recruit failed actor={:08X} reason=no_teammate_slot",
                actor->GetFormID());
            return false;
        }

        // Victory recruit starts from a defeated hostile actor, so it is not yet
        // recruit-like when the player chooses the topic. Mark it pending before
        // commit; otherwise CommitRecruit and the Papyrus teammate registry see it
        // as a normal hostile and alias assignment is rejected.
        TFD::Recruit::MarkRecruitCommitPending(
            actor,
            8.0,
            TFD::Recruit::SourceFlow::Victory,
            "defeated_humanoid_recruit");

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

        TFD::Recruit::CommitOptions recruitOptions{};
        recruitOptions.sourceFlow = TFD::Recruit::SourceFlow::Victory;
        recruitOptions.reason = "defeated_humanoid_recruit";
        recruitOptions.quarantineHostileFactions = true;
        recruitOptions.clearCombat = true;
        recruitOptions.evaluatePackage = true;
        recruitOptions.detailedLog = true;
        recruitOptions.throttleObserve = false;
        recruitOptions.ensurePacifyAlliance = true;
        recruitOptions.applyRuntimeProfile = true;

        const auto commit = TFD::Recruit::CommitRecruit(actor, recruitOptions);
        if (!commit.attempted || commit.skipped || commit.hostileFactionMatchesAfter > 0) {
            spdlog::warn(
                "[TFD][TeammateManager] defeated humanoid recruit failed actor={:08X} reason=commit_not_clean attempted={} skipped={} rawAfter={} hostileAfter={}",
                actor->GetFormID(),
                commit.attempted ? 1 : 0,
                commit.skipped ? 1 : 0,
                commit.rawHostileAfter ? 1 : 0,
                commit.hostileFactionMatchesAfter);
            return false;
        }

        const bool aliasOk = TFD::TeammateManager::RegisterOrRefreshTeammateNow(actor, "defeated_humanoid_recruit");
        if (!aliasOk) {
            spdlog::warn(
                "[TFD][TeammateManager] defeated humanoid recruit failed actor={:08X} reason=alias_assign_failed",
                actor->GetFormID());
            return false;
        }

        if (!TFD::FlowController::QueueBridgeModEvent(BridgeInternal::kHumanoidTeammateAssignEvent, actor)) {
            spdlog::warn("[TFD][TeammateManager] defeated humanoid recruit bridge dispatch failed actor={:08X}", actor->GetFormID());
        }
        if (actor->IsInCombat()) {
            actor->StopCombat();
        }
        actor->DrawWeaponMagicHands(false);
        if (actor->Is3DLoaded()) {
            actor->EvaluatePackage();
        }
        if (BridgeInternal::g_runtimeProviders.clearPendingDefeatedDialogueTarget) {
            BridgeInternal::g_runtimeProviders.clearPendingDefeatedDialogueTarget();
        }
        AliasInternal::RefreshRecruitCapacityGlobalsUnsafe("defeated_humanoid_recruit_done");
        spdlog::info("[TFD][TeammateManager] defeated humanoid recruit actor={:08X} aliasOk=1", actor->GetFormID());
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
