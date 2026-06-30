#pragma once

#include "TFDDefeatRuntimeProviders.h"

namespace TFD::DefeatRuntimeProviderWiring
{
	TFD::DefeatRuntimeProviders::InstallHooks BuildHooks();
	void Install();
}
