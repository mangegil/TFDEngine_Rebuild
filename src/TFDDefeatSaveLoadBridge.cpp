#include "TFDDefeatSaveLoadBridge.h"

#include <algorithm>
#include <atomic>
#include <cstdint>

#include <RE/Skyrim.h>
#include <RE/A/ActorValues.h>
#include <spdlog/spdlog.h>

#include "TFDSettings.h"
#include "TFDCaptive.h"
#include "TFDRescue.h"
#include "TFDBleedout.h"
#include "TFDDialogueLifecycle.h"
#include "TFDPlayerBleedImmunityGuard.h"
#include "TFDPlayerDamageGuard.h"
#include "TFDBleedLockRuntime.h"
#include "TFDDefeatBleedLockWiring.h"
#include "TFDDefeatBleedRuntimeResetWiring.h"
#include "TFDDefeatReleaseGrace.h"
#include "TFDDefeatAggressorResolver.h"
#include "TFDCaptiveRecaptureRecoveryWiring.h"
#include "TFDHostilityController.h"
#include "TFDTransition.h"
#include "TFDDefeatTransitionWiring.h"
#include "TFDDefeatFlowRefresh.h"
#include "TFDPleasureRuntime.h"

namespace TFD::DefeatSaveLoadBridge
{
	namespace
	{
		std::atomic_bool g_loadTransition{ false };
		bool g_hasQueuedProgressState = false;
		bool g_queuedBleedOutState = false;

		static RE::Actor* PlayerActor()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

		static bool ComputePlayerBleedOutState(RE::Actor* player)
		{
			if (!player || player->IsDead() || player->IsDisabled()) {
				return false;
			}

			const float hpNow = player->GetActorValue(RE::ActorValue::kHealth);
			const float hpMax = (std::max)(1.0f, player->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float pct = (hpNow / hpMax) * 100.0f;
			const float threshold = TFD::Settings::GetDefeatThresholdPct();
			return pct <= threshold;
		}

		static void SetRescueStateValue(int value)
		{
			TFD::Rescue::SetStateValue(value);
		}
	}

	bool IsLoadTransitionActive()
	{
		return g_loadTransition.load(std::memory_order_acquire);
	}

	void ClearLoadTransition(const char* reason)
	{
		g_loadTransition.store(false, std::memory_order_release);
		if (reason && reason[0] != '\0') {
			spdlog::debug("[TFD][DefeatSaveLoadBridge][P31D] ClearLoadTransition reason={}", reason);
		}
	}

	void ClearQueuedProgressState(const char* reason)
	{
		g_hasQueuedProgressState = false;
		TFD::Captive::ClearQueuedLoadedState();
		g_queuedBleedOutState = false;
		if (reason && reason[0] != '\0') {
			spdlog::debug("[TFD][DefeatSaveLoadBridge][P31D] ClearQueuedProgressState reason={}", reason);
		}
	}

	bool GetCaptiveStateForSave()
	{
		return TFD::Captive::GetStateFlag();
	}

	std::uint32_t GetCaptivePhaseForSave()
	{
		return TFD::Captive::GetPhaseRaw();
	}

	bool GetBleedOutStateForSave()
	{
		if (auto* player = PlayerActor()) {
			return ComputePlayerBleedOutState(player);
		}
		return g_queuedBleedOutState;
	}

	void QueueLoadedBleedOutState(bool active)
	{
		g_queuedBleedOutState = active;
		spdlog::info("[TFD][DefeatSaveLoadBridge] QueueLoadedBleedOutState state={}", active ? 1 : 0);
	}

	void QueueLoadedProgressState(bool stateActive, std::uint32_t phaseRaw)
	{
		g_hasQueuedProgressState = true;
		auto phase = stateActive ? TFD::Captive::PhaseFromRaw(phaseRaw) : TFD::Captive::PhaseValue::None;
		if (stateActive && phase == TFD::Captive::PhaseValue::None) {
			phase = TFD::Captive::PhaseValue::Escape;
		}
		TFD::Captive::QueueLoadedState(stateActive, phase);
		spdlog::info(
			"[TFD][DefeatSaveLoadBridge] QueueLoadedProgressState state={} phase={} normalized={}",
			stateActive ? 1 : 0,
			phaseRaw,
			static_cast<int>(TFD::Captive::GetQueuedPhase()));
	}

