#include "TFDDefeatLifecycleBootstrap.h"

namespace TFD::DefeatLifecycleBootstrap
{
    void Install(const InstallHooks& hooks)
    {
        if (hooks.buildBleedoutProviders) {
            TFD::Bleedout::InstallDefeatLifecycleProviders(hooks.buildBleedoutProviders());
        }

        if (hooks.buildFlowControllerProviders) {
            TFD::FlowController::InstallDefeatLifecycleProviders(hooks.buildFlowControllerProviders());
        }
    }
}
