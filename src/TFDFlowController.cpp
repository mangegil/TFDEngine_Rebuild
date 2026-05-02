#include "TFDFlowController.h"
#include "TFDBleedout.h"
#include "TFDBleedoutGreet.h"
#include "TFDCaptive.h"
#include "TFDCaptiveGreet.h"
#include "TFDHostilityController.h"
#include "TFDInteractionRouter.h"
#include "TFDInCombat.h"
#include "TFDInCombatGreet.h"
#include "TFDPleasureRuntime.h"
#include "TFDPreCombatGreet.h"
#include "TFDRelease.h"
#include "TFDTransition.h"
#include "TFDTame.h"
#include "TFDActor.h"

#include <cmath>
#include <cstdlib>
#include <string>
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
    static TFD::FlowController::PassiveRuntimeProviders g_passiveRuntimeProviders{};
    static TFD::FlowController::OutcomeRuntimeProviders g_outcomeRuntimeProviders{};
    static TFD::FlowController::BattleObserverRuntimeProviders g_battleObserverRuntimeProviders{};
    static TFD::FlowController::ContinuousRuntimeProviders g_continuousRuntimeProviders{};
    static bool g_flowRuntimeInstalled = false;

    constexpr const char* kBleedoutOutcomePayEvent = "TFDBleedoutOutcomePay";
    constexpr const char* kBleedoutOutcomePleasureEvent = "TFDBleedoutOutcomePleasure";
    constexpr const char* kBleedoutOutcomeCaptiveEvent = "TFDBleedoutOutcomeCaptive";
    constexpr const char* kBleedoutOutcomeResetEvent = "TFDBleedoutOutcomeReset";
    constexpr const char* kBleedoutOutcomeReleaseEvent = "TFDBleedoutOutcomeRelease";
    constexpr const char* kInCombatOutcomePayEvent = "TFDInCombatOutcomePay";
    constexpr const char* kInCombatOutcomePleasureEvent = "TFDInCombatOutcomePleasure";
    constexpr const char* kInCombatOutcomeCaptiveEvent = "TFDInCombatOutcomeCaptive";
    constexpr const char* kInCombatOutcomeResetEvent = "TFDInCombatOutcomeReset";
    constexpr const char* kInCombatOutcomeReleaseEvent = "TFDInCombatOutcomeRelease";
    constexpr const char* kInCombatOutcomeFollowEvent = "TFDInCombatOutcomeFollow";
    constexpr const char* kPleasureOutcomeReleaseEvent = "TFDPleasureOutcomeRelease";
    constexpr const char* kCaptiveOutcomeWorkEvent = "TFDCaptiveOutcomeWork";
    constexpr const char* kCaptiveOutcomeReturnEvent = "TFDCaptiveOutcomeReturn";
    constexpr const char* kCaptiveOutcomeReleaseEvent = "TFDCaptiveOutcomeRelease";
    constexpr const char* kCaptiveOutcomeEscapeEvent = "TFDCaptiveOutcomeEscape";
    constexpr const char* kCaptiveOutcomePleasureEvent = "TFDCaptiveOutcomePleasure";
    constexpr const char* kCaptiveOutcomeCancelEvent = "TFDCaptiveOutcomeCancel";
    constexpr const char* kAfterPleasureEnterEvent = "TFDAfterPleasureEnter";
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

    static RE::FormID ResolveActorFormIDFromEventArg(const std::string_view& arg)
    {
        if (auto* actor = ResolveActorFromEventArg(arg)) {
            return actor->GetFormID();
        }
        return 0;
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
        } else {
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

        if (g_continuousRuntimeProviders.postRuntimeFlowTick && g_continuousRuntimeProviders.postRuntimeFlowTick(player)) {
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
        const int defeat = g_defeatState ? static_cast<int>(std::lround(g_defeatState->value)) : 0;

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
            captive = static_cast<int>(TFD::Captive::GetPhaseRaw());
            if (captive <= 0) {
                switch (_snapshot.sub) {
                case SubFlow::EscapeAttempt:
                case SubFlow::EscapeFailed:
                case SubFlow::Recapture:
                    captive = 2;
                    break;
                default:
                    captive = 1;
                    break;
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
        } else {
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

        if (!input.hasStandingHostileCoalition && input.hasStandingTeammate && !input.hadValidObservedEnemy) {
            return ObservedDefeatResolution::LeftForDead;
        }

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
        const std::string reasonText = reason.empty() ? std::string{"observed_defeat_resolution"} : std::string{reason};
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
            } else {
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
        if (g_battleObserverRuntimeProviders.queueNonCaptiveChoice) {
            g_battleObserverRuntimeProviders.queueNonCaptiveChoice(why);
        }
        spdlog::info("[TFD][Flow] observed battle resolved -> non-captive choice reason={}", why);
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
            }
        } else if (name == kCaptiveOutcomeReturnEvent) {
            reason = "mod_event_captive_return";
            ok = flow.RequestCaptive(actorFormID, CaptiveMode::Kidnapped, reason);
            if (ok) {
                TFD::Captive::SetRuntimeState(true, TFD::Captive::PhaseValue::Captive);
                if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                    TFD::Captive::SyncPlayerAlias(player, reason);
                }
            }
        } else if (name == kCaptiveOutcomeReleaseEvent) {
            reason = "mod_event_captive_release";
            ok = EnsureCaptiveRootForOutcome(flow, actorFormID, reason) &&
                flow.RequestResolveCaptiveOutcome(CaptiveOutcome::Release, actorFormID, reason);
            TFD::Captive::SetRuntimeState(false, TFD::Captive::PhaseValue::None);
            TFD::HostilityController::ClearAggressionClamp();
            TFD::Actor::Ops::ClearAggressorFactionContext();
            if (ok) {
                (void)flow.RequestCompleteTerminalContext("mod_event_captive_release_complete");
            } else {
                flow.ResetRuntime("mod_event_captive_release_force_clear");
            }
        } else if (name == kCaptiveOutcomeCancelEvent) {
            reason = "mod_event_captive_cancel";
            ok = EnsureCaptiveRootForOutcome(flow, actorFormID, reason) &&
                flow.RequestResolveCaptiveOutcome(CaptiveOutcome::Cancel, actorFormID, reason);
            TFD::Captive::SetRuntimeState(false, TFD::Captive::PhaseValue::None);
            TFD::HostilityController::ClearAggressionClamp();
            TFD::Actor::Ops::ClearAggressorFactionContext();
            if (ok) {
                (void)flow.RequestCompleteTerminalContext("mod_event_captive_cancel_complete");
            } else {
                flow.ResetRuntime("mod_event_captive_cancel_force_clear");
            }
        } else if (name == kCaptiveOutcomeEscapeEvent) {
            reason = "mod_event_captive_escape";
            ok = EnsureCaptiveRootForOutcome(flow, actorFormID, reason) &&
                flow.RequestResolveCaptiveOutcome(CaptiveOutcome::EscapeStarted, actorFormID, reason);
            if (ok) {
                TFD::Captive::SetRuntimeState(true, TFD::Captive::PhaseValue::Escape);
                TFD::HostilityController::ClearAggressionClamp();
                TFD::Actor::Ops::ClearAggressorFactionContext();
            }
        } else if (name == kCaptiveOutcomePleasureEvent) {
            reason = "mod_event_captive_pleasure";
            ok = EnsureCaptiveRootForOutcome(flow, actorFormID, reason) &&
                flow.RequestResolveCaptiveOutcome(CaptiveOutcome::Pleasure, actorFormID, reason);
            if (ok) {
                TFD::Captive::SetRuntimeState(true, TFD::Captive::PhaseValue::Scene);
            }
        } else {
            return false;
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

    bool HandleOutcomeModEvent(const char* eventName, const char* strArg, float numArg, RE::TESForm* sender)
    {
        if (!eventName || !eventName[0]) {
            return false;
        }

        const std::string_view name{ eventName };
        const std::string_view arg = strArg ? std::string_view{ strArg } : std::string_view{};

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

        if (HandleCaptiveOutcomeModEvent(name, arg, sender)) {
            return true;
        }

        if (name == kAfterPleasureEnterEvent) {
            auto* actor = ResolveActorFromEventArg(arg);
            const int sourceFlow = ResolveSourceFlowFromEventArg(arg);
            auto& flow = TFD::FlowController::Controller::GetSingleton();
            const bool inCombatAfterPleasure = flow.IsInCombatAfterPleasureContextActive();
            if (actor && sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::Bleedout)) {
                if (TFD::Bleedout::HandleAfterPleasureEnter(actor, "after_pleasure_enter")) {
                    TFD::BleedoutGreet::BeginAfterPleasure(actor, "after_pleasure_enter");
                    spdlog::info("[TFD][Flow] bleedout after pleasure greet armed source={} actor={:08X}", sourceFlow, actor->GetFormID());
                } else {
                    spdlog::warn("[TFD][Flow] bleedout after pleasure flow reject source={} actor={:08X}", sourceFlow, actor ? actor->GetFormID() : 0u);
                }
            } else if (actor && inCombatAfterPleasure) {
                (void)TFD::InCombat::HandleAfterPleasureEnter(
                    actor,
                    "after_pleasure_enter",
                    TFD::InCombat::AfterPleasureHandlers{
                        [&](std::uint32_t actorFormID, const char* r) { TFD::InCombat::NoteAfterPleasure(actorFormID, r); },
                        [&](RE::Actor* greetActor, const char* r) -> bool { return TFD::InCombatGreet::BeginAfterPleasure(greetActor, r); }
                    });
                spdlog::info("[TFD][Flow] incombat after pleasure greet armed source={} actor={:08X}", sourceFlow, actor->GetFormID());
            } else if (actor && sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::Captive)) {
                (void)TFD::CaptiveGreet::BeginAfterPleasure(actor, "after_pleasure_enter");
                spdlog::info("[TFD][Flow] captive after pleasure greet armed source={} actor={:08X}", sourceFlow, actor->GetFormID());
            } else if (actor && sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::Teammate)) {
                TFD::InteractionRouter::DialogueOpen::BeginAfterPleasure(actor);
                spdlog::info("[TFD][Flow] teammate after pleasure greet armed source={} actor={:08X}", sourceFlow, actor->GetFormID());
            } else if (actor && sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::PreCombat)) {
                if (flow.RequestBeginAfterPleasure(actor->GetFormID(), "after_pleasure_enter")) {
                    TFD::InteractionRouter::DialogueOpen::BeginAfterPleasure(actor);
                    spdlog::info("[TFD][Flow] precombat after pleasure greet armed source={} actor={:08X}", sourceFlow, actor->GetFormID());
                }
            }
            return true;
        }

        if (name == kInCombatOutcomeReleaseEvent || name == kInCombatOutcomeFollowEvent) {
            auto* actor = ResolveActorFromEventArg(arg);
            TFD::InCombat::GraceEventContext context{};
            context.eventName = eventName;
            context.actor = actor;
            context.durationSec = numArg > 0.0f ? static_cast<double>(numArg) : 20.0;
            (void)TFD::InCombat::HandleReleaseFollowEvent(
                context,
                TFD::InCombat::GraceEventHandlers{
                    [&](RE::Actor* graceActor, double seconds, const char* graceReason) {
                        if (g_outcomeRuntimeProviders.applyReleaseFollowGrace) {
                            g_outcomeRuntimeProviders.applyReleaseFollowGrace(graceActor, seconds, graceReason);
                        }
                    }
                });
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

        if (name == kBleedoutOutcomeReleaseEvent || name == kPleasureOutcomeReleaseEvent) {
            auto* actor = ResolveActorFromEventArg(arg);
            const double durationSec = numArg > 0.0f ? static_cast<double>(numArg) : 20.0;
            if (!actor) {
                spdlog::warn("[TFD][Flow] grace event={} ignored reason=invalid_actor arg={}", std::string(name), strArg ? strArg : "");
                return true;
            }
            const char* graceReason = name == kBleedoutOutcomeReleaseEvent ? "bleedout_release" : "pleasure_release";
            if (g_outcomeRuntimeProviders.applyReleaseFollowGrace) {
                g_outcomeRuntimeProviders.applyReleaseFollowGrace(actor, durationSec, graceReason);
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
            return true;
        }

        if (name == kInCombatOutcomePleasureEvent) {
            auto actorFormID = ResolveActorFormIDFromEventArg(arg);
            if (!actorFormID) {
                actorFormID = TFD::InCombat::GetPrimaryActorFormID();
            }
            auto* flowActor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
            TFD::InCombat::OutcomeEventContext context{};
            context.eventName = "mod_event_pleasure";
            context.rawEventName = eventName;
            context.actor = flowActor;
            context.actorFormID = actorFormID;
            context.inCombatState = TFD::InCombat::IsActive();
            context.preserveCaptive = TFD::Captive::IsStandardCaptiveActive();
            (void)TFD::InCombat::HandleOutcomePleasureEvent(context, inCombatOutcomeEventHandlers);
            return true;
        }

        if (name == kInCombatOutcomeCaptiveEvent) {
            auto actorFormID = ResolveActorFormIDFromEventArg(arg);
            if (!actorFormID) {
                actorFormID = TFD::InCombat::GetPrimaryActorFormID();
            }
            auto* flowActor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
            TFD::InCombat::OutcomeEventContext context{};
            context.eventName = "mod_event_captive";
            context.rawEventName = eventName;
            context.actor = flowActor;
            context.actorFormID = actorFormID;
            context.inCombatState = TFD::InCombat::IsActive();
            (void)TFD::InCombat::HandleOutcomeCaptiveEvent(context, inCombatOutcomeEventHandlers);
            return true;
        }

        if (name == kInCombatOutcomeResetEvent) {
            TFD::InCombat::OutcomeEventContext context{};
            context.eventName = "mod_event_reset";
            context.rawEventName = eventName;
            (void)TFD::InCombat::HandleOutcomeResetEvent(context, inCombatOutcomeEventHandlers);
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
            context.preserveCaptive = TFD::Captive::IsStandardCaptiveActive();
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
        if (!actor) {
            spdlog::warn("[TFD][PassiveBreak] event={} ignored reason=invalid_actor arg={}",
                std::string(name),
                strArg ? strArg : "");
            return true;
        }

        const char* reason = name == kPassiveBreakCrimeEvent ? "player_crime" : "player_pickpocket";
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
            } else if (senderFormID != 0) {
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

    bool Controller::ResolveBleedoutOutcome(BleedoutOutcome outcome, std::uint32_t actorFormID, std::string_view reason)
    {
        std::scoped_lock lk(_lock);
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
        return _snapshot.root == RootFlow::Bleedout && _snapshot.gate == DecisionGate::PlayerBleedout;
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
        return _snapshot.root == RootFlow::InCombat ||
            _snapshot.sub == SubFlow::InCombatPleasure ||
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
        return RequestCaptive(actorFormID, CaptiveMode::Kidnapped, reason.empty() ? std::string_view{"mod_event_captive"} : reason);
    }

    bool Controller::BeginCaptivePleasureFromModEvent(std::uint32_t actorFormID, std::string_view reason)
    {
        if (!RequestCaptive(actorFormID, CaptiveMode::Kidnapped, "mod_event_captive_pleasure_begin")) {
            spdlog::warn("[TFD][Flow] ignore mod_event_pleasure reason=begin_captive_reject actor={:08X}", actorFormID);
            return false;
        }
        if (!RequestResolveCaptiveOutcome(CaptiveOutcome::Pleasure, actorFormID, reason.empty() ? std::string_view{"mod_event_pleasure"} : reason)) {
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