	void QueueDefaultProgressState()
	{
		g_hasQueuedProgressState = true;
		TFD::Captive::ClearQueuedLoadedState();
		g_queuedBleedOutState = false;
		spdlog::info("[TFD][DefeatSaveLoadBridge] QueueDefaultProgressState");
	}

	bool HasQueuedProgressState()
	{
		return g_hasQueuedProgressState;
	}

	void ApplyQueuedProgressState()
	{
		if (!g_hasQueuedProgressState) {
			QueueDefaultProgressState();
		}

		TFD::Transition::ClearLeftForDeadCooldown(TFD::DefeatTransitionWiring::BuildTransitionRuntimeHandlers());
		TFD::Captive::ApplyQueuedDefeatProgressState(TFD::Captive::ApplyQueuedDefeatProgressHandlers{
			[]() -> RE::Actor* { return PlayerActor(); },
			[]() { return TFD::DialogueLifecycle::IsDialogueOpen(); },
			[](bool open) { TFD::DialogueLifecycle::SetDialogueOpenObserved(open); }
			});
		TFD::PlayerBleedImmunityGuard::SetPlayerActive(false);
		TFD::PlayerDamageGuard::Reset("apply_queued_state");
		TFD::Bleedout::ClearBridgeAliases(nullptr, "apply_queued_state");
		SetRescueStateValue(0);
		TFD::DefeatFlowRefresh::RefreshPostDefeatGlobals();
		TFD::DefeatFlowRefresh::UpdatePreCombatState();
		spdlog::info(
			"[TFD][DefeatSaveLoadBridge] ApplyQueuedProgressState state={} phase={} bleed={}",
			TFD::Captive::GetQueuedStateFlag() ? 1 : 0,
			static_cast<int>(TFD::Captive::GetQueuedPhase()),
			g_queuedBleedOutState ? 1 : 0);
	}

	void ResetForLoad()
	{
		TFD::DefeatReleaseGrace::Clear("reset_for_load");
		TFD::CaptiveRecaptureRecoveryWiring::ResetPulse();
		TFD::PlayerBleedImmunityGuard::SetPlayerActive(false);
		TFD::DefeatBleedRuntimeResetWiring::Reset();
		TFD::BleedLockRuntime::ClearAll(TFD::DefeatBleedLockWiring::BuildContext(), "reset_for_load");
		TFD::Bleedout::ClearBridgeAliases(nullptr, "reset_for_load");
		TFD::DefeatAggressorResolver::ResetForLoad();
		TFD::Bleedout::DefeatGlue::ClearCaptiveOrchestrationResidue(false);
		TFD::HostilityController::ClearAggressionClamp();
		TFD::Transition::ClearLeftForDeadCooldown(TFD::DefeatTransitionWiring::BuildTransitionRuntimeHandlers());
		SetRescueStateValue(0);
		TFD::DefeatFlowRefresh::ResetRouterCombatContext();
		TFD::PleasureRuntime::ResetForLoad("defeat_reset_for_load");
		TFD::DefeatFlowRefresh::RefreshPostDefeatGlobals();
		spdlog::info("[TFD][DefeatSaveLoadBridge] ResetForLoad -> runtime only");
	}

	void SetLoadTransition(bool active)
	{
		g_loadTransition.store(active, std::memory_order_release);
		if (active) {
			TFD::PlayerBleedImmunityGuard::SetPlayerActive(false);
			TFD::BleedLockRuntime::ClearAll(TFD::DefeatBleedLockWiring::BuildContext(), "set_load_transition");
			TFD::Bleedout::ClearBridgeAliases(nullptr, "set_load_transition");
			TFD::Captive::ResetLockpickWatch();
			TFD::PleasureRuntime::ResetForLoad("defeat_set_load_transition");
			spdlog::info("[TFD][DefeatSaveLoadBridge] SetLoadTransition(true)");
		}
		else {
			spdlog::info("[TFD][DefeatSaveLoadBridge] SetLoadTransition(false)");
		}
	}
}
