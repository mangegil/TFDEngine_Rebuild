#include "TFDFlowController.h"
#include "TFDBleedout.h"
#include "TFDBleedoutGreet.h"
#include "TFDCaptive.h"
#include "TFDCaptiveGreet.h"
#include "TFDHostilityController.h"
#include "TFDInteractionRouter.h"
#include "TFDLocation.h"
#include "TFDInCombat.h"
#include "TFDInCombatGreet.h"
#include "TFDPleasureRuntime.h"
#include "TFDPreCombatGreet.h"
#include "TFDRelease.h"
#include "TFDTransition.h"
#include "TFDTame.h"
#include "TFDActor.h"
#include "TFDDefeatMonitor.h"
#include "TFDVictory.h"
#include "TFDForceGreetState.h"
#include "TFDSettings.h"
#include "TFDTeammateManager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <cstddef>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_set>
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

namespace
{

    static RE::TESGlobal* g_preCombatState = nullptr;
    static RE::TESGlobal* g_inCombatState = nullptr;
    static RE::TESGlobal* g_captiveState = nullptr;
    static RE::TESGlobal* g_pleasureState = nullptr;
    static RE::TESGlobal* g_defeatState = nullptr;
    static RE::TESGlobal* g_dialogueState = nullptr;
    static TFD::FlowController::PassiveRuntimeProviders g_passiveRuntimeProviders{};
    static TFD::FlowController::OutcomeRuntimeProviders g_outcomeRuntimeProviders{};
    static TFD::FlowController::BattleObserverRuntimeProviders g_battleObserverRuntimeProviders{};
    static TFD::FlowController::ContinuousRuntimeProviders g_continuousRuntimeProviders{};
    static bool g_flowRuntimeInstalled = false;
    static TFD::FlowController::ObservedMainStateSnapshot g_lastObservedDiagnostic{};

    static std::unordered_set<RE::FormID> g_inCombatCycleReleaseGraceActors{};

    static bool IsPlayerBleedoutInteractionOwnedForFlow()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (TFD::Bleedout::DefeatGlue::IsPlayerBleedHoldTargetBlocked()) {
            return true;
        }
        auto* state = player ? player->AsActorState() : nullptr;
        return state && state->IsBleedingOut();
    }

    static void MarkInCombatCycleReleaseGrace(RE::FormID actorFormID)
    {
        if (actorFormID != 0) {
            g_inCombatCycleReleaseGraceActors.insert(actorFormID);
        }
    }

    static bool ConsumeInCombatCycleReleaseGrace(RE::FormID actorFormID)
    {
        if (actorFormID == 0) {
            return false;
        }
        const auto erased = g_inCombatCycleReleaseGraceActors.erase(actorFormID);
        return erased > 0;
    }
    static bool g_hasObservedDiagnostic = false;
    static std::chrono::steady_clock::time_point g_lastObservedDiagnosticTick{};
    static std::chrono::steady_clock::time_point g_lastObservedDiagnosticLog{};
    static bool g_hasStaleInCombatNeutralSince = false;
    static std::chrono::steady_clock::time_point g_staleInCombatNeutralSince{};
    static RE::FormID g_staleInCombatNeutralPrimary = 0;
    constexpr auto kObservedDiagnosticTickInterval = std::chrono::milliseconds(500);
    constexpr auto kObservedDiagnosticRepeatInterval = std::chrono::seconds(4);

    constexpr const char* kBleedoutOutcomePayEvent = "TFDBleedoutOutcomePay";
    constexpr const char* kBleedoutOutcomePleasureEvent = "TFDBleedoutOutcomePleasure";
    constexpr const char* kBleedoutOutcomeCaptiveEvent = "TFDBleedoutOutcomeCaptive";
    constexpr const char* kBleedoutOutcomeResetEvent = "TFDBleedoutOutcomeReset";
    constexpr const char* kBleedoutOutcomeReleaseEvent = "TFDBleedoutOutcomeRelease";
    constexpr const char* kBleedoutOutcomeDoNothingEvent = "TFDBleedoutOutcomeDoNothing";
    constexpr const char* kInCombatOutcomePayEvent = "TFDInCombatOutcomePay";
    constexpr const char* kInCombatOutcomePleasureEvent = "TFDInCombatOutcomePleasure";
    constexpr const char* kInCombatOutcomeCaptiveEvent = "TFDInCombatOutcomeCaptive";
    constexpr const char* kInCombatOutcomeFightEvent = "TFDInCombatOutcomeFight";
    constexpr const char* kInCombatOutcomeDoNothingEvent = "TFDInCombatOutcomeDoNothing";
    constexpr const char* kInCombatOutcomeCancelEvent = "TFDInCombatOutcomeCancel";
    constexpr const char* kInCombatOutcomeFailedEvent = "TFDInCombatOutcomeFailed";
    constexpr const char* kInCombatOutcomeResetEvent = "TFDInCombatOutcomeReset";
    constexpr const char* kInCombatOutcomeReleaseEvent = "TFDInCombatOutcomeRelease";
    constexpr const char* kInCombatOutcomeReleaseEndEvent = "TFDInCombatOutcomeReleaseEnd";
    constexpr const char* kInCombatOutcomeFollowEvent = "TFDInCombatOutcomeFollow";
    constexpr const char* kInCombatOutcomeRecruitEvent = "TFDInCombatOutcomeRecruit";
    constexpr const char* kInCombatOutcomeJoinEnemyEvent = "TFDInCombatOutcomeJoinEnemy";
    constexpr const char* kInCombatPleasureRejectCycleOpenEvent = "TFDInCombatPleasureRejectCycleOpen";
    constexpr const char* kPleasureOutcomeReleaseEvent = "TFDPleasureOutcomeRelease";
    constexpr const char* kCaptiveOutcomeWorkEvent = "TFDCaptiveOutcomeWork";
    constexpr const char* kCaptiveOutcomeReturnEvent = "TFDCaptiveOutcomeReturn";
    constexpr const char* kCaptiveOutcomeReturnNoTransitionEvent = "TFDCaptiveOutcomeReturnNoTransition";
    constexpr const char* kCaptiveOutcomeStayInPrisonIdleEvent = "TFDCaptiveOutcomeStayInPrisonIdle";
    constexpr const char* kCaptiveOutcomeReleaseEvent = "TFDCaptiveOutcomeRelease";
    constexpr const char* kCaptiveOutcomeEscapeEvent = "TFDCaptiveOutcomeEscape";
    constexpr const char* kCaptiveOutcomePleasureEvent = "TFDCaptiveOutcomePleasure";
    constexpr const char* kCaptiveOutcomeCancelEvent = "TFDCaptiveOutcomeCancel";
    constexpr const char* kCaptiveWorkRefreshResourcesEvent = "TFDCaptiveWorkRefreshResources";
    constexpr const char* kCaptiveWorkNoJobEvent = "TFDCaptiveWorkNoJob";
    constexpr const char* kCaptiveWorkMiningCompletedEvent = "TFDCaptiveWorkMiningCompleted";
    constexpr const char* kAfterPleasureEnterEvent = "TFDAfterPleasureEnter";
    constexpr const char* kAfterPleasureForceOpenEvent = "TFDAfterPleasureForceOpen";
    constexpr const char* kPleasureFailedEnterEvent = "TFDPleasureFailedEnter";
    constexpr const char* kPleasureFailedAggroEvent = "TFDPleasureFailedAggro";
    constexpr const char* kPleasureFailedStartCombatEvent = "TFDPleasureFailedStartCombat";
    constexpr const char* kCaptivePleasureFailedFightStartCombatEvent = "TFDCaptivePleasureFailedFightStartCombat";
    constexpr const char* kAfterPleasureChoiceReleaseEvent = "TFDAfterPleasureChoiceRelease";
    constexpr const char* kAfterPleasureChoiceRecruitEvent = "TFDAfterPleasureChoiceRecruit";
    constexpr const char* kAfterPleasureChoiceFinishEvent = "TFDAfterPleasureChoiceFinish";
    constexpr const char* kAfterPleasureChoiceJoinEnemyEvent = "TFDAfterPleasureChoiceJoinEnemy";
    constexpr const char* kAfterPleasureChoiceKidnapEvent = "TFDAfterPleasureChoiceKidnap";
    constexpr const char* kAfterPleasureChoiceWorkEvent = "TFDAfterPleasureChoiceWork";
    constexpr const char* kAfterPleasureChoicePleasureEvent = "TFDAfterPleasureChoicePleasure";
    constexpr const char* kPassiveBreakCrimeEvent = "TFDPassiveBreakCrime";
    constexpr const char* kPassiveBreakPickpocketEvent = "TFDPassiveBreakPickpocket";

    static void ResolveGlobal(RE::TESGlobal*& global, const char* editorId)
    {
        if (!global) {
            global = RE::TESForm::LookupByEditorID<RE::TESGlobal>(editorId);
        }
    }

    static void SetGlobalInt(RE::TESGlobal* global, int value)
    {
        if (global) {
            global->value = static_cast<float>(value);
        }
    }

    static void ScrubCaptiveReleasedWorkDialogueGlobals(const char* reason)
    {
        ResolveGlobal(g_preCombatState, "TFDPreCombatState");
        ResolveGlobal(g_inCombatState, "TFDInCombatState");
        ResolveGlobal(g_captiveState, "TFDCaptiveState");
        ResolveGlobal(g_pleasureState, "TFDPleasureState");
        ResolveGlobal(g_defeatState, "TFDDefeatState");
        ResolveGlobal(g_dialogueState, "TFDDialogueState");

        SetGlobalInt(g_preCombatState, 0);
        SetGlobalInt(g_inCombatState, 0);
        SetGlobalInt(g_captiveState, 3);
        SetGlobalInt(g_pleasureState, 0);
        SetGlobalInt(g_defeatState, 0);
        SetGlobalInt(g_dialogueState, 0);

        spdlog::info(
            "[TFD][Flow][R171] captive released work dialogue globals scrubbed reason={} pre=0 in=0 captive=3 pleasure=0 defeat=0 dialogue=0",
            reason ? reason : "released_work_dialogue_scrub");
    }

    void ScrubBleedoutTerminalGlobals(std::string_view reason, bool leftForDead)
    {
        const std::string why = reason.empty() ? std::string("bleedout_terminal") : std::string(reason);

        ResolveGlobal(g_preCombatState, "TFDPreCombatState");
        ResolveGlobal(g_inCombatState, "TFDInCombatState");
        ResolveGlobal(g_defeatState, "TFDDefeatState");
        ResolveGlobal(g_dialogueState, "TFDDialogueState");

        SetGlobalInt(g_preCombatState, 0);
        SetGlobalInt(g_inCombatState, 0);
        SetGlobalInt(g_defeatState, 0);
        SetGlobalInt(g_dialogueState, 0);

        spdlog::info(
            "[TFD][Flow][R216A] bleedout terminal globals scrubbed reason={} leftForDead={} pre=0 in=0 defeat=0 dialogue=0",
            why,
            leftForDead ? 1 : 0);
    }

    void LogFlowSnapshot(const char* op, std::string_view reason, const TFD::FlowController::Snapshot& s, std::uint32_t actorFormID = 0, const char* detail = nullptr)
    {
        spdlog::info(
            "[TFD][Flow] {} detail={} reason={} root={} ctx={} gate={} sub={} captiveMode={} token={} primary={:08X} terminal={} actor={:08X}",
            op ? op : "unknown",
            detail ? detail : "-",
            reason.empty() ? std::string{ "-" } : std::string{ reason },
            TFD::FlowController::Controller::ToString(s.root),
            TFD::FlowController::Controller::ToString(s.contextRoot),
            TFD::FlowController::Controller::ToString(s.gate),
            TFD::FlowController::Controller::ToString(s.sub),
            TFD::FlowController::Controller::ToString(s.captiveMode),
            s.token,
            s.primaryActorFormID,
            s.terminalResolved ? 1 : 0,
            actorFormID);
    }

    void LogFlowReject(const char* op, std::string_view reason, const TFD::FlowController::Snapshot& s)
    {
        spdlog::warn(
            "[TFD][Flow] reject op={} reason={} root={} ctx={} gate={} sub={} captiveMode={} token={} primary={:08X} terminal={}",
            op ? op : "unknown",
            reason.empty() ? std::string{ "-" } : std::string{ reason },
            TFD::FlowController::Controller::ToString(s.root),
            TFD::FlowController::Controller::ToString(s.contextRoot),
            TFD::FlowController::Controller::ToString(s.gate),
            TFD::FlowController::Controller::ToString(s.sub),
            TFD::FlowController::Controller::ToString(s.captiveMode),
            s.token,
            s.primaryActorFormID,
            s.terminalResolved ? 1 : 0);
    }
}


static bool IsBleedoutRuntimeActive()
{
    return g_passiveRuntimeProviders.isBleedoutActive && g_passiveRuntimeProviders.isBleedoutActive();
}

static bool HasReleaseFollowGraceRuntime()
{
    return g_passiveRuntimeProviders.hasReleaseFollowGrace && g_passiveRuntimeProviders.hasReleaseFollowGrace();
}

static int GetGlobalValueInt(RE::TESGlobal* global)
{
    if (!global) {
        return 0;
    }
    return static_cast<int>(std::lround(global->value));
}

static void ResolveObservedDiagnosticGlobals()
{
    ResolveGlobal(g_preCombatState, "TFDPreCombatState");
    ResolveGlobal(g_inCombatState, "TFDInCombatState");
    ResolveGlobal(g_captiveState, "TFDCaptiveState");
    ResolveGlobal(g_pleasureState, "TFDPleasureState");
    ResolveGlobal(g_defeatState, "TFDDefeatState");
    ResolveGlobal(g_dialogueState, "TFDDialogueState");
}

static void AddObservedReasonFlag(std::uint32_t& flags, TFD::FlowController::ObservedReasonFlag flag)
{
    flags |= static_cast<std::uint32_t>(flag);
}

static bool HasObservedReasonFlag(std::uint32_t flags, TFD::FlowController::ObservedReasonFlag flag)
{
    return (flags & static_cast<std::uint32_t>(flag)) != 0;
}

static const char* ObservedStateName(TFD::FlowController::ObservedMainState value)
{
    switch (value) {
    case TFD::FlowController::ObservedMainState::Neutral:
        return "Neutral";
    case TFD::FlowController::ObservedMainState::Precombat:
        return "Precombat";
    case TFD::FlowController::ObservedMainState::Incombat:
        return "Incombat";
    case TFD::FlowController::ObservedMainState::Defeat:
        return "Defeat";
    case TFD::FlowController::ObservedMainState::Captive:
        return "Captive";
    default:
        return "Unknown";
    }
}

static TFD::FlowController::ObservedMainState ProjectMainStateFromRoot(TFD::FlowController::RootFlow root)
{
    using TFD::FlowController::ObservedMainState;
    using TFD::FlowController::RootFlow;

    switch (root) {
    case RootFlow::Captive:
        return ObservedMainState::Captive;
    case RootFlow::Bleedout:
    case RootFlow::LeftForDead:
        return ObservedMainState::Defeat;
    case RootFlow::InCombat:
        return ObservedMainState::Incombat;
    case RootFlow::PreCombat:
        return ObservedMainState::Precombat;
    case RootFlow::Rescue:
    case RootFlow::Recovery:
    case RootFlow::None:
    default:
        return ObservedMainState::Neutral;
    }
}

static TFD::FlowController::RootFlow ProjectObservedExternalRootFlow(const TFD::FlowController::Snapshot& snapshot)
{
    using TFD::FlowController::RootFlow;

    if (snapshot.root != RootFlow::None) {
        return snapshot.root;
    }
    if (!TFD::Transition::IsRecoveryActive()) {
        return RootFlow::None;
    }

    switch (TFD::Transition::GetCurrentFallbackBranch()) {
    case TFD::Transition::FallbackBranch::RescueCached:
        return RootFlow::Rescue;
    case TFD::Transition::FallbackBranch::RecoveryFollower:
    case TFD::Transition::FallbackBranch::RecoveryPotion:
        return RootFlow::Recovery;
    case TFD::Transition::FallbackBranch::LeftForDeadSolo:
    case TFD::Transition::FallbackBranch::LeftForDeadWithFollower:
        return RootFlow::LeftForDead;
    case TFD::Transition::FallbackBranch::None:
    default:
        break;
    }
    return RootFlow::None;
}

static TFD::FlowController::Snapshot ProjectObservedExternalSnapshot(TFD::FlowController::Snapshot snapshot)
{
    using TFD::FlowController::RootFlow;

    const auto projectedRoot = ProjectObservedExternalRootFlow(snapshot);
    if (projectedRoot != RootFlow::None && snapshot.root == RootFlow::None) {
        snapshot.root = projectedRoot;
        if (snapshot.contextRoot == RootFlow::None) {
            snapshot.contextRoot = projectedRoot;
        }
    }
    return snapshot;
}

static TFD::FlowController::ObservedMainState ProjectMainStateFromGlobals(const TFD::FlowController::ObservedMainStateSnapshot& observed)
{
    using TFD::FlowController::ObservedMainState;

    if (observed.captiveGlobal > 0) {
        return ObservedMainState::Captive;
    }
    if (observed.defeatGlobal >= 2) {
        return ObservedMainState::Defeat;
    }
    if (observed.inCombatGlobal > 0) {
        return ObservedMainState::Incombat;
    }
    if (observed.preCombatGlobal > 0) {
        return ObservedMainState::Precombat;
    }
    return ObservedMainState::Neutral;
}

static bool IsObservedPlayerSideActor(RE::Actor* actor, RE::Actor* player)
{
    if (!actor || !player) {
        return false;
    }
    if (actor == player) {
        return true;
    }
    return actor->IsPlayerTeammate() ||
        TFD::TeammateManager::IsActiveFollowerActor(actor) ||
        TFD::TeammateManager::IsTFDManagedTeammateActor(actor) ||
        TFD::Tame::IsCompanion(actor);
}

static bool IsObservedPassiveHandoffGuardedActor(RE::Actor* actor)
{
    if (!actor) {
        return false;
    }

    return TFD::HostilityController::IsPayDialoguePassiveGuardActive(actor) ||
        TFD::HostilityController::IsPayDialoguePersistentTakeoverPending(actor) ||
        TFD::HostilityController::IsActorTemporarilySuppressed(actor) ||
        TFD::Actor::Ops::HasReleaseFollowGrace(actor) ||
        TFD::Actor::Ops::HasTemporaryFollowLock(actor);
}

static bool IsInCombatPayFollowupRoutingPending(const TFD::FlowController::Snapshot& snapshot)
{
    using TFD::FlowController::RootFlow;
    using TFD::FlowController::SubFlow;

    return snapshot.root == RootFlow::InCombat &&
        snapshot.contextRoot == RootFlow::InCombat &&
        snapshot.sub == SubFlow::InCombatPayFollowup &&
        !snapshot.terminalResolved;
}

static bool IsDialogueOwnedProjectionHold(
    const TFD::FlowController::Snapshot& snapshot,
    int dialogueGlobal,
    bool playerBleedRuntimeActive)
{
    using TFD::FlowController::RootFlow;
    using TFD::FlowController::SubFlow;

    if (snapshot.sub == SubFlow::PreCombatPayFollowup ||
        snapshot.sub == SubFlow::InCombatPayFollowup) {
        return true;
    }

    const bool bleedoutOwned =
        snapshot.root == RootFlow::Bleedout ||
        snapshot.contextRoot == RootFlow::Bleedout ||
        snapshot.sub == SubFlow::BleedoutPleasure ||
        snapshot.sub == SubFlow::BleedoutAfterPleasure;
    if (!bleedoutOwned) {
        return false;
    }

    const auto dialogueMode = TFD::InteractionRouter::DialogueOpen::GetMode();
    const bool nativeBleedDialoguePending =
        TFD::InteractionRouter::DialogueOpen::IsActive() &&
        (dialogueMode == TFD::InteractionRouter::DialogueOpen::Mode::Bleedout ||
            dialogueMode == TFD::InteractionRouter::DialogueOpen::Mode::AfterPleasure ||
            dialogueMode == TFD::InteractionRouter::DialogueOpen::Mode::PleasureFailed);

    return playerBleedRuntimeActive ||
        dialogueGlobal > 0 ||
        nativeBleedDialoguePending ||
        TFD::BleedoutGreet::IsActive() ||
        TFD::BleedoutGreet::HasFlowGreetConfirmed() ||
        TFD::BleedoutGreet::HasSeenDialogue() ||
        TFD::BleedoutGreet::HasStickyReopenPending();
}

static bool HasLineOfSightBetween(RE::Actor* from, RE::TESObjectREFR* to)
{
    if (!from || !to) {
        return false;
    }
    bool hasLOSData = false;
    return from->HasLineOfSight(to, hasLOSData);
}

static std::string BuildObservedReasonString(std::uint32_t flags)
{
    struct Entry
    {
        TFD::FlowController::ObservedReasonFlag flag;
        const char* name;
    };

    static constexpr Entry entries[] = {
        { TFD::FlowController::ObservedReasonFlag::CaptiveRuntime, "captive_runtime" },
        { TFD::FlowController::ObservedReasonFlag::CaptiveGlobal, "captive_global" },
        { TFD::FlowController::ObservedReasonFlag::CaptiveRoot, "captive_root" },
        { TFD::FlowController::ObservedReasonFlag::PlayerBleedRuntime, "player_bleed_runtime" },
        { TFD::FlowController::ObservedReasonFlag::DefeatGlobal, "defeat_global" },
        { TFD::FlowController::ObservedReasonFlag::BleedoutRoot, "bleedout_root" },
        { TFD::FlowController::ObservedReasonFlag::ActiveHostile, "active_hostile" },
        { TFD::FlowController::ObservedReasonFlag::DefeatedLivingEnemy, "defeated_living_enemy" },
        { TFD::FlowController::ObservedReasonFlag::MutualLosHostile, "mutual_los_hostile" },
        { TFD::FlowController::ObservedReasonFlag::RootInCombat, "root_incombat" },
        { TFD::FlowController::ObservedReasonFlag::RootPreCombat, "root_precombat" }
    };

    std::ostringstream out;
    bool first = true;
    for (const auto& entry : entries) {
        if (!HasObservedReasonFlag(flags, entry.flag)) {
            continue;
        }
        if (!first) {
            out << '|';
        }
        out << entry.name;
        first = false;
    }
    if (first) {
        return "none";
    }
    return out.str();
}

static TFD::FlowController::ObservedMainStateSnapshot BuildObservedMainStateSnapshotImpl(
    const TFD::FlowController::Snapshot& flowSnapshot,
    bool combatActive,
    RE::Actor* player)
{
    using TFD::FlowController::ObservedMainState;
    using TFD::FlowController::ObservedReasonFlag;
    using TFD::FlowController::RootFlow;

    ResolveObservedDiagnosticGlobals();

    TFD::FlowController::ObservedMainStateSnapshot observed{};
    observed.flow = ProjectObservedExternalSnapshot(flowSnapshot);
    observed.combatActiveFlag = combatActive;
    observed.preCombatGlobal = GetGlobalValueInt(g_preCombatState);
    observed.inCombatGlobal = GetGlobalValueInt(g_inCombatState);
    observed.defeatGlobal = GetGlobalValueInt(g_defeatState);
    observed.captiveGlobal = GetGlobalValueInt(g_captiveState);
    observed.pleasureGlobal = GetGlobalValueInt(g_pleasureState);
    observed.dialogueGlobal = GetGlobalValueInt(g_dialogueState);
    const bool inCombatPayFollowupRoutingPending = IsInCombatPayFollowupRoutingPending(observed.flow);
    observed.rootProjected = inCombatPayFollowupRoutingPending ?
        ObservedMainState::Neutral :
        ProjectMainStateFromRoot(observed.flow.root);

    if (observed.flow.root == RootFlow::Captive) {
        AddObservedReasonFlag(observed.reasonFlags, ObservedReasonFlag::CaptiveRoot);
    }
    if (observed.flow.root == RootFlow::Bleedout || observed.flow.root == RootFlow::LeftForDead) {
        AddObservedReasonFlag(observed.reasonFlags, ObservedReasonFlag::BleedoutRoot);
    }
    if ((observed.flow.root == RootFlow::InCombat || combatActive) &&
        !inCombatPayFollowupRoutingPending) {
        AddObservedReasonFlag(observed.reasonFlags, ObservedReasonFlag::RootInCombat);
    }
    if (observed.flow.root == RootFlow::PreCombat) {
        AddObservedReasonFlag(observed.reasonFlags, ObservedReasonFlag::RootPreCombat);
    }

    observed.captiveRuntimeActive = TFD::Captive::IsActive();
    if (observed.captiveRuntimeActive) {
        AddObservedReasonFlag(observed.reasonFlags, ObservedReasonFlag::CaptiveRuntime);
    }
    if (observed.captiveGlobal > 0) {
        AddObservedReasonFlag(observed.reasonFlags, ObservedReasonFlag::CaptiveGlobal);
    }

    observed.playerBleedRuntimeActive = IsBleedoutRuntimeActive();
    if (observed.playerBleedRuntimeActive) {
        AddObservedReasonFlag(observed.reasonFlags, ObservedReasonFlag::PlayerBleedRuntime);
    }
    if (observed.defeatGlobal >= 2) {
        AddObservedReasonFlag(observed.reasonFlags, ObservedReasonFlag::DefeatGlobal);
    }

    if (player) {
        observed.playerInCombat = player->IsInCombat();
    }

    const bool dialogueOwnedProjectionHold = IsDialogueOwnedProjectionHold(
        observed.flow,
        observed.dialogueGlobal,
        observed.playerBleedRuntimeActive);

    if (player && player->GetParentCell()) {
        TFD::Actor::ScanOptions options{};
        options.radius = (std::max)(4200.0f, TFD::Settings::GetScanRadius());
        options.npcOnly = false;

        const auto world = TFD::Actor::BuildSnapshot(player, options);
        observed.scannedActorCount = static_cast<std::uint32_t>(world.actors.size());
        const auto playerFormID = player->GetFormID();

        for (const auto& info : world.actors) {
            auto* actor = info.get();
            if (!actor || actor == player || actor->IsDisabled() || actor->IsDead()) {
                continue;
            }

            if (info.standing && info.playerSide) {
                ++observed.playerSideStandingCount;
            }

            if (IsObservedPlayerSideActor(actor, player)) {
                continue;
            }

            // P7: while a dialogue-owned transaction is active, the root owner already
            // controls the passive state.  Do not let ambient/crowd hostility re-project
            // observed state to InCombat between Pay/Bleedout outcome commits.
            if (dialogueOwnedProjectionHold) {
                continue;
            }

            if (IsObservedPassiveHandoffGuardedActor(actor)) {
                continue;
            }

            const bool defeatedLivingEnemy = TFD::Actor::Ops::IsDefeatedEnemyKnocked(actor);
            if (defeatedLivingEnemy) {
                ++observed.defeatedLivingEnemyCount;
                if (!observed.reasonActorFormID) {
                    observed.reasonActorFormID = info.formID;
                }
                AddObservedReasonFlag(observed.reasonFlags, ObservedReasonFlag::DefeatedLivingEnemy);
                continue;
            }

            if (!info.standing) {
                continue;
            }

            auto* target = info.getCurrentTarget();
            const bool targetsPlayerSide =
                info.currentTargetFormID == playerFormID ||
                IsObservedPlayerSideActor(target, player);
            const bool hostileToPlayer = info.hostileToPlayer || actor->IsHostileToActor(player);
            const bool playerHasLosToActor = HasLineOfSightBetween(player, actor);
            const bool actorHasLosToPlayer = HasLineOfSightBetween(actor, player);
            const bool mutualLos = playerHasLosToActor && actorHasLosToPlayer;

            // P8: InCombat projection must be target-driven, not raw hostile-flag driven.
            // Keep the broad scan radius intact, but only promote an actor to activeHostile
            // when its combat target is the player or a player-side actor. Actors with
            // currentTarget=00000000 can still count as mutual-LOS precombat pressure, but
            // they must not reopen/flip InCombat truce state.
            if (targetsPlayerSide) {
                ++observed.activeHostileCount;
                if (!observed.reasonActorFormID) {
                    observed.reasonActorFormID = info.formID;
                    observed.reasonTargetFormID = info.currentTargetFormID;
                }
                AddObservedReasonFlag(observed.reasonFlags, ObservedReasonFlag::ActiveHostile);
            }
            else if (hostileToPlayer && mutualLos) {
                ++observed.mutualLosHostileCount;
                if (!observed.reasonActorFormID) {
                    observed.reasonActorFormID = info.formID;
                }
                AddObservedReasonFlag(observed.reasonFlags, ObservedReasonFlag::MutualLosHostile);
            }
        }
    }

    // R93B: observed is now the FlowController-owned read-only world state.
    // It is still separate from session root/globals so we can consolidate safely
    // without changing gameplay behavior in this patch.
    // P26OWN: InCombatPayFollowup is a routing-pending state, not active combat.
    // Keep root/context alive so linked Pay outcomes can still arrive, but do not
    // project this private routing root as public InCombat while the actor is
    // already pacified and there is no hostile pressure.
    if (inCombatPayFollowupRoutingPending &&
        observed.activeHostileCount == 0 &&
        observed.mutualLosHostileCount == 0 &&
        observed.defeatedLivingEnemyCount == 0 &&
        !observed.captiveRuntimeActive &&
        !observed.playerBleedRuntimeActive) {
        observed.observed = ObservedMainState::Neutral;
    }
    else if (dialogueOwnedProjectionHold && observed.rootProjected != ObservedMainState::Neutral) {
        observed.observed = observed.rootProjected;
    }
    else if (observed.captiveRuntimeActive) {
        observed.observed = ObservedMainState::Captive;
    }
    else if (observed.playerBleedRuntimeActive) {
        observed.observed = ObservedMainState::Defeat;
    }
    else if (observed.activeHostileCount > 0) {
        observed.observed = ObservedMainState::Incombat;
    }
    else if (observed.defeatedLivingEnemyCount > 0) {
        observed.observed = ObservedMainState::Neutral;
    }
    else if (observed.mutualLosHostileCount > 0) {
        observed.observed = ObservedMainState::Precombat;
    }
    else {
        observed.observed = ObservedMainState::Neutral;
    }

    observed.globalsProjected = ProjectMainStateFromGlobals(observed);
    return observed;
}

static bool ShouldLogObservedMainStateDiagnostic(const TFD::FlowController::ObservedMainStateSnapshot& observed, std::chrono::steady_clock::time_point now)
{
    if (!g_hasObservedDiagnostic) {
        return true;
    }
    if (observed.observed != g_lastObservedDiagnostic.observed ||
        observed.rootProjected != g_lastObservedDiagnostic.rootProjected ||
        observed.globalsProjected != g_lastObservedDiagnostic.globalsProjected ||
        observed.reasonFlags != g_lastObservedDiagnostic.reasonFlags) {
        return true;
    }
    if (observed.observed != observed.rootProjected || observed.observed != observed.globalsProjected) {
        return (now - g_lastObservedDiagnosticLog) >= kObservedDiagnosticRepeatInterval;
    }
    return false;
}

static void LogObservedMainStateDiagnostic(const TFD::FlowController::ObservedMainStateSnapshot& observed, std::string_view reason)
{
    const auto reasonText = BuildObservedReasonString(observed.reasonFlags);
    const bool mismatchRoot = observed.observed != observed.rootProjected;
    const bool mismatchGlobals = observed.observed != observed.globalsProjected;

    spdlog::info(
        "[TFD][Flow][R93B] observed={} rootProjected={} globalsProjected={} mismatchRoot={} mismatchGlobals={} reason={} flags={} root={} ctx={} gate={} sub={} captiveMode={} token={} primary={:08X} combatActive={} globals(pre={} in={} defeat={} captive={} pleasure={} dialogue={}) counts(scanned={} activeHostile={} mutualLos={} defeatedLiving={} playerSide={}) actor={:08X} target={:08X} tickReason={}",
        ObservedStateName(observed.observed),
        ObservedStateName(observed.rootProjected),
        ObservedStateName(observed.globalsProjected),
        mismatchRoot ? 1 : 0,
        mismatchGlobals ? 1 : 0,
        reasonText,
        observed.reasonFlags,
        TFD::FlowController::Controller::ToString(observed.flow.root),
        TFD::FlowController::Controller::ToString(observed.flow.contextRoot),
        TFD::FlowController::Controller::ToString(observed.flow.gate),
        TFD::FlowController::Controller::ToString(observed.flow.sub),
        TFD::FlowController::Controller::ToString(observed.flow.captiveMode),
        observed.flow.token,
        observed.flow.primaryActorFormID,
        observed.combatActiveFlag ? 1 : 0,
        observed.preCombatGlobal,
        observed.inCombatGlobal,
        observed.defeatGlobal,
        observed.captiveGlobal,
        observed.pleasureGlobal,
        observed.dialogueGlobal,
        observed.scannedActorCount,
        observed.activeHostileCount,
        observed.mutualLosHostileCount,
        observed.defeatedLivingEnemyCount,
        observed.playerSideStandingCount,
        observed.reasonActorFormID,
        observed.reasonTargetFormID,
        reason.empty() ? std::string{ "-" } : std::string{ reason });
}


static bool IsObservedPrimaryInvalidOrDead(const TFD::FlowController::ObservedMainStateSnapshot& observed)
{
    if (observed.flow.primaryActorFormID == 0) {
        return true;
    }

    auto* actor = RE::TESForm::LookupByID<RE::Actor>(observed.flow.primaryActorFormID);
    return !actor || actor->IsDead() || actor->IsDisabled();
}

