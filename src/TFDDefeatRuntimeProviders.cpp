#include "TFDDefeatRuntimeProviders.h"

#include "TFDTeammateManager.h"
#include "TFDHostilityController.h"

namespace TFD::DefeatRuntimeProviders
{
    void Install(const InstallHooks& hooks)
    {
        TFD::TeammateManager::RuntimeProviders teammateRuntimeProviders{};
        teammateRuntimeProviders.hasAllyBleedLock = hooks.teammateHasAllyBleedLock;
        teammateRuntimeProviders.isBleedingOutActor = hooks.teammateIsBleedingOutActor;
        teammateRuntimeProviders.isDialogueCapableDefeatedEnemy = hooks.teammateIsDialogueCapableDefeatedEnemy;
        teammateRuntimeProviders.getDefeatedEnemyRemainingSeconds = hooks.teammateGetDefeatedEnemyRemainingSeconds;
        teammateRuntimeProviders.suppressDefeatedReentry = hooks.teammateSuppressDefeatedReentry;
        teammateRuntimeProviders.releaseBleedLock = hooks.teammateReleaseBleedLock;
        teammateRuntimeProviders.restoreActorHealthToSafePct = hooks.teammateRestoreActorHealthToSafePct;
        teammateRuntimeProviders.resolvePendingDefeatedDialogueTarget = hooks.teammateResolvePendingDefeatedDialogueTarget;
        teammateRuntimeProviders.clearPendingDefeatedDialogueTarget = hooks.teammateClearPendingDefeatedDialogueTarget;
        teammateRuntimeProviders.setPendingDefeatedDialogueTarget = hooks.teammateSetPendingDefeatedDialogueTarget;
        teammateRuntimeProviders.reviveDownedAlly = hooks.teammateReviveDownedAlly;
        TFD::TeammateManager::InstallRuntimeProviders(std::move(teammateRuntimeProviders));

        TFD::HostilityController::InstallBleedTruceRuntimeProviders(TFD::HostilityController::BleedTruceRuntimeProviders{
            hooks.hostilityStartBleedTruceSessionForSpeaker,
            hooks.hostilityReleaseBleedTruceSession
        });

        TFD::Tame::RuntimeProviders tameRuntimeProviders{};
        tameRuntimeProviders.isCreatureDefeatedEnemy = hooks.tameIsCreatureDefeatedEnemy;
        tameRuntimeProviders.getDefeatedEnemyRemainingSeconds = hooks.tameGetDefeatedEnemyRemainingSeconds;
        tameRuntimeProviders.suppressDefeatedReentry = hooks.tameSuppressDefeatedReentry;
        tameRuntimeProviders.releaseBleedLock = hooks.tameReleaseBleedLock;
        tameRuntimeProviders.restoreActorHealthToSafePct = hooks.tameRestoreActorHealthToSafePct;
        tameRuntimeProviders.releaseBleedNoSpeakerTameSession = hooks.tameReleaseBleedNoSpeakerTameSession;
        tameRuntimeProviders.tryEnsureBleedNoSpeakerTameSession = hooks.tameTryEnsureBleedNoSpeakerTameSession;
        TFD::Tame::InstallRuntimeProviders(std::move(tameRuntimeProviders));
    }
}
