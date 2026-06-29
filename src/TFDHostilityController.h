#pragma once

#include <RE/Skyrim.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace TFD::HostilityController
{
    enum class Mode : std::uint8_t
    {
        None = 0,
        Tame,
        TrucePreCombat,
        TruceInCombat,
        BleedoutSuppress,
        BleedoutPayReleaseSuppress
    };

    enum class ReleaseReason : std::uint8_t
    {
        Generic = 0,
        HardFailsafeExpired,
        InvalidActor,
        PlayerAggression,
        PlayerArmed,
        FightChoice,
        DialogueClosed,
        FlowHandoff,
        InCombatPleasureEnd,
        TooFar,
        TameBroken,
        TameExpired
    };
}

namespace TFD::Tame
{
    enum class TameDisposition : std::uint8_t;
}

namespace TFD::HostilityController
{
    struct Entry
    {
        RE::FormID actorId{ 0 };
        Mode mode{ Mode::None };

        RE::FormID sessionId{ 0 };
        RE::FormID primaryTargetId{ 0 };

        double startTimeSec{ 0.0 };
        double lastCalmRefreshSec{ 0.0 };
        double endTimeSec{ 0.0 };
        double companionExpireGameDays{ 0.0 };
        double lastSuppressionApplySec{ 0.0 };
        double lastPackageEvalSec{ 0.0 };

        float originalAggression{ 0.0f };
        bool hasOriginalAggression{ false };
        bool hadPacifyFaction{ false };
        bool addedPacifyFaction{ false };
        bool stablePacifyInitialized{ false };
        bool resultDialogueLightHoldActive{ false };

        TFD::Tame::TameDisposition disposition{ static_cast<TFD::Tame::TameDisposition>(0) };
        bool temporaryTeammateApplied{ false };
        bool allowDialogue{ false };
        bool isPrimaryTarget{ false };
    };

    struct Session
    {
        RE::FormID sessionId{ 0 };
        RE::FormID playerId{ 0 };
        RE::FormID primaryTargetId{ 0 };

        Mode primaryMode{ Mode::None };
        ReleaseReason pendingReleaseReason{ ReleaseReason::Generic };

        double startTimeSec{ 0.0 };
        double lastCalmRefreshSec{ 0.0 };
        double endTimeSec{ 0.0 };
        double companionExpireGameDays{ 0.0 };
        double invalidSinceSec{ 0.0 };
        double armedSinceSec{ 0.0 };
        double tooFarSinceSec{ 0.0 };
        double tameStartleSinceSec{ 0.0 };
        double lastPlayerSampleSec{ 0.0 };
        RE::NiPoint3 lastPlayerPos{};
        bool hasPlayerSample{ false };

        TFD::Tame::TameDisposition disposition{ static_cast<TFD::Tame::TameDisposition>(0) };
        bool temporaryTeammateApplied{ false };
        bool dialogueRequested{ false };
        bool dialogueOpened{ false };
        bool suppressBridgeEvents{ false };
        bool flowHandoffHold{ false };
        double flowHandoffHoldUntilSec{ 0.0 };
        std::vector<RE::FormID> dialogueAssignedActorIds{};
        bool finished{ false };
    };

    void Reset();
    void ResetForLoad();
    void Update(double nowSec);

    std::optional<RE::FormID> BeginTrucePreCombatSession(
        RE::Actor* player,
        RE::Actor* primaryTarget,
        double nowSec);

    std::optional<RE::FormID> BeginTruceInCombatSession(
        RE::Actor* player,
        RE::Actor* primaryTarget,
        double nowSec,
        bool allowDialogue,
        bool ignoreSpent = false,
        bool suppressBridgeEvents = false,
        bool allowPacifiedBridge = false);

    std::optional<RE::FormID> BeginCellTruceBurst(
        RE::Actor* player,
        RE::Actor* primaryTarget,
        double nowSec,
        double durationSec,
        float radius);

    // P14OWN: Bleedout owns its own suppression. It must not borrow
    // TruceInCombat/Tame sessions because that makes InCombat bridge events,
    // truce_spent, ignoreSpent retry, and tame promotion leak into Bleedout.
    std::optional<RE::FormID> BeginBleedoutSuppressSession(
        RE::Actor* player,
        RE::Actor* primaryTarget,
        double nowSec,
        double durationSec = 0.0,
        bool applyCellBubble = false,
        float radius = 0.0f,
        const char* reason = nullptr);

