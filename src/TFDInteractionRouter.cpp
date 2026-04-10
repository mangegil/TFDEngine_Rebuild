#include "TFDInteractionRouter.h"
#include "TFDTameBait.h"
#include "TFDInCombatGreet.h"
#include "TFDPreCombatGreet.h"
#include "TFDCaptiveRuntime.h"
#include "TFDRescueRuntime.h"
#include "TFDPleasureRuntime.h"
#include "TFDDefeatMonitor.h"
#include "TFDActorScan.h"
#include "TFDCompanionRestore.h"

#include <spdlog/spdlog.h>

#include <cmath>

namespace TFD::InteractionRouter
{
    namespace
    {
        bool IsPlayerValid(RE::Actor* player)
        {
            if (!player) {
                return false;
            }

            if (!player->IsPlayerRef()) {
                return false;
            }

            if (player->IsDead()) {
                return false;
            }

            return true;
        }

        bool IsTargetValid(RE::Actor* target)
        {
            return TFD::TargetClassifier::IsValidActor(target);
        }

        bool IsTargetInCombat(RE::Actor* target)
        {
            return target && target->IsInCombat();
        }

        RE::Actor* ResolveCurrentCombatTarget(RE::Actor* actor)
        {
            if (!actor) {
                return nullptr;
            }

            auto targetSp = actor->GetActorRuntimeData().currentCombatTarget.get();
            return targetSp.get();
        }

        bool IsPlayerSideActor(RE::Actor* actor, RE::PlayerCharacter* player)
        {
            if (!actor || !player) {
                return false;
            }
            if (actor == player) {
                return true;
            }
            return actor->IsPlayerTeammate() || TFD::Pacify::IsCompanion(actor);
        }

        bool IsActorActivelyTargetingPlayerSide(RE::Actor* actor, RE::PlayerCharacter* player)
        {
            if (!actor || !player) {
                return false;
            }
            auto* combatTarget = ResolveCurrentCombatTarget(actor);
            return IsPlayerSideActor(combatTarget, player);
        }

        bool IsEnemyToPlayer(RE::Actor* player, RE::Actor* target)
        {
            if (!player || !target) {
                return false;
            }

            if (target->IsHostileToActor(player)) {
                return true;
            }

            auto* combatTarget = ResolveCurrentCombatTarget(target);
            if (combatTarget && combatTarget->GetFormID() == player->GetFormID()) {
                return true;
            }

            return false;
        }

        bool IsNonHostileActiveTameFollower(RE::Actor* actor, const TFD::ActorScan::Entry& entry)
        {
            if (!actor) {
                return false;
            }

            if (!TFD::Pacify::IsPacified(actor)) {
                return false;
            }

            if (TFD::Pacify::GetMode(actor) != TFD::Pacify::Mode::Tame) {
                return false;
            }

            const bool inCombat = actor->IsInCombat() || entry.inCombat;
            return !entry.hostile && !inCombat;
        }

        float GetDistance(RE::Actor* a, RE::Actor* b)
        {
            if (!a || !b) {
                return 999999.0f;
            }

            const auto aPos = a->GetPosition();
            const auto bPos = b->GetPosition();
            return aPos.GetDistance(bPos);
        }

        float GetActorFrontDot2D(RE::Actor* actor, RE::PlayerCharacter* player)
        {
            if (!actor || !player) {
                return -1.0f;
            }

            const auto playerPos = player->GetPosition();
            const auto actorPos = actor->GetPosition();

            const float dx = actorPos.x - playerPos.x;
            const float dy = actorPos.y - playerPos.y;
            const float d2 = dx * dx + dy * dy;
            if (d2 <= 1.0f) {
                return 1.0f;
            }

            const float len = std::sqrt(d2);
            const float ang = player->GetAngleZ();
            const float fx = std::sin(ang);
            const float fy = std::cos(ang);
            const float nx = dx / len;
            const float ny = dy / len;
            return nx * fx + ny * fy;
        }

