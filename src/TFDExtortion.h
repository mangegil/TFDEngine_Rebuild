#pragma once

#include <cstdint>

namespace RE
{
    class Actor;
}

namespace TFD::Extortion
{
    void Install();
    void Shutdown();

    void CancelAll(const char* reason = "cancel_all");
    void OnPreLoadGame();
    void OnPostLoadGame();

    void BeginPreCombat(RE::Actor* actor, const char* reason);
    void HandlePreCombatOutcomeEvent(const char* eventName, RE::Actor* actor);

    bool HasActive();
    bool IsActive(RE::Actor* actor);

    bool TickPreCombat(
        RE::Actor* actor,
        double nowSec,
        bool dialogueOpen,
        bool dialogueOpenActiveForPreCombat);
}
