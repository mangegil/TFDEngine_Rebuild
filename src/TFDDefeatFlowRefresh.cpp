#include "TFDDefeatFlowRefresh.h"

#include <algorithm>
#include <mutex>
#include <utility>

#include <spdlog/spdlog.h>

#include "TFDFlowController.h"
#include "TFDHostilityController.h"
#include "TFDPleasureRuntime.h"
#include "TFDPostDefeatState.h"
#include "TFDSettings.h"

namespace TFD::DefeatFlowRefresh
{
	namespace
	{
		std::mutex g_providerLock;
		Dependencies g_dependencies{};
		bool g_hasProvider = false;
		bool g_lastRouterCombatContextActive = false;

		Dependencies ResolveDependencies()
		{
			std::scoped_lock lock(g_providerLock);
			return g_dependencies;
		}

		bool IsObservedEnemyTarget(RE::Actor* target, const std::vector<RE::Actor*>& enemies)
		{
			if (!target) {
				return false;
			}

			for (auto* enemy : enemies) {
				if (enemy && enemy == target) {
					return true;
				}
			}
			return false;
		}

		bool HasRealCombatProof(const Dependencies& dependencies, RE::Actor* player, const std::vector<RE::Actor*>& enemies)
		{
			if (!player) {
				return false;
			}

			auto& flow = TFD::FlowController::Controller::GetSingleton();
			if (flow.IsCombatOrBleedRootActive()) {
				return true;
			}
			if (flow.IsCaptiveEscapeContextActive()) {
				return true;
			}

			// Real combat proof must come from actual combat state or a live combat target.
			// Observed enemies alone are only threat/precombat data and must not flip
			// dialogue-facing combat/defeat globals to 1.
			if (player->IsInCombat()) {
				return true;
			}

			auto* playerTarget = dependencies.resolveCurrentCombatTarget ? dependencies.resolveCurrentCombatTarget(player) : nullptr;
			if (IsObservedEnemyTarget(playerTarget, enemies)) {
				return true;
			}

			for (auto* enemy : enemies) {
				if (!enemy) {
					continue;
				}

				if (dependencies.isActorActivelyTargetingPlayerSideForRouter &&
					dependencies.isActorActivelyTargetingPlayerSideForRouter(enemy, player)) {
					return true;
				}
			}

			return false;
		}

		bool IsDefeatCombatContextActive(const Dependencies& dependencies, RE::Actor* player, const std::vector<RE::Actor*>& enemies)
		{
			if (HasRealCombatProof(dependencies, player, enemies)) {
				return true;
			}
			if (dependencies.isPlayerBleedRuntimeActive && dependencies.isPlayerBleedRuntimeActive()) {
				return true;
			}
			if (dependencies.isBattleObserveHold && dependencies.isBattleObserveHold()) {
				return true;
			}
			return false;
		}

		bool IsRouterCombatContextActive(const Dependencies& dependencies, RE::Actor* player, const std::vector<RE::Actor*>& enemies)
		{
			if (!player) {
				return false;
			}

			for (auto* enemy : enemies) {
				if (dependencies.isActorActivelyTargetingPlayerSideForRouter &&
					dependencies.isActorActivelyTargetingPlayerSideForRouter(enemy, player)) {
					return true;
				}
			}

			return false;
		}

		std::vector<RE::Actor*> FilterTemporarilySuppressedEnemiesForDialogueGlobals(
			const std::vector<RE::Actor*>& enemies,
			bool* onlySuppressedEnemies,
			std::size_t* suppressedCount)
		{
			std::vector<RE::Actor*> filtered;
			filtered.reserve(enemies.size());

			std::size_t localSuppressedCount = 0;
			for (auto* enemy : enemies) {
				if (!enemy) {
					continue;
				}
				if (TFD::HostilityController::IsActorTemporarilySuppressed(enemy)) {
					++localSuppressedCount;
					continue;
				}
				filtered.push_back(enemy);
			}

			if (onlySuppressedEnemies) {
				*onlySuppressedEnemies = !enemies.empty() && filtered.empty() && localSuppressedCount > 0;
			}
			if (suppressedCount) {
				*suppressedCount = localSuppressedCount;
			}
			return filtered;
		}
	}

