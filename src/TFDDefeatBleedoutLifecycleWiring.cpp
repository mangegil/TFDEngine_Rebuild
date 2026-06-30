#include "TFDDefeatBleedoutLifecycleWiring.h"

#include <algorithm>
#include <mutex>
#include <utility>

#include <spdlog/spdlog.h>

#include "TFDActor.h"
#include "TFDBleedLockRuntime.h"
#include "TFDCaptive.h"
#include "TFDCaptiveRecaptureRecoveryWiring.h"
#include "TFDDefeatAggressorResolver.h"
#include "TFDDefeatBattleObserveState.h"
#include "TFDDefeatBleedLockWiring.h"
#include "TFDDefeatBleedoutRuntimeTimer.h"
#include "TFDDialogueLifecycle.h"
#include "TFDBleedoutGreet.h"
#include "TFDFlowController.h"
#include "TFDDefeatBridge.h"
#include "TFDDefeatFlowRefresh.h"
#include "TFDDefeatRuntimeActions.h"
#include "TFDHostilityController.h"
#include "TFDSettings.h"
#include "TFDTame.h"
#include "TFDTransition.h"

namespace TFD::DefeatBleedoutLifecycleWiring
{
	namespace
	{
		using BleedTerminalCommit = TFD::Bleedout::TerminalCommit;
		using CaptivePhaseValue = TFD::Captive::PhaseValue;

		std::mutex g_providerLock;
		Dependencies g_dependencies{};
		bool g_hasProvider = false;

		Dependencies ResolveDependencies()
		{
			std::scoped_lock lock(g_providerLock);
			return g_dependencies;
		}

		RE::Actor* PlayerFrom(const Dependencies& deps)
		{
			return deps.getPlayer ? deps.getPlayer() : RE::PlayerCharacter::GetSingleton();
		}

		TFD::Transition::RuntimeHandlers BuildTransitionHandlersFrom(const Dependencies& deps)
		{
			return deps.buildTransitionRuntimeHandlers ? deps.buildTransitionRuntimeHandlers() : TFD::Transition::RuntimeHandlers{};
		}

		TFD::BleedoutDialogueRuntime::Context BuildDialogueRuntimeContextFrom(const Dependencies& deps)
		{
			return deps.buildBleedoutDialogueRuntimeContext ? deps.buildBleedoutDialogueRuntimeContext() : TFD::BleedoutDialogueRuntime::Context{};
		}

		void SetGraceSecondsFrom(const Dependencies& deps, int seconds)
		{
			if (deps.setGraceSeconds) {
				deps.setGraceSeconds(seconds);
			}
		}

		void ResetBleedRuntimeStateFrom(const Dependencies& deps, bool preserveCaptive = false)
		{
			if (deps.resetBleedRuntimeState) {
				deps.resetBleedRuntimeState(preserveCaptive);
			}
		}
	}

	void InstallProvider(Dependencies dependencies)
	{
		std::scoped_lock lock(g_providerLock);
		g_dependencies = std::move(dependencies);
		g_hasProvider = true;
		spdlog::info("[TFD][BleedoutLifecycle][P27C] provider installed");
	}

	void ShutdownProvider()
	{
		std::scoped_lock lock(g_providerLock);
		g_dependencies = Dependencies{};
		g_hasProvider = false;
		spdlog::info("[TFD][BleedoutLifecycle][P27C] provider cleared");
	}

	bool HasProvider()
	{
		std::scoped_lock lock(g_providerLock);
		return g_hasProvider;
	}

