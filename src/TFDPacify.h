#pragma once

#include <RE/Skyrim.h>

#include <cstdint>
#include <optional>

namespace TFD::Pacify
{
    enum class Mode : std::uint8_t
    {
        None = 0,
        Tame,
        TrucePreCombat,
        TruceInCombat
    };

    struct Entry
    {
        RE::FormID actorId{ 0 };
        Mode mode{ Mode::None };

        RE::FormID sessionId{ 0 };
        RE::FormID primaryTargetId{ 0 };

        double startTimeSec{ 0.0 };
        double endTimeSec{ 0.0 };

        bool allowDialogue{ false };
        bool isPrimaryTarget{ false };
    };

    struct Session
    {
        RE::FormID sessionId{ 0 };
        RE::FormID playerId{ 0 };
        RE::FormID primaryTargetId{ 0 };

        Mode primaryMode{ Mode::None };

        double startTimeSec{ 0.0 };
        double endTimeSec{ 0.0 };

        bool dialogueRequested{ false };
        bool dialogueOpened{ false };
        bool finished{ false };
    };

    void Reset();
    void Update(double nowSec);

    std::optional<RE::FormID> BeginTameSession(
        RE::Actor* player,
        RE::Actor* primaryTarget,
        double nowSec);

    std::optional<RE::FormID> BeginTrucePreCombatSession(
        RE::Actor* player,
        RE::Actor* primaryTarget,
        double nowSec);

    std::optional<RE::FormID> BeginTruceInCombatSession(
        RE::Actor* player,
        RE::Actor* primaryTarget,
        double nowSec);

    bool IsPacified(RE::Actor* actor);
    Mode GetMode(RE::Actor* actor);
    bool CanOpenDialogue(RE::Actor* actor);

    void ReleaseSession(RE::FormID sessionId);
    void ReleaseAll();

    const char* ToString(Mode mode);
}