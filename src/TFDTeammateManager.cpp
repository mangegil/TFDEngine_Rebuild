#include "TFDTeammateManager.h"
#include "TFDActor.h"
#include "TFDCombatBehavior.h"
#include "TFDPleasureRuntime.h"

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
#include <utility>

#include "TFDFlowController.h"
#include "TFDSettings.h"
#include "TFDHostilityController.h"
#include "TFDTame.h"
#include "TFDRecruit.h"
#include "TFDPayModel.h"
#include "EditorIdCache.h"
#include <cctype>

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
            RE::TESQuest* expiredQuest{ nullptr };
            std::array<RE::BGSRefAlias*, 10> expiredAliases{};
            RE::TESFaction* expiredTeammateFaction{ nullptr };
            RE::TESFaction* teammateFaction{ nullptr };
            RE::TESFaction* currentFollowerFaction{ nullptr };
            RE::TESFaction* playerFollowerFaction{ nullptr };
            RE::TESGlobal* teammateStateGlobal{ nullptr };
            RE::TESGlobal* recruitSlotsFreeGlobal{ nullptr };
            RE::TESGlobal* hasPotionsGlobal{ nullptr };
        };

        inline RegistryCache g_registry{};
        inline std::atomic_bool g_installed{ false };
        inline std::atomic_bool g_running{ false };
        inline std::atomic_bool g_tickPending{ false };
        inline std::thread g_worker{};
        inline std::mutex g_syncLock{};

        inline std::unordered_set<RE::FormID> g_knownConvertedTeammates{};
        inline std::unordered_map<RE::FormID, std::uint32_t> g_invalidAliasStrikes{};
        inline std::unordered_map<RE::FormID, double> g_contractEndDays{};
        inline std::unordered_set<RE::FormID> g_expiredContractActors{};
        inline std::unordered_map<RE::FormID, double> g_contractExpiryNextAttemptSec{};
        inline std::unordered_map<RE::FormID, std::uint32_t> g_contractExpiryOpenAttempts{};
        inline RE::FormID g_contractExpiryActiveActor{ 0 };
        inline int g_contractExpiryActiveAliasIndex{ -1 };
        inline RE::FormID g_contractExpiryPleasureHoldActor{ 0 };
        inline double g_contractExpiryPleasureHoldStartedSec{ 0.0 };
        inline double g_contractExpiryPleasureHoldLastLogSec{ 0.0 };
        inline RE::FormID g_downedRecoveryDialogueHoldActor{ 0 };
        inline double g_downedRecoveryDialogueHoldUntilSec{ 0.0 };
        inline double g_downedRecoveryDialogueHoldLastLogSec{ 0.0 };
        inline int g_lastRecruitSlotsFreeWritten{ -999 };
        inline int g_lastTeammateStateWritten{ -999 };
        inline int g_lastHasPotionsWritten{ -999 };

        struct DeferredHumanoidAssignEntry
        {
            std::string reason{};
            double dueSec{ 0.0 };
            std::uint32_t attempts{ 0 };
        };

        inline std::mutex g_deferredHumanoidAssignLock{};
        inline std::unordered_map<RE::FormID, DeferredHumanoidAssignEntry> g_deferredHumanoidAssigns{};

        constexpr std::uint32_t kConvertedAliasInvalidGraceTicks = 12;
        constexpr double kHumanoidContractDays = 1.0;
        constexpr double kDeferredHumanoidAssignDelaySeconds = 0.35;
        constexpr double kDeferredHumanoidAssignRetrySeconds = 0.60;
        constexpr std::uint32_t kDeferredHumanoidAssignMaxAttempts = 30;
        constexpr double kContractExpiryRetryBlockedSeconds = 8.0;
        constexpr double kContractExpiryRetryFailedSeconds = 12.0;
        constexpr double kContractExpiryRetryAfterOpenSeconds = 45.0;
        constexpr double kContractExpiryPackageGraceSeconds = 0.0; // R66: native opens expired teammate dialogue immediately; CK package must not win first frame
        constexpr double kContractExpiryPleasureHoldMaxSeconds = 300.0;
        constexpr double kContractExpiryPleasureHoldLogIntervalSeconds = 5.0;
        constexpr double kDownedRecoveryDialogueHoldLogIntervalSeconds = 2.0;
        constexpr float kContractExpiryForcegreetRadius = 4096.0f;
        constexpr float kTeammateHealTargetPct = 0.85f;
        constexpr float kTeammateHealMinAbsHp = 45.0f;

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

            auto collectTeammateAliases = [](RE::TESQuest* quest, std::array<RE::BGSRefAlias*, 10>& outAliases) {
                if (!quest) {
                    return;
                }
                for (auto* baseAlias : quest->aliases) {
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
                        if (slot >= 1 && slot <= static_cast<int>(outAliases.size())) {
                            const auto index = static_cast<std::size_t>(slot - 1);
                            outAliases[index] = refAlias;
                        }
                    }
                    catch (...) {
                    }
                }
                };

            collectTeammateAliases(g_registry.quest, g_registry.teammateAliases);

            g_registry.expiredQuest = RE::TESForm::LookupByEditorID<RE::TESQuest>("TFDExpiredTeammateQuest");
            if (!g_registry.expiredQuest) {
                spdlog::warn("[TFD][TeammateManager] quest TFDExpiredTeammateQuest not found; contract expiry will use native fallback only");
            }
            collectTeammateAliases(g_registry.expiredQuest, g_registry.expiredAliases);

            g_registry.expiredTeammateFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDExpiredTeammate");
            if (!g_registry.expiredTeammateFaction) {
                spdlog::warn("[TFD][TeammateManager] faction TFDExpiredTeammate not found; expired contract package status will be skipped");
            }

            g_registry.teammateFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDTeammateFaction");
            g_registry.currentFollowerFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>("CurrentFollowerFaction");
            g_registry.playerFollowerFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>("PlayerFollowerFaction");
            g_registry.teammateStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDTeammateState");
            g_registry.recruitSlotsFreeGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDRecruitSlotsFree");
            g_registry.hasPotionsGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDHasPotions");
            if (!g_registry.teammateFaction) {
                spdlog::warn("[TFD][TeammateManager] faction TFDTeammateFaction not found");
            }
            std::size_t found = 0;
            for (auto* alias : g_registry.teammateAliases) {
                if (alias) {
                    ++found;
                }
            }
            spdlog::info("[TFD][TeammateManager] alias registry resolved quest={:08X} aliases={} tfdTeammateFaction={:08X} expiredTeammateFaction={:08X} currentFollowerFaction={:08X} playerFollowerFaction={:08X} teammateState={:08X} recruitSlotsFree={:08X} hasPotions={:08X}",
                g_registry.quest ? g_registry.quest->GetFormID() : 0u,
                found,
                g_registry.teammateFaction ? g_registry.teammateFaction->GetFormID() : 0u,
                g_registry.expiredTeammateFaction ? g_registry.expiredTeammateFaction->GetFormID() : 0u,
                g_registry.currentFollowerFaction ? g_registry.currentFollowerFaction->GetFormID() : 0u,
                g_registry.playerFollowerFaction ? g_registry.playerFollowerFaction->GetFormID() : 0u,
                g_registry.teammateStateGlobal ? g_registry.teammateStateGlobal->GetFormID() : 0u,
                g_registry.recruitSlotsFreeGlobal ? g_registry.recruitSlotsFreeGlobal->GetFormID() : 0u,
                g_registry.hasPotionsGlobal ? g_registry.hasPotionsGlobal->GetFormID() : 0u);
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

        bool IsActorInExpiredAliasUnsafe(RE::Actor* actor)
        {
            if (!actor) {
                return false;
            }
            const auto actorId = actor->GetFormID();
            for (auto* alias : g_registry.expiredAliases) {
                if (!alias) {
                    continue;
                }
                auto* current = alias->GetActorReference();
                if (current && current->GetFormID() == actorId) {
                    return true;
                }
            }
            return false;
        }

        RE::TESFaction* ResolvePacifyFactionUnsafe()
        {
            static RE::TESFaction* pacifyFaction = nullptr;
            static bool tried = false;
            if (!tried) {
                tried = true;
                pacifyFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDPacifyFaction");
                if (!pacifyFaction) {
                    spdlog::warn("[TFD][TeammateManager][R93V] faction TFDPacifyFaction not found for stale grace cleanup");
                }
            }
            return pacifyFaction;
        }

        void ClearStrayExpiredHelperFactionUnsafe(RE::Actor* actor, const char* reason)
        {
            if (!actor || actor == Player()) {
                return;
            }
            bool changed = false;
            if (g_registry.expiredTeammateFaction && actor->IsInFaction(g_registry.expiredTeammateFaction)) {
                actor->RemoveFromFaction(g_registry.expiredTeammateFaction);
                changed = true;
            }
            if (auto* pacifyFaction = ResolvePacifyFactionUnsafe()) {
                if (actor->IsInFaction(pacifyFaction)) {
                    actor->RemoveFromFaction(pacifyFaction);
                    changed = true;
                }
            }
            if (changed) {
                spdlog::info(
                    "[TFD][TeammateManager][R93V] cleared stray release helper factions actor={:08X} reason={}",
                    actor->GetFormID(),
                    reason ? reason : "unknown");
            }
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

            if (g_registry.expiredTeammateFaction && actor->IsInFaction(g_registry.expiredTeammateFaction)) {
                // R93V: TFDExpiredTeammate is a valid converted marker only for the
                // actual expired-contract flow. It must not make random release-grace
                // actors enter TFD teammate aliases. Release/follow grace can use this
                // faction in older builds, so reject it while runtime grace is active
                // and clean up stray non-expired actors when no expired alias/known
                // conversion backs the marker.
                if (TFD::Actor::Ops::HasReleaseFollowGrace(actor)) {
                    return false;
                }

                const bool knownConverted = g_knownConvertedTeammates.find(actor->GetFormID()) != g_knownConvertedTeammates.end();
                const bool expiredAliasActor = IsActorInExpiredAliasUnsafe(actor);
                if (knownConverted || expiredAliasActor) {
                    return true;
                }

                ClearStrayExpiredHelperFactionUnsafe(actor, "stray_expired_marker_not_converted");
                return false;
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

        bool IsAliasManagedTeammate(RE::Actor* actor)
        {
            // R67: the TFD teammate alias/package quest owns only TFD-converted humanoids.
            // Vanilla/framework followers are still recognized as player-side actors by
            // IsValidTeammate()/IsPlayerSideTeammateAnchor(), but they must not be placed
            // into TFDPlayerTeammateQuest aliases because that applies TFDTeammatePackage
            // and can override their native follower framework combat AI.
            return IsTFDConvertedTeammate(actor);
        }

        bool IsPlayerSideTeammateAnchor(RE::Actor* actor)
        {
            ResolveRegistry();
            if (!actor || actor == Player() || actor->IsDisabled() || actor->IsDead()) {
                return false;
            }

            if (IsTFDConvertedTeammate(actor)) {
                return true;
            }
            if (actor->IsPlayerTeammate()) {
                return true;
            }
            if (HasFollowerAnchorFaction(actor)) {
                return true;
            }

            return false;
        }

        bool IsVanillaAssistIsolationCombatPressure(RE::Actor* actor, RE::PlayerCharacter* player, RE::Actor* combatTarget)
        {
            if (!actor || !player || actor == player) {
                return false;
            }
            if (!IsTFDConvertedTeammate(actor)) {
                return false;
            }
            if (actor->IsDead() || actor->IsDisabled()) {
                return false;
            }

            auto* playerTarget = ResolveCombatTarget(player);
            if (player->IsInCombat() || playerTarget) {
                return true;
            }
            if (actor->IsInCombat() || combatTarget) {
                return true;
            }
            if (player->IsWeaponDrawn() && actor->IsWeaponDrawn()) {
                return true;
            }

            return false;
        }

        bool IsCombatCapableTeammate(RE::Actor* actor)
        {
            if (!IsValidTeammate(actor)) {
                return false;
            }

            if (TFD::PleasureRuntime::IsInCombatPleasureChainActive()) {
                spdlog::info(
                    "[TFD][TeammateManager][R94G] combat capable suppressed during incombat pleasure chain actor={:08X}",
                    actor ? actor->GetFormID() : 0u);
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

            // R94I: TFDTeammateState is a roster / dialogue-condition global,
            // not an active-combat-capability flag.
            //
            // During InCombat pleasure chaining R94G intentionally suppresses
            // converted teammates from combat/assist until the chain is stable.
            // Counting only IsCombatCapableTeammate() here made the roster look
            // empty even when aliases were already filled, producing
            // teammateState=0 with registered=2. Keep combat suppression local to
            // combat behavior and report the actual registered teammate roster.
            const int registeredCount = static_cast<int>(teammates.size());
            if (registeredCount > 1) {
                return 2;
            }

            return registeredCount == 1 ? 1 : 0;
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

        void WriteQuestAlias(RE::TESQuest* quest, RE::BGSRefAlias* alias, RE::Actor* actor);
        void WriteAlias(RE::BGSRefAlias* alias, RE::Actor* actor);
        void RefreshRecruitCapacityGlobalsUnsafe(const char* reason);
        void ClearExpiredTeammateStatusForActorIdUnsafe(RE::FormID actorId, const char* reason);

        double CurrentGameDays()
        {
            auto* calendar = RE::Calendar::GetSingleton();
            if (!calendar) {
                return 0.0;
            }

            double days = static_cast<double>(calendar->rawDaysPassed);
            if (calendar->gameDaysPassed) {
                const double globalDays = static_cast<double>(calendar->gameDaysPassed->value);
                if (globalDays > 0.0) {
                    days = globalDays;
                }
            }

            // Some runtime paths expose only the whole day counter here while
            // GameHour carries the fractional part. Contract expiry must be a
            // true 24-hour timer, not "next day boundary".
            if (calendar->gameHour) {
                const double hourRaw = static_cast<double>(calendar->gameHour->value);
                double hour = std::fmod(hourRaw, 24.0);
                if (hour < 0.0) {
                    hour += 24.0;
                }
                const double hourFrac = hour / 24.0;
                const double nearestWholeDay = std::round(days);
                if (std::abs(days - nearestWholeDay) <= 0.0001) {
                    days = std::floor(days) + hourFrac;
                }
            }

            return days;
        }

        double NowRealSeconds()
        {
            using Clock = std::chrono::steady_clock;
            static const auto start = Clock::now();
            return std::chrono::duration<double>(Clock::now() - start).count();
        }

        void ClearDownedRecoveryDialogueHoldUnsafe(RE::FormID actorId, const char* reason)
        {
            if (g_downedRecoveryDialogueHoldActor == 0) {
                return;
            }
            if (actorId != 0 && actorId != g_downedRecoveryDialogueHoldActor) {
                return;
            }

            spdlog::info("[TFD][TeammateManager] downed recovery dialogue hold cleared actor={:08X} reason={}",
                g_downedRecoveryDialogueHoldActor,
                reason ? reason : "unknown");

            g_downedRecoveryDialogueHoldActor = 0;
            g_downedRecoveryDialogueHoldUntilSec = 0.0;
            g_downedRecoveryDialogueHoldLastLogSec = 0.0;
        }

        void ClearDownedRecoveryDialogueHoldUnsafe(RE::Actor* actor, const char* reason)
        {
            ClearDownedRecoveryDialogueHoldUnsafe(actor ? actor->GetFormID() : 0, reason);
        }

        void ArmDownedRecoveryDialogueHoldUnsafe(RE::Actor* actor, double seconds, const char* reason)
        {
            if (!actor || actor->IsDead() || actor->IsDisabled()) {
                return;
            }

            const double nowReal = NowRealSeconds();
            const double safeSeconds = std::clamp(seconds, 1.0, 30.0);
            g_downedRecoveryDialogueHoldActor = actor->GetFormID();
            g_downedRecoveryDialogueHoldUntilSec = nowReal + safeSeconds;
            g_downedRecoveryDialogueHoldLastLogSec = 0.0;

            spdlog::info("[TFD][TeammateManager] downed recovery dialogue hold armed actor={:08X} seconds={:.1f} reason={}",
                g_downedRecoveryDialogueHoldActor,
                safeSeconds,
                reason ? reason : "unknown");
        }

        bool IsDownedRecoveryDialogueHoldActorUnsafe(RE::Actor* actor)
        {
            if (!actor || g_downedRecoveryDialogueHoldActor == 0) {
                return false;
            }
            if (actor->GetFormID() != g_downedRecoveryDialogueHoldActor) {
                return false;
            }

            const double nowReal = NowRealSeconds();
            if (g_downedRecoveryDialogueHoldUntilSec > 0.0 && nowReal > g_downedRecoveryDialogueHoldUntilSec) {
                ClearDownedRecoveryDialogueHoldUnsafe(g_downedRecoveryDialogueHoldActor, "hold_timeout");
                return false;
            }

            if (g_downedRecoveryDialogueHoldLastLogSec <= 0.0 || nowReal - g_downedRecoveryDialogueHoldLastLogSec >= kDownedRecoveryDialogueHoldLogIntervalSeconds) {
                g_downedRecoveryDialogueHoldLastLogSec = nowReal;
                spdlog::info("[TFD][TeammateManager] downed recovery dialogue hold active actor={:08X} remaining={:.1f}s",
                    g_downedRecoveryDialogueHoldActor,
                    std::max(0.0, g_downedRecoveryDialogueHoldUntilSec - nowReal));
            }

            return true;
        }

        bool IsDownedRecoveryDialogueHoldActiveUnsafe()
        {
            if (g_downedRecoveryDialogueHoldActor == 0) {
                return false;
            }

            const double nowReal = NowRealSeconds();
            if (g_downedRecoveryDialogueHoldUntilSec > 0.0 && nowReal > g_downedRecoveryDialogueHoldUntilSec) {
                ClearDownedRecoveryDialogueHoldUnsafe(g_downedRecoveryDialogueHoldActor, "hold_timeout");
                return false;
            }
            return true;
        }

        bool IsReasonExtendContractPleasure(const char* reason)
        {
            return reason && std::string_view(reason) == "extend_contract_pleasure";
        }

        RE::FormID ParseFirstFormIDToken(std::string_view text)
        {
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
                text.remove_prefix(1);
            }
            if (text.empty()) {
                return 0;
            }

            unsigned base = 10;
            if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
                base = 16;
                text.remove_prefix(2);
            }

            std::uint64_t value = 0;
            bool any = false;
            for (char ch : text) {
                int digit = -1;
                if (base == 10) {
                    if (ch >= '0' && ch <= '9') {
                        digit = ch - '0';
                    }
                }
                else {
                    if (ch >= '0' && ch <= '9') {
                        digit = ch - '0';
                    }
                    else if (ch >= 'a' && ch <= 'f') {
                        digit = 10 + (ch - 'a');
                    }
                    else if (ch >= 'A' && ch <= 'F') {
                        digit = 10 + (ch - 'A');
                    }
                }

                if (digit < 0 || static_cast<unsigned>(digit) >= base) {
                    break;
                }

                any = true;
                value = (value * base) + static_cast<std::uint64_t>(digit);
                if (value > static_cast<std::uint64_t>(std::numeric_limits<RE::FormID>::max())) {
                    return 0;
                }
            }

            return any ? static_cast<RE::FormID>(value) : 0;
        }

        void BeginContractExpiryPleasureHoldUnsafe(RE::FormID actorId, const char* reason)
        {
            if (actorId == 0) {
                return;
            }

            const double nowReal = NowRealSeconds();
            g_contractExpiryPleasureHoldActor = actorId;
            g_contractExpiryPleasureHoldStartedSec = nowReal;
            g_contractExpiryPleasureHoldLastLogSec = 0.0;

            spdlog::info("[TFD][TeammateManager] contract expiry queue hold begin actor={:08X} reason={} policy=wait_afterpleasure_terminal",
                actorId,
                reason ? reason : "unknown");
        }

        void EndContractExpiryPleasureHoldUnsafe(RE::FormID actorId, const char* reason)
        {
            if (g_contractExpiryPleasureHoldActor == 0) {
                return;
            }
            if (actorId != 0 && actorId != g_contractExpiryPleasureHoldActor) {
                return;
            }

            spdlog::info("[TFD][TeammateManager] contract expiry queue hold end actor={:08X} reason={}",
                g_contractExpiryPleasureHoldActor,
                reason ? reason : "unknown");

            g_contractExpiryPleasureHoldActor = 0;
            g_contractExpiryPleasureHoldStartedSec = 0.0;
            g_contractExpiryPleasureHoldLastLogSec = 0.0;
        }

        bool IsContractExpiryPleasureHoldActiveUnsafe(double nowReal)
        {
            if (g_contractExpiryPleasureHoldActor == 0) {
                return false;
            }

            const double elapsed = std::max(0.0, nowReal - g_contractExpiryPleasureHoldStartedSec);
            if (elapsed > kContractExpiryPleasureHoldMaxSeconds) {
                spdlog::warn("[TFD][TeammateManager] contract expiry queue hold expired actor={:08X} elapsed={:.1f}s max={:.1f}s action=release_hold",
                    g_contractExpiryPleasureHoldActor,
                    elapsed,
                    kContractExpiryPleasureHoldMaxSeconds);
                EndContractExpiryPleasureHoldUnsafe(g_contractExpiryPleasureHoldActor, "pleasure_hold_timeout");
                return false;
            }

            if (g_contractExpiryPleasureHoldLastLogSec <= 0.0 || nowReal - g_contractExpiryPleasureHoldLastLogSec >= kContractExpiryPleasureHoldLogIntervalSeconds) {
                g_contractExpiryPleasureHoldLastLogSec = nowReal;
                spdlog::info("[TFD][TeammateManager] contract expiry queue held actor={:08X} elapsed={:.1f}s reason=extend_pleasure_afterpleasure_pending",
                    g_contractExpiryPleasureHoldActor,
                    elapsed);
            }

            return true;
        }

        void NoteAfterPleasureTerminalForContractExpiryUnsafe(RE::FormID actorId, const char* eventName)
        {
            if (actorId == 0) {
                return;
            }
            EndContractExpiryPleasureHoldUnsafe(actorId, eventName ? eventName : "after_pleasure_terminal");
        }

        RE::TESTopicInfo* ResolveTeammateGreetTopicInfo()
        {
            static RE::TESTopicInfo* info = nullptr;
            static bool attempted = false;

            if (!attempted) {
                attempted = true;
                constexpr RE::FormID kTeammateGreetInfoLocalFormID = 0x00195939;
                constexpr std::string_view kPluginName{ "TFDEngine.esp" };

                if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
                    info = dataHandler->LookupForm<RE::TESTopicInfo>(kTeammateGreetInfoLocalFormID, kPluginName);
                }

                if (info) {
                    spdlog::info("[TFD][TeammateManager] TFDDialogueTeammateGreet INFO resolved {:08X} local={:06X}",
                        info->GetFormID(),
                        kTeammateGreetInfoLocalFormID);
                }
                else {
                    spdlog::warn("[TFD][TeammateManager] TFDDialogueTeammateGreet INFO {:06X} not found in {}; contract expiry forcegreet will fall back to default topic selection",
                        kTeammateGreetInfoLocalFormID,
                        kPluginName);
                }
            }

            return info;
        }

        bool IsBlockingMenuOpen()
        {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) {
                return false;
            }

            return ui->IsMenuOpen(RE::MainMenu::MENU_NAME) ||
                ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) ||
                ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME) ||
                ui->IsMenuOpen(RE::Console::MENU_NAME) ||
                ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME) ||
                ui->IsMenuOpen(RE::JournalMenu::MENU_NAME) ||
                ui->IsMenuOpen(RE::LockpickingMenu::MENU_NAME);
        }

        std::string ToLowerAscii(std::string_view in)
        {
            std::string out;
            out.reserve(in.size());
            for (char c : in) {
                out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            return out;
        }

        bool ContainsNoCase(std::string_view haystack, std::string_view needle)
        {
            if (haystack.empty() || needle.empty()) {
                return false;
            }
            const auto h = ToLowerAscii(haystack);
            const auto n = ToLowerAscii(needle);
            return h.find(n) != std::string::npos;
        }

        bool IsManualTeammateDialogueRefreshReason(std::string_view reason)
        {
            return reason == "teammate_activate_dialogue" ||
                reason == "teammate_crosshair_activate_dialogue" ||
                reason == "teammate_downed_activate_dialogue" ||
                reason == "teammate_downed_fallback_activate_dialogue" ||
                reason == "teammate_defeated_redirect_dialogue";
        }

        bool IsHealthPotionCandidate(RE::AlchemyItem* potion)
        {
            if (!potion || potion->IsPoison() || potion->IsFood()) {
                return false;
            }

            // Do not require AlchemyItem::IsMedicine() here. On SE 1.5.97/CommonLibSSE-NG
            // ordinary restore-health ALCH records such as RestoreHealth01 can fail that
            // runtime classification even though they are valid inventory healing potions.
            const auto editorId = TFD::Util::GetEditorId(potion);
            const char* displayName = potion->GetName();
            const std::string_view nameView = displayName ? std::string_view(displayName) : std::string_view{};

            if (ContainsNoCase(editorId, "restorehealth") ||
                ContainsNoCase(editorId, "restore_health") ||
                ContainsNoCase(editorId, "healing") ||
                ContainsNoCase(nameView, "healing") ||
                ContainsNoCase(nameView, "restore health") ||
                ContainsNoCase(nameView, "health potion")) {
                return true;
            }

            return false;
        }

        RE::AlchemyItem* ResolveHealthPotionCandidateUnsafe()
        {
            auto* player = Player();
            if (!player) {
                return nullptr;
            }

            RE::AlchemyItem* bestPotion = nullptr;
            const auto inv = player->GetInventory([](RE::TESBoundObject& obj) {
                if (!obj.Is(RE::FormType::AlchemyItem)) {
                    return false;
                }
                auto* potion = obj.As<RE::AlchemyItem>();
                return IsHealthPotionCandidate(potion);
                }, true);

            for (const auto& [item, invData] : inv) {
                const auto& [count, entry] = invData;
                (void)entry;
                if (count <= 0) {
                    continue;
                }
                auto* potion = item ? item->As<RE::AlchemyItem>() : nullptr;
                if (!IsHealthPotionCandidate(potion)) {
                    continue;
                }
                if (!bestPotion || potion->GetFormID() < bestPotion->GetFormID()) {
                    bestPotion = potion;
                }
            }

            return bestPotion;
        }

        bool HasHealthPotionAvailableUnsafe()
        {
            return ResolveHealthPotionCandidateUnsafe() != nullptr;
        }

        void WriteHasPotions(int value)
        {
            ResolveRegistry();
            if (!g_registry.hasPotionsGlobal) {
                return;
            }
            g_registry.hasPotionsGlobal->value = static_cast<float>(std::clamp(value, 0, 1));
        }

        void RefreshPotionGlobalUnsafe(const char* reason)
        {
            const int hasPotions = HasHealthPotionAvailableUnsafe() ? 1 : 0;
            WriteHasPotions(hasPotions);
            if (hasPotions != g_lastHasPotionsWritten) {
                g_lastHasPotionsWritten = hasPotions;
                spdlog::info("[TFD][TeammateManager] potion global refresh TFDHasPotions={} reason={}",
                    hasPotions,
                    reason ? reason : "unknown");
            }
        }

        bool ConsumeHealthPotionFromPlayerUnsafe(const char* reason)
        {
            ResolveRegistry();
            auto* player = Player();
            auto* potion = ResolveHealthPotionCandidateUnsafe();
            if (!player || !potion) {
                RefreshPotionGlobalUnsafe(reason ? reason : "consume_health_potion_failed");
                spdlog::warn("[TFD][TeammateManager] consume health potion failed reason={} hasPlayer={} hasPotion={}",
                    reason ? reason : "unknown",
                    player ? 1 : 0,
                    potion ? 1 : 0);
                return false;
            }

            player->RemoveItem(potion, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            spdlog::info("[TFD][TeammateManager] consume health potion item={:08X} reason={}",
                potion->GetFormID(),
                reason ? reason : "unknown");
            RefreshPotionGlobalUnsafe(reason ? reason : "consume_health_potion");
            return true;
        }

        bool RestoreActorHealthDirect(RE::Actor* actor, float targetPct, float minAbsHp, const char* reason)
        {
            if (!actor || actor->IsDead() || actor->IsDisabled()) {
                return false;
            }

            const float maxHp = std::max(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
            const float targetHp = std::clamp(std::max(minAbsHp, maxHp * targetPct), 1.0f, maxHp);
            const float hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
            if (hpNow < targetHp) {
                actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, targetHp - hpNow);
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
            if (actor->Is3DLoaded()) {
                actor->EvaluatePackage();
            }

            spdlog::info("[TFD][TeammateManager] restore actor health actor={:08X} hpNow={:.1f} target={:.1f} max={:.1f} reason={}",
                actor->GetFormID(),
                hpNow,
                targetHp,
                maxHp,
                reason ? reason : "unknown");
            return true;
        }

        bool RestoreTeammateHealthUnsafe(RE::Actor* actor, const char* reason)
        {
            if (!actor || !IsValidTeammate(actor)) {
                spdlog::warn("[TFD][TeammateManager] restore teammate health rejected actor={:08X} reason={} detail=not_valid_teammate",
                    actor ? actor->GetFormID() : 0u,
                    reason ? reason : "unknown");
                return false;
            }
            const bool ok = RestoreActorHealthDirect(actor, kTeammateHealTargetPct, kTeammateHealMinAbsHp, reason ? reason : "teammate_restore_health");
            if (ok) {
                ClearDownedRecoveryDialogueHoldUnsafe(actor, reason ? reason : "teammate_restore_health");
            }
            return ok;
        }

        void ClearContractExpiryRuntimeForActorIdUnsafe(RE::FormID actorId, const char* reason)
        {
            if (actorId == 0) {
                return;
            }

            ClearExpiredTeammateStatusForActorIdUnsafe(actorId, reason ? reason : "contract_runtime_clear");

            g_expiredContractActors.erase(actorId);
            g_contractExpiryNextAttemptSec.erase(actorId);
            g_contractExpiryOpenAttempts.erase(actorId);

            if (g_contractExpiryActiveActor == actorId) {
                g_contractExpiryActiveActor = 0;
                g_contractExpiryActiveAliasIndex = -1;
                spdlog::info("[TFD][TeammateManager] contract expiry active actor cleared actor={:08X} reason={}",
                    actorId,
                    reason ? reason : "unknown");
            }
        }

        void ClearContractExpiryRuntimeForActorUnsafe(RE::Actor* actor, const char* reason)
        {
            if (!actor) {
                return;
            }

            ClearContractExpiryRuntimeForActorIdUnsafe(actor->GetFormID(), reason);
        }

        void EnsureContractForActorUnsafe(RE::Actor* actor, const char* reason)
        {
            if (!actor || actor == Player() || actor->IsDead() || actor->IsDisabled()) {
                return;
            }
            if (!IsTFDConvertedTeammate(actor)) {
                return;
            }

            const auto actorId = actor->GetFormID();
            if (actorId == 0) {
                return;
            }

            const double nowDays = CurrentGameDays();
            auto it = g_contractEndDays.find(actorId);
            if (it != g_contractEndDays.end()) {
                if (it->second > nowDays) {
                    return;
                }

                const bool firstExpiredMark = g_expiredContractActors.insert(actorId).second;
                if (firstExpiredMark) {
                    spdlog::info("[TFD][TeammateManager] contract expired actor={:08X} endDay={:.4f} nowDay={:.4f} reason={} action=await_forcegreet",
                        actorId,
                        it->second,
                        nowDays,
                        reason ? reason : "unknown");
                }
                return;
            }

            const double endDays = nowDays + kHumanoidContractDays;
            g_contractEndDays[actorId] = endDays;
            ClearContractExpiryRuntimeForActorIdUnsafe(actorId, reason ? reason : "contract_set");
            spdlog::info("[TFD][TeammateManager] contract set actor={:08X} days=1.00 endDay={:.4f} reason={}",
                actorId,
                endDays,
                reason ? reason : "unknown");
        }

        bool ExtendContractForActorUnsafe(RE::Actor* actor, const char* reason)
        {
            if (!actor || !IsValidTeammate(actor)) {
                spdlog::warn("[TFD][TeammateManager] contract extend rejected actor={:08X} reason={} detail=not_valid_teammate",
                    actor ? actor->GetFormID() : 0u,
                    reason ? reason : "unknown");
                return false;
            }

            const double nowDays = CurrentGameDays();
            const auto actorId = actor->GetFormID();
            const auto found = g_contractEndDays.find(actorId);
            const double oldEnd = found != g_contractEndDays.end() ? found->second : nowDays;
            const double base = std::max(nowDays, oldEnd);
            const double newEnd = base + kHumanoidContractDays;
            g_contractEndDays[actorId] = newEnd;
            const bool holdQueueForPleasure = IsReasonExtendContractPleasure(reason);
            ClearContractExpiryRuntimeForActorIdUnsafe(actorId, reason ? reason : "contract_extend");
            if (holdQueueForPleasure) {
                BeginContractExpiryPleasureHoldUnsafe(actorId, "extend_contract_pleasure");
            }
            spdlog::info("[TFD][TeammateManager] contract extended actor={:08X} oldEndDay={:.4f} newEndDay={:.4f} reason={} holdQueue={}",
                actorId,
                oldEnd,
                newEnd,
                reason ? reason : "unknown",
                holdQueueForPleasure ? 1 : 0);
            return true;
        }

        void RemoveContractForActorUnsafe(RE::Actor* actor, const char* reason)
        {
            if (!actor) {
                return;
            }
            const auto actorId = actor->GetFormID();
            g_contractEndDays.erase(actorId);
            ClearContractExpiryRuntimeForActorIdUnsafe(actorId, reason ? reason : "contract_clear");
            spdlog::info("[TFD][TeammateManager] contract cleared actor={:08X} reason={}",
                actorId,
                reason ? reason : "unknown");
        }

        bool ClearAliasForActorUnsafe(RE::Actor* actor, const char* reason)
        {
            ResolveRegistry();
            if (!actor || !g_registry.quest) {
                return false;
            }

            bool cleared = false;
            for (auto* alias : g_registry.teammateAliases) {
                if (!alias) {
                    continue;
                }
                auto* current = alias->GetActorReference();
                if (!current || current->GetFormID() != actor->GetFormID()) {
                    continue;
                }
                WriteAlias(alias, nullptr);
                cleared = true;
                spdlog::info("[TFD][TeammateManager] clear teammate alias alias='{}' actor={:08X} reason={}",
                    alias->aliasName.c_str(),
                    actor->GetFormID(),
                    reason ? reason : "unknown");
            }
            return cleared;
        }

        bool ReleaseHumanoidTeammateContractUnsafe(RE::Actor* actor, const char* reason)
        {
            if (!actor || actor == Player()) {
                return false;
            }

            ResolveRegistry();
            RemoveContractForActorUnsafe(actor, reason ? reason : "release_contract");
            ClearAliasForActorUnsafe(actor, reason ? reason : "release_contract");
            ForgetConvertedTeammate(actor, reason ? reason : "release_contract");

            if (g_registry.teammateFaction && actor->IsInFaction(g_registry.teammateFaction)) {
                actor->RemoveFromFaction(g_registry.teammateFaction);
            }
            if (g_registry.expiredTeammateFaction && actor->IsInFaction(g_registry.expiredTeammateFaction)) {
                actor->RemoveFromFaction(g_registry.expiredTeammateFaction);
            }

            if (!actor->IsDead() && !actor->IsDisabled()) {
                if (actor->IsInCombat()) {
                    actor->StopCombat();
                }
                if (auto* process = RE::ProcessLists::GetSingleton()) {
                    process->StopCombatAndAlarmOnActor(actor, false);
                }
                if (actor->IsWeaponDrawn()) {
                    actor->DrawWeaponMagicHands(false);
                }
                // CommonLibSSE-NG SE 1.5.97 does not expose Actor::SetPlayerTeammate().
                // Papyrus clears the engine teammate flag through Actor.SetPlayerTeammate(False, False).
                // Native owns alias/faction/contract cleanup only.
                if (actor->Is3DLoaded()) {
                    actor->EvaluatePackage();
                }
            }

            RefreshRecruitCapacityGlobalsUnsafe(reason ? reason : "release_contract");
            spdlog::info("[TFD][TeammateManager] humanoid contract terminated actor={:08X} reason={}",
                actor->GetFormID(),
                reason ? reason : "unknown");
            return true;
        }

        void RefreshRecruitCapacityGlobalsUnsafe(const char* reason)
        {
            ResolveRegistry();
            RefreshPotionGlobalUnsafe(reason ? reason : "recruit_capacity_refresh");

            std::vector<RE::Actor*> registeredStateActors;
            registeredStateActors.reserve(g_registry.teammateAliases.size());
            for (auto* alias : g_registry.teammateAliases) {
                if (!alias) {
                    continue;
                }

                auto* actor = alias->GetActorReference();
                if (!actor || !IsAliasManagedTeammate(actor)) {
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
            // TFDTeammateFaction / TFDExpiredTeammate are owned by TFDRecruit, contract-expiry, and temporary TFD follow flows.
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
                const bool actorHasExternalCombatTarget =
                    actorTarget &&
                    actorTarget != player &&
                    !actorTarget->IsDead() &&
                    !actorTarget->IsDisabled() &&
                    !IsPlayerSideTeammateAnchor(actorTarget) &&
                    !TFD::HostilityController::IsActorTemporarilySuppressed(actorTarget);
                const bool activeCombatConflict =
                    actorTargetsPlayer ||
                    playerTargetsActor ||
                    TFD::Recruit::HasKnownHostileSourceFaction(actor) ||
                    (actor->IsInCombat() && !actorHasExternalCombatTarget);

                // R80: do not run the heavy recruit commit path while a converted
                // teammate is already fighting a valid external enemy. CommitRecruit
                // clears combat and evaluates package; doing that during assist combat
                // can make the actor sheathe, idle, or wait until self-defense damage.
                if (actorHasExternalCombatTarget) {
                    spdlog::info(
                        "[TFD][TeammateManager] skip heavy alias resettle actor={:08X} reason=active_external_combat target={:08X}",
                        actor->GetFormID(),
                        actorTarget->GetFormID());
                    return;
                }

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

        void WriteQuestAlias(RE::TESQuest* quest, RE::BGSRefAlias* alias, RE::Actor* actor)
        {
            if (!quest || !alias) {
                return;
            }
            RE::ObjectRefHandle handle{};
            if (actor) {
                handle = actor->CreateRefHandle();
            }
            RE::BSWriteLockGuard lock(quest->aliasAccessLock);
            auto it = quest->refAliasMap.find(alias->aliasID);
            if (actor) {
                if (it != quest->refAliasMap.end()) {
                    it->second = handle;
                }
                else {
                    quest->refAliasMap.insert({ alias->aliasID, handle });
                }
            }
            else {
                if (it != quest->refAliasMap.end()) {
                    quest->refAliasMap.erase(it);
                }
            }
        }

        void WriteAlias(RE::BGSRefAlias* alias, RE::Actor* actor)
        {
            WriteQuestAlias(g_registry.quest, alias, actor);
        }

        RE::Actor* LookupActorById(RE::FormID actorId)
        {
            return actorId != 0 ? RE::TESForm::LookupByID<RE::Actor>(actorId) : nullptr;
        }

        void SyncExpiredFactionActiveUnsafe(RE::Actor* actor, bool active, const char* reason)
        {
            ResolveRegistry();
            if (!actor || !g_registry.expiredTeammateFaction) {
                return;
            }

            if (active) {
                if (actor->IsInFaction(g_registry.expiredTeammateFaction)) {
                    actor->RemoveFromFaction(g_registry.expiredTeammateFaction);
                }
                actor->AddToFaction(g_registry.expiredTeammateFaction, 1);
                spdlog::info("[TFD][TeammateManager] expired teammate faction active actor={:08X} rank=1 reason={}",
                    actor->GetFormID(),
                    reason ? reason : "unknown");
                return;
            }

            if (actor->IsInFaction(g_registry.expiredTeammateFaction)) {
                actor->RemoveFromFaction(g_registry.expiredTeammateFaction);
                spdlog::info("[TFD][TeammateManager] expired teammate faction cleared actor={:08X} reason={}",
                    actor->GetFormID(),
                    reason ? reason : "unknown");
            }
        }

        void ClearExpiredTeammateStatusForActorIdUnsafe(RE::FormID actorId, const char* reason)
        {
            ResolveRegistry();
            if (actorId == 0) {
                return;
            }

            bool clearedAlias = false;
            for (auto* alias : g_registry.expiredAliases) {
                if (!alias) {
                    continue;
                }
                auto* current = alias->GetActorReference();
                if (!current || current->GetFormID() != actorId) {
                    continue;
                }

                SyncExpiredFactionActiveUnsafe(current, false, reason ? reason : "clear_expired_status");
                WriteQuestAlias(g_registry.expiredQuest, alias, nullptr);
                clearedAlias = true;
                spdlog::info("[TFD][TeammateManager] expired quest alias cleared alias='{}' actor={:08X} reason={}",
                    alias->aliasName.c_str(),
                    actorId,
                    reason ? reason : "unknown");
            }

            if (auto* actor = LookupActorById(actorId)) {
                SyncExpiredFactionActiveUnsafe(actor, false, reason ? reason : "clear_expired_status_lookup");
            }

            if (clearedAlias && g_contractExpiryActiveActor == actorId) {
                g_contractExpiryActiveAliasIndex = -1;
            }
        }

        void ClearAllExpiredTeammateStatusUnsafe(const char* reason)
        {
            ResolveRegistry();
            for (auto* alias : g_registry.expiredAliases) {
                if (!alias) {
                    continue;
                }
                auto* current = alias->GetActorReference();
                if (current) {
                    SyncExpiredFactionActiveUnsafe(current, false, reason ? reason : "clear_all_expired_status");
                    spdlog::info("[TFD][TeammateManager] expired quest alias cleared alias='{}' actor={:08X} reason={}",
                        alias->aliasName.c_str(),
                        current->GetFormID(),
                        reason ? reason : "unknown");
                }
                WriteQuestAlias(g_registry.expiredQuest, alias, nullptr);
            }
        }

        int FindTeammateAliasIndexForActorUnsafe(RE::Actor* actor)
        {
            if (!actor) {
                return -1;
            }
            const auto actorId = actor->GetFormID();
            for (std::size_t i = 0; i < g_registry.teammateAliases.size(); ++i) {
                auto* alias = g_registry.teammateAliases[i];
                if (!alias) {
                    continue;
                }
                auto* current = alias->GetActorReference();
                if (current && current->GetFormID() == actorId) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

        bool ArmExpiredTeammateStatusUnsafe(RE::Actor* actor, int teammateAliasIndex, const char* reason, bool resetNativeFallbackGrace)
        {
            ResolveRegistry();
            if (!actor || teammateAliasIndex < 0 || teammateAliasIndex >= static_cast<int>(g_registry.expiredAliases.size())) {
                return false;
            }
            if (!g_registry.expiredQuest) {
                return false;
            }

            auto* targetAlias = g_registry.expiredAliases[static_cast<std::size_t>(teammateAliasIndex)];
            if (!targetAlias) {
                spdlog::warn("[TFD][TeammateManager] expired quest alias missing index={} actor={:08X} reason={}",
                    teammateAliasIndex + 1,
                    actor->GetFormID(),
                    reason ? reason : "unknown");
                return false;
            }

            const auto actorId = actor->GetFormID();
            const auto reasonText = reason ? reason : "arm_expired_teammate";

            auto* targetCurrent = targetAlias->GetActorReference();
            const bool targetAlreadySet = targetCurrent && targetCurrent->GetFormID() == actorId;
            const bool factionMissing = g_registry.expiredTeammateFaction && !actor->IsInFaction(g_registry.expiredTeammateFaction);
            const bool shouldPrime = !targetAlreadySet || factionMissing || resetNativeFallbackGrace;
            bool changed = false;

            // Important ordering:
            // CK can start evaluating the expired alias package as soon as the alias is filled.
            // The expired dialogue options are conditioned on TFDExpiredTeammate, so mark the
            // actor as expired BEFORE filling the alias. Otherwise the first forcegreet can open
            // with the branch hidden until the next retry.
            if (shouldPrime) {
                if (targetCurrent && targetCurrent->GetFormID() != actorId) {
                    SyncExpiredFactionActiveUnsafe(targetCurrent, false, "expired_alias_replaced");
                }

                SyncExpiredFactionActiveUnsafe(actor, true, reasonText);
                actor->AllowPCDialogue(true);
                if (!actor->IsAIEnabled()) {
                    actor->EnableAI(true);
                }
                if (actor->IsWeaponDrawn()) {
                    actor->DrawWeaponMagicHands(false);
                }

                spdlog::info("[TFD][TeammateManager] expired teammate prearmed actor={:08X} faction=1 slot={} reason={}",
                    actorId,
                    teammateAliasIndex + 1,
                    reasonText);
            }

            for (std::size_t i = 0; i < g_registry.expiredAliases.size(); ++i) {
                auto* alias = g_registry.expiredAliases[i];
                if (!alias) {
                    continue;
                }

                auto* current = alias->GetActorReference();
                if (i == static_cast<std::size_t>(teammateAliasIndex)) {
                    if (!current || current->GetFormID() != actorId) {
                        WriteQuestAlias(g_registry.expiredQuest, alias, actor);
                        changed = true;
                    }
                    continue;
                }

                if (current) {
                    if (current->GetFormID() != actorId) {
                        SyncExpiredFactionActiveUnsafe(current, false, "expired_alias_single_active_guard");
                    }
                    WriteQuestAlias(g_registry.expiredQuest, alias, nullptr);
                    changed = true;
                    spdlog::info("[TFD][TeammateManager] expired quest alias cleared alias='{}' actor={:08X} reason=single_active_guard",
                        alias->aliasName.c_str(),
                        current->GetFormID());
                }
            }

            if (shouldPrime || changed) {
                // R66: do not immediately EvaluatePackage here.
                // The expired alias package can forcegreet before the dialogue conditions finish settling,
                // which skips the top-level "extend contract" branch and jumps straight into the cure choices.
                // Native owns the first expired-contract open; the expired quest/faction remains the status marker.
                if (resetNativeFallbackGrace) {
                    g_contractExpiryNextAttemptSec.erase(actorId);
                    g_contractExpiryOpenAttempts.erase(actorId);
                }

                spdlog::info("[TFD][TeammateManager] expired teammate armed alias='{}' actor={:08X} slot={} nativeFallbackGrace={:.1f}s prearmed=1 packageEval=0 nativeOwnsOpen=1 reason={}",
                    targetAlias->aliasName.c_str(),
                    actorId,
                    teammateAliasIndex + 1,
                    resetNativeFallbackGrace ? kContractExpiryPackageGraceSeconds : 0.0,
                    reasonText);
            }

            return true;
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
                if (actor && TFD::Actor::Ops::HasReleaseFollowGrace(actor)) {
                    spdlog::info(
                        "[TFD][TeammateManager][R93V] skip release-follow grace actor={:08X} reason=collect_nearby",
                        actor->GetFormID());
                    continue;
                }
                if (!IsAliasManagedTeammate(actor)) {
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

        void DeferHumanoidTeammateAssignEvent(RE::Actor* actor, const char* reason, const char* deferReason)
        {
            if (!actor) {
                return;
            }

            const auto actorId = actor->GetFormID();
            if (actorId == 0) {
                return;
            }

            const double now = NowRealSeconds();
            std::size_t pendingCount = 0;
            std::uint32_t attempts = 0;
            {
                std::scoped_lock lk(g_deferredHumanoidAssignLock);
                auto& entry = g_deferredHumanoidAssigns[actorId];
                if (reason && reason[0] != '\0') {
                    entry.reason = reason;
                }
                else if (entry.reason.empty()) {
                    entry.reason = "deferred_humanoid_assign";
                }
                entry.dueSec = now + kDeferredHumanoidAssignDelaySeconds;
                attempts = entry.attempts;
                pendingCount = g_deferredHumanoidAssigns.size();
            }

            spdlog::info(
                "[TFD][TeammateManager][R95C] deferred humanoid assign actor={:08X} reason={} deferReason={} attempts={} pending={} due={:.2f}",
                actorId,
                reason ? reason : "unknown",
                deferReason ? deferReason : "unknown",
                attempts,
                static_cast<unsigned int>(pendingCount),
                kDeferredHumanoidAssignDelaySeconds);
        }

        void ClearDeferredHumanoidTeammateAssignEvent(RE::Actor* actor, const char* reason)
        {
            if (!actor) {
                return;
            }

            const auto actorId = actor->GetFormID();
            if (actorId == 0) {
                return;
            }

            bool removed = false;
            std::size_t pendingCount = 0;
            {
                std::scoped_lock lk(g_deferredHumanoidAssignLock);
                removed = g_deferredHumanoidAssigns.erase(actorId) > 0;
                pendingCount = g_deferredHumanoidAssigns.size();
            }

            if (removed) {
                spdlog::info(
                    "[TFD][TeammateManager][R96D] deferred humanoid assign cleared actor={:08X} reason={} pending={}",
                    actorId,
                    reason ? reason : "unknown",
                    static_cast<unsigned int>(pendingCount));
            }
        }

        bool QueueHumanoidTeammateAssignEvent(RE::Actor* actor, const char* reason, bool allowDuringInCombatPleasureChain = false)
        {
            if (!actor) {
                return false;
            }

            const bool chainActive = TFD::PleasureRuntime::IsInCombatPleasureChainActive();
            if (chainActive && !allowDuringInCombatPleasureChain) {
                DeferHumanoidTeammateAssignEvent(actor, reason, "incombat_pleasure_chain_active");
                spdlog::info(
                    "[TFD][TeammateManager][R94G] queue humanoid assign deferred by incombat pleasure chain actor={:08X} reason={}",
                    actor->GetFormID(),
                    reason ? reason : "unknown");
                return false;
            }

            if (chainActive && allowDuringInCombatPleasureChain) {
                ClearDeferredHumanoidTeammateAssignEvent(actor, reason ? reason : "force_assign_during_chain");
                spdlog::info(
                    "[TFD][TeammateManager][R96D] queue humanoid assign bypass chain actor={:08X} reason={}",
                    actor->GetFormID(),
                    reason ? reason : "unknown");
            }

            auto* combatTarget = ResolveCombatTarget(actor);
            const bool combatDiagActor = TFD::CombatBehavior::IsDiagnosticActor(actor);
            const bool combatDiagTarget = TFD::CombatBehavior::IsDiagnosticActor(combatTarget);
            const char* useReason = reason ? reason : "teammate_manager_register_now";

            if (TFD::CombatBehavior::ShouldSuppressTeammatePackageRepair(actor, useReason)) {
                spdlog::info(
                    "[TFD][TeammateManager] queue humanoid assign skipped actor={:08X} reason={} rule=active_combat_behavior target={:08X} actorRole={} targetRole={} actorCombat={}",
                    actor->GetFormID(),
                    useReason,
                    combatTarget ? combatTarget->GetFormID() : 0u,
                    TFD::CombatBehavior::DiagnosticRole(actor),
                    TFD::CombatBehavior::DiagnosticRole(combatTarget),
                    actor->IsInCombat() ? 1 : 0);
                return false;
            }

            const bool queued = TFD::FlowController::QueueBridgeModEvent(
                "TFDHumanoidTeammateAssign",
                actor,
                useReason,
                0.0f);

            spdlog::info(
                "[TFD][TeammateManager] queue humanoid assign actor={:08X} reason={} ok={} combatDiagActor={} actorRole={} target={:08X} targetRole={} targetDiag={}",
                actor->GetFormID(),
                useReason,
                queued ? 1 : 0,
                combatDiagActor ? 1 : 0,
                TFD::CombatBehavior::DiagnosticRole(actor),
                combatTarget ? combatTarget->GetFormID() : 0u,
                TFD::CombatBehavior::DiagnosticRole(combatTarget),
                combatDiagTarget ? 1 : 0);

            if (combatDiagActor || combatDiagTarget) {
                spdlog::info(
                    "[TFD][TeammateManagerDiag] queue_during_combat_behavior actor={:08X} reason={} actorCombat={} target={:08X} actorRole={} targetRole={}",
                    actor->GetFormID(),
                    useReason,
                    actor->IsInCombat() ? 1 : 0,
                    combatTarget ? combatTarget->GetFormID() : 0u,
                    TFD::CombatBehavior::DiagnosticRole(actor),
                    TFD::CombatBehavior::DiagnosticRole(combatTarget));
            }

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
                "[TFD][TeammateManager] invalid alias diag alias='{}' actor={:08X} action={} reason={} strikes={} maxGrace={} hasActor={} dead={} disabled={} loaded={} tfdRank={} expiredRank={} knownConverted={} nowConverted={} playerTeammate={} currentFollower={} playerFollower={} pendingRecruit={} inCombat={} actorTarget={:08X} playerTarget={:08X} rawHostile={} hostileFaction={} dist={:.1f}",
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
                FactionRank(actor, g_registry.expiredTeammateFaction),
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

            // R93V: Do not preserve aliases that were created only because an
            // InCombat Release/Follow grace marker looked like a converted teammate.
            // These actors are still hostile bandits with no current TFD teammate
            // marker, so preserving the alias re-applies teammate packages/contracts
            // and causes the cross-save pacify bug.
            if (TFD::Actor::Ops::HasReleaseFollowGrace(actor)) {
                ClearStrayExpiredHelperFactionUnsafe(actor, "alias_clear_release_follow_grace");
                ForgetConvertedTeammate(actor, "alias_clear_release_follow_grace");
                LogInvalidAliasDiagnostic(alias, actor, "clear_release_follow_grace", reason, 0);
                return false;
            }

            const bool hasCurrentTfdMarker = HasAnyTFDTeammateFactionNow(actor);
            const bool hasNaturalFollowerState = actor->IsPlayerTeammate() || HasFollowerAnchorFaction(actor);
            if (!hasCurrentTfdMarker && !hasNaturalFollowerState && HasPlayerHostility(actor)) {
                ClearStrayExpiredHelperFactionUnsafe(actor, "alias_clear_hostile_stale_conversion");
                ForgetConvertedTeammate(actor, "alias_clear_hostile_stale_conversion");
                LogInvalidAliasDiagnostic(alias, actor, "clear_hostile_stale_conversion", reason, 0);
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
            spdlog::info("[TFD][TeammateManager] R86 vanilla assist isolation: humanoid post-load catchup skipped reason={} attempt={}", reason ? reason : "post_load_humanoid_catchup", attempt);
            return 0;

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

                auto* combatTarget = ResolveCombatTarget(actor);
                const bool hasValidExternalCombatTarget =
                    combatTarget &&
                    combatTarget != player &&
                    !combatTarget->IsDead() &&
                    !combatTarget->IsDisabled() &&
                    !IsPlayerSideTeammateAnchor(combatTarget) &&
                    !TFD::HostilityController::IsActorTemporarilySuppressed(combatTarget);

                const bool combatDiagActor = TFD::CombatBehavior::IsDiagnosticActor(actor);
                const bool combatDiagTarget = TFD::CombatBehavior::IsDiagnosticActor(combatTarget);
                const char* useReason = reason ? reason : "post_load_humanoid_catchup";
                const bool combatBehaviorProtected = TFD::CombatBehavior::ShouldSuppressTeammatePackageRepair(actor, useReason);
                const bool vanillaAssistIsolationProtected = IsVanillaAssistIsolationCombatPressure(actor, player, combatTarget);
                const bool packageRepairProtected = combatBehaviorProtected || vanillaAssistIsolationProtected;

                // R30: do not rubber-band followers during ordinary running.
                // Move only on the final post-loading pass, and only if the actor
                // is still unloaded, in a different cell and far away, or extremely far.
                // R85: do not move/clear/recommit during live combat pressure. This keeps the
                // vanilla follower package stack isolated from TFD catchup repair.
                const bool allowEmergencyMove = attempt >= 3;
                const bool shouldMove = !packageRepairProtected && allowEmergencyMove && (unloaded || far || wrongCellFar);


                if (!hasValidExternalCombatTarget && !packageRepairProtected) {
                    if (combatDiagActor || combatDiagTarget) {
                        spdlog::info(
                            "[TFD][TeammateManagerDiag] catchup_clear_before actor={:08X} reason={} attempt={} target={:08X} actorCombat={} actorRole={} targetRole={} wrongCell={} unloaded={} dist={:.1f}",
                            actor->GetFormID(),
                            reason ? reason : "post_load_humanoid_catchup",
                            attempt,
                            combatTarget ? combatTarget->GetFormID() : 0u,
                            actor->IsInCombat() ? 1 : 0,
                            TFD::CombatBehavior::DiagnosticRole(actor),
                            TFD::CombatBehavior::DiagnosticRole(combatTarget),
                            wrongCell ? 1 : 0,
                            unloaded ? 1 : 0,
                            dist);
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
                    if (combatDiagActor || combatDiagTarget) {
                        auto* targetAfterClear = ResolveCombatTarget(actor);
                        spdlog::info(
                            "[TFD][TeammateManagerDiag] catchup_clear_after actor={:08X} reason={} attempt={} targetAfter={:08X} actorCombat={} actorRole={} targetAfterRole={}",
                            actor->GetFormID(),
                            reason ? reason : "post_load_humanoid_catchup",
                            attempt,
                            targetAfterClear ? targetAfterClear->GetFormID() : 0u,
                            actor->IsInCombat() ? 1 : 0,
                            TFD::CombatBehavior::DiagnosticRole(actor),
                            TFD::CombatBehavior::DiagnosticRole(targetAfterClear));
                    }
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
                options.clearCombat = !hasValidExternalCombatTarget && !packageRepairProtected;
                options.evaluatePackage = false;
                options.detailedLog = false;
                options.throttleObserve = true;
                options.ensurePacifyAlliance = true;
                options.applyRuntimeProfile = true;
                if (!packageRepairProtected) {
                    TFD::Recruit::CommitRecruit(actor, player, options);
                    assigned = QueueHumanoidTeammateAssignEvent(actor, useReason);

                    const bool shouldEvaluateForPackageRepair = moved || wrongCell || dist > kEvaluateDistance || !actor->Is3DLoaded();
                    if (shouldEvaluateForPackageRepair) {
                        actor->EvaluatePackage();
                        evaluated = true;
                    }
                }
                else {
                    spdlog::info(
                        "[TFD][TeammateManager] post-load package repair skipped actor={:08X} reason={} rule={} target={:08X} actorRole={} targetRole={} actorCombat={}",
                        actor->GetFormID(),
                        useReason,
                        vanillaAssistIsolationProtected ? "vanilla_assist_isolation" : "active_combat_behavior",
                        combatTarget ? combatTarget->GetFormID() : 0u,
                        TFD::CombatBehavior::DiagnosticRole(actor),
                        TFD::CombatBehavior::DiagnosticRole(combatTarget),
                        actor->IsInCombat() ? 1 : 0);
                }

                spdlog::info(
                    "[TFD][TeammateManager] humanoid post-load catchup actor={:08X} reason={} attempt={} moved={} assigned={} evaluated={} wrongCell={} unloaded={} activeCombatPreserved={} dist={:.1f} combatDiagActor={} actorRole={} target={:08X} targetRole={} targetDiag={}",
                    actor->GetFormID(),
                    reason ? reason : "post_load_humanoid_catchup",
                    attempt,
                    moved ? 1 : 0,
                    assigned ? 1 : 0,
                    evaluated ? 1 : 0,
                    wrongCell ? 1 : 0,
                    unloaded ? 1 : 0,
                    (hasValidExternalCombatTarget || packageRepairProtected) ? 1 : 0,
                    dist,
                    combatDiagActor ? 1 : 0,
                    TFD::CombatBehavior::DiagnosticRole(actor),
                    combatTarget ? combatTarget->GetFormID() : 0u,
                    TFD::CombatBehavior::DiagnosticRole(combatTarget),
                    combatDiagTarget ? 1 : 0);

                ++touched;
            }

            return touched;
        }

        void QueueHumanoidTeammateCatchupAfterLoad(const char* reason)
        {
            spdlog::info("[TFD][TeammateManager] R86 vanilla assist isolation: humanoid post-load catchup queue ignored reason={}", reason && reason[0] ? reason : "post_load_humanoid_catchup");
            return;

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

        bool RegisterOrRefreshAliasForActor(RE::Actor* actor, const char* reason, bool forceAssignDuringInCombatPleasureChain = false)
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

            if (!IsAliasManagedTeammate(actor)) {
                const bool pendingRecruit = TFD::Recruit::IsRecruitCommitPending(actor);
                const bool naturalPlayerSide = IsNaturalPlayerSideTeammate(actor);
                spdlog::warn(
                    "[TFD][TeammateManager] register now rejected actor={:08X} reason={} detail={} pendingRecruit={} naturalPlayerSide={}",
                    actor->GetFormID(),
                    reason ? reason : "unknown",
                    pendingRecruit ? "recruit_commit_pending_no_alias_pre_marker" : (naturalPlayerSide ? "natural_follower_not_alias_managed" : "not_tfd_managed_teammate"),
                    pendingRecruit ? 1 : 0,
                    naturalPlayerSide ? 1 : 0);
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
                    const std::string_view reasonView{ reason ? reason : "" };
                    const bool manualDialogueRefresh = IsManualTeammateDialogueRefreshReason(reasonView);

                    if (IsTFDConvertedTeammate(actor)) {
                        RememberConvertedTeammate(actor, reason ? reason : "register_now_refresh_converted");
                        ResetInvalidAliasStrike(actor);
                    }
                    SyncTeammateFaction(actor, true);
                    EnsureContractForActorUnsafe(actor, reason ? reason : "register_now_refresh");

                    // R56: manual teammate dialogue activation is only a dialogue prep pass.
                    // Do not bounce Papyrus alias assignment or force EvaluatePackage here;
                    // the actor is already in this alias slot and package churn can create
                    // small follow/dialogue timing artifacts. Initial recruit and real alias
                    // repair still use the normal event/evaluate path.
                    bool queuedAssign = false;
                    bool evaluatedPackage = false;
                    if (!manualDialogueRefresh) {
                        const bool chainLocked = TFD::PleasureRuntime::IsInCombatPleasureChainActive();
                        queuedAssign = QueueHumanoidTeammateAssignEvent(
                            actor,
                            reason ? reason : "register_now_refresh",
                            forceAssignDuringInCombatPleasureChain);
                        if ((forceAssignDuringInCombatPleasureChain || !chainLocked) && actor->Is3DLoaded()) {
                            actor->EvaluatePackage();
                            evaluatedPackage = true;
                        }
                    }

                    spdlog::info(
                        "[TFD][TeammateManager] register now {} alias='{}' actor={:08X} reason={} queued={} eval={} forceDuringChain={}",
                        manualDialogueRefresh ? "manual_refresh" : "refresh",
                        alias->aliasName.c_str(),
                        actor->GetFormID(),
                        reason ? reason : "unknown",
                        queuedAssign ? 1 : 0,
                        evaluatedPackage ? 1 : 0,
                        forceAssignDuringInCombatPleasureChain ? 1 : 0);

                    if (!manualDialogueRefresh) {
                        RefreshRecruitCapacityGlobalsUnsafe(reason ? reason : "register_now_refresh");
                    }
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
                QueueHumanoidTeammateAssignEvent(actor, reason ? reason : "register_now_no_slot_bridge_only", forceAssignDuringInCombatPleasureChain);
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
            EnsureContractForActorUnsafe(actor, reason ? reason : "register_now_assign");
            bool queuedAssign = false;
            bool evaluatedPackage = false;
            {
                const bool chainLocked = TFD::PleasureRuntime::IsInCombatPleasureChainActive();
                queuedAssign = QueueHumanoidTeammateAssignEvent(
                    actor,
                    reason ? reason : "register_now_assign",
                    forceAssignDuringInCombatPleasureChain);
                if ((forceAssignDuringInCombatPleasureChain || !chainLocked) && actor->Is3DLoaded()) {
                    actor->EvaluatePackage();
                    evaluatedPackage = true;
                }
            }

            spdlog::info(
                "[TFD][TeammateManager] register now fill alias='{}' actor={:08X} name='{}' reason={} queued={} eval={} forceDuringChain={}",
                targetAlias->aliasName.c_str(),
                actor->GetFormID(),
                actor->GetName() ? actor->GetName() : "",
                reason ? reason : "unknown",
                queuedAssign ? 1 : 0,
                evaluatedPackage ? 1 : 0,
                forceAssignDuringInCombatPleasureChain ? 1 : 0);

            RefreshRecruitCapacityGlobalsUnsafe(reason ? reason : "register_now_fill");
            return true;
        }

        bool IsContractExpiredUnsafe(RE::Actor* actor, double nowDays, double* endDayOut = nullptr)
        {
            if (!actor || actor == Player() || actor->IsDead() || actor->IsDisabled()) {
                return false;
            }
            if (!IsTFDConvertedTeammate(actor)) {
                return false;
            }

            const auto actorId = actor->GetFormID();
            const auto found = g_contractEndDays.find(actorId);
            if (found == g_contractEndDays.end()) {
                return false;
            }

            if (endDayOut) {
                *endDayOut = found->second;
            }
            return found->second <= nowDays;
        }

        bool CanOpenContractExpiredGreetUnsafe(RE::Actor* actor, double nowReal, const char*& blockReason, double& retryDelaySec)
        {
            blockReason = "none";
            retryDelaySec = kContractExpiryRetryBlockedSeconds;

            auto* player = Player();
            if (!actor || !player) {
                blockReason = "missing_actor_or_player";
                return false;
            }
            if (actor == player || actor->IsDead() || actor->IsDisabled()) {
                blockReason = "invalid_actor";
                return false;
            }
            if (!IsTFDConvertedTeammate(actor)) {
                blockReason = "not_tfd_converted";
                return false;
            }
            if (g_contractExpiryActiveActor != 0 && actor->GetFormID() != g_contractExpiryActiveActor) {
                blockReason = "not_active_expired_actor";
                retryDelaySec = 6.0;
                return false;
            }
            if (!actor->Is3DLoaded()) {
                blockReason = "not_3d_loaded";
                return false;
            }
            if (IsBlockingMenuOpen()) {
                blockReason = "menu_open";
                retryDelaySec = 4.0;
                return false;
            }
            if (TFD::FlowController::IsDialogueContextActive() || TFD::FlowController::IsPassiveHoldActive() || TFD::FlowController::IsPleasureLockActive()) {
                blockReason = "tfd_context_active";
                retryDelaySec = 6.0;
                return false;
            }
            if (player->IsInCombat() || actor->IsInCombat()) {
                blockReason = "combat_active";
                retryDelaySec = 10.0;
                return false;
            }
            if (HasPlayerHostility(actor)) {
                blockReason = "hostility_active";
                retryDelaySec = 10.0;
                return false;
            }

            const float distance = actor->GetPosition().GetDistance(player->GetPosition());
            if (distance > kContractExpiryForcegreetRadius) {
                blockReason = "too_far";
                retryDelaySec = 12.0;
                return false;
            }

            const auto retryIt = g_contractExpiryNextAttemptSec.find(actor->GetFormID());
            if (retryIt != g_contractExpiryNextAttemptSec.end() && retryIt->second > nowReal) {
                blockReason = "cooldown";
                retryDelaySec = std::max(1.0, retryIt->second - nowReal);
                return false;
            }

            return true;
        }

        bool TryOpenContractExpiredGreetUnsafe(RE::Actor* actor, double nowDays, double nowReal)
        {
            if (!actor) {
                return false;
            }

            double endDay = 0.0;
            if (!IsContractExpiredUnsafe(actor, nowDays, &endDay)) {
                return false;
            }

            const auto actorId = actor->GetFormID();
            const char* blockReason = "none";
            double retryDelaySec = kContractExpiryRetryBlockedSeconds;
            if (!CanOpenContractExpiredGreetUnsafe(actor, nowReal, blockReason, retryDelaySec)) {
                if (blockReason && std::string_view(blockReason) != "cooldown") {
                    g_contractExpiryNextAttemptSec[actorId] = nowReal + retryDelaySec;
                    spdlog::info("[TFD][TeammateManager] contract expired forcegreet deferred actor={:08X} reason={} retry={:.1f}s endDay={:.4f} nowDay={:.4f}",
                        actorId,
                        blockReason ? blockReason : "unknown",
                        retryDelaySec,
                        endDay,
                        nowDays);
                }
                return false;
            }

            if (!actor->IsAIEnabled()) {
                actor->EnableAI(true);
            }
            if (actor->IsWeaponDrawn()) {
                actor->DrawWeaponMagicHands(false);
            }
            if (auto* process = RE::ProcessLists::GetSingleton()) {
                process->StopCombatAndAlarmOnActor(actor, false);
            }

            actor->AllowPCDialogue(true);
            // R66: avoid EvaluatePackage before the native root open. The expired alias package can
            // start its own forcegreet path and skip the top-level contract branch.
            auto* teammateGreetInfo = ResolveTeammateGreetTopicInfo();
            actor->SetDialogueWithPlayer(false, false, nullptr);
            const bool opened = actor->SetDialogueWithPlayer(true, true, teammateGreetInfo);

            g_contractExpiryOpenAttempts[actorId] += 1;
            g_contractExpiryNextAttemptSec[actorId] = nowReal + (opened ? kContractExpiryRetryAfterOpenSeconds : kContractExpiryRetryFailedSeconds);

            spdlog::info("[TFD][TeammateManager] contract expired native root forcegreet actor={:08X} opened={} attempts={} endDay={:.4f} nowDay={:.4f} topicInfo={:08X} nextRetry={:.1f}s",
                actorId,
                opened ? 1 : 0,
                g_contractExpiryOpenAttempts[actorId],
                endDay,
                nowDays,
                teammateGreetInfo ? teammateGreetInfo->GetFormID() : 0u,
                opened ? kContractExpiryRetryAfterOpenSeconds : kContractExpiryRetryFailedSeconds);

            return opened;
        }

        void ProcessExpiredContractsUnsafe()
        {
            ResolveRegistry();
            if (!g_registry.quest) {
                return;
            }

            const double nowDays = CurrentGameDays();
            const double nowReal = NowRealSeconds();

            struct ExpiredEntry
            {
                RE::Actor* actor{ nullptr };
                RE::BGSRefAlias* alias{ nullptr };
                int aliasIndex{ -1 };
                double endDay{ 0.0 };
            };

            std::vector<ExpiredEntry> expired;
            expired.reserve(g_registry.teammateAliases.size());

            for (std::size_t i = 0; i < g_registry.teammateAliases.size(); ++i) {
                auto* alias = g_registry.teammateAliases[i];
                if (!alias) {
                    continue;
                }

                auto* actor = alias->GetActorReference();
                if (!actor) {
                    continue;
                }

                double endDay = 0.0;
                if (!IsContractExpiredUnsafe(actor, nowDays, &endDay)) {
                    continue;
                }

                const auto actorId = actor->GetFormID();
                const bool firstExpiredMark = g_expiredContractActors.insert(actorId).second;
                if (firstExpiredMark) {
                    spdlog::info("[TFD][TeammateManager] contract expired detected actor={:08X} alias='{}' slot={} endDay={:.4f} nowDay={:.4f}",
                        actorId,
                        alias->aliasName.c_str(),
                        static_cast<int>(i) + 1,
                        endDay,
                        nowDays);
                }

                expired.push_back(ExpiredEntry{ actor, alias, static_cast<int>(i), endDay });
            }

            if (expired.empty()) {
                if (g_contractExpiryActiveActor != 0) {
                    spdlog::info("[TFD][TeammateManager] contract expiry active actor cleared actor={:08X} reason=no_expired_contracts",
                        g_contractExpiryActiveActor);
                }
                g_contractExpiryActiveActor = 0;
                g_contractExpiryActiveAliasIndex = -1;
                ClearAllExpiredTeammateStatusUnsafe("no_expired_contracts");
                return;
            }

            if (IsContractExpiryPleasureHoldActiveUnsafe(nowReal)) {
                if (g_contractExpiryActiveActor != 0) {
                    spdlog::info("[TFD][TeammateManager] contract expiry active actor cleared actor={:08X} reason=pleasure_queue_hold",
                        g_contractExpiryActiveActor);
                    ClearExpiredTeammateStatusForActorIdUnsafe(g_contractExpiryActiveActor, "pleasure_queue_hold");
                    g_contractExpiryActiveActor = 0;
                    g_contractExpiryActiveAliasIndex = -1;
                }
                return;
            }

            ExpiredEntry* active = nullptr;
            if (g_contractExpiryActiveActor != 0) {
                for (auto& entry : expired) {
                    if (entry.actor && entry.actor->GetFormID() == g_contractExpiryActiveActor) {
                        active = &entry;
                        break;
                    }
                }

                if (!active) {
                    spdlog::info("[TFD][TeammateManager] contract expiry active actor cleared actor={:08X} reason=active_actor_missing_or_resolved",
                        g_contractExpiryActiveActor);
                    ClearExpiredTeammateStatusForActorIdUnsafe(g_contractExpiryActiveActor, "active_actor_missing_or_resolved");
                    g_contractExpiryActiveActor = 0;
                    g_contractExpiryActiveAliasIndex = -1;
                }
            }

            if (!active) {
                active = &expired.front();
                g_contractExpiryActiveActor = active->actor ? active->actor->GetFormID() : 0;
                g_contractExpiryActiveAliasIndex = active->aliasIndex;

                spdlog::info("[TFD][TeammateManager] contract expiry queue active actor={:08X} alias='{}' slot={} pending={} expired={} endDay={:.4f} nowDay={:.4f}",
                    g_contractExpiryActiveActor,
                    active->alias ? active->alias->aliasName.c_str() : "",
                    active->aliasIndex + 1,
                    expired.size() > 0 ? static_cast<unsigned>(expired.size() - 1) : 0u,
                    static_cast<unsigned>(expired.size()),
                    active->endDay,
                    nowDays);

                if (active->actor) {
                    const bool armed = ArmExpiredTeammateStatusUnsafe(active->actor, active->aliasIndex, "contract_expired_queue_activate", true);
                    if (armed) {
                        TryOpenContractExpiredGreetUnsafe(active->actor, nowDays, nowReal);
                    }
                }
                return;
            }

            const unsigned pending = expired.size() > 0 ? static_cast<unsigned>(expired.size() - 1) : 0u;
            spdlog::debug("[TFD][TeammateManager] contract expiry active actor retained actor={:08X} slot={} pending={} expired={}",
                active->actor ? active->actor->GetFormID() : 0u,
                active->aliasIndex + 1,
                pending,
                static_cast<unsigned>(expired.size()));

            if (active->actor) {
                ArmExpiredTeammateStatusUnsafe(active->actor, active->aliasIndex, "contract_expired_queue_retain", false);
                TryOpenContractExpiredGreetUnsafe(active->actor, nowDays, nowReal);
            }
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

                if (!IsAliasManagedTeammate(current)) {
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
                EnsureContractForActorUnsafe(current, "sync_alias_valid");
                remaining.erase(std::remove_if(remaining.begin(), remaining.end(), [&](RE::Actor* actor) {
                    return actor == current || (actor && current && actor->GetFormID() == current->GetFormID());
                    }), remaining.end());
            }

            for (auto* alias : g_registry.teammateAliases) {
                if (!alias || remaining.empty()) {
                    continue;
                }
                auto* current = alias->GetActorReference();
                if (current && IsAliasManagedTeammate(current)) {
                    continue;
                }
                if (current && IsKnownConvertedAliasActor(current) && !IsHardInvalidConvertedAliasActor(current)) {
                    continue;
                }
                auto* actor = remaining.front();
                remaining.erase(remaining.begin());
                WriteAlias(alias, actor);
                const bool convertedActor = IsTFDConvertedTeammate(actor);
                if (convertedActor) {
                    RememberConvertedTeammate(actor, "sync_fill_converted");
                    ResetInvalidAliasStrike(actor);
                }
                SyncTeammateFaction(actor, true);
                EnsureContractForActorUnsafe(actor, "sync_fill_alias");

                bool queuedAssign = false;
                bool evaluatedPackage = false;
                const bool chainLocked = TFD::PleasureRuntime::IsInCombatPleasureChainActive();
                if (convertedActor) {
                    queuedAssign = QueueHumanoidTeammateAssignEvent(actor, "sync_fill_alias");
                    if (!chainLocked && actor && actor->Is3DLoaded()) {
                        actor->EvaluatePackage();
                        evaluatedPackage = true;
                    }
                }

                spdlog::info("[TFD][TeammateManager][R95B] fill alias='{}' actor={:08X} name='{}' converted={} chainLocked={} queuedAssign={} evalPackage={}",
                    alias->aliasName.c_str(),
                    actor ? actor->GetFormID() : 0u,
                    actor && actor->GetName() ? actor->GetName() : "",
                    convertedActor ? 1 : 0,
                    chainLocked ? 1 : 0,
                    queuedAssign ? 1 : 0,
                    evaluatedPackage ? 1 : 0);
            }
            RefreshRecruitCapacityGlobalsUnsafe("sync_aliases");
            ProcessExpiredContractsUnsafe();

        }

        void FlushDeferredHumanoidTeammateAssignEvents(const char* reason)
        {
            const double now = NowRealSeconds();

            if (TFD::PleasureRuntime::IsInCombatPleasureChainActive()) {
                std::size_t postponed = 0;
                std::uint32_t maxAttempts = 0;
                {
                    std::scoped_lock lk(g_deferredHumanoidAssignLock);
                    for (auto& [actorId, entry] : g_deferredHumanoidAssigns) {
                        if (entry.dueSec <= now) {
                            // R96B: waiting for an active InCombat pleasure chain is not a failed assign attempt.
                            // The old R95C code incremented attempts every UI tick while the chain was still
                            // valid, so the first recruited actor could hit max_attempts and get dropped before
                            // the chain ended. Keep the request alive and only count real dispatch failures.
                            entry.dueSec = now + kDeferredHumanoidAssignRetrySeconds;
                            maxAttempts = std::max(maxAttempts, entry.attempts);
                            ++postponed;
                        }
                    }
                }

                if (postponed > 0) {
                    spdlog::info(
                        "[TFD][TeammateManager][R96B] deferred humanoid assign wait preserved count={} reason={} rule=incombat_pleasure_chain_active maxAttempts={}",
                        static_cast<unsigned int>(postponed),
                        reason ? reason : "unknown",
                        maxAttempts);
                }
                return;
            }

            struct DueAssign
            {
                RE::FormID actorId{ 0 };
                std::string reason{};
                std::uint32_t attempts{ 0 };
            };

            std::vector<DueAssign> due;
            {
                std::scoped_lock lk(g_deferredHumanoidAssignLock);
                for (auto it = g_deferredHumanoidAssigns.begin(); it != g_deferredHumanoidAssigns.end();) {
                    auto& entry = it->second;
                    if (entry.dueSec > now) {
                        ++it;
                        continue;
                    }

                    if (entry.attempts >= kDeferredHumanoidAssignMaxAttempts) {
                        spdlog::warn(
                            "[TFD][TeammateManager][R96B] deferred humanoid assign dropped actor={:08X} attempts={} reason={} rule=max_dispatch_failures",
                            it->first,
                            entry.attempts,
                            entry.reason.empty() ? "unknown" : entry.reason.c_str());
                        it = g_deferredHumanoidAssigns.erase(it);
                        continue;
                    }

                    due.push_back(DueAssign{ it->first, entry.reason, entry.attempts });
                    it = g_deferredHumanoidAssigns.erase(it);
                }
            }

            if (due.empty()) {
                return;
            }

            spdlog::info(
                "[TFD][TeammateManager][R96B] deferred humanoid assign flush count={} reason={}",
                static_cast<unsigned int>(due.size()),
                reason ? reason : "unknown");

            for (const auto& item : due) {
                auto* actor = LookupActorById(item.actorId);
                if (!actor || actor->IsDisabled() || actor->IsDead()) {
                    spdlog::warn(
                        "[TFD][TeammateManager][R96B] deferred humanoid assign skipped actor={:08X} reason={} rule=invalid_actor",
                        item.actorId,
                        item.reason.empty() ? "unknown" : item.reason.c_str());
                    continue;
                }

                const char* assignReason = item.reason.empty() ? "deferred_humanoid_assign_flush" : item.reason.c_str();
                const bool queued = QueueHumanoidTeammateAssignEvent(actor, assignReason);
                bool evaluated = false;
                if (queued && actor->Is3DLoaded()) {
                    actor->EvaluatePackage();
                    evaluated = true;
                }

                if (!queued) {
                    const auto nextAttempts = item.attempts + 1;
                    if (nextAttempts < kDeferredHumanoidAssignMaxAttempts) {
                        std::size_t pendingCount = 0;
                        {
                            std::scoped_lock lk(g_deferredHumanoidAssignLock);
                            auto& retry = g_deferredHumanoidAssigns[item.actorId];
                            retry.reason = assignReason;
                            retry.attempts = nextAttempts;
                            retry.dueSec = now + kDeferredHumanoidAssignRetrySeconds;
                            pendingCount = g_deferredHumanoidAssigns.size();
                        }

                        spdlog::warn(
                            "[TFD][TeammateManager][R96B] deferred humanoid assign requeued actor={:08X} reason={} attempts={} pending={} rule=dispatch_failed",
                            item.actorId,
                            assignReason,
                            nextAttempts,
                            static_cast<unsigned int>(pendingCount));
                    }
                    else {
                        spdlog::warn(
                            "[TFD][TeammateManager][R96B] deferred humanoid assign final drop actor={:08X} reason={} attempts={} rule=dispatch_failed_max",
                            item.actorId,
                            assignReason,
                            nextAttempts);
                    }
                }

                spdlog::info(
                    "[TFD][TeammateManager][R96B] deferred humanoid assign dispatched actor={:08X} reason={} queued={} eval={} attempts={}",
                    item.actorId,
                    assignReason,
                    queued ? 1 : 0,
                    evaluated ? 1 : 0,
                    item.attempts);
            }
        }

        void TickUI()

        {
            SyncAliasesImpl();
            FlushDeferredHumanoidTeammateAssignEvents("tick_ui");
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
    constexpr const char* kTeammateGreetStartedEvent = "TFDTeammateGreetStarted";
    constexpr const char* kTeammateExtendContractGoldEvent = "TFDTeammateExtendContractGold";
    constexpr const char* kTeammateExtendContractPleasureEvent = "TFDTeammateExtendContractPleasure";
    constexpr const char* kTeammateRestoreHealthPotionEvent = "TFDTeammateRestoreHealthPotion";
    constexpr const char* kTeammateRestoreHealthPleasureEvent = "TFDTeammateRestoreHealthPleasure";
    constexpr const char* kTeammateTerminateContractEvent = "TFDTeammateTerminateContract";
    constexpr const char* kAfterPleasureChoiceFinishEvent = "TFDAfterPleasureChoiceFinish";
    constexpr const char* kAfterPleasureChoiceRecruitEvent = "TFDAfterPleasureChoiceRecruit";
    constexpr const char* kAfterPleasureChoiceJoinEnemyEvent = "TFDAfterPleasureChoiceJoinEnemy";
    constexpr const char* kAfterPleasureChoiceReleaseEvent = "TFDAfterPleasureChoiceRelease";
    constexpr const char* kAfterPleasureChoiceWorkEvent = "TFDAfterPleasureChoiceWork";
    constexpr const char* kAfterPleasureChoiceKidnapEvent = "TFDAfterPleasureChoiceKidnap";
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
            if (name.empty()) {
                return RE::BSEventNotifyControl::kContinue;
            }

            auto* senderActor = ev->sender ? ev->sender->As<RE::Actor>() : nullptr;

            if (name == kDefeatedHumanoidRecruitEvent) {
                auto* pendingActor = g_runtimeProviders.resolvePendingDefeatedDialogueTarget ? g_runtimeProviders.resolvePendingDefeatedDialogueTarget() : nullptr;
                auto* actor = senderActor ? senderActor : pendingActor;
                if (!actor) {
                    spdlog::warn("[TFD][TeammateManager] defeated humanoid recruit event ignored reason=no_sender_or_pending_target");
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (senderActor && pendingActor && senderActor->GetFormID() != pendingActor->GetFormID()) {
                    spdlog::warn(
                        "[TFD][TeammateManager] defeated humanoid recruit event target mismatch sender={:08X} pending={:08X} action=use_sender",
                        senderActor->GetFormID(),
                        pendingActor->GetFormID());
                }
                const bool ok = TFD::TeammateManager::RecruitDefeatedHumanoidAsTeammate(actor);
                spdlog::info("[TFD][TeammateManager] defeated humanoid recruit event actor={:08X} sender={:08X} pending={:08X} ok={}",
                    actor->GetFormID(),
                    senderActor ? senderActor->GetFormID() : 0u,
                    pendingActor ? pendingActor->GetFormID() : 0u,
                    ok ? 1 : 0);
                return RE::BSEventNotifyControl::kContinue;
            }

            if (name == kAfterPleasureChoiceFinishEvent) {
                const RE::FormID actorId = AliasInternal::ParseFirstFormIDToken(ev->strArg.c_str());
                AliasInternal::NoteAfterPleasureTerminalForContractExpiryUnsafe(actorId, name.data());
                return RE::BSEventNotifyControl::kContinue;
            }

            if (name == kAfterPleasureChoiceRecruitEvent ||
                name == kAfterPleasureChoiceJoinEnemyEvent ||
                name == kAfterPleasureChoiceReleaseEvent ||
                name == kAfterPleasureChoiceWorkEvent ||
                name == kAfterPleasureChoiceKidnapEvent) {
                const RE::FormID actorId = AliasInternal::ParseFirstFormIDToken(ev->strArg.c_str());
                spdlog::info("[TFD][TeammateManager] contract expiry queue hold preserved actor={:08X} reason={} policy=wait_for_that_is_enough",
                    actorId,
                    name.data());
                return RE::BSEventNotifyControl::kContinue;
            }

            if (!senderActor) {
                return RE::BSEventNotifyControl::kContinue;
            }

            if (name == kTeammateGreetStartedEvent) {
                AliasInternal::EnsureContractForActorUnsafe(senderActor, "teammate_greet_started");
                AliasInternal::RefreshRecruitCapacityGlobalsUnsafe("teammate_greet_started");
                (void)TFD::PayModel::PublishSharedGold(senderActor, TFD::PayModel::PayContext::TeammateContract, "teammate_greet_started");
                return RE::BSEventNotifyControl::kContinue;
            }
            if (name == kTeammateExtendContractGoldEvent) {
                const bool ok = AliasInternal::ExtendContractForActorUnsafe(senderActor, "extend_contract_gold");
                spdlog::info("[TFD][TeammateManager] contract extend gold actor={:08X} ok={}", senderActor->GetFormID(), ok ? 1 : 0);
                return RE::BSEventNotifyControl::kContinue;
            }
            if (name == kTeammateExtendContractPleasureEvent) {
                const bool ok = AliasInternal::ExtendContractForActorUnsafe(senderActor, "extend_contract_pleasure");
                spdlog::info("[TFD][TeammateManager] contract extend pleasure actor={:08X} ok={}", senderActor->GetFormID(), ok ? 1 : 0);
                return RE::BSEventNotifyControl::kContinue;
            }
            if (name == kTeammateRestoreHealthPotionEvent) {
                bool ok = false;
                if (AliasInternal::ConsumeHealthPotionFromPlayerUnsafe("teammate_restore_health_potion")) {
                    ok = AliasInternal::RestoreTeammateHealthUnsafe(senderActor, "teammate_restore_health_potion");
                }
                spdlog::info("[TFD][TeammateManager] teammate restore health potion actor={:08X} ok={}", senderActor->GetFormID(), ok ? 1 : 0);
                return RE::BSEventNotifyControl::kContinue;
            }
            if (name == kTeammateRestoreHealthPleasureEvent) {
                const bool ok = AliasInternal::RestoreTeammateHealthUnsafe(senderActor, "teammate_restore_health_pleasure");
                spdlog::info("[TFD][TeammateManager] teammate restore health pleasure actor={:08X} ok={}", senderActor->GetFormID(), ok ? 1 : 0);
                return RE::BSEventNotifyControl::kContinue;
            }
            if (name == kTeammateTerminateContractEvent) {
                const bool ok = AliasInternal::ReleaseHumanoidTeammateContractUnsafe(senderActor, "terminate_contract");
                spdlog::info("[TFD][TeammateManager] teammate terminate contract actor={:08X} ok={}", senderActor->GetFormID(), ok ? 1 : 0);
                return RE::BSEventNotifyControl::kContinue;
            }

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
        return AliasInternal::RegisterOrRefreshAliasForActor(actor, reason, false);
    }

    bool RegisterOrRefreshTeammateNowImmediatePackage(RE::Actor* actor, const char* reason)
    {
        return AliasInternal::RegisterOrRefreshAliasForActor(actor, reason, true);
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

    bool IsPlayerSideTeammateActor(RE::Actor* actor)
    {
        return AliasInternal::IsPlayerSideTeammateAnchor(actor);
    }

    bool IsTFDManagedTeammateActor(RE::Actor* actor)
    {
        return AliasInternal::IsAliasManagedTeammate(actor);
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
        if (!AliasInternal::HasHealthPotionAvailableUnsafe()) {
            AliasInternal::RefreshPotionGlobalUnsafe("defeated_humanoid_recruit_no_potion");
            spdlog::warn(
                "[TFD][TeammateManager] defeated humanoid recruit failed actor={:08X} reason=no_health_potion",
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
        if (!AliasInternal::ConsumeHealthPotionFromPlayerUnsafe("defeated_humanoid_recruit")) {
            AliasInternal::ReleaseHumanoidTeammateContractUnsafe(actor, "defeated_humanoid_recruit_potion_consume_failed");
            spdlog::warn(
                "[TFD][TeammateManager] defeated humanoid recruit failed actor={:08X} reason=potion_consume_failed_after_alias",
                actor->GetFormID());
            return false;
        }
        AliasInternal::EnsureContractForActorUnsafe(actor, "defeated_humanoid_recruit");

        // RegisterOrRefreshTeammateNow() already queues TFDHumanoidTeammateAssign
        // with strArg="defeated_humanoid_recruit".
        // Do not queue a second blank bridge event here.
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
    bool ExtendHumanoidTeammateContract(RE::Actor* actor, const char* reason)
    {
        std::scoped_lock lock(AliasInternal::g_syncLock);
        AliasInternal::ResolveRegistry();
        return AliasInternal::ExtendContractForActorUnsafe(actor, reason ? reason : "native_extend_contract");
    }

    bool RestoreHumanoidTeammateHealthWithPotion(RE::Actor* actor)
    {
        std::scoped_lock lock(AliasInternal::g_syncLock);
        AliasInternal::ResolveRegistry();
        if (!AliasInternal::ConsumeHealthPotionFromPlayerUnsafe("native_restore_teammate_potion")) {
            return false;
        }
        return AliasInternal::RestoreTeammateHealthUnsafe(actor, "native_restore_teammate_potion");
    }

    bool RestoreHumanoidTeammateHealthWithPleasure(RE::Actor* actor)
    {
        std::scoped_lock lock(AliasInternal::g_syncLock);
        AliasInternal::ResolveRegistry();
        return AliasInternal::RestoreTeammateHealthUnsafe(actor, "native_restore_teammate_pleasure");
    }

    bool TerminateHumanoidTeammateContract(RE::Actor* actor)
    {
        std::scoped_lock lock(AliasInternal::g_syncLock);
        AliasInternal::ResolveRegistry();
        return AliasInternal::ReleaseHumanoidTeammateContractUnsafe(actor, "native_terminate_contract");
    }

    void ArmDownedTeammateRecoveryDialogueHold(RE::Actor* actor, double seconds, const char* reason)
    {
        std::scoped_lock lock(AliasInternal::g_syncLock);
        AliasInternal::ArmDownedRecoveryDialogueHoldUnsafe(actor, seconds, reason ? reason : "native_downed_recovery_dialogue");
    }

    bool IsDownedTeammateRecoveryDialogueHoldActor(RE::Actor* actor)
    {
        std::scoped_lock lock(AliasInternal::g_syncLock);
        return AliasInternal::IsDownedRecoveryDialogueHoldActorUnsafe(actor);
    }

    bool IsDownedTeammateRecoveryDialogueHoldActive()
    {
        std::scoped_lock lock(AliasInternal::g_syncLock);
        return AliasInternal::IsDownedRecoveryDialogueHoldActiveUnsafe();
    }

    void ClearDownedTeammateRecoveryDialogueHold(RE::Actor* actor, const char* reason)
    {
        std::scoped_lock lock(AliasInternal::g_syncLock);
        AliasInternal::ClearDownedRecoveryDialogueHoldUnsafe(actor, reason ? reason : "native_clear_downed_recovery_dialogue");
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
