#pragma once

#include <chrono>
#include <functional>

#include <RE/Skyrim.h>

#include "TFDBleedLockState.h"
#include "TFDPlayerDamageGuard.h"

namespace TFD::BleedLockRuntime
{
	using Kind = TFD::BleedLockState::Kind;
	using Entry = TFD::BleedLockState::Entry;

	struct Context
	{
		std::function<RE::Actor*()> getPlayer;
		std::function<RE::Actor*()> resolveAggressor;
		std::function<void(bool)> setPlayerBleedImmune;
		std::function<float()> getMinHp;
		std::function<void(float)> setMinHp;
		std::function<void(float)> maxMinHp;
		std::function<void()> resetPreDeathShield;
		std::function<bool()> isInBleedState;
		std::function<float(RE::Actor*, float)> resolvePlayerBleedRuntimeSafeHealth;
		std::function<TFD::PlayerDamageGuard::Config(RE::Actor*, float, float, float)> buildPlayerDamageGuardConfig;
		std::function<void(RE::Actor*, float)> clampHealth;
		std::function<void(RE::Actor*, float)> clampHealthCeiling;
		std::function<float(RE::Actor*, float)> resolveActorHealthForPct;
		std::function<float(RE::Actor*)> getActorHealthPct;
		std::function<bool(RE::Actor*)> isActorBleedingOut;
		std::function<void(RE::Actor*, Entry&)> applyBleedRegenOverride;
		std::function<void(RE::Actor*, const Entry&)> restoreBleedRegenOverride;
		std::function<void(RE::Actor*, const Entry&, const char*, const char*)> logReferenceBleedState;
		std::function<void(RE::Actor*, Entry&, std::chrono::steady_clock::time_point)> maybeLogReferenceBleedSamples;
		std::function<void(RE::Actor*, Entry&)> enforcePlayerBleedInvulnerability;
		std::function<void(RE::Actor*, Entry&, const char*)> forcePlayerBleedAlive;
		std::function<void()> tickPlayerKillmoveSuppression;
		std::function<void()> tickPlayerOverkillBlockPending;
		std::function<void()> tickPlayerPreDeathShield;
		std::function<void()> scanBleedLockCandidates;
		std::function<bool(RE::Actor*)> isDownedTeammateRecoveryDialogueHoldActor;
		std::function<bool()> isPlayerBattleObserveActive;
		std::function<void(const char*)> resetPlayerDamageGuard;
		std::function<void(const char*)> clearPendingOverkillRoute;
		std::function<void()> clearPreDeathShield;
		std::function<void()> resetEnemyThresholdScanTimer;
	};

	bool HasActive(RE::Actor* actor);
	bool HasPlayer(const Context& context);
	bool HasAlly(RE::Actor* actor);
	void Enter(const Context& context, RE::Actor* actor, Kind kind, float thresholdPct, const char* reason);
	void Release(const Context& context, RE::Actor* actor, const char* reason, bool playGetUp);
	void ReleasePlayer(const Context& context, const char* reason, bool playGetUp);
	void ClearAll(const Context& context, const char* reason);
	void Tick(const Context& context);
}
