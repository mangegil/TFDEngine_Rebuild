#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

#include <RE/Skyrim.h>

#include "TFDActor.h"

namespace TFD::PlayerDownRouter
{
	struct ThresholdScan
	{
		float scanRadius{ 0.0f };
		RE::Actor* initialAggressor{ nullptr };
		TFD::Actor::Snapshot coalitionSnapshot{};
		std::vector<RE::Actor*> standingFollowers{};
		bool unresolvedBattle{ false };
		bool playerSideStanding{ false };
		bool hostileCoalitionStanding{ false };
	};

	struct ThresholdClassification
	{
		RE::Actor* rememberedAggressor{ nullptr };
		bool observeUnresolvedBattle{ false };
		bool observeStandingFollowers{ false };
	};

	struct PendingOverkillRoute
	{
		bool pending{ false };
		RE::ActorHandle attacker{};
		std::chrono::steady_clock::time_point queuedAt{};
		float thresholdPct{ 0.0f };
		float hpBefore{ 0.0f };
		float originalDamage{ 0.0f };
		float clampedDamage{ 0.0f };
		float blockedDamage{ 0.0f };
		float safeFloorHp{ 0.0f };
	};

	struct DispatchHandlers
	{
		std::function<void(RE::Actor*)> rememberAggressor;
		std::function<RE::Actor*(float scanRadius, float maxDistance, RE::Actor* preferred)> findBleedoutSpeaker;
		std::function<void(RE::Actor* player, RE::Actor* speaker)> startBleedWindow;
		std::function<bool(RE::Actor* player)> startBattleObservePending;
		std::function<void(int seconds)> setGraceSeconds;
	};

	struct NoThreatFallbackHandlers
	{
		std::function<bool()> hasTerminalCommit;
		std::function<bool()> hasCachedRescueDestination;
		std::function<bool()> hasPlayerBleedLock;
		std::function<void(RE::Actor* player, float thresholdPct, const char* reason)> enterPlayerBleedLock;
		std::function<void(const char* reason, bool playGetUp)> releasePlayerBleedLock;
		std::function<void(bool immune)> setPlayerBleedImmune;
		std::function<void(RE::Actor* player)> preparePlayer;
		std::function<bool(const char* reason)> executeNoMarkerFallback;
	};

	struct PendingRouteHandlers
	{
		std::function<void(bool immune)> setPlayerBleedImmune;
		std::function<void(RE::Actor* player)> preparePlayer;
		std::function<void(RE::Actor* player, float minHealth)> clampHealth;
		std::function<float(RE::Actor* player, float thresholdPct)> resolveSafeFloorHealth;
		std::function<bool()> hasPlayerBleedOwner;
		std::function<void(RE::Actor* player, float thresholdPct, const char* reason)> enterPlayerBleedLock;
		std::function<bool()> isBleedDecisionActive;
		std::function<ThresholdScan(RE::Actor* player)> scanThresholdOutcome;
		std::function<bool(RE::Actor* actor)> isObserverAlly;
		std::function<void(RE::Actor* actor)> rememberAggressor;
		std::function<float(RE::Actor* actor)> getHealthPct;
		DispatchHandlers dispatch;
	};

	[[nodiscard]] bool HasPendingOverkillRoute();
	[[nodiscard]] PendingOverkillRoute GetPendingOverkillRoute();
	void QueueOverkillRoute(RE::Actor* attacker, float thresholdPct, float hpBefore, float originalDamage, float clampedDamage, float blockedDamage, float safeFloorHp, const char* reason);
	void AccumulatePendingOverkillDamage(float originalDamage, float clampedDamage, float blockedDamage);
	void ClearPendingOverkillRoute(const char* reason);

	[[nodiscard]] ThresholdClassification ClassifyThresholdOutcome(const ThresholdScan& scan);
	[[nodiscard]] bool HasThresholdRescueFallbackCandidate(const ThresholdScan& scan, bool hasCachedDestination);
	[[nodiscard]] bool TryBeginNoThreatRescueFallback(RE::Actor* player, const ThresholdScan& scan, float thresholdPct, const char* reason, const NoThreatFallbackHandlers& handlers);
	[[nodiscard]] bool DispatchThresholdOutcome(RE::Actor* player, const ThresholdScan& scan, const ThresholdClassification& classification, const DispatchHandlers& handlers);
	[[nodiscard]] bool TickPendingOverkillRoute(RE::Actor* player, const PendingRouteHandlers& handlers);
}
