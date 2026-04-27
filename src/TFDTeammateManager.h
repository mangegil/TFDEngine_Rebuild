#pragma once

#include <cstddef>
#include <functional>
#include <vector>

namespace RE
{
    class Actor;
}

namespace TFD::TeammateManager
{

    struct FollowerResolution
    {
        RE::Actor* standing{ nullptr };
        RE::Actor* downed{ nullptr };
    };

    struct RuntimeProviders
    {
        std::function<bool(RE::Actor*)> hasAllyBleedLock;
        std::function<bool(RE::Actor*)> isBleedingOutActor;
        std::function<bool(RE::Actor*)> isDialogueCapableDefeatedEnemy;
        std::function<double(RE::Actor*)> getDefeatedEnemyRemainingSeconds;
        std::function<void(RE::Actor*, double, const char*)> suppressDefeatedReentry;
        std::function<void(RE::Actor*, const char*, bool)> releaseBleedLock;
        std::function<void(RE::Actor*, float, float, float, float, float, const char*)> restoreActorHealthToSafePct;
        std::function<RE::Actor*()> resolvePendingDefeatedDialogueTarget;
        std::function<void()> clearPendingDefeatedDialogueTarget;
        std::function<void(RE::Actor*)> setPendingDefeatedDialogueTarget;
        std::function<bool(RE::Actor*, float)> reviveDownedAlly;
    };

    void Install();
    void Shutdown();
    void SyncNow();
    bool RegisterOrRefreshTeammateNow(RE::Actor* actor, const char* reason = nullptr);

    std::size_t RestoreNow();

    void InstallRuntimeProviders(RuntimeProviders providers);
    void ResetRuntimeProviders();

    bool IsActiveFollowerActor(RE::Actor* actor);
    std::vector<RE::Actor*> CollectRegisteredTeammates();
    std::vector<RE::Actor*> CollectKnownTeammates(float radius);
    FollowerResolution ResolveFollowerCandidates(float radius);
    std::vector<RE::Actor*> CollectStandingFollowers(float radius);
    void RecoverVictoryTeammates();
    bool ReviveDownedAlly(RE::Actor* actor, float targetHealthPct = 55.0f);
    void SetPendingDefeatedDialogueTarget(RE::Actor* actor);
    bool RecruitDefeatedHumanoidAsTeammate(RE::Actor* actor);

    bool IsCreatureCompanion(RE::Actor* actor);
    double GetRemainingCreatureCompanionHours(RE::Actor* actor);
    bool ReleaseCreatureCompanion(RE::Actor* actor);
}
