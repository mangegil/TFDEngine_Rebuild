#pragma once

#include <cstdint>
#include <functional>

#include <RE/Skyrim.h>

#include "TFDDefeatRuntimeActions.h"
#include "TFDFlowController.h"
#include "TFDTransition.h"

namespace TFD::DefeatFlowLifecycleWiring
{
	struct Dependencies
	{
		std::function<bool()> inBleedState;
		std::function<std::uint32_t()> resolveBleedFlowActorFormID;
		std::function<void()> resetBleedRuntimeState;
		std::function<void(bool)> setPlayerBleedImmune;
		std::function<TFD::DefeatRuntimeActions::NoMarkerFallbackContext()> buildNoMarkerFallbackContext;
		std::function<RE::Actor*()> resolveObservedDownedFollower;
		std::function<TFD::Transition::RuntimeHandlers()> buildTransitionRuntimeHandlers;
	};

	void InstallProvider(Dependencies dependencies);
	void ShutdownProvider();
	bool HasProvider();
	TFD::FlowController::DefeatLifecycleProviders BuildProviders();
}
