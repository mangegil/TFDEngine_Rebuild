#include "TFDInteractionRouter.h"
#include "TFDTameBait.h"

#include <spdlog/spdlog.h>

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

        float GetDistance(RE::Actor* a, RE::Actor* b)
        {
            if (!a || !b) {
                return 999999.0f;
            }

            const auto aPos = a->GetPosition();
            const auto bPos = b->GetPosition();
            return aPos.GetDistance(bPos);
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
