#pragma once

#include <RE/Skyrim.h>

#include "TFDBleedout.h"

namespace TFD::DefeatBridge
{
    bool QueueModEvent(const char* eventName, RE::TESForm* sender = nullptr, const char* strArg = "", float numArg = 0.0f);
    TFD::Bleedout::SupportBridgeHandlers BuildBleedSupportHandlers();
    void ClearBleedSupportAliases(const char* reason);
    void QueueNonCaptiveChoiceRequest(const char* reason);
}
