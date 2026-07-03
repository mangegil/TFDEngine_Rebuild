#include "TFDBleedoutDialogueRuntime.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <spdlog/spdlog.h>

#include "TFDActor.h"

namespace TFD::BleedoutDialogueRuntime
{
	namespace
	{
		float Distance3D(const RE::NiPoint3& a, const RE::NiPoint3& b)
		{
			const float dx = a.x - b.x;
			const float dy = a.y - b.y;
			const float dz = a.z - b.z;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		}

		TFD::Bleedout::SpeakerLogicHandlers BuildSpeakerHandlers(const Context& context, bool preserveAssigned = false)
		{
			if (context.buildSpeakerHandlers) {
				return context.buildSpeakerHandlers(preserveAssigned);
			}
			return TFD::Bleedout::SpeakerLogicHandlers{};
		}

		RE::Actor* GetPlayer(const Context& context)
		{
			return context.getPlayer ? context.getPlayer() : nullptr;
		}
	}

	RE::Actor* FindBestSpeaker(float radius, float maxDist, RE::Actor* preferred, const Context& context)
	{
		auto* player = GetPlayer(context);
		if (!player) {
			return nullptr;
		}

		const auto handlers = BuildSpeakerHandlers(context, false);
		const float scanRadius = (std::max)(12000.0f, (std::max)(radius, maxDist));
		auto isCandidate = [&](RE::Actor* actor, float* outDistance = nullptr) -> bool {
			if (outDistance) {
				*outDistance = -1.0f;
			}
			if (!actor || actor == player || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
				return false;
			}
			if (!handlers.isCaptiveSupportedAggressor || !handlers.isCaptiveSupportedAggressor(actor)) {
				return false;
			}
			if (!handlers.isBleedSpaceCompatible || !handlers.isBleedSpaceCompatible(actor, player)) {
				return false;
			}

			const float dist = Distance3D(actor->GetPosition(), player->GetPosition());
			if (outDistance) {
				*outDistance = dist;
			}
			if (dist > scanRadius) {
				return false;
			}

			auto* currentTarget = handlers.resolveCurrentCombatTarget ? handlers.resolveCurrentCombatTarget(actor) : nullptr;
			const bool targetsPlayer = currentTarget == player;
			const bool targetsFollower = currentTarget && handlers.isActiveFollowerActor && handlers.isActiveFollowerActor(currentTarget);
			const bool preferredActor = preferred && actor == preferred;
			const bool rememberedActor = handlers.resolveLastAggressor && handlers.resolveLastAggressor() == actor;
			const bool engaged = targetsPlayer || targetsFollower || actor->IsHostileToActor(player) || actor->IsInCombat() || preferredActor || rememberedActor;
			if (!engaged) {
				return false;
			}

			const bool standing = handlers.isStandingEnemyThresholdActor && handlers.isStandingEnemyThresholdActor(actor);
			if (standing) {
				return true;
			}

			// P32Q: During Captive Escape killmove-veto, the actual attacker can be
			// below the normal enemy defeat threshold while still actively executing
			// the hit that downed the player. Allow that preferred/remembered attacker
			// if it is directly targeting the player, instead of replacing it with a
			// random coalition speaker nearby.
			return targetsPlayer && (preferredActor || rememberedActor);
		};

		float preferredDist = -1.0f;
		if (preferred && isCandidate(preferred, &preferredDist)) {
			spdlog::info("[TFD][BleedoutDialogueRuntime][R24] bleed speaker preferred actor={:08X} dist={:.1f}",
				preferred->GetFormID(), preferredDist);
			return preferred;
		}

		auto snapshot = TFD::Actor::BuildSnapshot(scanRadius, false);

		RE::Actor* best = nullptr;
		float bestScore = std::numeric_limits<float>::max();
		RE::Actor* last = handlers.resolveLastAggressor ? handlers.resolveLastAggressor() : nullptr;
		for (const auto& info : snapshot.actors) {
			auto* actor = info.get();
			float dist = -1.0f;
			if (!isCandidate(actor, &dist)) {
				continue;
			}

			auto* currentTarget = handlers.resolveCurrentCombatTarget ? handlers.resolveCurrentCombatTarget(actor) : nullptr;
			const bool targetsPlayer = currentTarget == player;
			const bool targetsFollower = currentTarget && handlers.isActiveFollowerActor && handlers.isActiveFollowerActor(currentTarget);
			const bool hostile = info.hostileToPlayer || actor->IsHostileToActor(player);
			const bool inCombat = info.inCombat || actor->IsInCombat();
			const bool front = handlers.isActorCloseAndFront && handlers.isActorCloseAndFront(actor, player, 448.0f);
			const bool los = handlers.hasLineOfSightToPlayer && handlers.hasLineOfSightToPlayer(actor, player);

			float score = dist;
			if (targetsPlayer) score -= 1200.0f;
			if (targetsFollower) score -= 850.0f;
			if (hostile) score -= 360.0f;
			if (inCombat) score -= 250.0f;
			if (front) score -= 160.0f;
			if (los) score -= 80.0f;
			if (actor == preferred) score -= 600.0f;
			if (last && last == actor) score -= 300.0f;

			if (score < bestScore) {
				bestScore = score;
				best = actor;
			}
		}

		if (best) {
			spdlog::info("[TFD][BleedoutDialogueRuntime][P32Q] bleed speaker scan radius={:.1f} best={:08X} score={:.1f}",
				scanRadius, best->GetFormID(), bestScore);
			return best;
		}

		if (preferred) {
			if (const auto* preferredInfo = TFD::Actor::FindActorInfo(snapshot, preferred); preferredInfo && preferredInfo->coalitionID >= 0) {
				if (auto* coalitionSpeaker = TFD::Actor::ResolveSpeakerCandidate(snapshot, preferredInfo->coalitionID)) {
					float coalitionDist = -1.0f;
					if (isCandidate(coalitionSpeaker, &coalitionDist)) {
						spdlog::info("[TFD][BleedoutDialogueRuntime][P32Q] bleed speaker preferred coalition fallback actor={:08X} dist={:.1f}",
							coalitionSpeaker->GetFormID(), coalitionDist);
						return coalitionSpeaker;
					}
				}
			}
		}

		if (snapshot.winningCoalitionCandidateID >= 0) {
			if (auto* coalitionSpeaker = TFD::Actor::ResolveSpeakerCandidate(snapshot, snapshot.winningCoalitionCandidateID)) {
				float coalitionDist = -1.0f;
				if (isCandidate(coalitionSpeaker, &coalitionDist)) {
					spdlog::info("[TFD][BleedoutDialogueRuntime][P32Q] bleed speaker winning coalition fallback actor={:08X} dist={:.1f}",
						coalitionSpeaker->GetFormID(), coalitionDist);
					return coalitionSpeaker;
				}
			}
		}

		spdlog::info("[TFD][BleedoutDialogueRuntime][P32Q] bleed speaker scan radius={:.1f} best=00000000 score=0.0",
			scanRadius);
		return nullptr;
	}

