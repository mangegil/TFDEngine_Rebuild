#include "TFDPlayerDownRouterWiring.h"

namespace TFD::PlayerDownRouterWiring
{
	TFD::PlayerDownRouter::DispatchHandlers BuildDispatchHandlers(const DispatchContext& context)
	{
		TFD::PlayerDownRouter::DispatchHandlers handlers{};
		handlers.rememberAggressor = [context](RE::Actor* actor) {
			if (context.rememberAggressor) {
				context.rememberAggressor(actor);
			}
		};
		handlers.findBleedoutSpeaker = [context](float scanRadius, float maxDistance, RE::Actor* preferred) -> RE::Actor* {
			return context.findBleedoutSpeaker ? context.findBleedoutSpeaker(scanRadius, maxDistance, preferred) : nullptr;
		};
		handlers.startBleedWindow = [context](RE::Actor* player, RE::Actor* speaker) {
			if (context.startBleedWindow) {
				context.startBleedWindow(player, speaker);
			}
		};
		handlers.startBattleObservePending = [context](RE::Actor* player) -> bool {
			return context.startBattleObservePending ? context.startBattleObservePending(player) : false;
		};
		handlers.setGraceSeconds = [context](int seconds) {
			if (context.setGraceSeconds) {
				context.setGraceSeconds(seconds);
			}
		};
		return handlers;
	}

	TFD::PlayerDownRouter::NoThreatFallbackHandlers BuildNoThreatHandlers(const NoThreatContext& context)
	{
		TFD::PlayerDownRouter::NoThreatFallbackHandlers handlers{};
		handlers.hasTerminalCommit = [context]() -> bool { return context.hasTerminalCommit ? context.hasTerminalCommit() : false; };
		handlers.hasCachedRescueDestination = [context]() -> bool { return context.hasCachedRescueDestination ? context.hasCachedRescueDestination() : false; };
		handlers.hasPlayerBleedLock = [context]() -> bool { return context.hasPlayerBleedLock ? context.hasPlayerBleedLock() : false; };
		handlers.enterPlayerBleedLock = [context](RE::Actor* player, float thresholdPct, const char* reason) {
			if (context.enterPlayerBleedLock) {
				context.enterPlayerBleedLock(player, thresholdPct, reason);
			}
		};
		handlers.releasePlayerBleedLock = [context](const char* reason, bool playGetUp) {
			if (context.releasePlayerBleedLock) {
				context.releasePlayerBleedLock(reason, playGetUp);
			}
		};
		handlers.setPlayerBleedImmune = [context](bool immune) {
			if (context.setPlayerBleedImmune) {
				context.setPlayerBleedImmune(immune);
			}
		};
		handlers.preparePlayer = [context](RE::Actor* player) {
			if (context.preparePlayer) {
				context.preparePlayer(player);
			}
		};
		handlers.executeNoMarkerFallback = [context](const char* reason) -> bool {
			return context.executeNoMarkerFallback ? context.executeNoMarkerFallback(reason) : false;
		};
		return handlers;
	}

	TFD::PlayerDownRouter::PendingRouteHandlers BuildPendingHandlers(const PendingContext& context)
	{
		TFD::PlayerDownRouter::PendingRouteHandlers handlers{};
		handlers.setPlayerBleedImmune = [context](bool immune) {
			if (context.setPlayerBleedImmune) {
				context.setPlayerBleedImmune(immune);
			}
		};
		handlers.preparePlayer = [context](RE::Actor* player) {
			if (context.preparePlayer) {
				context.preparePlayer(player);
			}
		};
		handlers.clampHealth = [context](RE::Actor* player, float minHealth) {
			if (context.clampHealth) {
				context.clampHealth(player, minHealth);
			}
		};
		handlers.resolveSafeFloorHealth = [context](RE::Actor* player, float thresholdPct) -> float {
			return context.resolveSafeFloorHealth ? context.resolveSafeFloorHealth(player, thresholdPct) : 1.0f;
		};
		handlers.hasPlayerBleedOwner = [context]() -> bool { return context.hasPlayerBleedOwner ? context.hasPlayerBleedOwner() : false; };
		handlers.enterPlayerBleedLock = [context](RE::Actor* player, float thresholdPct, const char* reason) {
			if (context.enterPlayerBleedLock) {
				context.enterPlayerBleedLock(player, thresholdPct, reason);
			}
		};
		handlers.isBleedDecisionActive = [context]() -> bool { return context.isBleedDecisionActive ? context.isBleedDecisionActive() : false; };
		handlers.scanThresholdOutcome = [context](RE::Actor* player) -> TFD::PlayerDownRouter::ThresholdScan {
			return context.scanThresholdOutcome ? context.scanThresholdOutcome(player) : TFD::PlayerDownRouter::ThresholdScan{};
		};
		handlers.isObserverAlly = [context](RE::Actor* actor) -> bool { return context.isObserverAlly ? context.isObserverAlly(actor) : false; };
		handlers.rememberAggressor = [context](RE::Actor* actor) {
			if (context.rememberAggressor) {
				context.rememberAggressor(actor);
			}
		};
		handlers.getHealthPct = [context](RE::Actor* actor) -> float { return context.getHealthPct ? context.getHealthPct(actor) : 100.0f; };
		handlers.dispatch = BuildDispatchHandlers(context.dispatchContext);
		return handlers;
	}
}
