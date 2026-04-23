#include "TFDInteractionRouter.h"
#include "TFDFlowController.h"
#include "TFDHostilityController.h"
#include "TFDTame.h"
#include "TFDInCombatGreet.h"
#include "TFDPreCombatGreet.h"
#include "TFDCaptive.h"
#include "TFDRescue.h"
#include "TFDPleasureRuntime.h"
#include "TFDDefeatMonitor.h"
#include "TFDTeammateManager.h"
#include "TFDActor.h"
#include "TFDBleedout.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cmath>
#include <mutex>
#include <limits>

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
            return TFD::Actor::Interaction::IsValidActor(target);
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
            return actor->IsPlayerTeammate() ||
                TFD::TeammateManager::IsActiveFollowerActor(actor) ||
                TFD::Tame::IsCompanion(actor);
        }

        bool IsActorActivelyTargetingPlayerSide(RE::Actor* actor, RE::PlayerCharacter* player)
        {
            if (!actor || !player) {
                return false;
            }
            auto* combatTarget = ResolveCurrentCombatTarget(actor);
            return IsPlayerSideActor(combatTarget, player);
        }

        void ResolveSnapshotTargetContext(
            const TFD::Actor::Snapshot& snapshot,
            RE::Actor* actor,
            RE::PlayerCharacter* player,
            const TFD::Actor::ActorInfo& info,
            bool& outTargetPlayer,
            bool& outTargetPlayerSide,
            bool& outActorInCombat)
        {
            outTargetPlayer = false;
            outTargetPlayerSide = false;
            outActorInCombat = false;

            if (!actor || !player) {
                return;
            }

            const RE::FormID playerId = player->GetFormID();
            outTargetPlayer = info.currentTargetFormID == playerId;
            outTargetPlayerSide = outTargetPlayer;

            if (!outTargetPlayer && info.currentTargetFormID != 0) {
                if (const auto* targetInfo = TFD::Actor::FindActorInfo(snapshot, info.currentTargetFormID)) {
                    outTargetPlayerSide = targetInfo->playerSide;
                }
                else if (auto targetSp = actor->GetActorRuntimeData().currentCombatTarget.get()) {
                    if (auto* currentTarget = targetSp.get()) {
                        outTargetPlayerSide =
                            currentTarget->IsPlayerRef() ||
                            currentTarget->IsPlayerTeammate() ||
                            TFD::TeammateManager::IsActiveFollowerActor(currentTarget) ||
                            TFD::Tame::IsCompanion(currentTarget);
                    }
                }
            }

            outActorInCombat = actor->IsInCombat() || info.inCombat;
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

        bool IsNonHostileActiveTameFollower(RE::Actor* actor, const TFD::Actor::ActorInfo& info)
        {
            if (!actor) {
                return false;
            }

            if (!TFD::HostilityController::IsSuppressed(actor)) {
                return false;
            }

            if (TFD::HostilityController::GetMode(actor) != TFD::HostilityController::Mode::Tame) {
                return false;
            }

            const bool inCombat = actor->IsInCombat() || info.inCombat;
            return !info.hostileToPlayer && !inCombat;
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

        RE::TESGlobal* g_interactionStateGlobal = nullptr;
        bool g_loggedInteractionStateMissing = false;

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

        void ResolveInteractionStateGlobal()
        {
            if (g_interactionStateGlobal) {
                return;
            }

            g_interactionStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDInteractionState");
            if (!g_interactionStateGlobal && !g_loggedInteractionStateMissing) {
                g_loggedInteractionStateMissing = true;
                spdlog::warn("[TFD][Router] global TFDInteractionState not found");
            }
        }

        float ScoreTruceCandidate(
            const TFD::Actor::Snapshot& snapshot,
            RE::Actor* actor,
            RE::PlayerCharacter* player,
            const TFD::Actor::ActorInfo& info,
            Action desiredAction)
        {
            if (!actor || !player) {
                return -1.0e30f;
            }
            if (IsPlayerSideActor(actor, player)) {
                return -1.0e30f;
            }
            if (TFD::Actor::Ops::IsDefeatedEnemyKnocked(actor)) {
                return -1.0e30f;
            }

            const float dot = GetActorFrontDot2D(actor, player);
            const bool front = dot >= 0.15f;
            const bool weaponDrawn = actor->IsWeaponDrawn();

            bool targetPlayer = false;
            bool targetingPlayerSide = false;
            bool actorInCombat = false;
            ResolveSnapshotTargetContext(snapshot, actor, player, info, targetPlayer, targetingPlayerSide, actorInCombat);

            const bool committedHostile = actorInCombat || targetingPlayerSide;

            const auto classify = TFD::Actor::Interaction::ClassifyTarget(
                player,
                actor,
                false,
                committedHostile,
                info.dist,
                desiredAction == Action::TrucePreCombat
                ? TFD::Actor::Interaction::TruceMode::PreCombat
                : TFD::Actor::Interaction::TruceMode::InCombat);

            if (!classify.valid ||
                classify.intent != TFD::Actor::Interaction::Intent::Truce) {
                return -1.0e30f;
            }

            if (desiredAction == Action::TrucePreCombat) {
                if (!actor->IsHostileToActor(player)) {
                    if (classify.valid && classify.intent == TFD::Actor::Interaction::Intent::Truce) {
                        spdlog::info(
                            "[TFD][Router] reject precombat target={:08X} reason=not_hostile_to_player",
                            actor->GetFormID());
                    }
                    return -1.0e30f;
                }
                if (actorInCombat || targetPlayer || targetingPlayerSide || info.currentTargetFormID != 0) {
                    if (classify.valid && classify.intent == TFD::Actor::Interaction::Intent::Truce) {
                        spdlog::info(
                            "[TFD][Router] reject precombat target={:08X} reason=context combat={} targetPlayer={} targetPlayerSide={} currentTarget={:08X}",
                            actor->GetFormID(),
                            actorInCombat ? 1 : 0,
                            targetPlayer ? 1 : 0,
                            targetingPlayerSide ? 1 : 0,
                            info.currentTargetFormID);
                    }
                    return -1.0e30f;
                }
                if (info.dist > 3500.0f) {
                    return -1.0e30f;
                }

                float score = 12000.0f;
                score -= info.dist;
                if (front) {
                    score += 1200.0f;
                }
                if (classify.allowDialogue) {
                    score += 500.0f;
                }
                if (info.hostileToPlayer) {
                    score += 350.0f;
                }
                if (weaponDrawn) {
                    score += 100.0f;
                }
                score += (dot * 250.0f);
                return score;
            }

            if (desiredAction == Action::TruceInCombat) {
                const bool inCombatContext = actorInCombat && (targetPlayer || targetingPlayerSide || info.currentTargetFormID != 0);
                if (!inCombatContext) {
                    if (classify.valid && classify.intent == TFD::Actor::Interaction::Intent::Truce) {
                        spdlog::info(
                            "[TFD][Router] reject incombat target={:08X} reason=context combat={} targetPlayer={} targetPlayerSide={} currentTarget={:08X}",
                            actor->GetFormID(),
                            actorInCombat ? 1 : 0,
                            targetPlayer ? 1 : 0,
                            targetingPlayerSide ? 1 : 0,
                            info.currentTargetFormID);
                    }
                    return -1.0e30f;
                }
                if (info.dist > 3500.0f) {
                    return -1.0e30f;
                }

                float score = 22000.0f;
                score -= info.dist;
                if (front) {
                    score += 700.0f;
                }
                if (classify.allowDialogue) {
                    score += 500.0f;
                }
                if (targetPlayer) {
                    score += 700.0f;
                }
                if (info.hostileToPlayer) {
                    score += 300.0f;
                }
                if (weaponDrawn) {
                    score += 100.0f;
                }
                score += (dot * 250.0f);
                return score;
            }

            return -1.0e30f;
        }

        float ScoreTameCandidate(
            RE::Actor* actor,
            RE::PlayerCharacter* player,
            const TFD::Actor::ActorInfo& info)
        {
            if (!actor || !player) {
                return -1.0e30f;
            }
            if (TFD::Actor::Ops::IsDefeatedEnemyKnocked(actor)) {
                return -1.0e30f;
            }

            if (IsNonHostileActiveTameFollower(actor, info)) {
                return -1.0e30f;
            }

            const bool front = IsActorCloseAndFront(actor, player, 1400.0f);
            const bool inCombat = actor->IsInCombat() || info.inCombat;
            const bool weaponDrawn = actor->IsWeaponDrawn();

            const auto classify = TFD::Actor::Interaction::ClassifyTarget(
                player,
                actor,
                false,
                inCombat,
                info.dist,
                TFD::Actor::Interaction::TruceMode::Auto);

            if (!classify.valid ||
                classify.intent != TFD::Actor::Interaction::Intent::Tame) {
                return -1.0e30f;
            }

            if (info.dist > 768.0f) {
                return -1.0e30f;
            }

            const bool combatRelevant = info.hostileToPlayer || inCombat;
            if (!combatRelevant) {
                return -1.0e30f;
            }

            float score = inCombat ? 32000.0f : 30000.0f;
            score -= info.dist;
            if (front) {
                score += 300.0f;
            }
            if (info.hostileToPlayer) {
                score += 250.0f;
            }
            if (weaponDrawn) {
                score += 350.0f;
            }
            return score;
        }

        FailReason TranslateClassifierReject(TFD::Actor::Interaction::RejectReason reason)
        {
            using RejectReason = TFD::Actor::Interaction::RejectReason;

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

        ResolveResult MakeResolveFailure(
            FailReason reason,
            RE::FormID playerId = 0,
            RE::FormID targetId = 0,
            Action action = Action::None)
        {
            ResolveResult result{};
            result.action = action;
            result.failReason = reason;
            result.playerId = playerId;
            result.targetId = targetId;
            result.valid = false;
            result.shouldBeginSession = false;
            result.shouldOpenDialogue = false;
            return result;
        }

        Action ResolveIntentAction(TFD::Actor::Interaction::Intent intent, bool targetInCombat)
        {
            switch (intent) {
            case TFD::Actor::Interaction::Intent::Tame:
                return Action::Tame;
            case TFD::Actor::Interaction::Intent::Truce:
                return targetInCombat ? Action::TruceInCombat : Action::TrucePreCombat;
            case TFD::Actor::Interaction::Intent::None:
            default:
                return Action::None;
            }
        }

        ResolveResult ResolveValidatedHotkeyAction(
            RE::Actor* player,
            RE::Actor* target,
            bool isCaptivePhase,
            bool targetInCombat,
            float distanceToPlayer)
        {
            if (isCaptivePhase) {
                return MakeResolveFailure(FailReason::NoUsableAction, player->GetFormID(), target->GetFormID());
            }

            if (IsPlayerSideActor(target, player->As<RE::PlayerCharacter>())) {
                spdlog::info(
                    "[TFD][Router] reject target={:08X} reason=player_side_actor",
                    target->GetFormID());
                return MakeResolveFailure(
                    FailReason::TargetRejected,
                    player->GetFormID(),
                    target->GetFormID());
            }

            const auto classify = TFD::Actor::Interaction::ClassifyTarget(
                player,
                target,
                isCaptivePhase,
                targetInCombat,
                distanceToPlayer,
                targetInCombat
                ? TFD::Actor::Interaction::TruceMode::InCombat
                : TFD::Actor::Interaction::TruceMode::PreCombat);
            const bool enemyToPlayer = IsEnemyToPlayer(player, target);

            spdlog::info(
                "[TFD][Router] classify target={:08X} inCombat={} hostileToPlayer={} dist={:.1f} class={} kind={} intent={} allowDialogue={} valid={} reject={}",
                target->GetFormID(),
                targetInCombat ? 1 : 0,
                enemyToPlayer ? 1 : 0,
                distanceToPlayer,
                TFD::Actor::Interaction::ToString(classify.creatureClass),
                TFD::Actor::Interaction::ToString(classify.kind),
                TFD::Actor::Interaction::ToString(classify.intent),
                classify.allowDialogue ? 1 : 0,
                classify.valid ? 1 : 0,
                TFD::Actor::Interaction::ToString(classify.rejectReason));

            if (!classify.valid) {
                return MakeResolveFailure(
                    TranslateClassifierReject(classify.rejectReason),
                    player->GetFormID(),
                    target->GetFormID());
            }

            if (classify.intent == TFD::Actor::Interaction::Intent::Truce && targetInCombat && !enemyToPlayer) {
                spdlog::info(
                    "[TFD][Router] reject target={:08X} reason=not_enemy_to_player_incombat",
                    target->GetFormID());
                return MakeResolveFailure(
                    FailReason::TargetRejected,
                    player->GetFormID(),
                    target->GetFormID());
            }

            const Action action = ResolveIntentAction(classify.intent, targetInCombat);
            switch (action) {
            case Action::Tame:
                if (!TFD::Tame::CanStart(target)) {
                    spdlog::info(
                        "[TFD][Router] reject tame target={:08X} reason=active_tame_requires_feed",
                        target->GetFormID());
                    return MakeResolveFailure(
                        FailReason::TameAlreadyActive,
                        player->GetFormID(),
                        target->GetFormID(),
                        Action::Tame);
                }

                if (TFD::Tame::CollectValidBaits(player, target).empty()) {
                    spdlog::info(
                        "[TFD][Router] reject tame target={:08X} reason=no_valid_bait",
                        target->GetFormID());
                    return MakeResolveFailure(
                        FailReason::NoValidBait,
                        player->GetFormID(),
                        target->GetFormID(),
                        Action::Tame);
                }
                break;

            case Action::TrucePreCombat:
                if (!target->IsHostileToActor(player)) {
                    spdlog::info(
                        "[TFD][Router] reject precombat target={:08X} reason=resolve_not_hostile_to_player",
                        target->GetFormID());
                    return MakeResolveFailure(
                        FailReason::TargetRejected,
                        player->GetFormID(),
                        target->GetFormID(),
                        action);
                }
                if (!TFD::HostilityController::CanStartTruce(target)) {
                    return MakeResolveFailure(
                        FailReason::TruceUnavailable,
                        player->GetFormID(),
                        target->GetFormID(),
                        action);
                }
                break;

            case Action::TruceInCombat:
                if (!TFD::HostilityController::CanStartTruce(target)) {
                    return MakeResolveFailure(
                        FailReason::TruceUnavailable,
                        player->GetFormID(),
                        target->GetFormID(),
                        action);
                }
                break;

            case Action::None:
            default:
                return MakeResolveFailure(
                    FailReason::NoUsableAction,
                    player->GetFormID(),
                    target->GetFormID());
            }

            ResolveResult result{};
            result.action = action;
            result.failReason = FailReason::None;
            result.playerId = player->GetFormID();
            result.targetId = target->GetFormID();
            result.valid = true;
            result.shouldBeginSession = true;
            result.shouldOpenDialogue = classify.allowDialogue;
            return result;
        }

        std::optional<RE::FormID> TryBeginResolvedAction(
            RE::Actor* player,
            RE::Actor* target,
            const ResolveResult& resolved,
            double nowSec)
        {
            switch (resolved.action) {
            case Action::Tame:
                return TFD::Tame::BeginSession(
                    player,
                    target,
                    nowSec,
                    resolved.shouldOpenDialogue,
                    true);

            case Action::TrucePreCombat:
                return TFD::HostilityController::BeginTrucePreCombatSession(player, target, nowSec);

            case Action::TruceInCombat:
                return TFD::HostilityController::BeginTruceInCombatSession(
                    player,
                    target,
                    nowSec,
                    resolved.shouldOpenDialogue);

            case Action::None:
            default:
                return std::nullopt;
            }
        }

        const char* NotificationForPrimaryFailure(FailReason reason)
        {
            switch (reason) {
            case FailReason::TameAlreadyActive:
                return "TFD: Already Tamed. Use Shift+H to Feed";
            case FailReason::NoValidBait:
                return "TFD: No Valid Bait";
            case FailReason::SessionBeginFailed:
                return "TFD: Pack Tame Failed";
            case FailReason::TruceUnavailable:
                return "TFD: Truce Failed";
            case FailReason::InvalidTarget:
                return "TFD: No Valid Target";
            case FailReason::InvalidPlayer:
            case FailReason::NoUsableAction:
            case FailReason::TargetRejected:
            case FailReason::None:
            default:
                return "TFD: Interaction Failed";
            }
        }

        const char* NotificationForPrimarySuccess(Action action)
        {
            switch (action) {
            case Action::TruceInCombat:
                return "TFD: InCombat Truce";
            case Action::TrucePreCombat:
                return "TFD: PreCombat Truce";
            case Action::Tame:
                return "TFD: Tame";
            case Action::None:
            default:
                return "TFD: Interaction Started";
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

        if (!IsPlayerValid(player)) {
            return MakeResolveFailure(FailReason::InvalidPlayer);
        }

        if (!IsTargetValid(target)) {
            return MakeResolveFailure(FailReason::InvalidTarget, player->GetFormID());
        }

        const bool targetInCombat = IsTargetInCombat(target);
        const float distanceToPlayer = GetDistance(player, target);
        return ResolveValidatedHotkeyAction(
            player,
            target,
            isCaptivePhase,
            targetInCombat,
            distanceToPlayer);
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

        const auto sessionId = TryBeginResolvedAction(player, target, resolved, nowSec);
        if (!sessionId.has_value()) {
            result.failReason = (resolved.action == Action::None)
                ? FailReason::NoUsableAction
                : FailReason::SessionBeginFailed;
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

    Action ResolvePreferredTruceAction(const TFD::FlowController::Snapshot& snapshot)
    {
        return snapshot.root == TFD::FlowController::RootFlow::InCombat ? Action::TruceInCombat : Action::TrucePreCombat;
    }

    Action FallbackTruceAction(Action action)
    {
        switch (action) {
        case Action::TrucePreCombat:
            return Action::TruceInCombat;
        case Action::TruceInCombat:
            return Action::TrucePreCombat;
        default:
            return Action::None;
        }
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

    FlowOwnedPrimaryResult HandleFlowOwnedPrimaryHotkey(RE::Actor* player, const TFD::FlowController::Snapshot& snapshot)
    {
        FlowOwnedPrimaryResult result{};
        if (!player) {
            return result;
        }

        if (TFD::Rescue::IsActive() || TFD::PleasureRuntime::IsActive()) {
            result.kind = FlowOwnedPrimaryKind::Busy;
            result.handled = true;
            result.success = false;
            result.notification = "TFD: Busy";
            return result;
        }

        if (TFD::Captive::IsEscapeActive()) {
            result.kind = FlowOwnedPrimaryKind::Escape;
            result.interactionState = 6;
            result.handled = true;
            result.success = false;
            result.notification = "TFD: Escape";
            return result;
        }

        if (snapshot.root == TFD::FlowController::RootFlow::Bleedout || TFD::FlowController::Controller::GetSingleton().IsBleedDecisionActive()) {
            result.kind = FlowOwnedPrimaryKind::Bleedout;
            result.interactionState = 5;
            result.handled = true;
            result.success = TFD::Bleedout::DefeatGlue::BeginDialogueHotkey();
            result.notification = result.success ? "TFD: BleedOut Truce" : "TFD: No Response";
            return result;
        }

        if (TFD::Captive::IsStandardCaptiveActive()) {
            result.kind = FlowOwnedPrimaryKind::Captive;
            result.interactionState = 4;
            result.handled = true;
            result.success = TFD::Captive::BeginCaptorCallHotkey(player);
            result.notification = result.success ? "TFD: Calling Captor" : "TFD: No Response";
            return result;
        }

        return result;
    }

    PrimaryHotkeyPickResult PickPrimaryHotkeyTarget(
        RE::PlayerCharacter* player,
        const TFD::FlowController::Snapshot& snapshot,
        float radius,
        bool allowTameFallback)
    {
        PrimaryHotkeyPickResult result{};
        if (!player) {
            return result;
        }

        const Action preferredAction = ResolvePreferredTruceAction(snapshot);
        const Action fallbackAction = FallbackTruceAction(preferredAction);
        const auto actorSnapshot = TFD::Actor::BuildSnapshot(radius, false);

        auto tryPickForAction = [&](Action action, RE::Actor*& outTarget, float& outScore) {
            outTarget = nullptr;
            outScore = -1.0e30f;
            for (const auto& info : actorSnapshot.actors) {
                auto* actor = info.get();
                if (!actor || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
                    continue;
                }

                const float score = ScoreTruceCandidate(actorSnapshot, actor, player, info, action);
                if (score > outScore) {
                    outScore = score;
                    outTarget = actor;
                }
            }
            };

        RE::Actor* bestTarget = nullptr;
        float bestScore = -1.0e30f;
        Action chosenAction = Action::None;

        tryPickForAction(preferredAction, bestTarget, bestScore);
        if (bestTarget) {
            chosenAction = preferredAction;
        }
        else if (fallbackAction != Action::None) {
            tryPickForAction(fallbackAction, bestTarget, bestScore);
            if (bestTarget) {
                chosenAction = fallbackAction;
            }
        }

        if (bestTarget) {
            bool targetPlayer = false;
            bool targetPlayerSide = false;
            bool actorInCombat = false;
            std::uint32_t currentTargetFormID = 0;
            if (const auto* bestInfo = TFD::Actor::FindActorInfo(actorSnapshot, bestTarget)) {
                ResolveSnapshotTargetContext(actorSnapshot, bestTarget, player, *bestInfo, targetPlayer, targetPlayerSide, actorInCombat);
                currentTargetFormID = bestInfo->currentTargetFormID;
            }
            else {
                RE::Actor* currentTarget = ResolveCurrentCombatTarget(bestTarget);
                targetPlayer = currentTarget == player;
                targetPlayerSide = IsPlayerSideActor(currentTarget, player);
                actorInCombat = bestTarget->IsInCombat();
                currentTargetFormID = currentTarget ? currentTarget->GetFormID() : 0u;
            }

            spdlog::info(
                "[TFD][Router] primary pick target={:08X} action={} score={:.1f} dist={:.1f} inCombat={} targetPlayer={} targetPlayerSide={} currentTarget={:08X}",
                bestTarget->GetFormID(),
                ToString(chosenAction),
                bestScore,
                GetDistance(player, bestTarget),
                actorInCombat ? 1 : 0,
                targetPlayer ? 1 : 0,
                targetPlayerSide ? 1 : 0,
                currentTargetFormID);
            result.target = bestTarget;
            result.action = chosenAction;
            result.interactionState = InteractionStateForAction(chosenAction);
            result.valid = true;
            return result;
        }

        if (!allowTameFallback) {
            return result;
        }

        bestTarget = nullptr;
        bestScore = -1.0e30f;

        for (const auto& info : actorSnapshot.actors) {
            auto* actor = info.get();
            if (!actor || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
                continue;
            }

            const float score = ScoreTameCandidate(actor, player, info);
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
        const TFD::FlowController::Snapshot& snapshot,
        double nowSec,
        float radius,
        bool allowTameFallback)
    {
        PrimaryHotkeyExecuteResult result{};
        result.handled = true;

        RE::PlayerCharacter* pc = nullptr;
        if (player) {
            if (player->IsPlayerRef()) {
                pc = RE::PlayerCharacter::GetSingleton();
            }
            if (!pc) {
                pc = player->As<RE::PlayerCharacter>();
            }
        }

        if (!pc) {
            spdlog::info("[TFD][Router] primary execute abort reason=player_cast_failed actor={:08X}",
                player ? player->GetFormID() : 0u);
            result.failReason = FailReason::InvalidPlayer;
            result.notification = NotificationForPrimaryFailure(result.failReason);
            return result;
        }

        const auto pick = PickPrimaryHotkeyTarget(pc, snapshot, radius, allowTameFallback);
        result.requestedAction = pick.action;

        if (!pick.valid || !pick.target || pick.action == Action::None) {
            spdlog::info("[TFD][Router] primary execute no valid pick action={} valid={} target={:08X}",
                ToString(pick.action),
                pick.valid ? 1 : 0,
                pick.target ? pick.target->GetFormID() : 0u);
            result.failReason = FailReason::InvalidTarget;
            result.notification = NotificationForPrimaryFailure(result.failReason);
            return result;
        }

        if (pick.action == Action::Tame) {
            const auto exec = HandleHotkeyPress(player, pick.target, false, nowSec);
            result.failReason = exec.failReason;
            result.finalAction = exec.action == Action::None ? Action::Tame : exec.action;

            if (!exec.executed) {
                result.notification = NotificationForPrimaryFailure(exec.failReason);
                return result;
            }

            result.success = true;
            result.interactionState = InteractionStateForAction(Action::Tame);
            result.notification = NotificationForPrimarySuccess(result.finalAction);
            return result;
        }

        Action startedAction = Action::None;
        const bool started = BeginTruceForAction(pick.target, pick.action, &startedAction);
        if (!started) {
            result.failReason = FailReason::TruceUnavailable;
            result.notification = NotificationForPrimaryFailure(result.failReason);
            return result;
        }

        if (startedAction == Action::None) {
            startedAction = pick.action;
        }

        result.success = true;
        result.finalAction = startedAction;
        result.interactionState = InteractionStateForAction(startedAction);
        result.notification = NotificationForPrimarySuccess(startedAction);
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
            result.success = TFD::Tame::RecruitDefeatedCreatureAsTeammate(defeatedCreature, nowSec);
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

    int GetInteractionStateForAction(Action action)
    {
        return InteractionStateForAction(action);
    }

    void SetInteractionStateValue(int value)
    {
        ResolveInteractionStateGlobal();
        if (!g_interactionStateGlobal) {
            return;
        }

        const float desired = static_cast<float>(value);
        if (g_interactionStateGlobal->value != desired) {
            g_interactionStateGlobal->value = desired;
        }
    }

    void SetInteractionStateForAction(Action action)
    {
        SetInteractionStateValue(GetInteractionStateForAction(action));
    }

    void ClearInteractionStateValue()
    {
        SetInteractionStateValue(0);
    }

    int GetInteractionStateValue()
    {
        ResolveInteractionStateGlobal();
        if (!g_interactionStateGlobal) {
            return 0;
        }

        return static_cast<int>(std::lround(g_interactionStateGlobal->value));
    }

    RE::Actor* PickExactDialogueDefeatedTarget(float radius)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return nullptr;
        }

        const auto snapshot = TFD::Actor::BuildSnapshot(radius, false);

        RE::Actor* best = nullptr;
        float bestScore = -1.0e30f;

        for (const auto& info : snapshot.actors) {
            auto* actor = info.get();
            if (!actor) {
                continue;
            }
            if (actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
                continue;
            }
            if (info.dist > radius) {
                continue;
            }
            if (actor->GetParentCell() != player->GetParentCell()) {
                continue;
            }
            if (!TFD::Actor::Ops::IsDialogueCapableDefeatedEnemy(actor)) {
                continue;
            }

            const float frontDot = GetActorFrontDot2D(actor, player);
            if (frontDot < 0.75f) {
                continue;
            }

            float score = (frontDot * 100000.0f) - info.dist;
            if (frontDot >= 0.96f) {
                score += 4000.0f;
            }
            else if (frontDot >= 0.90f) {
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

        auto scoreActor = [&](RE::Actor* actor, const TFD::Actor::ActorInfo& info) -> float {
            if (!actor) {
                return -1.0e30f;
            }
            if (actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
                return -1.0e30f;
            }
            if (!TFD::Tame::HasActiveSession(actor)) {
                return -1.0e30f;
            }
            if (info.dist > radius) {
                return -1.0e30f;
            }

            const float frontDot = GetActorFrontDot2D(actor, player);
            if (frontDot < 0.80f) {
                return -1.0e30f;
            }

            float score = (frontDot * 100000.0f) - info.dist;
            if (frontDot >= 0.98f) {
                score += 6000.0f;
            }
            else if (frontDot >= 0.94f) {
                score += 3500.0f;
            }
            else if (frontDot >= 0.90f) {
                score += 1500.0f;
            }
            if (actor->IsInCombat() || info.inCombat) {
                score += 50.0f;
            }
            return score;
            };

        auto snapshot = TFD::Actor::BuildSnapshot(radius, false);

        RE::Actor* best = nullptr;
        float bestScore = -1.0e30f;

        for (const auto& info : snapshot.actors) {
            auto* actor = info.get();
            const float score = scoreActor(actor, info);
            if (score > bestScore) {
                bestScore = score;
                best = actor;
            }
        }

        if (!best) {
            const auto restored = TFD::TeammateManager::RestoreNow();
            if (restored > 0) {
                snapshot = TFD::Actor::BuildSnapshot(radius, false);
                for (const auto& info : snapshot.actors) {
                    auto* actor = info.get();
                    const float score = scoreActor(actor, info);
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

        auto scoreActor = [&](RE::Actor* actor, const TFD::Actor::ActorInfo& info) -> float {
            if (!actor) {
                return -1.0e30f;
            }
            if (actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
                return -1.0e30f;
            }
            if (!TFD::Actor::Ops::IsCreatureDefeatedEnemy(actor)) {
                return -1.0e30f;
            }
            if (TFD::Actor::Ops::GetDefeatedEnemyRemainingSeconds(actor) <= 0.0) {
                return -1.0e30f;
            }
            if (info.dist > radius) {
                return -1.0e30f;
            }

            const float frontDot = GetActorFrontDot2D(actor, player);
            if (frontDot < 0.80f) {
                return -1.0e30f;
            }

            float score = (frontDot * 100000.0f) - info.dist;
            if (frontDot >= 0.98f) {
                score += 6000.0f;
            }
            else if (frontDot >= 0.94f) {
                score += 3500.0f;
            }
            else if (frontDot >= 0.90f) {
                score += 1500.0f;
            }
            return score;
            };

        const auto snapshot = TFD::Actor::BuildSnapshot(radius, false);

        RE::Actor* best = nullptr;
        float bestScore = -1.0e30f;

        for (const auto& info : snapshot.actors) {
            auto* actor = info.get();
            const float score = scoreActor(actor, info);
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


    namespace DialogueOpen
    {
        namespace
        {
            using Clock = std::chrono::steady_clock;

            constexpr auto kInitialDelay = std::chrono::milliseconds(90);
            constexpr auto kRetryDelay = std::chrono::milliseconds(180);
            constexpr auto kPackageRefreshDelay = std::chrono::milliseconds(350);
            constexpr auto kHardResetDelay = std::chrono::milliseconds(650);
            constexpr auto kDefaultTimeout = std::chrono::milliseconds(2500);
            constexpr auto kInCombatTimeout = std::chrono::milliseconds(12000);
            constexpr auto kPreCombatTimeout = std::chrono::milliseconds(12000);
            constexpr auto kBleedoutTimeout = std::chrono::milliseconds(4000);
            constexpr auto kAfterPleasureTimeout = std::chrono::milliseconds(4500);
            constexpr auto kCommitQuietWindow = std::chrono::milliseconds(1200);
            constexpr auto kPreCombatRangeGateLogInterval = std::chrono::milliseconds(900);
            constexpr auto kPreCombatApproachNudgeInterval = std::chrono::milliseconds(350);
            constexpr float kPreCombatForceGreetMaxDistance = 160.0f;
            constexpr float kPreCombatForceGreetMaxDistanceSq = kPreCombatForceGreetMaxDistance * kPreCombatForceGreetMaxDistance;
            constexpr float kInCombatForceGreetMaxDistance = 420.0f;
            constexpr float kInCombatForceGreetMaxDistanceSq = kInCombatForceGreetMaxDistance * kInCombatForceGreetMaxDistance;

            struct PendingState
            {
                std::mutex lock{};
                RE::ActorHandle speaker{};
                Mode mode = Mode::None;
                bool active = false;
                bool succeeded = false;
                bool requestIssued = false;
                std::uint32_t attempts = 0;
                Clock::time_point started{};
                Clock::time_point nextAttempt{};
                Clock::time_point deadline{};
                Clock::time_point quietUntil{};
                Clock::time_point lastPackageRefresh{};
                Clock::time_point lastHardReset{};
                Clock::time_point lastRangeGateLog{};
                Clock::time_point lastApproachNudge{};
            };

            PendingState g_pending{};
            RE::TESGlobal* g_dialogueStateGlobal = nullptr;
            bool g_loggedDialogueStateMissing = false;

            const char* ModeName(Mode mode)
            {
                switch (mode) {
                case Mode::Bleedout:
                    return "Bleedout";
                case Mode::CaptiveMarker:
                    return "CaptiveMarker";
                case Mode::InCombatTruce:
                    return "InCombatTruce";
                case Mode::PreCombatTruce:
                    return "PreCombatTruce";
                case Mode::AfterPleasure:
                    return "AfterPleasure";
                case Mode::Rescue:
                    return "Rescue";
                default:
                    return "None";
                }
            }

            void ResolveDialogueStateGlobal()
            {
                if (g_dialogueStateGlobal) {
                    return;
                }
                g_dialogueStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDDialogueState");
                if (!g_dialogueStateGlobal && !g_loggedDialogueStateMissing) {
                    g_loggedDialogueStateMissing = true;
                    spdlog::warn("[TFD][DialogueOpen] global TFDDialogueState not found");
                }
            }

            bool IsDialogueOpen()
            {
                auto* ui = RE::UI::GetSingleton();
                return ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
            }

            void SetDialogueStateValue(int value)
            {
                ResolveDialogueStateGlobal();
                if (!g_dialogueStateGlobal) {
                    return;
                }
                const float desired = static_cast<float>(value);
                if (g_dialogueStateGlobal->value != desired) {
                    g_dialogueStateGlobal->value = desired;
                }
            }

            bool PendingCountsAsDialogueStateLocked()
            {
                if (!g_pending.active) {
                    return false;
                }

                switch (g_pending.mode) {
                case Mode::PreCombatTruce:
                    return g_pending.requestIssued;
                default:
                    return true;
                }
            }

            void SyncDialogueStateLocked(bool dialogueOpen)
            {
                SetDialogueStateValue((dialogueOpen || PendingCountsAsDialogueStateLocked()) ? 1 : 0);
            }

            bool CanAttemptOpen(RE::PlayerCharacter* player, RE::Actor* speaker)
            {
                return player && speaker && speaker != player && !speaker->IsDead() && !speaker->IsDisabled();
            }

            float DistanceSquared(RE::Actor* a, RE::Actor* b)
            {
                if (!a || !b) {
                    return std::numeric_limits<float>::max();
                }

                const auto ap = a->GetPosition();
                const auto bp = b->GetPosition();
                const float dx = ap.x - bp.x;
                const float dy = ap.y - bp.y;
                const float dz = ap.z - bp.z;
                return dx * dx + dy * dy + dz * dz;
            }

            bool IsForceGreetRangeReady(RE::PlayerCharacter* player, RE::Actor* speaker, float maxDistanceSq)
            {
                if (!player || !speaker) {
                    return false;
                }
                if (!speaker->Is3DLoaded()) {
                    return false;
                }
                return DistanceSquared(player, speaker) <= maxDistanceSq;
            }

            bool IsPreCombatForceGreetRangeReady(RE::PlayerCharacter* player, RE::Actor* speaker)
            {
                return IsForceGreetRangeReady(player, speaker, kPreCombatForceGreetMaxDistanceSq);
            }

            bool IsInCombatForceGreetRangeReady(RE::PlayerCharacter* player, RE::Actor* speaker)
            {
                return IsForceGreetRangeReady(player, speaker, kInCombatForceGreetMaxDistanceSq);
            }

            std::uint32_t PendingSpeakerFormID()
            {
                auto sp = RE::Actor::LookupByHandle(g_pending.speaker.native_handle());
                auto* actor = sp.get();
                return actor ? actor->GetFormID() : 0u;
            }

            void ResetLocked()
            {
                g_pending.speaker = {};
                g_pending.mode = Mode::None;
                g_pending.active = false;
                g_pending.succeeded = false;
                g_pending.requestIssued = false;
                g_pending.attempts = 0;
                g_pending.started = {};
                g_pending.nextAttempt = {};
                g_pending.deadline = {};
                g_pending.quietUntil = {};
                g_pending.lastPackageRefresh = {};
                g_pending.lastHardReset = {};
                g_pending.lastRangeGateLog = {};
                g_pending.lastApproachNudge = {};
            }

            void CancelLocked(const char* reason)
            {
                if (g_pending.active || g_pending.mode != Mode::None) {
                    spdlog::info(
                        "[TFD][DialogueOpen] cancel mode={} reason={} speaker={:08X} attempts={} requestIssued={} succeeded={}",
                        ModeName(g_pending.mode),
                        reason ? reason : "unknown",
                        PendingSpeakerFormID(),
                        g_pending.attempts,
                        g_pending.requestIssued ? 1 : 0,
                        g_pending.succeeded ? 1 : 0);
                }
                ResetLocked();
                SyncDialogueStateLocked(IsDialogueOpen());
            }

            void PrepareSpeakerForDialogue(RE::PlayerCharacter* player, RE::Actor* speaker, bool hardReset)
            {
                if (!player || !speaker) {
                    return;
                }

                if (!speaker->IsAIEnabled()) {
                    speaker->EnableAI(true);
                }

                speaker->AllowPCDialogue(true);

                if (hardReset) {
                    speaker->SetDialogueWithPlayer(false, false, nullptr);
                }

                speaker->EvaluatePackage(false, true);
                speaker->EvaluatePackage(true, true);
            }

            bool NudgePreCombatApproach(RE::PlayerCharacter* player, RE::Actor* speaker)
            {
                if (!player || !speaker || speaker == player) {
                    return false;
                }

                PrepareSpeakerForDialogue(player, speaker, false);
                return speaker->SetDialogueWithPlayer(true, false, nullptr);
            }

            void BeginCommon(RE::Actor* speaker, Mode mode, const char* reason)
            {
                std::scoped_lock lk(g_pending.lock);
                ResetLocked();

                if (!speaker || speaker->IsDead() || speaker->IsDisabled()) {
                    SyncDialogueStateLocked(IsDialogueOpen());
                    spdlog::warn(
                        "[TFD][DialogueOpen] begin rejected mode={} reason={} speaker={:08X}",
                        ModeName(mode),
                        reason ? reason : "unknown",
                        speaker ? speaker->GetFormID() : 0u);
                    return;
                }

                const auto now = Clock::now();
                const auto timeout = mode == Mode::Bleedout ?
                    kBleedoutTimeout :
                    (mode == Mode::AfterPleasure ?
                        kAfterPleasureTimeout :
                        (mode == Mode::PreCombatTruce ?
                            kPreCombatTimeout :
                            (mode == Mode::InCombatTruce ? kInCombatTimeout : kDefaultTimeout)));
                g_pending.speaker = speaker->GetHandle();
                g_pending.mode = mode;
                g_pending.active = true;
                g_pending.succeeded = false;
                g_pending.requestIssued = false;
                g_pending.attempts = 0;
                g_pending.started = now;
                g_pending.nextAttempt = now + kInitialDelay;
                g_pending.deadline = now + timeout;
                g_pending.quietUntil = {};
                g_pending.lastPackageRefresh = {};
                g_pending.lastHardReset = {};
                g_pending.lastRangeGateLog = {};
                g_pending.lastApproachNudge = {};
                SyncDialogueStateLocked(IsDialogueOpen());

                spdlog::info(
                    "[TFD][DialogueOpen] begin mode={} reason={} speaker={:08X} delayMs={} timeoutMs={}",
                    ModeName(mode),
                    reason ? reason : "unknown",
                    speaker->GetFormID(),
                    static_cast<int>(kInitialDelay.count()),
                    static_cast<int>(timeout.count()));
            }
        }

        void Install()
        {
            std::scoped_lock lk(g_pending.lock);
            ResetLocked();
            ResolveDialogueStateGlobal();
            SyncDialogueStateLocked(IsDialogueOpen());
            spdlog::info("[TFD][DialogueOpen] Install active (native open pending)");
        }

        void BeginBleedout(RE::Actor* speaker)
        {
            BeginCommon(speaker, Mode::Bleedout, "bleedout");
        }

        void BeginCaptiveMarker(RE::Actor* speaker)
        {
            BeginCommon(speaker, Mode::CaptiveMarker, "captive_marker");
        }

        void BeginInCombatTruce(RE::Actor* speaker)
        {
            BeginCommon(speaker, Mode::InCombatTruce, "incombat_truce");
        }

        void BeginPreCombatTruce(RE::Actor* speaker)
        {
            BeginCommon(speaker, Mode::PreCombatTruce, "precombat_truce");
        }

        void BeginAfterPleasure(RE::Actor* speaker)
        {
            BeginCommon(speaker, Mode::AfterPleasure, "after_pleasure");
        }

        void BeginRescue(RE::Actor* speaker)
        {
            BeginCommon(speaker, Mode::Rescue, "rescue");
        }

        void Tick()
        {
            std::scoped_lock lk(g_pending.lock);
            const bool dialogueOpen = IsDialogueOpen();
            SyncDialogueStateLocked(dialogueOpen);
            if (!g_pending.active) {
                return;
            }

            if (dialogueOpen) {
                const auto completedMode = g_pending.mode;
                g_pending.succeeded = true;
                g_pending.active = false;
                g_pending.mode = Mode::None;
                SyncDialogueStateLocked(true);
                spdlog::info(
                    "[TFD][DialogueOpen] success mode={} speaker={:08X} attempts={} requestIssued={}",
                    ModeName(completedMode),
                    PendingSpeakerFormID(),
                    g_pending.attempts,
                    g_pending.requestIssued ? 1 : 0);
                return;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            auto speakerSp = RE::Actor::LookupByHandle(g_pending.speaker.native_handle());
            auto* speaker = speakerSp.get();
            const auto now = Clock::now();

            if (!CanAttemptOpen(player, speaker)) {
                CancelLocked("invalid_target");
                return;
            }

            if (now >= g_pending.deadline) {
                spdlog::warn(
                    "[TFD][DialogueOpen] timeout mode={} speaker={:08X} attempts={} requestIssued={}",
                    ModeName(g_pending.mode),
                    speaker->GetFormID(),
                    g_pending.attempts,
                    g_pending.requestIssued ? 1 : 0);
                ResetLocked();
                SyncDialogueStateLocked(IsDialogueOpen());
                return;
            }

            if (g_pending.requestIssued && g_pending.quietUntil.time_since_epoch().count() != 0 && now < g_pending.quietUntil) {
                return;
            }

            const bool needsPreCombatRangeGate =
                g_pending.mode == Mode::PreCombatTruce &&
                !g_pending.requestIssued &&
                !IsPreCombatForceGreetRangeReady(player, speaker);
            const bool needsInCombatRangeGate =
                g_pending.mode == Mode::InCombatTruce &&
                !g_pending.requestIssued &&
                !IsInCombatForceGreetRangeReady(player, speaker);

            if (needsPreCombatRangeGate || needsInCombatRangeGate) {
                g_pending.nextAttempt = now + kRetryDelay;
                SyncDialogueStateLocked(false);

                if (needsPreCombatRangeGate &&
                    (g_pending.lastApproachNudge.time_since_epoch().count() == 0 ||
                        (now - g_pending.lastApproachNudge) >= kPreCombatApproachNudgeInterval)) {
                    g_pending.lastApproachNudge = now;
                    const bool approachOk = NudgePreCombatApproach(player, speaker);
                    spdlog::info(
                        "[TFD][DialogueOpen] approach nudge mode={} speaker={:08X} ok={} dist={:.1f} targetDist={:.1f}",
                        ModeName(g_pending.mode),
                        speaker->GetFormID(),
                        approachOk ? 1 : 0,
                        std::sqrt(DistanceSquared(player, speaker)),
                        kPreCombatForceGreetMaxDistance);
                }

                if (g_pending.lastRangeGateLog.time_since_epoch().count() == 0 ||
                    (now - g_pending.lastRangeGateLog) >= kPreCombatRangeGateLogInterval) {
                    g_pending.lastRangeGateLog = now;
                    const float dist = std::sqrt(DistanceSquared(player, speaker));
                    const float maxDist = needsInCombatRangeGate ?
                        kInCombatForceGreetMaxDistance :
                        kPreCombatForceGreetMaxDistance;
                    spdlog::info(
                        "[TFD][DialogueOpen] wait range mode={} speaker={:08X} dist={:.1f} max={:.1f}",
                        ModeName(g_pending.mode),
                        speaker->GetFormID(),
                        dist,
                        maxDist);
                }

                return;
            }

            const bool shouldHardReset =
                !g_pending.requestIssued &&
                (g_pending.lastHardReset.time_since_epoch().count() == 0 ||
                    g_pending.attempts == 0 ||
                    (now - g_pending.lastHardReset) >= kHardResetDelay);

            if (shouldHardReset) {
                PrepareSpeakerForDialogue(player, speaker, true);
                g_pending.lastHardReset = now;
                spdlog::info(
                    "[TFD][DialogueOpen] handshake reset mode={} speaker={:08X} attempts={} requestIssued={}",
                    ModeName(g_pending.mode),
                    speaker->GetFormID(),
                    g_pending.attempts,
                    g_pending.requestIssued ? 1 : 0);
            }
            else if (!g_pending.requestIssued &&
                (g_pending.lastPackageRefresh.time_since_epoch().count() == 0 ||
                    (now - g_pending.lastPackageRefresh) >= kPackageRefreshDelay)) {
                PrepareSpeakerForDialogue(player, speaker, false);
                g_pending.lastPackageRefresh = now;
            }

            if (now < g_pending.nextAttempt) {
                return;
            }

            const bool forceGreet =
                g_pending.mode == Mode::PreCombatTruce ||
                g_pending.mode == Mode::InCombatTruce;
            const bool ok = speaker->SetDialogueWithPlayer(true, forceGreet, nullptr);
            ++g_pending.attempts;
            const bool firstIssued = ok && !g_pending.requestIssued;
            g_pending.requestIssued = g_pending.requestIssued || ok;
            if (g_pending.requestIssued) {
                g_pending.quietUntil = now + kCommitQuietWindow;
                g_pending.nextAttempt = g_pending.quietUntil;
            }
            else {
                g_pending.nextAttempt = now + kRetryDelay;
            }
            SyncDialogueStateLocked(IsDialogueOpen());

            spdlog::info(
                "[TFD][DialogueOpen] try mode={} speaker={:08X} attempt={} ok={} requestIssued={}",
                ModeName(g_pending.mode),
                speaker->GetFormID(),
                g_pending.attempts,
                ok ? 1 : 0,
                g_pending.requestIssued ? 1 : 0);

            if (firstIssued) {
                spdlog::info(
                    "[TFD][DialogueOpen] quiet window mode={} speaker={:08X} holdMs={} attempt={}",
                    ModeName(g_pending.mode),
                    speaker->GetFormID(),
                    static_cast<int>(kCommitQuietWindow.count()),
                    g_pending.attempts);
            }
        }

        void Cancel()
        {
            std::scoped_lock lk(g_pending.lock);
            CancelLocked("api_cancel");
        }

        bool IsActive()
        {
            std::scoped_lock lk(g_pending.lock);
            return g_pending.active;
        }

        bool DidSucceed()
        {
            std::scoped_lock lk(g_pending.lock);
            const bool result = g_pending.succeeded;
            g_pending.succeeded = false;
            return result;
        }

        Mode GetMode()
        {
            std::scoped_lock lk(g_pending.lock);
            return g_pending.active ? g_pending.mode : Mode::None;
        }
    }
}
