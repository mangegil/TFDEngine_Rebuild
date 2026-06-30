#include "TFDDefeatLifecycleBootstrapWiring.h"

#include <spdlog/spdlog.h>

namespace TFD::DefeatLifecycleBootstrapWiring
{
	TFD::DefeatLifecycleBootstrap::InstallHooks BuildHooks(const Dependencies& dependencies)
	{
		TFD::DefeatLifecycleBootstrap::InstallHooks hooks{};
		hooks.buildBleedoutProviders = [dependencies]() {
			return dependencies.buildBleedoutProviders ? dependencies.buildBleedoutProviders() : TFD::Bleedout::DefeatLifecycleProviders{};
			};
		hooks.buildFlowControllerProviders = [dependencies]() {
			return dependencies.buildFlowControllerProviders ? dependencies.buildFlowControllerProviders() : TFD::FlowController::DefeatLifecycleProviders{};
			};
		return hooks;
	}

	void Install(const Dependencies& dependencies)
	{
		TFD::DefeatLifecycleBootstrap::Install(BuildHooks(dependencies));
		spdlog::info("[TFD][DefeatLifecycle][P27B] lifecycle bootstrap hooks installed");
	}
}
