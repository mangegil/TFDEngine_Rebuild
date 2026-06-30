#include "TFDPlayerDownRouterPendingWiring.h"

#include <mutex>
#include <utility>

#include <spdlog/spdlog.h>

namespace TFD::PlayerDownRouterPendingWiring
{
	namespace
	{
		std::mutex g_providerLock;
		Dependencies g_dependencies{};
		bool g_hasProvider = false;
	}

	void InstallProvider(Dependencies dependencies)
	{
		std::scoped_lock lock(g_providerLock);
		g_dependencies = std::move(dependencies);
		g_hasProvider = true;
		spdlog::info("[TFD][PlayerDownRouter][P12] pending route wiring provider installed");
	}

	void ShutdownProvider()
	{
		std::scoped_lock lock(g_providerLock);
		g_dependencies = Dependencies{};
		g_hasProvider = false;
		spdlog::info("[TFD][PlayerDownRouter][P12] pending route wiring provider cleared");
	}

	bool HasProvider()
	{
		std::scoped_lock lock(g_providerLock);
		return g_hasProvider;
	}

	TFD::PlayerDownRouterWiring::PendingContext BuildPendingContext()
	{
		Dependencies dependencies{};
		{
			std::scoped_lock lock(g_providerLock);
			dependencies = g_dependencies;
		}

		TFD::PlayerDownRouterWiring::PendingContext context{};
		context.setPlayerBleedImmune = [dependencies](bool immune) {
			if (dependencies.setPlayerBleedImmune) {
				dependencies.setPlayerBleedImmune(immune);
			}
		};
		context.preparePlayer = [dependencies](RE::Actor* player) {
			if (dependencies.preparePlayer) {
				dependencies.preparePlayer(player);
			}
		};
		context.clampHealth = [dependencies](RE::Actor* player, float minHealth) {
			if (dependencies.clampHealth) {
				dependencies.clampHealth(player, minHealth);
			}
		};
		context.resolveSafeFloorHealth = [dependencies](RE::Actor* player, float thresholdPct) -> float {
			return dependencies.resolveSafeFloorHealth ? dependencies.resolveSafeFloorHealth(player, thresholdPct) : 1.0f;
		};
		context.hasPlayerBleedOwner = [dependencies]() -> bool {
			return dependencies.hasPlayerBleedOwner ? dependencies.hasPlayerBleedOwner() : false;
		};
		context.enterPlayerBleedLock = [dependencies](RE::Actor* player, float thresholdPct, const char* reason) {
			if (dependencies.enterPlayerBleedLock) {
				dependencies.enterPlayerBleedLock(player, thresholdPct, reason);
			}
		};
		context.isBleedDecisionActive = [dependencies]() -> bool {
			return dependencies.isBleedDecisionActive ? dependencies.isBleedDecisionActive() : false;
		};
		context.scanThresholdOutcome = [dependencies](RE::Actor* player) -> TFD::PlayerDownRouter::ThresholdScan {
			return dependencies.scanThresholdOutcome ? dependencies.scanThresholdOutcome(player) : TFD::PlayerDownRouter::ThresholdScan{};
		};
		context.isObserverAlly = [dependencies](RE::Actor* actor) -> bool {
			return dependencies.isObserverAlly ? dependencies.isObserverAlly(actor) : false;
		};
		context.rememberAggressor = [dependencies](RE::Actor* actor) {
			if (dependencies.rememberAggressor) {
				dependencies.rememberAggressor(actor);
			}
		};
		context.getHealthPct = [dependencies](RE::Actor* actor) -> float {
			return dependencies.getHealthPct ? dependencies.getHealthPct(actor) : 100.0f;
		};
		context.dispatchContext = dependencies.buildDispatchContext ? dependencies.buildDispatchContext() : TFD::PlayerDownRouterWiring::DispatchContext{};
		return context;
	}

	void TickPendingOverkillRoute()
	{
		Dependencies dependencies{};
		bool hasProvider = false;
		{
			std::scoped_lock lock(g_providerLock);
			dependencies = g_dependencies;
			hasProvider = g_hasProvider;
		}

		if (!TFD::PlayerDownRouter::HasPendingOverkillRoute()) {
			return;
		}

		if (!hasProvider || !dependencies.getPlayer) {
			spdlog::warn("[TFD][PlayerDownRouter][P12] pending overkill route tick skipped reason=no_provider");
			return;
		}

		(void)TFD::PlayerDownRouter::TickPendingOverkillRoute(
			dependencies.getPlayer(),
			TFD::PlayerDownRouterWiring::BuildPendingHandlers(BuildPendingContext()));
	}
}