static bool AutoCompleteStaleInCombatNeutralIfNeeded(
    const TFD::FlowController::ObservedMainStateSnapshot& observed,
    std::chrono::steady_clock::time_point now,
    std::string_view tickReason)
{
    using TFD::FlowController::ObservedMainState;
    using TFD::FlowController::RootFlow;
    using TFD::FlowController::SubFlow;

    const bool inCombatRoot =
        observed.flow.root == RootFlow::InCombat &&
        observed.flow.contextRoot == RootFlow::InCombat &&
        !observed.flow.terminalResolved;
    const bool noWorldPressure =
        observed.activeHostileCount == 0 &&
        observed.mutualLosHostileCount == 0 &&
        observed.defeatedLivingEnemyCount == 0 &&
        !observed.captiveRuntimeActive &&
        !observed.playerBleedRuntimeActive;
    const bool neutralStaleRoot =
        observed.flow.sub == SubFlow::None &&
        observed.observed == ObservedMainState::Neutral;
    const bool abandonedPayFollowup =
        observed.flow.sub == SubFlow::InCombatPayFollowup &&
        noWorldPressure &&
        (observed.flow.primaryActorFormID == 0 || observed.reasonActorFormID == 0);

    // P20OWN: Patch 19 cleaned the InCombatPayFollowup orphan, but a plain
    // InCombat/Truce root can also survive with no hostile, no dialogue, and no
    // reason actor.  In that state the old gate=Truce flag must not protect the
    // root forever; otherwise it only clears later through PlayerArmed, which
    // rehostiles the actor and reopens the encounter.
    const bool orphanTruceRoot =
        neutralStaleRoot &&
        observed.flow.gate == TFD::FlowController::DecisionGate::Truce &&
        observed.dialogueGlobal <= 0 &&
        observed.reasonActorFormID == 0 &&
        observed.activeHostileCount == 0 &&
        observed.mutualLosHostileCount == 0;

    const bool candidate =
        inCombatRoot &&
        noWorldPressure &&
        (neutralStaleRoot || abandonedPayFollowup || orphanTruceRoot);

    if (!candidate) {
        g_hasStaleInCombatNeutralSince = false;
        g_staleInCombatNeutralPrimary = 0;
        return false;
    }

    const auto primary = observed.flow.primaryActorFormID;
    bool primarySuppressed = false;
    bool primaryCanOpenTruceDialogue = false;
    bool primaryPayGuardActive = false;
    if (primary != 0) {
        if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(primary)) {
            primarySuppressed = TFD::HostilityController::IsSuppressed(actor);
            primaryCanOpenTruceDialogue = TFD::HostilityController::CanOpenDialogue(actor);
            primaryPayGuardActive = TFD::HostilityController::IsPayDialoguePassiveGuardActive(actor);
        }
    }

    const bool activeDialogueProtection = observed.dialogueGlobal > 0;

    // R465A: Bleedout -> AfterPleasure Recruit can intentionally bridge into an
    // InCombat pleasure-cycle ForceGreet.  During the one-frame/short Papyrus
    // handoff after the menu closes, the world may look neutral:
    // dialogueGlobal == 0, no hostile pressure, gate=Truce, reasonActor=0.
    // The old orphan-truce cleanup could release the active TruceInCombat
    // session before Papyrus delivered the explicit InCombat outcome
    // (Pleasure/Failed/Fight).  That made the next fight handoff run from
    // root=None and caused crowd hostility to flicker/cancel.  Let the
    // InCombatGreet owner process the close edge first when the active primary
    // is still the pleasure-cycle speaker.
    const bool activeCycleDialogueProtection =
        orphanTruceRoot &&
        primary != 0 &&
        TFD::InCombatGreet::IsPleasureCycleActiveForActor(primary != 0 ? RE::TESForm::LookupByID<RE::Actor>(primary) : nullptr) &&
        (TFD::InCombatGreet::HasSeenDialogue() ||
            (TFD::InteractionRouter::DialogueOpen::IsActive() &&
                TFD::InteractionRouter::DialogueOpen::GetMode() == TFD::InteractionRouter::DialogueOpen::Mode::InCombatTruce));

    const bool activeTruceProtection =
        activeCycleDialogueProtection ||
        (!orphanTruceRoot &&
            !abandonedPayFollowup &&
            (observed.flow.gate == TFD::FlowController::DecisionGate::Truce ||
                primarySuppressed ||
                primaryCanOpenTruceDialogue));
    const bool protectedTruceDecision = activeDialogueProtection || activeTruceProtection;
    const bool payFollowupTerminalPending = abandonedPayFollowup && primaryPayGuardActive;

    if (protectedTruceDecision) {
        if (g_hasStaleInCombatNeutralSince) {
            spdlog::info(
                "[TFD][Flow][R139] stale InCombat neutral protected by active truce/dialogue primary={:08X} tickReason={} gate={} dialogue={} suppressed={} canDialogue={} orphanTruce={} cycleProtect={}",
                primary,
                tickReason.empty() ? std::string{ "-" } : std::string{ tickReason },
                TFD::FlowController::Controller::ToString(observed.flow.gate),
                observed.dialogueGlobal,
                primarySuppressed ? 1 : 0,
                primaryCanOpenTruceDialogue ? 1 : 0,
                orphanTruceRoot ? 1 : 0,
                activeCycleDialogueProtection ? 1 : 0);
        }
        g_hasStaleInCombatNeutralSince = false;
        g_staleInCombatNeutralPrimary = 0;
        return false;
    }

    if (!g_hasStaleInCombatNeutralSince || g_staleInCombatNeutralPrimary != primary) {
        g_hasStaleInCombatNeutralSince = true;
        g_staleInCombatNeutralSince = now;
        g_staleInCombatNeutralPrimary = primary;
        if (payFollowupTerminalPending) {
            spdlog::info(
                "[TFD][Flow][P21OWN] defer stale InCombatPayFollowup cleanup primary={:08X} reason=terminal_outcome_pending payGuard=1 holdSec=8 tickReason={} dialogue={} sub={} gate={}",
                primary,
                tickReason.empty() ? std::string{ "-" } : std::string{ tickReason },
                observed.dialogueGlobal,
                TFD::FlowController::Controller::ToString(observed.flow.sub),
                TFD::FlowController::Controller::ToString(observed.flow.gate));
        }
        return false;
    }

    const bool primaryInvalid = IsObservedPrimaryInvalidOrDead(observed);
    const auto hold = abandonedPayFollowup ?
        (payFollowupTerminalPending ? std::chrono::seconds(8) : std::chrono::seconds(2)) :
        (orphanTruceRoot ? std::chrono::seconds(3) :
            (primaryInvalid ? std::chrono::seconds(1) : std::chrono::seconds(6)));
    if ((now - g_staleInCombatNeutralSince) < hold) {
        return false;
    }

    const char* resetReason = abandonedPayFollowup ?
        "stale_incombat_pay_followup_no_owner" :
        (orphanTruceRoot ? "stale_incombat_truce_root_no_pressure" :
            (primaryInvalid ? "stale_incombat_primary_dead_neutral" : "stale_incombat_no_active_hostiles_neutral"));

    bool releasedTruceSession = false;
    if (primary != 0) {
        if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(primary)) {
            // Use FlowHandoff so this is a state-owner cleanup, not a betrayal,
            // PlayerArmed wake, or forced rehostile path.  Any release safe-pass
            // owner remains responsible for its normal 10s hostile return.
            releasedTruceSession = TFD::HostilityController::ReleaseActiveTruceSessionForActor(
                actor,
                TFD::HostilityController::ReleaseReason::FlowHandoff,
                false);
        }
    }

    spdlog::info(
        "[TFD][Flow][P20OWN] auto complete stale InCombat root primary={:08X} primaryInvalid={} abandonedPayFollowup={} payPending={} orphanTruce={} releasedSession={} reason={} tickReason={} globals(in={} defeat={}) sub={} gate={} dialogue={} reasonActor={:08X}",
        primary,
        primaryInvalid ? 1 : 0,
        abandonedPayFollowup ? 1 : 0,
        payFollowupTerminalPending ? 1 : 0,
        orphanTruceRoot ? 1 : 0,
        releasedTruceSession ? 1 : 0,
        resetReason,
        tickReason.empty() ? std::string{ "-" } : std::string{ tickReason },
        observed.inCombatGlobal,
        observed.defeatGlobal,
        TFD::FlowController::Controller::ToString(observed.flow.sub),
        TFD::FlowController::Controller::ToString(observed.flow.gate),
        observed.dialogueGlobal,
        observed.reasonActorFormID);

    TFD::FlowController::Controller::GetSingleton().ResetRuntime(resetReason);
    g_hasStaleInCombatNeutralSince = false;
    g_staleInCombatNeutralPrimary = 0;
    return true;
}


static bool AutoInterruptInCombatAfterPleasureIfNeeded(
    const TFD::FlowController::ObservedMainStateSnapshot& observed,
    std::string_view tickReason)
{
    using TFD::FlowController::ObservedMainState;
    using TFD::FlowController::RootFlow;
    using TFD::FlowController::SubFlow;

    const bool inCombatAfterPleasureOwner =
        observed.flow.contextRoot == RootFlow::InCombat &&
        observed.flow.sub == SubFlow::InCombatAfterPleasure;
    const bool combatPressure =
        observed.observed == ObservedMainState::Incombat &&
        (observed.activeHostileCount > 0 || observed.mutualLosHostileCount > 0);

    if (!inCombatAfterPleasureOwner || !combatPressure) {
        return false;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* primaryActor = observed.flow.primaryActorFormID != 0 ?
        RE::TESForm::LookupByID<RE::Actor>(observed.flow.primaryActorFormID) :
        nullptr;
    auto* pressureActor = observed.reasonActorFormID != 0 ?
        RE::TESForm::LookupByID<RE::Actor>(observed.reasonActorFormID) :
        nullptr;

    if (!player || !primaryActor) {
        spdlog::warn(
            "[TFD][Flow][R347A] reject incombat afterpleasure interrupt reason=invalid_actor primary={:08X} pressure={:08X} player={} tickReason={}",
            observed.flow.primaryActorFormID,
            observed.reasonActorFormID,
            player ? 1 : 0,
            tickReason.empty() ? std::string{ "-" } : std::string{ tickReason });
        return false;
    }

    const char* why = "incombat_after_pleasure_combat_interrupt";

    // R347A: InCombat AfterPleasure is a dialogue-owned passive state.  If real
    // combat pressure returns while this menu is still open, the dialogue owner
    // must lose authority immediately.  Do not go through the generic stale clear
    // bridge; this is owned by the InCombat AfterPleasure transition itself.
    TFD::InteractionRouter::DialogueOpen::ForceCloseDialogueMenu(why);
    TFD::PleasureRuntime::Break(why, true, true, false);
    TFD::ForceGreetState::ResetAfterPleasure();

    const bool primaryBroken = TFD::HostilityController::BreakPassiveOwnershipForFightChoice(
        primaryActor,
        player,
        why);

    bool pressureBroken = false;
    if (pressureActor && pressureActor != primaryActor && pressureActor != player) {
        pressureBroken = TFD::HostilityController::BreakPassiveOwnershipForFightChoice(
            pressureActor,
            player,
            "incombat_after_pleasure_combat_interrupt_pressure_actor");
    }

    const bool afterPleasureComplete = TFD::FlowController::Controller::GetSingleton().RequestCompleteAfterPleasure(why);

    TFD::FlowController::QueueBridgeModEvent("TFDInCombatClear", primaryActor, why, 1.0f);
    TFD::FlowController::QueueBridgeModEvent("TFDTruceUnassign", primaryActor, why, 1.0f);
    TFD::FlowController::QueueBridgeModEvent("TFDInCombatEmergencyCancel", primaryActor, why, 1.0f);
    TFD::InCombat::Complete(why);

    spdlog::warn(
        "[TFD][Flow][R347A] incombat afterpleasure interrupted by combat primary={:08X} pressureActor={:08X} pressureTarget={:08X} activeHostile={} mutualLos={} primaryBroken={} pressureBroken={} afterPleasureComplete={} dialogue={} pleasure={} tickReason={}",
        observed.flow.primaryActorFormID,
        observed.reasonActorFormID,
        observed.reasonTargetFormID,
        observed.activeHostileCount,
        observed.mutualLosHostileCount,
        primaryBroken ? 1 : 0,
        pressureBroken ? 1 : 0,
        afterPleasureComplete ? 1 : 0,
        observed.dialogueGlobal,
        observed.pleasureGlobal,
        tickReason.empty() ? std::string{ "-" } : std::string{ tickReason });

    return true;
}

static RE::Actor* ResolveFallbackPassivePrimaryActor()
{
    if (g_passiveRuntimeProviders.resolveFallbackPassivePrimaryActor) {
        return g_passiveRuntimeProviders.resolveFallbackPassivePrimaryActor();
    }
    return nullptr;
}

static RE::Actor* ResolveCurrentPassivePrimaryActor()
{
    const auto snapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
    if (snapshot.primaryActorFormID != 0) {
        if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(snapshot.primaryActorFormID)) {
            return actor;
        }
    }

    if (auto* speaker = TFD::PleasureRuntime::GetPrimarySpeaker()) {
        return speaker;
    }

    return ResolveFallbackPassivePrimaryActor();
}

static bool ShouldUsePassivePrimaryFallbackForCoverage()
{
    auto& flow = TFD::FlowController::Controller::GetSingleton();
    const auto snapshot = flow.GetSnapshot();

    if (snapshot.root == TFD::FlowController::RootFlow::Captive &&
        snapshot.captiveMode == TFD::FlowController::CaptiveMode::JoinedEnemy) {
        return true;
    }

    if (TFD::PleasureRuntime::IsPassiveLockActive() ||
        flow.IsPleasureSubFlowActive() ||
        flow.IsAfterPleasureSubFlowActive()) {
        return true;
    }

    if (TFD::Captive::IsCaptivePassiveHoldActive()) {
        return true;
    }

    if (IsBleedoutRuntimeActive() || flow.IsTerminalDialogueGateActive()) {
        return true;
    }

    return false;
}

static bool IsActorCoveredByCurrentPassiveContext(RE::Actor* actor)
{
    if (!actor || actor == RE::PlayerCharacter::GetSingleton() || actor->IsDead() || actor->IsDisabled()) {
        return false;
    }

    // R300B: a provider returning false must not shut down the local fallback.
    // TFDDefeatMonitor currently installs a conservative provider that can return
    // false for every actor; the fallback below is the shared actor-ownership net
    // for pacify, truce, grace, pleasure, and phase-faction actors.
    if (g_passiveRuntimeProviders.isActorCoveredByPassiveContext &&
        g_passiveRuntimeProviders.isActorCoveredByPassiveContext(actor)) {
        return true;
    }

    if (TFD::HostilityController::IsActorTemporarilySuppressed(actor) ||
        TFD::HostilityController::IsSuppressed(actor)) {
        return true;
    }

    if (TFD::Actor::Ops::HasReleaseFollowGrace(actor) ||
        TFD::Actor::Ops::HasTemporaryFollowLock(actor)) {
        return true;
    }

    if (TFD::PleasureRuntime::IsActorTracked(actor)) {
        return true;
    }

    if (ShouldUsePassivePrimaryFallbackForCoverage()) {
        if (auto* primary = ResolveCurrentPassivePrimaryActor()) {
            if (actor == primary) {
                return true;
            }
            if (TFD::Actor::SharesAllowedFactionExact(actor, primary)) {
                return true;
            }
        }
    }

    return false;
}

static RE::Actor* ResolveActorFromEventArg(const std::string_view& arg)
{
    if (arg.empty()) {
        return nullptr;
    }

    std::string parsedArg(arg);
    char* end = nullptr;
    const auto raw = std::strtoul(parsedArg.c_str(), &end, 0);
    if (end == nullptr || end == parsedArg.c_str()) {
        return nullptr;
    }

    return RE::TESForm::LookupByID<RE::Actor>(static_cast<RE::FormID>(raw));
}

static RE::TESFaction* ResolveFactionByEditorID(const char* editorId)
{
    if (!editorId || !editorId[0]) {
        return nullptr;
    }
    return RE::TESForm::LookupByEditorID<RE::TESFaction>(editorId);
}

static bool RemoveFactionByEditorID(RE::Actor* actor, const char* editorId, const char* reason)
{
    if (!actor || !editorId || !editorId[0]) {
        return false;
    }

    auto* faction = ResolveFactionByEditorID(editorId);
    if (!faction || !actor->IsInFaction(faction)) {
        return false;
    }

    actor->RemoveFromFaction(faction);
    spdlog::info(
        "[TFD][Flow] actor phase faction removed actor={:08X} faction={} reason={}",
        actor->GetFormID(),
        editorId,
        reason ? reason : "unknown");
    return true;
}

static void ClearPleasureFailedFightSpeakerSuppressors(RE::Actor* actor, const char* reason)
{
    if (!actor) {
        return;
    }

    // W52: Pleasure Failed -> Fight is an explicit combat handoff.  It must not
    // keep any dialogue/pacify/phase faction from Truce, Bleedout, Captive,
    // AfterPleasure, or temporary release grace.  Keeping even one of these can
    // make the actor rehostile but unable to draw/attack, producing the fast
    // draw/sheath loop reported in testing.
    const char* passiveFactionEditorIDs[] = {
        "TFDAfterPleasureFaction",
        "TFDPleasureFailedFaction",
        "TFDWorkingCaptiveFaction",
        "TFDPacifyFaction",
        "TFDPreCombatTruceFaction",
        "TFDInCombatTruceFaction",
        "TFDBleedOutFaction",
        "TFDBleedoutFaction",
        "TFDCaptiveFaction",
        "TFDPleasureRejectFaction",
        "TFDPleasureWatcherFaction",
        "TFDDesireFaction",
        "TFDSaviorFaction"
    };

    std::vector<std::string> removedNames{};
    removedNames.reserve(sizeof(passiveFactionEditorIDs) / sizeof(passiveFactionEditorIDs[0]));
    for (const char* editorID : passiveFactionEditorIDs) {
        if (RemoveFactionByEditorID(actor, editorID, reason)) {
            removedNames.emplace_back(editorID ? editorID : "<null>");
        }
    }

    TFD::Actor::Ops::RemoveReleaseFollowGraceFromActorOnlyForFightChoice(
        actor,
        reason ? reason : "pleasure_failed_fight_clear_grace_no_restore");
    TFD::Bleedout::ClearBleedoutDialogueFactionForActor(actor, reason ? reason : "pleasure_failed_fight_clear_bleedout_dialogue");

    if (auto* process = RE::ProcessLists::GetSingleton()) {
        process->ClearCachedFactionFightReactions();
        process->runDetection = true;
    }

    actor->AllowPCDialogue(true);
    // R152: do not EvaluatePackage here. This helper is used by the explicit
    // PleasureFailed -> Fight path; package evaluation after removing grace can
    // immediately choose a sheathed vanilla/dialogue package before the final
    // combat owner handoff has locked target + draw. HostilityController now owns
    // the final order: remove passive owner -> set aggression -> lock target -> draw.

    if (!removedNames.empty()) {
        std::ostringstream ss;
        for (std::size_t i = 0; i < removedNames.size(); ++i) {
            if (i > 0) {
                ss << ',';
            }
            ss << removedNames[i];
        }
        spdlog::info(
            "[TFD][Flow][W52] pleasure failed fight passive suppressors cleared actor={:08X} removed={} reason={}",
            actor->GetFormID(),
            ss.str(),
            reason ? reason : "unknown");
    }
    else {
        spdlog::info(
            "[TFD][Flow][W52] pleasure failed fight passive suppressors checked actor={:08X} removed=none reason={}",
            actor->GetFormID(),
            reason ? reason : "unknown");
    }
}

static bool SuppressDefeatedEnemyAutoDeathForFightChoiceIfNeeded(RE::Actor* actor, const char* reason)
{
    const char* why = reason ? reason : "pleasure_failed_fight";
    if (!actor) {
        return false;
    }

    // R150: PleasureFailed -> Fight from Bleedout targets the attacker, not a
    // knocked defeated enemy.  The old unconditional defeated-auto-death guard
    // sent BleedoutStop/GetUpStart + EvaluatePackage to the standing attacker
    // immediately before rehostile, which could leave the actor hostile but
    // weapon-sheathed.  Only run that get-up guard for actors that are actually
    // in defeated/knocked state.
    if (!TFD::Actor::Ops::IsDefeatedEnemyKnocked(actor)) {
        spdlog::info(
            "[TFD][Flow][R150] skip defeated getup suppress actor={:08X} knocked=0 reason={}",
            actor->GetFormID(),
            why);
        return false;
    }

    TFD::Victory::SuppressAutoDeathForExternalFight(actor, 24.0, why);
    return true;
}

static RE::FormID ResolveActorFormIDFromEventArg(const std::string_view& arg)
{
    if (auto* actor = ResolveActorFromEventArg(arg)) {
        return actor->GetFormID();
    }
    return 0;
}


static bool IsValidReleaseWakeActor(RE::Actor* actor, RE::Actor* player)
{
    return actor &&
        player &&
        actor != player &&
        !actor->IsDead() &&
        !actor->IsDisabled();
}

static std::size_t WakeInCombatReleasePackForDetection(const std::vector<RE::Actor*>& actors, std::string_view reason)
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || actors.empty()) {
        return 0;
    }

    if (auto* process = RE::ProcessLists::GetSingleton()) {
        process->runDetection = true;
        process->ClearCachedFactionFightReactions();
    }

    std::vector<RE::FormID> seen;
    seen.reserve(actors.size());
    std::size_t queued = 0;

    for (auto* actor : actors) {
        if (!IsValidReleaseWakeActor(actor, player)) {
            continue;
        }

        const RE::FormID actorId = actor->GetFormID();
        if (actorId == 0 || std::find(seen.begin(), seen.end(), actorId) != seen.end()) {
            continue;
        }
        seen.push_back(actorId);

        const bool hostileBefore = actor->IsHostileToActor(player);
        const bool inCombatBefore = actor->IsInCombat();

        actor->SetBeenAttacked(true);
        player->SetBeenAttacked(true);
        (void)actor->RequestDetectionLevel(player, RE::DETECTION_PRIORITY::kCritical);
        (void)player->RequestDetectionLevel(actor, RE::DETECTION_PRIORITY::kCritical);

        if (!actor->IsWeaponDrawn()) {
            actor->DrawWeaponMagicHands(true);
        }

        actor->EvaluatePackage(false, true);
        actor->EvaluatePackage(true, true);

        const bool sent = TFD::FlowController::QueueBridgeModEvent(
            "TFDInCombatResumeCombat",
            actor,
            "incombat_release_detection_wake",
            1.0f);
        if (sent) {
            ++queued;
        }

        spdlog::info(
            "[TFD][Flow][R93Y] incombat release detection wake actor={:08X} hostileBefore={} inCombatBefore={} eventQueued={} reason={}",
            actorId,
            hostileBefore ? 1 : 0,
            inCombatBefore ? 1 : 0,
            sent ? 1 : 0,
            reason.empty() ? std::string{ "-" } : std::string{ reason });
    }

    if (auto* process = RE::ProcessLists::GetSingleton()) {
        process->ClearCachedFactionFightReactions();
    }

    spdlog::info(
        "[TFD][Flow][R93Y] incombat release detection wake summary candidates={} queued={} reason={}",
        static_cast<unsigned int>(actors.size()),
        static_cast<unsigned int>(queued),
        reason.empty() ? std::string{ "-" } : std::string{ reason });

    return queued;
}

static RE::FormID ResolveActorFormIDFromEventArgOrSender(const std::string_view& arg, RE::TESForm* sender)
{
    if (const auto actorFormID = ResolveActorFormIDFromEventArg(arg); actorFormID != 0) {
        return actorFormID;
    }

    if (sender) {
        if (auto* actor = sender->As<RE::Actor>()) {
            return actor->GetFormID();
        }
        return sender->GetFormID();
    }

    return TFD::FlowController::Controller::GetSingleton().GetSnapshot().primaryActorFormID;
}

static int ResolveSourceFlowFromEventArg(const std::string_view& arg)
{
    if (arg.empty()) {
        return 0;
    }

    const auto firstSep = arg.find('|');
    if (firstSep == std::string_view::npos || firstSep + 1 >= arg.size()) {
        return 0;
    }

    auto sourceView = arg.substr(firstSep + 1);
    const auto secondSep = sourceView.find('|');
    if (secondSep != std::string_view::npos) {
        sourceView = sourceView.substr(0, secondSep);
    }

    std::string sourceText(sourceView);
    char* end = nullptr;
    const auto raw = std::strtol(sourceText.c_str(), &end, 10);
    if (end == nullptr || end == sourceText.c_str()) {
        return 0;
    }

    return static_cast<int>(raw);
}

static bool IsAfterPleasureTerminalChoiceEvent(std::string_view name)
{
    return name == kAfterPleasureChoiceReleaseEvent ||
        name == kAfterPleasureChoiceRecruitEvent ||
        name == kAfterPleasureChoiceFinishEvent ||
        name == kAfterPleasureChoiceJoinEnemyEvent ||
        name == kAfterPleasureChoiceKidnapEvent ||
        name == kAfterPleasureChoiceWorkEvent;
}

static bool IsInCombatSource(int sourceFlow)
{
    return sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::InCombat);
}

static bool IsTeammateSource(int sourceFlow)
{
    return sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::Teammate);
}

static bool IsNoGenericPleasureAftermathSource(int sourceFlow)
{
    return IsTeammateSource(sourceFlow);
}

static TFD::HostilityController::ReleaseReason ResolveInCombatTerminalReleaseReason(std::string_view name)
{
    using TFD::HostilityController::ReleaseReason;
    if (name == kAfterPleasureChoiceFinishEvent) {
        return ReleaseReason::DialogueClosed;
    }
    // R99A: recruit terminal from an InCombat after-pleasure chain is not a
    // normal Generic cleanup. Generic releases only remove TFD suppression; they do
    // not wake the engine detection/combat cache. After recruit, converted teammates
    // must keep follow package, while any remaining hostile pack members must have
    // detection rearmed after the truce aliases are cleared.
    if (name == kAfterPleasureChoiceRecruitEvent) {
        return ReleaseReason::InCombatPleasureEnd;
    }
    return ReleaseReason::Generic;
}

static const char* ResolveInCombatTerminalReason(std::string_view name)
{
    if (name == kAfterPleasureChoiceReleaseEvent) {
        return "incombat_after_pleasure_release";
    }
    if (name == kAfterPleasureChoiceRecruitEvent) {
        return "incombat_after_pleasure_recruit";
    }
    if (name == kAfterPleasureChoiceFinishEvent) {
        return "incombat_after_pleasure_finish";
    }
    if (name == kAfterPleasureChoiceJoinEnemyEvent) {
        return "incombat_after_pleasure_join_enemy";
    }
    if (name == kAfterPleasureChoiceKidnapEvent) {
        return "incombat_after_pleasure_kidnap";
    }
    if (name == kAfterPleasureChoiceWorkEvent) {
        return "incombat_after_pleasure_work";
    }
    return "incombat_after_pleasure_terminal";
}

static const char* ResolveBleedoutAfterPleasureTerminalReason(std::string_view name)
{
    if (name == kAfterPleasureChoiceReleaseEvent) {
        return "bleedout_after_pleasure_release";
    }
    if (name == kAfterPleasureChoiceRecruitEvent) {
        return "bleedout_after_pleasure_recruit";
    }
    if (name == kAfterPleasureChoiceFinishEvent) {
        return "bleedout_after_pleasure_finish";
    }
    if (name == kAfterPleasureChoiceJoinEnemyEvent) {
        return "bleedout_after_pleasure_join_enemy";
    }
    if (name == kAfterPleasureChoiceKidnapEvent) {
        return "bleedout_after_pleasure_kidnap";
    }
    if (name == kAfterPleasureChoiceWorkEvent) {
        return "bleedout_after_pleasure_work";
    }
    return "bleedout_after_pleasure_terminal";
}

static std::uint32_t ResolveBleedFlowActorFormIDFromProviders()
{
    if (g_outcomeRuntimeProviders.resolveBleedFlowActorFormID) {
        return g_outcomeRuntimeProviders.resolveBleedFlowActorFormID();
    }
    return TFD::FlowController::Controller::GetSingleton().GetSnapshot().primaryActorFormID;
}

static bool IsBleedStateActiveFromProviders()
{
    return g_outcomeRuntimeProviders.isBleedStateActive && g_outcomeRuntimeProviders.isBleedStateActive();
}

namespace TFD::FlowController
{

    namespace
    {
        RootFlow ProjectExternalRootFlow(const Snapshot& snapshot)
        {
            if (snapshot.root != RootFlow::None) {
                return snapshot.root;
            }

            if (!TFD::Transition::IsRecoveryActive()) {
                return RootFlow::None;
            }

            switch (TFD::Transition::GetCurrentFallbackBranch()) {
            case TFD::Transition::FallbackBranch::RescueCached:
                return RootFlow::Rescue;
            case TFD::Transition::FallbackBranch::RecoveryFollower:
            case TFD::Transition::FallbackBranch::RecoveryPotion:
                return RootFlow::Recovery;
            case TFD::Transition::FallbackBranch::LeftForDeadSolo:
            case TFD::Transition::FallbackBranch::LeftForDeadWithFollower:
                return RootFlow::LeftForDead;
            case TFD::Transition::FallbackBranch::None:
            default:
                break;
            }

            return RootFlow::None;
        }

        Snapshot ProjectExternalSnapshot(Snapshot snapshot)
        {
            const auto projectedRoot = ProjectExternalRootFlow(snapshot);
            if (projectedRoot != RootFlow::None && snapshot.root == RootFlow::None) {
                snapshot.root = projectedRoot;
                if (snapshot.contextRoot == RootFlow::None) {
                    snapshot.contextRoot = projectedRoot;
                }
            }
            return snapshot;
        }
    }

    void InstallRuntime()
    {
        g_flowRuntimeInstalled = true;
        TFD::Release::Install();
        spdlog::info("[TFD][Flow] Runtime Install");
    }

    void ResetRuntimeLifecycle()
    {
        TFD::InteractionRouter::DialogueOpen::Cancel();
        TFD::Release::Reset();
        spdlog::info("[TFD][Flow] Runtime ResetLifecycle");
    }

    bool TickRuntime()
    {
        if (!g_flowRuntimeInstalled) {
            return false;
        }

        if (g_continuousRuntimeProviders.preRuntimeFlowTick && g_continuousRuntimeProviders.preRuntimeFlowTick()) {
            return true;
        }

        if (g_continuousRuntimeProviders.isGamePaused && g_continuousRuntimeProviders.isGamePaused()) {
            return true;
        }

        RE::Actor* player = nullptr;
        if (g_continuousRuntimeProviders.resolvePlayer) {
            player = g_continuousRuntimeProviders.resolvePlayer();
        }
        else {
            player = RE::PlayerCharacter::GetSingleton();
        }
        if (!player) {
            if (g_continuousRuntimeProviders.onNoPlayerTick) {
                g_continuousRuntimeProviders.onNoPlayerTick();
            }
            return true;
        }

        TFD::InteractionRouter::DialogueOpen::Tick();
        TFD::PleasureRuntime::Tick();
        TFD::Actor::Ops::MaintainReleaseFollowGrace();
        TFD::Release::Tick();

        if (g_continuousRuntimeProviders.updateAmbientKidnapAvailability) {
            g_continuousRuntimeProviders.updateAmbientKidnapAvailability(false);
        }

        const bool postRuntimeHandled = g_continuousRuntimeProviders.postRuntimeFlowTick && g_continuousRuntimeProviders.postRuntimeFlowTick(player);
        TFD::FlowController::Controller::GetSingleton().TickObservedMainStateDiagnostic(
            player,
            postRuntimeHandled ? "post_runtime_flow_tick_handled" : "runtime_tick");
        if (postRuntimeHandled) {
            return true;
        }

        return false;
    }

    void InstallContinuousRuntimeProviders(ContinuousRuntimeProviders providers)
    {
        g_continuousRuntimeProviders = std::move(providers);
    }

    void ResetContinuousRuntimeProviders()
    {
        g_continuousRuntimeProviders = {};
    }

