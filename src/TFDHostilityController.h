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
        TruceInCombat
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
        bool suppressBridgeEvents = false);

    std::optional<RE::FormID> BeginCellTruceBurst(
        RE::Actor* player,
        RE::Actor* primaryTarget,
        double nowSec,
        double durationSec,
        float radius);

    bool IsSuppressed(RE::Actor* actor);
    Mode GetMode(RE::Actor* actor);
    bool CanOpenDialogue(RE::Actor* actor);
    bool PreserveTruceSessionForFlowHandoff(RE::Actor* primaryTarget, double durationSec, const char* reason = nullptr);
    bool IsFlowHandoffHoldActive(RE::Actor* actor);
    [[nodiscard]] std::vector<RE::Actor*> CollectActiveTruceActors(RE::Actor* primaryTarget);
    [[nodiscard]] std::vector<RE::Actor*> CollectDialogueTruceActors(RE::Actor* primaryTarget);
    std::size_t ReleaseDialogueTruceActors(
        RE::Actor* primaryTarget,
        const std::vector<RE::Actor*>& actors,
        ReleaseReason reason = ReleaseReason::DialogueClosed);

    bool CanStartTruce(RE::Actor* actor);
    bool HasSpentTruce(RE::Actor* actor);
    bool WasTruceBetrayed(RE::Actor* actor);

    void ReleaseSession(RE::FormID sessionId, ReleaseReason reason = ReleaseReason::Generic);
    bool ReleaseActiveTruceSessionForActor(
        RE::Actor* actor,
        ReleaseReason reason = ReleaseReason::DialogueClosed,
        bool onlyIfStillHostile = false);
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
    void CancelPendingWaves();
    void ScheduleStopCombatWaves(float radius, bool npcOnly, int waves, int intervalMs);

    void ApplyAggressionClamp(RE::Actor* actor);
    void ClearAggressionClamp();

    void TickCaptiveSuppression();
    void ResetCaptiveSuppression();

    void ClearAllTemporaryHostility();

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
