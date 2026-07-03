#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace RE
{
    class Actor;
}

namespace SKSE
{
    class SerializationInterface;
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
        std::function<bool(RE::Actor*, float)> reviveDownedAlly;
    };

    void Install();
    void Shutdown();
    void SyncNow();
    std::size_t GetMaxRecruitSlots();
    std::size_t GetRecruitSlotsFree();
    void RefreshRecruitCapacityGlobals(const char* reason = nullptr);
    bool RegisterOrRefreshTeammateNow(RE::Actor* actor, const char* reason = nullptr);
    bool RegisterOrRefreshTeammateNowImmediatePackage(RE::Actor* actor, const char* reason = nullptr);
    bool RegisterOrRefreshTeammateNowDeferredPackage(RE::Actor* actor, const char* reason = nullptr);
    void QueueHumanoidTeammateCatchupAfterLoad(const char* reason = nullptr);
    void ResetHumanoidContractTransientForLoad(const char* reason = nullptr);
    void ClearHumanoidContractStateForLoad(const char* reason = nullptr);
    bool SaveHumanoidContractState(SKSE::SerializationInterface* intfc);
    bool LoadHumanoidContractState(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length);

    void ArmVictoryRecruitVisualRefreshHold(RE::Actor* actor, double seconds = 3.0, const char* reason = nullptr);
    bool IsVictoryRecruitVisualRefreshHoldActor(RE::Actor* actor);
    void ClearVictoryRecruitVisualRefreshHold(RE::Actor* actor, const char* reason = nullptr);

    std::size_t RestoreNow();

    void InstallRuntimeProviders(RuntimeProviders providers);
    void ResetRuntimeProviders();

    bool IsActiveFollowerActor(RE::Actor* actor);
    bool IsPlayerSideTeammateActor(RE::Actor* actor);
    bool IsTFDManagedTeammateActor(RE::Actor* actor);
    std::vector<RE::Actor*> CollectRegisteredTeammates();
    std::vector<RE::Actor*> CollectKnownTeammates(float radius);
    FollowerResolution ResolveFollowerCandidates(float radius);
    std::vector<RE::Actor*> CollectStandingFollowers(float radius);
    bool ReviveDownedAlly(RE::Actor* actor, float targetHealthPct = 55.0f);
    bool ExtendHumanoidTeammateContract(RE::Actor* actor, const char* reason = nullptr);
    bool RestoreHumanoidTeammateHealthWithPotion(RE::Actor* actor);
    bool RestoreHumanoidTeammateHealthWithPleasure(RE::Actor* actor);
    bool TerminateHumanoidTeammateContract(RE::Actor* actor);

    void ArmDownedTeammateRecoveryDialogueHold(RE::Actor* actor, double seconds = 10.0, const char* reason = nullptr);
    bool IsDownedTeammateRecoveryDialogueHoldActor(RE::Actor* actor);
    bool IsDownedTeammateRecoveryDialogueHoldActive();
    void ClearDownedTeammateRecoveryDialogueHold(RE::Actor* actor, const char* reason = nullptr);

    bool IsCreatureCompanion(RE::Actor* actor);
    double GetRemainingCreatureCompanionHours(RE::Actor* actor);
    bool ReleaseCreatureCompanion(RE::Actor* actor);
}
