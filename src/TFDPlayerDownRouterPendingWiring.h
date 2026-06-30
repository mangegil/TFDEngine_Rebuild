#pragma once

#include <functional>

#include <RE/Skyrim.h>

#include "TFDPlayerDownRouter.h"
#include "TFDPlayerDownRouterWiring.h"

namespace TFD::PlayerDownRouterPendingWiring
{
	struct Dependencies
	{
		std::function<RE::Actor*()> getPlayer;
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
		std::function<TFD::PlayerDownRouterWiring::DispatchContext()> buildDispatchContext;
	};

	void InstallProvider(Dependencies dependencies);
	void ShutdownProvider();
	bool HasProvider();
	TFD::PlayerDownRouterWiring::PendingContext BuildPendingContext();
	void TickPendingOverkillRoute();
}
