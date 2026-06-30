#pragma once

#include <functional>

#include "TFDDefeatLifecycleBootstrap.h"

namespace TFD::DefeatLifecycleBootstrapWiring
{
	struct Dependencies
	{
		std::function<TFD::Bleedout::DefeatLifecycleProviders()> buildBleedoutProviders;
		std::function<TFD::FlowController::DefeatLifecycleProviders()> buildFlowControllerProviders;
	};

	TFD::DefeatLifecycleBootstrap::InstallHooks BuildHooks(const Dependencies& dependencies);
	void Install(const Dependencies& dependencies);
}
