#pragma once

#include <RE/Skyrim.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace TFD::Actor
{
    bool SharesAllowedFactionExact(RE::Actor* lhs, RE::Actor* rhs);

    struct ScanOptions
    {
        float radius{ 0.0f };
        bool npcOnly{ false };
    };

    enum class ParticipationKind : std::uint8_t
    {
        None = 0,
        Participant,
        Outsider
    };

    struct ActorInfo
    {
        RE::ActorHandle actor{};
        std::uint32_t formID{ 0 };
        float dist{ 0.0f };
        bool hostileToPlayer{ false };
        bool inCombat{ false };
        bool standing{ false };
        bool playerSide{ false };
        ParticipationKind participation{ ParticipationKind::None };
        std::int32_t coalitionID{ -1 };
        RE::ActorHandle currentTarget{};
        std::uint32_t currentTargetFormID{ 0 };
        std::vector<std::uint32_t> targetedBy;
        bool isTargetingAnyone{ false };
        bool isTargetedByAnyone{ false };
        bool isMutuallyEngaged{ false };
        bool isBattleParticipant{ false };
        bool isOutsider{ false };

        [[nodiscard]] RE::Actor* get() const
        {
            auto sp = actor.get();
            return sp.get();
        }

        [[nodiscard]] RE::Actor* getCurrentTarget() const
        {
            auto sp = currentTarget.get();
            return sp.get();
        }
    };

    struct CoalitionInfo
    {
        std::int32_t coalitionID{ -1 };
        bool playerSide{ false };
        bool hostileToPlayerSide{ false };
        std::uint32_t memberCount{ 0 };
        std::uint32_t standingCount{ 0 };
        std::vector<std::uint32_t> memberFormIDs;
        std::vector<std::uint32_t> standingMemberFormIDs;
        std::uint32_t speakerCandidateFormID{ 0 };
        std::vector<std::uint32_t> crowdCandidateFormIDs;
    };

    struct Snapshot
    {
        RE::ActorHandle player{};
        ScanOptions options{};
        std::vector<ActorInfo> actors;
        std::vector<CoalitionInfo> coalitions;
        std::vector<std::uint32_t> outsiderFormIDs;
        std::int32_t playerCoalitionID{ -1 };
        std::int32_t winningCoalitionCandidateID{ -1 };
        std::uint32_t activeCoalitionCount{ 0 };
        bool conflictResolved{ true };
    };

    [[nodiscard]] Snapshot BuildSnapshot(float radius, bool npcOnly = false);
    [[nodiscard]] Snapshot BuildSnapshot(RE::Actor* player, const ScanOptions& options);
    [[nodiscard]] const ActorInfo* FindActorInfo(const Snapshot& snapshot, RE::Actor* actor);
    [[nodiscard]] const ActorInfo* FindActorInfo(const Snapshot& snapshot, std::uint32_t formID);
    [[nodiscard]] const CoalitionInfo* FindCoalition(const Snapshot& snapshot, std::int32_t coalitionID);
    [[nodiscard]] RE::Actor* GetCurrentTarget(const Snapshot& snapshot, RE::Actor* actor);
    [[nodiscard]] RE::Actor* GetCurrentTarget(RE::Actor* actor);
    [[nodiscard]] RE::Actor* ResolveSpeakerCandidate(const Snapshot& snapshot, std::int32_t coalitionID);
    [[nodiscard]] std::vector<RE::Actor*> ResolveCrowdCandidates(const Snapshot& snapshot, std::int32_t coalitionID);
    [[nodiscard]] std::vector<RE::Actor*> ResolveStandingCoalitionMembers(const Snapshot& snapshot, std::int32_t coalitionID);
    [[nodiscard]] std::vector<RE::Actor*> ResolveStandingPlayerSideActors(const Snapshot& snapshot, bool includePlayer = false);
    [[nodiscard]] std::vector<RE::Actor*> GetAttackersOf(const Snapshot& snapshot, RE::Actor* actor);
    [[nodiscard]] bool IsActorTargetingAnyone(const Snapshot& snapshot, RE::Actor* actor);
    [[nodiscard]] bool IsActorTargetedByAnyone(const Snapshot& snapshot, RE::Actor* actor);
    [[nodiscard]] bool IsMutuallyEngaged(const Snapshot& snapshot, RE::Actor* actor);
    [[nodiscard]] bool IsActorParticipatingInBattle(const Snapshot& snapshot, RE::Actor* actor);
    [[nodiscard]] bool IsActorOutsider(const Snapshot& snapshot, RE::Actor* actor);
    [[nodiscard]] bool HasStandingPlayerSide(const Snapshot& snapshot);
    [[nodiscard]] bool HasStandingTeammateOnPlayerSide(const Snapshot& snapshot);
    [[nodiscard]] bool HasStandingHostileCoalition(const Snapshot& snapshot);
    [[nodiscard]] bool IsConflictResolved(const Snapshot& snapshot);

    [[nodiscard]] bool IsDownByHealthThreshold(RE::Actor* actor, float thresholdPct);
    [[nodiscard]] RE::Actor* FindBestAggressor(float radius, RE::Actor* player = nullptr);
    [[nodiscard]] RE::Actor* ResolveAggressor(float radius = 0.0f, RE::Actor* player = nullptr);

    // Legacy compatibility shim. Snapshot-based APIs should be preferred by new callers.
    namespace Scan
    {
        struct Entry
        {
            RE::ActorHandle actor;
            float dist{ 0.0f };
            bool hostile{ false };
            bool inCombat{ false };
        };

        std::int32_t Rescan(float radius, bool npcOnly);
        std::int32_t GetCount();
        RE::Actor* GetActor(std::int32_t index);
        Entry GetEntry(std::int32_t index);
        std::string GetActorName(std::int32_t index);
        RE::Actor* SelectFacingTarget();
    }

    namespace Interaction
    {
        enum class TargetKind : std::uint8_t
        {
            None = 0,
            Negotiable,
            Creature,
            Ignore
        };

        enum class Intent : std::uint8_t
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

        enum class CreatureClass : std::uint8_t
        {
            None = 0,
            FullDialogue,
            SimpleCommand,
            NonverbalIntelligent,
            Beast,
            UnknownFallback
        };

        enum class TruceMode : std::uint8_t
        {
            Auto = 0,
            PreCombat,
            InCombat
        };

        struct ClassifyResult
        {
            TargetKind kind{ TargetKind::None };
            Intent intent{ Intent::None };
            RejectReason rejectReason{ RejectReason::None };
            CreatureClass creatureClass{ CreatureClass::None };

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
        CreatureClass GetCreatureClass(RE::Actor* actor);
        bool CanUseTruce(RE::Actor* actor);
        bool CanUseTame(RE::Actor* actor);
        ClassifyResult ClassifyTarget(
            RE::Actor* player,
            RE::Actor* target,
            bool isCaptivePhase,
            bool targetInCombat,
            float distanceToPlayer,
            TruceMode truceMode = TruceMode::Auto);
        const char* ToString(TargetKind value);
        const char* ToString(Intent value);
        const char* ToString(RejectReason value);
        const char* ToString(CreatureClass value);
    }

    namespace Ops
    {
        inline constexpr const char* kAllowListEditorId = "TFDAllowedAggressorFactions";

        void Initialize();
        bool ApplyAggressorFactionContext(RE::Actor* aggressor);
        void ClearAggressorFactionContext();
        bool HasAggressorFactionContext();
        bool SharesAllowedFactionExact(RE::Actor* lhs, RE::Actor* rhs);

        std::vector<RE::Actor*> CollectTruceActors();
        std::vector<RE::Actor*> CollectTruceActorsForSpeaker(RE::Actor* speaker);
        bool HasAnyReleaseFollowGrace();
        bool HasReleaseFollowGrace(RE::Actor* actor);
        void ApplyReleaseFollowGraceToActorOnly(RE::Actor* actor, double durationSeconds, const char* reason = nullptr);
        void RemoveReleaseFollowGraceFromActorOnly(RE::Actor* actor, const char* reason = nullptr);
        void ApplyReleaseFollowGraceToSpeakerAndCrowd(RE::Actor* speaker, double durationSeconds, const char* reason = nullptr);
        void RemoveReleaseFollowGraceFromSpeakerAndCrowd(RE::Actor* speaker, const char* reason = nullptr);
        void CancelReleaseFollowGraceFromPlayerAggression(RE::Actor* actor, const char* reason = nullptr);
        void MaintainReleaseFollowGrace();
        void ClearAllReleaseFollowGrace(const char* reason = nullptr);

        struct DefeatedEnemyQueryHooks
        {
            bool (*isTrackedEnemy)(RE::Actor* actor){ nullptr };
            bool (*isLastAggressor)(RE::Actor* actor){ nullptr };
        };

        struct DefeatedEnemyStateHooks
        {
            bool (*tryGetState)(RE::Actor* actor, std::uint8_t* lockKindValue, bool* defeatedManaged, std::chrono::steady_clock::time_point* deadline){ nullptr };
        };

        void InstallDefeatedEnemyQueryHooks(const DefeatedEnemyQueryHooks& hooks);
        void InstallDefeatedEnemyStateHooks(const DefeatedEnemyStateHooks& hooks);
        bool IsDefeatedEnemyCandidate(RE::Actor* actor);
        void SuppressDefeatedEnemyReentry(RE::Actor* actor, double seconds, const char* reason = nullptr);
        void ApplyDefeatedEnemyPassiveOverride(RE::Actor* actor, float& savedAggression, bool& aggressionOverridden);
        void RestoreDefeatedEnemyPassiveOverride(RE::Actor* actor, float& savedAggression, bool& aggressionOverridden);
        bool IsDefeatedEnemyKnocked(RE::Actor* actor);
        bool IsDefeatedEnemyKnocked(RE::Actor* actor, std::uint8_t lockKindValue, bool defeatedManaged);
        bool IsDialogueCapableDefeatedEnemy(RE::Actor* actor);
        bool IsDialogueCapableDefeatedEnemy(RE::Actor* actor, std::uint8_t lockKindValue, bool defeatedManaged);
        bool IsCreatureDefeatedEnemy(RE::Actor* actor);
        bool IsCreatureDefeatedEnemy(RE::Actor* actor, std::uint8_t lockKindValue, bool defeatedManaged);
        double GetDefeatedEnemyRemainingSeconds(RE::Actor* actor);
        double GetDefeatedEnemyRemainingSeconds(RE::Actor* actor, bool defeatedManaged, std::chrono::steady_clock::time_point deadline);

        void SyncDefeatedEnemyMirror(RE::Actor* actor, int& aliasSlot, bool& factionApplied);
        void ClearDefeatedEnemyMirror(RE::Actor* actor, int& aliasSlot, bool& factionApplied, const char* reason = nullptr);
        void ClearDefeatedEnemyState(
            RE::Actor* actor,
            int& aliasSlot,
            bool& factionApplied,
            bool& defeatedManaged,
            bool& defeatedAutoDeathIssued,
            bool& defeatedFatalDamageApplied,
            std::chrono::steady_clock::time_point& defeatedDeadline,
            float& savedAggression,
            bool& aggressionOverridden,
            const char* reason = nullptr);
        void ClearAllDefeatedEnemyMirrors(const char* reason = nullptr);
    }
}
