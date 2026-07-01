#pragma once

#include <cstdint>
#include <vector>

namespace RE
{
	class Actor;
	class PlayerCharacter;
}

namespace TFD::DefeatActorQueries
{
	struct FollowerResolution
	{
		RE::Actor* standing{ nullptr };
		RE::Actor* downed{ nullptr };
	};

	RE::PlayerCharacter* Player();
	std::uint32_t ResolveBleedFlowActorFormID();

	bool ActorHasLineOfSightToPlayer(RE::Actor* actor, RE::Actor* player);
	bool IsBleedSpaceCompatible(RE::Actor* actor, RE::Actor* player);
	bool IsActorCloseAndFront(RE::Actor* actor, RE::Actor* player, float maxDist);

	bool IsObserverAlly(RE::Actor* actor);
	bool IsTrackedDefeatedEnemy(RE::Actor* actor);
	bool IsValidBleedBattleEnemyRosterActor(RE::Actor* actor, RE::Actor* player);
	bool IsActorBleedingOut(RE::Actor* actor);
	float GetActorHealthPct(RE::Actor* actor);
	bool HasActiveBleedLock(RE::Actor* actor);
	bool IsPlayerBleedLockActive(RE::Actor* player);
	bool IsActorDownByThreshold(RE::Actor* actor, float thresholdPct);
	bool IsStandingAllyThresholdActor(RE::Actor* actor);
	bool IsStandingEnemyThresholdActor(RE::Actor* actor);

	bool IsActiveFollowerActor(RE::Actor* actor);
	RE::Actor* ResolveCurrentCombatTarget(RE::Actor* actor);
	bool IsPlayerSideActorForRouter(RE::Actor* actor, RE::Actor* player);
	bool IsActorActivelyTargetingPlayerSideForRouter(RE::Actor* actor, RE::Actor* player);

	std::vector<RE::Actor*> CollectLiveStandingObservedEnemies(RE::Actor* player, float radius, RE::Actor* preferredEnemy, const std::vector<RE::Actor*>& allies);
	FollowerResolution ResolveFollowerCandidates(float radius);
	std::vector<RE::Actor*> CollectBleedStandingFollowers(float radius);
	bool HasPlayerBleedLock();
}
