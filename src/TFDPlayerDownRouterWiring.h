#pragma once

#include <functional>

#include <RE/Skyrim.h>

#include "TFDPlayerDownRouter.h"
#include "TFDFlowController.h"

namespace TFD::PlayerDownRouterWiring
{
	struct DispatchContext
	{
		std::function<void(RE::Actor*)> rememberAggressor;
		std::function<RE::Actor*(float, float, RE::Actor*)> findBleedoutSpeaker;
		std::function<void(RE::Actor*, RE::Actor*)> startBleedWindow;
		std::function<bool(RE::Actor*)> startBattleObservePending;
		std::function<void(int)> setGraceSeconds;
	};

	struct NoThreatContext
	{
		std::function<bool()> hasTerminalCommit;
		std::function<bool()> hasCachedRescueDestination;
		std::function<bool()> hasPlayerBleedLock;
		std::function<void(RE::Actor*, float, const char*)> enterPlayerBleedLock;
		std::function<void(const char*, bool)> releasePlayerBleedLock;
		std::function<void(bool)> setPlayerBleedImmune;
		std::function<void(RE::Actor*)> preparePlayer;
		std::function<bool(const char*)> executeNoMarkerFallback;
	};

	struct PendingContext
	{
		std::function<void(bool)> setPlayerBleedImmune;
		std::function<void(RE::Actor*)> preparePlayer;
		std::function<void(RE::Actor*, float)> clampHealth;
		std::function<float(RE::Actor*, float)> resolveSafeFloorHealth;
		std::function<bool()> hasPlayerBleedOwner;
		std::function<void(RE::Actor*, float, const char*)> enterPlayerBleedLock;
		std::function<bool()> isBleedDecisionActive;
		std::function<TFD::PlayerDownRouter::ThresholdScan(RE::Actor*)> scanThresholdOutcome;
		std::function<bool(RE::Actor*)> isObserverAlly;
		std::function<void(RE::Actor*)> rememberAggressor;
		std::function<float(RE::Actor*)> getHealthPct;
		DispatchContext dispatchContext;
	};

	TFD::PlayerDownRouter::DispatchHandlers BuildDispatchHandlers(const DispatchContext& context);
	TFD::PlayerDownRouter::NoThreatFallbackHandlers BuildNoThreatHandlers(const NoThreatContext& context);
	TFD::PlayerDownRouter::PendingRouteHandlers BuildPendingHandlers(const PendingContext& context);
}
