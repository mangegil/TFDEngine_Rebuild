#pragma once

#include <functional>
#include <vector>

#include <RE/Skyrim.h>

#include "TFDDefeatRuntimeActions.h"
#include "TFDTransition.h"

namespace TFD::DefeatTransitionWiring
{
	struct Dependencies
	{
		std::function<RE::Actor*()> getPlayer;
		std::function<RE::Actor*()> resolveAggressor;
		std::function<RE::Actor*(float)> findBestAggressor;
		std::function<bool(RE::Actor*)> isCombatSupportedAggressor;
		std::function<bool(RE::Actor*)> isActiveFollowerActor;
		std::function<bool(RE::Actor*)> isStandingAllyThresholdActor;
		std::function<std::vector<RE::Actor*>()> collectRegisteredTeammates;
		std::function<std::vector<RE::Actor*>(float, RE::Actor*, bool)> collectBleedoutCrowd;
		std::function<void(int)> setGraceSeconds;
		std::function<void(bool)> setPlayerBleedImmune;
		std::function<void()> resetBleedRuntimeState;
	};

	void InstallProvider(Dependencies dependencies);
	void ShutdownProvider();
	bool HasProvider();

	void StartBleedWindow(RE::Actor* player, RE::Actor* aggressor);
	TFD::Transition::RuntimeHandlers BuildTransitionRuntimeHandlers();
	TFD::DefeatRuntimeActions::NoMarkerFallbackContext BuildNoMarkerFallbackContext();
}