    std::optional<RE::FormID> BeginBleedoutSuppressBurst(
        RE::Actor* player,
        RE::Actor* primaryTarget,
        double nowSec,
        double durationSec,
        float radius,
        const char* reason = nullptr);

    // P15BOWN: Bleedout Pay is terminal PayRelease, not a linked Pay > Release branch.
    // This final owner starts after dialogue cleanup, so cleanup cannot kill it in the same frame.
    std::optional<RE::FormID> BeginBleedoutPayReleaseSuppressSession(
        RE::Actor* player,
        RE::Actor* primaryTarget,
        double nowSec,
        double durationSec = 0.0,
        const char* reason = nullptr);

    bool ReleaseBleedoutSuppressSession(
        RE::FormID sessionId,
        ReleaseReason reason = ReleaseReason::Generic,
        const char* debugReason = nullptr);
    bool IsBleedoutSuppressed(RE::Actor* actor);
    bool IsBleedoutPayReleaseSuppressed(RE::Actor* actor);

    bool IsSuppressed(RE::Actor* actor);
    Mode GetMode(RE::Actor* actor);
    bool CanOpenDialogue(RE::Actor* actor);
    bool PreserveTruceSessionForFlowHandoff(RE::Actor* primaryTarget, double durationSec, const char* reason = nullptr);

    // R499A: renew an existing InCombat Pleasure handoff without recollecting
    // actors or mutating combat, alarm, AI package, or dialogue ownership.
    bool RefreshTruceSessionForFlowHandoff(RE::Actor* sessionActor, double durationSec, const char* reason = nullptr);

    bool IsFlowHandoffHoldActive(RE::Actor* actor);

    // P5PAY: lightweight in-dialogue passive guard for CK Pay linked topics.
    // This is not a dialogue closer and not a terminal outcome owner. It only
    // prevents old truce/session cleanup from restoring aggression between
    // Pay and the real follow-up choice.
    void ArmPayDialoguePassiveGuard(RE::Actor* actor, double durationSec = 30.0, const char* reason = nullptr);
    bool IsPayDialoguePassiveGuardActive(RE::Actor* actor);
    void ReleasePayDialoguePassiveGuard(RE::Actor* actor, bool restoreAggression = true, const char* reason = nullptr);
    void ConsumePayDialoguePassiveGuardAsPersistent(RE::Actor* actor, const char* reason = nullptr);
    void ExtendPayDialoguePassiveGuardCombatBlock(RE::Actor* actor, double durationSec = 5.0, const char* reason = nullptr);
    void MarkPayDialoguePassiveGuardRemovePacifyOnRelease(RE::Actor* actor, const char* reason = nullptr);
    bool IsPayDialoguePersistentTakeoverPending(RE::Actor* actor);

    // P9: hook-facing suppression is split. UpdateCombat may be blocked during
    // a short passive handoff, but DoDetect is only blocked when a still-hostile
    // actor tries to detect the player/player-side. Do not blind converted
    // followers or block follower detection of enemies.
    bool ShouldBlockCombatUpdateForActor(RE::Actor* actor);
    bool ShouldBlockPlayerSideDetection(RE::Actor* viewer, RE::Actor* target);
    [[nodiscard]] std::vector<RE::Actor*> CollectActiveTruceActors(RE::Actor* primaryTarget);
    [[nodiscard]] std::vector<RE::Actor*> CollectDialogueTruceActors(RE::Actor* primaryTarget);
    [[nodiscard]] std::vector<RE::Actor*> CollectInCombatStyleTruceActors(
        RE::Actor* player,
        RE::Actor* primaryTarget,
        float radius);
    std::size_t ReleaseDialogueTruceActors(
        RE::Actor* primaryTarget,
        const std::vector<RE::Actor*>& actors,
        ReleaseReason reason = ReleaseReason::DialogueClosed);
    std::optional<RE::FormID> PromoteTruceActorForCycle(
        RE::Actor* actor,
        const char* debugReason = nullptr);

    bool DemoteTruceActorForCycleHold(
        RE::Actor* actor,
        const char* debugReason = nullptr);

    bool ReleaseSingleTruceActorForCycle(
        RE::Actor* actor,
        ReleaseReason reason = ReleaseReason::FlowHandoff,
        const char* debugReason = nullptr);

    bool CanStartTruce(RE::Actor* actor);
    bool HasSpentTruce(RE::Actor* actor);
    bool WasTruceBetrayed(RE::Actor* actor);

