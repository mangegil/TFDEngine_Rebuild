#pragma once

#include <cstdint>
#include <vector>

namespace RE
{
    class Actor;
    class PlayerCharacter;
}

namespace TFD::Recruit
{
    enum class SourceFlow : std::uint8_t
    {
        Unknown = 0,
        PreCombat,
        InCombat,
        Bleedout,
        Captive,
        Victory,
        Pleasure,
        Defeated,
        Teammate,
        Dialogue
    };

    struct ObserveOptions
    {
        SourceFlow sourceFlow{ SourceFlow::Unknown };
        const char* reason{ nullptr };
        bool detailed{ false };
        bool throttle{ true };
    };

    struct CommitOptions
    {
        SourceFlow sourceFlow{ SourceFlow::Unknown };
        const char* reason{ nullptr };
        bool quarantineHostileFactions{ true };
        bool clearCombat{ true };
        bool evaluatePackage{ true };
        bool detailedLog{ false };
        bool throttleObserve{ true };
        bool ensurePacifyAlliance{ true };
        bool applyRuntimeProfile{ true };
    };

    struct CommitResult
    {
        bool attempted{ false };
        bool skipped{ false };
        bool recruitLikeBefore{ false };
        bool pendingCommitBefore{ false };
        bool rawHostileBefore{ false };
        bool rawHostileAfter{ false };
        unsigned hostileFactionMatchesBefore{ 0 };
        unsigned hostileFactionMatchesAfter{ 0 };
        unsigned removedHostileFactions{ 0 };
        unsigned ensuredStateFactions{ 0 };
        bool playerFactionEnsured{ false };
        bool runtimeProfileApplied{ false };
        bool combatCleared{ false };
    };

    const char* ToString(SourceFlow sourceFlow);

    bool IsRecruitLike(RE::Actor* actor);
    bool HasKnownHostileSourceFaction(RE::Actor* actor);
    bool IsRawHostileToPlayer(RE::Actor* actor, RE::PlayerCharacter* player = nullptr);

    void ObserveRecruitState(RE::Actor* actor, RE::PlayerCharacter* player, const ObserveOptions& options);
    void ObserveRecruitState(RE::Actor* actor, const ObserveOptions& options);

    CommitResult CommitRecruit(RE::Actor* actor, RE::PlayerCharacter* player, const CommitOptions& options);
    CommitResult CommitRecruit(RE::Actor* actor, const CommitOptions& options);
    unsigned CommitRecruitGroup(const std::vector<RE::Actor*>& actors, RE::PlayerCharacter* player, const CommitOptions& options);

    void MarkRecruitCommitPending(RE::Actor* actor, double durationSec, SourceFlow sourceFlow, const char* reason);
    void MarkRecruitCommitPendingGroup(const std::vector<RE::Actor*>& actors, double durationSec, SourceFlow sourceFlow, const char* reason);
    std::vector<RE::Actor*> CollectRecruitCommitPendingActors(SourceFlow sourceFlow, bool includeUnknownSource = false);
    bool IsRecruitCommitPending(RE::Actor* actor);
    void ClearRecruitCommitPending(RE::Actor* actor, const char* reason);
}
