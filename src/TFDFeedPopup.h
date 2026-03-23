#pragma once

#include <RE/Skyrim.h>

namespace TFD::FeedPopup
{
    void Init();
    bool Open(RE::Actor* target);
    void Close();
    bool IsOpen();
    bool PrevSelection();
    bool NextSelection();
    bool ConfirmSelection();
    bool CancelSelection();
}
