#pragma once

#include <chrono>
#include <cstdint>
#include <unordered_set>

#include <RE/Skyrim.h>

namespace TFD::DefeatBattleObserveState
{
	struct ObserverState
	{
		bool hadValidObservedEnemy = false;
		std::unordered_set<RE::FormID> allyIds{};
		std::unordered_set<RE::FormID> enemyIds{};
	};

	class CommitScope
	{
	public:
		CommitScope();
		~CommitScope();
	};

	bool& Pending();
	std::chrono::steady_clock::time_point& PendingUntil();
	std::chrono::steady_clock::time_point& PendingLastRedirect();
	int& PendingEmptyEnemyTicks();
	int& PendingEmptyAllyTicks();

	bool& Active();
	std::chrono::steady_clock::time_point& ActiveSince();
	std::chrono::steady_clock::time_point& ActiveLastRedirect();
	int& ActiveEmptyEnemyTicks();
	int& ActiveEmptyAllyTicks();

	RE::ActorHandle& PreferredEnemy();
	ObserverState& Observer();

	bool IsPending();
	bool IsActive();
	bool IsTrackedEnemy(RE::Actor* actor);
	bool HadValidObservedEnemy();
	bool IsObservedCombatCommitInProgress();

	void ResetFlags();
	void ResetTracking();
	void ResetAll();
}
