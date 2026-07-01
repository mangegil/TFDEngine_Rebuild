#include "TFDDefeatMonitor.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include "TFDActor.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>

#include "TFDSettings.h"
#include "TFDLocation.h"
#include "TFDCaptive.h"
#include "TFDDefeatCaptiveTickWiring.h"
#include "TFDRescue.h"
#include "TFDDefeatTransitionWiring.h"
#include "TFDHostilityController.h"

#include "TFDInCombat.h"
#include "TFDTame.h"
#include "TFDFlowController.h"
#include "TFDInteractionRouter.h"
#include "TFDTransition.h"
#include "TFDBleedout.h"
#include "TFDPleasureRuntime.h"
#include "TFDPlayerDamageGuard.h"
#include "TFDPlayerBleedImmunityGuard.h"
#include "TFDDialogueLifecycle.h"
#include "TFDPlayerThresholdOutcomeWiring.h"
#include "TFDBleedLockRuntime.h"
#include "TFDDefeatBleedRuntimeState.h"
#include "TFDDefeatBleedLockWiring.h"
#include "TFDDefeatBleedoutTickWiring.h"
#include "TFDDefeatMonitorBootstrapWiring.h"
#include "TFDDefeatEventBridge.h"
#include "TFDDefeatBleedRuntimeResetWiring.h"
#include "TFDDefeatSaveLoadBridge.h"
#include "TFDDefeatThresholdSensor.h"
#include "TFDDefeatActorQueries.h"
#include "TFDDefeatReleaseGrace.h"
#include "TFDDefeatFlowRefresh.h"
#include "TFDDefeatAggressorResolver.h"
#include "TFDDefeatBleedoutDialogueWiring.h"
#include "TFDTeammateManager.h"
#include "TFDCaptiveRecaptureRecoveryWiring.h"

namespace TFD::DefeatMonitor
{
	namespace
	{


		std::atomic_bool g_installed{ false };
		std::atomic_bool g_running{ false };
		std::atomic_flag g_tickPending = ATOMIC_FLAG_INIT;
		std::thread g_worker{};


			// P31G: bootstrap dependency construction moved to TFDDefeatMonitorBootstrapWiring.
			// DefeatMonitor keeps only tick/install/shutdown shell glue and public compatibility wrappers.

		static void TickUI()
		{
			struct Guard {
				~Guard() { g_tickPending.clear(std::memory_order_release); }
			} guard;
			if (!TFD::Settings::GetEnabled()) return;
			if (TFD::DefeatSaveLoadBridge::IsLoadTransitionActive()) return;
			RE::UI* ui = RE::UI::GetSingleton();
			TFD::Transition::PollResult();
			TFD::Transition::ProcessPendingFadeIn();
			if (TFD::Transition::IsAwaiting() || TFD::Transition::HasPendingFadeIn()) {
				TFD::Transition::MaintainCalmWindow(TFD::DefeatTransitionWiring::BuildTransitionRuntimeHandlers());
				TFD::DefeatFlowRefresh::UpdatePreCombatState();
				return;
			}
			TFD::DefeatFlowRefresh::RefreshPostDefeatGlobals();

			// P31E: threshold sensor owns enemy HP crossing. Victory still owns
			// registry, passive state, countdown, and auto-death.
			TFD::DefeatThresholdSensor::TickEnemy("pre_owner_gate");

			const bool inCombatDialogueOpen = TFD::DialogueLifecycle::IsDialogueOpen();
			if (!inCombatDialogueOpen &&
				!TFD::InteractionRouter::DialogueOpen::IsActive() &&
				TFD::Bleedout::TickQueuedAfterPleasureCrowdContinuation()) {
				return;
			}
			TFD::CaptiveRecaptureRecoveryWiring::TickPulse();
			const bool captiveBleedOverlay = TFD::Captive::HasEscapeBreakRebleedPending() || TFD::DefeatBleedRuntimeState::IsInBleedState();
			const auto dialogueSnapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
			auto dialogueTick = TFD::DialogueLifecycle::TickOpenEdges(TFD::DialogueLifecycle::TickInput{
				captiveBleedOverlay,
				TFD::InCombat::IsActive(),
				TFD::Rescue::IsActive(),
				dialogueSnapshot.sub == TFD::FlowController::SubFlow::BleedoutAfterPleasure,
				TFD::PleasureRuntime::IsBlocking()
			});
			if (dialogueTick.handledBleedAfterPleasurePause) {
				return;
			}

			if (!TFD::DefeatCaptiveTickWiring::TickRuntime(TFD::DefeatActorQueries::Player(), captiveBleedOverlay)) {
				return;
			}

			if (ui && ui->GameIsPaused()) return;
			RE::Actor* player = TFD::DefeatActorQueries::Player();
			if (!player) {
				TFD::DefeatFlowRefresh::RefreshPostDefeatGlobals();
				return;
			}
			TFD::FlowController::TickRuntime();
			TFD::Location::UpdateAmbientKidnapAvailability(false);
			if (TFD::Bleedout::DefeatGlue::HandlePendingEscapeBreak()) {
				return;
			}

			// R215A: Pay/Release and Left-For-Dead terminal outcomes arm the
			// post-defeat recovery cooldown before the next threshold scan.  The
			// old order scanned TickBleedLocks() first, so a still-low player HP
			// could immediately create a new Bleedout window before recovery had
			// a chance to own the state.
			if (TFD::Transition::IsLeftForDeadCooldownActive(TFD::DefeatTransitionWiring::BuildTransitionRuntimeHandlers())) {
				TFD::Transition::TickLeftForDeadCooldown(TFD::DefeatTransitionWiring::BuildTransitionRuntimeHandlers());
				return;
			}

			TFD::BleedLockRuntime::Tick(TFD::DefeatBleedLockWiring::BuildContext());
			TFD::DefeatFlowRefresh::UpdatePreCombatState();
			if (TFD::DefeatReleaseGrace::IsActive()) return;
			if (!captiveBleedOverlay && !TFD::DefeatBleedRuntimeState::IsInBleedState() && TFD::InCombat::IsActive()) {
				// R470A: DefeatMonitor must not own InCombat dialogue lifecycle.
				// InCombatGreet / FlowController own sticky reopen, close, complete,
				// cancel, and AfterPleasure handoff.  DefeatMonitor only observed the
				// DialogueMenu open edge above for compatibility with existing state.
			}


			if (TFD::DefeatBleedoutTickWiring::Tick(player)) {
				return;
			}
			if (TFD::DefeatThresholdSensor::HandlePlayer(player, "player_threshold")) {
				return;
			}

		}