    void Controller::RefreshFlowGlobalsLocked()
    {
        ResolveGlobal(g_preCombatState, "TFDPreCombatState");
        ResolveGlobal(g_inCombatState, "TFDInCombatState");
        ResolveGlobal(g_captiveState, "TFDCaptiveState");
        ResolveGlobal(g_pleasureState, "TFDPleasureState");
        ResolveGlobal(g_defeatState, "TFDDefeatState");

        const int preCombat = (_snapshot.root == RootFlow::PreCombat) ? 1 : 0;

        const auto runtimePhase = TFD::PleasureRuntime::GetPhase();
        const auto runtimeSource = TFD::PleasureRuntime::GetSourceContext();
        const bool pleasureFailedDialogue = TFD::PleasureRuntime::IsPleasureFailedDialogueActive();

        // R134: FlowController owns the dialogue-facing root mirrors whenever a
        // flow transition is committed.  Do not preserve TFDDefeatState=2 just
        // because a previous Bleedout dialogue wrote it.  That stale value wins
        // over InCombat/PreCombat in HUD and CK conditions and can keep
        // the system looking like it is still in Defeat after Pleasure/Captive/
        // Release outcomes have already committed.
        const bool captiveEscapeBleedoutDecisionRoot =
            _snapshot.root == RootFlow::Captive &&
            _snapshot.gate == DecisionGate::PlayerBleedout &&
            (_snapshot.sub == SubFlow::EscapeFailed || _snapshot.sub == SubFlow::Recapture) &&
            !_snapshot.terminalResolved;
        const bool captiveEscapeBreakBleedoutRoot =
            _snapshot.root == RootFlow::Bleedout &&
            _snapshot.contextRoot == RootFlow::Captive &&
            _snapshot.gate == DecisionGate::PlayerBleedout &&
            _snapshot.sub == SubFlow::BleedoutEscapeBreak &&
            !_snapshot.terminalResolved;
        const bool bleedoutDecisionRoot =
            ((_snapshot.root == RootFlow::Bleedout && _snapshot.sub == SubFlow::None) ||
                captiveEscapeBleedoutDecisionRoot ||
                captiveEscapeBreakBleedoutRoot) &&
            _snapshot.gate == DecisionGate::PlayerBleedout &&
            !_snapshot.terminalResolved;
        const bool bleedoutTerminalPleasure =
            _snapshot.terminalResolved &&
            _snapshot.root == RootFlow::None &&
            _snapshot.sub == SubFlow::BleedoutPleasure;
        const bool bleedoutSourcePleasureRuntime =
            runtimeSource == TFD::PleasureRuntime::SourceContext::Bleedout &&
            (runtimePhase == TFD::PleasureRuntime::Phase::PleasureStartPending ||
                runtimePhase == TFD::PleasureRuntime::Phase::PleasureActive ||
                runtimePhase == TFD::PleasureRuntime::Phase::PleasureEnding);

        const int observedDefeat = g_defeatState ? static_cast<int>(std::lround(g_defeatState->value)) : 0;
        int defeat = observedDefeat;
        if (bleedoutDecisionRoot) {
            defeat = 2;
        }
        else if (bleedoutTerminalPleasure || bleedoutSourcePleasureRuntime || defeat >= 2) {
            defeat = 0;
        }

        if (observedDefeat >= 2 && defeat == 0) {
            spdlog::info(
                "[TFD][Flow][R134] scrub stale Defeat global old={} root={} ctx={} gate={} sub={} terminal={} primary={:08X}",
                observedDefeat,
                ToString(_snapshot.root),
                ToString(_snapshot.contextRoot),
                ToString(_snapshot.gate),
                ToString(_snapshot.sub),
                _snapshot.terminalResolved ? 1 : 0,
                _snapshot.primaryActorFormID);
        }

        const bool bleedoutAfterPleasure =
            runtimeSource == TFD::PleasureRuntime::SourceContext::Bleedout &&
            (runtimePhase == TFD::PleasureRuntime::Phase::AfterPleasureAwaitQuest ||
                runtimePhase == TFD::PleasureRuntime::Phase::AfterPleasureDialogue ||
                runtimePhase == TFD::PleasureRuntime::Phase::Finalizing ||
                pleasureFailedDialogue);

        const bool captiveReleasedWork =
            _snapshot.root == RootFlow::Captive &&
            (_snapshot.sub == SubFlow::WorkForEnemy || TFD::Captive::IsReleasedWorkActive());

        int inCombat = (defeat >= 2) ? 0 : (_combatActive ? 1 : 0);
        if (_snapshot.sub == SubFlow::BleedoutAfterPleasure || bleedoutTerminalPleasure || bleedoutSourcePleasureRuntime ||
            bleedoutAfterPleasure || pleasureFailedDialogue || captiveReleasedWork) {
            inCombat = 0;
        }

        int captive = 0;
        if ((_snapshot.root == RootFlow::Captive || _snapshot.contextRoot == RootFlow::Captive) &&
            _snapshot.captiveMode != CaptiveMode::JoinedEnemy) {
            const bool captiveEscapeSub =
                _snapshot.sub == SubFlow::EscapeAttempt ||
                _snapshot.sub == SubFlow::EscapeFailed ||
                _snapshot.sub == SubFlow::Recapture ||
                _snapshot.sub == SubFlow::InCombatEscapeBreak ||
                _snapshot.sub == SubFlow::BleedoutEscapeBreak;

            if (captiveEscapeSub && !_snapshot.terminalResolved) {
                captive = 2;
            }
            else {
                captive = static_cast<int>(TFD::Captive::GetPhaseRaw());
                if (captive <= 0) {
                    captive = 1;
                }
            }
        }

        int pleasure = 0;
        if (TFD::PleasureRuntime::IsActive()) {
            if (pleasureFailedDialogue) {
                // R137: Pleasure Failed is its own dialogue condition state.
                // Do not collapse it to generic AfterPleasure=2, otherwise CK
                // child topics can fail while the native opener keeps reopening.
                pleasure = 3;
            }
            else switch (TFD::PleasureRuntime::GetPhase()) {
            case TFD::PleasureRuntime::Phase::PleasureStartPending:
            case TFD::PleasureRuntime::Phase::PleasureActive:
            case TFD::PleasureRuntime::Phase::PleasureEnding:
            case TFD::PleasureRuntime::Phase::RedoPending:
                pleasure = 1;
                break;
            case TFD::PleasureRuntime::Phase::AfterPleasureAwaitQuest:
            case TFD::PleasureRuntime::Phase::AfterPleasureDialogue:
            case TFD::PleasureRuntime::Phase::Finalizing:
                pleasure = 2;
                break;
            default:
                break;
            }
        }
        else {
            switch (_snapshot.sub) {
            case SubFlow::PreCombatPleasure:
            case SubFlow::InCombatPleasure:
            case SubFlow::BleedoutPleasure:
            case SubFlow::CaptivePleasure:
                pleasure = 1;
                break;
            case SubFlow::PreCombatAfterPleasure:
            case SubFlow::InCombatAfterPleasure:
            case SubFlow::BleedoutAfterPleasure:
            case SubFlow::CaptiveAfterPleasure:
                pleasure = 2;
                break;
            default:
                break;
            }
        }

        SetGlobalInt(g_preCombatState, preCombat);
        SetGlobalInt(g_inCombatState, inCombat);
        SetGlobalInt(g_captiveState, captive);
        SetGlobalInt(g_pleasureState, pleasure);
        SetGlobalInt(g_defeatState, defeat);
    }

    Controller& Controller::GetSingleton()
    {
        static Controller singleton;
        return singleton;
    }


    void InstallDefeatLifecycleProviders(DefeatLifecycleProviders providers)
    {
        InstallPassiveRuntimeProviders(std::move(providers.passive));
        InstallOutcomeRuntimeProviders(std::move(providers.outcome));
        InstallBattleObserverRuntimeProviders(std::move(providers.battleObserver));
        InstallContinuousRuntimeProviders(std::move(providers.continuous));
    }

    void ResetDefeatLifecycleProviders()
    {
        ResetPassiveRuntimeProviders();
        ResetOutcomeRuntimeProviders();
        ResetBattleObserverRuntimeProviders();
        ResetContinuousRuntimeProviders();
    }

    void InstallPassiveRuntimeProviders(PassiveRuntimeProviders providers)
    {
        g_passiveRuntimeProviders = std::move(providers);
    }

    void ResetPassiveRuntimeProviders()
    {
        g_passiveRuntimeProviders = {};
    }

    void InstallOutcomeRuntimeProviders(OutcomeRuntimeProviders providers)
    {
        g_outcomeRuntimeProviders = std::move(providers);
    }

    void ResetOutcomeRuntimeProviders()
    {
        g_outcomeRuntimeProviders = {};
    }

    void InstallBattleObserverRuntimeProviders(BattleObserverRuntimeProviders providers)
    {
        g_battleObserverRuntimeProviders = std::move(providers);
    }

    void ResetBattleObserverRuntimeProviders()
    {
        g_battleObserverRuntimeProviders = {};
    }

    ObservedDefeatResolution EvaluateObservedDefeatResolution(const ObservedDefeatInput& input)
    {
        if (input.forceCaptive) {
            return ObservedDefeatResolution::Captive;
        }

        if (!input.conflictResolved) {
            return ObservedDefeatResolution::ContinueObserve;
        }

        // CB03: battle-observe Left For Dead is no longer resolved here.
        // Left For Dead is owned by the Bleedout forcegreet "Do Nothing" outcome.
        // A standing player-side actor means the observe phase should resolve to recovery/rescue fallback.
        if (input.hasStandingPlayerSide || input.hasStandingTeammate) {
            return ObservedDefeatResolution::NonCaptiveChoice;
        }

        if (input.hasStandingHostileCoalition && (input.hasCaptiveMarker || input.canUseCaptiveFallback)) {
            return ObservedDefeatResolution::Captive;
        }

        return ObservedDefeatResolution::LeftForDead;
    }

    NonCaptiveFallbackResolution EvaluateNonCaptiveFallback(const NonCaptiveFallbackInput& input)
    {
        if (input.hasSavior) {
            return input.hasCachedRescueDestination ? NonCaptiveFallbackResolution::RescueCached : NonCaptiveFallbackResolution::RecoveryFollower;
        }

        if (input.hasStandingFollower) {
            return NonCaptiveFallbackResolution::RecoveryFollower;
        }

        if (input.hasDownedFollower) {
            return NonCaptiveFallbackResolution::LeftForDeadWithFollower;
        }

        if (input.hasCachedRescueDestination) {
            return NonCaptiveFallbackResolution::RescueCached;
        }

        if (input.hasRecoveryPotion) {
            return NonCaptiveFallbackResolution::RecoveryPotion;
        }

        return NonCaptiveFallbackResolution::LeftForDeadSolo;
    }


    bool ExecuteResolvedNoMarkerFallback(const char* reason, const NonCaptiveFallbackExecutionHandlers& handlers)
    {
        if (!handlers.tryBeginTerminalCommit ||
            !handlers.clearCaptiveOrchestrationResidue ||
            !handlers.getPlayer ||
            !handlers.clearBridgeAliases ||
            !handlers.setPlayerBleedImmune ||
            !handlers.resetBleedRuntimeState ||
            !handlers.clearLastAggressor ||
            !handlers.updatePreCombatState ||
            !handlers.resolveNoMarkerFallback ||
            !handlers.getBranchName ||
            !handlers.beginRescueTransition ||
            !handlers.forceLeftForDeadSolo ||
            !handlers.beginRecoverTransition) {
            return false;
        }

        const auto* why = reason ? reason : "noncaptive_fallback";
        if (!handlers.tryBeginTerminalCommit(TFD::Bleedout::TerminalCommit::NonCaptiveFallback, why)) {
            return false;
        }

        handlers.clearCaptiveOrchestrationResidue();

        auto* player = handlers.getPlayer();
        if (player && player->IsWeaponDrawn()) {
            player->DrawWeaponMagicHands(false);
        }

        const auto branch = handlers.resolveNoMarkerFallback(why);
        if (branch == TFD::Transition::FallbackBranch::None) {
            return false;
        }

        handlers.clearBridgeAliases(why);
        handlers.setPlayerBleedImmune(false);
        handlers.resetBleedRuntimeState();

        // R8: A non-captive fallback is a terminal handoff out of the old
        // Bleedout owner.  R2-R7 could execute RescueCached correctly, then
        // leave FlowController stuck at root=Bleedout/sub=BleedoutPleasure
        // from the previous bleedout dialogue.  That kept TFDDefeatState=2
        // and made post-defeat globals override Neutral even after Rescue
        // finished.  Clear only the native flow owner here; Transition/Rescue
        // still owns the rescue branch, Savior alias, calm window, and
        // TFDRescueState after beginRescueTransition() below.
        TFD::FlowController::Controller::GetSingleton().ResetRuntime("noncaptive_fallback_terminal_handoff");

        if (player && !player->IsDead() && !player->IsDisabled()) {
            player->NotifyAnimationGraph("BleedoutStart");
        }
        handlers.clearLastAggressor();
        handlers.updatePreCombatState();

        spdlog::info("[TFD][Flow][R8] committed no-marker fallback branch={} reason={} old_bleedout_owner_cleared=1",
            handlers.getBranchName(branch), why);

        if (branch == TFD::Transition::FallbackBranch::RescueCached) {
            if (!handlers.beginRescueTransition(reason ? reason : "rescue_cached")) {
                handlers.forceLeftForDeadSolo();
                handlers.beginRecoverTransition("rescue_cached_fallback_left_for_dead");
            }
            return true;
        }

        handlers.beginRecoverTransition(reason ? reason : handlers.getBranchName(branch));
        return true;
    }

    bool ApplyObservedDefeatResolution(const ObservedDefeatInput& input, std::string_view reason)
    {
        const auto resolution = EvaluateObservedDefeatResolution(input);
        const std::string reasonText = reason.empty() ? std::string{ "observed_defeat_resolution" } : std::string{ reason };
        const auto* why = reasonText.c_str();
        spdlog::info(
            "[TFD][Flow] observed defeat input reason={} resolved={} hadEnemy={} playerSide={} teammate={} hostile={} marker={} fallback={} forceCaptive={} actor={:08X}",
            why,
            input.conflictResolved ? 1 : 0,
            input.hadValidObservedEnemy ? 1 : 0,
            input.hasStandingPlayerSide ? 1 : 0,
            input.hasStandingTeammate ? 1 : 0,
            input.hasStandingHostileCoalition ? 1 : 0,
            input.hasCaptiveMarker ? 1 : 0,
            input.canUseCaptiveFallback ? 1 : 0,
            input.forceCaptive ? 1 : 0,
            input.actorFormID);
        switch (resolution) {
        case ObservedDefeatResolution::ContinueObserve:
            spdlog::info("[TFD][Flow] observed defeat decision=continue reason={}", why);
            return false;
        case ObservedDefeatResolution::NonCaptiveChoice:
            HandleObservedBattleWin(why);
            return true;
        case ObservedDefeatResolution::LeftForDead:
            HandleObservedLeftForDead(why);
            return true;
        case ObservedDefeatResolution::Captive: {
            auto actorFormID = input.actorFormID != 0 ? input.actorFormID : ResolveBleedFlowActorFormIDFromProviders();
            const bool ok = Controller::GetSingleton().RequestCaptive(actorFormID, CaptiveMode::Kidnapped, why);
            if (!ok) {
                spdlog::warn("[TFD][Flow] observed defeat decision=captive reject actor={:08X} reason={}", actorFormID, why);
            }
            else {
                spdlog::info("[TFD][Flow] observed defeat decision=captive actor={:08X} reason={}", actorFormID, why);
            }
            return ok;
        }
        case ObservedDefeatResolution::None:
        default:
            break;
        }
        return false;
    }

    void HandleObservedBattleWin(const char* reason)
    {
        const auto* why = reason ? reason : "battle_observe_win";
        if (g_battleObserverRuntimeProviders.clearBridgeAliases) {
            g_battleObserverRuntimeProviders.clearBridgeAliases(why);
        }
        if (g_battleObserverRuntimeProviders.clearCaptiveResidue) {
            g_battleObserverRuntimeProviders.clearCaptiveResidue();
        }
        if (g_battleObserverRuntimeProviders.clearAllFactions) {
            g_battleObserverRuntimeProviders.clearAllFactions();
        }
        if (g_battleObserverRuntimeProviders.resetBleedRuntimeState) {
            g_battleObserverRuntimeProviders.resetBleedRuntimeState();
        }
        if (g_battleObserverRuntimeProviders.setPlayerBleedImmune) {
            g_battleObserverRuntimeProviders.setPlayerBleedImmune(false);
        }

        bool fallbackCommitted = false;
        if (g_battleObserverRuntimeProviders.beginResolvedNoMarkerFallback) {
            fallbackCommitted = g_battleObserverRuntimeProviders.beginResolvedNoMarkerFallback(why);
        }
        else {
            fallbackCommitted = TFD::Transition::DefeatGlue::BeginResolvedNoMarkerFallback(why);
        }
        bool queuedLegacyChoice = false;
        if (!fallbackCommitted && g_battleObserverRuntimeProviders.queueNonCaptiveChoice) {
            g_battleObserverRuntimeProviders.queueNonCaptiveChoice(why);
            queuedLegacyChoice = true;
        }

        const bool settleQueued = QueueBridgeModEvent("TFDCombatBehaviorSettle", nullptr, why, 1.0f);

        spdlog::info(
            "[TFD][Flow][CB10] observed battle resolved -> recovery fallback committed={} queuedLegacyChoice={} settleQueued={} reason={}",
            fallbackCommitted ? 1 : 0,
            queuedLegacyChoice ? 1 : 0,
            settleQueued ? 1 : 0,
            why);
    }

    void HandleObservedLeftForDead(const char* reason)
    {
        const auto* why = reason ? reason : "battle_observe_loss";
        RE::Actor* follower = nullptr;
        if (g_battleObserverRuntimeProviders.resolveObservedDownedFollower) {
            follower = g_battleObserverRuntimeProviders.resolveObservedDownedFollower();
        }
        if (g_battleObserverRuntimeProviders.armObservedLeftForDeadFallback) {
            g_battleObserverRuntimeProviders.armObservedLeftForDeadFallback(follower);
        }
        if (g_battleObserverRuntimeProviders.clearBridgeAliases) {
            g_battleObserverRuntimeProviders.clearBridgeAliases(why);
        }
        if (g_battleObserverRuntimeProviders.clearCaptiveResidue) {
            g_battleObserverRuntimeProviders.clearCaptiveResidue();
        }
        if (g_battleObserverRuntimeProviders.clearAllFactions) {
            g_battleObserverRuntimeProviders.clearAllFactions();
        }
        if (g_battleObserverRuntimeProviders.resetBleedRuntimeState) {
            g_battleObserverRuntimeProviders.resetBleedRuntimeState();
        }
        if (g_battleObserverRuntimeProviders.setPlayerBleedImmune) {
            g_battleObserverRuntimeProviders.setPlayerBleedImmune(false);
        }
        if (g_battleObserverRuntimeProviders.beginRecoverTransition) {
            g_battleObserverRuntimeProviders.beginRecoverTransition(why);
        }
        const char* branch = "unknown";
        if (g_battleObserverRuntimeProviders.getCurrentFallbackBranchName) {
            if (const auto* resolved = g_battleObserverRuntimeProviders.getCurrentFallbackBranchName()) {
                branch = resolved;
            }
        }
        spdlog::info("[TFD][Flow] observed battle resolved -> left for dead reason={} branch={}", why, branch);
    }

    bool EnsureCaptiveRootForOutcome(Controller& flow, std::uint32_t actorFormID, std::string_view reason)
    {
        if (flow.GetSnapshot().root == RootFlow::Captive) {
            return true;
        }

        if (TFD::Captive::IsActive()) {
            return flow.RequestCaptive(actorFormID, CaptiveMode::Kidnapped, reason.empty() ? std::string_view{ "mod_event_captive_recover" } : reason);
        }

        return false;
    }

    // Generic Captive guards retained.
    bool IsCaptiveEscapeGuardActive(TFD::FlowController::Controller& flow)
    {
        if (TFD::Captive::IsEscapeActive() || TFD::Captive::HasEscapeBreakRebleedPending() || TFD::Captive::IsEscapeBleedoutActive()) {
            return true;
        }

        if (flow.IsCaptiveEscapeBreakContextActive()) {
            return true;
        }

        const auto snapshot = flow.GetSnapshot();
        return snapshot.root == RootFlow::Captive &&
            (snapshot.sub == SubFlow::EscapeAttempt ||
                snapshot.sub == SubFlow::EscapeFailed ||
                snapshot.sub == SubFlow::Recapture);
    }

    bool IsCaptiveOwnedPleasureFailure(
        TFD::FlowController::Controller& flow,
        const TFD::FlowController::Snapshot& snapshot,
        int reportedSourceFlow)
    {
        if (reportedSourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::Captive)) {
            return true;
        }

        if (TFD::PleasureRuntime::GetSourceContext() == TFD::PleasureRuntime::SourceContext::Captive) {
            return true;
        }

        if (IsCaptiveEscapeGuardActive(flow)) {
            return true;
        }

        if (snapshot.root != RootFlow::Captive && snapshot.contextRoot != RootFlow::Captive) {
            return false;
        }

