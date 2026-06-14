#include "TFDVictory.h"

#include "TFDActor.h"
#include "TFDHostilityController.h"
#include "TFDSettings.h"
#include "TFDTeammateManager.h"
#include "TFDTame.h"

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace TFD::Victory
{
    namespace
    {
        RE::TESGlobal* g_stateGlobal = nullptr;
        bool g_logged = false;
        std::chrono::steady_clock::time_point g_observedCombatContextUntil{};
        std::chrono::steady_clock::time_point g_dialogueReadyHoldUntil{};
        RE::FormID g_dialogueReadyHoldActorFormID = 0;

        static constexpr int kStateNeutral = 0;
        static constexpr int kStateNo = 1;
        static constexpr int kStateYes = 2;
        static constexpr int kObservedCombatContextLingerMs = 2500;
        static constexpr float kDefeatedDialogueScanRadius = 512.0f;
        static constexpr float kVictoryObservedScanRadius = 2400.0f;
        static constexpr float kActorLevelDefeatedScanRadius = 4200.0f;

        void ResolveGlobal()
        {
            if (!g_stateGlobal) {
                g_stateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDVictoryState");
                if (g_stateGlobal && !g_logged) {
                    g_logged = true;
                    spdlog::info("[TFD][Victory] TFDVictoryState resolved {:08X}", g_stateGlobal->GetFormID());
                }
            }
        }

        inline std::chrono::steady_clock::time_point Now()
        {
            return std::chrono::steady_clock::now();
        }

        bool HasKeywordByEditorID(RE::Actor* actor, const char* editorID)
        {
            if (!actor || !editorID || !editorID[0]) {
                return false;
            }
            auto* keyword = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(editorID);
            return keyword && actor->HasKeyword(keyword);
        }

        bool IsHumanoidVictoryActor(RE::Actor* actor)
        {
            return HasKeywordByEditorID(actor, "ActorTypeNPC");
        }

        bool IsBasicLivingVictoryActor(RE::Actor* actor, RE::Actor* player)
        {
            if (!actor || !player) {
                return false;
            }
            if (actor == player) {
                return false;
            }
            if (actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
                return false;
            }
            if (actor->GetParentCell() != player->GetParentCell()) {
                return false;
            }
            if (actor->IsPlayerTeammate() || TFD::TeammateManager::IsActiveFollowerActor(actor) || TFD::TeammateManager::IsPlayerSideTeammateActor(actor)) {
                return false;
            }
            if (TFD::Tame::IsCompanion(actor) || TFD::Tame::HasActiveSession(actor)) {
                return false;
            }
            return true;
        }

        bool HasLineOfSightToPlayer(RE::Actor* actor, RE::Actor* player)
        {
            if (!actor || !player) {
                return false;
            }
            bool hasLOSData = false;
            return actor->HasLineOfSight(player, hasLOSData);
        }

        bool IsTargetingPlayer(RE::Actor* actor, RE::Actor* player)
        {
            if (!actor || !player) {
                return false;
            }
            auto targetSp = actor->GetActorRuntimeData().currentCombatTarget.get();
            auto* target = targetSp.get();
            if (!target) {
                return false;
            }
            return target == player || target->GetFormID() == player->GetFormID();
        }


        bool IsRelevantLivingEnemyActor(RE::Actor* actor, RE::Actor* player)
        {
            if (!IsBasicLivingVictoryActor(actor, player)) {
                return false;
            }
            if (TFD::Actor::Ops::IsDialogueCapableDefeatedEnemy(actor)) {
                return true;
            }
            if (TFD::Actor::Ops::IsDefeatedEnemyCandidate(actor)) {
                return true;
            }
            if (actor->IsHostileToActor(player) || actor->IsInCombat()) {
                return true;
            }

            auto targetSp = actor->GetActorRuntimeData().currentCombatTarget.get();
            auto* currentTarget = targetSp.get();
            if (currentTarget == player) {
                return true;
            }
            if (currentTarget && TFD::TeammateManager::IsActiveFollowerActor(currentTarget)) {
                return true;
            }
            return false;
        }

        bool IsUsableDefeatedDialogueActor(RE::Actor* actor, RE::Actor* player)
        {
            if (!IsBasicLivingVictoryActor(actor, player)) {
                return false;
            }
            if (!TFD::Actor::Ops::IsDialogueCapableDefeatedEnemy(actor)) {
                return false;
            }
            if (TFD::Actor::Ops::GetDefeatedEnemyRemainingSeconds(actor) <= 0.0) {
                return false;
            }
            return true;
        }

        bool IsThresholdDefeatedVictoryActor(RE::Actor* actor, RE::Actor* player, bool allowContextFallback)
        {
            if (!IsBasicLivingVictoryActor(actor, player)) {
                return false;
            }
            if (!IsHumanoidVictoryActor(actor)) {
                return false;
            }
            if (!TFD::Actor::IsDownByHealthThreshold(actor, TFD::Settings::GetEnemyDownedThresholdPct())) {
                return false;
            }
            return allowContextFallback || IsRelevantLivingEnemyActor(actor, player);
        }


        bool IsStandingForVictoryBlock(RE::Actor* actor)
        {
            if (!actor || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
                return false;
            }
            if (actor->GetActorValue(RE::ActorValue::kHealth) <= 0.0f) {
                return false;
            }

            // R304A: A TFD-knocked enemy remains alive during its defeated window,
            // but it is no longer a standing combat blocker.  Treat it as an
            // individual defeated candidate, not as proof that the encounter is
            // still unresolved.
            if (TFD::Actor::Ops::IsDefeatedEnemyKnocked(actor)) {
                return false;
            }
            if (TFD::Actor::IsDownByHealthThreshold(actor, TFD::Settings::GetEnemyDownedThresholdPct())) {
                return false;
            }
            return true;
        }

        bool IsDefeatedVictoryCandidate(RE::Actor* actor, RE::Actor* player, bool allowThresholdFallback)
        {
            if (!IsBasicLivingVictoryActor(actor, player)) {
                return false;
            }
            if (TFD::Actor::Ops::IsDialogueCapableDefeatedEnemy(actor)) {
                return true;
            }
            if (TFD::Actor::Ops::IsDefeatedEnemyKnocked(actor)) {
                return true;
            }
            return IsThresholdDefeatedVictoryActor(actor, player, allowThresholdFallback);
        }

        bool SharesDefeatedEncounterFaction(RE::Actor* actor, const std::vector<RE::Actor*>& defeatedActors)
        {
            if (!actor) {
                return false;
            }
            for (auto* defeated : defeatedActors) {
                if (!defeated || defeated == actor) {
                    continue;
                }
                if (TFD::Actor::SharesAllowedFactionExact(actor, defeated)) {
                    return true;
                }
            }
            return false;
        }

        struct CollectiveVictoryEvaluation
        {
            bool hasCandidate{ false };
            bool resolved{ false };
            bool hasStandingBlocker{ false };
            RE::FormID candidateFormID{ 0 };
            RE::FormID blockerFormID{ 0 };
            std::uint32_t activeCoalitionCount{ 0 };
            std::uint32_t standingHostileCoalitionCount{ 0 };
        };

        CollectiveVictoryEvaluation EvaluateCollectiveVictory(
            float radius,
            bool activeVictoryContext,
            RE::Actor* requiredCandidate)
        {
            CollectiveVictoryEvaluation result{};

            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return result;
            }

            const float scanRadius = (std::max)(radius, kActorLevelDefeatedScanRadius);
            TFD::Actor::ScanOptions options{};
            options.radius = scanRadius;
            options.npcOnly = false;
            const auto snapshot = TFD::Actor::BuildSnapshot(player, options);

            const auto requiredFormID = requiredCandidate ? requiredCandidate->GetFormID() : 0;
            std::vector<RE::Actor*> defeatedActors;
            defeatedActors.reserve(snapshot.actors.size() + (requiredCandidate ? 1u : 0u));

            auto addCandidate = [&](RE::Actor* actor) {
                if (!actor || actor == player) {
                    return;
                }
                if (!IsDefeatedVictoryCandidate(actor, player, activeVictoryContext)) {
                    return;
                }
                for (auto* existing : defeatedActors) {
                    if (existing == actor) {
                        return;
                    }
                }
                defeatedActors.push_back(actor);
                result.hasCandidate = true;
                if (result.candidateFormID == 0) {
                    result.candidateFormID = actor->GetFormID();
                }
            };

            if (requiredCandidate) {
                addCandidate(requiredCandidate);
            }

            for (const auto& info : snapshot.actors) {
                auto* actor = info.get();
                if (!actor || info.dist > scanRadius) {
                    continue;
                }
                addCandidate(actor);
            }

            if (requiredFormID != 0) {
                bool foundRequired = false;
                for (auto* defeated : defeatedActors) {
                    if (defeated && defeated->GetFormID() == requiredFormID) {
                        foundRequired = true;
                        break;
                    }
                }
                if (!foundRequired) {
                    result.hasCandidate = false;
                    result.candidateFormID = 0;
                    return result;
                }
                result.candidateFormID = requiredFormID;
            }

            if (!result.hasCandidate) {
                return result;
            }

            auto isDefeatedActorRef = [&](RE::Actor* actor) {
                for (auto* defeated : defeatedActors) {
                    if (defeated == actor) {
                        return true;
                    }
                }
                return false;
            };

            std::vector<std::int32_t> effectiveStandingCoalitions;
            std::vector<std::int32_t> effectiveHostileCoalitions;

            auto pushUniqueCoalition = [](std::vector<std::int32_t>& values, std::int32_t coalitionID) {
                if (coalitionID < 0) {
                    return;
                }
                if (std::find(values.begin(), values.end(), coalitionID) == values.end()) {
                    values.push_back(coalitionID);
                }
            };

            auto markBlocker = [&](RE::Actor* actor) {
                result.hasStandingBlocker = true;
                if (actor && result.blockerFormID == 0) {
                    result.blockerFormID = actor->GetFormID();
                }
            };

            for (const auto& info : snapshot.actors) {
                auto* actor = info.get();
                if (!actor || info.dist > scanRadius) {
                    continue;
                }
                if (!IsBasicLivingVictoryActor(actor, player)) {
                    continue;
                }

                // R304A: actor-level defeated is preserved, but it does not count
                // as a standing coalition member during the 10s Victory window.
                if (isDefeatedActorRef(actor)) {
                    continue;
                }
                if (!IsStandingForVictoryBlock(actor)) {
                    continue;
                }

                const bool standingEncounterCoalition = info.isBattleParticipant && !info.playerSide;
                const bool activeEnemy = IsRelevantLivingEnemyActor(actor, player);
                const bool suppressedByTFD =
                    TFD::HostilityController::IsSuppressed(actor) ||
                    TFD::HostilityController::IsActorTemporarilySuppressed(actor);
                const bool sameDefeatedFaction = SharesDefeatedEncounterFaction(actor, defeatedActors);

                const bool targetingPlayer = IsTargetingPlayer(actor, player);
                const bool hasPlayerLOS = HasLineOfSightToPlayer(actor, player);
                const bool activeThreatSignal = targetingPlayer || hasPlayerLOS;
                const bool enemyIdentity =
                    actor->IsHostileToActor(player) ||
                    actor->IsInCombat() ||
                    activeEnemy ||
                    suppressedByTFD ||
                    sameDefeatedFaction ||
                    standingEncounterCoalition;

                // R306A: Player Victory is blocked by an active threat to the
                // player, not by every standing faction mate in the cell.  A
                // standing enemy that is neither targeting the player nor able
                // to see the player is not an immediate threat and must not
                // erase the 10s defeated-enemy Victory window.
                if (enemyIdentity && activeThreatSignal) {
                    pushUniqueCoalition(effectiveStandingCoalitions, info.coalitionID);
                    if (!info.playerSide) {
                        pushUniqueCoalition(effectiveHostileCoalitions, info.coalitionID);
                    }
                    markBlocker(actor);
                }
            }

            result.activeCoalitionCount = static_cast<std::uint32_t>(effectiveStandingCoalitions.size());
            result.standingHostileCoalitionCount = static_cast<std::uint32_t>(effectiveHostileCoalitions.size());

            result.resolved = result.hasCandidate && !result.hasStandingBlocker;
            return result;
        }

        bool HasActorLevelDefeatedLivingVictoryActor(float radius)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return false;
            }

            const float scanRadius = (std::max)(radius, kActorLevelDefeatedScanRadius);
            const auto snapshot = TFD::Actor::BuildSnapshot(scanRadius, false);

            for (const auto& info : snapshot.actors) {
                auto* actor = info.get();
                if (!actor || info.dist > scanRadius) {
                    continue;
                }
                if (!IsBasicLivingVictoryActor(actor, player)) {
                    continue;
                }
                if (TFD::Actor::Ops::IsDefeatedEnemyKnocked(actor)) {
                    return true;
                }
            }

            return false;
        }

        bool HasThresholdDefeatedVictoryActor(float radius, bool allowContextFallback)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return false;
            }

            const float scanRadius = (std::max)(radius, kVictoryObservedScanRadius);
            const auto snapshot = TFD::Actor::BuildSnapshot(scanRadius, false);

            for (const auto& info : snapshot.actors) {
                auto* actor = info.get();
                if (!actor || info.dist > scanRadius) {
                    continue;
                }
                if (IsThresholdDefeatedVictoryActor(actor, player, allowContextFallback)) {
                    return true;
                }
            }

            return false;
        }

        bool IsDialogueReadyHoldActive()
        {
            const auto now = Now();
            if (g_dialogueReadyHoldActorFormID == 0 || now >= g_dialogueReadyHoldUntil) {
                g_dialogueReadyHoldActorFormID = 0;
                g_dialogueReadyHoldUntil = {};
                return false;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(g_dialogueReadyHoldActorFormID);
            if (!IsUsableDefeatedDialogueActor(actor, player)) {
                spdlog::info(
                    "[TFD][Victory] dialogue ready hold cleared actor={:08X} reason=actor_not_usable",
                    g_dialogueReadyHoldActorFormID);
                g_dialogueReadyHoldActorFormID = 0;
                g_dialogueReadyHoldUntil = {};
                return false;
            }

            return true;
        }

        bool HasRelevantLivingEnemyActor(float radius)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return false;
            }

            const float scanRadius = (std::max)(radius, kVictoryObservedScanRadius);
            const auto snapshot = TFD::Actor::BuildSnapshot(scanRadius, false);

            for (const auto& info : snapshot.actors) {
                auto* actor = info.get();
                if (!actor || info.dist > scanRadius) {
                    continue;
                }
                if (IsRelevantLivingEnemyActor(actor, player)) {
                    return true;
                }
            }

            return false;
        }
    }

    void SetStateValue(int value)
    {
        ResolveGlobal();
        if (!g_stateGlobal) {
            return;
        }

        if (value < kStateNeutral || value > kStateYes) {
            spdlog::warn("[TFD][Victory] rejected invalid state value={}", value);
            return;
        }

        const float desired = static_cast<float>(value);
        if (g_stateGlobal->value != desired) {
            const int previous = static_cast<int>(std::lround(g_stateGlobal->value));
            g_stateGlobal->value = desired;
            spdlog::info("[TFD][Victory] state set previous={} value={}", previous, value);
        }
    }

    int GetStateValue()
    {
        ResolveGlobal();
        return g_stateGlobal ? static_cast<int>(std::lround(g_stateGlobal->value)) : kStateNeutral;
    }

    bool IsActive()
    {
        return GetStateValue() != kStateNeutral;
    }

    void ResetObservedContext()
    {
        g_observedCombatContextUntil = {};
    }

    void ArmDialogueReadyHold(RE::Actor* actor, double seconds, const char* reason)
    {
        if (!actor || seconds <= 0.0) {
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!IsUsableDefeatedDialogueActor(actor, player)) {
            return;
        }

        const auto duration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
        g_dialogueReadyHoldActorFormID = actor->GetFormID();
        g_dialogueReadyHoldUntil = Now() + duration;

        spdlog::info(
            "[TFD][Victory] dialogue ready hold armed actor={:08X} seconds={:.2f} reason={}",
            actor->GetFormID(),
            seconds,
            reason ? reason : "unknown");
    }

    RE::Actor* FindDialogueCapableDefeatedEnemy(float radius)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return nullptr;
        }

        const float scanRadius = (std::max)(radius, kDefeatedDialogueScanRadius);
        const auto snapshot = TFD::Actor::BuildSnapshot(scanRadius, false);

        RE::Actor* best = nullptr;
        float bestDist = scanRadius + 1.0f;

        for (const auto& info : snapshot.actors) {
            auto* actor = info.get();
            if (!IsUsableDefeatedDialogueActor(actor, player)) {
                continue;
            }
            if (info.dist > scanRadius) {
                continue;
            }
            if (info.dist < bestDist) {
                bestDist = info.dist;
                best = actor;
            }
        }

        return best;
    }

    bool HasDialogueCapableDefeatedEnemy(float radius)
    {
        return FindDialogueCapableDefeatedEnemy(radius) != nullptr;
    }

    void MarkDefeatedDialogueAvailable(RE::Actor* actor)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!IsUsableDefeatedDialogueActor(actor, player)) {
            return;
        }
        if (CanAdvertiseVictoryNow(actor, "mark_defeated_dialogue_available")) {
            SetStateValue(kStateYes);
        }
        else {
            SetStateValue(kStateNo);
        }
    }

    bool CanAdvertiseVictoryNow(RE::Actor* requiredCandidate, const char* reason)
    {
        const auto evaluation = EvaluateCollectiveVictory(
            kActorLevelDefeatedScanRadius,
            true,
            requiredCandidate);

        if (reason && !evaluation.resolved) {
            spdlog::info(
                "[TFD][Victory][R306A] blocked Player Victory by active threat reason={} candidate={:08X} required={:08X} blocker={:08X} hasCandidate={} threatBlocker={} activeThreatCoalitions={} hostileThreatCoalitions={}",
                reason,
                evaluation.candidateFormID,
                requiredCandidate ? requiredCandidate->GetFormID() : 0u,
                evaluation.blockerFormID,
                evaluation.hasCandidate ? 1 : 0,
                evaluation.hasStandingBlocker ? 1 : 0,
                evaluation.activeCoalitionCount,
                evaluation.standingHostileCoalitionCount);
        }

        return evaluation.resolved;
    }

    int ComputeObservedState(const ObservedContext& context)
    {
        // 0 = Neutral: no living enemy remains relevant to the player.
        // 1 = No: at least one active threat still targets or sees the player.
        // 2 = Yes: at least one enemy is defeated and no active player threat remains.
        if (!context.hasPlayer) {
            ResetObservedContext();
            return kStateNeutral;
        }

        if (context.playerDown) {
            ResetObservedContext();
            return kStateNeutral;
        }

        const auto now = Now();
        const int previousState = GetStateValue();
        const bool lingerActive =
            g_observedCombatContextUntil != std::chrono::steady_clock::time_point{} &&
            now < g_observedCombatContextUntil;

        if (IsDialogueReadyHoldActive()) {
            const auto heldVictory = EvaluateCollectiveVictory(
                kActorLevelDefeatedScanRadius,
                true,
                RE::TESForm::LookupByID<RE::Actor>(g_dialogueReadyHoldActorFormID));
            return heldVictory.resolved ? kStateYes : kStateNo;
        }

        // R304A: Evaluate defeated candidates before the broad combat-context
        // "hasEnemies" hold.  A TFD-knocked enemy is still alive for the 10s
        // interaction window, so stale combat/enemy flags must not hide a valid
        // last-man-standing Victory.
        const bool activeVictoryContext = context.combatContext || lingerActive || previousState == kStateYes;

        const auto collectiveVictory = EvaluateCollectiveVictory(
            kActorLevelDefeatedScanRadius,
            activeVictoryContext,
            nullptr);
        if (collectiveVictory.resolved) {
            return kStateYes;
        }
        if (collectiveVictory.hasCandidate && collectiveVictory.hasStandingBlocker) {
            if (context.combatContext) {
                g_observedCombatContextUntil = now + std::chrono::milliseconds(kObservedCombatContextLingerMs);
            }
            return kStateNo;
        }

        if (context.hasEnemies && context.combatContext) {
            g_observedCombatContextUntil = now + std::chrono::milliseconds(kObservedCombatContextLingerMs);
            return kStateNo;
        }

        // Standing observed enemies without combat proof are precombat/threat data,
        // not a committed combat-resolution context.  Do not advertise VictoryState=1
        // just because an actor can see or warn the player.

        if (context.combatContext && HasRelevantLivingEnemyActor(kVictoryObservedScanRadius)) {
            g_observedCombatContextUntil = now + std::chrono::milliseconds(kObservedCombatContextLingerMs);
            return kStateNo;
        }

        ResetObservedContext();
        return kStateNeutral;
    }

    void RefreshObservedState(const ObservedContext& context)
    {
        SetStateValue(ComputeObservedState(context));
    }
}
