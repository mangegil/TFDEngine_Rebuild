#pragma once

#include <chrono>

#include <RE/Skyrim.h>

#include "TFDBleedLockState.h"
#include "TFDPlayerDamageGuard.h"

namespace TFD::DefeatBleedHealthGuard
{
	using BleedLockEntry = TFD::BleedLockState::Entry;

	void ClampHealth(RE::Actor* actor, float minHp);
	void ClampHealthCeiling(RE::Actor* actor, float maxHp);
	float ResolveActorHealthForPct(RE::Actor* actor, float pct);
	float ResolvePlayerBleedRuntimeSafeHealth(RE::Actor* actor, float thresholdPct);
	TFD::PlayerDamageGuard::Config BuildPlayerDamageGuardConfig(RE::Actor* actor, float thresholdPct, float protectedHp, float safeFloorHp);
	void EnforcePlayerBleedInvulnerability(RE::Actor* actor, BleedLockEntry& entry);
	void LogReferenceBleedState(RE::Actor* actor, const BleedLockEntry& entry, const char* point, const char* note);
	void MaybeLogReferenceBleedSamples(RE::Actor* actor, BleedLockEntry& entry, std::chrono::steady_clock::time_point now);
}
