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

    bool BeginPreCombat(RE::Actor* actor, const char* reason);
    void HandlePreCombatOutcomeEvent(const char* eventName, RE::Actor* actor);
    void HandlePreCombatTerminalPendingEvent(RE::Actor* actor, const char* reason);

    bool HasActive();
    bool IsActive(RE::Actor* actor);

    enum class TickResult
    {
        NotActive = 0,
        Consumed,
        AllowAbort
    };

    TickResult TickPreCombat(
        RE::Actor* actor,
        double nowSec,
        bool dialogueOpen,
        bool dialogueOpenActiveForPreCombat);
}
