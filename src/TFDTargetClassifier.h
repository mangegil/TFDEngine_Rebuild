#pragma once

#include <RE/Skyrim.h>

#include <cstdint>

namespace TFD::TargetClassifier
{
    enum class TargetKind : std::uint8_t
    {
        None = 0,
        Negotiable,
        Creature,
        Ignore
    };

    enum class InteractionIntent : std::uint8_t
    {
        None = 0,
        Tame,
        Truce
    };

    enum class RejectReason : std::uint8_t
    {
        None = 0,
        InvalidActor,
        NotNegotiable,
        NotCreature,
        TameRequiresPreCombat,
        TooFar,
        UnsafeState,
        CaptiveOnlyMode
    };

    struct ClassifyResult
    {
        TargetKind kind{ TargetKind::None };
        InteractionIntent intent{ InteractionIntent::None };
        RejectReason rejectReason{ RejectReason::None };

        bool valid{ false };
        bool negotiable{ false };
        bool tameable{ false };

        bool allowDialogue{ false };
        bool requiresPreCombat{ false };
        bool allowsInCombat{ false };
    };

    bool IsValidActor(RE::Actor* actor);

    bool IsNegotiable(RE::Actor* actor);
    bool IsCreature(RE::Actor* actor);

    bool CanUseTruce(RE::Actor* actor);
    bool CanUseTame(RE::Actor* actor);

    ClassifyResult ClassifyForHotkey(
        RE::Actor* player,
        RE::Actor* target,
        bool isCaptivePhase,
        bool targetInCombat,
        float distanceToPlayer);

    const char* ToString(TargetKind value);
    const char* ToString(InteractionIntent value);
    const char* ToString(RejectReason value);
}