        switch (snapshot.sub) {
        case SubFlow::CaptivePleasure:
        case SubFlow::CaptiveAfterPleasure:
        case SubFlow::PleasureFailedDialogue:
        case SubFlow::EscapeAttempt:
        case SubFlow::EscapeFailed:
        case SubFlow::Recapture:
        case SubFlow::InCombatEscapeBreak:
        case SubFlow::BleedoutEscapeBreak:
            return true;
        default:
            return false;
        }
    }

    bool HandleCaptiveOutcomeModEvent(std::string_view name, std::string_view arg, RE::TESForm* sender)
    {
        if (name.rfind("TFDCaptiveOutcome", 0) != 0) {
            return false;
        }

        auto& flow = TFD::FlowController::Controller::GetSingleton();
        const auto actorFormID = ResolveActorFormIDFromEventArgOrSender(arg, sender);
        const auto senderFormID = sender ? sender->GetFormID() : 0u;

        bool ok = false;
        const char* reason = "mod_event_captive";

        if (name == kCaptiveOutcomeWorkEvent) {
            reason = "mod_event_captive_work";
            ok = EnsureCaptiveRootForOutcome(flow, actorFormID, reason) &&
                flow.RequestResolveCaptiveOutcome(CaptiveOutcome::WorkForEnemy, actorFormID, reason);
            if (ok) {
                TFD::Captive::SetRuntimeState(true, TFD::Captive::PhaseValue::ReleasedWork);
                TFD::Captive::BeginReleasedWorkRuntime(RE::TESForm::LookupByID<RE::Actor>(actorFormID), reason);
                (void)TFD::Location::RefreshCaptiveWorkResourceState(true, reason);
                TFD::Captive::SyncCaptiveWorkResourceAliases(reason);
            }
        }
        else if (name == kCaptiveOutcomeReturnEvent) {
            reason = "mod_event_captive_return";

            // Captive return can be selected from Work Pleasure AfterPleasure.
            // In that route Papyrus bypasses the generic TFDAfterPleasureChoiceFinish
            // event so the native PleasureRuntime must be closed here, otherwise
            // the primary hotkey sees PleasureRuntime::IsActive() and reports
            // "TFD: Busy" after the player has already returned to Captive.
            TFD::PleasureRuntime::Break(reason, true, true, true);
            (void)TFD::FlowController::QueueBridgeModEvent(
                "TFDSystemEventClearAfterPleasure",
                nullptr,
                reason,
                1.0f);

            if (IsCaptiveEscapeGuardActive(flow)) {
                TFD::Captive::ClearReleasedWorkRuntime("captive_return_ignored_escape_active");
                TFD::Location::ClearCaptiveWorkResourceState("captive_return_ignored_escape_active");
                spdlog::info(
                    "[TFD][Flow] captive return ignored because escape is active actor={:08X} sender={:08X} reason={}",
                    actorFormID,
                    senderFormID,
                    reason);
                return true;
            }

            TFD::Captive::BeginReturnToCaptiveTransitionGuard(reason, 4.0);
            ok = flow.RequestCaptive(actorFormID, CaptiveMode::Kidnapped, reason);
            if (ok) {
                TFD::Captive::SetRuntimeState(true, TFD::Captive::PhaseValue::Captive);
                TFD::Captive::ClearReleasedWorkRuntime(reason);
                TFD::Location::ClearCaptiveWorkResourceState(reason);
                if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                    TFD::Captive::SyncPlayerAlias(player, reason);
                }

                const std::string returnReason{ reason };
                const auto queueReturnTransition = [actorFormID, returnReason]() {
                    auto finishReturnGuard = [&returnReason]() {
                        TFD::Captive::EndReturnToCaptiveTransitionGuard(returnReason.c_str());
                        };

                    auto& queuedFlow = TFD::FlowController::Controller::GetSingleton();
                    if (IsCaptiveEscapeGuardActive(queuedFlow)) {
                        spdlog::info(
                            "[TFD][Flow] captive return transition aborted actor={:08X} reason=escape_active source={}",
                            actorFormID,
                            returnReason);
                        finishReturnGuard();
                        return;
                    }

                    auto runtimeHandlers = TFD::Transition::DefeatGlue::BuildTransitionRuntimeHandlers();
                    auto captiveHandlers = TFD::Transition::DefeatGlue::BuildTransitionCaptiveHandlers();

                    if (IsCaptiveEscapeGuardActive(queuedFlow)) {
                        spdlog::info(
                            "[TFD][Flow] captive return transition aborted actor={:08X} reason=escape_active_after_handlers source={}",
                            actorFormID,
                            returnReason);
                        finishReturnGuard();
                        return;
                    }

                    if (!TFD::Transition::ResolveCaptiveMarkerForOutcome(runtimeHandlers)) {
                        spdlog::warn(
                            "[TFD][Flow] captive return transition rejected actor={:08X} reason=no_captive_marker source={}",
                            actorFormID,
                            returnReason);
                        finishReturnGuard();
                        return;
                    }

                    if (IsCaptiveEscapeGuardActive(queuedFlow)) {
                        spdlog::info(
                            "[TFD][Flow] captive return transition aborted actor={:08X} reason=escape_active_before_transition source={}",
                            actorFormID,
                            returnReason);
                        finishReturnGuard();
                        return;
                    }

                    const bool transitioned = TFD::Transition::CompleteCaptiveTransitionNow(returnReason.c_str(), runtimeHandlers, captiveHandlers);
                    if (transitioned) {
                        if (IsCaptiveEscapeGuardActive(queuedFlow)) {
                            spdlog::info(
                                "[TFD][Flow] captive return post-transition captive restore skipped actor={:08X} reason=escape_active source={}",
                                actorFormID,
                                returnReason);
                        }
                        else {
                            (void)TFD::FlowController::Controller::GetSingleton().RequestCaptive(
                                actorFormID,
                                TFD::FlowController::CaptiveMode::Kidnapped,
                                returnReason);
                            TFD::Captive::SetRuntimeState(true, TFD::Captive::PhaseValue::Captive);
                        }
                    }

                    finishReturnGuard();
                    spdlog::info(
                        "[TFD][Flow] captive return transition actor={:08X} completed={} reason={}",
                        actorFormID,
                        transitioned ? 1 : 0,
                        returnReason);
                    };

                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask(queueReturnTransition);
                }
                else {
                    queueReturnTransition();
                }
            }
            else {
                TFD::Captive::EndReturnToCaptiveTransitionGuard(reason);
            }
        }
        else if (name == kCaptiveOutcomeReturnNoTransitionEvent) {
            reason = "mod_event_captive_return_no_transition";

            // Captive no-transition return is used by Calling Captor/Nevermind,
            // Captive Pleasure reject/redo reject, and phase-4 Scene -> Captive
            // restores.  It must re-arm native CaptiveIdle/Calling Captor without
            // replaying the full captive transition, confiscation, teleport, or
            // stash cycle owned by TFDCaptiveOutcomeReturn.
            TFD::PleasureRuntime::Break(reason, true, true, true);
            (void)TFD::FlowController::QueueBridgeModEvent(
                "TFDSystemEventClearAfterPleasure",
                nullptr,
                reason,
                1.0f);

            if (IsCaptiveEscapeGuardActive(flow)) {
                TFD::Captive::ClearReleasedWorkRuntime("captive_return_no_transition_ignored_escape_active");
                TFD::Location::ClearCaptiveWorkResourceState("captive_return_no_transition_ignored_escape_active");
                spdlog::info(
                    "[TFD][Flow] captive no-transition return ignored because escape is active actor={:08X} sender={:08X} reason={}",
                    actorFormID,
                    senderFormID,
                    reason);
                return true;
            }

            ok = flow.RequestCaptive(actorFormID, CaptiveMode::Kidnapped, reason);
            if (ok) {
                TFD::Captive::SetRuntimeState(true, TFD::Captive::PhaseValue::Captive);
                TFD::Captive::ClearReleasedWorkRuntime(reason);
                TFD::Location::ClearCaptiveWorkResourceState(reason);
                if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                    TFD::Captive::SyncPlayerAlias(player, reason);
                    TFD::Captive::ArmEscapeContextFromCurrentState(player);
                    TFD::Captive::ResetLockpickWatch();
                    TFD::Captive::SealDoorIfPresent();
                }
                TFD::HostilityController::TickCaptiveSuppression();
                TFD::InteractionRouter::ClearInteractionStateValue();
            }
        }
        else if (name == kCaptiveOutcomeStayInPrisonIdleEvent) {
            reason = "mod_event_captive_stay_in_prison_idle";

            // R157: Stay in Prison is a local captive-idle terminal for the
            // Captive PleasureFailed dialogue.  It must finish the native
            // CaptiveAfterPleasure subflow so Calling Captor can be used again,
            // but it must not replay the heavier no-transition return path that
            // re-arms aliases/packages/door/escape context and causes visible
            // refresh/blink at the captive marker.
            TFD::PleasureRuntime::Break(reason, true, true, true);
            (void)TFD::FlowController::QueueBridgeModEvent(
                "TFDSystemEventClearAfterPleasure",
                nullptr,
                reason,
                1.0f);

            const auto before = flow.GetSnapshot();
            ok = flow.RequestCompleteAfterPleasure(reason);
            if (!ok && before.root == RootFlow::Captive) {
                const auto mode = before.captiveMode != CaptiveMode::None ? before.captiveMode : CaptiveMode::Kidnapped;
                ok = flow.RequestCaptive(actorFormID, mode, reason);
            }

            if (ok) {
                TFD::Captive::SetRuntimeState(true, TFD::Captive::PhaseValue::Captive);
                TFD::Captive::ClearReleasedWorkRuntime(reason);
                TFD::Location::ClearCaptiveWorkResourceState(reason);
                TFD::InteractionRouter::ClearInteractionStateValue();
            }

            const auto after = flow.GetSnapshot();
            spdlog::info(
                "[TFD][Flow][R157] stay in prison idle complete actor={:08X} sender={:08X} ok={} beforeRoot={} beforeSub={} afterRoot={} afterSub={} reason={}",
                actorFormID,
                senderFormID,
                ok ? 1 : 0,
                Controller::ToString(before.root),
                Controller::ToString(before.sub),
                Controller::ToString(after.root),
                Controller::ToString(after.sub),
                reason);
        }
        else if (name == kCaptiveOutcomeReleaseEvent) {
            reason = "mod_event_captive_release";
            ok = EnsureCaptiveRootForOutcome(flow, actorFormID, reason) &&
                flow.RequestResolveCaptiveOutcome(CaptiveOutcome::Release, actorFormID, reason);
            TFD::Captive::SetRuntimeState(false, TFD::Captive::PhaseValue::None);
            TFD::Captive::ClearReleasedWorkRuntime(reason);
            TFD::Location::ClearCaptiveWorkResourceState(reason);
            TFD::HostilityController::ClearAggressionClamp();
            TFD::Actor::Ops::ClearAggressorFactionContext();
            if (ok) {
                (void)flow.RequestCompleteTerminalContext("mod_event_captive_release_complete");
            }
            else {
                flow.ResetRuntime("mod_event_captive_release_force_clear");
            }
        }
        else if (name == kCaptiveOutcomeCancelEvent) {
            reason = "mod_event_captive_cancel";
            ok = EnsureCaptiveRootForOutcome(flow, actorFormID, reason) &&
                flow.RequestResolveCaptiveOutcome(CaptiveOutcome::Cancel, actorFormID, reason);
            TFD::Captive::SetRuntimeState(false, TFD::Captive::PhaseValue::None);
            TFD::Captive::ClearReleasedWorkRuntime(reason);
            TFD::Location::ClearCaptiveWorkResourceState(reason);
            TFD::HostilityController::ClearAggressionClamp();
            TFD::Actor::Ops::ClearAggressorFactionContext();
            if (ok) {
                (void)flow.RequestCompleteTerminalContext("mod_event_captive_cancel_complete");
            }
            else {
                flow.ResetRuntime("mod_event_captive_cancel_force_clear");
            }
        }
        else if (name == kCaptiveOutcomeEscapeEvent) {
            reason = "mod_event_captive_escape";

            // R169: During Captive escape-failed rebleed, Bleedout Forcegreet is a
            // mandatory choice gate.  Papyrus captive escape updates can still fire
            // while the Bleedout dialogue is open or while R168 is waiting to reopen
            // it.  Those stale escape events must not move the flow back to
            // Captive/EscapeAttempt, otherwise BleedoutGreet::Begin rejects the forced
            // reopen with flowOwnerMismatch=1 and the player is left without a choice.
            const auto beforeEscape = flow.GetSnapshot();
            const bool captiveBleedoutChoiceGate =
                beforeEscape.root == RootFlow::Captive &&
                beforeEscape.gate == DecisionGate::PlayerBleedout &&
                (beforeEscape.sub == SubFlow::EscapeFailed || beforeEscape.sub == SubFlow::Recapture);
            const bool staleEscapeDuringBleedoutChoice =
                captiveBleedoutChoiceGate ||
                (TFD::Bleedout::OwnsCurrentFlow() &&
                    TFD::BleedoutGreet::HasSeenDialogue() &&
                    !TFD::Bleedout::HasTerminalCommit());

            if (staleEscapeDuringBleedoutChoice) {
                ok = true;
                TFD::Bleedout::ClearSystemEventOutcomeWindow("r169_ignore_captive_escape_during_bleedout_choice");
                if (!TFD::InteractionRouter::DialogueOpen::IsActive()) {
                    TFD::BleedoutGreet::MarkStickyReopenPending(true, "r169_ignore_captive_escape_during_bleedout_choice");
                }
                spdlog::info(
                    "[TFD][Flow][R169] captive escape event ignored during bleedout choice actor={:08X} sender={:08X} root={} gate={} sub={} sticky={} seenDialogue={} terminal={}",
                    actorFormID,
                    senderFormID,
                    Controller::ToString(beforeEscape.root),
                    Controller::ToString(beforeEscape.gate),
                    Controller::ToString(beforeEscape.sub),
                    TFD::BleedoutGreet::HasStickyReopenPending() ? 1 : 0,
                    TFD::BleedoutGreet::HasSeenDialogue() ? 1 : 0,
                    TFD::Bleedout::HasTerminalCommit() ? 1 : 0);
            }
            else {
                ok = EnsureCaptiveRootForOutcome(flow, actorFormID, reason) &&
                    flow.RequestResolveCaptiveOutcome(CaptiveOutcome::EscapeStarted, actorFormID, reason);
                if (ok) {
                    TFD::Captive::SetRuntimeState(true, TFD::Captive::PhaseValue::Escape);
                    TFD::Captive::ClearReleasedWorkRuntime(reason);
                    TFD::Location::ClearCaptiveWorkResourceState(reason);

                    auto* player = RE::PlayerCharacter::GetSingleton();
                    auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
                    (void)TFD::HostilityController::BreakCaptivePassiveForCombat(
                        player,
                        actor,
                        TFD::HostilityController::ReleaseReason::FightChoice,
                        reason,
                        true,
                        true);
                }
            }
        }
        else if (name == kCaptiveOutcomePleasureEvent) {
            reason = "mod_event_captive_pleasure";
            ok = EnsureCaptiveRootForOutcome(flow, actorFormID, reason) &&
                flow.RequestResolveCaptiveOutcome(CaptiveOutcome::Pleasure, actorFormID, reason);
            if (ok) {
                TFD::Captive::SetRuntimeState(true, TFD::Captive::PhaseValue::Scene);
                TFD::Captive::ClearReleasedWorkRuntime(reason);
                TFD::Location::ClearCaptiveWorkResourceState(reason);
                TFD::HostilityController::TickCaptiveSuppression();
            }
        }
        else {
            return false;
        }

        if (ok) {
            TFD::Captive::ClearCaptorCallCooldown();
        }

        spdlog::info(
            "[TFD][Flow] captive outcome event={} actor={:08X} sender={:08X} ok={} reason={}",
            std::string(name),
            actorFormID,
            senderFormID,
            ok ? 1 : 0,
            reason);
        return true;
    }

    static void QueueInCombatCaptiveTransition(std::uint32_t actorFormID, const char* reason)
    {
        if (actorFormID == 0) {
            spdlog::warn("[TFD][Flow][R93U] incombat captive transition skipped reason=no_actor source={}",
                reason && reason[0] ? reason : "mod_event_incombat_captive");
            return;
        }

        const std::string why = reason && reason[0] ? reason : "mod_event_incombat_captive";
        auto runTransition = [actorFormID, why]() {
            auto runtimeHandlers = TFD::Transition::DefeatGlue::BuildTransitionRuntimeHandlers();
            auto captiveHandlers = TFD::Transition::DefeatGlue::BuildTransitionCaptiveHandlers();

            if (!TFD::Transition::ResolveCaptiveMarkerForOutcome(runtimeHandlers)) {
                spdlog::warn(
                    "[TFD][Flow][R93U] incombat captive transition rejected actor={:08X} reason=no_captive_marker source={}",
                    actorFormID,
                    why);
                return;
            }

            const bool transitioned = TFD::Transition::CompleteCaptiveTransitionNow(why.c_str(), runtimeHandlers, captiveHandlers);
            if (transitioned) {
                (void)TFD::FlowController::Controller::GetSingleton().RequestCaptive(
                    actorFormID,
                    TFD::FlowController::CaptiveMode::Kidnapped,
                    why);
                TFD::Captive::SetRuntimeState(true, TFD::Captive::PhaseValue::Captive);
                if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                    TFD::Captive::SyncPlayerAlias(player, why.c_str());
                }
            }

            spdlog::info(
                "[TFD][Flow][R93U] incombat captive transition actor={:08X} completed={} reason={}",
                actorFormID,
                transitioned ? 1 : 0,
                why);
            };

        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask(runTransition);
        }
        else {
            runTransition();
        }
    }

    bool HandleOutcomeModEvent(const char* eventName, const char* strArg, float numArg, RE::TESForm* sender)
    {
        if (!eventName || !eventName[0]) {
            return false;
        }

        const std::string_view name{ eventName };
        const std::string_view arg = strArg ? std::string_view{ strArg } : std::string_view{};

        if (name == kCaptiveWorkRefreshResourcesEvent) {
            const std::string reason = arg.empty() ? std::string{ "mod_event_work_refresh" } : std::string{ arg };
            const bool ok = TFD::Location::RefreshCaptiveWorkResourceState(false, reason.c_str());
            if (ok) {
                TFD::Captive::SyncCaptiveWorkResourceAliases(reason.c_str());
            }
            spdlog::info("[TFD][Flow] captive work resource refresh event ok={} reason={}", ok ? 1 : 0, reason);
            return true;
        }

        if (name == kCaptiveWorkMiningCompletedEvent) {
            const std::string reason = arg.empty() ? std::string{ "mod_event_work_mining_completed" } : std::string{ arg };
            const bool marked = TFD::Location::MarkLastCaptiveWorkMiningRefDepleted(reason.c_str());
            const bool refreshed = TFD::Location::RefreshCaptiveWorkResourceState(true, reason.c_str());
            if (refreshed) {
                TFD::Captive::SyncCaptiveWorkResourceAliases(reason.c_str());
            }
            spdlog::info(
                "[TFD][Flow] captive work mining completed event marked={} refreshed={} reason={}",
                marked ? 1 : 0,
                refreshed ? 1 : 0,
                reason);
            return true;
        }

        if (name == kCaptiveWorkNoJobEvent) {
            auto* actor = ResolveActorFromEventArg(arg);
            const double cooldownSec = numArg > 0.0f ? static_cast<double>(numArg) : 10.0;
            TFD::Captive::HandleReleasedWorkNoJob(actor, cooldownSec, "mod_event_work_no_job");
            spdlog::info("[TFD][Flow] captive work no-job event actor={:08X} cooldown={:.2f}",
                actor ? actor->GetFormID() : 0u,
                cooldownSec);
            return true;
        }

        if (name == kInCombatPleasureRejectCycleOpenEvent) {
            auto* actor = sender ? sender->As<RE::Actor>() : nullptr;
            if (!actor) {
                actor = ResolveActorFromEventArg(arg);
            }

            TFD::InteractionRouter::Action action = TFD::InteractionRouter::Action::None;
            bool ok = false;
            const char* result = "no_actor";

            if (actor && (actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded())) {
                result = "invalid_actor";
            }
            else if (actor) {
                // R475A: InCombat reject-cycle reopen is not the same as a normal
                // hotkey truce. The candidate was selected from the active TFD
                // crowd/desire pool, and may be intentionally pacified/StopCombat'ed
                // while AfterPleasure protects the player. Do not require a live
                // combat target to the player side for this handoff.
                ok = TFD::InCombatGreet::BeginForPleasureCycleActor(actor, &action, true);
                result = ok ? "begin_ok" : "begin_failed";
            }

            spdlog::info(
                "[TFD][Flow][R475A] incombat pleasure reject native reopen actor={:08X} ok={} action={} sourceFlow={:.0f} reason={} result={} allowPacifiedBridge=1",
                actor ? actor->GetFormID() : 0u,
                ok ? 1 : 0,
                TFD::InteractionRouter::ToString(action),
                static_cast<double>(numArg),
                arg.empty() ? std::string{ "-" } : std::string{ arg },
                result);
            return true;
        }

        if (name == "TFDPreCombatRootGreetRejected") {
            auto* actor = ResolveActorFromEventArg(arg);
            const double cooldownSec = numArg > 0.0f ? static_cast<double>(numArg) : 5.0;

            if (actor) {
                TFD::InteractionRouter::DialogueOpen::ArmTemporaryDialogueCooldown(
                    actor,
                    cooldownSec,
                    "TFDPreCombatRootGreetRejected");
            }

            // Root-greet reentry rejection must not hide DialogueMenu.
            // During PreCombat Pay -> Recruit/Follow/Release followup, the active
            // DialogueMenu can already be the valid followup menu. Forcing kHide
            // here causes the reopen-then-close symptom and can leave Skyrim's
            // camera/mouse input in a bad menu-transition state.
            spdlog::info(
                "[TFD][Flow] root greet rejected event actor={:08X} cooldown={:.2f}s forceClose=0 policy=cooldown_only",
                actor ? actor->GetFormID() : 0u,
                cooldownSec);
            return true;
        }

        if (name == "TFDTemporaryFollowExit") {
            auto* actor = ResolveActorFromEventArg(arg);
            const double cooldownSec = numArg > 0.0f ? static_cast<double>(numArg) : 12.0;
            bool lockRemoved = false;
            bool graceRemoved = false;
            bool truceReleased = false;
            bool flowReset = false;

            if (actor) {
                TFD::Actor::Ops::RemoveTemporaryFollowLockFromActorOnly(actor, "temporary_follow_exit");
                lockRemoved = true;

                if (g_outcomeRuntimeProviders.removeReleaseFollowGrace) {
                    g_outcomeRuntimeProviders.removeReleaseFollowGrace(actor, "temporary_follow_exit");
                    graceRemoved = true;
                }

                TFD::InteractionRouter::DialogueOpen::ArmTemporaryDialogueCooldown(
                    actor,
                    cooldownSec,
                    "temporary_follow_exit");

                truceReleased = TFD::HostilityController::ReleaseActiveTruceSessionForActor(
                    actor,
                    TFD::HostilityController::ReleaseReason::FlowHandoff,
                    false);
            }

            auto& flow = TFD::FlowController::Controller::GetSingleton();
            const auto snapshot = flow.GetSnapshot();
            const std::uint32_t actorFormID = actor ? actor->GetFormID() : 0u;
            const bool actorMatches = actorFormID == 0u ||
                snapshot.primaryActorFormID == 0u ||
                snapshot.primaryActorFormID == actorFormID;
            const bool captiveContext =
                snapshot.root == RootFlow::Captive ||
                snapshot.contextRoot == RootFlow::Captive ||
                snapshot.sub == SubFlow::CaptiveIdle ||
                snapshot.sub == SubFlow::CaptivePleasure ||
                snapshot.sub == SubFlow::CaptiveAfterPleasure ||
                snapshot.sub == SubFlow::WorkForEnemy ||
                snapshot.sub == SubFlow::EscapeAttempt ||
                snapshot.sub == SubFlow::EscapeFailed ||
                snapshot.sub == SubFlow::Recapture ||
                snapshot.sub == SubFlow::InCombatEscapeBreak ||
                snapshot.sub == SubFlow::BleedoutEscapeBreak ||
                snapshot.sub == SubFlow::JoinedEnemyIdle;
            const bool transientOwner =
                snapshot.root == RootFlow::PreCombat ||
                snapshot.contextRoot == RootFlow::PreCombat ||
                snapshot.root == RootFlow::InCombat ||
                snapshot.contextRoot == RootFlow::InCombat ||
                snapshot.sub == SubFlow::PreCombatPayFollowup ||
                snapshot.sub == SubFlow::InCombatPayFollowup ||
                snapshot.sub == SubFlow::PreCombatPleasure ||
                snapshot.sub == SubFlow::InCombatPleasure ||
                snapshot.sub == SubFlow::PreCombatAfterPleasure ||
                snapshot.sub == SubFlow::InCombatAfterPleasure;

            if (!captiveContext && transientOwner && actorMatches) {
                flow.ResetRuntime("temporary_follow_exit_contract");
                flowReset = true;
            }

            TFD::InteractionRouter::ClearInteractionStateValue();

            spdlog::info(
                "[TFD][Flow][R152] temporary follow exit actor={:08X} cooldown={:.2f} lockRemoved={} graceRemoved={} truceReleased={} flowReset={} oldRoot={} oldCtx={} oldSub={} oldPrimary={:08X} captiveContext={} arg={}",
                actorFormID,
                cooldownSec,
                lockRemoved ? 1 : 0,
                graceRemoved ? 1 : 0,
                truceReleased ? 1 : 0,
                flowReset ? 1 : 0,
                Controller::ToString(snapshot.root),
                Controller::ToString(snapshot.contextRoot),
                Controller::ToString(snapshot.sub),
                snapshot.primaryActorFormID,
                captiveContext ? 1 : 0,
                arg.empty() ? std::string{ "-" } : std::string{ arg });
            return true;
        }

        if (name.rfind("TFDPreCombatOutcome", 0) == 0) {
            const auto snapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
            auto* actor = ResolveActorFromEventArg(arg);
            spdlog::info(
                "[TFD][Flow] precombat outcome callback event={} arg={} actor={:08X} sender={:08X} root={} sub={} primary={:08X} numArg={:.2f}",
                eventName,
                strArg ? strArg : "",
                actor ? actor->GetFormID() : 0u,
                sender ? sender->GetFormID() : 0u,
                Controller::ToString(snapshot.root),
                Controller::ToString(snapshot.sub),
                snapshot.primaryActorFormID,
                static_cast<double>(numArg));
        }

        const bool pleasureRuntimeRecognized = TFD::PleasureRuntime::HandleModEvent(
            eventName,
            strArg ? strArg : "",
            numArg,
            sender);
        const auto pleasureRuntimePhaseAfterEvent = TFD::PleasureRuntime::GetPhase();

        // R264A: Redo/Pleading from PleasureFailed is not a terminal AfterPleasure
        // choice, so it is intentionally excluded from
        // IsAfterPleasureTerminalChoiceEvent().  The old resume hook lived inside
        // that terminal-only block and therefore never ran.  If FlowController keeps
        // sub=PleasureFailedDialogue while PleasureRuntime already accepted Redo,
        // the next successful scene can reach TFDAfterPleasureEnter with
        // runtimeFailed=0 but flowFailed=1 and the AfterPleasure forcegreet is
        // dropped.  Resume the flow owner immediately when Redo is chosen.
        if (name == kAfterPleasureChoicePleasureEvent) {
            auto* actor = ResolveActorFromEventArg(arg);
            const int reportedSourceFlow = ResolveSourceFlowFromEventArg(arg);
            const auto runtimeSource = TFD::PleasureRuntime::GetSourceContext();
            const int effectiveSourceFlow = runtimeSource != TFD::PleasureRuntime::SourceContext::None
                ? static_cast<int>(runtimeSource)
                : reportedSourceFlow;
            auto& flow = TFD::FlowController::Controller::GetSingleton();
            const auto snapshot = flow.GetSnapshot();
            if (actor && snapshot.sub == SubFlow::PleasureFailedDialogue) {
                const bool resumed = flow.RequestResumePleasureFromPleasureFailed(
                    actor->GetFormID(),
                    effectiveSourceFlow,
                    "pleasure_failed_pleading_redo_r264");
                spdlog::info(
                    "[TFD][Flow][R264A] pleasure failed pleading redo resumed flow reportedSource={} effectiveSource={} runtimeSource={} actor={:08X} resumed={} oldRoot={} oldCtx={} oldSub={}",
                    reportedSourceFlow,
                    effectiveSourceFlow,
                    TFD::PleasureRuntime::GetSourceContextName(),
                    actor->GetFormID(),
                    resumed ? 1 : 0,
                    Controller::ToString(snapshot.root),
                    Controller::ToString(snapshot.contextRoot),
                    Controller::ToString(snapshot.sub));
                return true;
            }
        }

        if (IsAfterPleasureTerminalChoiceEvent(name)) {
            auto* actor = ResolveActorFromEventArg(arg);
            const int sourceFlow = ResolveSourceFlowFromEventArg(arg);
            auto& flow = TFD::FlowController::Controller::GetSingleton();
            const auto snapshot = flow.GetSnapshot();

            if (actor && name == kAfterPleasureChoicePleasureEvent && snapshot.sub == SubFlow::PleasureFailedDialogue) {
                const bool resumed = flow.RequestResumePleasureFromPleasureFailed(
                    actor->GetFormID(),
                    sourceFlow,
                    "pleasure_failed_pleading_redo");
                spdlog::info(
                    "[TFD][Flow][R223A] pleasure failed pleading resumed pleasure source={} actor={:08X} resumed={} oldRoot={} oldCtx={} oldSub={}",
                    sourceFlow,
                    actor->GetFormID(),
                    resumed ? 1 : 0,
                    Controller::ToString(snapshot.root),
                    Controller::ToString(snapshot.contextRoot),
                    Controller::ToString(snapshot.sub));
                return true;
            }


            if (actor && IsTeammateSource(sourceFlow)) {
                TFD::InteractionRouter::ClearInteractionStateValue();
                spdlog::info(
                    "[TFD][Flow][R321A] teammate pleasure special terminal event={} source={} actor={:08X} oldRoot={} oldCtx={} oldSub={} policy=no_afterpleasure_fg",
                    std::string(name),
                    sourceFlow,
                    actor->GetFormID(),
                    Controller::ToString(snapshot.root),
                    Controller::ToString(snapshot.contextRoot),
                    Controller::ToString(snapshot.sub));
                return true;
            }

            if (actor && IsInCombatSource(sourceFlow)) {
                const auto releaseReason = ResolveInCombatTerminalReleaseReason(name);
                const char* terminalReason = ResolveInCombatTerminalReason(name);
                const bool cycleQueued = TFD::PleasureRuntime::HasQueuedCycleForConsumedActor(
                    actor,
                    TFD::PleasureRuntime::SourceContext::InCombat);

                if (cycleQueued) {
                    bool releaseSingle = false;
                    bool demoteHold = false;
                    if (name == kAfterPleasureChoiceRecruitEvent) {
                        demoteHold = TFD::HostilityController::DemoteTruceActorForCycleHold(
                            actor,
                            "incombat_after_pleasure_recruit_cycle_hold");
                    }
                    else {
                        releaseSingle = TFD::HostilityController::ReleaseSingleTruceActorForCycle(
                            actor,
                            TFD::HostilityController::ReleaseReason::FlowHandoff,
                            terminalReason);
                    }
                    TFD::InCombat::Complete("incombat_after_pleasure_cycle_preserve_crowd");
                    spdlog::info(
                        "[TFD][Flow][R94G] incombat after pleasure terminal preserved cycle event={} source={} actor={:08X} cycleQueued=1 releaseSingle={} demoteHold={} releaseReason={} policy={}",
                        std::string(name),
                        sourceFlow,
                        actor->GetFormID(),
                        releaseSingle ? 1 : 0,
                        demoteHold ? 1 : 0,
                        TFD::HostilityController::ToString(TFD::HostilityController::ReleaseReason::FlowHandoff),
                        name == kAfterPleasureChoiceRecruitEvent ? "recruit_hold_until_chain_end" : "current_actor_only");
                    return true;
                }

                const bool completeFlow = flow.RequestCompleteAfterPleasure(terminalReason);
                // R95B: Finalize held InCombat-after-pleasure recruits before releasing the
                // truce session. If the session is released first, converted actors can be
                // caught by PlayerArmed/FightChoice rehostile cleanup and keep their old
                // bandit patrol package instead of receiving the teammate follow package.
                const auto flushedDeferred = TFD::PleasureRuntime::FlushDeferredInCombatRecruits("incombat_after_pleasure_terminal_final");
                const bool releaseTruce = TFD::HostilityController::ReleaseActiveTruceSessionForActor(
                    actor,
                    releaseReason,
                    false);
                TFD::InCombat::Complete(terminalReason);
                TFD::InteractionRouter::ClearInteractionStateValue();
                spdlog::info(
                    "[TFD][Flow][R94G] incombat after pleasure terminal event={} source={} actor={:08X} cycleQueued=0 flowComplete={} truceRelease={} deferredRegistered={} releaseReason={}",
                    std::string(name),
                    sourceFlow,
                    actor->GetFormID(),
                    completeFlow ? 1 : 0,
                    releaseTruce ? 1 : 0,
                    static_cast<unsigned int>(flushedDeferred),
                    TFD::HostilityController::ToString(releaseReason));
                return true;
            }

            const bool returnToCaptive =
                name == kAfterPleasureChoiceFinishEvent ||
                name == kAfterPleasureChoiceKidnapEvent;
            const bool sourceIsCaptive =
                sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::Captive) ||
                TFD::PleasureRuntime::GetSourceContext() == TFD::PleasureRuntime::SourceContext::Captive ||
                snapshot.root == RootFlow::Captive ||
                snapshot.contextRoot == RootFlow::Captive ||
                snapshot.sub == SubFlow::CaptivePleasure ||
                snapshot.sub == SubFlow::CaptiveAfterPleasure ||
                snapshot.sub == SubFlow::InCombatEscapeBreak ||
                snapshot.sub == SubFlow::BleedoutEscapeBreak ||
                TFD::Captive::IsEscapeBleedoutActive();

            // C33: Papyrus can report source=Bleedout for Captive escape-bleedout
            // AfterPleasure. The native pleasure runtime and flow snapshot still own
            // that chain as Captive, so resolve Captive return/recapture before the
            // generic Bleedout terminal branch can neutralize it.
            if (sourceIsCaptive && returnToCaptive) {
                auto* captiveActor = actor ? actor : TFD::PleasureRuntime::GetPrimarySpeaker();
                const char* terminalReason = name == kAfterPleasureChoiceKidnapEvent
                    ? "captive_after_pleasure_return"
                    : "captive_after_pleasure_finish";

                // R295A: In Captive AfterPleasure, Finish/Kidnap is a terminal
                // return-to-cell decision.  Restoring CaptiveIdle in place leaves
                // the player physically outside the cell/marker and the captive
                // escape marker-radius watcher immediately promotes the situation
                // into EscapeStarted, rehostilizing the captors.  Always commit the
                // native recapture transition here so the player is moved to the
                // cached CaptiveMarker and the escape overlay is closed by the
                // Captive owner.
                const bool recaptured = TFD::Captive::CommitRecapture(captiveActor, terminalReason);
                (void)TFD::FlowController::QueueBridgeModEvent(
                    "TFDSystemEventClearAfterPleasure",
                    nullptr,
                    "captive_after_pleasure_recapture",
                    1.0f);
                spdlog::info(
                    "[TFD][Flow][R295A] captive after pleasure terminal recapture event={} reportedSource={} runtimeSource={} actor={:08X} recaptured={} root={} ctx={} sub={} reason={}",
                    std::string(name),
                    sourceFlow,
                    TFD::PleasureRuntime::GetSourceContextName(),
                    captiveActor ? captiveActor->GetFormID() : 0u,
                    recaptured ? 1 : 0,
                    Controller::ToString(snapshot.root),
                    Controller::ToString(snapshot.contextRoot),
                    Controller::ToString(snapshot.sub),
                    terminalReason);
                return true;
            }

            if (actor && sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::Bleedout)) {
                const char* terminalReason = ResolveBleedoutAfterPleasureTerminalReason(name);

                const bool captiveBleedoutWorkHandoff =
                    name == kAfterPleasureChoiceWorkEvent &&
                    (snapshot.root == RootFlow::Captive ||
                        snapshot.contextRoot == RootFlow::Captive ||
                        TFD::Captive::IsActive());

                if (captiveBleedoutWorkHandoff) {
                    const bool completeFlow = flow.RequestCompleteAfterPleasure("captive_bleedout_after_pleasure_work_handoff");

                    // Returning from Bleedout AfterPleasure to Work must rebuild the native
                    // ReleasedWork runtime, not only the FlowController sub-state.  The Work
                    // marker/dialogue route is owned by Captive::g_currentWorkBossFormID; if
                    // this is left cleared after Escape/PleasureFailed cleanup, the objective
                    // can display without a valid Boss target and every activation is rejected.
                    TFD::Captive::SetRuntimeState(true, TFD::Captive::PhaseValue::ReleasedWork);
                    TFD::Captive::BeginReleasedWorkRuntime(actor, "captive_bleedout_after_pleasure_work_handoff");
                    ScrubCaptiveReleasedWorkDialogueGlobals("captive_bleedout_after_pleasure_work_handoff");

                    (void)TFD::FlowController::QueueBridgeModEvent(
                        "TFDSystemEventClearAfterPleasure",
                        nullptr,
                        "captive_bleedout_after_pleasure_work_handoff",
                        1.0f);

                    spdlog::info(
                        "[TFD][Flow] captive bleedout after pleasure work handoff event={} source={} actor={:08X} complete={} nativeReleasedWork=1 root={} ctx={} sub={} reason={}",
                        std::string(name),
                        sourceFlow,
                        actor->GetFormID(),
                        completeFlow ? 1 : 0,
                        Controller::ToString(snapshot.root),
                        Controller::ToString(snapshot.contextRoot),
                        Controller::ToString(snapshot.sub),
                        terminalReason);
                    return true;
                }

                const bool cycleQueued = TFD::PleasureRuntime::HasQueuedCycleForConsumedActor(
                    actor,
                    TFD::PleasureRuntime::SourceContext::Bleedout);

                if (cycleQueued) {
                    bool releaseSingle = false;
                    bool demoteHold = false;
                    bool earlyBleedoutComplete = false;
                    if (name == kAfterPleasureChoiceRecruitEvent) {
                        // R471A: Recruit->cycle bridge is a two-owner handoff.  Do not call
                        // Bleedout::CompleteAfterPleasure here: that clears Bleedout crowd
                        // pacify before InCombatGreet has opened, producing a hostile gap.
                        // PleasureRuntime finalizes the Bleedout suppress owner only after
                        // BeginForPleasureCycleActor succeeds.
                        demoteHold = TFD::HostilityController::DemoteTruceActorForCycleHold(
                            actor,
                            "bleedout_after_pleasure_recruit_cycle_hold_deferred");
                        TFD::InteractionRouter::ClearInteractionStateValue();
                    }
                    else {
                        releaseSingle = TFD::HostilityController::ReleaseSingleTruceActorForCycle(
                            actor,
                            TFD::HostilityController::ReleaseReason::FlowHandoff,
                            terminalReason);
                        earlyBleedoutComplete = TFD::Bleedout::CompleteAfterPleasure("bleedout_after_pleasure_cycle_preserve_crowd");
                    }

                    // R130/R471A: Bleedout follows the InCombat cycle pattern.  For Recruit,
                    // the old Bleedout owner is kept alive until the next InCombat speaker
                    // confirms.  For non-Recruit terminal choices, current actor cleanup is
                    // still local and can complete immediately.
                    spdlog::info(
                        "[TFD][Flow][R471A] bleedout after pleasure terminal preserved cycle event={} source={} actor={:08X} cycleQueued=1 releaseSingle={} demoteHold={} earlyComplete={} policy={}",
                        std::string(name),
                        sourceFlow,
                        actor->GetFormID(),
                        releaseSingle ? 1 : 0,
                        demoteHold ? 1 : 0,
                        earlyBleedoutComplete ? 1 : 0,
                        name == kAfterPleasureChoiceRecruitEvent ? "recruit_defer_bleedout_cleanup_until_incombat_success" : "current_actor_only");
                    return true;
                }

                const auto flushedDeferred = TFD::PleasureRuntime::FlushDeferredInCombatRecruits("bleedout_after_pleasure_terminal_final");
                const bool completeFlow = TFD::Bleedout::CompletePleasureCycleChainNeutral(terminalReason);
                TFD::InteractionRouter::ClearInteractionStateValue();
                spdlog::info(
                    "[TFD][Flow][R133] bleedout after pleasure terminal event={} source={} actor={:08X} cycleQueued=0 flowComplete={} deferredRegistered={} reason={}",
                    std::string(name),
                    sourceFlow,
                    actor->GetFormID(),
                    completeFlow ? 1 : 0,
                    static_cast<unsigned int>(flushedDeferred),
                    terminalReason);
                return true;
            }
        }


        if (HandleCaptiveOutcomeModEvent(name, arg, sender)) {
            return true;
        }

        if (name == kAfterPleasureForceOpenEvent) {
            auto* actor = ResolveActorFromEventArg(arg);
            const int sourceFlow = ResolveSourceFlowFromEventArg(arg);
            if (actor && sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::Bleedout)) {
                const bool beginAfter = TFD::Bleedout::HandleAfterPleasureEnter(actor, "bleedout_after_pleasure_force_open");
                const bool nativeOpen = (beginAfter || TFD::Bleedout::OwnsCurrentFlow())
                    ? TFD::BleedoutGreet::BeginAfterPleasure(actor, "bleedout_after_pleasure_force_open")
                    : false;
                spdlog::info(
                    "[TFD][Flow][R127] bleedout after pleasure force-open follows native pattern source={} actor={:08X} flowBegin={} nativeHardOpen={}",
                    sourceFlow,
                    actor->GetFormID(),
                    beginAfter ? 1 : 0,
                    nativeOpen ? 1 : 0);
            }
            else {
                spdlog::warn(
                    "[TFD][Flow][R110] after pleasure force-open ignored source={} actor={:08X}",
                    sourceFlow,
                    actor ? actor->GetFormID() : 0u);
            }
            return true;
        }

        if (name == kPleasureFailedEnterEvent) {
            auto* actor = ResolveActorFromEventArg(arg);
            const int sourceFlow = ResolveSourceFlowFromEventArg(arg);
            if (pleasureRuntimePhaseAfterEvent != TFD::PleasureRuntime::Phase::PleasureFailedDialogue) {
                spdlog::info(
                    "[TFD][Flow][R498A] PleasureFailedEnter route suppressed by runtime result owner recognized={} phase={} source={} actor={:08X} policy=no_success_to_failure_flip",
                    pleasureRuntimeRecognized ? 1 : 0,
                    TFD::PleasureRuntime::GetPhaseName(),
                    sourceFlow,
                    actor ? actor->GetFormID() : 0u);
                return true;
            }
            if (actor && IsNoGenericPleasureAftermathSource(sourceFlow)) {
                auto& flow = TFD::FlowController::Controller::GetSingleton();
                bool completeFlow = false;
                TFD::PleasureRuntime::Break("special_source_pleasure_failed_suppressed", true, true, true);
                TFD::InteractionRouter::ClearInteractionStateValue();
                const auto snapshot = flow.GetSnapshot();
                spdlog::info(
                    "[TFD][Flow][R321A] pleasure failed enter suppressed for special source={} actor={:08X} completeFlow={} root={} ctx={} sub={} policy=no_pleasurefailed_fg",
                    sourceFlow,
                    actor->GetFormID(),
                    completeFlow ? 1 : 0,
                    Controller::ToString(snapshot.root),
                    Controller::ToString(snapshot.contextRoot),
                    Controller::ToString(snapshot.sub));
                return true;
            }
            if (actor) {
                auto& flow = TFD::FlowController::Controller::GetSingleton();
                // R223A: PleasureFailed must be a separate terminal dialogue owner.
                // Do not route it through BeginAfterPleasure, otherwise a late
                // TFDAfterPleasureEnter can reopen normal AfterPleasure while the
                // player is still in the failed dialogue / pleading handoff.
                const bool beginFailed = flow.RequestBeginPleasureFailed(
                    actor->GetFormID(),
                    sourceFlow,
                    "pleasure_failed_enter_owner_only");
                TFD::InteractionRouter::DialogueOpen::BeginPleasureFailed(actor);
                spdlog::info(
                    "[TFD][Flow][R223A] pleasure failed greet armed independent source={} actor={:08X} beginFailed={} marker=TFDPleasureFailedFaction reason=no_speaker_climax",
                    sourceFlow,
                    actor->GetFormID(),
                    beginFailed ? 1 : 0);
            }
            else {
                spdlog::warn("[TFD][Flow][R137] pleasure failed enter rejected no actor source={} arg={}", sourceFlow, std::string(arg));
            }
            return true;
        }

        if (name == kPleasureFailedAggroEvent) {
            auto* actor = ResolveActorFromEventArg(arg);
            const int sourceFlow = ResolveSourceFlowFromEventArg(arg);
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto& flow = TFD::FlowController::Controller::GetSingleton();
            const auto beforeFailure = flow.GetSnapshot();
            const bool captiveFailureOwner = actor && IsCaptiveOwnedPleasureFailure(flow, beforeFailure, sourceFlow);
            const int effectiveSourceFlow = captiveFailureOwner ?
                static_cast<int>(TFD::PleasureRuntime::SourceContext::Captive) :
                sourceFlow;

            bool passiveOwnerCleared = false;
            bool combatRootCommitted = false;
            bool captiveEscapeArmed = false;
            bool captiveFightBreakArmed = false;
            std::size_t captiveCrowdSoftReleaseCount = 0;
            auto breakFightPassiveOwner = [&](const char* reason) -> bool {
                if (!actor || !player) {
                    return false;
                }
                if (captiveFailureOwner) {
                    return TFD::HostilityController::BreakCaptiveFightPassiveOwnership(actor, player, reason);
                }
                return TFD::HostilityController::BreakPassiveOwnershipForFightChoice(actor, player, reason);
                };
            if (actor) {
                TFD::Transition::AbortCalmWindowForCombat(actor, "pleasure_failed_fight_pre_break");
                (void)SuppressDefeatedEnemyAutoDeathForFightChoiceIfNeeded(actor, "pleasure_failed_fight_speaker");
                ClearPleasureFailedFightSpeakerSuppressors(actor, "pleasure_failed_fight_pre_break");
            }

            TFD::PleasureRuntime::Break("pleasure_failed_fight", true, true, true);
            TFD::InteractionRouter::DialogueOpen::Cancel();

            // R242A: Fight is a hard combat boundary.  The previous order cleared
            // some passive owners and ran FightChoice rehostile while FlowController
            // still reported root=None ctx=Bleedout sub=PleasureFailedDialogue.
            // That allowed dialogue/after-pleasure style package state to win the
            // first combat refresh, leaving the actor hostile and target-locked but
            // not in a real attack stance.  Commit the combat root first, then allow
            // HostilityController to release passive owners and refresh combat.
            if (actor) {
                if (captiveFailureOwner) {
                    captiveFightBreakArmed = flow.RequestInCombatEscapeBreak(
                        actor->GetFormID(),
                        "pleasure_failed_fight_captive_escape_break");
                    combatRootCommitted = captiveFightBreakArmed;
                }
                else {
                    flow.NotifyCombatStarted(actor->GetFormID(), "pleasure_failed_fight");
                    combatRootCommitted = true;
                }
                if (!combatRootCommitted) {
                    flow.NotifyCombatStarted(actor->GetFormID(), "pleasure_failed_fight");
                    combatRootCommitted = true;
                }

                if (captiveFailureOwner && player) {
                    // R273A: do not hard-force captive crowd/watchers into drawn weapon
                    // or direct combat updates here.  Release TFD pacify/passive
                    // ownership only, then EvaluatePackage once and let Skyrim's
                    // normal combat AI decide who joins the fight.
                    captiveCrowdSoftReleaseCount = TFD::HostilityController::SoftReleaseCaptiveCrowdPassiveForCombat(
                        player,
                        actor,
                        "pleasure_failed_fight_captive_crowd_soft_release",
                        false);
                }

                TFD::Transition::AbortCalmWindowForCombat(actor, "pleasure_failed_fight_after_root_commit");
                ClearPleasureFailedFightSpeakerSuppressors(actor, "pleasure_failed_fight_post_root_commit");
                passiveOwnerCleared = breakFightPassiveOwner("pleasure_failed_fight_post_root_commit_owner") || passiveOwnerCleared;
            }
            bool truceHardCleared = false;
            bool bleedoutCleared = false;
            bool pleasureQuestClearQueued = false;
            bool systemClearQueued = false;
            bool combatQueued = false;
            // R260A: do not queue Papyrus Pleasure/Bleedout/Truce clears before
            // the final combat bridge.  QueueBridgeModEvent runs on the SKSE task
            // queue in insertion order; when the broad clear events are inserted
            // before TFDPleasureFailedStartCombat, Papyrus/XPMSE/alias cleanup can
            // settle the actor into a sheathed non-stance graph before the bridge
            // has a chance to StartCombat + DrawWeapon.  Let native release owners
            // and rehostile first, then queue TFDPleasureFailedStartCombat before
            // the broad clear bundle below.

            bool truceReleased = false;
            if (actor) {
                truceReleased = TFD::HostilityController::ReleaseActiveTruceSessionForActor(
                    actor,
                    TFD::HostilityController::ReleaseReason::FightChoice,
                    false);
                passiveOwnerCleared = breakFightPassiveOwner("pleasure_failed_fight_after_truce_release_owner") || passiveOwnerCleared;
            }

            if (actor && captiveFailureOwner) {
                ClearPleasureFailedFightSpeakerSuppressors(actor, "pleasure_failed_fight_after_captive_escape_break");
            }

            if (auto* process = RE::ProcessLists::GetSingleton()) {
                process->runDetection = true;
                process->ClearCachedFactionFightReactions();
            }

            bool nativeCombatRefresh = false;
            if (actor && player) {
                actor->AllowPCDialogue(true);
                actor->SetBeenAttacked(true);
                player->SetBeenAttacked(true);
                (void)actor->RequestDetectionLevel(player, RE::DETECTION_PRIORITY::kCritical);
                (void)player->RequestDetectionLevel(actor, RE::DETECTION_PRIORITY::kCritical);
                // R241A: do not force weapon draw here. PleasureFailed -> Fight should
                // restore aggression/target/combat ownership only; Skyrim must enter
                // stance naturally after passive aliases/packages/factions are gone.
                // Forced draw/sheathe calls can desync the animation graph.
                // R152: do not EvaluatePackage between fight selection and final
                // FightChoice owner break. The combat handoff must not let a stale
                // after-pleasure/dialogue package win after the actor is handed to combat.
                passiveOwnerCleared = breakFightPassiveOwner("pleasure_failed_fight_pre_rehostile_owner") || passiveOwnerCleared;
                if (captiveFailureOwner) {
                    // R277A: Captive PleasureFailed > Fight must not use the generic
                    // native FightChoice refresh because that path force-locks combat
                    // target and can issue DrawWeapon. The SystemEvent captive no-draw
                    // bridge owns the StartCombat commit for speaker + crowd.
                    nativeCombatRefresh = false;
                    spdlog::info(
                        "[TFD][Flow][R277A] captive pleasure failed fight native hard refresh skipped actor={:08X} reason=no_draw_owner_specific_bridge",
                        actor->GetFormID());
                }
                else {
                    nativeCombatRefresh = TFD::HostilityController::ForceDetectionAndCombatRefresh(
                        actor,
                        player,
                        TFD::HostilityController::ReleaseReason::FightChoice,
                        true);
                    if (!nativeCombatRefresh) {
                        TFD::HostilityController::QueueDetectionAndCombatRefresh(
                            actor,
                            player,
                            TFD::HostilityController::ReleaseReason::FightChoice,
                            true);
                    }
                }
                if (!combatRootCommitted) {
                    flow.NotifyCombatStarted(actor->GetFormID(), "pleasure_failed_fight");
                    combatRootCommitted = true;
                }
                TFD::Transition::AbortCalmWindowForCombat(actor, "pleasure_failed_fight_post_rehostile");
                (void)SuppressDefeatedEnemyAutoDeathForFightChoiceIfNeeded(actor, "pleasure_failed_fight_post_rehostile");
                ClearPleasureFailedFightSpeakerSuppressors(actor, "pleasure_failed_fight_post_rehostile");
                passiveOwnerCleared = breakFightPassiveOwner("pleasure_failed_fight_post_rehostile_owner") || passiveOwnerCleared;
            }

            if (actor && captiveFailureOwner) {
                // R278A: Captive PleasureFailed > Fight must not emit the generic
                // TFDPleasureFailedStartCombat event. TFDTruceBridge also listens to
                // that event and can arm/clear combat handoff for unrelated Truce
                // ownership. Route the speaker through the captive-specific no-draw
                // SystemEvent bridge instead; crowd/watchers already use the paired
                // captive crowd bridge from HostilityController.
                combatQueued = TFD::FlowController::QueueBridgeModEvent(
                    kCaptivePleasureFailedFightStartCombatEvent,
                    actor,
                    "pleasure_failed_fight_captive_speaker",
                    static_cast<float>(effectiveSourceFlow));
            }
            else {
                combatQueued = TFD::FlowController::QueueBridgeModEvent(
                    kPleasureFailedStartCombatEvent,
                    actor,
                    "pleasure_failed_fight",
                    static_cast<float>(effectiveSourceFlow));
            }

            // R198/R260A/R261A: native PleasureRuntime::Break() only clears the
            // native runtime.  Papyrus Pleasure/SystemEvent/Truce/Bleedout aliases
            // still need cleanup, but S3 proved that simply queueing the combat
            // bridge first is not enough: Papyrus event stacks can interleave and
            // the broad clear bundle can run before the R257 StartCombat/DrawWeapon
            // pass.  Do not dispatch TFDPleasureClear / TFDBleedoutClearAll /
            // TFDTruceHardClearAll directly here.  Queue one SystemEvent cleanup
            // owner with the effective source encoded in numArg; SystemEvent waits
            // for the combat bridge window to settle, then relays the source-specific
            // broad clear and does the route clear.
            systemClearQueued = TFD::FlowController::QueueBridgeModEvent(
                "TFDSystemEventClearAfterPleasure",
                actor,
                "pleasure_failed_fight_after_combat_bridge",
                static_cast<float>(effectiveSourceFlow));
            pleasureQuestClearQueued = false;
            truceHardCleared = false;
            bleedoutCleared = false;

            spdlog::info(
                "[TFD][Flow][R278A] pleasure failed fight actor={:08X} reportedSource={} effectiveSource={} captiveOwner={} passiveOwnerCleared={} captiveCrowdSoftRelease={} combatRootCommitted={} truceReleased={} truceHardCleared={} bleedoutCleared={} pleasureClearQueued={} delayedSystemClearQueued={} captiveEscapeArmed={} captiveFightBreakArmed={} nativeCombatRefresh={} combatQueued={} oldRoot={} oldCtx={} oldSub={} reason=no_speaker_climax",
                actor ? actor->GetFormID() : 0u,
                sourceFlow,
                effectiveSourceFlow,
                captiveFailureOwner ? 1 : 0,
                passiveOwnerCleared ? 1 : 0,
                static_cast<unsigned int>(captiveCrowdSoftReleaseCount),
                combatRootCommitted ? 1 : 0,
                truceReleased ? 1 : 0,
                truceHardCleared ? 1 : 0,
                bleedoutCleared ? 1 : 0,
                pleasureQuestClearQueued ? 1 : 0,
                systemClearQueued ? 1 : 0,
                captiveEscapeArmed ? 1 : 0,
                captiveFightBreakArmed ? 1 : 0,
                nativeCombatRefresh ? 1 : 0,
                combatQueued ? 1 : 0,
                Controller::ToString(beforeFailure.root),
                Controller::ToString(beforeFailure.contextRoot),
                Controller::ToString(beforeFailure.sub));
            return true;
        }

        if (name == kAfterPleasureEnterEvent) {
            auto* actor = ResolveActorFromEventArg(arg);
            const int reportedSourceFlow = ResolveSourceFlowFromEventArg(arg);
            const auto runtimeSource = TFD::PleasureRuntime::GetSourceContext();
            const auto runtimePhase = pleasureRuntimePhaseAfterEvent;
            if (runtimePhase != TFD::PleasureRuntime::Phase::AfterPleasureDialogue) {
                spdlog::info(
                    "[TFD][Flow][R498A] AfterPleasureEnter route suppressed by runtime result owner recognized={} phase={} reportedSource={} actor={:08X} policy=no_failure_to_success_flip",
                    pleasureRuntimeRecognized ? 1 : 0,
                    TFD::PleasureRuntime::GetPhaseName(),
                    reportedSourceFlow,
                    actor ? actor->GetFormID() : 0u);
                return true;
            }
            const int sourceFlow = runtimeSource != TFD::PleasureRuntime::SourceContext::None
                ? static_cast<int>(runtimeSource)
                : reportedSourceFlow;
            auto& flow = TFD::FlowController::Controller::GetSingleton();
            if (actor && IsNoGenericPleasureAftermathSource(sourceFlow)) {
                bool completeFlow = false;
                TFD::PleasureRuntime::Break("special_source_afterpleasure_enter_suppressed", true, true, true);
                TFD::InteractionRouter::ClearInteractionStateValue();
                const auto snapshot = flow.GetSnapshot();
                spdlog::info(
                    "[TFD][Flow][R321A] after pleasure enter suppressed for special source={} reportedSource={} actor={:08X} completeFlow={} runtimeSource={} root={} ctx={} sub={} policy=no_afterpleasure_fg",
                    sourceFlow,
                    reportedSourceFlow,
                    actor->GetFormID(),
                    completeFlow ? 1 : 0,
                    TFD::PleasureRuntime::GetSourceContextName(),
                    Controller::ToString(snapshot.root),
                    Controller::ToString(snapshot.contextRoot),
                    Controller::ToString(snapshot.sub));
                return true;
            }
            bool runtimeFailed = TFD::PleasureRuntime::IsPleasureFailedDialogueActive();
            bool flowFailed = flow.IsPleasureFailedSubFlowActive();
            if (actor && (runtimeFailed || flowFailed)) {
                const bool runtimeIsAfterPleasure =
                    runtimePhase == TFD::PleasureRuntime::Phase::AfterPleasureAwaitQuest ||
                    runtimePhase == TFD::PleasureRuntime::Phase::AfterPleasureDialogue;

                if (!runtimeFailed && flowFailed && runtimeIsAfterPleasure) {
                    const bool resumed = flow.RequestResumePleasureFromPleasureFailed(
                        actor->GetFormID(),
                        sourceFlow,
                        "after_pleasure_stale_failed_owner_recover_r264");
                    flowFailed = flow.IsPleasureFailedSubFlowActive();
                    spdlog::info(
                        "[TFD][Flow][R264A] after pleasure recovered stale failed owner reportedSource={} effectiveSource={} runtimeSource={} runtimePhase={} actor={:08X} resumed={} flowFailedAfter={}",
                        reportedSourceFlow,
                        sourceFlow,
                        TFD::PleasureRuntime::GetSourceContextName(),
                        TFD::PleasureRuntime::GetPhaseName(),
                        actor->GetFormID(),
                        resumed ? 1 : 0,
                        flowFailed ? 1 : 0);
                }

                if (runtimeFailed || flowFailed) {
                    spdlog::info(
                        "[TFD][Flow][R264A] after pleasure enter ignored while pleasure failed owns dialogue reportedSource={} effectiveSource={} actor={:08X} runtimeFailed={} flowFailed={} runtimePhase={} runtimeSource={}",
                        reportedSourceFlow,
                        sourceFlow,
                        actor->GetFormID(),
                        runtimeFailed ? 1 : 0,
                        flowFailed ? 1 : 0,
                        TFD::PleasureRuntime::GetPhaseName(),
                        TFD::PleasureRuntime::GetSourceContextName());
                    return true;
                }
            }
            if (actor && sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::Bleedout)) {
                const auto beforeAfter = flow.GetSnapshot();
                const bool captiveLayerAfterPleasure =
                    flow.IsCaptiveEscapeBreakContextActive() ||
                    beforeAfter.root == RootFlow::Captive ||
                    beforeAfter.contextRoot == RootFlow::Captive ||
                    beforeAfter.sub == SubFlow::CaptiveAfterPleasure ||
                    beforeAfter.sub == SubFlow::BleedoutEscapeBreak;

                if (captiveLayerAfterPleasure) {
                    const bool beginAfter = flow.RequestBeginAfterPleasure(actor->GetFormID(), "bleedout_captive_after_pleasure_enter");
                    const bool captiveOpen = TFD::CaptiveGreet::BeginAfterPleasure(actor, "bleedout_captive_after_pleasure_enter");
                    spdlog::info(
                        "[TFD][Flow][R274A] bleedout captive after pleasure routed captive source={} actor={:08X} flowBegin={} captiveOpen={} oldRoot={} oldCtx={} oldSub={}",
                        sourceFlow,
                        actor->GetFormID(),
                        beginAfter ? 1 : 0,
                        captiveOpen ? 1 : 0,
                        Controller::ToString(beforeAfter.root),
                        Controller::ToString(beforeAfter.contextRoot),
                        Controller::ToString(beforeAfter.sub));
                    return true;
                }

                const bool beginAfter = TFD::Bleedout::HandleAfterPleasureEnter(actor, "bleedout_after_pleasure_enter");
                bool nativeOpen = false;
                if (beginAfter || TFD::Bleedout::OwnsCurrentFlow()) {
                    nativeOpen = TFD::BleedoutGreet::BeginAfterPleasure(actor, "bleedout_after_pleasure_enter");
                }

                // Captive Pleasure can be reached from a Bleedout dialogue while the
                // actual post-scene owner has already transitioned to Captive.  In
                // that case the old Bleedout source value is stale; route the hard
                // open through CaptiveGreet instead of dropping the AfterPleasure.
                bool captiveFallbackOpen = false;
                if (!nativeOpen) {
                    const auto afterBleedAttempt = flow.GetSnapshot();
                    if (afterBleedAttempt.root == RootFlow::Captive && afterBleedAttempt.sub == SubFlow::CaptiveAfterPleasure) {
                        captiveFallbackOpen = TFD::CaptiveGreet::BeginAfterPleasure(actor, "bleedout_to_captive_after_pleasure_enter");
                    }
                }

                if (beginAfter || nativeOpen || captiveFallbackOpen) {
                    spdlog::info(
                        "[TFD][Flow][R127] bleedout after pleasure greet armed source={} actor={:08X} flowBegin={} nativeHardOpen={} captiveFallbackOpen={}",
                        sourceFlow,
                        actor->GetFormID(),
                        beginAfter ? 1 : 0,
                        nativeOpen ? 1 : 0,
                        captiveFallbackOpen ? 1 : 0);
                }
                else {
                    spdlog::warn("[TFD][Flow][R127] bleedout after pleasure enter rejected source={} actor={:08X}", sourceFlow, actor ? actor->GetFormID() : 0u);
                }
            }
            else if (actor && IsInCombatSource(sourceFlow)) {
                const bool beginAfter = flow.RequestBeginAfterPleasure(actor->GetFormID(), "incombat_after_pleasure_enter");
                const bool alreadyInCombatAfter = flow.IsInCombatAfterPleasureContextActive();
                const bool runtimeAfterDialogue = runtimePhase == TFD::PleasureRuntime::Phase::AfterPleasureDialogue;
                const bool nativeAfterOpenSucceeded = TFD::InteractionRouter::DialogueOpen::WasLastSuccess(
                    TFD::InteractionRouter::DialogueOpen::Mode::AfterPleasure,
                    actor->GetFormID());

                if (!beginAfter && alreadyInCombatAfter && runtimeAfterDialogue && nativeAfterOpenSucceeded) {
                    // R469A: Papyrus can re-send TFDAfterPleasureEnter from the
                    // no-commit reopen watchdog after the first hard-open already
                    // succeeded. Re-running InCombatGreet::BeginAfterPleasure here
                    // resets the native handshake and causes visible AfterPleasure
                    // flicker. Keep the existing owner instead.
                    spdlog::info(
                        "[TFD][Flow][R469A] duplicate incombat after pleasure enter ignored source={} actor={:08X} flowBegin=0 runtimePhase={} nativeOpen=1",
                        sourceFlow,
                        actor->GetFormID(),
                        TFD::PleasureRuntime::GetPhaseName());
                    return true;
                }

                if (beginAfter || alreadyInCombatAfter) {
                    (void)TFD::InCombat::HandleAfterPleasureEnter(
                        actor,
                        "after_pleasure_enter",
                        TFD::InCombat::AfterPleasureHandlers{
                            [&](std::uint32_t actorFormID, const char* r) { TFD::InCombat::NoteAfterPleasure(actorFormID, r); },
                            [&](RE::Actor* greetActor, const char* r) -> bool { return TFD::InCombatGreet::BeginAfterPleasure(greetActor, r); }
                        });
                    spdlog::info("[TFD][Flow][R93T] incombat after pleasure greet armed source={} actor={:08X} flowBegin={} duplicateFallback={}", sourceFlow, actor->GetFormID(), beginAfter ? 1 : 0, (!beginAfter && alreadyInCombatAfter) ? 1 : 0);
                }
                else {
                    spdlog::warn("[TFD][Flow][R93T] incombat after pleasure enter rejected source={} actor={:08X}", sourceFlow, actor->GetFormID());
                }
            }
            else if (actor && sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::Captive)) {
                const bool beginAfter = flow.RequestBeginAfterPleasure(actor->GetFormID(), "captive_after_pleasure_enter");
                (void)TFD::CaptiveGreet::BeginAfterPleasure(actor, "after_pleasure_enter");
                spdlog::info("[TFD][Flow] captive after pleasure greet armed source={} actor={:08X} flowBegin={}", sourceFlow, actor->GetFormID(), beginAfter ? 1 : 0);
            }
            else if (actor && sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::Teammate)) {
                TFD::PleasureRuntime::Break("teammate_afterpleasure_enter_suppressed", true, true, true);
                TFD::InteractionRouter::ClearInteractionStateValue();
                spdlog::info("[TFD][Flow][R321A] teammate after pleasure greet suppressed source={} actor={:08X} policy=no_afterpleasure_fg", sourceFlow, actor->GetFormID());
            }
            else if (actor && sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::PreCombat)) {
                if (flow.RequestBeginAfterPleasure(actor->GetFormID(), "after_pleasure_enter")) {
                    TFD::InteractionRouter::DialogueOpen::BeginAfterPleasure(actor);
                    spdlog::info("[TFD][Flow] precombat after pleasure greet armed source={} actor={:08X}", sourceFlow, actor->GetFormID());
                }
            }
            return true;
        }

        if (name == kInCombatOutcomeReleaseEndEvent) {
            auto* actor = ResolveActorFromEventArg(arg);
            const bool restoreRequested = numArg > 0.5f;
            bool graceRemoved = false;
            std::size_t wakeQueued = 0;

            std::vector<RE::Actor*> releasePack{};
            if (actor && restoreRequested) {
                releasePack = TFD::Actor::Ops::CollectTruceActorsForSpeaker(actor);
            }

            // P12B: neutral ReleaseSafePass expiry is not a combat-restore point.
            // Do not route it through generic ReleaseFollowGrace cleanup, because that
            // path can EvaluatePackage/UpdateCombat while Papyrus is ending the safe
            // pass and has caused native crashes during TFDInCombatOutcomeReleaseEnd.
            // Generic grace cleanup is still allowed for restore=true paths.
            const bool cycleReleaseGrace = actor && restoreRequested ? ConsumeInCombatCycleReleaseGrace(actor->GetFormID()) : false;
            if (actor && restoreRequested && cycleReleaseGrace) {
                TFD::Actor::Ops::RemoveReleaseFollowGraceFromActorOnly(actor, "incombat_cycle_release_end");
                graceRemoved = true;
            }
            else if (actor && restoreRequested && g_outcomeRuntimeProviders.removeReleaseFollowGrace) {
                g_outcomeRuntimeProviders.removeReleaseFollowGrace(actor, "incombat_release_end");
                graceRemoved = true;
            }
            else if (actor && !restoreRequested) {
                spdlog::info(
                    "[TFD][Flow][P12B] neutral incombat release end skipped native grace cleanup actor={:08X} reason=incombat_release_end",
                    actor->GetFormID());
            }
            if (actor && !actor->IsDead() && !actor->IsDisabled()) {
                // P10/P12B: Papyrus ReleaseSafePass reports whether the actor should
                // be re-woken.  A neutral expiry only drops the Pay guard bookkeeping;
                // it must not restore aggression or call generic grace cleanup.
                TFD::HostilityController::ReleasePayDialoguePassiveGuard(actor, restoreRequested, "incombat_release_end");
            }

            if (actor && restoreRequested) {
                if (releasePack.empty()) {
                    releasePack.push_back(actor);
                }
                wakeQueued = WakeInCombatReleasePackForDetection(releasePack, "incombat_release_end");
            }

            spdlog::info(
                "[TFD][Flow][R93Y] incombat release end actor={:08X} restore={} graceRemoved={} wakeQueued={} pack={} arg={}",
                actor ? actor->GetFormID() : 0u,
                restoreRequested ? 1 : 0,
                graceRemoved ? 1 : 0,
                static_cast<unsigned int>(wakeQueued),
                static_cast<unsigned int>(releasePack.size()),
                arg.empty() ? std::string{ "-" } : arg);
            return true;
        }

        if (name == kInCombatOutcomeReleaseEvent) {
            auto* actor = ResolveActorFromEventArg(arg);
            auto& flow = TFD::FlowController::Controller::GetSingleton();
            const std::uint32_t actorFormID = actor ? actor->GetFormID() : TFD::InCombat::GetPrimaryActorFormID();
            const bool flowDone = flow.RequestResolveInCombatOutcome(
                InCombatOutcome::Release,
                actorFormID,
                "mod_event_incombat_release");
            const bool terminalComplete = flowDone ? flow.RequestCompleteTerminalContext("mod_event_incombat_release_complete") : false;

            if (actor && TFD::HostilityController::IsPayDialoguePassiveGuardActive(actor)) {
                const double guardSeconds = (numArg > 0.0f ? static_cast<double>(numArg) : 10.0) + 5.0;
                TFD::HostilityController::ArmPayDialoguePassiveGuard(actor, guardSeconds, "incombat_release_safe_pass_extend");
            }

            const bool cycleRelease = actor && TFD::InCombatGreet::IsPleasureCycleActiveForActor(actor);
            if (cycleRelease) {
                const double graceSeconds = numArg > 0.0f ? static_cast<double>(numArg) : 10.0;
                const bool queuedNext = TFD::PleasureRuntime::QueueNextCycleAfterTerminal(
                    actor,
                    TFD::PleasureRuntime::SourceContext::InCombat,
                    "incombat_cycle_pay_release");
                TFD::Actor::Ops::ApplyReleaseFollowGraceToActorOnly(actor, graceSeconds, "incombat_cycle_release_to_vanilla");
                MarkInCombatCycleReleaseGrace(actor->GetFormID());
                const bool releasedSingle = TFD::HostilityController::ReleaseSingleTruceActorForCycle(
                    actor,
                    TFD::HostilityController::ReleaseReason::FlowHandoff,
                    "incombat_cycle_pay_release");
                const auto flushedDeferred = queuedNext ? std::size_t{ 0 } :
                    TFD::PleasureRuntime::FlushDeferredInCombatRecruits("incombat_cycle_pay_release_final");
                TFD::InCombat::Complete("mod_event_incombat_cycle_release");
                spdlog::info(
                    "[TFD][Flow][R95B] incombat cycle release actor={:08X} flowDone={} terminalComplete={} queuedNext={} releasedSingle={} deferredRegistered={} duration={:.2f} policy=current_actor_only_flush_on_final",
                    actorFormID,
                    flowDone ? 1 : 0,
                    terminalComplete ? 1 : 0,
                    queuedNext ? 1 : 0,
                    releasedSingle ? 1 : 0,
                    static_cast<unsigned int>(flushedDeferred),
                    graceSeconds);
                return true;
            }

            // P2PAY: InCombat Pay > Release is temporary, but the temporary
            // countdown is owned by TFDInCombatQuestScript ReleaseSafePass.
            // Do not route through generic TFDRelease/ReleaseFollowGrace here.
            const auto flushedDeferred = TFD::PleasureRuntime::FlushDeferredInCombatRecruits("mod_event_incombat_release_final");
            const bool truceReleased = actor ?
                TFD::HostilityController::ReleaseActiveTruceSessionForActor(actor, TFD::HostilityController::ReleaseReason::FlowHandoff, false) :
                false;
            TFD::InCombat::Complete("mod_event_incombat_release_safepass_owner");
            spdlog::info(
                "[TFD][Flow][P2PAY] incombat release terminal actor={:08X} flowDone={} terminalComplete={} truceRelease={} deferredRegistered={} duration={:.2f} papyrusSafePassOwns=1",
                actorFormID,
                flowDone ? 1 : 0,
                terminalComplete ? 1 : 0,
                truceReleased ? 1 : 0,
                static_cast<unsigned int>(flushedDeferred),
                static_cast<double>(numArg));
            return true;
        }

        if (name == kInCombatOutcomeFollowEvent) {
            auto* actor = ResolveActorFromEventArg(arg);
            const double followSeconds = numArg > 0.0f ? static_cast<double>(numArg) : 20.0;
            auto& flow = TFD::FlowController::Controller::GetSingleton();
            const std::uint32_t actorFormID = actor ? actor->GetFormID() : TFD::InCombat::GetPrimaryActorFormID();
            const bool flowDone = flow.RequestResolveInCombatOutcome(
                InCombatOutcome::Follow,
                actorFormID,
                "mod_event_incombat_follow");
            const bool terminalComplete = flowDone ? flow.RequestCompleteTerminalContext("mod_event_incombat_follow_complete") : false;

            if (actor && TFD::HostilityController::IsPayDialoguePassiveGuardActive(actor)) {
                const double guardSeconds = followSeconds + 30.0;
                TFD::HostilityController::ArmPayDialoguePassiveGuard(actor, guardSeconds, "incombat_follow_temporary_extend");
            }

            const bool cycleFollow = actor && TFD::InCombatGreet::IsPleasureCycleActiveForActor(actor);
            if (cycleFollow) {
                const bool queuedNext = TFD::PleasureRuntime::QueueNextCycleAfterTerminal(
                    actor,
                    TFD::PleasureRuntime::SourceContext::InCombat,
                    "incombat_cycle_pay_follow");
                TFD::Actor::Ops::ApplyReleaseFollowGraceToActorOnly(actor, followSeconds, "incombat_cycle_follow");
                MarkInCombatCycleReleaseGrace(actor->GetFormID());
                const bool releasedSingle = TFD::HostilityController::ReleaseSingleTruceActorForCycle(
                    actor,
                    TFD::HostilityController::ReleaseReason::FlowHandoff,
                    "incombat_cycle_pay_follow");
                const auto flushedDeferred = queuedNext ? std::size_t{ 0 } :
                    TFD::PleasureRuntime::FlushDeferredInCombatRecruits("incombat_cycle_pay_follow_final");
                TFD::InCombat::Complete("mod_event_incombat_cycle_follow");
                spdlog::info(
                    "[TFD][Flow][R95B] incombat cycle follow actor={:08X} flowDone={} terminalComplete={} queuedNext={} releasedSingle={} deferredRegistered={} duration={:.2f} policy=current_actor_only_flush_on_final",
                    actorFormID,
                    flowDone ? 1 : 0,
                    terminalComplete ? 1 : 0,
                    queuedNext ? 1 : 0,
                    releasedSingle ? 1 : 0,
                    static_cast<unsigned int>(flushedDeferred),
                    followSeconds);
                return true;
            }

            // P2PAY: InCombat Pay > Follow is temporary follower owned by
            // TFDTemporaryFollowerQuest. Avoid generic ReleaseFollowGrace so native
            // does not add expired-teammate/pacify markers beside the follow alias.
            const auto flushedDeferred = TFD::PleasureRuntime::FlushDeferredInCombatRecruits("mod_event_incombat_follow_final");
            const bool truceReleased = actor ?
                TFD::HostilityController::ReleaseActiveTruceSessionForActor(actor, TFD::HostilityController::ReleaseReason::FlowHandoff, false) :
                false;
            TFD::InCombat::Complete("mod_event_incombat_follow_temporary_owner");
            spdlog::info(
                "[TFD][Flow][P2PAY] incombat follow terminal event={} actor={:08X} flowDone={} terminalComplete={} truceRelease={} deferredRegistered={} temporaryFollowerOwns=1",
                std::string(name),
                actorFormID,
                flowDone ? 1 : 0,
                terminalComplete ? 1 : 0,
                truceReleased ? 1 : 0,
                static_cast<unsigned int>(flushedDeferred));
            return true;
        }

        // Bleedout terminal outcomes must be consumed before the generic Release router.
        // Otherwise TFDBleedoutOutcomeRelease is treated as a vanilla release and bypasses
        // the Bleedout-owned cleanup/transition path.
        if (name == kBleedoutOutcomeDoNothingEvent) {
            auto* actor = ResolveActorFromEventArg(arg);
            std::uint32_t actorFormID = actor ? actor->GetFormID() : 0;
            if (!actorFormID) {
                actorFormID = ResolveBleedFlowActorFormIDFromProviders();
                actor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
            }

            const bool bleedActive = IsBleedStateActiveFromProviders();
            auto& flow = TFD::FlowController::Controller::GetSingleton();
            const bool captiveEscapeBleedout = bleedActive && IsCaptiveEscapeGuardActive(flow);
            if (captiveEscapeBleedout) {
                const bool recaptured = TFD::Captive::CommitRecapture(actor, "captive_escape_bleedout_do_nothing_recapture");
                spdlog::info(
                    "[TFD][Flow][C47] captive escape-bleedout do-nothing -> recapture actor={:08X} bleedActive={} recaptured={}",
                    actorFormID,
                    bleedActive ? 1 : 0,
                    recaptured ? 1 : 0);
                return true;
            }

            bool flowResolved = false;
            bool leftForDeadStarted = false;
            bool terminalCooldownArmed = false;
            if (bleedActive) {
                flowResolved = flow.RequestResolveBleedoutOutcome(BleedoutOutcome::Cancel, actorFormID, "bleedout_do_nothing_left_for_dead");
                TFD::FlowController::HandleObservedLeftForDead("bleedout_do_nothing_left_for_dead");
                // R210A: Do Nothing is a terminal left-for-dead outcome. Without an
                // immediate post-terminal cooldown, the Defeat monitor can scan the
                // still-low player HP in the same tick and open a new generic Bleedout
                // decision before the left-for-dead/recovery transition owns the state.
                TFD::Transition::BeginLeftForDeadCooldown(12);
                ScrubBleedoutTerminalGlobals("bleedout_do_nothing_left_for_dead", true);
                (void)QueueBridgeModEvent("TFDSystemEventForceClearRoute", nullptr, "bleedout_do_nothing_left_for_dead", 1.0f);
                terminalCooldownArmed = true;
                leftForDeadStarted = true;
            }

            spdlog::info(
                "[TFD][Flow][R210A] bleedout do-nothing -> left-for-dead actor={:08X} bleedActive={} flowResolved={} started={} cooldownArmed={}",
                actorFormID,
                bleedActive ? 1 : 0,
                flowResolved ? 1 : 0,
                leftForDeadStarted ? 1 : 0,
                terminalCooldownArmed ? 1 : 0);
            return true;
        }

        if (name == kBleedoutOutcomeReleaseEvent) {
            auto* actor = ResolveActorFromEventArg(arg);
            std::uint32_t actorFormID = actor ? actor->GetFormID() : 0;
            if (!actorFormID) {
                actorFormID = ResolveBleedFlowActorFormIDFromProviders();
                actor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
            }

            const double durationSec = numArg > 0.0f ? static_cast<double>(numArg) : 10.0;
            constexpr const char* graceReason = "bleedout_pay_terminal_release";
            if (actor) {
                spdlog::info(
                    "[TFD][Flow][P15BOWN] bleedout release event delegated to terminal PayRelease owner actor={:08X} duration={:.1f} reason={}",
                    actor->GetFormID(),
                    durationSec,
                    graceReason);
            }
            else {
                spdlog::warn(
                    "[TFD][Flow][R100C] bleedout release has no valid actor arg={} resolved={:08X}",
                    strArg ? strArg : "",
                    actorFormID);
            }

            const bool bleedActive = IsBleedStateActiveFromProviders();
            auto& flow = TFD::FlowController::Controller::GetSingleton();
            const auto before = flow.GetSnapshot();
            const bool bleedFlowContext =
                before.root == RootFlow::Bleedout ||
                before.contextRoot == RootFlow::Bleedout ||
                before.sub == SubFlow::BleedoutPleasure ||
                before.sub == SubFlow::BleedoutAfterPleasure;
            bool flowResolved = false;
            bool flowCompleted = false;
            bool forcedClear = false;
            bool terminalComplete = false;

            if (bleedActive || bleedFlowContext) {
                if (bleedActive || before.root == RootFlow::Bleedout || before.sub == SubFlow::BleedoutPleasure) {
                    flowResolved = flow.RequestResolveBleedoutOutcome(BleedoutOutcome::Cancel, actorFormID, graceReason);
                }

                // If the flow is already in BleedoutAfterPleasure terminal context,
                // RequestResolveBleedoutOutcome can legitimately reject because the
                // root is None.  The correct terminal action is still to complete the
                // terminal context.
                if (!flowResolved && bleedFlowContext) {
                    flowResolved = true;
                }

                if (flowResolved) {
                    flowCompleted = flow.RequestCompleteTerminalContext("bleedout_release_complete");
                }

                if (!flowCompleted) {
                    const auto afterComplete = flow.GetSnapshot();
                    const bool stillBleedContext =
                        afterComplete.root == RootFlow::Bleedout ||
                        afterComplete.contextRoot == RootFlow::Bleedout ||
                        afterComplete.sub == SubFlow::BleedoutPleasure ||
                        afterComplete.sub == SubFlow::BleedoutAfterPleasure;
                    if (stillBleedContext) {
                        flow.ResetRuntime("bleedout_release_force_clear");
                        forcedClear = true;
                        flowCompleted = true;
                    }
                }

                terminalComplete = TFD::Bleedout::CompletePayRelease(
                    graceReason,
                    TFD::Bleedout::Builders::BuildPayReleaseCompletionHandlers());
                ScrubBleedoutTerminalGlobals(graceReason, false);
                (void)QueueBridgeModEvent("TFDSystemEventForceClearRoute", nullptr, graceReason, 1.0f);

                if (actor) {
                    spdlog::info(
                        "[TFD][Flow][P15BOWN] bleedout release cleanup delegated actor={:08X} terminalComplete={} duration={:.1f}",
                        actor->GetFormID(),
                        terminalComplete ? 1 : 0,
                        durationSec);
                }
            }

            spdlog::info(
                "[TFD][Flow][W47] bleedout release terminal actor={:08X} seconds={:.1f} bleedActive={} bleedContext={} flowResolved={} flowCompleted={} forcedClear={} terminalComplete={}",
                actorFormID,
                durationSec,
                bleedActive ? 1 : 0,
                bleedFlowContext ? 1 : 0,
                flowResolved ? 1 : 0,
                flowCompleted ? 1 : 0,
                forcedClear ? 1 : 0,
                terminalComplete ? 1 : 0);
            return true;
        }

        SKSE::ModCallbackEvent callbackEv{ eventName, strArg ? strArg : "", numArg, sender };
        if (TFD::Release::HandleModCallbackEvent(&callbackEv)) {
            return true;
        }
        if (TFD::PreCombatGreet::HandleModCallbackEvent(
            &callbackEv,
            TFD::PreCombatGreet::GraceEventHandlers{
                [&](RE::Actor* graceActor, double seconds, const char* graceReason) {
                    if (g_outcomeRuntimeProviders.applyReleaseFollowGrace) {
                        g_outcomeRuntimeProviders.applyReleaseFollowGrace(graceActor, seconds, graceReason);
                    }
                },
                [&](RE::Actor* graceActor, const char* graceReason) {
                    if (g_outcomeRuntimeProviders.removeReleaseFollowGrace) {
                        g_outcomeRuntimeProviders.removeReleaseFollowGrace(graceActor, graceReason);
                    }
                }
            })) {
            return true;
        }

        if (name == kPleasureOutcomeReleaseEvent) {
            auto* actor = ResolveActorFromEventArg(arg);
            const double durationSec = numArg > 0.0f ? static_cast<double>(numArg) : 20.0;
            if (!actor) {
                spdlog::warn("[TFD][Flow] grace event={} ignored reason=invalid_actor arg={}", std::string(name), strArg ? strArg : "");
                return true;
            }
            if (g_outcomeRuntimeProviders.applyReleaseFollowGrace) {
                g_outcomeRuntimeProviders.applyReleaseFollowGrace(actor, durationSec, "pleasure_release");
            }
            return true;
        }

        const TFD::InCombat::OutcomeEventHandlers inCombatOutcomeEventHandlers{
            [&](const char* reason, double seconds) { TFD::Bleedout::ArmSystemEventOutcomeWindow(reason, seconds); },
            [&](TFD::InCombat::DialogueOutcome outcome, const char* reason) { TFD::InCombat::SetDialogueOutcome(outcome, reason); },
            [&](const char* reason) { TFD::InCombat::ClearDialogueOutcome(reason); },
            [&](std::uint32_t actorFormID, const char* reason) -> bool {
                auto& flow = TFD::FlowController::Controller::GetSingleton();
                return flow.RequestCaptiveFromModEvent(actorFormID, reason ? reason : "mod_event_captive");
            },
            [&](std::uint32_t actorFormID, const char* reason) -> bool {
                auto& flow = TFD::FlowController::Controller::GetSingleton();
                return flow.RequestCaptivePleasureFromModEvent(actorFormID, reason ? reason : "mod_event_pleasure");
            },
            [&](const char* reason) {
                if (g_outcomeRuntimeProviders.preparePlayerForCaptivePleasureScene) {
                    g_outcomeRuntimeProviders.preparePlayerForCaptivePleasureScene(reason);
                }
            },
            [&](const char* reason) {
                if (g_outcomeRuntimeProviders.completeCaptivePleasureHandoff) {
                    g_outcomeRuntimeProviders.completeCaptivePleasureHandoff(reason);
                }
            },
            [&](const char* reason) {
                spdlog::info("[TFD][Flow][R93T] incombat pleasure handler prepare noop reason={}", reason ? reason : "-");
            },
            [&](const char* reason) {
                spdlog::info("[TFD][Flow][R93T] incombat pleasure handler complete noop reason={}", reason ? reason : "-");
            }
        };

        if (name == kInCombatOutcomePayEvent) {
            auto actorFormID = ResolveActorFormIDFromEventArg(arg);
            if (!actorFormID) {
                actorFormID = TFD::InCombat::GetPrimaryActorFormID();
            }
            auto* flowActor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
            TFD::InCombat::OutcomeEventContext context{};
            context.eventName = "mod_event_pay";
            context.rawEventName = eventName;
            context.actor = flowActor;
            context.actorFormID = actorFormID;
            context.inCombatState = TFD::InCombat::IsActive();
            (void)TFD::InCombat::HandleOutcomePayEvent(context, inCombatOutcomeEventHandlers);
            auto& flow = TFD::FlowController::Controller::GetSingleton();
            const bool flowDone = flow.RequestResolveInCombatOutcome(InCombatOutcome::Pay, actorFormID, "mod_event_incombat_pay");
            spdlog::info("[TFD][Flow][R93T] incombat pay branch actor={:08X} flowDone={} truceRelease=0", actorFormID, flowDone ? 1 : 0);
            return true;
        }

        if (name == kInCombatOutcomePleasureEvent) {
            auto actorFormID = ResolveActorFormIDFromEventArg(arg);
            if (!actorFormID) {
                actorFormID = TFD::InCombat::GetPrimaryActorFormID();
            }
            if (IsPlayerBleedoutInteractionOwnedForFlow()) {
                spdlog::warn(
                    "[TFD][Flow][CB05] reject stale incombat pleasure while player bleedout actor={:08X} reason=block_incombat_pleasure_bleedout_owned",
                    actorFormID);
                TFD::InCombatGreet::CancelAll("reject_incombat_pleasure_player_bleedout");
                TFD::InCombat::Complete("reject_incombat_pleasure_player_bleedout");
                return true;
            }
            auto* flowActor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
            TFD::InCombat::OutcomeEventContext context{};
            context.eventName = "mod_event_pleasure";
            context.rawEventName = eventName;
            context.actor = flowActor;
            context.actorFormID = actorFormID;
            context.inCombatState = TFD::InCombat::IsActive();
            context.preserveCaptive = TFD::Captive::IsCaptivePassiveHoldActive();
            auto& flow = TFD::FlowController::Controller::GetSingleton();
            const bool flowPleasure = context.preserveCaptive ? true : flow.RequestResolveInCombatOutcome(InCombatOutcome::Pleasure, actorFormID, "mod_event_incombat_pleasure");
            const bool preserveTruce = flowActor ? TFD::HostilityController::PreserveTruceSessionForFlowHandoff(flowActor, 90.0, "incombat_pleasure_handoff") : false;
            const bool runtimeStarted = (!context.preserveCaptive && flowActor) ?
                TFD::PleasureRuntime::BeginPleasure(flowActor, TFD::PleasureRuntime::SourceContext::InCombat, "mod_event_incombat_pleasure") :
                false;
            (void)TFD::InCombat::HandleOutcomePleasureEvent(context, inCombatOutcomeEventHandlers);
            spdlog::info(
                "[TFD][Flow][R93T] incombat pleasure handoff actor={:08X} flowPleasure={} preserveTruce={} preserveCaptive={} runtimeStarted={}",
                actorFormID,
                flowPleasure ? 1 : 0,
                preserveTruce ? 1 : 0,
                context.preserveCaptive ? 1 : 0,
                runtimeStarted ? 1 : 0);
            return true;
        }

        if (name == kInCombatOutcomeRecruitEvent || name == kInCombatOutcomeJoinEnemyEvent) {
            auto actorFormID = ResolveActorFormIDFromEventArg(arg);
            if (!actorFormID) {
                actorFormID = TFD::InCombat::GetPrimaryActorFormID();
            }
            auto* flowActor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
            auto& flow = TFD::FlowController::Controller::GetSingleton();
            const bool isRecruit = name == kInCombatOutcomeRecruitEvent;
            const bool flowDone = flow.RequestResolveInCombatOutcome(
                isRecruit ? InCombatOutcome::RecruitEnemy : InCombatOutcome::JoinEnemy,
                actorFormID,
                isRecruit ? "mod_event_incombat_recruit" : "mod_event_incombat_join_enemy");
            const bool terminalComplete = (isRecruit && flowDone) ? flow.RequestCompleteTerminalContext("mod_event_incombat_recruit_complete") : false;

            const bool cycleTerminal = flowActor && TFD::InCombatGreet::IsPleasureCycleActiveForActor(flowActor);
            if (cycleTerminal) {
                const bool queuedNext = TFD::PleasureRuntime::QueueNextCycleAfterTerminal(
                    flowActor,
                    TFD::PleasureRuntime::SourceContext::InCombat,
                    isRecruit ? "incombat_cycle_pay_recruit" : "incombat_cycle_pay_join_enemy");
                const bool releasedSingle = TFD::HostilityController::ReleaseSingleTruceActorForCycle(
                    flowActor,
                    TFD::HostilityController::ReleaseReason::FlowHandoff,
                    isRecruit ? "incombat_cycle_pay_recruit" : "incombat_cycle_pay_join_enemy");
                const auto flushedDeferred = queuedNext ? std::size_t{ 0 } :
                    TFD::PleasureRuntime::FlushDeferredInCombatRecruits(isRecruit ? "incombat_cycle_pay_recruit_final" : "incombat_cycle_pay_join_enemy_final");
                TFD::InCombat::Complete(isRecruit ? "mod_event_incombat_cycle_recruit" : "mod_event_incombat_cycle_join_enemy");
                spdlog::info(
                    "[TFD][Flow][R95B] incombat cycle recruit/join event={} actor={:08X} flowDone={} terminalComplete={} queuedNext={} releasedSingle={} deferredRegistered={} policy=current_actor_only_flush_on_final",
                    std::string(name),
                    actorFormID,
                    flowDone ? 1 : 0,
                    terminalComplete ? 1 : 0,
                    queuedNext ? 1 : 0,
                    releasedSingle ? 1 : 0,
                    static_cast<unsigned int>(flushedDeferred));
                return true;
            }

            const auto flushedDeferred = TFD::PleasureRuntime::FlushDeferredInCombatRecruits(isRecruit ? "mod_event_incombat_recruit_final" : "mod_event_incombat_join_enemy_final");
            if (flowActor) {
                TFD::HostilityController::ConsumePayDialoguePassiveGuardAsPersistent(
                    flowActor,
                    isRecruit ? "incombat_recruit_persistent_owner" : "incombat_join_enemy_persistent_owner");
            }
            const bool truceReleased = flowActor ? TFD::HostilityController::ReleaseActiveTruceSessionForActor(flowActor, TFD::HostilityController::ReleaseReason::FlowHandoff, false) : false;
            TFD::InCombat::Complete(isRecruit ? "mod_event_incombat_recruit" : "mod_event_incombat_join_enemy");
            spdlog::info(
                "[TFD][Flow][R95B] incombat recruit/join terminal event={} actor={:08X} flowDone={} terminalComplete={} truceRelease={} deferredRegistered={}",
                std::string(name),
                actorFormID,
                flowDone ? 1 : 0,
                terminalComplete ? 1 : 0,
                truceReleased ? 1 : 0,
                static_cast<unsigned int>(flushedDeferred));
            return true;
        }

        if (name == kInCombatOutcomeCaptiveEvent) {
            auto actorFormID = ResolveActorFormIDFromEventArg(arg);
            if (!actorFormID) {
                actorFormID = TFD::InCombat::GetPrimaryActorFormID();
            }
            auto* flowActor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
            TFD::Bleedout::ArmSystemEventOutcomeWindow("mod_event_incombat_captive", 3.0);
            TFD::InCombat::SetDialogueOutcome(TFD::InCombat::DialogueOutcome::Captive, "mod_event_incombat_captive");

            auto& flow = TFD::FlowController::Controller::GetSingleton();
            const bool flowDone = flow.RequestResolveInCombatOutcome(InCombatOutcome::Captive, actorFormID, "mod_event_incombat_captive");
            const bool preserved = flowActor ? TFD::HostilityController::PreserveTruceSessionForFlowHandoff(flowActor, 90.0, "incombat_captive_handoff") : false;
            const bool truceReleased = flowActor ? TFD::HostilityController::ReleaseActiveTruceSessionForActor(flowActor, TFD::HostilityController::ReleaseReason::FlowHandoff, false) : false;
            if (flowDone) {
                QueueInCombatCaptiveTransition(actorFormID, "mod_event_incombat_captive");
            }
            TFD::InCombat::Complete("mod_event_incombat_captive");
            spdlog::info(
                "[TFD][Flow][R93U] incombat captive terminal actor={:08X} flowDone={} preserved={} truceRelease={} transitionQueued={}",
                actorFormID,
                flowDone ? 1 : 0,
                preserved ? 1 : 0,
                truceReleased ? 1 : 0,
                flowDone ? 1 : 0);
            return true;
        }

        if (name == kInCombatOutcomeFightEvent ||
            name == kInCombatOutcomeDoNothingEvent ||
            name == kInCombatOutcomeCancelEvent ||
            name == kInCombatOutcomeFailedEvent) {
            auto actorFormID = ResolveActorFormIDFromEventArg(arg);
            if (!actorFormID) {
                actorFormID = TFD::InCombat::GetPrimaryActorFormID();
            }

            auto* flowActor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
            auto& flow = TFD::FlowController::Controller::GetSingleton();

            const bool isDoNothing = name == kInCombatOutcomeDoNothingEvent;
            const bool isCancel = name == kInCombatOutcomeCancelEvent;
            const bool isFailed = name == kInCombatOutcomeFailedEvent;
            const auto outcome = isCancel ? InCombatOutcome::Cancel : (isFailed ? InCombatOutcome::Failed : (isDoNothing ? InCombatOutcome::DoNothing : InCombatOutcome::Fight));
            const char* reasonText = isCancel ? "mod_event_incombat_cancel" : (isFailed ? "mod_event_incombat_failed" : (isDoNothing ? "mod_event_incombat_do_nothing" : "mod_event_incombat_fight"));

            TFD::InCombat::ClearDialogueOutcome(reasonText);
            const bool flowDone = flow.RequestResolveInCombatOutcome(outcome, actorFormID, reasonText);
            const bool terminalComplete = ((isCancel || isFailed) && flowDone) ? flow.RequestCompleteTerminalContext(isFailed ? "mod_event_incombat_failed_complete" : "mod_event_incombat_cancel_complete") : false;
            const auto releaseReason = isCancel ? TFD::HostilityController::ReleaseReason::DialogueClosed : TFD::HostilityController::ReleaseReason::FightChoice;
            const auto flushedDeferred = TFD::PleasureRuntime::FlushDeferredInCombatRecruits(reasonText);
            const bool truceReleased = flowActor ? TFD::HostilityController::ReleaseActiveTruceSessionForActor(flowActor, releaseReason, false) : false;
            TFD::InCombat::Complete(reasonText);
            spdlog::info(
                "[TFD][Flow][R95B] incombat explicit outcome event={} actor={:08X} outcome={} flowDone={} terminalComplete={} truceRelease={} deferredRegistered={}",
                std::string(name),
                actorFormID,
                TFD::FlowController::Controller::ToString(outcome),
                flowDone ? 1 : 0,
                terminalComplete ? 1 : 0,
                truceReleased ? 1 : 0,
                static_cast<unsigned int>(flushedDeferred));
            return true;
        }

        if (name == kInCombatOutcomeResetEvent) {
            auto actorFormID = ResolveActorFormIDFromEventArg(arg);
            if (!actorFormID) {
                actorFormID = TFD::InCombat::GetPrimaryActorFormID();
            }
            auto* flowActor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
            TFD::InCombat::OutcomeEventContext context{};
            context.eventName = "mod_event_reset";
            context.rawEventName = eventName;
            context.actor = flowActor;
            context.actorFormID = actorFormID;
            context.inCombatState = TFD::InCombat::IsActive();
            (void)TFD::InCombat::HandleOutcomeResetEvent(context, inCombatOutcomeEventHandlers);
            auto& flow = TFD::FlowController::Controller::GetSingleton();
            const bool flowDone = flow.RequestResolveInCombatOutcome(InCombatOutcome::Fight, actorFormID, "mod_event_incombat_reset");
            const auto flushedDeferred = TFD::PleasureRuntime::FlushDeferredInCombatRecruits("mod_event_incombat_reset_final");
            const bool truceReleased = flowActor ? TFD::HostilityController::ReleaseActiveTruceSessionForActor(flowActor, TFD::HostilityController::ReleaseReason::FightChoice, false) : false;
            TFD::InCombat::Complete("mod_event_incombat_reset");
            spdlog::info("[TFD][Flow][R95B] incombat reset terminal actor={:08X} flowDone={} truceRelease={} deferredRegistered={}", actorFormID, flowDone ? 1 : 0, truceReleased ? 1 : 0, static_cast<unsigned int>(flushedDeferred));
            return true;
        }

        const TFD::Bleedout::OutcomeEventHandlers bleedOutcomeEventHandlers{
            [&](const char* rawEventName2) {
                const auto commit = TFD::Bleedout::GetTerminalCommit();
                return TFD::Bleedout::ShouldDropSystemEventBecauseFallback(rawEventName2, commit == TFD::Bleedout::TerminalCommit::Captive || commit == TFD::Bleedout::TerminalCommit::NonCaptiveFallback, TFD::Bleedout::GetTerminalCommitName(commit));
            },
            [&](const char* reason, double seconds) { TFD::Bleedout::ArmSystemEventOutcomeWindow(reason, seconds); },
            [&](TFD::Bleedout::DialogueOutcome outcome, const char* reason) { TFD::Bleedout::SetDialogueOutcome(outcome, reason); },
            [&](const char* reason) { TFD::Bleedout::ClearDialogueOutcome(reason); },
            [&](const char* reason) { TFD::Bleedout::ClearSystemEventOutcomeWindow(reason); },
            [&](std::uint32_t actorFormID, const char* reason) -> bool {
                auto& flow = TFD::FlowController::Controller::GetSingleton();
                return flow.RequestCaptivePleasureFromModEvent(actorFormID, reason ? reason : "mod_event_pleasure");
            },
            [&](const char* reason) {
                if (g_outcomeRuntimeProviders.preparePlayerForCaptivePleasureScene) {
                    g_outcomeRuntimeProviders.preparePlayerForCaptivePleasureScene(reason);
                }
            },
            [&](const char* reason) {
                if (g_outcomeRuntimeProviders.completeCaptivePleasureHandoff) {
                    g_outcomeRuntimeProviders.completeCaptivePleasureHandoff(reason);
                }
            },
            [&](const char* reason) {
                if (g_outcomeRuntimeProviders.preparePlayerForBleedoutPleasureScene) {
                    g_outcomeRuntimeProviders.preparePlayerForBleedoutPleasureScene(reason);
                }
            },
            [&](const char* reason) {
                if (g_outcomeRuntimeProviders.completeBleedPleasureHandoff) {
                    g_outcomeRuntimeProviders.completeBleedPleasureHandoff(reason ? reason : "bleed_pleasure_handoff");
                }
            }
        };

        if (name == kBleedoutOutcomePayEvent) {
            const auto actorFormID = ResolveBleedFlowActorFormIDFromProviders();
            auto* flowActor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
            const auto prePayCommit = TFD::Bleedout::GetTerminalCommit();
            const bool protectedTerminalCommit =
                prePayCommit == TFD::Bleedout::TerminalCommit::Captive ||
                prePayCommit == TFD::Bleedout::TerminalCommit::NonCaptiveFallback;
            TFD::Bleedout::OutcomeEventContext context{};
            context.eventName = "mod_event_pay";
            context.rawEventName = eventName;
            context.actor = flowActor;
            context.actorFormID = actorFormID;
            context.inBleedState = IsBleedStateActiveFromProviders();
            const bool handledPay = TFD::Bleedout::HandleOutcomePayEvent(context, bleedOutcomeEventHandlers);
            if (handledPay && context.inBleedState && !protectedTerminalCommit && g_outcomeRuntimeProviders.completeBleedPayRelease) {
                spdlog::info(
                    "[TFD][Flow][R9] immediate bleedout pay terminal completion actor={:08X} rawEvent={} reason=mod_event_pay",
                    actorFormID,
                    eventName ? eventName : "<null>");
                g_outcomeRuntimeProviders.completeBleedPayRelease("mod_event_pay_immediate_terminal");
            }
            return true;
        }

        if (name == kBleedoutOutcomePleasureEvent) {
            const auto actorFormID = ResolveBleedFlowActorFormIDFromProviders();
            auto* flowActor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
            auto& flow = TFD::FlowController::Controller::GetSingleton();
            const bool bleedActive = IsBleedStateActiveFromProviders();
            const bool captiveEscapeBleedout = bleedActive && IsCaptiveEscapeGuardActive(flow);
            TFD::Bleedout::OutcomeEventContext context{};
            context.eventName = "mod_event_pleasure";
            context.rawEventName = eventName;
            context.actor = flowActor;
            context.actorFormID = actorFormID;
            context.inBleedState = bleedActive;
            context.preserveCaptive = captiveEscapeBleedout || TFD::Captive::IsCaptivePassiveHoldActive() || TFD::Captive::IsEscapeBleedoutActive();
            if (captiveEscapeBleedout) {
                spdlog::info(
                    "[TFD][Flow][R273A] bleedout pleasure preserved captive escape-break actor={:08X} reason=mod_event_pleasure",
                    actorFormID);
            }
            (void)TFD::Bleedout::HandleOutcomePleasureEvent(context, bleedOutcomeEventHandlers);
            return true;
        }

        if (name == kBleedoutOutcomeCaptiveEvent) {
            const auto actorFormID = ResolveBleedFlowActorFormIDFromProviders();
            auto* flowActor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
            auto& flow = TFD::FlowController::Controller::GetSingleton();
            const bool bleedActive = IsBleedStateActiveFromProviders();
            const bool captiveEscapeBleedout = bleedActive && IsCaptiveEscapeGuardActive(flow);
            if (captiveEscapeBleedout) {
                const bool recaptured = TFD::Captive::CommitRecapture(flowActor, "captive_escape_bleedout_captive_choice_recapture");
                spdlog::info(
                    "[TFD][Flow][C47] captive escape-bleedout captive choice -> recapture actor={:08X} bleedActive={} recaptured={}",
                    actorFormID,
                    bleedActive ? 1 : 0,
                    recaptured ? 1 : 0);
                return true;
            }

            TFD::Bleedout::OutcomeEventContext context{};
            context.eventName = "mod_event_captive";
            context.rawEventName = eventName;
            context.actor = flowActor;
            context.actorFormID = actorFormID;
            context.inBleedState = bleedActive;
            (void)TFD::Bleedout::HandleOutcomeCaptiveEvent(context, bleedOutcomeEventHandlers);
            return true;
        }

        if (name == kBleedoutOutcomeResetEvent) {
            TFD::Bleedout::OutcomeEventContext context{};
            context.eventName = "mod_event_reset";
            context.rawEventName = eventName;
            (void)TFD::Bleedout::HandleOutcomeResetEvent(context, bleedOutcomeEventHandlers);
            return true;
        }

        return false;
    }

    bool HandlePassiveBreakHitEvent(RE::Actor* causeActor, RE::Actor* targetActor)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!causeActor || !targetActor || !player || causeActor != player) {
            return false;
        }

        if (g_passiveRuntimeProviders.cancelReleaseFollowGraceForActor) {
            g_passiveRuntimeProviders.cancelReleaseFollowGraceForActor(targetActor, "player_attack_cancel");
        }

        return HandlePassiveInvalidationAgainstActor(targetActor, "player_hit", false);
    }

    bool HandlePassiveBreakModEvent(const char* eventName, const char* strArg)
    {
        const std::string_view name = eventName ? std::string_view(eventName) : std::string_view{};
        if (name != kPassiveBreakCrimeEvent && name != kPassiveBreakPickpocketEvent) {
            return false;
        }

        auto* actor = ResolveActorFromEventArg(strArg ? std::string_view(strArg) : std::string_view{});
        const char* reason = name == kPassiveBreakCrimeEvent ? "player_crime" : "player_pickpocket";
        if (!actor) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player && TFD::Captive::GetStateFlag() &&
                TFD::HostilityController::HasVisibleCaptiveCombatWitness(player, nullptr, reason)) {
                (void)TFD::Captive::TriggerPlayerAggressionEscape(nullptr, reason);
                spdlog::info("[TFD][PassiveBreak] event={} handled by visible captive witness reason={} arg=invalid",
                    std::string(name),
                    reason);
                return true;
            }

            spdlog::warn("[TFD][PassiveBreak] event={} ignored reason=invalid_actor arg={}",
                std::string(name),
                strArg ? strArg : "");
            return true;
        }

        (void)HandlePassiveInvalidationAgainstActor(actor, reason, true);
        return true;
    }

    bool QueueBridgeModEvent(const char* eventName, RE::TESForm* sender, const char* strArg, float numArg)
    {
        if (!eventName || !eventName[0]) {
            return false;
        }

        auto* task = SKSE::GetTaskInterface();
        if (!task) {
            spdlog::warn("[TFD][Flow] QueueBridgeModEvent failed: no task interface event={}", eventName);
            return false;
        }

        const std::string name{ eventName };
        const std::string sarg{ strArg ? strArg : "" };
        const float narg = numArg;

        std::uint32_t actorHandle = 0;
        RE::FormID senderFormID = 0;

        if (sender) {
            senderFormID = sender->GetFormID();
            if (auto* actor = sender->As<RE::Actor>()) {
                actorHandle = actor->GetHandle().native_handle();
            }
        }

        task->AddTask([name, sarg, narg, actorHandle, senderFormID]() {
            RE::TESForm* outSender = nullptr;

            if (actorHandle != 0) {
                auto actorSp = RE::Actor::LookupByHandle(actorHandle);
                outSender = actorSp.get();
                if (!outSender && senderFormID != 0) {
                    outSender = RE::TESForm::LookupByID(senderFormID);
                }
            }
            else if (senderFormID != 0) {
                outSender = RE::TESForm::LookupByID(senderFormID);
            }

            auto* src = SKSE::GetModCallbackEventSource();
            if (!src) {
                spdlog::warn("[TFD][Flow] QueueBridgeModEvent dispatch skipped: no callback source event={} sender={:08X}", name, senderFormID);
                return;
            }

            SKSE::ModCallbackEvent ev{ name.c_str(), sarg.c_str(), narg, outSender };
            src->SendEvent(&ev);

            spdlog::info("[TFD][Flow] QueueBridgeModEvent dispatch event={} sender={:08X} resolved={:08X}",
                name,
                senderFormID,
                outSender ? outSender->GetFormID() : 0u);
            });

        return true;
    }

    DialogueContextKind GetDialogueContextKind()
    {
        auto& flow = Controller::GetSingleton();
        const auto snapshot = flow.GetSnapshot();

        if (snapshot.root == RootFlow::Captive && snapshot.captiveMode == CaptiveMode::JoinedEnemy) {
            return DialogueContextKind::JoinedEnemy;
        }

        // R274A: terminal dialogue overlays must win over the persistent Captive
        // base context.  Otherwise manual activation after an AfterPleasure close is
        // classified as generic Captive/Calling Captor and steals the active route.
        if (flow.IsAfterPleasureSubFlowActive()) {
            return DialogueContextKind::AfterPleasure;
        }

        if (TFD::Captive::GetStateFlag()) {
            return DialogueContextKind::Captive;
        }

        if (IsBleedoutRuntimeActive()) {
            return DialogueContextKind::Bleedout;
        }

        if (snapshot.root == RootFlow::PreCombat && snapshot.gate == DecisionGate::Truce) {
            return DialogueContextKind::PreCombat;
        }

        if (snapshot.root == RootFlow::Bleedout && snapshot.gate == DecisionGate::PlayerBleedout) {
            return DialogueContextKind::Bleedout;
        }

        if (snapshot.root == RootFlow::InCombat && snapshot.gate == DecisionGate::Truce) {
            return DialogueContextKind::InCombat;
        }

        return DialogueContextKind::None;
    }

    const char* GetDialogueContextName()
    {
        switch (GetDialogueContextKind()) {
        case DialogueContextKind::PreCombat: return "PreCombat";
        case DialogueContextKind::InCombat: return "InCombat";
        case DialogueContextKind::Bleedout: return "Bleedout";
        case DialogueContextKind::Captive: return "Captive";
        case DialogueContextKind::AfterPleasure: return "AfterPleasure";
        case DialogueContextKind::JoinedEnemy: return "JoinedEnemy";
        default: return "None";
        }
    }

    bool IsDialogueContextActive()
    {
        return GetDialogueContextKind() != DialogueContextKind::None;
    }

    PassiveHoldKind GetPassiveHoldKind()
    {
        const auto snapshot = Controller::GetSingleton().GetSnapshot();
        auto& flow = Controller::GetSingleton();

        if (snapshot.root == RootFlow::Captive && snapshot.captiveMode == CaptiveMode::JoinedEnemy) {
            return PassiveHoldKind::JoinedEnemy;
        }

        if (TFD::PleasureRuntime::IsPassiveLockActive() || flow.IsPleasureSubFlowActive() || flow.IsAfterPleasureSubFlowActive()) {
            return PassiveHoldKind::Pleasure;
        }

        if (TFD::Captive::IsCaptivePassiveHoldActive()) {
            return PassiveHoldKind::Captive;
        }

        if (HasReleaseFollowGraceRuntime()) {
            return PassiveHoldKind::Grace;
        }

        if (IsBleedoutRuntimeActive() || flow.IsTerminalDialogueGateActive()) {
            return PassiveHoldKind::Dialogue;
        }

        return PassiveHoldKind::None;
    }

    const char* GetPassiveHoldName()
    {
        switch (GetPassiveHoldKind()) {
        case PassiveHoldKind::Dialogue: return "Dialogue";
        case PassiveHoldKind::Grace: return "Grace";
        case PassiveHoldKind::Pleasure: return "Pleasure";
        case PassiveHoldKind::Captive: return "Captive";
        case PassiveHoldKind::JoinedEnemy: return "JoinedEnemy";
        default: return "None";
        }
    }

    bool IsPassiveHoldActive()
    {
        return GetPassiveHoldKind() != PassiveHoldKind::None;
    }

    bool IsPassiveHoldProtectedHandoff()
    {
        const auto holdKind = GetPassiveHoldKind();
        return holdKind == PassiveHoldKind::Pleasure ||
            holdKind == PassiveHoldKind::Captive ||
            holdKind == PassiveHoldKind::JoinedEnemy;
    }

    bool IsPleasureLockActive()
    {
        return TFD::PleasureRuntime::IsPassiveLockActive();
    }

    bool IsPreCombatBlocked()
    {
        const auto snapshot = Controller::GetSingleton().GetSnapshot();
        if (snapshot.root == RootFlow::Rescue ||
            snapshot.root == RootFlow::Recovery ||
            snapshot.root == RootFlow::LeftForDead) {
            return true;
        }

        switch (GetDialogueContextKind()) {
        case DialogueContextKind::Captive:
        case DialogueContextKind::AfterPleasure:
        case DialogueContextKind::JoinedEnemy:
        case DialogueContextKind::Bleedout:
            return true;
        default:
            break;
        }

        const auto holdKind = GetPassiveHoldKind();
        return holdKind == PassiveHoldKind::Pleasure ||
            holdKind == PassiveHoldKind::Captive ||
            holdKind == PassiveHoldKind::JoinedEnemy;
    }

    bool HandlePassiveInvalidationAgainstActor(RE::Actor* actor, const char* reason, bool severeCrime)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!actor || !player || actor == player) {
            return false;
        }

        const bool covered = IsActorCoveredByCurrentPassiveContext(actor);
        const auto contextKind = GetDialogueContextKind();
        const auto holdKind = GetPassiveHoldKind();
        const bool playerHit = !severeCrime && reason && std::string_view(reason) == "player_hit";

        if (playerHit && TFD::Actor::Ops::IsDefeatedEnemyKnocked(actor)) {
            spdlog::info("[TFD][PassiveBreak][R306A] ignore player hit on defeated enemy actor={:08X} ctx={} hold={} covered={}",
                actor->GetFormID(),
                GetDialogueContextName(),
                GetPassiveHoldName(),
                covered ? 1 : 0);
            return false;
        }

        if (playerHit &&
            contextKind == DialogueContextKind::Captive &&
            Controller::GetSingleton().IsCaptiveCombatEscapeBreakContextActive()) {
            spdlog::info("[TFD][PassiveBreak][R300B] ignore repeated captive combat-owned player hit actor={:08X} ctx={} hold={} covered={}",
                actor->GetFormID(),
                GetDialogueContextName(),
                GetPassiveHoldName(),
                covered ? 1 : 0);
            return false;
        }

        if (playerHit && !covered) {
            if (contextKind != DialogueContextKind::None || holdKind != PassiveHoldKind::None) {
                spdlog::info("[TFD][PassiveBreak][R300B] ignore player hit without actor ownership actor={:08X} ctx={} hold={} covered=0",
                    actor->GetFormID(),
                    GetDialogueContextName(),
                    GetPassiveHoldName());
            }
            return false;
        }

        if (!covered && holdKind == PassiveHoldKind::None) {
            return false;
        }

        const auto actorId = actor->GetFormID();
        spdlog::info("[TFD][PassiveBreak] actor={:08X} reason={} ctx={} hold={} covered={} severeCrime={}",
            actorId,
            reason ? reason : "unknown",
            GetDialogueContextName(),
            GetPassiveHoldName(),
            covered ? 1 : 0,
            severeCrime ? 1 : 0);

        bool changed = false;
        if (!playerHit && g_passiveRuntimeProviders.clearAllReleaseFollowGrace) {
            g_passiveRuntimeProviders.clearAllReleaseFollowGrace(reason ? reason : "passive_break");
        }

        changed |= TFD::HostilityController::ReleaseActiveTruceSessionForActor(actor, TFD::Tame::ReleaseReason::PlayerAggression, false);
        if (auto* primary = ResolveCurrentPassivePrimaryActor(); primary && primary != actor) {
            changed |= TFD::HostilityController::ReleaseActiveTruceSessionForActor(primary, TFD::Tame::ReleaseReason::PlayerAggression, false);
        }

        if (IsBleedoutRuntimeActive() || TFD::Bleedout::GetTruceSessionID() != 0) {
            if (g_passiveRuntimeProviders.releaseBleedPlayerAggressionTruce) {
                g_passiveRuntimeProviders.releaseBleedPlayerAggressionTruce();
            }
            if (g_passiveRuntimeProviders.clearAllBleedLocks) {
                g_passiveRuntimeProviders.clearAllBleedLocks(reason ? reason : "passive_break");
            }
            if (g_passiveRuntimeProviders.clearBleedBridgeAliases) {
                g_passiveRuntimeProviders.clearBleedBridgeAliases(actor, reason ? reason : "passive_break");
            }
            changed = true;
        }

        if (TFD::PleasureRuntime::IsActive() || TFD::PleasureRuntime::IsBlocking()) {
            TFD::PleasureRuntime::Break(reason ? reason : "passive_break", true, true, true);
            changed = true;
        }

        TFD::HostilityController::ClearAggressionClamp();
        TFD::Actor::Ops::ClearAggressorFactionContext();

        auto& flow = Controller::GetSingleton();
        switch (contextKind) {
        case DialogueContextKind::JoinedEnemy:
            TFD::Captive::SetRuntimeState(false, TFD::Captive::PhaseValue::None);
            flow.ResetRuntime(reason ? reason : "player_aggression_joined_enemy");
            changed = true;
            break;
        case DialogueContextKind::Captive:
            changed = TFD::Captive::TriggerPlayerAggressionEscape(actor, reason ? reason : "player_aggression_captive") || changed;
            break;
        case DialogueContextKind::AfterPleasure:
            flow.ResetRuntime(reason ? reason : "player_aggression_afterpleasure");
            changed = true;
            break;
        default:
            break;
        }


        return changed || covered;
    }

    Snapshot Controller::GetSnapshot() const
    {
        std::scoped_lock lk(_lock);
        return ProjectExternalSnapshot(_snapshot);
    }

    ObservedMainStateSnapshot Controller::GetObservedMainStateSnapshot() const
    {
        ObservedMainStateSnapshot cached{};
        bool hasCached = false;
        Snapshot flowSnapshot{};
        bool combatActive = false;
        {
            std::scoped_lock lk(_lock);
            cached = _observedSnapshot;
            hasCached = _hasObservedSnapshot;
            flowSnapshot = _snapshot;
            combatActive = _combatActive;
        }

        if (hasCached) {
            return cached;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        return BuildObservedMainStateSnapshotImpl(flowSnapshot, combatActive, player);
    }

    ObservedMainState Controller::GetObservedMainState() const
    {
        return GetObservedMainStateSnapshot().observed;
    }

    void Controller::TickObservedMainStateDiagnostic(RE::Actor* player, std::string_view reason)
    {
        const auto now = std::chrono::steady_clock::now();
        if (g_hasObservedDiagnostic && (now - g_lastObservedDiagnosticTick) < kObservedDiagnosticTickInterval) {
            return;
        }
        g_lastObservedDiagnosticTick = now;

        Snapshot flowSnapshot{};
        bool combatActive = false;
        {
            std::scoped_lock lk(_lock);
            flowSnapshot = _snapshot;
            combatActive = _combatActive;
        }

        auto observed = BuildObservedMainStateSnapshotImpl(flowSnapshot, combatActive, player);
        {
            std::scoped_lock lk(_lock);
            _observedSnapshot = observed;
            _hasObservedSnapshot = true;
        }

        if (AutoInterruptInCombatAfterPleasureIfNeeded(observed, reason)) {
            return;
        }

        if (AutoCompleteStaleInCombatNeutralIfNeeded(observed, now, reason)) {
            return;
        }

        if (!ShouldLogObservedMainStateDiagnostic(observed, now)) {
            return;
        }

        LogObservedMainStateDiagnostic(observed, reason);
        g_lastObservedDiagnostic = observed;
        g_hasObservedDiagnostic = true;
        g_lastObservedDiagnosticLog = now;
    }

    bool Controller::RequestPreCombat(std::uint32_t actorFormID, std::string_view reason)
    {
        return BeginPreCombat(actorFormID, reason);
    }

    bool Controller::RequestInCombat(std::uint32_t actorFormID, std::string_view reason)
    {
        return BeginInCombat(actorFormID, reason);
    }

    bool Controller::RequestCaptive(std::uint32_t actorFormID, CaptiveMode mode, std::string_view reason)
    {
        return BeginCaptive(actorFormID, mode, reason);
    }


    bool Controller::RequestTruceDecision(std::uint32_t actorFormID, std::string_view reason)
    {
        return BeginTruceDecision(actorFormID, reason);
    }

    bool Controller::RequestPlayerBleedoutDecision(std::uint32_t actorFormID, std::string_view reason)
    {
        return BeginPlayerBleedoutDecision(actorFormID, reason);
    }

    bool Controller::RequestEnemyBleedoutDecision(std::uint32_t actorFormID, std::string_view reason)
    {
        return BeginEnemyBleedoutDecision(actorFormID, reason);
    }

    bool Controller::RequestResolvePreCombatOutcome(PreCombatOutcome outcome, std::uint32_t actorFormID, std::string_view reason)
    {
        return ResolvePreCombatOutcome(outcome, actorFormID, reason);
    }

    bool Controller::RequestResolveInCombatOutcome(InCombatOutcome outcome, std::uint32_t actorFormID, std::string_view reason)
    {
        return ResolveInCombatOutcome(outcome, actorFormID, reason);
    }

    bool Controller::RequestResolveBleedoutOutcome(BleedoutOutcome outcome, std::uint32_t actorFormID, std::string_view reason)
    {
        return ResolveBleedoutOutcome(outcome, actorFormID, reason);
    }


    bool Controller::RequestResolveCaptiveOutcome(CaptiveOutcome outcome, std::uint32_t actorFormID, std::string_view reason)
    {
        return ResolveCaptiveOutcome(outcome, actorFormID, reason);
    }

    bool Controller::RequestResolveInCombatPleasure(std::uint32_t actorFormID, std::string_view reason)
    {
        return ResolveInCombatOutcome(InCombatOutcome::Pleasure, actorFormID, reason);
    }

    bool Controller::RequestResolveInCombatTerminal(std::uint32_t actorFormID, std::string_view reason)
    {
        return ResolveInCombatOutcome(InCombatOutcome::Cancel, actorFormID, reason);
    }

    bool Controller::RequestBeginAfterPleasure(std::uint32_t actorFormID, std::string_view reason)
    {
        return BeginAfterPleasure(actorFormID, reason);
    }

    bool Controller::RequestBeginPleasureFailed(std::uint32_t actorFormID, int sourceFlow, std::string_view reason)
    {
        return BeginPleasureFailed(actorFormID, sourceFlow, reason);
    }

    bool Controller::RequestResumePleasureFromPleasureFailed(std::uint32_t actorFormID, int sourceFlow, std::string_view reason)
    {
        return ResumePleasureFromPleasureFailed(actorFormID, sourceFlow, reason);
    }

    bool Controller::RequestCompleteAfterPleasure(std::string_view reason)
    {
        return CompleteAfterPleasure(reason);
    }

    bool Controller::RequestCompleteTerminalContext(std::string_view reason)
    {
        return CompleteTerminalContext(reason);
    }

    bool Controller::RequestAbortPreCombat(std::uint32_t actorFormID, std::string_view reason)
    {
        return AbortPreCombat(actorFormID, reason);
    }

    bool Controller::RequestCaptiveFromModEvent(std::uint32_t actorFormID, std::string_view reason)
    {
        return BeginCaptiveFromModEvent(actorFormID, reason);
    }

    bool Controller::RequestCaptivePleasureFromModEvent(std::uint32_t actorFormID, std::string_view reason)
    {
        return BeginCaptivePleasureFromModEvent(actorFormID, reason);
    }

    void Controller::ResetRuntime(std::string_view reason)
    {
        std::scoped_lock lk(_lock);
        _combatActive = false;
        ClearAllLocked();
        RefreshFlowGlobalsLocked();
        LogFlowSnapshot("ResetRuntime", reason, _snapshot);
    }

    void Controller::ResetForLoad(std::string_view reason)
    {
        std::scoped_lock lk(_lock);
        _combatActive = false;
        ClearAllLocked();
        RefreshFlowGlobalsLocked();
        LogFlowSnapshot("ResetForLoad", reason, _snapshot);
    }

    bool Controller::BeginPreCombat(std::uint32_t actorFormID, std::string_view reason)
    {
        std::scoped_lock lk(_lock);
        if (_snapshot.root == RootFlow::Captive ||
            _snapshot.sub != SubFlow::None || _snapshot.terminalResolved) {
            return RejectLocked("BeginPreCombat", reason);
        }
        if (_snapshot.root == RootFlow::PreCombat && !GuardPreCombatOwnerLocked(actorFormID, "BeginPreCombat", reason)) {
            return false;
        }
        _combatActive = false;
        const bool ok = BeginRootLocked(RootFlow::PreCombat, actorFormID, reason);
        RefreshFlowGlobalsLocked();
        return ok;
    }

    bool Controller::BeginInCombat(std::uint32_t actorFormID, std::string_view reason)
    {
        std::scoped_lock lk(_lock);
        if (_snapshot.root == RootFlow::Captive) {
            return RejectLocked("BeginInCombat", reason);
        }
        _combatActive = true;
        const bool ok = BeginRootLocked(RootFlow::InCombat, actorFormID, reason);
        RefreshFlowGlobalsLocked();
        return ok;
    }

    bool Controller::RequestInCombatEscapeBreak(std::uint32_t actorFormID, std::string_view reason)
    {
        std::scoped_lock lk(_lock);
        if (_snapshot.root == RootFlow::Rescue ||
            _snapshot.root == RootFlow::Recovery || _snapshot.root == RootFlow::LeftForDead) {
            return RejectLocked("RequestInCombatEscapeBreak", reason);
        }

        _snapshot.root = RootFlow::InCombat;
        _snapshot.contextRoot = RootFlow::Captive;
        _snapshot.gate = DecisionGate::None;
        if (_snapshot.captiveMode == CaptiveMode::None) {
            _snapshot.captiveMode = CaptiveMode::Kidnapped;
        }
        _snapshot.sub = SubFlow::InCombatEscapeBreak;
        _snapshot.terminalResolved = false;
        _combatActive = true;
        SetPrimaryActorLocked(actorFormID);
        BumpTokenLocked();
        RefreshFlowGlobalsLocked();
        LogFlowSnapshot("RequestInCombatEscapeBreak", reason, _snapshot, actorFormID, "CaptiveEscapeBreak");
        return true;
    }

    bool Controller::BeginCaptive(std::uint32_t actorFormID, CaptiveMode mode, std::string_view reason)
    {
        std::scoped_lock lk(_lock);
        const bool ok = BeginCaptiveLocked(actorFormID, mode, reason);
        RefreshFlowGlobalsLocked();
        return ok;
    }


    bool Controller::BeginTruceDecision(std::uint32_t actorFormID, std::string_view reason)
    {
        std::scoped_lock lk(_lock);
        if (_snapshot.root != RootFlow::PreCombat && _snapshot.root != RootFlow::InCombat) {
            return RejectLocked("BeginTruceDecision", reason);
        }
        if (_snapshot.root == RootFlow::PreCombat && !GuardPreCombatOwnerLocked(actorFormID, "BeginTruceDecision", reason)) {
            return false;
        }
        if (_snapshot.root == RootFlow::InCombat && !GuardInCombatOwnerLocked(actorFormID, "BeginTruceDecision", reason)) {
            return false;
        }
        _snapshot.gate = DecisionGate::Truce;
        SetPrimaryActorLocked(actorFormID);
        RefreshFlowGlobalsLocked();
        LogFlowSnapshot("BeginTruceDecision", reason, _snapshot, actorFormID);
        return true;
    }

    bool Controller::BeginPlayerBleedoutDecision(std::uint32_t actorFormID, std::string_view reason)
    {
        std::scoped_lock lk(_lock);

        if (_snapshot.root == RootFlow::InCombat &&
            _snapshot.contextRoot == RootFlow::Captive &&
            _snapshot.sub == SubFlow::InCombatEscapeBreak &&
            !_snapshot.terminalResolved) {
            _snapshot.root = RootFlow::Bleedout;
            _snapshot.contextRoot = RootFlow::Captive;
            _snapshot.gate = DecisionGate::PlayerBleedout;
            _snapshot.sub = SubFlow::BleedoutEscapeBreak;
            if (_snapshot.captiveMode == CaptiveMode::None) {
                _snapshot.captiveMode = CaptiveMode::Kidnapped;
            }
            SetPrimaryActorLocked(actorFormID);
            _combatActive = true;
            RefreshFlowGlobalsLocked();
            LogFlowSnapshot("BeginPlayerBleedoutDecision", reason, _snapshot, actorFormID, "bleedout_escape_break_preserve_captive");
            return true;
        }

        if (_snapshot.root == RootFlow::Bleedout &&
            _snapshot.contextRoot == RootFlow::Captive &&
            _snapshot.sub == SubFlow::BleedoutEscapeBreak &&
            !_snapshot.terminalResolved) {
            _snapshot.gate = DecisionGate::PlayerBleedout;
            SetPrimaryActorLocked(actorFormID);
            _combatActive = true;
            RefreshFlowGlobalsLocked();
            LogFlowSnapshot("BeginPlayerBleedoutDecision", reason, _snapshot, actorFormID, "bleedout_escape_break_duplicate");
            return true;
        }

        if (_snapshot.root == RootFlow::Captive) {
            if (_snapshot.sub == SubFlow::EscapeAttempt || _snapshot.sub == SubFlow::EscapeFailed || _snapshot.sub == SubFlow::Recapture) {
                if (_snapshot.gate == DecisionGate::PlayerBleedout && _snapshot.sub == SubFlow::EscapeFailed) {
                    if (actorFormID != 0) {
                        SetPrimaryActorLocked(actorFormID);
                    }
                    _combatActive = true;
                    RefreshFlowGlobalsLocked();
                    LogFlowSnapshot("BeginPlayerBleedoutDecision", reason, _snapshot, actorFormID, "escape_rebleed_duplicate_ignored");
                    return true;
                }
                _snapshot.gate = DecisionGate::PlayerBleedout;
                if (_snapshot.sub == SubFlow::EscapeAttempt) {
                    _snapshot.sub = SubFlow::EscapeFailed;
                }
                SetPrimaryActorLocked(actorFormID);
                _combatActive = true;
                RefreshFlowGlobalsLocked();
                LogFlowSnapshot("BeginPlayerBleedoutDecision", reason, _snapshot, actorFormID, "escape_rebleed_preserve_captive");
                return true;
            }
            return RejectLocked("BeginPlayerBleedoutDecision", reason);
        }

        if (_snapshot.root != RootFlow::None && _snapshot.root != RootFlow::InCombat && _snapshot.root != RootFlow::Bleedout) {
            return RejectLocked("BeginPlayerBleedoutDecision", reason);
        }

        if (_snapshot.root != RootFlow::Bleedout) {
            ClearDecisionLocked();
            ClearSubLocked();
            ClearTerminalLocked();
            _snapshot.root = RootFlow::Bleedout;
            _snapshot.contextRoot = RootFlow::Bleedout;
            _snapshot.captiveMode = CaptiveMode::None;
            BumpTokenLocked();
        }

        _snapshot.gate = DecisionGate::PlayerBleedout;
        SetPrimaryActorLocked(actorFormID);
        _combatActive = true;
        RefreshFlowGlobalsLocked();
        LogFlowSnapshot("BeginPlayerBleedoutDecision", reason, _snapshot, actorFormID);
        return true;
    }

    bool Controller::BeginEnemyBleedoutDecision(std::uint32_t actorFormID, std::string_view reason)
    {
        std::scoped_lock lk(_lock);
        if (_snapshot.root != RootFlow::InCombat) {
            return RejectLocked("BeginEnemyBleedoutDecision", reason);
        }
        _snapshot.gate = DecisionGate::EnemyBleedout;
        SetPrimaryActorLocked(actorFormID);
        RefreshFlowGlobalsLocked();
        LogFlowSnapshot("BeginEnemyBleedoutDecision", reason, _snapshot, actorFormID);
        return true;
    }

    bool Controller::ResolvePreCombatOutcome(PreCombatOutcome outcome, std::uint32_t actorFormID, std::string_view reason)
    {
        std::scoped_lock lk(_lock);

        // PreCombat terminal follow-up choices can arrive after the root has already
        // moved into a terminal/after-pleasure context. In that valid state the
        // snapshot root is None, while contextRoot/sub still preserve PreCombat
        // ownership. Do not reject Captive/JoinEnemy/Fight-style terminal commits
        // just because Pleasure/AfterPleasure has temporarily cleared root.
        const bool validPreCombatRoot =
            _snapshot.root == RootFlow::PreCombat ||
            _snapshot.contextRoot == RootFlow::PreCombat ||
            _snapshot.sub == SubFlow::PreCombatPayFollowup ||
            _snapshot.sub == SubFlow::PreCombatPleasure ||
            _snapshot.sub == SubFlow::PreCombatAfterPleasure;

        if (!validPreCombatRoot) {
            return RejectLocked("ResolvePreCombatOutcome", reason);
        }
        if (!GuardPreCombatOwnerLocked(actorFormID, "ResolvePreCombatOutcome", reason)) {
            return false;
        }

        bool handled = false;
        switch (outcome) {
        case PreCombatOutcome::Pay:
            _snapshot.contextRoot = RootFlow::PreCombat;
            _snapshot.gate = DecisionGate::None;
            _snapshot.captiveMode = CaptiveMode::None;
            _snapshot.sub = SubFlow::PreCombatPayFollowup;
            _snapshot.terminalResolved = false;
            SetPrimaryActorLocked(actorFormID);
            handled = true;
            break;
        case PreCombatOutcome::Pleasure:
            handled = EnterTerminalContextLocked(RootFlow::PreCombat, SubFlow::PreCombatPleasure, actorFormID, reason);
            break;
        case PreCombatOutcome::Fight:
            _combatActive = true;
            handled = TransitionRootLocked(RootFlow::InCombat, actorFormID, reason);
            break;
        case PreCombatOutcome::Captive:
            handled = BeginCaptiveLocked(actorFormID, CaptiveMode::Surrendered, reason);
            break;
        case PreCombatOutcome::JoinEnemy:
            handled = BeginCaptiveLocked(actorFormID, CaptiveMode::JoinedEnemy, reason);
            break;
        case PreCombatOutcome::RecruitEnemy:
        case PreCombatOutcome::Release:
        case PreCombatOutcome::Follow:
            // W45: Release can be a terminal AfterPleasure/Redo rejection outcome,
            // not only a Pay follow-up.  W44 correctly sent a 10s release/grace
            // event, but native rejected the flow because sub=PreCombatAfterPleasure,
            // leaving ctx/sub alive and causing red flicker + fast draw/sheath loops.
            if (_snapshot.sub != SubFlow::PreCombatPayFollowup &&
                _snapshot.sub != SubFlow::PreCombatPleasure &&
                _snapshot.sub != SubFlow::PreCombatAfterPleasure) {
                return RejectLocked("ResolvePreCombatOutcome", reason);
            }
            handled = EnterTerminalContextLocked(RootFlow::PreCombat, SubFlow::None, actorFormID, reason);
            break;
        case PreCombatOutcome::Cancel:
        case PreCombatOutcome::Failed:
        case PreCombatOutcome::DoNothing:
            handled = EnterTerminalContextLocked(RootFlow::PreCombat, SubFlow::None, actorFormID, reason);
            break;
        case PreCombatOutcome::None:
        default:
            return RejectLocked("ResolvePreCombatOutcome", reason);
        }
        RefreshFlowGlobalsLocked();
        LogFlowSnapshot("ResolvePreCombatOutcome", reason, _snapshot, actorFormID, ToString(outcome));
        return handled;
    }

    bool Controller::ResolveInCombatOutcome(InCombatOutcome outcome, std::uint32_t actorFormID, std::string_view reason)
    {
        std::scoped_lock lk(_lock);

        const bool validRoot = _snapshot.root == RootFlow::InCombat ||
            _snapshot.contextRoot == RootFlow::InCombat ||
            _snapshot.sub == SubFlow::InCombatPayFollowup ||
            _snapshot.sub == SubFlow::InCombatPleasure ||
            _snapshot.sub == SubFlow::InCombatAfterPleasure;

        if (!validRoot) {
            return RejectLocked("ResolveInCombatOutcome", reason);
        }
        if (!GuardInCombatOwnerLocked(actorFormID, "ResolveInCombatOutcome", reason)) {
            return false;
        }

        bool handled = false;
        switch (outcome) {
        case InCombatOutcome::Pay:
            if (!(_snapshot.root == RootFlow::InCombat && _snapshot.gate == DecisionGate::Truce)) {
                return RejectLocked("ResolveInCombatOutcomePay", reason);
            }
            _combatActive = false;
            _snapshot.contextRoot = RootFlow::InCombat;
            _snapshot.gate = DecisionGate::None;
            _snapshot.captiveMode = CaptiveMode::None;
            _snapshot.sub = SubFlow::InCombatPayFollowup;
            _snapshot.terminalResolved = false;
            SetPrimaryActorLocked(actorFormID);
            handled = true;
            break;
        case InCombatOutcome::Pleasure:
            _combatActive = false;
            handled = EnterTerminalContextLocked(RootFlow::InCombat, SubFlow::InCombatPleasure, actorFormID, reason);
            break;
        case InCombatOutcome::Fight:
        case InCombatOutcome::DoNothing:
            _combatActive = true;
            handled = TransitionRootLocked(RootFlow::InCombat, actorFormID, reason);
            _snapshot.gate = DecisionGate::None;
            _snapshot.sub = SubFlow::None;
            _snapshot.terminalResolved = false;
            break;
        case InCombatOutcome::Captive:
            handled = BeginCaptiveLocked(actorFormID, CaptiveMode::Surrendered, reason);
            break;
        case InCombatOutcome::JoinEnemy:
            handled = BeginCaptiveLocked(actorFormID, CaptiveMode::JoinedEnemy, reason);
            break;
        case InCombatOutcome::RecruitEnemy:
        case InCombatOutcome::Release:
        case InCombatOutcome::Follow:
            if (_snapshot.sub != SubFlow::InCombatPayFollowup &&
                _snapshot.sub != SubFlow::InCombatPleasure &&
                _snapshot.sub != SubFlow::InCombatAfterPleasure) {
                return RejectLocked("ResolveInCombatOutcomeTerminal", reason);
            }
            _combatActive = false;
            handled = EnterTerminalContextLocked(RootFlow::InCombat, SubFlow::None, actorFormID, reason);
            break;
        case InCombatOutcome::Cancel:
        case InCombatOutcome::Failed:
            handled = EnterTerminalContextLocked(RootFlow::InCombat, SubFlow::None, actorFormID, reason);
            break;
        case InCombatOutcome::None:
        default:
            return RejectLocked("ResolveInCombatOutcome", reason);
        }

        RefreshFlowGlobalsLocked();
        LogFlowSnapshot("ResolveInCombatOutcome", reason, _snapshot, actorFormID, ToString(outcome));
        return handled;
    }

    bool Controller::ResolveInCombatPleasure(std::uint32_t actorFormID, std::string_view reason)
    {
        return ResolveInCombatOutcome(InCombatOutcome::Pleasure, actorFormID, reason);
    }

    bool Controller::ResolveInCombatTerminal(std::uint32_t actorFormID, std::string_view reason)
    {
        return ResolveInCombatOutcome(InCombatOutcome::Cancel, actorFormID, reason);
    }

    bool Controller::ResolveBleedoutOutcome(BleedoutOutcome outcome, std::uint32_t actorFormID, std::string_view reason)
    {
        std::scoped_lock lk(_lock);

        const bool captiveEscapeBleedout =
            _snapshot.root == RootFlow::Captive &&
            _snapshot.gate == DecisionGate::PlayerBleedout &&
            (_snapshot.sub == SubFlow::EscapeAttempt ||
                _snapshot.sub == SubFlow::EscapeFailed ||
                _snapshot.sub == SubFlow::Recapture);
        const bool captiveEscapeBreakBleedout =
            _snapshot.root == RootFlow::Bleedout &&
            _snapshot.contextRoot == RootFlow::Captive &&
            _snapshot.gate == DecisionGate::PlayerBleedout &&
            _snapshot.sub == SubFlow::BleedoutEscapeBreak;

        if (captiveEscapeBleedout || captiveEscapeBreakBleedout) {
            bool handledCaptive = false;
            switch (outcome) {
            case BleedoutOutcome::Captive:
                _combatActive = false;
                _snapshot.root = RootFlow::Captive;
                _snapshot.contextRoot = RootFlow::Captive;
                if (_snapshot.captiveMode == CaptiveMode::None) {
                    _snapshot.captiveMode = CaptiveMode::Kidnapped;
                }
                SetPrimaryActorLocked(actorFormID);
                SetCaptiveIdleLocked();
                handledCaptive = true;
                break;
            case BleedoutOutcome::Cancel:
            case BleedoutOutcome::Failed:
                _combatActive = false;
                _snapshot.root = RootFlow::Captive;
                _snapshot.contextRoot = RootFlow::Captive;
                if (_snapshot.captiveMode == CaptiveMode::None) {
                    _snapshot.captiveMode = CaptiveMode::Kidnapped;
                }
                SetPrimaryActorLocked(actorFormID);
                SetCaptiveIdleLocked();
                handledCaptive = true;
                break;
            default:
                return RejectLocked("ResolveBleedoutOutcome", reason);
            }

            RefreshFlowGlobalsLocked();
            LogFlowSnapshot("ResolveBleedoutOutcome", reason, _snapshot, actorFormID, ToString(outcome));
            return handledCaptive;
        }

        if (_snapshot.root != RootFlow::Bleedout || _snapshot.gate != DecisionGate::PlayerBleedout) {
            return RejectLocked("ResolveBleedoutOutcome", reason);
        }

        bool handled = false;
        switch (outcome) {
        case BleedoutOutcome::Pay:
            handled = EnterTerminalContextLocked(RootFlow::Bleedout, SubFlow::None, actorFormID, reason);
            break;
        case BleedoutOutcome::Pleasure:
            // R461A: Bleedout -> Pleasure is not a terminal neutral outcome.
            // PleasureRuntime owns OStim/AfterPleasure/PleasureFailed, but the
            // flow root must keep a Bleedout-source owner until that runtime
            // commits a terminal outcome.  The previous R248A terminal handoff
            // set root=None/context=None immediately, which let ClearAll/PreCombat
            // observation leak in while OStim was still only StartPending.
            _snapshot.root = RootFlow::Bleedout;
            _snapshot.contextRoot = RootFlow::Bleedout;
            _snapshot.gate = DecisionGate::None;
            _snapshot.captiveMode = CaptiveMode::None;
            _snapshot.sub = SubFlow::BleedoutPleasure;
            _snapshot.terminalResolved = false;
            SetPrimaryActorLocked(actorFormID);
            BumpTokenLocked();
            _combatActive = false;
            handled = true;
            spdlog::info(
                "[TFD][Flow][R461A] bleedout pleasure handoff owner locked actor={:08X} reason={}",
                actorFormID,
                reason.empty() ? std::string{ "-" } : std::string{ reason });
            break;
        case BleedoutOutcome::Captive:
            handled = BeginCaptiveLocked(actorFormID, CaptiveMode::Kidnapped, reason);
            break;
        case BleedoutOutcome::Cancel:
        case BleedoutOutcome::Failed:
            handled = EnterTerminalContextLocked(RootFlow::Bleedout, SubFlow::None, actorFormID, reason);
            break;
        case BleedoutOutcome::None:
        default:
            return RejectLocked("ResolveBleedoutOutcome", reason);
        }
        RefreshFlowGlobalsLocked();
        LogFlowSnapshot("ResolveBleedoutOutcome", reason, _snapshot, actorFormID, ToString(outcome));
        return handled;
    }


    bool Controller::ResolveCaptiveOutcome(CaptiveOutcome outcome, std::uint32_t actorFormID, std::string_view reason)
    {
        std::scoped_lock lk(_lock);
        if (_snapshot.root != RootFlow::Captive) {
            return RejectLocked("ResolveCaptiveOutcome", reason);
        }

        switch (outcome) {
        case CaptiveOutcome::Pleasure:
            _snapshot.sub = SubFlow::CaptivePleasure;
            _snapshot.contextRoot = RootFlow::Captive;
            _snapshot.terminalResolved = false;
            _snapshot.gate = DecisionGate::None;
            SetPrimaryActorLocked(actorFormID);
            break;
        case CaptiveOutcome::WorkForEnemy:
            _snapshot.sub = SubFlow::WorkForEnemy;
            _snapshot.contextRoot = RootFlow::Captive;
            _snapshot.terminalResolved = false;
            _snapshot.gate = DecisionGate::None;
            _combatActive = false;
            SetPrimaryActorLocked(actorFormID);
            break;
        case CaptiveOutcome::EscapeStarted:
            _snapshot.sub = SubFlow::EscapeAttempt;
            _snapshot.contextRoot = RootFlow::Captive;
            _snapshot.terminalResolved = false;
            _snapshot.gate = DecisionGate::None;
            SetPrimaryActorLocked(actorFormID);
            break;
        case CaptiveOutcome::EscapeSucceeded:
        case CaptiveOutcome::Release:
        case CaptiveOutcome::Cancel:
        case CaptiveOutcome::Failed:
            return EnterTerminalContextLocked(RootFlow::Captive, SubFlow::None, actorFormID, reason);
        case CaptiveOutcome::EscapeFailed:
            _snapshot.sub = SubFlow::EscapeFailed;
            _snapshot.contextRoot = RootFlow::Captive;
            _snapshot.terminalResolved = false;
            _snapshot.gate = DecisionGate::None;
            SetPrimaryActorLocked(actorFormID);
            break;
        case CaptiveOutcome::Recaptured:
            _snapshot.sub = SubFlow::Recapture;
            _snapshot.contextRoot = RootFlow::Captive;
            _snapshot.terminalResolved = false;
            _snapshot.gate = DecisionGate::None;
            SetPrimaryActorLocked(actorFormID);
            break;
        case CaptiveOutcome::None:
        default:
            return RejectLocked("ResolveCaptiveOutcome", reason);
        }

        RefreshFlowGlobalsLocked();
        LogFlowSnapshot("ResolveCaptiveOutcome", reason, _snapshot, actorFormID, ToString(outcome));
        return true;
    }

    bool Controller::BeginAfterPleasure(std::uint32_t actorFormID, std::string_view reason)
    {
        std::scoped_lock lk(_lock);
        switch (_snapshot.sub) {
        case SubFlow::PreCombatPleasure:
            _snapshot.sub = SubFlow::PreCombatAfterPleasure;
            break;
        case SubFlow::InCombatPleasure:
            _snapshot.sub = SubFlow::InCombatAfterPleasure;
            break;
        case SubFlow::BleedoutPleasure:
            _snapshot.sub = SubFlow::BleedoutAfterPleasure;
            _combatActive = false;
            break;
        case SubFlow::CaptivePleasure:
            _snapshot.sub = SubFlow::CaptiveAfterPleasure;
            break;
        case SubFlow::WorkForEnemy:
            if (_snapshot.root == RootFlow::Captive &&
                (_snapshot.contextRoot == RootFlow::Captive || _snapshot.contextRoot == RootFlow::None) &&
                (reason == "pleasure_failed_enter" ||
                    reason == "captive_after_pleasure_enter" ||
                    reason == "after_pleasure_enter")) {
                _snapshot.contextRoot = RootFlow::Captive;
                _snapshot.sub = SubFlow::CaptiveAfterPleasure;
                _snapshot.gate = DecisionGate::None;
                _combatActive = false;
                break;
            }
            return RejectLocked("BeginAfterPleasure", reason);
        case SubFlow::BleedoutEscapeBreak:
            // R206A: post-bleedout scene from Captive escape-break must open
            // Captive/Work AfterPleasure, not generic Bleedout AfterPleasure.
            _snapshot.root = RootFlow::None;
            _snapshot.contextRoot = RootFlow::Captive;
            _snapshot.gate = DecisionGate::None;
            _snapshot.sub = SubFlow::CaptiveAfterPleasure;
            _snapshot.terminalResolved = true;
            _combatActive = false;
            break;
        case SubFlow::PreCombatAfterPleasure:
        case SubFlow::InCombatAfterPleasure:
        case SubFlow::BleedoutAfterPleasure:
        case SubFlow::CaptiveAfterPleasure:
        default:
            return RejectLocked("BeginAfterPleasure", reason);
        }
        SetPrimaryActorLocked(actorFormID);
        RefreshFlowGlobalsLocked();
        LogFlowSnapshot("BeginAfterPleasure", reason, _snapshot, actorFormID);
        return true;
    }

    bool Controller::BeginPleasureFailed(std::uint32_t actorFormID, int sourceFlow, std::string_view reason)
    {
        std::scoped_lock lk(_lock);

        switch (_snapshot.sub) {
        case SubFlow::PreCombatPleasure:
        case SubFlow::PreCombatAfterPleasure:
            _snapshot.contextRoot = RootFlow::PreCombat;
            break;
        case SubFlow::InCombatPleasure:
        case SubFlow::InCombatAfterPleasure:
            _snapshot.contextRoot = RootFlow::InCombat;
            break;
        case SubFlow::BleedoutPleasure:
        case SubFlow::BleedoutAfterPleasure:
            _snapshot.contextRoot = RootFlow::Bleedout;
            _combatActive = false;
            break;
        case SubFlow::CaptivePleasure:
        case SubFlow::CaptiveAfterPleasure:
        case SubFlow::WorkForEnemy:
            _snapshot.contextRoot = RootFlow::Captive;
            break;
        case SubFlow::PleasureFailedDialogue:
            break;
        default:
            if (sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::Captive)) {
                _snapshot.contextRoot = RootFlow::Captive;
            }
            else if (sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::Bleedout)) {
                _snapshot.contextRoot = RootFlow::Bleedout;
            }
            else if (sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::InCombat)) {
                _snapshot.contextRoot = RootFlow::InCombat;
            }
            else if (sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::PreCombat)) {
                _snapshot.contextRoot = RootFlow::PreCombat;
            }
            break;
        }

        _snapshot.sub = SubFlow::PleasureFailedDialogue;
        _snapshot.gate = DecisionGate::None;
        _snapshot.terminalResolved = false;
        SetPrimaryActorLocked(actorFormID);
        RefreshFlowGlobalsLocked();
        LogFlowSnapshot("BeginPleasureFailed", reason, _snapshot, actorFormID, "PleasureFailedIndependent");
        return true;
    }

    bool Controller::ResumePleasureFromPleasureFailed(std::uint32_t actorFormID, int sourceFlow, std::string_view reason)
    {
        std::scoped_lock lk(_lock);

        if (_snapshot.sub != SubFlow::PleasureFailedDialogue) {
            return RejectLocked("ResumePleasureFromPleasureFailed", reason);
        }

        if (sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::PreCombat) ||
            _snapshot.contextRoot == RootFlow::PreCombat) {
            _snapshot.sub = SubFlow::PreCombatPleasure;
            _snapshot.contextRoot = RootFlow::PreCombat;
        }
        else if (sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::InCombat) ||
            _snapshot.contextRoot == RootFlow::InCombat) {
            _snapshot.sub = SubFlow::InCombatPleasure;
            _snapshot.contextRoot = RootFlow::InCombat;
        }
        else if (sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::Bleedout) ||
            _snapshot.contextRoot == RootFlow::Bleedout) {
            _snapshot.sub = SubFlow::BleedoutPleasure;
            _snapshot.contextRoot = RootFlow::Bleedout;
            _combatActive = false;
        }
        else if (sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::Captive) ||
            _snapshot.contextRoot == RootFlow::Captive) {
            _snapshot.sub = SubFlow::CaptivePleasure;
            _snapshot.contextRoot = RootFlow::Captive;
        }
        else {
            _snapshot.sub = SubFlow::InCombatPleasure;
            _snapshot.contextRoot = RootFlow::InCombat;
        }

        _snapshot.gate = DecisionGate::None;
        _snapshot.terminalResolved = false;
        SetPrimaryActorLocked(actorFormID);
        RefreshFlowGlobalsLocked();
        LogFlowSnapshot("ResumePleasureFromPleasureFailed", reason, _snapshot, actorFormID, "PleasureFailedPleadingRedo");
        return true;
    }

    bool Controller::CompleteAfterPleasure(std::string_view reason)
    {
        std::scoped_lock lk(_lock);
        switch (_snapshot.sub) {
        case SubFlow::PreCombatPleasure:
        case SubFlow::PreCombatAfterPleasure:
        case SubFlow::InCombatPleasure:
        case SubFlow::InCombatAfterPleasure:
            return CompleteTerminalContextLocked(reason);
        case SubFlow::BleedoutPleasure:
            if (reason == "pleasure_aborted_no_afterpleasure" ||
                reason == "bleedout_start_failed" ||
                reason == "r461_bleedout_abort_to_pleasure_failed") {
                // R461A: a Bleedout-source scene abort/start-fail must become
                // PleasureFailed dialogue, not Neutral/PreCombat.  Do not clear
                // the Bleedout-source owner here; TFDPleasureFailedEnter will
                // open the controlled failed forcegreet.
                _snapshot.root = RootFlow::Bleedout;
                _snapshot.contextRoot = RootFlow::Bleedout;
                _snapshot.gate = DecisionGate::None;
                _snapshot.sub = SubFlow::PleasureFailedDialogue;
                _snapshot.terminalResolved = false;
                _combatActive = false;
                RefreshFlowGlobalsLocked();
                LogFlowSnapshot("CompleteAfterPleasure", reason, _snapshot, _snapshot.primaryActorFormID, "R461A_BleedoutAbortToPleasureFailed");
                return true;
            }
            return CompleteTerminalContextLocked(reason);
        case SubFlow::BleedoutAfterPleasure:
            return CompleteTerminalContextLocked(reason);
        case SubFlow::CaptivePleasure:
        case SubFlow::CaptiveAfterPleasure:
            SetCaptiveIdleLocked();
            RefreshFlowGlobalsLocked();
            LogFlowSnapshot("CompleteAfterPleasure", reason, _snapshot);
            return true;
        default:
            return RejectLocked("CompleteAfterPleasure", reason);
        }
    }

    bool Controller::CompleteTerminalContext(std::string_view reason)
    {
        std::scoped_lock lk(_lock);
        return CompleteTerminalContextLocked(reason);
    }

    bool Controller::AbortPreCombat(std::uint32_t actorFormID, std::string_view reason)
    {
        std::scoped_lock lk(_lock);

        if (_snapshot.root != RootFlow::PreCombat) {
            spdlog::info(
                "[TFD][Flow] abort precombat ignored reason={} actor={:08X} root={} ctx={} gate={} sub={} token={} primary={:08X} terminal={}",
                reason.empty() ? std::string{ "-" } : std::string{ reason },
                actorFormID,
                ToString(_snapshot.root),
                ToString(_snapshot.contextRoot),
                ToString(_snapshot.gate),
                ToString(_snapshot.sub),
                _snapshot.token,
                _snapshot.primaryActorFormID,
                _snapshot.terminalResolved ? 1 : 0);
            return false;
        }

        if (_snapshot.terminalResolved) {
            spdlog::info(
                "[TFD][Flow] abort precombat ignored reason={} actor={:08X} root={} ctx={} gate={} sub={} token={} primary={:08X} terminal=1",
                reason.empty() ? std::string{ "-" } : std::string{ reason },
                actorFormID,
                ToString(_snapshot.root),
                ToString(_snapshot.contextRoot),
                ToString(_snapshot.gate),
                ToString(_snapshot.sub),
                _snapshot.token,
                _snapshot.primaryActorFormID);
            return false;
        }

        const auto primary = _snapshot.primaryActorFormID;
        if (actorFormID == 0) {
            spdlog::warn(
                "[TFD][Flow] abort precombat reject reason={} actor=00000000 primary={:08X} root={} gate={} sub={} token={}",
                reason.empty() ? std::string{ "-" } : std::string{ reason },
                primary,
                ToString(_snapshot.root),
                ToString(_snapshot.gate),
                ToString(_snapshot.sub),
                _snapshot.token);
            return false;
        }

        if (primary != 0 && primary != actorFormID) {
            spdlog::warn(
                "[TFD][Flow] abort precombat owner reject reason={} actor={:08X} primary={:08X} root={} gate={} sub={} token={}",
                reason.empty() ? std::string{ "-" } : std::string{ reason },
                actorFormID,
                primary,
                ToString(_snapshot.root),
                ToString(_snapshot.gate),
                ToString(_snapshot.sub),
                _snapshot.token);
            return false;
        }

        const auto before = _snapshot;
        ClearAllLocked();
        _combatActive = false;
        RefreshFlowGlobalsLocked();

        spdlog::info(
            "[TFD][Flow] AbortPreCombat reason={} actor={:08X} oldRoot={} oldCtx={} oldGate={} oldSub={} oldToken={} oldPrimary={:08X}",
            reason.empty() ? std::string{ "-" } : std::string{ reason },
            actorFormID,
            ToString(before.root),
            ToString(before.contextRoot),
            ToString(before.gate),
            ToString(before.sub),
            before.token,
            before.primaryActorFormID);
        LogFlowSnapshot("AbortPreCombat", reason, _snapshot, actorFormID);
        return true;
    }

    void Controller::NotifyCombatStarted(std::uint32_t actorFormID, std::string_view reason)
    {
        std::scoped_lock lk(_lock);
        auto* combatActor = actorFormID != 0 ? RE::TESForm::LookupByID<RE::Actor>(actorFormID) : nullptr;
        const bool temporaryFollowLocked = combatActor && TFD::Actor::Ops::HasTemporaryFollowLock(combatActor);
        const bool playerSideActor = combatActor &&
            (combatActor->IsPlayerTeammate() ||
                TFD::TeammateManager::IsActiveFollowerActor(combatActor) ||
                TFD::TeammateManager::IsPlayerSideTeammateActor(combatActor));
        if (temporaryFollowLocked || playerSideActor) {
            if (_snapshot.root == RootFlow::InCombat &&
                _snapshot.primaryActorFormID == actorFormID &&
                _snapshot.gate == DecisionGate::None &&
                _snapshot.sub == SubFlow::None) {
                ClearAllLocked();
            }
            _combatActive = false;
            RefreshFlowGlobalsLocked();
            spdlog::info(
                "[TFD][Flow][R144] combat start ignored for temporary/player-side actor actor={:08X} reason={} temporaryFollow={} playerSide={} root={} sub={} primary={:08X}",
                actorFormID,
                reason.empty() ? std::string{ "-" } : std::string{ reason },
                temporaryFollowLocked ? 1 : 0,
                playerSideActor ? 1 : 0,
                ToString(_snapshot.root),
                ToString(_snapshot.sub),
                _snapshot.primaryActorFormID);
            return;
        }

        if (_snapshot.sub == SubFlow::PleasureFailedDialogue && reason == "pleasure_failed_fight") {
            const auto oldContext = _snapshot.contextRoot;
            const auto oldSub = _snapshot.sub;
            const auto oldToken = _snapshot.token;

            _combatActive = true;
            (void)BeginRootLocked(RootFlow::InCombat, actorFormID, reason);
            RefreshFlowGlobalsLocked();

            spdlog::info(
                "[TFD][Flow][R223A] combat start accepted from independent pleasure failed actor={:08X} oldCtx={} oldSub={} oldToken={} newRoot={} newCtx={} newSub={}",
                actorFormID,
                ToString(oldContext),
                ToString(oldSub),
                oldToken,
                ToString(_snapshot.root),
                ToString(_snapshot.contextRoot),
                ToString(_snapshot.sub));
            return;
        }

        const bool terminalPleasureContext = _snapshot.terminalResolved &&
            (_snapshot.sub == SubFlow::PreCombatPleasure ||
                _snapshot.sub == SubFlow::PreCombatAfterPleasure ||
                _snapshot.sub == SubFlow::InCombatPleasure ||
                _snapshot.sub == SubFlow::InCombatAfterPleasure ||
                _snapshot.sub == SubFlow::BleedoutPleasure ||
                _snapshot.sub == SubFlow::BleedoutAfterPleasure ||
                _snapshot.sub == SubFlow::CaptivePleasure ||
                _snapshot.sub == SubFlow::CaptiveAfterPleasure);
        if (terminalPleasureContext) {
            const bool explicitPleasureFailedFight = reason == "pleasure_failed_fight";
            if (explicitPleasureFailedFight) {
                const auto oldContext = _snapshot.contextRoot;
                const auto oldSub = _snapshot.sub;
                const auto oldToken = _snapshot.token;

                _combatActive = true;
                (void)BeginRootLocked(RootFlow::InCombat, actorFormID, reason);
                RefreshFlowGlobalsLocked();

                spdlog::info(
                    "[TFD][Flow][R138] combat start accepted from terminal pleasure failed fight actor={:08X} oldCtx={} oldSub={} oldToken={} newRoot={} newCtx={} newSub={} terminal={}",
                    actorFormID,
                    ToString(oldContext),
                    ToString(oldSub),
                    oldToken,
                    ToString(_snapshot.root),
                    ToString(_snapshot.contextRoot),
                    ToString(_snapshot.sub),
                    _snapshot.terminalResolved ? 1 : 0);
                return;
            }

            // R137: terminal pleasure/pleasure-failed dialogue owns the actor.
            // Combat notifications can still arrive from nearby enemies or stale
            // alarm data; do not let them wipe the terminal context into InCombat.
            _combatActive = false;
            RefreshFlowGlobalsLocked();
            spdlog::info(
                "[TFD][Flow][R137] combat start ignored during terminal pleasure context actor={:08X} reason={} ctx={} sub={} pleasureFailed={} terminal=1",
                actorFormID,
                reason.empty() ? std::string{ "-" } : std::string{ reason },
                ToString(_snapshot.contextRoot),
                ToString(_snapshot.sub),
                TFD::PleasureRuntime::IsPleasureFailedDialogueActive() ? 1 : 0);
            return;
        }
        if (_snapshot.root == RootFlow::Captive && (_snapshot.sub == SubFlow::EscapeAttempt || _snapshot.sub == SubFlow::EscapeFailed || _snapshot.sub == SubFlow::Recapture)) {
            _combatActive = true;
            RefreshFlowGlobalsLocked();
            return;
        }
        if (_snapshot.root == RootFlow::Captive) {
            return;
        }
        _combatActive = true;
        if (_snapshot.root == RootFlow::Bleedout) {
            SetPrimaryActorLocked(actorFormID);
            RefreshFlowGlobalsLocked();
            return;
        }
        (void)BeginRootLocked(RootFlow::InCombat, actorFormID, reason);
        RefreshFlowGlobalsLocked();
    }

    void Controller::NotifyCombatEnded(std::string_view reason)
    {
        std::scoped_lock lk(_lock);
        _combatActive = false;
        if ((_snapshot.root == RootFlow::InCombat || _snapshot.root == RootFlow::Bleedout) && _snapshot.gate == DecisionGate::None && _snapshot.sub == SubFlow::None) {
            ClearAllLocked();
        }
        RefreshFlowGlobalsLocked();
        LogFlowSnapshot("NotifyCombatEnded", reason, _snapshot);
    }

    void Controller::NotifyPlayerBleedout(std::uint32_t actorFormID, std::string_view reason)
    {
        (void)RequestPlayerBleedoutDecision(actorFormID, reason);
    }

    void Controller::NotifyEnemyBleedout(std::uint32_t actorFormID, std::string_view reason)
    {
        (void)RequestEnemyBleedoutDecision(actorFormID, reason);
    }

    bool Controller::CanStartPreCombat() const
    {
        std::scoped_lock lk(_lock);
        const auto snapshot = ProjectExternalSnapshot(_snapshot);
        return snapshot.root == RootFlow::None && snapshot.sub == SubFlow::None && !snapshot.terminalResolved;
    }

    bool Controller::CanStartInCombatTruce() const
    {
        std::scoped_lock lk(_lock);
        const auto snapshot = ProjectExternalSnapshot(_snapshot);
        return snapshot.root == RootFlow::InCombat && !snapshot.terminalResolved;
    }

    bool Controller::CanEnterCaptive() const
    {
        std::scoped_lock lk(_lock);
        const auto root = ProjectExternalRootFlow(_snapshot);
        return root != RootFlow::Rescue &&
            root != RootFlow::Recovery &&
            root != RootFlow::LeftForDead;
    }


    bool Controller::IsCaptiveContext() const
    {
        std::scoped_lock lk(_lock);
        return _snapshot.root == RootFlow::Captive || _snapshot.contextRoot == RootFlow::Captive;
    }

    bool Controller::IsJoinedEnemyMode() const
    {
        std::scoped_lock lk(_lock);
        return _snapshot.root == RootFlow::Captive && _snapshot.captiveMode == CaptiveMode::JoinedEnemy;
    }

    bool Controller::IsBleedDecisionActive() const
    {
        std::scoped_lock lk(_lock);
        const bool captiveEscapeBleedoutDecision =
            _snapshot.root == RootFlow::Captive &&
            _snapshot.gate == DecisionGate::PlayerBleedout &&
            (_snapshot.sub == SubFlow::EscapeFailed || _snapshot.sub == SubFlow::Recapture) &&
            !_snapshot.terminalResolved;
        const bool captiveEscapeBreakBleedoutDecision =
            _snapshot.root == RootFlow::Bleedout &&
            _snapshot.contextRoot == RootFlow::Captive &&
            _snapshot.gate == DecisionGate::PlayerBleedout &&
            _snapshot.sub == SubFlow::BleedoutEscapeBreak &&
            !_snapshot.terminalResolved;
        return (_snapshot.root == RootFlow::Bleedout && _snapshot.gate == DecisionGate::PlayerBleedout) ||
            captiveEscapeBleedoutDecision ||
            captiveEscapeBreakBleedoutDecision;
    }

    bool Controller::IsCombatOrBleedRootActive() const
    {
        std::scoped_lock lk(_lock);
        return _snapshot.root == RootFlow::InCombat || _snapshot.root == RootFlow::Bleedout;
    }

    bool Controller::IsRescueRootActive() const
    {
        std::scoped_lock lk(_lock);
        return ProjectExternalRootFlow(_snapshot) == RootFlow::Rescue;
    }

    bool Controller::IsRecoveryRootActive() const
    {
        std::scoped_lock lk(_lock);
        return ProjectExternalRootFlow(_snapshot) == RootFlow::Recovery;
    }

    bool Controller::IsLeftForDeadRootActive() const
    {
        std::scoped_lock lk(_lock);
        return ProjectExternalRootFlow(_snapshot) == RootFlow::LeftForDead;
    }

    bool Controller::IsCaptiveEscapeContextActive() const
    {
        std::scoped_lock lk(_lock);
        return (_snapshot.root == RootFlow::Captive &&
            (_snapshot.sub == SubFlow::EscapeAttempt ||
                _snapshot.sub == SubFlow::EscapeFailed ||
                _snapshot.sub == SubFlow::Recapture)) ||
            (_snapshot.contextRoot == RootFlow::Captive &&
                (_snapshot.sub == SubFlow::InCombatEscapeBreak ||
                    _snapshot.sub == SubFlow::BleedoutEscapeBreak));
    }

    bool Controller::IsCaptiveEscapeBreakContextActive() const
    {
        std::scoped_lock lk(_lock);
        return _snapshot.contextRoot == RootFlow::Captive &&
            (_snapshot.sub == SubFlow::InCombatEscapeBreak ||
                _snapshot.sub == SubFlow::BleedoutEscapeBreak ||
                _snapshot.sub == SubFlow::CaptiveAfterPleasure);
    }

    bool Controller::IsCaptiveCombatEscapeBreakContextActive() const
    {
        std::scoped_lock lk(_lock);
        return _snapshot.contextRoot == RootFlow::Captive &&
            (_snapshot.sub == SubFlow::InCombatEscapeBreak ||
                _snapshot.sub == SubFlow::BleedoutEscapeBreak);
    }

    bool Controller::IsPleasureFailedSubFlowActive() const
    {
        std::scoped_lock lk(_lock);
        return _snapshot.sub == SubFlow::PleasureFailedDialogue;
    }

    bool Controller::IsPleasureSubFlowActive() const
    {
        std::scoped_lock lk(_lock);
        switch (_snapshot.sub) {
        case SubFlow::PreCombatPleasure:
        case SubFlow::InCombatPleasure:
        case SubFlow::BleedoutPleasure:
        case SubFlow::CaptivePleasure:
            return true;
        default:
            break;
        }
        return false;
    }

    bool Controller::IsAfterPleasureSubFlowActive() const
    {
        std::scoped_lock lk(_lock);
        switch (_snapshot.sub) {
        case SubFlow::PreCombatAfterPleasure:
        case SubFlow::InCombatAfterPleasure:
        case SubFlow::BleedoutAfterPleasure:
        case SubFlow::CaptiveAfterPleasure:
            return true;
        default:
            break;
        }
        return false;
    }

    bool Controller::IsInCombatAfterPleasureContextActive() const
    {
        std::scoped_lock lk(_lock);
        return _snapshot.sub == SubFlow::InCombatPleasure ||
            _snapshot.sub == SubFlow::InCombatAfterPleasure;
    }

    bool Controller::IsTerminalDialogueGateActive() const
    {
        std::scoped_lock lk(_lock);
        const bool rootOk = _snapshot.root == RootFlow::PreCombat ||
            _snapshot.root == RootFlow::InCombat ||
            _snapshot.root == RootFlow::Bleedout;
        const bool gateOk = _snapshot.gate == DecisionGate::Truce ||
            _snapshot.gate == DecisionGate::PlayerBleedout;
        return rootOk && gateOk && !_snapshot.terminalResolved;
    }

    bool Controller::BeginCaptiveFromModEvent(std::uint32_t actorFormID, std::string_view reason)
    {
        return RequestCaptive(actorFormID, CaptiveMode::Kidnapped, reason.empty() ? std::string_view{ "mod_event_captive" } : reason);
    }

    bool Controller::BeginCaptivePleasureFromModEvent(std::uint32_t actorFormID, std::string_view reason)
    {
        const auto requestReason = reason.empty() ? std::string_view{ "mod_event_pleasure" } : reason;

        {
            std::scoped_lock lk(_lock);

            const bool flowEscapeBreakBleedout =
                _snapshot.root == RootFlow::Bleedout &&
                _snapshot.contextRoot == RootFlow::Captive &&
                _snapshot.gate == DecisionGate::PlayerBleedout &&
                _snapshot.sub == SubFlow::BleedoutEscapeBreak &&
                !_snapshot.terminalResolved;

            const bool captiveEscapeBleedout = TFD::Captive::IsEscapeBleedoutActive();
            const bool releasedWorkContext = TFD::Captive::IsReleasedWorkActive();

            if (flowEscapeBreakBleedout || captiveEscapeBleedout) {
                // R234A: Bleedout Pleasure selected from a Captive Work escape-break
                // is not a new Kidnapped/Captive entry.  The player was already in
                // ReleasedWork before the failed escape, so preserve Work ownership
                // and move the flow directly into CaptivePleasure.  The previous path
                // called RequestCaptive(Kidnapped), which forced TFDCaptiveState 3 -> 1,
                // cleared work aliases, and let marker_radius restore aggression before
                // OStim could own the scene.
                _snapshot.root = RootFlow::Captive;
                _snapshot.contextRoot = RootFlow::Captive;
                _snapshot.gate = DecisionGate::None;
                _snapshot.sub = SubFlow::CaptivePleasure;
                _snapshot.terminalResolved = false;
                _combatActive = false;
                if (_snapshot.captiveMode == CaptiveMode::None) {
                    _snapshot.captiveMode = CaptiveMode::Surrendered;
                }
                SetPrimaryActorLocked(actorFormID);
                RefreshFlowGlobalsLocked();
                LogFlowSnapshot("BeginCaptivePleasureFromModEvent", requestReason, _snapshot, actorFormID,
                    releasedWorkContext ? "R234A_WorkEscapeBreakPleasure" : "R234A_CaptiveEscapeBreakPleasure");
                spdlog::info("[TFD][Flow][R234A] captive escape-break bleedout pleasure accepted actor={:08X} preserveWork={} captiveEscapeBleedout={} reason={}",
                    actorFormID,
                    releasedWorkContext ? 1 : 0,
                    captiveEscapeBleedout ? 1 : 0,
                    std::string(requestReason));
                return true;
            }
        }

        if (!RequestCaptive(actorFormID, CaptiveMode::Kidnapped, "mod_event_captive_pleasure_begin")) {
            spdlog::warn("[TFD][Flow] ignore mod_event_pleasure reason=begin_captive_reject actor={:08X}", actorFormID);
            return false;
        }
        if (!RequestResolveCaptiveOutcome(CaptiveOutcome::Pleasure, actorFormID, requestReason)) {
            spdlog::warn("[TFD][Flow] ignore mod_event_pleasure reason=captive_flow_reject actor={:08X}", actorFormID);
            return false;
        }
        return true;
    }

    bool Controller::BeginRootLocked(RootFlow next, std::uint32_t actorFormID, std::string_view reason)
    {
        (void)reason;
        if (_snapshot.root == next) {
            if (next == RootFlow::PreCombat && !GuardPreCombatOwnerLocked(actorFormID, "BeginRootLocked", reason)) {
                return false;
            }
            SetPrimaryActorLocked(actorFormID);
            return true;
        }
        ClearDecisionLocked();
        ClearSubLocked();
        ClearTerminalLocked();
        _snapshot.root = next;
        _snapshot.contextRoot = next;
        _snapshot.captiveMode = CaptiveMode::None;
        SetPrimaryActorLocked(actorFormID);
        BumpTokenLocked();
        RefreshFlowGlobalsLocked();
        LogFlowSnapshot("BeginRootLocked", reason, _snapshot, actorFormID, ToString(next));
        return true;
    }

    bool Controller::TransitionRootLocked(RootFlow next, std::uint32_t actorFormID, std::string_view reason)
    {
        (void)reason;
        if (_snapshot.root == next) {
            SetPrimaryActorLocked(actorFormID);
            return true;
        }
        ClearDecisionLocked();
        ClearSubLocked();
        ClearTerminalLocked();
        _snapshot.root = next;
        _snapshot.contextRoot = next;
        _snapshot.captiveMode = CaptiveMode::None;
        SetPrimaryActorLocked(actorFormID);
        BumpTokenLocked();
        RefreshFlowGlobalsLocked();
        LogFlowSnapshot("TransitionRootLocked", reason, _snapshot, actorFormID, ToString(next));
        return true;
    }

    bool Controller::BeginCaptiveLocked(std::uint32_t actorFormID, CaptiveMode mode, std::string_view reason)
    {
        (void)reason;
        if (mode == CaptiveMode::None) {
            return RejectLocked("BeginCaptive", reason);
        }
        if (!TransitionRootLocked(RootFlow::Captive, actorFormID, reason)) {
            return false;
        }
        _combatActive = false;
        _snapshot.captiveMode = mode;
        SetCaptiveIdleLocked();
        RefreshFlowGlobalsLocked();
        LogFlowSnapshot("BeginCaptiveLocked", reason, _snapshot, actorFormID, ToString(mode));
        return true;
    }

    bool Controller::EnterTerminalContextLocked(RootFlow contextRoot, SubFlow sub, std::uint32_t actorFormID, std::string_view reason)
    {
        (void)reason;
        _snapshot.root = RootFlow::None;
        _snapshot.contextRoot = contextRoot;
        _snapshot.gate = DecisionGate::None;
        _snapshot.captiveMode = CaptiveMode::None;
        _snapshot.sub = sub;
        _snapshot.terminalResolved = true;
        SetPrimaryActorLocked(actorFormID);
        BumpTokenLocked();
        RefreshFlowGlobalsLocked();
        LogFlowSnapshot("EnterTerminalContextLocked", reason, _snapshot, actorFormID, ToString(contextRoot));
        return true;
    }

    bool Controller::CompleteTerminalContextLocked(std::string_view reason)
    {
        (void)reason;
        if (!_snapshot.terminalResolved || _snapshot.root != RootFlow::None) {
            return RejectLocked("CompleteTerminalContext", reason);
        }
        ClearAllLocked();
        _combatActive = false;
        TFD::InteractionRouter::ClearInteractionStateValue();
        RefreshFlowGlobalsLocked();
        LogFlowSnapshot("CompleteTerminalContextLocked", reason, _snapshot);
        return true;
    }

    bool Controller::GuardPreCombatOwnerLocked(std::uint32_t actorFormID, std::string_view op, std::string_view reason) const
    {
        if (actorFormID == 0) {
            spdlog::warn(
                "[TFD][Flow] precombat owner reject op={} reason={} actor=00000000 primary={:08X} root={} gate={} sub={} token={}",
                op.empty() ? "unknown" : std::string{ op },
                reason.empty() ? std::string{ "-" } : std::string{ reason },
                _snapshot.primaryActorFormID,
                ToString(_snapshot.root),
                ToString(_snapshot.gate),
                ToString(_snapshot.sub),
                _snapshot.token);
            return RejectLocked(op, reason);
        }

        const auto primary = _snapshot.primaryActorFormID;
        if (primary != 0 && primary != actorFormID) {
            spdlog::warn(
                "[TFD][Flow] precombat owner reject op={} reason={} actor={:08X} primary={:08X} root={} gate={} sub={} token={}",
                op.empty() ? "unknown" : std::string{ op },
                reason.empty() ? std::string{ "-" } : std::string{ reason },
                actorFormID,
                primary,
                ToString(_snapshot.root),
                ToString(_snapshot.gate),
                ToString(_snapshot.sub),
                _snapshot.token);
            return RejectLocked(op, reason);
        }

        return true;
    }

    bool Controller::GuardInCombatOwnerLocked(std::uint32_t actorFormID, std::string_view op, std::string_view reason) const
    {
        if (actorFormID == 0) {
            spdlog::warn(
                "[TFD][Flow][R95A] incombat owner reject op={} reason={} actor=00000000 primary={:08X} root={} gate={} sub={} token={}",
                op.empty() ? "unknown" : std::string{ op },
                reason.empty() ? std::string{ "-" } : std::string{ reason },
                _snapshot.primaryActorFormID,
                ToString(_snapshot.root),
                ToString(_snapshot.gate),
                ToString(_snapshot.sub),
                _snapshot.token);
            return RejectLocked(op, reason);
        }

        const auto primary = _snapshot.primaryActorFormID;
        if (primary != 0 && primary != actorFormID) {
            spdlog::warn(
                "[TFD][Flow][R95A] incombat owner reject op={} reason={} actor={:08X} primary={:08X} root={} gate={} sub={} token={}",
                op.empty() ? "unknown" : std::string{ op },
                reason.empty() ? std::string{ "-" } : std::string{ reason },
                actorFormID,
                primary,
                ToString(_snapshot.root),
                ToString(_snapshot.gate),
                ToString(_snapshot.sub),
                _snapshot.token);
            return RejectLocked(op, reason);
        }

        return true;
    }

    bool Controller::RejectLocked(std::string_view op, std::string_view reason) const
    {
        LogFlowReject(op.data(), reason, _snapshot);
        return false;
    }

    void Controller::SetPrimaryActorLocked(std::uint32_t actorFormID)
    {
        _snapshot.primaryActorFormID = actorFormID;
    }

    void Controller::ClearDecisionLocked()
    {
        _snapshot.gate = DecisionGate::None;
    }

    void Controller::ClearSubLocked()
    {
        _snapshot.sub = SubFlow::None;
    }

    void Controller::BumpTokenLocked()
    {
        ++_snapshot.token;
        if (_snapshot.token == 0) {
            ++_snapshot.token;
        }
    }

    void Controller::ClearTerminalLocked()
    {
        _snapshot.terminalResolved = false;
    }

    void Controller::ClearAllLocked()
    {
        _snapshot = Snapshot{};
    }

    void Controller::SetCaptiveIdleLocked()
    {
        _snapshot.contextRoot = RootFlow::Captive;
        _snapshot.gate = DecisionGate::None;
        _snapshot.terminalResolved = false;
        _snapshot.sub = (_snapshot.captiveMode == CaptiveMode::JoinedEnemy) ? SubFlow::JoinedEnemyIdle : SubFlow::CaptiveIdle;
    }

    const char* Controller::ToString(RootFlow value)
    {
        switch (value) {
        case RootFlow::None: return "None";
        case RootFlow::PreCombat: return "PreCombat";
        case RootFlow::InCombat: return "InCombat";
        case RootFlow::Bleedout: return "Bleedout";
        case RootFlow::Captive: return "Captive";
        case RootFlow::Rescue: return "Rescue";
        case RootFlow::Recovery: return "Recovery";
        case RootFlow::LeftForDead: return "LeftForDead";
        default: return "UnknownRootFlow";
        }
    }

    const char* Controller::ToString(ObservedMainState value)
    {
        return ObservedStateName(value);
    }

    const char* Controller::ToString(DecisionGate value)
    {
        switch (value) {
        case DecisionGate::None: return "None";
        case DecisionGate::PlayerBleedout: return "PlayerBleedout";
        case DecisionGate::EnemyBleedout: return "EnemyBleedout";
        case DecisionGate::Truce: return "Truce";
        default: return "UnknownDecisionGate";
        }
    }

    const char* Controller::ToString(CaptiveMode value)
    {
        switch (value) {
        case CaptiveMode::None: return "None";
        case CaptiveMode::Kidnapped: return "Kidnapped";
        case CaptiveMode::Surrendered: return "Surrendered";
        case CaptiveMode::JoinedEnemy: return "JoinedEnemy";
        default: return "UnknownCaptiveMode";
        }
    }

    const char* Controller::ToString(SubFlow value)
    {
        switch (value) {
        case SubFlow::None: return "None";
        case SubFlow::PreCombatPayFollowup: return "PreCombatPayFollowup";
        case SubFlow::PreCombatPleasure: return "PreCombatPleasure";
        case SubFlow::PreCombatAfterPleasure: return "PreCombatAfterPleasure";
        case SubFlow::InCombatPayFollowup: return "InCombatPayFollowup";
        case SubFlow::InCombatPleasure: return "InCombatPleasure";
        case SubFlow::InCombatAfterPleasure: return "InCombatAfterPleasure";
        case SubFlow::BleedoutPleasure: return "BleedoutPleasure";
        case SubFlow::BleedoutAfterPleasure: return "BleedoutAfterPleasure";
        case SubFlow::PleasureFailedDialogue: return "PleasureFailedDialogue";
        case SubFlow::CaptiveIdle: return "CaptiveIdle";
        case SubFlow::CaptivePleasure: return "CaptivePleasure";
        case SubFlow::CaptiveAfterPleasure: return "CaptiveAfterPleasure";
        case SubFlow::WorkForEnemy: return "WorkForEnemy";
        case SubFlow::EscapeAttempt: return "EscapeAttempt";
        case SubFlow::EscapeFailed: return "EscapeFailed";
        case SubFlow::Recapture: return "Recapture";
        case SubFlow::JoinedEnemyIdle: return "JoinedEnemyIdle";
        case SubFlow::InCombatEscapeBreak: return "InCombatEscapeBreak";
        case SubFlow::BleedoutEscapeBreak: return "BleedoutEscapeBreak";
        default: return "UnknownSubFlow";
        }
    }

    const char* Controller::ToString(PreCombatOutcome value)
    {
        switch (value) {
        case PreCombatOutcome::None: return "None";
        case PreCombatOutcome::Pay: return "Pay";
        case PreCombatOutcome::Pleasure: return "Pleasure";
        case PreCombatOutcome::Fight: return "Fight";
        case PreCombatOutcome::Captive: return "Captive";
        case PreCombatOutcome::JoinEnemy: return "JoinEnemy";
        case PreCombatOutcome::RecruitEnemy: return "RecruitEnemy";
        case PreCombatOutcome::Release: return "Release";
        case PreCombatOutcome::Follow: return "Follow";
        case PreCombatOutcome::Cancel: return "Cancel";
        case PreCombatOutcome::Failed: return "Failed";
        case PreCombatOutcome::DoNothing: return "DoNothing";
        default: return "UnknownPreCombatOutcome";
        }
    }

    const char* Controller::ToString(InCombatOutcome value)
    {
        switch (value) {
        case InCombatOutcome::None: return "None";
        case InCombatOutcome::Pay: return "Pay";
        case InCombatOutcome::Pleasure: return "Pleasure";
        case InCombatOutcome::Fight: return "Fight";
        case InCombatOutcome::Captive: return "Captive";
        case InCombatOutcome::JoinEnemy: return "JoinEnemy";
        case InCombatOutcome::RecruitEnemy: return "RecruitEnemy";
        case InCombatOutcome::Release: return "Release";
        case InCombatOutcome::Follow: return "Follow";
        case InCombatOutcome::DoNothing: return "DoNothing";
        case InCombatOutcome::Cancel: return "Cancel";
        case InCombatOutcome::Failed: return "Failed";
        default: return "UnknownInCombatOutcome";
        }
    }

    const char* Controller::ToString(BleedoutOutcome value)
    {
        switch (value) {
        case BleedoutOutcome::None: return "None";
        case BleedoutOutcome::Pay: return "Pay";
        case BleedoutOutcome::Pleasure: return "Pleasure";
        case BleedoutOutcome::Captive: return "Captive";
        case BleedoutOutcome::Cancel: return "Cancel";
        case BleedoutOutcome::Failed: return "Failed";
        default: return "UnknownBleedoutOutcome";
        }
    }

    const char* ToString(NonCaptiveFallbackResolution value)
    {
        switch (value) {
        case NonCaptiveFallbackResolution::None: return "None";
        case NonCaptiveFallbackResolution::RecoveryFollower: return "RecoveryFollower";
        case NonCaptiveFallbackResolution::RecoveryPotion: return "RecoveryPotion";
        case NonCaptiveFallbackResolution::RescueCached: return "RescueCached";
        case NonCaptiveFallbackResolution::LeftForDeadSolo: return "LeftForDeadSolo";
        case NonCaptiveFallbackResolution::LeftForDeadWithFollower: return "LeftForDeadWithFollower";
        default: return "UnknownNonCaptiveFallbackResolution";
        }
    }


    const char* Controller::ToString(CaptiveOutcome value)
    {
        switch (value) {
        case CaptiveOutcome::None: return "None";
        case CaptiveOutcome::Pleasure: return "Pleasure";
        case CaptiveOutcome::WorkForEnemy: return "WorkForEnemy";
        case CaptiveOutcome::EscapeStarted: return "EscapeStarted";
        case CaptiveOutcome::EscapeSucceeded: return "EscapeSucceeded";
        case CaptiveOutcome::EscapeFailed: return "EscapeFailed";
        case CaptiveOutcome::Recaptured: return "Recaptured";
        case CaptiveOutcome::Release: return "Release";
        case CaptiveOutcome::Cancel: return "Cancel";
        case CaptiveOutcome::Failed: return "Failed";
        default: return "UnknownCaptiveOutcome";
        }
    }
}
