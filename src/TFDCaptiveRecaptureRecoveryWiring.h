#pragma once

#include <functional>

#include <RE/Skyrim.h>

#include "TFDCaptiveRecaptureRecovery.h"

namespace TFD::CaptiveRecaptureRecoveryWiring
{
	struct Dependencies
	{
		std::function<RE::Actor*()> getPlayer;
		std::function<void(const char*, bool)> releasePlayerBleedLock;
		std::function<void(RE::Actor*, float, float, float, float, float, const char*)> restoreActorHealthToSafePct;
		std::function<void(bool)> resetBleedRuntimeState;
		std::function<void(const char*)> forceStopBleedRuntimeForCaptiveRecapture;
		std::function<void(bool)> setPlayerBleedImmune;
		std::function<void(RE::Actor*)> playPlayerGetUp;
		std::function<void()> refreshPostDefeatGlobals;
		std::function<float(RE::Actor*)> getActorHealthPct;
		std::function<float()> getDefeatThresholdPct;
		std::function<bool(RE::Actor*)> isActorBleedingOut;
	};

	void SetDependencies(Dependencies dependencies);
	void ClearDependencies();
	bool HasDependencies();

	TFD::CaptiveRecaptureRecovery::Context BuildContext();

	void ArmPulse(const char* reason = nullptr);
	void TickPulse();
	void ResetPulse();

	void InstallProvider(Dependencies dependencies);
	void ShutdownProvider();
}
