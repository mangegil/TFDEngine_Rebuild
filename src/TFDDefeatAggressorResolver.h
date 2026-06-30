#pragma once

#include <chrono>
#include <functional>

#include <RE/Skyrim.h>

namespace TFD::DefeatAggressorResolver
{
	struct Dependencies
	{
		std::function<RE::PlayerCharacter*()> getPlayer;
		std::function<bool(RE::Actor*)> isActiveFollowerActor;
		std::function<bool(RE::Actor*)> isStandingEnemyThresholdActor;
		std::function<bool(RE::Actor*)> isObserverAlly;
	};

	void InstallProvider(Dependencies dependencies);
	void ShutdownProvider();
	bool HasProvider();

	bool IsCaptiveSupportedAggressor(RE::Actor* actor);
	bool IsCombatSupportedAggressor(RE::Actor* actor);
	bool IsReasonableCombatAggressor(RE::Actor* actor, RE::Actor* player, float maxDist, float* outDistance = nullptr);
	bool IsBleedCrowdSupportedAggressor(RE::Actor* actor);

	RE::Actor* FindBestAggressor(float radius);
	RE::Actor* ResolveAggressor();

	void RememberAggressor(RE::Actor* actor);
	void ClearLastAggressor();
	void SetLastAggressor(RE::Actor* actor);
	RE::Actor* ResolveLastAggressor();
	bool IsLastAggressor(RE::Actor* actor);

	void NoteEnemyTargetingPlayer(RE::Actor* actor);
	void ClearLastEnemyTargetingPlayer();
	bool HasRecentEnemyTargetingPlayer(double maxAgeSec);
	RE::Actor* ResolveLastEnemyTargetingPlayer(float radius = 0.0f, double maxAgeSec = 15.0);
	RE::Actor* ResolveCachedPlayerOverkillAttacker();

	void ResetForLoad();
}
