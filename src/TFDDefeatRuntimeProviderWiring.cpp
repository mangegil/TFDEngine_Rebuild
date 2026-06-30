#include "TFDDefeatRuntimeProviderWiring.h"

#include <RE/Skyrim.h>

#include "TFDActor.h"
#include "TFDBleedLockState.h"
#include "TFDBleedout.h"
#include "TFDTeammateManager.h"
#include "TFDVictory.h"

namespace TFD::DefeatRuntimeProviderWiring
{
	namespace
	{
		using BleedLockKind = TFD::BleedLockState::Kind;

		RE::Actor* Player()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

		bool IsActorBleedingOut(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			if (auto* state = actor->AsActorState()) {
				return state->IsBleedingOut();
			}
			return false;
		}

		bool HasAllyBleedLock(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			auto& bleedLocks = TFD::BleedLockState::Entries();
			auto it = bleedLocks.find(actor->GetFormID());
			return it != bleedLocks.end() && it->second.kind == BleedLockKind::Ally;
		}
	}

	TFD::DefeatRuntimeProviders::InstallHooks BuildHooks()
	{
		TFD::DefeatRuntimeProviders::InstallHooks hooks{};
		hooks.teammateHasAllyBleedLock = [](RE::Actor* actor) { return HasAllyBleedLock(actor); };
		hooks.teammateIsBleedingOutActor = [](RE::Actor* actor) { return IsActorBleedingOut(actor); };
		hooks.teammateReviveDownedAlly = [](RE::Actor* actor, float targetHealthPct) { return TFD::TeammateManager::ReviveDownedAlly(actor, targetHealthPct); };
		hooks.hostilityStartBleedTruceSessionForSpeaker = [](RE::Actor* player, RE::Actor* speaker, const char* reason) {
			return TFD::Bleedout::StartTruceSessionForSpeaker(player, speaker, reason, TFD::Bleedout::RuntimeHost::BuildStateRefs(), TFD::Bleedout::RuntimeHost::BuildHandlers());
			};
		hooks.hostilityReleaseBleedTruceSession = [](TFD::Tame::ReleaseReason reason) { TFD::Bleedout::ReleaseTruceSession(reason); };
		hooks.tameIsCreatureDefeatedEnemy = [](RE::Actor* actor) { return TFD::Actor::Ops::IsCreatureDefeatedEnemy(actor); };
		hooks.tameGetDefeatedEnemyRemainingSeconds = [](RE::Actor* actor) { return TFD::Actor::Ops::GetDefeatedEnemyRemainingSeconds(actor); };
		hooks.tameSuppressDefeatedReentry = [](RE::Actor* actor, double seconds, const char* reason) { TFD::Actor::Ops::SuppressDefeatedEnemyReentry(actor, seconds, reason); };
		hooks.tameReleaseBleedLock = [](RE::Actor* actor, const char* reason, bool playGetUp) { (void)TFD::Victory::ReleaseManagedEnemy(actor, reason ? reason : "tame_release", playGetUp); };
		hooks.tameRestoreActorHealthToSafePct = [](RE::Actor* actor, float thresholdPct, float bonusPct, float minSafePct, float maxSafePct, float minAbsHp, const char* reason) {
			TFD::Victory::RestoreActorHealthToSafePct(actor, thresholdPct, bonusPct, minSafePct, maxSafePct, minAbsHp, reason ? reason : "tame_recover");
			};
		hooks.tameReleaseBleedNoSpeakerTameSession = [](const char* reason) { TFD::Bleedout::ReleaseNoSpeakerTameSession(reason); };
		hooks.tameTryEnsureBleedNoSpeakerTameSession = [](const std::vector<RE::Actor*>& actors, const char* reason) {
			auto* player = Player();
			if (!player) {
				return false;
			}
			return TFD::Bleedout::TryEnsureNoSpeakerTameSession(actors, player, reason, TFD::Bleedout::RuntimeHost::BuildHandlers());
			};
		return hooks;
	}

	void Install()
	{
		TFD::DefeatRuntimeProviders::Install(BuildHooks());
	}
}
