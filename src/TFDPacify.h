#pragma once

#include <RE/Skyrim.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace TFD::Pacify
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
        DialogueClosed,
        TooFar,
        TameBroken,
        TameExpired
    };

    enum class TameDisposition : std::uint8_t
    {
        None = 0,
        Calm,
        Companion
    };

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
        double lastPacifyApplySec{ 0.0 };
        double lastPackageEvalSec{ 0.0 };

        TameDisposition disposition{ TameDisposition::None };
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

        TameDisposition disposition{ TameDisposition::None };
        bool temporaryTeammateApplied{ false };
        bool dialogueRequested{ false };
        bool dialogueOpened{ false };
        bool finished{ false };
    };

    struct ActiveTameSnapshot
    {
        RE::FormID actorId{ 0 };
        RE::FormID sessionId{ 0 };
        Mode mode{ Mode::None };
        TameDisposition disposition{ TameDisposition::None };
        double remainingTameSec{ 0.0 };
        double remainingCompanionHours{ 0.0 };
        bool loaded{ false };
        std::string actorName{};
    };

    enum class FeedAction : std::uint8_t
    {
        Calm = 0,
        Teammate
    };

    struct FeedOptionSnapshot
    {
        RE::FormID itemId{ 0 };
        std::string label{};
        std::string itemName{};
        std::int32_t count{ 0 };
        std::int32_t cost{ 0 };
        double calmExtendSec{ 0.0 };
        FeedAction action{ FeedAction::Calm };
    };

    void Reset();
    void Update(double nowSec);

    std::optional<RE::FormID> BeginTameSession(
        RE::Actor* player,
        RE::Actor* primaryTarget,
        double nowSec,
        bool allowDialogue = false,
        bool allowLocalSplash = false);

    std::optional<RE::FormID> BeginTrucePreCombatSession(
        RE::Actor* player,
        RE::Actor* primaryTarget,
        double nowSec);

    std::optional<RE::FormID> BeginTruceInCombatSession(
        RE::Actor* player,
        RE::Actor* primaryTarget,
        double nowSec,
        bool allowDialogue,
        bool ignoreSpent = false);

    bool IsPacified(RE::Actor* actor);
    Mode GetMode(RE::Actor* actor);
    bool CanOpenDialogue(RE::Actor* actor);
    bool CanStartTame(RE::Actor* actor);
    bool HasActiveTameSession(RE::Actor* actor);
    std::vector<ActiveTameSnapshot> GetActiveTameSnapshots(double nowSec = 0.0);
    std::vector<FeedOptionSnapshot> GetActiveTameFeedOptions(RE::Actor* actor, FeedAction action);
    bool ApplyActiveTameFeed(RE::Actor* actor, RE::FormID itemId, FeedAction action);
    bool ExtendActiveTameSession(RE::Actor* actor, double addSec, double nowSec);
    double GetRemainingTameTime(RE::Actor* actor, double nowSec);
    TameDisposition GetDisposition(RE::Actor* actor);
    bool IsCompanion(RE::Actor* actor);
    bool PromoteActiveTameToCompanion(RE::Actor* actor, double addHoursGameTime);
    bool ExtendActiveCompanionHours(RE::Actor* actor, double addHoursGameTime);
    bool ReleaseActiveTameActor(RE::Actor* actor, ReleaseReason reason = ReleaseReason::Generic);
    double GetRemainingCompanionHours(RE::Actor* actor);

    bool CanStartTruce(RE::Actor* actor);
    bool HasSpentTruce(RE::Actor* actor);
    bool WasTruceBetrayed(RE::Actor* actor);

    std::optional<RE::FormID> BeginCellTruceBurst(
        RE::Actor* player,
        RE::Actor* primaryTarget,
        double nowSec,
        double durationSec,
        float radius);

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
    const char* ToString(TameDisposition disposition);
}
