#pragma once

#include <functional>
#include <vector>

#include <RE/Skyrim.h>

#include "TFDPlayerDownRouter.h"

namespace TFD::PlayerThresholdOutcomeWiring
{
	using ThresholdScan = TFD::PlayerDownRouter::ThresholdScan;
	using ThresholdClassification = TFD::PlayerDownRouter::ThresholdClassification;

	struct Dependencies
	{
		std::function<std::vector<RE::Actor*>(float)> collectStandingFollowers;
		std::function<bool(RE::Actor*)> isObserverAlly;
		std::function<bool()> isInBleedState;
		std::function<void(const char*)> releaseStaleEscapeBreakBleedRuntime;
	};

	void InstallProvider(Dependencies dependencies);
	void ShutdownProvider();
	bool HasProvider();

	ThresholdScan ScanOutcome(RE::Actor* player);
	ThresholdClassification ClassifyOutcome(const ThresholdScan& scan);
	bool TryBeginNoThreatRescueFallback(RE::Actor* player, const ThresholdScan& scan, float thresholdPct, const char* reason);
	bool TryBeginNoThreatRescueFallback(RE::Actor* player, float thresholdPct, const char* reason);
	bool DispatchOutcome(RE::Actor* player, const ThresholdScan& scan, const ThresholdClassification& classification);
	bool DispatchImmediateBleedout(RE::Actor* player, float playerHpPct, float playerThreshold, const char* reason);
	bool HandlePlayerThreshold(RE::Actor* player, float playerHpPct, float playerThreshold, const char* reason);
	void ClearImmediateDispatchThrottle();
}