    bool AbortActiveInCombatTruceOnHit(const RE::TESHitEvent* ev, const char* reason = nullptr);
    void ReleaseSession(RE::FormID sessionId, ReleaseReason reason = ReleaseReason::Generic);
    bool ReleaseActiveTruceSessionForActor(
        RE::Actor* actor,
        ReleaseReason reason = ReleaseReason::DialogueClosed,
        bool onlyIfStillHostile = false);
    bool BreakPassiveOwnershipForFightChoice(
        RE::Actor* actor,
        RE::Actor* player,
        const char* debugReason = nullptr);
    bool BreakCaptiveFightPassiveOwnership(
        RE::Actor* actor,
        RE::Actor* player,
        const char* debugReason = nullptr);

    bool ForceDetectionAndCombatRefresh(
        RE::Actor* actor,
        RE::Actor* player,
        ReleaseReason reason = ReleaseReason::Generic,
        bool drawWeapon = true);
    void QueueDetectionAndCombatRefresh(
        RE::Actor* actor,
        RE::Actor* player,
        ReleaseReason reason = ReleaseReason::Generic,
        bool drawWeapon = true);
    void ReleaseAll();

    const char* ToString(Mode mode);
    const char* ToString(ReleaseReason reason);
    const char* ToString(TFD::Tame::TameDisposition disposition);

    void StopCombatSweep(float radius, bool npcOnly);
    void StopCombatAndAlarmSweep(float radius, bool npcOnly, const char* reason = nullptr);
    void CancelPendingWaves();
    void ScheduleStopCombatWaves(float radius, bool npcOnly, int waves, int intervalMs);
    void ScheduleStopCombatAndAlarmWaves(float radius, bool npcOnly, int waves, int intervalMs, const char* reason = nullptr);

    void ApplyAggressionClamp(RE::Actor* actor);
    void ClearAggressionClamp();
    void ClearAggressionClampForSettledHandoff(const char* reason = nullptr);

    void TickCaptiveSuppression();
    void ResetCaptiveSuppression();

    [[nodiscard]] bool HasVisibleCaptiveCombatWitness(
        RE::Actor* player,
        RE::Actor* preferredActor = nullptr,
        const char* reason = nullptr);

    std::size_t BreakCaptivePassiveForCombat(
        RE::Actor* player,
        RE::Actor* triggerActor,
        ReleaseReason reason = ReleaseReason::PlayerAggression,
        const char* debugReason = nullptr,
        bool requireLineOfSightForCrowd = true,
        bool forceTriggerActor = true);

    std::size_t SoftReleaseCaptiveCrowdPassiveForCombat(
        RE::Actor* player,
        RE::Actor* triggerActor,
        const char* debugReason = nullptr,
        bool requireLineOfSightForCrowd = true);

    void ClearAllTemporaryHostility();
    void ArmFightChoiceCombatOwnerBypass(RE::Actor* actor, double durationSec = 12.0, const char* reason = nullptr);

    struct BleedTruceRuntimeProviders
    {
        std::function<bool(RE::Actor*, RE::Actor*, const char*)> startSessionForSpeaker;
        std::function<void(ReleaseReason)> releaseSession;
    };

    void InstallBleedTruceRuntimeProviders(BleedTruceRuntimeProviders providers);
    void ResetBleedTruceRuntimeProviders();
    bool StartBleedTruceSessionForSpeaker(RE::Actor* player, RE::Actor* speaker, const char* reason);
    void ReleaseBleedTruceSession(ReleaseReason reason);

    bool HasActiveDialoguePhaseFaction(RE::Actor* actor);
    bool IsActorTemporarilySuppressed(RE::Actor* actor);
}

namespace TFD::HostilityController::Runtime
{
    using EntryMap = std::unordered_map<RE::FormID, Entry>;
    using SessionMap = std::unordered_map<RE::FormID, Session>;

    EntryMap& Entries();
    SessionMap& Sessions();

    double NowSec();
    double GameDays();
    double ClampTameEndTime(double nowSec, double endTimeSec);

    RE::Actor* ResolveActor(RE::FormID actorId);
    void SyncSessionDisposition(Session& session);
    void SendModEvent(const char* eventName, RE::Actor* sender);
}

namespace TFD::HostilityController::Internal
{
    bool ValidateActor(RE::Actor* actor);
    std::optional<RE::FormID> BeginTameBaseSession(
        RE::Actor* player,
        RE::Actor* primaryTarget,
        double nowSec,
        bool allowDialogue);
}