        bool IsActorCloseAndFront(RE::Actor* actor, RE::PlayerCharacter* player, float maxDist)
        {
            if (!actor || !player) {
                return false;
            }

            const auto playerPos = player->GetPosition();
            const auto actorPos = actor->GetPosition();

            const float dx = actorPos.x - playerPos.x;
            const float dy = actorPos.y - playerPos.y;
            const float d2 = dx * dx + dy * dy;
            if (d2 > (maxDist * maxDist)) {
                return false;
            }

            return GetActorFrontDot2D(actor, player) >= 0.20f;
        }

        int InteractionStateForAction(Action action)
        {
            switch (action) {
            case Action::TrucePreCombat:
                return 1;
            case Action::TruceInCombat:
                return 2;
            case Action::Tame:
                return 3;
            case Action::None:
            default:
                return 0;
            }
        }

        float ScoreTruceCandidate(
            RE::Actor* actor,
            RE::PlayerCharacter* player,
            const TFD::ActorScan::Entry& entry,
            Action desiredAction)
        {
            if (!actor || !player) {
                return -1.0e30f;
            }
            if (TFD::DefeatMonitor::IsDefeatedEnemyKnocked(actor)) {
                return -1.0e30f;
            }

            const bool front = IsActorCloseAndFront(actor, player, 1400.0f);
            const bool inCombat = IsActorActivelyTargetingPlayerSide(actor, player);
            const bool weaponDrawn = actor->IsWeaponDrawn();

            const auto classify = TFD::TargetClassifier::ClassifyForHotkey(
                player,
                actor,
                false,
                inCombat,
                entry.dist);

            if (!classify.valid ||
                classify.intent != TFD::TargetClassifier::InteractionIntent::Truce) {
                return -1.0e30f;
            }

            if (desiredAction == Action::TrucePreCombat) {
                if (!inCombat || !front || entry.dist > 1150.0f) {
                    return -1.0e30f;
                }

                float score = 50000.0f;
                score -= entry.dist;
                if (weaponDrawn) {
                    score += 900.0f;
                }
                if (entry.hostile) {
                    score += 350.0f;
                }
                if (classify.allowDialogue) {
                    score += 250.0f;
                }
                return score;
            }

            if (desiredAction == Action::TruceInCombat) {
                if (!inCombat || entry.dist > 1400.0f) {
                    return -1.0e30f;
                }

                float score = 20000.0f;
                score -= entry.dist;
                if (front) {
                    score += 500.0f;
                }
                if (entry.hostile) {
                    score += 150.0f;
                }
                if (classify.allowDialogue) {
                    score += 250.0f;
                }
                return score;
            }

            return -1.0e30f;
        }

        float ScoreTameCandidate(
            RE::Actor* actor,
            RE::PlayerCharacter* player,
            const TFD::ActorScan::Entry& entry)
        {
            if (!actor || !player) {
                return -1.0e30f;
            }
            if (TFD::DefeatMonitor::IsDefeatedEnemyKnocked(actor)) {
                return -1.0e30f;
            }

            if (IsNonHostileActiveTameFollower(actor, entry)) {
                return -1.0e30f;
            }

            const bool front = IsActorCloseAndFront(actor, player, 1400.0f);
            const bool inCombat = actor->IsInCombat() || entry.inCombat;
            const bool weaponDrawn = actor->IsWeaponDrawn();

            const auto classify = TFD::TargetClassifier::ClassifyForHotkey(
                player,
                actor,
                false,
                inCombat,
                entry.dist);

            if (!classify.valid ||
                classify.intent != TFD::TargetClassifier::InteractionIntent::Tame) {
                return -1.0e30f;
            }

            if (entry.dist > 768.0f) {
                return -1.0e30f;
            }

            const bool combatRelevant = entry.hostile || inCombat;
            if (!combatRelevant) {
                return -1.0e30f;
            }

            float score = inCombat ? 32000.0f : 30000.0f;
            score -= entry.dist;
            if (front) {
                score += 300.0f;
            }
            if (entry.hostile) {
                score += 250.0f;
            }
            if (weaponDrawn) {
                score += 350.0f;
            }
            return score;
        }

