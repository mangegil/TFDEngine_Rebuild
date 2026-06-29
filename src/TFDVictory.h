#pragma once

#include <RE/Skyrim.h>

#include <cstdint>
#include <string_view>

namespace TFD::Victory
{
    enum class SessionPhase : std::uint8_t
    {
        Empty = 0,
        OpeningDialogue,
        DialogueOpen,
        AwaitingChoiceCommit,
        KillCommitted,
        LootCommitted,
        RecruitCommitted
    };

    struct SessionSnapshot
    {
        bool active{ false };
        std::uint32_t sessionID{ 0 };
        RE::FormID selectedActorFormID{ 0 };
        SessionPhase phase{ SessionPhase::Empty };
    };

    // R394A ownership contract:
    // - DefeatMonitor only reports hostile enemies below Enemy Downed Threshold.
    // - Victory owns hostile-enemy defeated state, registry, countdown, and auto-death.
    // - Victory also owns the selected-actor manual dialogue session.
    // - FlowController, ForceGreetState, and generic SystemEvent routes do not own Victory.
    bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm);

    void Install();
    void Shutdown();

    bool NotifyEnemyBelowThreshold(RE::Actor* actor, float thresholdPct, std::string_view reason);
    void Tick();

    bool ReleaseManagedEnemy(RE::Actor* actor, std::string_view reason, bool playGetUp);
    void SuppressAutoDeathForExternalFight(RE::Actor* actor, double seconds, std::string_view reason);
    void RestoreActorHealthToSafePct(
        RE::Actor* actor,
        float thresholdPct,
        float bonusPct,
        float minSafePct,
        float maxSafePct,
        float minAbsHp,
        std::string_view reason);

    void ResetForLoad(std::string_view reason);
    void SetLoadTransition(bool active, std::string_view reason);

    // Returns true when the activation belongs to Victory and must be consumed,
    // including safe rejections such as active threats or another open session.
    bool BeginManualInteraction(RE::Actor* selectedActor, std::string_view reason);

    // Routed from the global UI menu sink. Victory ignores these unless it owns
    // an active selected-actor session.
    void NotifyDialogueMenuStateChanged(bool opening);
    void NotifyContainerMenuStateChanged(bool opening);

    // Terminal outcome owned entirely by Victory. The fragment only commits the
    // choice. Death is staged after DialogueMenu closes so the normal death graph
    // can run and TopicInfo end-fragment ordering cannot invalidate the session.
    bool RequestKill(RE::Actor* speaker, std::string_view reason);

    // Delayed terminal Loot outcome. The TopicInfo fragment only sends this
    // request. Victory owns dialogue-close timing, native OpenInventory dispatch,
    // ContainerMenu observation, one-shot Get Up, and final cleanup.
    bool RequestLoot(RE::Actor* speaker, std::string_view reason);

    // Terminal Recruit outcome. The fragment only commits the choice. Victory
    // owns conversion order, teammate handoff, visual release, deferred teammate package handoff, defeated cleanup,
    // and session finalization.
    bool RequestRecruit(RE::Actor* speaker, std::string_view reason);

    // R414A diagnostic-only hook. Called from the global hit sink after Victory
    // Recruit finalizes so player-hit visual recovery can be sampled without
    // changing behavior.
    void ObserveRecentRecruitHit(RE::Actor* target, RE::Actor* cause, std::string_view reason);

    // R422A: CombatBehavior bridge must not retarget actors while Victory owns
    // defeated enemies or an active Victory dialogue session. This prevents
    // Papyrus StartCombat storms against pending/defeated Victory targets.
    bool IsCombatBehaviorSuppressed();
    bool IsCombatBehaviorSuppressedActor(RE::Actor* actor);

    // Public reset is a non-terminal cancel: selected actor remains defeated and
    // its ten-second countdown restarts from the beginning.
    void ResetSession(std::string_view reason);
    bool IsSessionActive();
    SessionSnapshot GetSessionSnapshot();
    const char* ToString(SessionPhase phase);
}
