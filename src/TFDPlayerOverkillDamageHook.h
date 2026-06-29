#pragma once

#include <chrono>
#include <functional>

#include <RE/Skyrim.h>

namespace TFD::PlayerOverkillDamageHook
{
	struct Context
	{
		std::function<RE::Actor*()> getPlayer;
		std::function<RE::Actor*()> resolveCachedAttacker;
		std::function<bool()> hasPendingOverkillRoute;
		std::function<bool()> hasPlayerBleedLock;
		std::function<bool()> isInBleedState;
		std::function<bool(double)> hasRecentEnemyTargetingPlayer;
		std::function<bool(RE::Actor*)> isObserverAlly;
		std::function<void(bool)> setPlayerBleedImmune;
		std::function<void(RE::Actor*, float)> clampHealth;
		std::function<void(RE::Actor*)> noteEnemyTargetingPlayer;
		std::function<bool()> isPreDeathShieldActive;
		std::function<float(RE::Actor*, float)> resolveBleedRuntimeSafeHealth;
		std::function<void(RE::Actor*)> rememberAggressor;
		std::function<float()> getDefeatThresholdPct;
		std::function<float(RE::Actor*)> getActorHealthPct;
		std::function<RE::Actor*()> resolveAggressor;
		std::function<RE::Actor*(float, double)> resolveLastEnemyTargetingPlayer;
		std::function<void(RE::Actor*, float, const char*)> enterPlayerBleedLock;
		std::function<bool(RE::Actor*, float, float, const char*)> dispatchThresholdScanImmediateBleedout;
		std::function<bool(RE::Actor*, float, const char*)> tryBeginThresholdNoThreatRescueFallback;
	};

	float ResolveSafeFloorHealth(RE::Actor* actor, float thresholdPct);
	float ResolvePreDeathArmPct(float thresholdPct);
	bool IsPreDeathShieldActive();
	void SetPreDeathShieldActive(bool active, RE::Actor* player, float hpPct, float thresholdPct, const char* reason);
	void ClearPreDeathShield(const char* reason, bool releaseBleedImmuneIfUnowned);
	void TickPreDeathShield();
	void SetKillmoveGuard(bool enable, const char* reason, std::chrono::milliseconds hold = std::chrono::milliseconds(1800));
	bool IsKillmoveGuardActive();
	bool TryQueueKillmoveBlockedBleedout(RE::Actor* player, RE::Actor* attacker, const char* reason);
	void TickKillmoveSuppression();
	void SetContext(Context context);
	void ClearContext();
	bool HasContext();
	void Install();
}