        FailReason TranslateClassifierReject(TFD::TargetClassifier::RejectReason reason)
        {
            using RejectReason = TFD::TargetClassifier::RejectReason;

            switch (reason) {
            case RejectReason::None:
                return FailReason::None;
            case RejectReason::InvalidActor:
                return FailReason::InvalidTarget;
            case RejectReason::CaptiveOnlyMode:
                return FailReason::NoUsableAction;
            case RejectReason::NotNegotiable:
            case RejectReason::NotCreature:
            case RejectReason::TameRequiresPreCombat:
            case RejectReason::TooFar:
            case RejectReason::UnsafeState:
            default:
                return FailReason::TargetRejected;
            }
        }
    }

    ResolveResult ResolveHotkeyAction(
        RE::Actor* player,
        RE::Actor* target,
        bool isCaptivePhase,
        double nowSec)
    {
        (void)nowSec;

        ResolveResult result{};

        if (!IsPlayerValid(player)) {
            result.valid = false;
            result.failReason = FailReason::InvalidPlayer;
            return result;
        }

        result.playerId = player->GetFormID();

        if (isCaptivePhase) {
            result.valid = false;
            result.failReason = FailReason::NoUsableAction;
            return result;
        }

        if (!IsTargetValid(target)) {
            result.valid = false;
            result.failReason = FailReason::InvalidTarget;
            return result;
        }

        result.targetId = target->GetFormID();

        const bool targetInCombat = IsTargetInCombat(target);
        const float distanceToPlayer = GetDistance(player, target);

        const auto classify = TFD::TargetClassifier::ClassifyForHotkey(
            player,
            target,
            isCaptivePhase,
            targetInCombat,
            distanceToPlayer);
        const bool enemyToPlayer = IsEnemyToPlayer(player, target);

        result.classify = classify;

        spdlog::info(
            "[TFD][Router] classify target={:08X} inCombat={} hostileToPlayer={} dist={:.1f} class={} kind={} intent={} allowDialogue={} valid={} reject={}",
            target->GetFormID(),
            targetInCombat ? 1 : 0,
            enemyToPlayer ? 1 : 0,
            distanceToPlayer,
            TFD::TargetClassifier::ToString(classify.creatureClass),
            TFD::TargetClassifier::ToString(classify.kind),
            TFD::TargetClassifier::ToString(classify.intent),
            classify.allowDialogue ? 1 : 0,
            classify.valid ? 1 : 0,
            TFD::TargetClassifier::ToString(classify.rejectReason));

        if (!classify.valid) {
            result.valid = false;
            result.failReason = TranslateClassifierReject(classify.rejectReason);
            return result;
        }

        if (classify.intent == TFD::TargetClassifier::InteractionIntent::Truce && !enemyToPlayer) {
            spdlog::info(
                "[TFD][Router] reject target={:08X} reason=not_enemy_to_player",
                target->GetFormID());
            result.valid = false;
            result.failReason = FailReason::TargetRejected;
            return result;
        }

        switch (classify.intent) {
        case TFD::TargetClassifier::InteractionIntent::Tame:
            if (!TFD::Pacify::CanStartTame(target)) {
                spdlog::info(
                    "[TFD][Router] reject tame target={:08X} reason=active_tame_requires_feed",
                    target->GetFormID());
                result.valid = false;
                result.failReason = FailReason::TameAlreadyActive;
                return result;
            }

            if (TFD::TameBait::CollectValidBaits(player, target).empty()) {
                spdlog::info(
                    "[TFD][Router] reject tame target={:08X} reason=no_valid_bait",
                    target->GetFormID());
                result.valid = false;
                result.failReason = FailReason::NoValidBait;
                return result;
            }

            result.action = Action::Tame;
            result.valid = true;
            result.failReason = FailReason::None;
            result.shouldBeginSession = true;
            result.shouldOpenDialogue = classify.allowDialogue;
            return result;

        case TFD::TargetClassifier::InteractionIntent::Truce:
            if (!TFD::Pacify::CanStartTruce(target)) {
                result.valid = false;
                result.failReason = FailReason::TruceUnavailable;
                return result;
            }

            result.action = targetInCombat ? Action::TruceInCombat : Action::TrucePreCombat;
            result.valid = true;
            result.failReason = FailReason::None;
            result.shouldBeginSession = true;
            result.shouldOpenDialogue = classify.allowDialogue;
            return result;

        case TFD::TargetClassifier::InteractionIntent::None:
        default:
            result.valid = false;
            result.failReason = FailReason::NoUsableAction;
            return result;
        }
    }

