#pragma once

#include <functional>

#include <RE/Skyrim.h>

#include "TFDPlayerDownRouter.h"
#include "TFDPlayerDownRouterWiring.h"

namespace TFD::PlayerDownRouterOutcomeWiring
{
	struct Dependencies
	{
		std::function<void(RE::Actor*)> rememberAggressor;
		std::function<RE::Actor*(float, float, RE::Actor*)> findBleedoutSpeaker;
		std::function<void(RE::Actor*, RE::Actor*)> startBleedWindow;
		std::function<bool(RE::Actor*)> startBattleObservePending;
		std::function<void(int)> setGraceSeconds;

		std::function<bool()> hasTerminalCommit;
		std::function<bool()> hasCachedRescueDestination;
		std::function<bool()> hasPlayerBleedLock;
		std::function<void(RE::Actor*, float, const char*)> enterPlayerBleedLock;
		std::function<void(const char*, bool)> releasePlayerBleedLock;
		std::function<void(bool)> setPlayerBleedImmune;
		std::function<void(RE::Actor*)> preparePlayer;
		std::function<bool(const char*)> executeNoMarkerFallback;
	};

	void InstallProvider(Dependencies dependencies);
	void ShutdownProvider();
	bool HasProvider();

	TFD::PlayerDownRouterWiring::DispatchContext BuildDispatchContext();
	TFD::PlayerDownRouterWiring::NoThreatContext BuildNoThreatContext();
	TFD::PlayerDownRouter::DispatchHandlers BuildDispatchHandlers();
	TFD::PlayerDownRouter::NoThreatFallbackHandlers BuildNoThreatHandlers();
}