	void InstallProvider(Dependencies dependencies)
	{
		std::scoped_lock lock(g_providerLock);
		g_dependencies = std::move(dependencies);
		g_hasProvider = true;
		spdlog::info("[TFD][DefeatFlowRefresh][P20] provider installed");
	}

	void ShutdownProvider()
	{
		std::scoped_lock lock(g_providerLock);
		g_dependencies = Dependencies{};
		g_hasProvider = false;
		g_lastRouterCombatContextActive = false;
		spdlog::info("[TFD][DefeatFlowRefresh][P20] provider cleared");
	}

	bool HasProvider()
	{
		std::scoped_lock lock(g_providerLock);
		return g_hasProvider;
	}

	void RefreshPostDefeatGlobals()
	{
		const auto dependencies = ResolveDependencies();
		auto* player = dependencies.getPlayer ? dependencies.getPlayer() : nullptr;
		const float sweepRadius = dependencies.getSweepRadius ? dependencies.getSweepRadius() : TFD::Settings::GetSweepRadius();
		const float radius = (std::max)(2200.0f, sweepRadius + 200.0f);
		auto followers = dependencies.collectStandingFollowers ? dependencies.collectStandingFollowers(radius) : std::vector<RE::Actor*>{};
		auto observedEnemies = dependencies.collectObservedEnemies ? dependencies.collectObservedEnemies(player, radius, followers) : std::vector<RE::Actor*>{};

		bool onlySuppressedDialogueEnemies = false;
		std::size_t suppressedEnemyCount = 0;
		auto enemies = FilterTemporarilySuppressedEnemiesForDialogueGlobals(
			observedEnemies,
			&onlySuppressedDialogueEnemies,
			&suppressedEnemyCount);

		const bool defeatContext = IsDefeatCombatContextActive(dependencies, player, enemies);
		const bool routerCombatContext = IsRouterCombatContextActive(dependencies, player, enemies);
		const bool pleasurePassiveLock = TFD::PleasureRuntime::IsPassiveLockActive();
		const bool battleObserveHold = dependencies.isBattleObserveHold ? dependencies.isBattleObserveHold() : false;
		const auto flowSnapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
		const bool playerBleedLockActive = dependencies.isPlayerBleedLockActive ? dependencies.isPlayerBleedLockActive(player) : false;
		const bool playerBleedRuntimeActive = dependencies.isPlayerBleedRuntimeActive ? dependencies.isPlayerBleedRuntimeActive() : false;
		const bool flowOwnsPlayerBleedout =
			flowSnapshot.root == TFD::FlowController::RootFlow::Bleedout ||
			flowSnapshot.gate == TFD::FlowController::DecisionGate::PlayerBleedout ||
			playerBleedRuntimeActive ||
			playerBleedLockActive;

		auto refreshResult = TFD::PostDefeatState::Refresh(TFD::PostDefeatState::RefreshInput{
			.player = player,
			.enemies = std::move(enemies),
			.defeatContext = defeatContext,
			.routerCombatContext = routerCombatContext,
			.onlySuppressedDialogueEnemies = onlySuppressedDialogueEnemies,
			.suppressedEnemyCount = suppressedEnemyCount,
			.pleasurePassiveLock = pleasurePassiveLock,
			.battleObserveHold = battleObserveHold,
			.forcePlayerBleedout = flowOwnsPlayerBleedout,
			.playerBleedLockActive = playerBleedLockActive,
			.playerBleedRuntimeActive = playerBleedRuntimeActive,
			.ownerRootName = TFD::FlowController::Controller::ToString(flowSnapshot.root),
			.ownerGateName = TFD::FlowController::Controller::ToString(flowSnapshot.gate)
			});
		g_lastRouterCombatContextActive = refreshResult.routerCombatContextActive;
	}

	void UpdatePreCombatState()
	{
		// ownership moved to TFDFlowController.cpp
	}

	void ResetRouterCombatContext()
	{
		g_lastRouterCombatContextActive = false;
	}

	bool LastRouterCombatContextActive()
	{
		return g_lastRouterCombatContextActive;
	}
}
