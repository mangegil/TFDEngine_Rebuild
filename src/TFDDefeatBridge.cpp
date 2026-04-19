#include "TFDDefeatBridge.h"

#include "TFDFlowController.h"
#include "TFDPreCombatGreet.h"

#include <spdlog/spdlog.h>

namespace TFD::DefeatBridge
{
    bool QueueModEvent(const char* eventName, RE::TESForm* sender, const char* strArg, float numArg)
    {
        return TFD::FlowController::QueueBridgeModEvent(eventName, sender, strArg, numArg);
    }

    void AssignPlayerSavior(RE::Actor* actor)
    {
        if (!actor) {
            return;
        }
        const bool queued = QueueModEvent("TFDPlayerSaviorAssign", actor);
        spdlog::info("[TFD][SaviorBridge] Assign actor={:08X} queued={}", actor->GetFormID(), queued);
    }

    void ClearPlayerSavior(RE::TESForm* sender, const char* reason)
    {
        const bool queued = QueueModEvent("TFDPlayerSaviorClear", sender);
        spdlog::info("[TFD][SaviorBridge] Clear queued={} reason={}", queued, reason ? reason : "unknown");
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