	bool IsReasonableSpeaker(RE::Actor* actor, RE::Actor* player, float maxDist, float* outDistance, const Context& context)
	{
		if (outDistance) {
			*outDistance = -1.0f;
		}
		if (!actor || !player) {
			return false;
		}
		const auto handlers = BuildSpeakerHandlers(context, false);
		if (!handlers.isCaptiveSupportedAggressor || !handlers.isCaptiveSupportedAggressor(actor)) {
			return false;
		}
		if (!handlers.isBleedSpaceCompatible || !handlers.isBleedSpaceCompatible(actor, player)) {
			return false;
		}

		const float dist = Distance3D(actor->GetPosition(), player->GetPosition());
		if (outDistance) {
			*outDistance = dist;
		}
		if (dist > (std::max)(12000.0f, maxDist)) {
			return false;
		}

		auto* currentTarget = handlers.resolveCurrentCombatTarget ? handlers.resolveCurrentCombatTarget(actor) : nullptr;
		if (currentTarget == player) {
			return true;
		}
		if (currentTarget && handlers.isActiveFollowerActor && handlers.isActiveFollowerActor(currentTarget)) {
			return true;
		}
		return actor->IsHostileToActor(player) || actor->IsInCombat();
	}

	bool CanUseAggressorForGreet(RE::Actor* player, RE::Actor* aggressor, float& outDistance, const Context& context)
	{
		outDistance = -1.0f;
		if (!player || !aggressor || aggressor == player || aggressor->IsDead(false) || aggressor->IsDisabled() || !aggressor->Is3DLoaded()) {
			return false;
		}
		const auto handlers = BuildSpeakerHandlers(context, false);
		if (!handlers.isCaptiveSupportedAggressor || !handlers.isCaptiveSupportedAggressor(aggressor)) {
			return false;
		}
		if (!handlers.isBleedSpaceCompatible || !handlers.isBleedSpaceCompatible(aggressor, player)) {
			return false;
		}

		outDistance = Distance3D(aggressor->GetPosition(), player->GetPosition());

		const auto currentSpeaker = context.currentSpeakerID ? context.currentSpeakerID() : 0u;
		const bool ownsFlow = context.ownsCurrentFlow ? context.ownsCurrentFlow() : false;
		if (ownsFlow && currentSpeaker == aggressor->GetFormID()) {
			return outDistance <= 2048.0f;
		}

		return IsReasonableSpeaker(aggressor, player, 12000.0f, &outDistance, context);
	}

	void ApplyOverdrive(RE::Actor* player, RE::Actor* speaker, const char* reason, bool restartDialogue, const Context& context)
	{
		const auto state = context.buildRuntimeStateRefs ? context.buildRuntimeStateRefs() : TFD::Bleedout::RuntimeHostStateRefs{};
		const auto handlers = context.buildRuntimeHostHandlers ? context.buildRuntimeHostHandlers() : TFD::Bleedout::RuntimeHostHandlers{};
		TFD::Bleedout::ApplyDialogueOverdrive(player, speaker, reason, restartDialogue, state, handlers);
	}
}
