#pragma once

#include <functional>

#include <RE/Skyrim.h>

namespace TFD::CaptiveRecaptureRecoveryPulse
{
	struct Context
	{
		std::function<RE::Actor*()> getPlayer;
		std::function<void(const char*)> recoverPlayer;
		std::function<bool(RE::Actor*)> isActorBleedingOut;
		std::function<float(RE::Actor*)> getActorHealthPct;
		std::function<float()> getDefeatThresholdPct;
	};

	void Arm(const Context& context, const char* reason = nullptr);
	void Tick(const Context& context);
	void Reset();
	bool IsPending();
}
