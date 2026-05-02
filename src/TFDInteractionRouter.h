#pragma once

#include <RE/Skyrim.h>

#include <cstdint>



namespace TFD::FlowController
{
    struct Snapshot;
}

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

    enum class FlowOwnedPrimaryKind : std::uint8_t
    {
        None = 0,
        Busy,
        Escape,
        Bleedout,
        Captive
    };

    struct FlowOwnedPrimaryResult
    {
        FlowOwnedPrimaryKind kind{ FlowOwnedPrimaryKind::None };
        int interactionState{ 0 };
        bool handled{ false };
        bool success{ false };
        const char* notification{ nullptr };
    };

    struct PrimaryHotkeyPickResult
    {
        RE::Actor* target{ nullptr };
        Action action{ Action::None };
        int interactionState{ 0 };
        bool valid{ false };
    };

    Action ResolvePreferredTruceAction(const TFD::FlowController::Snapshot& snapshot);
    int GetInteractionStateForAction(Action action);
    void SetInteractionStateValue(int value);
    void SetInteractionStateForAction(Action action);
    void ClearInteractionStateValue();
    int GetInteractionStateValue();
    bool BeginTruceForAction(RE::Actor* target, Action preferredAction, Action* outAction = nullptr);
    FlowOwnedPrimaryResult HandleFlowOwnedPrimaryHotkey(RE::Actor* player, const TFD::FlowController::Snapshot& snapshot);

    RE::Actor* PickExactDialogueDefeatedTarget(float radius);
    RE::Actor* PickExactTeammateDialogueTarget(float radius);
    RE::Actor* PickExactActiveTameTarget(float radius);
    RE::Actor* PickExactDefeatedCreatureTarget(float radius);

    PrimaryHotkeyPickResult PickPrimaryHotkeyTarget(
        RE::PlayerCharacter* player,
        const TFD::FlowController::Snapshot& snapshot,
        float radius,
        bool allowTameFallback = false);

    struct PrimaryHotkeyExecuteResult
    {
        Action requestedAction{ Action::None };
        Action finalAction{ Action::None };
        FailReason failReason{ FailReason::None };
        int interactionState{ 0 };
        bool handled{ false };
        bool success{ false };
        const char* notification{ nullptr };
    };

    PrimaryHotkeyExecuteResult ExecutePrimaryHotkey(
        RE::Actor* player,
        const TFD::FlowController::Snapshot& snapshot,
        double nowSec,
        float radius,
        bool allowTameFallback = false);

    enum class ShiftHotkeyAction : std::uint8_t
    {
        None = 0,
        RecruitDefeatedCreature,
        OpenFeedPopup
    };

    struct ShiftHotkeyExecuteResult
    {
        ShiftHotkeyAction action{ ShiftHotkeyAction::None };
        RE::Actor* target{ nullptr };
        int interactionState{ 0 };
        bool handled{ false };
        bool success{ false };
        const char* notification{ nullptr };
    };

    ShiftHotkeyExecuteResult ExecuteShiftHotkey(
        RE::Actor* player,
        double nowSec,
        float radius);

    namespace DialogueOpen
    {
        enum class Mode : std::int32_t
        {
            None = 0,
            Bleedout = 1,
            CaptiveMarker = 2,
            InCombatTruce = 3,
            PreCombatTruce = 4,
            AfterPleasure = 5,
            Rescue = 6,
            PreCombatFollowup = 7
        };

        void Install();

        void BeginBleedout(RE::Actor* speaker);
        void BeginCaptiveMarker(RE::Actor* speaker);
        void BeginInCombatTruce(RE::Actor* speaker);
        void BeginPreCombatTruce(RE::Actor* speaker);
        void BeginPreCombatFollowup(RE::Actor* speaker);
        void BeginAfterPleasure(RE::Actor* speaker);
        void BeginRescue(RE::Actor* speaker);

        void Tick();

        void Cancel();
        bool ForceCloseDialogueMenu(const char* reason = nullptr);
        bool IsActive();
        bool DidSucceed();
        Mode GetMode();

        void ArmTemporaryDialogueCooldown(RE::Actor* speaker, double durationSec, const char* reason = nullptr);
        bool IsTemporaryDialogueCooldownActive(RE::Actor* speaker);
    }

    const char* ToString(Action value);
    const char* ToString(FailReason value);
}