    ExecuteResult ExecuteResolvedAction(
        RE::Actor* player,
        RE::Actor* target,
        const ResolveResult& resolved,
        double nowSec)
    {
        ExecuteResult result{};
        result.action = resolved.action;
        result.failReason = resolved.failReason;
        result.targetId = resolved.targetId;
        result.resolved = resolved.valid;

        if (!resolved.valid) {
            return result;
        }

        if (!player || !target) {
            result.failReason = FailReason::InvalidTarget;
            return result;
        }

        std::optional<RE::FormID> sessionId;

        switch (resolved.action) {
        case Action::Tame:
            sessionId = TFD::Pacify::BeginTameSession(
                player,
                target,
                nowSec,
                resolved.shouldOpenDialogue,
                true);
            break;

        case Action::TrucePreCombat:
            sessionId = TFD::Pacify::BeginTrucePreCombatSession(player, target, nowSec);
            break;

        case Action::TruceInCombat:
            sessionId = TFD::Pacify::BeginTruceInCombatSession(
                player,
                target,
                nowSec,
                resolved.shouldOpenDialogue);
            break;

        case Action::None:
        default:
            result.failReason = FailReason::NoUsableAction;
            return result;
        }

        if (!sessionId.has_value()) {
            result.failReason = FailReason::SessionBeginFailed;
            return result;
        }

        result.sessionId = *sessionId;
        result.executed = true;
        result.dialogueRequested = resolved.shouldOpenDialogue;
        result.failReason = FailReason::None;
        return result;
    }

    ExecuteResult HandleHotkeyPress(
        RE::Actor* player,
        RE::Actor* target,
        bool isCaptivePhase,
        double nowSec)
    {
        const auto resolved = ResolveHotkeyAction(player, target, isCaptivePhase, nowSec);
        if (!resolved.valid) {
            ExecuteResult failed{};
            failed.action = resolved.action;
            failed.failReason = resolved.failReason;
            failed.targetId = resolved.targetId;
            failed.resolved = false;
            failed.executed = false;
            failed.dialogueRequested = false;
            return failed;
        }

        return ExecuteResolvedAction(player, target, resolved, nowSec);
    }



    Action ResolvePreferredTruceAction(const TFD::Flow::Snapshot& snapshot)
    {
        return snapshot.root == TFD::Flow::RootFlow::InCombat ? Action::TruceInCombat : Action::TrucePreCombat;
    }

    bool BeginTruceForAction(RE::Actor* target, Action preferredAction, Action* outAction)
    {
        switch (preferredAction) {
        case Action::TruceInCombat:
            return TFD::InCombatGreet::BeginForActor(target, outAction);
        case Action::TrucePreCombat:
            return TFD::PreCombatGreet::BeginForActor(target, outAction);
        case Action::None:
        case Action::Tame:
        default:
            if (outAction) {
                *outAction = Action::None;
            }
            return false;
        }
    }

    FlowOwnedPrimaryResult HandleFlowOwnedPrimaryHotkey(RE::Actor* player, const TFD::Flow::Snapshot& snapshot)
    {
        FlowOwnedPrimaryResult result{};
        if (!player) {
            return result;
        }

        if (TFD::RescueRuntime::IsActive() || TFD::PleasureRuntime::IsActive()) {
            result.kind = FlowOwnedPrimaryKind::Busy;
            result.handled = true;
            result.success = false;
            result.notification = "TFD: Busy";
            return result;
        }

        if (TFD::CaptiveRuntime::IsEscapeActive()) {
            result.kind = FlowOwnedPrimaryKind::Escape;
            result.interactionState = 6;
            result.handled = true;
            result.success = false;
            result.notification = "TFD: Escape";
            return result;
        }

        if (snapshot.root == TFD::Flow::RootFlow::Bleedout || TFD::Flow::Controller::GetSingleton().IsBleedDecisionActive()) {
            result.kind = FlowOwnedPrimaryKind::Bleedout;
            result.interactionState = 5;
            result.handled = true;
            result.success = TFD::DefeatMonitor::HandleBleedoutHotkey();
            result.notification = result.success ? "TFD: BleedOut Truce" : "TFD: No Response";
            return result;
        }

        if (TFD::CaptiveRuntime::IsStandardCaptiveActive()) {
            result.kind = FlowOwnedPrimaryKind::Captive;
            result.interactionState = 4;
            result.handled = true;
            result.success = TFD::CaptiveRuntime::BeginCaptorCallHotkey(player);
            result.notification = result.success ? "TFD: Calling Captor" : "TFD: No Response";
            return result;
        }

        return result;
    }

