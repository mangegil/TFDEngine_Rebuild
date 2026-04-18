#include "TFDDefeatBridge.h"

#include "TFDFlowController.h"
#include "TFDPreCombatGreet.h"

namespace TFD::DefeatBridge
{
    bool QueueModEvent(const char* eventName, RE::TESForm* sender, const char* strArg, float numArg)
    {
        return TFD::FlowController::QueueBridgeModEvent(eventName, sender, strArg, numArg);
    }

    TFD::Bleedout::SupportBridgeHandlers BuildBleedSupportHandlers()
    {
        TFD::Bleedout::SupportBridgeHandlers handlers{};
        handlers.queuePreCombatClearAll = []() { return QueueModEvent("TFDPreCombatClearAll", nullptr); };
        handlers.queueTruceClearAll = []() { return QueueModEvent("TFDTruceClearAll", nullptr); };
        handlers.queueInCombatClearAll = []() { return QueueModEvent("TFDInCombatClearAll", nullptr); };
        handlers.cancelAllPreCombat = []() { TFD::PreCombatGreet::CancelAll(); };
        return handlers;
    }

    void ClearBleedSupportAliases(const char* reason)
    {
        TFD::Bleedout::ClearSupportBridgeAliases(reason, BuildBleedSupportHandlers());
    }

    void QueueNonCaptiveChoiceRequest(const char* reason)
    {
        (void)TFD::Bleedout::EnterNonCaptiveChoice(
            reason ? reason : "queued_non_captive_choice",
            TFD::Bleedout::Builders::BuildNonCaptiveChoiceHandlers());
    }
}
