#pragma once

#include <functional>
#include <vector>

#include <RE/Skyrim.h>

namespace TFD::DefeatFlowRefresh
{
	struct Dependencies
	{
		std::function<RE::Actor*()> getPlayer;
		std::function<float()> getSweepRadius;
		std::function<std::vector<RE::Actor*>(float)> collectStandingFollowers;
		std::function<std::vector<RE::Actor*>(RE::Actor*, float, const std::vector<RE::Actor*>&)> collectObservedEnemies;
		std::function<RE::Actor*(RE::Actor*)> resolveCurrentCombatTarget;
		std::function<bool(RE::Actor*, RE::Actor*)> isActorActivelyTargetingPlayerSideForRouter;
		std::function<bool()> isPlayerBleedRuntimeActive;
		std::function<bool()> isBattleObserveHold;
		std::function<bool(RE::Actor*)> isPlayerBleedLockActive;
	};

	void InstallProvider(Dependencies dependencies);
	void ShutdownProvider();
	bool HasProvider();

	void RefreshPostDefeatGlobals();
	void UpdatePreCombatState();
	void ResetRouterCombatContext();
	bool LastRouterCombatContextActive();
}