    PrimaryHotkeyPickResult PickPrimaryHotkeyTarget(
        RE::PlayerCharacter* player,
        const TFD::Flow::Snapshot& snapshot,
        float radius,
        bool allowTameFallback)
    {
        PrimaryHotkeyPickResult result{};
        if (!player) {
            return result;
        }

        const Action preferredAction = ResolvePreferredTruceAction(snapshot);

        TFD::ActorScan::Rescan(radius, false);

        RE::Actor* bestTarget = nullptr;
        float bestScore = -1.0e30f;

        const auto count = TFD::ActorScan::GetCount();
        for (int i = 0; i < count; ++i) {
            const auto entry = TFD::ActorScan::GetEntry(i);
            auto* actor = TFD::ActorScan::GetActor(i);
            if (!actor || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
                continue;
            }

            const float score = ScoreTruceCandidate(actor, player, entry, preferredAction);
            if (score > bestScore) {
                bestScore = score;
                bestTarget = actor;
            }
        }

        if (bestTarget) {
            result.target = bestTarget;
            result.action = preferredAction;
            result.interactionState = InteractionStateForAction(preferredAction);
            result.valid = true;
            return result;
        }

        if (!allowTameFallback) {
            return result;
        }

        bestTarget = nullptr;
        bestScore = -1.0e30f;

        for (int i = 0; i < count; ++i) {
            const auto entry = TFD::ActorScan::GetEntry(i);
            auto* actor = TFD::ActorScan::GetActor(i);
            if (!actor || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
                continue;
            }

            const float score = ScoreTameCandidate(actor, player, entry);
            if (score > bestScore) {
                bestScore = score;
                bestTarget = actor;
            }
        }

        if (bestTarget) {
            result.target = bestTarget;
            result.action = Action::Tame;
            result.interactionState = InteractionStateForAction(Action::Tame);
            result.valid = true;
        }

        return result;
    }


    PrimaryHotkeyExecuteResult ExecutePrimaryHotkey(
        RE::Actor* player,
        const TFD::Flow::Snapshot& snapshot,
        double nowSec,
        float radius,
        bool allowTameFallback)
    {
        PrimaryHotkeyExecuteResult result{};
        result.handled = true;

        auto* pc = player ? player->As<RE::PlayerCharacter>() : nullptr;
        const auto pick = PickPrimaryHotkeyTarget(pc, snapshot, radius, allowTameFallback);
        result.requestedAction = pick.action;

        if (!pick.valid || !pick.target || pick.action == Action::None) {
            result.failReason = FailReason::InvalidTarget;
            result.notification = "TFD: No Valid Target";
            return result;
        }

        if (pick.action == Action::Tame) {
            const auto exec = HandleHotkeyPress(player, pick.target, false, nowSec);
            result.failReason = exec.failReason;
            result.finalAction = exec.action == Action::None ? Action::Tame : exec.action;

            if (!exec.executed) {
                switch (exec.failReason) {
                case FailReason::TameAlreadyActive:
                    result.notification = "TFD: Already Tamed. Use Shift+H to Feed";
                    break;
                case FailReason::NoValidBait:
                    result.notification = "TFD: No Valid Bait";
                    break;
                case FailReason::SessionBeginFailed:
                    result.notification = "TFD: Pack Tame Failed";
                    break;
                default:
                    result.notification = "TFD: Interaction Failed";
                    break;
                }
                return result;
            }

            result.success = true;
            result.interactionState = InteractionStateForAction(Action::Tame);
            result.notification = (exec.action == Action::Tame) ? "TFD: Tame" : "TFD: Tame Started";
            return result;
        }

        Action startedAction = Action::None;
        const bool started = BeginTruceForAction(pick.target, pick.action, &startedAction);
        if (!started) {
            result.failReason = FailReason::TruceUnavailable;
            result.notification = "TFD: Truce Failed";
            return result;
        }

        if (startedAction == Action::None) {
            startedAction = pick.action;
        }

        result.success = true;
        result.finalAction = startedAction;
        result.interactionState = InteractionStateForAction(startedAction);

        switch (startedAction) {
        case Action::TruceInCombat:
            result.notification = "TFD: InCombat Truce";
            break;
        case Action::TrucePreCombat:
            result.notification = "TFD: PreCombat Truce";
            break;
        case Action::Tame:
            result.notification = "TFD: Tame";
            break;
        case Action::None:
        default:
            result.notification = "TFD: Interaction Started";
            break;
        }

        return result;
    }


