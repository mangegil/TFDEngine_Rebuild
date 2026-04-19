#pragma once

#include <functional>
#include <vector>

#include "TFDTame.h"

namespace RE
{
    class Actor;
}

namespace TFD::DefeatRuntimeProviders
{
    struct InstallHooks
    {
        std::function<bool(RE::Actor*)> teammateHasAllyBleedLock;
        std::function<bool(RE::Actor*)> teammateIsBleedingOutActor;
        std::function<bool(RE::Actor*)> teammateIsDialogueCapableDefeatedEnemy;
        std::function<double(RE::Actor*)> teammateGetDefeatedEnemyRemainingSeconds;
        std::function<void(RE::Actor*, double, const char*)> teammateSuppressDefeatedReentry;
        std::function<void(RE::Actor*, const char*, bool)> teammateReleaseBleedLock;
        std::function<void(RE::Actor*, float, float, float, float, float, const char*)> teammateRestoreActorHealthToSafePct;
        std::function<RE::Actor*()> teammateResolvePendingDefeatedDialogueTarget;
        std::function<void()> teammateClearPendingDefeatedDialogueTarget;
        std::function<void(RE::Actor*)> teammateSetPendingDefeatedDialogueTarget;
        std::function<bool(RE::Actor*, float)> teammateReviveDownedAlly;

        std::function<bool(RE::Actor*, RE::Actor*, const char*)> hostilityStartBleedTruceSessionForSpeaker;
        std::function<void(TFD::Tame::ReleaseReason)> hostilityReleaseBleedTruceSession;

        std::function<bool(RE::Actor*)> tameIsCreatureDefeatedEnemy;
        std::function<double(RE::Actor*)> tameGetDefeatedEnemyRemainingSeconds;
        std::function<void(RE::Actor*, double, const char*)> tameSuppressDefeatedReentry;
        std::function<void(RE::Actor*, const char*, bool)> tameReleaseBleedLock;
        std::function<void(RE::Actor*, float, float, float, float, float, const char*)> tameRestoreActorHealthToSafePct;
        std::function<void(const char*)> tameReleaseBleedNoSpeakerTameSession;
        std::function<bool(const std::vector<RE::Actor*>&, const char*)> tameTryEnsureBleedNoSpeakerTameSession;
    };

    void Install(const InstallHooks& hooks);
}
