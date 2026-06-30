#include "TFDPlayerDownRouterOutcomeWiring.h"

#include <mutex>
#include <utility>

#include <spdlog/spdlog.h>

namespace TFD::PlayerDownRouterOutcomeWiring
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
		spdlog::info("[TFD][PlayerDownRouter][P13] outcome wiring provider installed");
	}

	void ShutdownProvider()
	{
		std::scoped_lock lock(g_providerLock);
		g_dependencies = Dependencies{};
		g_hasProvider = false;
		spdlog::info("[TFD][PlayerDownRouter][P13] outcome wiring provider cleared");
	}

	bool HasProvider()
	{
		std::scoped_lock lock(g_providerLock);
		return g_hasProvider;
	}

	TFD::PlayerDownRouterWiring::DispatchContext BuildDispatchContext()
	{
		Dependencies dependencies{};
		{
			std::scoped_lock lock(g_providerLock);
			dependencies = g_dependencies;
		}

		TFD::PlayerDownRouterWiring::DispatchContext context{};
		context.rememberAggressor = [dependencies](RE::Actor* actor) {
			if (dependencies.rememberAggressor) {
				dependencies.rememberAggressor(actor);
			}
		};
		context.findBleedoutSpeaker = [dependencies](float scanRadius, float maxDistance, RE::Actor* preferred) -> RE::Actor* {
			return dependencies.findBleedoutSpeaker ? dependencies.findBleedoutSpeaker(scanRadius, maxDistance, preferred) : nullptr;
		};
		context.startBleedWindow = [dependencies](RE::Actor* player, RE::Actor* speaker) {
			if (dependencies.startBleedWindow) {
				dependencies.startBleedWindow(player, speaker);
			}
		};
		context.startBattleObservePending = [dependencies](RE::Actor* player) -> bool {
			return dependencies.startBattleObservePending ? dependencies.startBattleObservePending(player) : false;
		};
		context.setGraceSeconds = [dependencies](int seconds) {
			if (dependencies.setGraceSeconds) {
				dependencies.setGraceSeconds(seconds);
			}
		};
		return context;
	}

	TFD::PlayerDownRouterWiring::NoThreatContext BuildNoThreatContext()
	{
		Dependencies dependencies{};
		{
			std::scoped_lock lock(g_providerLock);
			dependencies = g_dependencies;
		}

		TFD::PlayerDownRouterWiring::NoThreatContext context{};
		context.hasTerminalCommit = [dependencies]() -> bool {
			return dependencies.hasTerminalCommit ? dependencies.hasTerminalCommit() : false;
		};
		context.hasCachedRescueDestination = [dependencies]() -> bool {
			return dependencies.hasCachedRescueDestination ? dependencies.hasCachedRescueDestination() : false;
		};
		context.hasPlayerBleedLock = [dependencies]() -> bool {
			return dependencies.hasPlayerBleedLock ? dependencies.hasPlayerBleedLock() : false;
		};
		context.enterPlayerBleedLock = [dependencies](RE::Actor* player, float thresholdPct, const char* reason) {
			if (dependencies.enterPlayerBleedLock) {
				dependencies.enterPlayerBleedLock(player, thresholdPct, reason);
			}
		};
		context.releasePlayerBleedLock = [dependencies](const char* reason, bool playGetUp) {
			if (dependencies.releasePlayerBleedLock) {
				dependencies.releasePlayerBleedLock(reason, playGetUp);
			}
		};
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
		context.executeNoMarkerFallback = [dependencies](const char* reason) -> bool {
			return dependencies.executeNoMarkerFallback ? dependencies.executeNoMarkerFallback(reason) : false;
		};
		return context;
	}

	TFD::PlayerDownRouter::DispatchHandlers BuildDispatchHandlers()
	{
		return TFD::PlayerDownRouterWiring::BuildDispatchHandlers(BuildDispatchContext());
	}

	TFD::PlayerDownRouter::NoThreatFallbackHandlers BuildNoThreatHandlers()
	{
		return TFD::PlayerDownRouterWiring::BuildNoThreatHandlers(BuildNoThreatContext());
	}
}
