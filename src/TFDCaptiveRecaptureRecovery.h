#pragma once

#include <functional>
#include <utility>

#include <RE/Skyrim.h>

namespace TFD::CaptiveRecaptureRecovery
{
	struct Context
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
	};

	using ContextProvider = std::function<Context()>;

	void SetContextProvider(ContextProvider provider);
	void ClearContextProvider();
	bool HasContextProvider();

	void ForceRecoverPlayer(const Context& context, const char* reason = nullptr);
	void ForceRecoverPlayer(const char* reason = nullptr);
}
