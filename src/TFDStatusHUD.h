#pragma once

namespace TFD::StatusHUD
{
    enum class PlayerFlowState : int
    {
        Neutral = 0,
        Precombat = 1,
        Incombat = 2,
        // Value 3 is reserved after removal of the retired manual-interaction HUD state.
        Defeat = 4,
        Captive = 5
    };

    void Init();
    void ResetForLoad();
    void SetEnabled(bool enabled);
    void SetManualPlayerState(PlayerFlowState state);
    void ClearManualPlayerState();
}
