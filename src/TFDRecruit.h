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
        PreCombat = 1,
        InCombat = 2,
        Bleedout = 3,
        Captive = 4,
        // Values 5 and 7 are intentionally unused. Manual defeated interaction
        // owns its future recruit handoff outside this generic source enum.
        Pleasure = 6,
        Teammate = 8,
        Dialogue = 9
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
        // R410A: Victory Recruit needs a pre-3D-refresh dehostile pass that
        // removes hostility without entering the teammate alias/package system yet.
        // Keep these true for normal recruit commits.
        bool ensureTeammateFaction{ true };
        bool ensureFollowerAnchorFactions{ true };
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

    // Restore actor values/factions touched by native recruit conversion before a save swap.
    // This prevents converted enemies from leaking pacified/player-side runtime state into older saves.
    void RestoreRuntimeModifiedActorsForLoad(const char* reason = nullptr);

    // Re-apply the same runtime restore after the loaded world is ready, then refresh faction reaction
    // and combat detection for actors that are not legitimate TFD teammates in the loaded save.
    void RefreshRuntimeRestoredActorsAfterLoad(const char* reason = nullptr);
}