    ShiftHotkeyExecuteResult ExecuteShiftHotkey(
        RE::Actor* player,
        double nowSec,
        float radius)
    {
        (void)nowSec;

        ShiftHotkeyExecuteResult result{};

        if (!IsPlayerValid(player)) {
            result.handled = true;
            result.success = false;
            result.notification = "TFD: No Valid Target";
            return result;
        }

        if (auto* defeatedCreature = PickExactDefeatedCreatureTarget(radius)) {
            result.handled = true;
            result.action = ShiftHotkeyAction::RecruitDefeatedCreature;
            result.target = defeatedCreature;
            result.success = TFD::DefeatMonitor::RecruitDefeatedCreatureAsTeammate(defeatedCreature, nowSec);
            result.notification = result.success ?
                "TFD: Defeated Creature Recruited" :
                "TFD: Defeated Recruit Failed";
            return result;
        }

        if (auto* tameTarget = PickExactActiveTameTarget(radius)) {
            result.handled = true;
            result.action = ShiftHotkeyAction::OpenFeedPopup;
            result.target = tameTarget;
            result.interactionState = 3;
            result.success = true;
            return result;
        }

        result.handled = true;
        result.success = false;
        result.notification = "TFD: No Exact Tame Target";
        return result;
    }


    RE::Actor* PickExactDialogueDefeatedTarget(float radius)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return nullptr;
        }

        TFD::ActorScan::Rescan(radius, false);

        RE::Actor* best = nullptr;
        float bestScore = -1.0e30f;

        const auto count = TFD::ActorScan::GetCount();
        for (int i = 0; i < count; ++i) {
            auto entry = TFD::ActorScan::GetEntry(i);
            auto* actor = TFD::ActorScan::GetActor(i);
            if (!actor) {
                continue;
            }
            if (actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
                continue;
            }
            if (entry.dist > radius) {
                continue;
            }
            if (actor->GetParentCell() != player->GetParentCell()) {
                continue;
            }
            if (!TFD::DefeatMonitor::IsDialogueCapableDefeatedEnemy(actor)) {
                continue;
            }

            const float frontDot = GetActorFrontDot2D(actor, player);
            if (frontDot < 0.75f) {
                continue;
            }

            float score = (frontDot * 100000.0f) - entry.dist;
            if (frontDot >= 0.96f) {
                score += 4000.0f;
            } else if (frontDot >= 0.90f) {
                score += 2000.0f;
            }

            if (score > bestScore) {
                bestScore = score;
                best = actor;
            }
        }

        return best;
    }

    RE::Actor* PickExactActiveTameTarget(float radius)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return nullptr;
        }

        auto scoreActor = [&](RE::Actor* actor, const TFD::ActorScan::Entry& entry) -> float {
            if (!actor) {
                return -1.0e30f;
            }
            if (actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
                return -1.0e30f;
            }
            if (!TFD::Pacify::HasActiveTameSession(actor)) {
                return -1.0e30f;
            }
            if (entry.dist > radius) {
                return -1.0e30f;
            }

            const float frontDot = GetActorFrontDot2D(actor, player);
            if (frontDot < 0.80f) {
                return -1.0e30f;
            }

            float score = (frontDot * 100000.0f) - entry.dist;
            if (frontDot >= 0.98f) {
                score += 6000.0f;
            } else if (frontDot >= 0.94f) {
                score += 3500.0f;
            } else if (frontDot >= 0.90f) {
                score += 1500.0f;
            }
            if (actor->IsInCombat() || entry.inCombat) {
                score += 50.0f;
            }
            return score;
        };

        TFD::ActorScan::Rescan(radius, false);

        RE::Actor* best = nullptr;
        float bestScore = -1.0e30f;

        const auto count = TFD::ActorScan::GetCount();
        for (int i = 0; i < count; ++i) {
            auto entry = TFD::ActorScan::GetEntry(i);
            auto* actor = TFD::ActorScan::GetActor(i);
            const float score = scoreActor(actor, entry);
            if (score > bestScore) {
                bestScore = score;
                best = actor;
            }
        }

        if (!best) {
            const auto restored = TFD::CompanionRestore::RestoreNow();
            if (restored > 0) {
                TFD::ActorScan::Rescan(radius, false);

                const auto retryCount = TFD::ActorScan::GetCount();
                for (int i = 0; i < retryCount; ++i) {
                    auto entry = TFD::ActorScan::GetEntry(i);
                    auto* actor = TFD::ActorScan::GetActor(i);
                    const float score = scoreActor(actor, entry);
                    if (score > bestScore) {
                        bestScore = score;
                        best = actor;
                    }
                }
            }
        }

        return best;
    }

    RE::Actor* PickExactDefeatedCreatureTarget(float radius)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return nullptr;
        }

        auto scoreActor = [&](RE::Actor* actor, const TFD::ActorScan::Entry& entry) -> float {
            if (!actor) {
                return -1.0e30f;
            }
            if (actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
                return -1.0e30f;
            }
            if (!TFD::DefeatMonitor::IsCreatureDefeatedEnemy(actor)) {
                return -1.0e30f;
            }
            if (TFD::DefeatMonitor::GetDefeatedEnemyRemainingSeconds(actor) <= 0.0) {
                return -1.0e30f;
            }
            if (entry.dist > radius) {
                return -1.0e30f;
            }

            const float frontDot = GetActorFrontDot2D(actor, player);
            if (frontDot < 0.80f) {
                return -1.0e30f;
            }

            float score = (frontDot * 100000.0f) - entry.dist;
            if (frontDot >= 0.98f) {
                score += 6000.0f;
            } else if (frontDot >= 0.94f) {
                score += 3500.0f;
            } else if (frontDot >= 0.90f) {
                score += 1500.0f;
            }
            return score;
        };

        TFD::ActorScan::Rescan(radius, false);

        RE::Actor* best = nullptr;
        float bestScore = -1.0e30f;

        const auto count = TFD::ActorScan::GetCount();
        for (int i = 0; i < count; ++i) {
            auto entry = TFD::ActorScan::GetEntry(i);
            auto* actor = TFD::ActorScan::GetActor(i);
            const float score = scoreActor(actor, entry);
            if (score > bestScore) {
                bestScore = score;
                best = actor;
            }
        }

        return best;
    }


    const char* ToString(Action value)
    {
        switch (value) {
        case Action::None:
            return "None";
        case Action::Tame:
            return "Tame";
        case Action::TrucePreCombat:
            return "TrucePreCombat";
        case Action::TruceInCombat:
            return "TruceInCombat";
        default:
            return "Unknown";
        }
    }

    const char* ToString(FailReason value)
    {
        switch (value) {
        case FailReason::None:
            return "None";
        case FailReason::InvalidPlayer:
            return "InvalidPlayer";
        case FailReason::InvalidTarget:
            return "InvalidTarget";
        case FailReason::NoUsableAction:
            return "NoUsableAction";
        case FailReason::TargetRejected:
            return "TargetRejected";
        case FailReason::TruceUnavailable:
            return "TruceUnavailable";
        case FailReason::TameAlreadyActive:
            return "TameAlreadyActive";
        case FailReason::NoValidBait:
            return "NoValidBait";
        case FailReason::SessionBeginFailed:
            return "SessionBeginFailed";
        default:
            return "Unknown";
        }
    }
}
