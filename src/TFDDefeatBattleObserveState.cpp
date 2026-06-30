#include "TFDDefeatBattleObserveState.h"

namespace TFD::DefeatBattleObserveState
{
	namespace
	{
		bool g_pending = false;
		std::chrono::steady_clock::time_point g_pendingUntil{};
		std::chrono::steady_clock::time_point g_pendingLastRedirect{};
		int g_pendingEmptyEnemyTicks = 0;
		int g_pendingEmptyAllyTicks = 0;

		bool g_active = false;
		std::chrono::steady_clock::time_point g_activeSince{};
		std::chrono::steady_clock::time_point g_activeLastRedirect{};
		int g_activeEmptyEnemyTicks = 0;
		int g_activeEmptyAllyTicks = 0;

		RE::ActorHandle g_preferredEnemy{};
		ObserverState g_observer{};
		thread_local std::uint32_t g_commitDepth = 0;
	}

	CommitScope::CommitScope()
	{
		++g_commitDepth;
	}

	CommitScope::~CommitScope()
	{
		if (g_commitDepth > 0) {
			--g_commitDepth;
		}
	}

	bool& Pending() { return g_pending; }
	std::chrono::steady_clock::time_point& PendingUntil() { return g_pendingUntil; }
	std::chrono::steady_clock::time_point& PendingLastRedirect() { return g_pendingLastRedirect; }
	int& PendingEmptyEnemyTicks() { return g_pendingEmptyEnemyTicks; }
	int& PendingEmptyAllyTicks() { return g_pendingEmptyAllyTicks; }

	bool& Active() { return g_active; }
	std::chrono::steady_clock::time_point& ActiveSince() { return g_activeSince; }
	std::chrono::steady_clock::time_point& ActiveLastRedirect() { return g_activeLastRedirect; }
	int& ActiveEmptyEnemyTicks() { return g_activeEmptyEnemyTicks; }
	int& ActiveEmptyAllyTicks() { return g_activeEmptyAllyTicks; }

	RE::ActorHandle& PreferredEnemy() { return g_preferredEnemy; }
	ObserverState& Observer() { return g_observer; }

	bool IsPending() { return g_pending; }
	bool IsActive() { return g_active; }

	bool IsTrackedEnemy(RE::Actor* actor)
	{
		return actor && g_observer.enemyIds.find(actor->GetFormID()) != g_observer.enemyIds.end();
	}

	bool HadValidObservedEnemy()
	{
		return g_observer.hadValidObservedEnemy;
	}

	bool IsObservedCombatCommitInProgress()
	{
		return g_commitDepth > 0;
	}

	void ResetFlags()
	{
		g_pending = false;
		g_pendingUntil = {};
		g_pendingLastRedirect = {};
		g_pendingEmptyEnemyTicks = 0;
		g_pendingEmptyAllyTicks = 0;
		g_active = false;
		g_activeSince = {};
		g_activeLastRedirect = {};
		g_activeEmptyEnemyTicks = 0;
		g_activeEmptyAllyTicks = 0;
	}

	void ResetTracking()
	{
		g_preferredEnemy.reset();
		g_observer = {};
	}

	void ResetAll()
	{
		ResetFlags();
		ResetTracking();
	}
}
