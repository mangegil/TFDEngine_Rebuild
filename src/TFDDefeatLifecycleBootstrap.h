#pragma once

#include <functional>

#include "TFDBleedout.h"
#include "TFDFlowController.h"

namespace TFD::DefeatLifecycleBootstrap
{
    struct InstallHooks
    {
        std::function<TFD::Bleedout::DefeatLifecycleProviders()> buildBleedoutProviders;
        std::function<TFD::FlowController::DefeatLifecycleProviders()> buildFlowControllerProviders;
    };

    void Install(const InstallHooks& hooks);
}
