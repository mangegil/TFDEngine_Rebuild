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
        bool hasEnemies{ false };
    };

    void SetStateValue(int value);
    int GetStateValue();
    bool IsActive();

    void ResetObservedContext();
    int ComputeObservedState(const ObservedContext& context);
    void RefreshObservedState(const ObservedContext& context);

    RE::Actor* FindDialogueCapableDefeatedEnemy(float radius = 512.0f);
    bool HasDialogueCapableDefeatedEnemy(float radius = 512.0f);
    void MarkDefeatedDialogueAvailable(RE::Actor* actor);
}