		static void WorkerLoop()
		{
			while (g_running.load(std::memory_order_acquire)) {
				if (!g_tickPending.test_and_set(std::memory_order_acq_rel)) {
					auto* task = SKSE::GetTaskInterface();
					if (task) task->AddTask([]() { TickUI(); });
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
		}
	}

	// Defeat lifecycle / install / shutdown / queued progress / load state
	void Install()
	{
		if (g_installed.exchange(true, std::memory_order_acq_rel)) return;
		g_running.store(true, std::memory_order_release);
		TFD::DefeatSaveLoadBridge::ClearLoadTransition("install");
		TFD::Captive::ResetForLoad();
		TFD::Bleedout::DefeatGlue::ClearCaptiveOrchestrationResidue(false);
		TFD::PlayerBleedImmunityGuard::SetPlayerActive(false);
		TFD::PlayerDamageGuard::Reset("defeat_monitor_reset");

		auto bootstrapDependencies = TFD::DefeatMonitorBootstrapWiring::BuildDefaultDependencies();
		TFD::DefeatMonitorBootstrapWiring::InstallAllProviders(std::move(bootstrapDependencies));
		spdlog::info("[TFD][Defeat][R460A] player killmove guard lifecycle gated mode=combat_health_damage_or_hit_event");
		TFD::FlowController::Controller::GetSingleton().ResetRuntime("defeat_install");
		TFD::PleasureRuntime::Install();
		TFD::DefeatFlowRefresh::ResetRouterCombatContext();
		TFD::BleedLockRuntime::ClearAll(TFD::DefeatBleedLockWiring::BuildContext(), "install");
		TFD::Bleedout::ClearBridgeAliases(nullptr, "install");
		TFD::Actor::Ops::Initialize();
		TFD::Actor::Ops::InstallDefeatedEnemyQueryHooks(TFD::Actor::Ops::DefeatedEnemyQueryHooks{
			&TFD::DefeatActorQueries::IsTrackedDefeatedEnemy,
			&TFD::DefeatAggressorResolver::IsLastAggressor
			});
		TFD::Location::Initialize();
		TFD::DefeatEventBridge::Dependencies eventBridgeDependencies{};
		eventBridgeDependencies.getPlayerActor = []() -> RE::Actor* { return TFD::DefeatActorQueries::Player(); };
		eventBridgeDependencies.isObserverAlly = [](RE::Actor* actor) -> bool { return TFD::DefeatActorQueries::IsObserverAlly(actor); };
		TFD::DefeatEventBridge::Install(std::move(eventBridgeDependencies));
		g_worker = std::thread([]() { WorkerLoop(); });
		TFD::DefeatMonitor::ApplyQueuedProgressState();
		TFD::Rescue::SetStateValue(0);
		TFD::DefeatFlowRefresh::RefreshPostDefeatGlobals();
		spdlog::info("[TFD][Defeat] monitor installed");
	}

	void Shutdown()
	{
		if (!g_installed.exchange(false, std::memory_order_acq_rel)) return;
		g_running.store(false, std::memory_order_release);
		if (g_worker.joinable()) g_worker.join();
		TFD::CaptiveRecaptureRecoveryWiring::ShutdownProvider();
		TFD::DefeatMonitorBootstrapWiring::ShutdownPlayerDownAndOverkillProviders();
		TFD::PlayerThresholdOutcomeWiring::ShutdownProvider();
		TFD::DefeatCaptiveTickWiring::ShutdownProvider();
		TFD::DefeatFlowRefresh::ShutdownProvider();
		TFD::Bleedout::DefeatGlue::ClearCaptiveOrchestrationResidue(false);
		TFD::DefeatSaveLoadBridge::ClearQueuedProgressState("shutdown");
		TFD::PlayerBleedImmunityGuard::SetPlayerActive(false);
		TFD::DefeatBleedRuntimeResetWiring::Reset();
		TFD::FlowController::Controller::GetSingleton().ResetRuntime("defeat_shutdown");
		TFD::FlowController::ResetDefeatLifecycleProviders();
		TFD::TeammateManager::ResetRuntimeProviders();
		TFD::Tame::ResetRuntimeProviders();
		TFD::HostilityController::ResetBleedTruceRuntimeProviders();
		TFD::DefeatFlowRefresh::ResetRouterCombatContext();
		TFD::BleedLockRuntime::ClearAll(TFD::DefeatBleedLockWiring::BuildContext(), "shutdown");
		TFD::DefeatBleedLockWiring::ShutdownProvider();
		TFD::DefeatAggressorResolver::ShutdownProvider();
		TFD::Bleedout::ClearBridgeAliases(nullptr, "shutdown");
		TFD::DefeatSaveLoadBridge::ClearLoadTransition("shutdown");
		TFD::Transition::ClearLeftForDeadCooldown(TFD::DefeatTransitionWiring::BuildTransitionRuntimeHandlers());
		TFD::DefeatEventBridge::Shutdown();
		TFD::PleasureRuntime::Shutdown();
		TFD::Rescue::SetStateValue(0);
		TFD::DefeatFlowRefresh::RefreshPostDefeatGlobals();
		TFD::Bleedout::ResetDefeatLifecycleProviders();
		TFD::DefeatMonitorBootstrapWiring::ShutdownLifecycleProviders();
		TFD::DefeatBleedoutDialogueWiring::ShutdownProvider();
		TFD::Transition::DefeatGlue::Reset();
		TFD::DefeatReleaseGrace::ShutdownProvider();
		TFD::DefeatTransitionWiring::ShutdownProvider();
		spdlog::info("[TFD][Defeat] monitor shutdown");
	}

	void ResetGrace()
	{
		TFD::DefeatReleaseGrace::Reset("reset_grace");
	}

	bool HandlePassiveInvalidationAgainstActor(RE::Actor* actor, const char* reason)
	{
		return TFD::FlowController::HandlePassiveInvalidationAgainstActor(actor, reason, false);
	}

	bool GetCaptiveStateForSave()
	{
		return TFD::DefeatSaveLoadBridge::GetCaptiveStateForSave();
	}

	std::uint32_t GetCaptivePhaseForSave()
	{
		return TFD::DefeatSaveLoadBridge::GetCaptivePhaseForSave();
	}

	bool GetBleedOutStateForSave()
	{
		return TFD::DefeatSaveLoadBridge::GetBleedOutStateForSave();
	}

	void QueueLoadedBleedOutState(bool active)
	{
		TFD::DefeatSaveLoadBridge::QueueLoadedBleedOutState(active);
	}

	void QueueLoadedProgressState(bool stateActive, std::uint32_t phaseRaw)
	{
		TFD::DefeatSaveLoadBridge::QueueLoadedProgressState(stateActive, phaseRaw);
	}

	void QueueDefaultProgressState()
	{
		TFD::DefeatSaveLoadBridge::QueueDefaultProgressState();
	}

	bool HasQueuedProgressState()
	{
		return TFD::DefeatSaveLoadBridge::HasQueuedProgressState();
	}

	void ApplyQueuedProgressState()
	{
		TFD::DefeatSaveLoadBridge::ApplyQueuedProgressState();
	}

	void ResetForLoad()
	{
		TFD::DefeatSaveLoadBridge::ResetForLoad();
	}

	void SetLoadTransition(bool active)
	{
		TFD::DefeatSaveLoadBridge::SetLoadTransition(active);
	}

	bool IsThresholdDownedActor(RE::Actor* actor)
	{
		return TFD::DefeatThresholdSensor::IsDownedActor(actor);
	}

	bool IsThresholdCombatTargetValid(RE::Actor* actor)
	{
		return TFD::DefeatThresholdSensor::IsCombatTargetValid(actor);
	}



}
