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
#include "TFDVictory.h"
#include "TFDTame.h"
#include "TFDActor.h"
#include "TFDDefeatMonitor.h"
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
    static RE::TESGlobal* g_victoryState = nullptr;
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
    constexpr const char* kPleasureOutcomeReleaseEvent = "TFDPleasureOutcomeRelease";
    constexpr const char* kCaptiveOutcomeWorkEvent = "TFDCaptiveOutcomeWork";
    constexpr const char* kCaptiveOutcomeReturnEvent = "TFDCaptiveOutcomeReturn";
    constexpr const char* kCaptiveOutcomeReleaseEvent = "TFDCaptiveOutcomeRelease";
    constexpr const char* kCaptiveOutcomeEscapeEvent = "TFDCaptiveOutcomeEscape";
    constexpr const char* kCaptiveOutcomePleasureEvent = "TFDCaptiveOutcomePleasure";
    constexpr const char* kCaptiveOutcomeCancelEvent = "TFDCaptiveOutcomeCancel";
    constexpr const char* kCaptiveWorkRefreshResourcesEvent = "TFDCaptiveWorkRefreshResources";
    constexpr const char* kCaptiveWorkNoJobEvent = "TFDCaptiveWorkNoJob";
    constexpr const char* kCaptiveWorkMiningCompletedEvent = "TFDCaptiveWorkMiningCompleted";
    constexpr const char* kVictoryOutcomeRecruitEvent = "TFDVictoryOutcomeRecruit";
    constexpr const char* kVictoryOutcomeKillEvent = "TFDVictoryOutcomeKill";
    constexpr const char* kVictoryOutcomeLootEvent = "TFDVictoryOutcomeLoot";
    constexpr const char* kVictoryOutcomeCancelEvent = "TFDVictoryOutcomeCancel";
    constexpr const char* kVictoryOutcomePleasureEvent = "TFDVictoryOutcomePleasure";
    constexpr const char* kAfterPleasureEnterEvent = "TFDAfterPleasureEnter";
    constexpr const char* kAfterPleasureForceOpenEvent = "TFDAfterPleasureForceOpen";
    constexpr const char* kPleasureFailedEnterEvent = "TFDPleasureFailedEnter";
    constexpr const char* kPleasureFailedAggroEvent = "TFDPleasureFailedAggro";
    constexpr const char* kPleasureFailedStartCombatEvent = "TFDPleasureFailedStartCombat";
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
    ResolveGlobal(g_victoryState, "TFDVictoryState");
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
    case TFD::FlowController::ObservedMainState::Victory:
        return "Victory";
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
    case RootFlow::Victory:
        return ObservedMainState::Victory;
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
    if (observed.victoryGlobal >= 2) {
        return ObservedMainState::Victory;
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
        { TFD::FlowController::ObservedReasonFlag::RootVictory, "root_victory" },
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
    observed.victoryGlobal = GetGlobalValueInt(g_victoryState);
    observed.defeatGlobal = GetGlobalValueInt(g_defeatState);
    observed.captiveGlobal = GetGlobalValueInt(g_captiveState);
    observed.pleasureGlobal = GetGlobalValueInt(g_pleasureState);
    observed.dialogueGlobal = GetGlobalValueInt(g_dialogueState);
    observed.rootProjected = ProjectMainStateFromRoot(observed.flow.root);

    if (observed.flow.root == RootFlow::Captive) {
        AddObservedReasonFlag(observed.reasonFlags, ObservedReasonFlag::CaptiveRoot);
    }
    if (observed.flow.root == RootFlow::Bleedout || observed.flow.root == RootFlow::LeftForDead) {
        AddObservedReasonFlag(observed.reasonFlags, ObservedReasonFlag::BleedoutRoot);
    }
    if (observed.flow.root == RootFlow::InCombat || combatActive) {
        AddObservedReasonFlag(observed.reasonFlags, ObservedReasonFlag::RootInCombat);
    }
    if (observed.flow.root == RootFlow::Victory) {
        AddObservedReasonFlag(observed.reasonFlags, ObservedReasonFlag::RootVictory);
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

            if (targetsPlayerSide ||
                (observed.playerInCombat && hostileToPlayer) ||
                (info.inCombat && hostileToPlayer && (mutualLos || info.dist <= 1800.0f))) {
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
    if (observed.captiveRuntimeActive) {
        observed.observed = ObservedMainState::Captive;
    }
    else if (observed.playerBleedRuntimeActive) {
        observed.observed = ObservedMainState::Defeat;
    }
    else if (observed.activeHostileCount > 0) {
        observed.observed = ObservedMainState::Incombat;
    }
    else if (observed.defeatedLivingEnemyCount > 0) {
        observed.observed = ObservedMainState::Victory;
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
        "[TFD][Flow][R93B] observed={} rootProjected={} globalsProjected={} mismatchRoot={} mismatchGlobals={} reason={} flags={} root={} ctx={} gate={} sub={} captiveMode={} token={} primary={:08X} combatActive={} globals(pre={} in={} victory={} defeat={} captive={} pleasure={} dialogue={}) counts(scanned={} activeHostile={} mutualLos={} defeatedLiving={} playerSide={}) actor={:08X} target={:08X} tickReason={}",
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
        observed.victoryGlobal,
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


static bool IsAutoVictoryRootAllowed(TFD::FlowController::RootFlow root)
{
    using TFD::FlowController::RootFlow;
    return root == RootFlow::None || root == RootFlow::InCombat || root == RootFlow::Victory;
}

static bool ShouldAutoEnterObservedVictory(const TFD::FlowController::ObservedMainStateSnapshot& observed)
{
    using TFD::FlowController::ObservedMainState;
    using TFD::FlowController::RootFlow;
    using TFD::FlowController::SubFlow;

    if (observed.observed != ObservedMainState::Victory) {
        return false;
    }
    if (observed.activeHostileCount > 0 || observed.defeatedLivingEnemyCount == 0 || observed.reasonActorFormID == 0) {
        return false;
    }
    if (!IsAutoVictoryRootAllowed(observed.flow.root)) {
        return false;
    }
    if (observed.flow.root == RootFlow::Victory && observed.flow.primaryActorFormID == observed.reasonActorFormID) {
        return false;
    }
    if (observed.flow.sub != SubFlow::None || observed.flow.terminalResolved) {
        return false;
    }
    if (observed.captiveRuntimeActive || observed.playerBleedRuntimeActive) {
        return false;
    }
    return true;
}

static constexpr double kObservedVictoryDialogueReadyHoldSec = 8.0;

static void AutoEnterObservedVictoryIfNeeded(const TFD::FlowController::ObservedMainStateSnapshot& observed, std::string_view tickReason)
{
    if (!ShouldAutoEnterObservedVictory(observed)) {
        return;
    }

    bool armedDialogueReadyHold = false;
    if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(observed.reasonActorFormID)) {
        TFD::Victory::ArmDialogueReadyHold(actor, kObservedVictoryDialogueReadyHoldSec, "observed_defeated_living_enemy");
        armedDialogueReadyHold = true;
    }

    TFD::Victory::SetStateValue(2);
    const bool ok = TFD::FlowController::Controller::GetSingleton().RequestVictory(
        observed.reasonActorFormID,
        "observed_defeated_living_enemy");
    spdlog::info(
        "[TFD][Flow][R93N] auto enter Victory observed actor={:08X} ok={} readyHold={} root={} ctx={} globalsVictory={} tickReason={} counts(activeHostile={} defeatedLiving={} mutualLos={})",
        observed.reasonActorFormID,
        ok ? 1 : 0,
        armedDialogueReadyHold ? 1 : 0,
        TFD::FlowController::Controller::ToString(observed.flow.root),
        TFD::FlowController::Controller::ToString(observed.flow.contextRoot),
        observed.victoryGlobal,
        tickReason.empty() ? std::string{ "-" } : std::string{ tickReason },
        observed.activeHostileCount,
        observed.defeatedLivingEnemyCount,
        observed.mutualLosHostileCount);
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

static bool IsActorCoveredByCurrentPassiveContext(RE::Actor* actor)
{
    if (!actor || actor == RE::PlayerCharacter::GetSingleton() || actor->IsDead() || actor->IsDisabled()) {
        return false;
    }

    if (g_passiveRuntimeProviders.isActorCoveredByPassiveContext) {
        return g_passiveRuntimeProviders.isActorCoveredByPassiveContext(actor);
    }

    if (TFD::HostilityController::IsSuppressed(actor)) {
        return true;
    }

    if (TFD::PleasureRuntime::IsActorTracked(actor)) {
        return true;
    }

    if (auto* primary = ResolveCurrentPassivePrimaryActor()) {
        if (actor == primary) {
            return true;
        }
        if (TFD::Actor::SharesAllowedFactionExact(actor, primary)) {
            return true;
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

    // 02AA: Pleasure Failed -> Fight is a combat handoff, not a dialogue phase.
    // TFDAfterPleasureFaction is required for the CK root greet condition, but if it
    // survives into Fight it is treated by HostilityController as an active dialogue
    // phase marker and suppresses UpdateCombat/StartCombat. Clear it before rehostile.
    const bool afterRemoved = RemoveFactionByEditorID(actor, "TFDAfterPleasureFaction", reason);

    // WorkingCaptive should already be cleared by ClearReleasedWorkRuntime(), but clear
    // it here too as a hard guard before combat handoff.
    const bool workingRemoved = RemoveFactionByEditorID(actor, "TFDWorkingCaptiveFaction", reason);

    if (afterRemoved || workingRemoved) {
        actor->AllowPCDialogue(true);
        actor->EvaluatePackage(false, true);
        actor->EvaluatePackage(true, true);
        spdlog::info(
            "[TFD][Flow] pleasure failed fight suppressors cleared actor={:08X} after={} working={} reason={}",
            actor->GetFormID(),
            afterRemoved ? 1 : 0,
            workingRemoved ? 1 : 0,
            reason ? reason : "unknown");
    }
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

        // R132: a Bleedout crowd continuation after AfterPleasure is a
        // source-owned Bleedout decision, even though the player may already
        // be physically standing after the OStim handoff/recovery.  The Bleedout
        // root and child topics depend on the defeat globals matching the
        // Bleedout decision context.  Without this override the native opener
        // can fire the root line, but the choice stack closes immediately
        // because TFDDefeatState has drifted back to the standing value.
        const bool captiveEscapeBleedoutDecisionRoot =
            _snapshot.root == RootFlow::Captive &&
            _snapshot.gate == DecisionGate::PlayerBleedout &&
            (_snapshot.sub == SubFlow::EscapeFailed || _snapshot.sub == SubFlow::Recapture) &&
            !_snapshot.terminalResolved;
        const bool bleedoutDecisionRoot =
            ((_snapshot.root == RootFlow::Bleedout && _snapshot.sub == SubFlow::None) || captiveEscapeBleedoutDecisionRoot) &&
            _snapshot.gate == DecisionGate::PlayerBleedout &&
            !_snapshot.terminalResolved;

        const int observedDefeat = g_defeatState ? static_cast<int>(std::lround(g_defeatState->value)) : 0;
        const int defeat = bleedoutDecisionRoot ? 2 : observedDefeat;

        const auto runtimePhase = TFD::PleasureRuntime::GetPhase();
        const auto runtimeSource = TFD::PleasureRuntime::GetSourceContext();
        const bool bleedoutAfterPleasure =
            runtimeSource == TFD::PleasureRuntime::SourceContext::Bleedout &&
            (runtimePhase == TFD::PleasureRuntime::Phase::AfterPleasureAwaitQuest ||
                runtimePhase == TFD::PleasureRuntime::Phase::AfterPleasureDialogue ||
                runtimePhase == TFD::PleasureRuntime::Phase::Finalizing);

        int inCombat = (defeat == 2) ? 0 : (_combatActive ? 1 : 0);
        if (_snapshot.sub == SubFlow::BleedoutAfterPleasure || bleedoutAfterPleasure) {
            inCombat = 0;
        }

        int captive = 0;
        if (_snapshot.root == RootFlow::Captive && _snapshot.captiveMode != CaptiveMode::JoinedEnemy) {
            const bool captiveEscapeSub =
                _snapshot.sub == SubFlow::EscapeAttempt ||
                _snapshot.sub == SubFlow::EscapeFailed ||
                _snapshot.sub == SubFlow::Recapture;

            if (captiveEscapeSub && !_snapshot.terminalResolved) {
                captive = 2;
            } else {
                captive = static_cast<int>(TFD::Captive::GetPhaseRaw());
                if (captive <= 0) {
                    captive = 1;
                }
            }
        }

        int pleasure = 0;
        if (TFD::PleasureRuntime::IsActive()) {
            switch (TFD::PleasureRuntime::GetPhase()) {
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
            case SubFlow::VictoryPleasure:
                pleasure = 1;
                break;
            case SubFlow::PreCombatAfterPleasure:
            case SubFlow::InCombatAfterPleasure:
            case SubFlow::BleedoutAfterPleasure:
            case SubFlow::CaptiveAfterPleasure:
            case SubFlow::VictoryAfterPleasure:
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
        if (bleedoutDecisionRoot) {
            SetGlobalInt(g_defeatState, defeat);
        }
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

        if (input.hasRecoveryPotion) {
            return NonCaptiveFallbackResolution::RecoveryPotion;
        }

        if (input.hasCachedRescueDestination) {
            return NonCaptiveFallbackResolution::RescueCached;
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
        if (player && !player->IsDead() && !player->IsDisabled()) {
            player->NotifyAnimationGraph("BleedoutStart");
        }
        handlers.clearLastAggressor();
        handlers.updatePreCombatState();

        spdlog::info("[TFD][Flow] committed no-marker fallback branch={} reason={}",
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
        if (g_battleObserverRuntimeProviders.recoverVictoryTeammates) {
            g_battleObserverRuntimeProviders.recoverVictoryTeammates();
        }
        if (g_battleObserverRuntimeProviders.resetBleedRuntimeState) {
            g_battleObserverRuntimeProviders.resetBleedRuntimeState();
        }
        if (g_battleObserverRuntimeProviders.clearPendingDialogueTarget) {
            g_battleObserverRuntimeProviders.clearPendingDialogueTarget();
        }
        if (g_battleObserverRuntimeProviders.setPlayerBleedImmune) {
            g_battleObserverRuntimeProviders.setPlayerBleedImmune(false);
        }

        const bool fallbackCommitted = TFD::Transition::DefeatGlue::BeginResolvedNoMarkerFallback(why);
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

    bool HandleVictoryOutcomeModEvent(std::string_view name, std::string_view arg, RE::TESForm* sender)
    {
        if (name.rfind("TFDVictoryOutcome", 0) != 0) {
            return false;
        }

        auto& flow = TFD::FlowController::Controller::GetSingleton();
        const auto actorFormID = ResolveActorFormIDFromEventArgOrSender(arg, sender);
        const auto senderFormID = sender ? sender->GetFormID() : 0u;

        VictoryOutcome outcome = VictoryOutcome::None;
        const char* reason = "mod_event_victory";
        const char* completeReason = "mod_event_victory_complete";
        bool terminalOutcome = true;

        if (name == kVictoryOutcomeRecruitEvent) {
            outcome = VictoryOutcome::RecruitEnemy;
            reason = "mod_event_victory_recruit";
            completeReason = "mod_event_victory_recruit_complete";
        }
        else if (name == kVictoryOutcomeKillEvent) {
            outcome = VictoryOutcome::KillEnemy;
            reason = "mod_event_victory_kill";
            completeReason = "mod_event_victory_kill_complete";
        }
        else if (name == kVictoryOutcomeLootEvent) {
            outcome = VictoryOutcome::Cancel;
            reason = "mod_event_victory_loot";
            completeReason = "mod_event_victory_loot_complete";
        }
        else if (name == kVictoryOutcomeCancelEvent) {
            outcome = VictoryOutcome::Cancel;
            reason = "mod_event_victory_cancel";
            completeReason = "mod_event_victory_cancel_complete";
        }
        else if (name == kVictoryOutcomePleasureEvent) {
            outcome = VictoryOutcome::Pleasure;
            reason = "mod_event_victory_pleasure";
            completeReason = "mod_event_victory_pleasure_complete";
            terminalOutcome = false;
        }
        else {
            return false;
        }

        const auto before = flow.GetSnapshot();
        bool ok = false;
        bool completeOk = false;
        bool forcedClear = false;

        if (before.root == RootFlow::Victory) {
            ok = flow.RequestResolveVictoryOutcome(outcome, actorFormID, reason);
        }
        else if (before.contextRoot == RootFlow::Victory && before.terminalResolved) {
            ok = true;
        }
        else {
            spdlog::info(
                "[TFD][Flow] victory outcome event={} actor={:08X} sender={:08X} ignored root={} ctx={} terminal={} primary={:08X}",
                std::string(name),
                actorFormID,
                senderFormID,
                Controller::ToString(before.root),
                Controller::ToString(before.contextRoot),
                before.terminalResolved ? 1 : 0,
                before.primaryActorFormID);
            return true;
        }

        if (ok && terminalOutcome) {
            completeOk = flow.RequestCompleteTerminalContext(completeReason);
            if (!completeOk) {
                const auto afterResolve = flow.GetSnapshot();
                if (afterResolve.contextRoot == RootFlow::Victory || afterResolve.root == RootFlow::Victory) {
                    flow.ResetRuntime(completeReason);
                    forcedClear = true;
                }
            }
        }
        else if (!ok && terminalOutcome) {
            const auto afterReject = flow.GetSnapshot();
            if (afterReject.root == RootFlow::Victory) {
                flow.ResetRuntime(completeReason);
                forcedClear = true;
            }
        }

        spdlog::info(
            "[TFD][Flow] victory outcome event={} actor={:08X} sender={:08X} ok={} complete={} forcedClear={} terminal={} reason={}",
            std::string(name),
            actorFormID,
            senderFormID,
            ok ? 1 : 0,
            completeOk ? 1 : 0,
            forcedClear ? 1 : 0,
            terminalOutcome ? 1 : 0,
            reason);
        return true;
    }

    bool IsCaptiveEscapeGuardActive(TFD::FlowController::Controller& flow)
    {
        if (TFD::Captive::IsEscapeActive() || TFD::Captive::HasEscapeBreakRebleedPending() || TFD::Captive::IsEscapeBleedoutActive()) {
            return true;
        }

        const auto snapshot = flow.GetSnapshot();
        return snapshot.root == RootFlow::Captive &&
            (snapshot.sub == SubFlow::EscapeAttempt ||
                snapshot.sub == SubFlow::EscapeFailed ||
                snapshot.sub == SubFlow::Recapture);
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

        (void)TFD::PleasureRuntime::HandleModEvent(eventName, strArg ? strArg : "", numArg, sender);

        if (IsAfterPleasureTerminalChoiceEvent(name)) {
            auto* actor = ResolveActorFromEventArg(arg);
            const int sourceFlow = ResolveSourceFlowFromEventArg(arg);
            if (actor && IsInCombatSource(sourceFlow)) {
                auto& flow = TFD::FlowController::Controller::GetSingleton();
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
            if (actor && sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::Bleedout)) {
                const char* terminalReason = ResolveBleedoutAfterPleasureTerminalReason(name);

                auto& flow = TFD::FlowController::Controller::GetSingleton();
                const auto snapshot = flow.GetSnapshot();
                const bool captiveBleedoutWorkHandoff =
                    name == kAfterPleasureChoiceWorkEvent &&
                    (snapshot.root == RootFlow::Captive ||
                        snapshot.contextRoot == RootFlow::Captive ||
                        TFD::Captive::IsActive());

                if (captiveBleedoutWorkHandoff) {
                    const bool completeFlow = flow.RequestCompleteAfterPleasure("captive_bleedout_after_pleasure_work_handoff");
                    (void)TFD::FlowController::QueueBridgeModEvent(
                        "TFDSystemEventClearAfterPleasure",
                        nullptr,
                        "captive_bleedout_after_pleasure_work_handoff",
                        1.0f);

                    spdlog::info(
                        "[TFD][Flow] captive bleedout after pleasure work handoff event={} source={} actor={:08X} complete={} root={} ctx={} sub={} reason={}",
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
                    if (name == kAfterPleasureChoiceRecruitEvent) {
                        demoteHold = TFD::HostilityController::DemoteTruceActorForCycleHold(
                            actor,
                            "bleedout_after_pleasure_recruit_cycle_hold");
                    }
                    else {
                        releaseSingle = TFD::HostilityController::ReleaseSingleTruceActorForCycle(
                            actor,
                            TFD::HostilityController::ReleaseReason::FlowHandoff,
                            terminalReason);
                    }

                    // R130: Bleedout now follows the InCombat cycle pattern. PleasureRuntime
                    // pre-selects the next crowd speaker before recruit cleanup, so do not run
                    // the old late Bleedout crowd scan after the runtime has already finalized.
                    TFD::Bleedout::CompleteAfterPleasure("bleedout_after_pleasure_cycle_preserve_crowd");
                    spdlog::info(
                        "[TFD][Flow][R130] bleedout after pleasure terminal preserved cycle event={} source={} actor={:08X} cycleQueued=1 releaseSingle={} demoteHold={} policy={}",
                        std::string(name),
                        sourceFlow,
                        actor->GetFormID(),
                        releaseSingle ? 1 : 0,
                        demoteHold ? 1 : 0,
                        name == kAfterPleasureChoiceRecruitEvent ? "recruit_hold_until_bleed_cycle_end" : "current_actor_only");
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

            const bool sourceIsCaptive =
                sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::Captive) ||
                TFD::PleasureRuntime::GetSourceContext() == TFD::PleasureRuntime::SourceContext::Captive;
            const bool returnToCaptive =
                name == kAfterPleasureChoiceFinishEvent ||
                name == kAfterPleasureChoiceKidnapEvent;

            if (sourceIsCaptive && returnToCaptive) {
                auto& flow = TFD::FlowController::Controller::GetSingleton();
                auto* captiveActor = actor ? actor : TFD::PleasureRuntime::GetPrimarySpeaker();
                const char* terminalReason = name == kAfterPleasureChoiceKidnapEvent
                    ? "captive_after_pleasure_return"
                    : "captive_after_pleasure_finish";

                // If Pleasure was chosen from Captive EscapeFailed bleedout,
                // AfterPleasure terminal must finish the recapture commit, not
                // merely restore CaptiveIdle. Otherwise the bleedout/escape FSM
                // remains armed and the loot stash objective is not reopened for
                // the recapture cycle.
                if (TFD::Captive::IsEscapeBleedoutActive()) {
                    const bool recaptured = TFD::Captive::CommitRecapture(captiveActor, terminalReason);
                    (void)TFD::FlowController::QueueBridgeModEvent(
                        "TFDSystemEventClearAfterPleasure",
                        nullptr,
                        "captive_after_pleasure_recapture",
                        1.0f);
                    spdlog::info(
                        "[TFD][Flow] captive escape-bleedout after pleasure terminal event={} source={} actor={:08X} recaptured={} reason={}",
                        std::string(name),
                        sourceFlow,
                        captiveActor ? captiveActor->GetFormID() : 0u,
                        recaptured ? 1 : 0,
                        terminalReason);
                    return true;
                }

                // Captive Pleasure is a temporary Scene phase only.  Once the
                // AfterPleasure terminal choice returns to captive, restore the
                // runtime phase before refreshing Flow globals; otherwise
                // TFDCaptiveState stays at 4 and Captive escape watchers never
                // arm again.
                TFD::Captive::SetRuntimeState(true, TFD::Captive::PhaseValue::Captive);
                if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                    TFD::Captive::SyncPlayerAlias(player, terminalReason);
                    TFD::Captive::ArmEscapeContextFromCurrentState(player);
                    TFD::Captive::ResetLockpickWatch();
                    TFD::Captive::SealDoorIfPresent();
                }

                const bool completeFlow = flow.RequestCompleteAfterPleasure(terminalReason);
                TFD::HostilityController::TickCaptiveSuppression();
                (void)TFD::FlowController::QueueBridgeModEvent(
                    "TFDSystemEventClearAfterPleasure",
                    nullptr,
                    terminalReason,
                    1.0f);

                spdlog::info(
                    "[TFD][Flow] captive after pleasure terminal event={} source={} actor={:08X} flowComplete={} restoredPhase=Captive reason={}",
                    std::string(name),
                    sourceFlow,
                    captiveActor ? captiveActor->GetFormID() : 0u,
                    completeFlow ? 1 : 0,
                    terminalReason);
                return true;
            }
        }

        if (HandleVictoryOutcomeModEvent(name, arg, sender)) {
            return true;
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
            if (actor) {
                TFD::InteractionRouter::DialogueOpen::BeginPleasureFailed(actor);
                spdlog::info(
                    "[TFD][Flow] pleasure failed greet armed source={} actor={:08X} reason=no_speaker_climax",
                    sourceFlow,
                    actor->GetFormID());
            }
            else {
                spdlog::warn("[TFD][Flow] pleasure failed enter rejected no actor source={} arg={}", sourceFlow, std::string(arg));
            }
            return true;
        }

        if (name == kPleasureFailedAggroEvent) {
            auto* actor = ResolveActorFromEventArg(arg);
            const int sourceFlow = ResolveSourceFlowFromEventArg(arg);
            auto* player = RE::PlayerCharacter::GetSingleton();

            if (actor) {
                TFD::DefeatMonitor::SuppressDefeatedEnemyAutoDeathForActor(actor, 24.0, "pleasure_failed_fight_speaker");
                ClearPleasureFailedFightSpeakerSuppressors(actor, "pleasure_failed_fight_pre_break");
            }

            TFD::PleasureRuntime::Break("pleasure_failed_fight", true, true, true);
            TFD::InteractionRouter::DialogueOpen::Cancel();

            if (actor) {
                ClearPleasureFailedFightSpeakerSuppressors(actor, "pleasure_failed_fight_post_cancel");
            }
            (void)TFD::FlowController::QueueBridgeModEvent(
                "TFDSystemEventClearAfterPleasure",
                actor,
                "pleasure_failed_fight",
                1.0f);

            bool truceReleased = false;
            if (actor) {
                truceReleased = TFD::HostilityController::ReleaseActiveTruceSessionForActor(
                    actor,
                    TFD::HostilityController::ReleaseReason::FightChoice,
                    false);
            }

            bool captiveEscapeArmed = false;
            if (actor && sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::Captive)) {
                auto& flow = TFD::FlowController::Controller::GetSingleton();
                captiveEscapeArmed = flow.RequestResolveCaptiveOutcome(
                    TFD::FlowController::CaptiveOutcome::EscapeStarted,
                    actor->GetFormID(),
                    "pleasure_failed_fight") || captiveEscapeArmed;
                TFD::Captive::SetRuntimeState(true, TFD::Captive::PhaseValue::Escape);
                TFD::Captive::ClearReleasedWorkRuntime("pleasure_failed_fight");
                TFD::Location::ClearCaptiveWorkResourceState("pleasure_failed_fight");
                ClearPleasureFailedFightSpeakerSuppressors(actor, "pleasure_failed_fight_after_escape_state");
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
                if (!actor->IsWeaponDrawn()) {
                    actor->DrawWeaponMagicHands(true);
                }
                actor->EvaluatePackage(false, true);
                actor->EvaluatePackage(true, true);
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
                TFD::FlowController::Controller::GetSingleton().NotifyCombatStarted(actor->GetFormID(), "pleasure_failed_fight");
                TFD::DefeatMonitor::SuppressDefeatedEnemyAutoDeathForActor(actor, 24.0, "pleasure_failed_fight_post_rehostile");
                ClearPleasureFailedFightSpeakerSuppressors(actor, "pleasure_failed_fight_post_rehostile");
            }

            const bool combatQueued = TFD::FlowController::QueueBridgeModEvent(
                kPleasureFailedStartCombatEvent,
                actor,
                "pleasure_failed_fight",
                static_cast<float>(sourceFlow));

            spdlog::info(
                "[TFD][Flow] pleasure failed fight actor={:08X} source={} truceReleased={} captiveEscapeArmed={} nativeCombatRefresh={} combatQueued={} reason=no_speaker_climax",
                actor ? actor->GetFormID() : 0u,
                sourceFlow,
                truceReleased ? 1 : 0,
                captiveEscapeArmed ? 1 : 0,
                nativeCombatRefresh ? 1 : 0,
                combatQueued ? 1 : 0);
            return true;
        }

        if (name == kAfterPleasureEnterEvent) {
            auto* actor = ResolveActorFromEventArg(arg);
            const int sourceFlow = ResolveSourceFlowFromEventArg(arg);
            auto& flow = TFD::FlowController::Controller::GetSingleton();
            if (actor && sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::Bleedout)) {
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
                if (beginAfter || flow.IsInCombatAfterPleasureContextActive()) {
                    (void)TFD::InCombat::HandleAfterPleasureEnter(
                        actor,
                        "after_pleasure_enter",
                        TFD::InCombat::AfterPleasureHandlers{
                            [&](std::uint32_t actorFormID, const char* r) { TFD::InCombat::NoteAfterPleasure(actorFormID, r); },
                            [&](RE::Actor* greetActor, const char* r) -> bool { return TFD::InCombatGreet::BeginAfterPleasure(greetActor, r); }
                        });
                    spdlog::info("[TFD][Flow][R93T] incombat after pleasure greet armed source={} actor={:08X} flowBegin={}", sourceFlow, actor->GetFormID(), beginAfter ? 1 : 0);
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
                TFD::InteractionRouter::DialogueOpen::BeginAfterPleasure(actor);
                spdlog::info("[TFD][Flow] teammate after pleasure greet armed source={} actor={:08X}", sourceFlow, actor->GetFormID());
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

            const bool cycleReleaseGrace = actor ? ConsumeInCombatCycleReleaseGrace(actor->GetFormID()) : false;
            if (actor && cycleReleaseGrace) {
                TFD::Actor::Ops::RemoveReleaseFollowGraceFromActorOnly(actor, "incombat_cycle_release_end");
                graceRemoved = true;
            }
            else if (actor && g_outcomeRuntimeProviders.removeReleaseFollowGrace) {
                g_outcomeRuntimeProviders.removeReleaseFollowGrace(actor, "incombat_release_end");
                graceRemoved = true;
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

            // R93W: Match PreCombat Pay > Release routing.  Do not release the
            // TruceInCombat session with Generic here: Generic removes the truce
            // while Papyrus has just stopped combat, leaving actors pacified with
            // no release-to-vanilla grace/finalize lifecycle.  Route through the
            // shared Release module instead; it arms grace, releases the truce as
            // FlowHandoff, and finalizes cleanup after the grace timer.
            const auto flushedDeferred = TFD::PleasureRuntime::FlushDeferredInCombatRecruits("mod_event_incombat_release_final");
            SKSE::ModCallbackEvent releaseCallbackEv{ eventName, strArg ? strArg : "", numArg, sender };
            const bool releaseArmed = TFD::Release::HandleModCallbackEvent(&releaseCallbackEv);
            TFD::InCombat::Complete("mod_event_incombat_release_arm");
            spdlog::info(
                "[TFD][Flow][R95B] incombat release routed via TFDRelease actor={:08X} flowDone={} terminalComplete={} releaseArmed={} deferredRegistered={} duration={:.2f}",
                actorFormID,
                flowDone ? 1 : 0,
                terminalComplete ? 1 : 0,
                releaseArmed ? 1 : 0,
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

            TFD::InCombat::GraceEventContext context{};
            context.eventName = eventName;
            context.actor = actor;
            context.durationSec = followSeconds;
            (void)TFD::InCombat::HandleReleaseFollowEvent(
                context,
                TFD::InCombat::GraceEventHandlers{
                    [&](RE::Actor* graceActor, double seconds, const char* graceReason) {
                        if (g_outcomeRuntimeProviders.applyReleaseFollowGrace) {
                            g_outcomeRuntimeProviders.applyReleaseFollowGrace(graceActor, seconds, graceReason);
                        }
                    }
                });
            const auto flushedDeferred = TFD::PleasureRuntime::FlushDeferredInCombatRecruits("mod_event_incombat_follow_final");
            const bool truceReleased = actor ? TFD::HostilityController::ReleaseActiveTruceSessionForActor(actor, TFD::HostilityController::ReleaseReason::Generic, false) : false;
            TFD::InCombat::Complete("mod_event_incombat_follow");
            spdlog::info(
                "[TFD][Flow][R95B] incombat follow terminal event={} actor={:08X} flowDone={} terminalComplete={} truceRelease={} deferredRegistered={}",
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
            bool flowResolved = false;
            bool leftForDeadStarted = false;
            if (bleedActive) {
                auto& flow = TFD::FlowController::Controller::GetSingleton();
                flowResolved = flow.RequestResolveBleedoutOutcome(BleedoutOutcome::Cancel, actorFormID, "bleedout_do_nothing_left_for_dead");
                TFD::FlowController::HandleObservedLeftForDead("bleedout_do_nothing_left_for_dead");
                leftForDeadStarted = true;
            }

            spdlog::info(
                "[TFD][Flow][R100L] bleedout do-nothing -> left-for-dead actor={:08X} bleedActive={} flowResolved={} started={}",
                actorFormID,
                bleedActive ? 1 : 0,
                flowResolved ? 1 : 0,
                leftForDeadStarted ? 1 : 0);
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
            constexpr const char* graceReason = "bleedout_release";
            if (actor && g_outcomeRuntimeProviders.applyReleaseFollowGrace) {
                g_outcomeRuntimeProviders.applyReleaseFollowGrace(actor, durationSec, graceReason);
            }
            else if (!actor) {
                spdlog::warn(
                    "[TFD][Flow][R100C] bleedout release has no valid actor arg={} resolved={:08X}",
                    strArg ? strArg : "",
                    actorFormID);
            }

            const bool bleedActive = IsBleedStateActiveFromProviders();
            bool flowResolved = false;
            bool terminalComplete = false;
            if (bleedActive) {
                auto& flow = TFD::FlowController::Controller::GetSingleton();
                flowResolved = flow.RequestResolveBleedoutOutcome(BleedoutOutcome::Cancel, actorFormID, graceReason);
                terminalComplete = TFD::Bleedout::CompletePayRelease(
                    graceReason,
                    TFD::Bleedout::Builders::BuildPayReleaseCompletionHandlers());
            }

            spdlog::info(
                "[TFD][Flow][R100C] bleedout release terminal actor={:08X} seconds={:.1f} bleedActive={} flowResolved={} terminalComplete={}",
                actorFormID,
                durationSec,
                bleedActive ? 1 : 0,
                flowResolved ? 1 : 0,
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
            const bool truceReleased = flowActor ? TFD::HostilityController::ReleaseActiveTruceSessionForActor(flowActor, TFD::HostilityController::ReleaseReason::Generic, false) : false;
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
            TFD::Bleedout::OutcomeEventContext context{};
            context.eventName = "mod_event_pay";
            context.rawEventName = eventName;
            context.actor = flowActor;
            context.actorFormID = actorFormID;
            context.inBleedState = IsBleedStateActiveFromProviders();
            (void)TFD::Bleedout::HandleOutcomePayEvent(context, bleedOutcomeEventHandlers);
            return true;
        }

        if (name == kBleedoutOutcomePleasureEvent) {
            const auto actorFormID = ResolveBleedFlowActorFormIDFromProviders();
            auto* flowActor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
            TFD::Bleedout::OutcomeEventContext context{};
            context.eventName = "mod_event_pleasure";
            context.rawEventName = eventName;
            context.actor = flowActor;
            context.actorFormID = actorFormID;
            context.inBleedState = IsBleedStateActiveFromProviders();
            context.preserveCaptive = TFD::Captive::IsCaptivePassiveHoldActive() || TFD::Captive::IsEscapeBleedoutActive();
            (void)TFD::Bleedout::HandleOutcomePleasureEvent(context, bleedOutcomeEventHandlers);
            return true;
        }

        if (name == kBleedoutOutcomeCaptiveEvent) {
            const auto actorFormID = ResolveBleedFlowActorFormIDFromProviders();
            auto* flowActor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
            TFD::Bleedout::OutcomeEventContext context{};
            context.eventName = "mod_event_captive";
            context.rawEventName = eventName;
            context.actor = flowActor;
            context.actorFormID = actorFormID;
            context.inBleedState = IsBleedStateActiveFromProviders();
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

        if (TFD::Captive::GetStateFlag()) {
            return DialogueContextKind::Captive;
        }

        if (flow.IsAfterPleasureSubFlowActive()) {
            return DialogueContextKind::AfterPleasure;
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

        if (TFD::Captive::GetStateFlag()) {
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
        if (g_passiveRuntimeProviders.clearAllReleaseFollowGrace) {
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

        if (severeCrime && g_passiveRuntimeProviders.clearPendingDialogueTarget) {
            g_passiveRuntimeProviders.clearPendingDialogueTarget();
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

        AutoEnterObservedVictoryIfNeeded(observed, reason);

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

    bool Controller::RequestVictory(std::uint32_t actorFormID, std::string_view reason)
    {
        return BeginVictory(actorFormID, reason);
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

    bool Controller::RequestResolveVictoryOutcome(VictoryOutcome outcome, std::uint32_t actorFormID, std::string_view reason)
    {
        return ResolveVictoryOutcome(outcome, actorFormID, reason);
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
        if (_snapshot.root == RootFlow::Captive || _snapshot.root == RootFlow::Victory ||
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
        if (_snapshot.root == RootFlow::Captive || _snapshot.root == RootFlow::Victory) {
            return RejectLocked("BeginInCombat", reason);
        }
        _combatActive = true;
        const bool ok = BeginRootLocked(RootFlow::InCombat, actorFormID, reason);
        RefreshFlowGlobalsLocked();
        return ok;
    }

    bool Controller::BeginCaptive(std::uint32_t actorFormID, CaptiveMode mode, std::string_view reason)
    {
        std::scoped_lock lk(_lock);
        const bool ok = BeginCaptiveLocked(actorFormID, mode, reason);
        RefreshFlowGlobalsLocked();
        return ok;
    }

    bool Controller::BeginVictory(std::uint32_t actorFormID, std::string_view reason)
    {
        std::scoped_lock lk(_lock);
        if (_snapshot.root == RootFlow::Captive) {
            return RejectLocked("BeginVictory", reason);
        }
        _combatActive = false;
        const bool ok = TransitionRootLocked(RootFlow::Victory, actorFormID, reason);
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
        if (_snapshot.root != RootFlow::PreCombat) {
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
            if (_snapshot.sub != SubFlow::PreCombatPayFollowup) {
                return RejectLocked("ResolvePreCombatOutcome", reason);
            }
            handled = EnterTerminalContextLocked(RootFlow::PreCombat, SubFlow::None, actorFormID, reason);
            break;
        case PreCombatOutcome::Cancel:
        case PreCombatOutcome::Failed:
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

        if (captiveEscapeBleedout) {
            bool handledCaptive = false;
            switch (outcome) {
            case BleedoutOutcome::Captive:
                _combatActive = false;
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
            handled = EnterTerminalContextLocked(RootFlow::Bleedout, SubFlow::BleedoutPleasure, actorFormID, reason);
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

    bool Controller::ResolveVictoryOutcome(VictoryOutcome outcome, std::uint32_t actorFormID, std::string_view reason)
    {
        std::scoped_lock lk(_lock);
        if (_snapshot.root != RootFlow::Victory) {
            return RejectLocked("ResolveVictoryOutcome", reason);
        }

        bool handled = false;
        switch (outcome) {
        case VictoryOutcome::RecruitEnemy:
        case VictoryOutcome::KillEnemy:
        case VictoryOutcome::Cancel:
        case VictoryOutcome::Failed:
            handled = EnterTerminalContextLocked(RootFlow::Victory, SubFlow::None, actorFormID, reason);
            break;
        case VictoryOutcome::Pleasure:
            handled = EnterTerminalContextLocked(RootFlow::Victory, SubFlow::VictoryPleasure, actorFormID, reason);
            break;
        case VictoryOutcome::None:
        default:
            return RejectLocked("ResolveVictoryOutcome", reason);
        }
        RefreshFlowGlobalsLocked();
        LogFlowSnapshot("ResolveVictoryOutcome", reason, _snapshot, actorFormID, ToString(outcome));
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
        case SubFlow::VictoryPleasure:
            _snapshot.sub = SubFlow::VictoryAfterPleasure;
            break;
        case SubFlow::PreCombatAfterPleasure:
        case SubFlow::InCombatAfterPleasure:
        case SubFlow::BleedoutAfterPleasure:
        case SubFlow::CaptiveAfterPleasure:
        case SubFlow::VictoryAfterPleasure:
            break;
        default:
            return RejectLocked("BeginAfterPleasure", reason);
        }
        SetPrimaryActorLocked(actorFormID);
        RefreshFlowGlobalsLocked();
        LogFlowSnapshot("BeginAfterPleasure", reason, _snapshot, actorFormID);
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
        case SubFlow::BleedoutPleasure:
        case SubFlow::BleedoutAfterPleasure:
        case SubFlow::VictoryPleasure:
        case SubFlow::VictoryAfterPleasure:
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
        if (_snapshot.root == RootFlow::Victory) {
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
        return root != RootFlow::Victory &&
            root != RootFlow::Rescue &&
            root != RootFlow::Recovery &&
            root != RootFlow::LeftForDead;
    }

    bool Controller::CanEnterVictory() const
    {
        std::scoped_lock lk(_lock);
        const auto root = ProjectExternalRootFlow(_snapshot);
        return root != RootFlow::Captive &&
            root != RootFlow::Rescue &&
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
        return (_snapshot.root == RootFlow::Bleedout && _snapshot.gate == DecisionGate::PlayerBleedout) || captiveEscapeBleedoutDecision;
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
        return _snapshot.root == RootFlow::Captive &&
            (_snapshot.sub == SubFlow::EscapeAttempt ||
                _snapshot.sub == SubFlow::EscapeFailed ||
                _snapshot.sub == SubFlow::Recapture);
    }

    bool Controller::IsPleasureSubFlowActive() const
    {
        std::scoped_lock lk(_lock);
        switch (_snapshot.sub) {
        case SubFlow::PreCombatPleasure:
        case SubFlow::InCombatPleasure:
        case SubFlow::BleedoutPleasure:
        case SubFlow::CaptivePleasure:
        case SubFlow::VictoryPleasure:
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
        case SubFlow::VictoryAfterPleasure:
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

        // Captive escape-failed bleedout is still owned by the Captive root, not
        // by the generic Bleedout root. Do not restart Captive here; just move
        // the existing Captive flow into the Pleasure sub-flow so the bleedout
        // recovery/idle prep can run before OStim starts.
        if (TFD::Captive::IsEscapeBleedoutActive()) {
            if (!RequestResolveCaptiveOutcome(CaptiveOutcome::Pleasure, actorFormID, requestReason)) {
                spdlog::warn("[TFD][Flow] ignore mod_event_pleasure reason=captive_escape_bleedout_flow_reject actor={:08X}", actorFormID);
                return false;
            }
            spdlog::info("[TFD][Flow] captive escape-bleedout pleasure accepted actor={:08X} reason={}",
                actorFormID,
                std::string(requestReason));
            return true;
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
        if (mode == CaptiveMode::None || _snapshot.root == RootFlow::Victory) {
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
        case RootFlow::Victory: return "Victory";
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
        case SubFlow::VictoryPleasure: return "VictoryPleasure";
        case SubFlow::VictoryAfterPleasure: return "VictoryAfterPleasure";
        case SubFlow::CaptiveIdle: return "CaptiveIdle";
        case SubFlow::CaptivePleasure: return "CaptivePleasure";
        case SubFlow::CaptiveAfterPleasure: return "CaptiveAfterPleasure";
        case SubFlow::WorkForEnemy: return "WorkForEnemy";
        case SubFlow::EscapeAttempt: return "EscapeAttempt";
        case SubFlow::EscapeFailed: return "EscapeFailed";
        case SubFlow::Recapture: return "Recapture";
        case SubFlow::JoinedEnemyIdle: return "JoinedEnemyIdle";
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

    const char* Controller::ToString(VictoryOutcome value)
    {
        switch (value) {
        case VictoryOutcome::None: return "None";
        case VictoryOutcome::RecruitEnemy: return "RecruitEnemy";
        case VictoryOutcome::KillEnemy: return "KillEnemy";
        case VictoryOutcome::Pleasure: return "Pleasure";
        case VictoryOutcome::Cancel: return "Cancel";
        case VictoryOutcome::Failed: return "Failed";
        default: return "UnknownVictoryOutcome";
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