	TFD::Bleedout::DefeatLifecycleProviders BuildProviders()
	{
		const auto deps = ResolveDependencies();
		if (!HasProvider()) {
			spdlog::warn("[TFD][BleedoutLifecycle][P27C] BuildProviders called before provider install");
		}

		return TFD::Bleedout::DefeatLifecycleProviders{
		TFD::Bleedout::Builders::PendingSystemEventProvider{
			[](const char* r) { TFD::Bleedout::ClearDialogueOutcome(r); },
			[](const char* r) { TFD::Bleedout::ClearSystemEventOutcomeWindow(r); },
			[](const char* r) { (void)TFD::DefeatRuntimeActions::CompletePayRelease(r); },
			[]() { TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::FlowHandoff); },
			[deps]() -> bool { return TFD::Transition::ResolveCaptiveMarkerForOutcome(BuildTransitionHandlersFrom(deps)); },
			[]() {
				const bool teleported = TFD::Bleedout::DoBlackoutTeleport("blackout_teleport", TFD::Bleedout::Builders::BuildBlackoutHandlers());
				if (teleported) {
					TFD::CaptiveRecaptureRecoveryWiring::ArmPulse("pending_system_event_blackout");
				}
			},
			[deps](int seconds) { SetGraceSecondsFrom(deps, seconds); },
			[](const char* r) { TFD::Tame::ReleaseBleedNoSpeakerTameSession(r); },
			[]() { TFD::Bleedout::DefeatGlue::ExitSystemEventRuntime(); },
			[](const char* r) { (void)TFD::Bleedout::EnterNonCaptiveChoice(r, TFD::Bleedout::Builders::BuildNonCaptiveChoiceHandlers()); }
		},
		TFD::Bleedout::Builders::DialogueCloseProvider{
			[](const char* r) { TFD::Bleedout::ClearDialogueOutcome(r); },
			[](const char* r) { (void)TFD::DefeatRuntimeActions::CompletePayRelease(r); }
		},
		TFD::Bleedout::Builders::TimeoutProvider{
			[]() { TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::Generic); },
			[deps]() -> bool { return TFD::Transition::ResolveCaptiveMarkerForOutcome(BuildTransitionHandlersFrom(deps)); },
			[deps]() { ResetBleedRuntimeStateFrom(deps); },
			[]() {
				const bool teleported = TFD::Bleedout::DoBlackoutTeleport("blackout_teleport", TFD::Bleedout::Builders::BuildBlackoutHandlers());
				if (teleported) {
					TFD::CaptiveRecaptureRecoveryWiring::ArmPulse("timeout_blackout");
				}
			},
			[deps](int seconds) { SetGraceSecondsFrom(deps, seconds); },
			[](const char* r) { (void)TFD::Bleedout::EnterNonCaptiveChoice(r, TFD::Bleedout::Builders::BuildNonCaptiveChoiceHandlers()); }
		},
		TFD::Bleedout::Builders::BaseCompletionProvider{
			[](const char* r) { TFD::Bleedout::ClearSystemEventOutcomeWindow(r); },
			[](BleedTerminalCommit kind, const char* r) { return TFD::Bleedout::TryBeginTerminalCommit(kind, r); },
			[]() { TFD::Transition::ClearPendingFadeIn(); },
			[](const char* r) { TFD::Bleedout::ClearBridgeAliases(nullptr, r); },
			[]() { TFD::Captive::ClearEscapeContext(); },
			[]() { TFD::Captive::ResetLockpickWatch(); },
			[deps](bool active) { if (deps.graceActive) { deps.graceActive->store(active, std::memory_order_release); } },
			[](bool captive) { TFD::Captive::SetRuntimeState(captive, captive ? CaptivePhaseValue::Captive : CaptivePhaseValue::None); },
			[deps](bool immune) { if (deps.setPlayerBleedImmune) { deps.setPlayerBleedImmune(immune); } },
			[deps]() { TFD::Transition::RecoverPlayerForTransition(BuildTransitionHandlersFrom(deps)); },
			[]() { return TFD::Settings::GetSweepRadius(); },
			[deps](float radius) { TFD::DefeatRuntimeActions::ApplyTerminalCalmBubble(PlayerFrom(deps), TFD::DefeatAggressorResolver::ResolveAggressor(), radius, TFD::Settings::GetSweepRadius(), "defeat_monitor_legacy_calm_callback"); },
			[]() { TFD::DefeatFlowRefresh::UpdatePreCombatState(); },
			[]() -> RE::Actor* { return TFD::Bleedout::DefeatGlue::ResolveBleedRuntimeSpeaker(); },
			[](RE::Actor* speaker, bool captive, const char* why) { TFD::Bleedout::DefeatGlue::BeginBleedPleasureRuntime(speaker, captive, why); },
			[](bool v) { TFD::DialogueLifecycle::SetDialogueOpenObserved(v); },
			[](bool v) { TFD::Captive::SetPrevLockpickOpen(v); }
		},
		TFD::Bleedout::Builders::CaptivePleasureCompletionExtras{
			[]() { TFD::DefeatAggressorResolver::ClearLastAggressor(); },
			[deps](bool preserve) { ResetBleedRuntimeStateFrom(deps, preserve); },
			[deps](const char* r) { TFD::Captive::SyncPlayerAlias(PlayerFrom(deps), r); }
		},
		TFD::Bleedout::Builders::PayReleaseCompletionExtras{
			[]() { TFD::Actor::Ops::ClearAggressorFactionContext(); },
			[]() { TFD::DefeatAggressorResolver::ClearLastAggressor(); },
			[deps](bool preserve) { ResetBleedRuntimeStateFrom(deps, preserve); },
			[](int secs) { TFD::Transition::BeginLeftForDeadCooldown(secs); },
			[deps](int secs) { SetGraceSecondsFrom(deps, secs); }
		},
		TFD::Bleedout::Builders::BleedPleasureCompletionExtras{
			[deps](const char* r) { if (deps.transitionBleedRuntimeToPleasureCommit) { deps.transitionBleedRuntimeToPleasureCommit(r); } TFD::DefeatFlowRefresh::ResetRouterCombatContext(); },
			[](int secs) { TFD::Transition::BeginLeftForDeadCooldown(secs); },
			[deps](int secs) { SetGraceSecondsFrom(deps, secs); },
			[]() { TFD::DefeatFlowRefresh::RefreshPostDefeatGlobals(); }
		},
		TFD::Bleedout::RuntimeHost::Provider{
			deps.inBleedState,
			deps.minHp,
			&TFD::DefeatBleedoutRuntimeTimer::BleedStart(),
			&TFD::DefeatBleedoutRuntimeTimer::BleedLastSeconds(),
			&TFD::DefeatBleedoutRuntimeTimer::BleedPaused(),
			&TFD::DefeatBleedoutRuntimeTimer::BleedPauseStarted(),
			&TFD::DefeatBleedoutRuntimeTimer::BleedLastCalmPulse(),
			[]() { return TFD::Captive::HasEscapeBreakRebleedPending(); },
			[](bool pending) { TFD::Captive::SetEscapeBreakRebleedPending(pending); },
			deps.bleedPendingCaptiveOutcome,
			deps.bleedPendingNonCaptiveOutcome,
			&TFD::DefeatBattleObserveState::Pending(),
			&TFD::DefeatBattleObserveState::PendingUntil(),
			&TFD::DefeatBattleObserveState::PendingLastRedirect(),
			&TFD::DefeatBattleObserveState::PendingEmptyEnemyTicks(),
			&TFD::DefeatBattleObserveState::PendingEmptyAllyTicks(),
			&TFD::DefeatBattleObserveState::Active(),
			&TFD::DefeatBattleObserveState::ActiveSince(),
			&TFD::DefeatBattleObserveState::ActiveLastRedirect(),
			&TFD::DefeatBattleObserveState::ActiveEmptyEnemyTicks(),
			&TFD::DefeatBattleObserveState::ActiveEmptyAllyTicks(),
			[]() { return TFD::Bleedout::DefeatGlue::BuildLocalRuntimeHostHandlers(); },
			[]() { return TFD::Bleedout::DefeatGlue::BuildDialogueHotkeyHandlers(); },
			[deps]() -> RE::Actor* { return PlayerFrom(deps); },
			[]() { return (std::max)(2400.0f, TFD::Settings::GetSweepRadius()); },
			[]() { return 1800.0f; }
		},
		TFD::Bleedout::DefeatGlue::Provider{
			deps.graceActive,
			deps.graceUntil,
			deps.previousDialogueOpen,
			[]() { TFD::Transition::ClearPendingFadeIn(); },
			[]() { TFD::Captive::ClearEscapeContext(); },
			[]() { TFD::Captive::ResetLockpickWatch(); },
			[](bool open) { TFD::Captive::SetPrevLockpickOpen(open); },
			[]() { TFD::Captive::SetRuntimeState(false, CaptivePhaseValue::None); },
			[deps]() -> RE::Actor* { return PlayerFrom(deps); },
			[](const char* reason, bool playGetUp) { TFD::BleedLockRuntime::ReleasePlayer(TFD::DefeatBleedLockWiring::BuildContext(), reason, playGetUp); },
			[]() { return TFD::Bleedout::GetBleedSpeakerID(); },
			[]() -> RE::Actor* { return TFD::Bleedout::GetBleedSpeakerActor(); },
			[deps]() { return deps.buildBleedoutSpeakerHandlers ? deps.buildBleedoutSpeakerHandlers() : TFD::Bleedout::SpeakerLogicHandlers{}; },
			[]() { return TFD::DialogueLifecycle::IsDialogueOpen(); },
			[]() -> RE::Actor* { return TFD::DefeatAggressorResolver::ResolveAggressor(); },
			[](float radius) -> RE::Actor* { return TFD::DefeatAggressorResolver::FindBestAggressor(radius); },
			[](const char* reason) { TFD::Tame::ReleaseBleedNoSpeakerTameSession(reason); },
			[]() { TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::Generic); },
			[deps](RE::Actor* player, RE::Actor* speaker, const char* reason) { return TFD::HostilityController::StartBleedTruceSessionForSpeaker(player ? player : PlayerFrom(deps), speaker, reason); },
			[]() { TFD::Bleedout::ResetBleedSpeakerKick(); },
			[](const char* reason) { TFD::BleedoutGreet::ResetRuntime(reason); },
			[](RE::Actor* actor, const char* reason) { (void)TFD::BleedoutGreet::Begin(actor, reason); },
			[](const char* reason) { TFD::DefeatBridge::ClearBleedSupportAliases(reason); },
			[]() { TFD::DefeatBattleObserveState::ResetTracking(); },
			[]() { return TFD::DefeatBattleObserveState::IsObservedCombatCommitInProgress(); },
			[](RE::Actor* actor) { TFD::DefeatAggressorResolver::NoteEnemyTargetingPlayer(actor); },
			[deps](bool immune) { if (deps.setPlayerBleedImmune) { deps.setPlayerBleedImmune(immune); } },
			[deps](RE::Actor* actor, float minHp) { if (deps.clampHealth) { deps.clampHealth(actor, minHp); } },
			[deps](float radius) { return deps.collectBleedStandingFollowers ? deps.collectBleedStandingFollowers(radius) : std::vector<RE::Actor*>{}; },
			[](RE::Actor* player, const std::vector<RE::Actor*>& allies, float baseRadius) { return TFD::Bleedout::DefeatGlue::ComputeObservedEnemyScanRadius(player, allies, baseRadius); },
			[](float radius, double maxAgeSec) -> RE::Actor* { return TFD::DefeatAggressorResolver::ResolveLastEnemyTargetingPlayer(radius, maxAgeSec); },
			[deps](RE::Actor* actor) { return deps.isObserverAlly ? deps.isObserverAlly(actor) : false; },
			[](RE::Actor* player, float radius, RE::Actor* preferred, const std::vector<RE::Actor*>& allies) { return TFD::Bleedout::DefeatGlue::CollectCurrentObservedEnemies(player, radius, preferred, allies); },
			[](RE::Actor* player, const std::vector<RE::Actor*>& followers, const std::vector<RE::Actor*>& enemies, RE::Actor* preferred) { TFD::Bleedout::DefeatGlue::UpdateObservedBattleRoster(player, followers, enemies, preferred); },
			[]() { return TFD::Bleedout::DefeatGlue::CollectStandingFollowersFromSnapshot(); },
			[]() { return TFD::Bleedout::DefeatGlue::CollectStandingEnemiesFromSnapshot(); },
			[]() { return TFD::Bleedout::DefeatGlue::HadValidObservedEnemy(); },
			[deps]() { return deps.resolveBleedFlowActorFormID ? deps.resolveBleedFlowActorFormID() : 0u; },
			[](const TFD::FlowController::ObservedDefeatInput& input, const char* reason) {
				return TFD::FlowController::ApplyObservedDefeatResolution(input, reason ? reason : "battle_observe_resolution");
			},
			[]() { TFD::Bleedout::DefeatGlue::HandleObservedBattleWin(); },
			[](const char* reason) { TFD::Bleedout::DefeatGlue::HandleObservedLeftForDead(reason); },
			[deps](float radius, float maxDist, RE::Actor* preferred) { return TFD::BleedoutDialogueRuntime::FindBestSpeaker(radius, maxDist, preferred, BuildDialogueRuntimeContextFrom(deps)); },
			[deps](RE::Actor* actor, RE::Actor* player, float maxDist, float* outDistance) { return TFD::BleedoutDialogueRuntime::IsReasonableSpeaker(actor, player, maxDist, outDistance, BuildDialogueRuntimeContextFrom(deps)); },
			[deps](float radius, RE::Actor* preferred, bool preserveAssigned) { return deps.collectBleedoutCrowd ? deps.collectBleedoutCrowd(radius, preferred, preserveAssigned) : std::vector<RE::Actor*>{}; },
			[](RE::Actor* actor) { return TFD::DefeatAggressorResolver::IsCaptiveSupportedAggressor(actor); },
			[](RE::Actor* actor) { return TFD::Actor::Ops::ApplyAggressorFactionContext(actor); },
			[deps]() { return TFD::Transition::ResolveCaptiveMarkerForOutcome(BuildTransitionHandlersFrom(deps)); },
			[](RE::Actor* player, RE::Actor* aggressor, bool hasCaptiveOutcome, float* outDistance) { const float maxDist = hasCaptiveOutcome ? 1200.0f : 900.0f; return player && aggressor && TFD::DefeatAggressorResolver::IsCaptiveSupportedAggressor(aggressor) && TFD::DefeatAggressorResolver::IsReasonableCombatAggressor(aggressor, player, maxDist, outDistance); },
			[deps](const std::vector<RE::Actor*>& actors, const char* reason) { auto* player = PlayerFrom(deps); return player && TFD::Bleedout::TryEnsureNoSpeakerTameSession(actors, player, reason, TFD::Bleedout::RuntimeHost::BuildHandlers()); },
			[](RE::Actor* actor) { TFD::DefeatAggressorResolver::SetLastAggressor(actor); },
			[](RE::Actor* actor) { return TFD::DefeatAggressorResolver::IsBleedCrowdSupportedAggressor(actor); },
			[deps](RE::Actor* actor, RE::Actor* player) { return deps.isBleedSpaceCompatible ? deps.isBleedSpaceCompatible(actor, player) : true; },
			[](const char* msg) { if (msg && msg[0]) RE::DebugNotification(msg); },
			[](RE::Actor*, float, const char*) { /* R19: CombatBehavior owns player-target detach/retarget. */ },
			[](float radius) -> RE::Actor* { return TFD::Captive::ResolveEscapeBreakPreferredAggressor(radius, [](float fallbackRadius) { return TFD::DefeatAggressorResolver::FindBestAggressor(fallbackRadius); }); },
			[deps](RE::Actor* player, RE::Actor* aggressor, float* outDistance) { if (!player || !aggressor) { if (outDistance) *outDistance = -1.0f; return false; } float dist = -1.0f; const bool ok = TFD::BleedoutDialogueRuntime::CanUseAggressorForGreet(player, aggressor, dist, BuildDialogueRuntimeContextFrom(deps)); if (outDistance) *outDistance = dist; return ok; },
			[deps](RE::Actor* player, RE::Actor* speaker, const char* reason, bool restartDialogue) { TFD::BleedoutDialogueRuntime::ApplyOverdrive(player, speaker, reason, restartDialogue, BuildDialogueRuntimeContextFrom(deps)); },
			[deps](RE::Actor* actor) { return deps.isStandingAllyThresholdActor ? deps.isStandingAllyThresholdActor(actor) : false; },
			[deps](RE::Actor* actor) { return deps.isStandingEnemyThresholdActor ? deps.isStandingEnemyThresholdActor(actor) : false; }
		}
		};
	}
}
