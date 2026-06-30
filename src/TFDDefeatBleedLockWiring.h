#pragma once

#include <chrono>
#include <functional>

#include <RE/Skyrim.h>

#include "TFDBleedLockRuntime.h"
#include "TFDPlayerDamageGuard.h"
#include "TFDPlayerDownRouter.h"

namespace TFD::DefeatBleedLockWiring
{
	using Entry = TFD::BleedLockRuntime::Entry;

	struct Dependencies
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
		std::function<void(RE::Actor*, const Entry&, const char*, const char*)> logReferenceBleedState;
		std::function<void(RE::Actor*, Entry&, std::chrono::steady_clock::time_point)> maybeLogReferenceBleedSamples;
		std::function<void(RE::Actor*, Entry&)> enforcePlayerBleedInvulnerability;
		std::function<void()> tickPlayerKillmoveSuppression;
		std::function<void()> tickPlayerOverkillBlockPending;
		std::function<void()> tickPlayerPreDeathShield;
		std::function<void()> scanBleedLockCandidates;
		std::function<RE::Actor*(float)> findBestAggressor;
		std::function<bool(RE::Actor*)> isActiveFollowerActor;
		std::function<bool()> hasPlayerBleedLock;
		std::function<bool(RE::Actor*, float, float, const char*)> dispatchThresholdScanImmediateBleedout;
		std::function<TFD::PlayerDownRouter::ThresholdScan(RE::Actor*)> scanPlayerThresholdOutcome;
		std::function<bool(RE::Actor*, const TFD::PlayerDownRouter::ThresholdScan&, float, const char*)> tryBeginThresholdNoThreatRescueFallback;
		std::function<bool(RE::Actor*)> isDownedTeammateRecoveryDialogueHoldActor;
		std::function<bool()> isPlayerBattleObserveActive;
		std::function<void(const char*)> resetPlayerDamageGuard;
		std::function<void(const char*)> clearPendingOverkillRoute;
		std::function<void()> clearPreDeathShield;
		std::function<void()> resetEnemyThresholdScanTimer;
	};

	void InstallProvider(Dependencies dependencies);
	void ShutdownProvider();
	bool HasProvider();
	void ApplyBleedRegenOverride(RE::Actor* actor, Entry& entry);
	void RestoreBleedRegenOverride(RE::Actor* actor, const Entry& entry);
	void ForcePlayerBleedAlive(RE::Actor* actor, Entry& entry, const char* reason);
	void RestoreActorHealthToSafePct(RE::Actor* actor, float thresholdPct, float bonusPct, float minSafePct, float maxSafePct, float minAbsHp, const char* reason);

	TFD::BleedLockRuntime::Context BuildContext();
	void ScanCandidates();
}
