#pragma once

#include <RE/Skyrim.h>

#include <cstdint>

#include "TFDPacify.h"
#include "TFDTargetClassifier.h"

namespace TFD::InteractionRouter
{
    enum class Action : std::uint8_t
    {
        None = 0,
        Tame,
        TrucePreCombat,
        TruceInCombat
    };

    enum class FailReason : std::uint8_t
    {
        None = 0,
        InvalidPlayer,
        InvalidTarget,
        NoUsableAction,
        TargetRejected,
        TruceUnavailable,
        TameAlreadyActive,
        NoValidBait,
        SessionBeginFailed
    };

    struct ResolveResult
    {
        Action action{ Action::None };
        FailReason failReason{ FailReason::None };

        RE::FormID playerId{ 0 };
        RE::FormID targetId{ 0 };

        bool valid{ false };
        bool shouldBeginSession{ false };
        bool shouldOpenDialogue{ false };

        TFD::TargetClassifier::ClassifyResult classify{};
    };

    struct ExecuteResult
    {
        Action action{ Action::None };
        FailReason failReason{ FailReason::None };

        RE::FormID targetId{ 0 };
        RE::FormID sessionId{ 0 };

        bool resolved{ false };
        bool executed{ false };
        bool dialogueRequested{ false };
    };

    ResolveResult ResolveHotkeyAction(
        RE::Actor* player,
        RE::Actor* target,
        bool isCaptivePhase,
        double nowSec);

    ExecuteResult ExecuteResolvedAction(
        RE::Actor* player,
        RE::Actor* target,
        const ResolveResult& resolved,
        double nowSec);

    ExecuteResult HandleHotkeyPress(
        RE::Actor* player,
        RE::Actor* target,
        bool isCaptivePhase,
        double nowSec);

    const char* ToString(Action value);
    const char* ToString(FailReason value);
}
