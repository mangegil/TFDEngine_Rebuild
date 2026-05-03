#pragma once

#include <RE/Skyrim.h>

#include <cstdint>

namespace TFD::Victory
{
    struct ObservedContext
    {
        bool hasPlayer{ false };
        bool playerDown{ false };
        bool combatContext{ false };
        // This is the standing/active observed enemy roster from DefeatMonitor, not every living downed enemy.
        bool hasEnemies{ false };
    };

    void SetStateValue(int value);
    int GetStateValue();
    bool IsActive();

    void ResetObservedContext();
    void ArmDialogueReadyHold(RE::Actor* actor, double seconds, const char* reason = nullptr);
    int ComputeObservedState(const ObservedContext& context);
    void RefreshObservedState(const ObservedContext& context);

    RE::Actor* FindDialogueCapableDefeatedEnemy(float radius = 512.0f);
    bool HasDialogueCapableDefeatedEnemy(float radius = 512.0f);
    void MarkDefeatedDialogueAvailable(RE::Actor* actor);
}
