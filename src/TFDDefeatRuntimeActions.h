#pragma once

#include <functional>

#include <RE/Skyrim.h>

#include "TFDBleedout.h"
#include "TFDFlowController.h"
#include "TFDTransition.h"

namespace TFD::DefeatRuntimeActions
{
	TFD::Bleedout::RuntimeHostStateRefs BuildBleedRuntimeHostStateRefs();
	bool CompletePayRelease(const char* reason);
	void ApplyTerminalCalmBubble(RE::Actor* player, RE::Actor* primary, float radius, float configuredSweepRadius, const char* reason);

	struct NoMarkerFallbackContext
	{
		std::function<TFD::Transition::RuntimeHandlers()> buildTransitionRuntimeHandlers;
		std::function<void()> clearCaptiveOrchestrationResidue;
		std::function<RE::Actor*()> getPlayer;
		std::function<void(const char*)> clearBridgeAliases;
		std::function<void(bool)> setPlayerBleedImmune;
		std::function<void()> resetBleedRuntimeState;
		std::function<void()> clearLastAggressor;
		std::function<void()> updatePreCombatState;
	};

	TFD::FlowController::NonCaptiveFallbackExecutionHandlers BuildNoMarkerFallbackExecutionHandlers(const NoMarkerFallbackContext& context);
	bool ExecuteResolvedNoMarkerFallback(const char* reason, const NoMarkerFallbackContext& context);
}
