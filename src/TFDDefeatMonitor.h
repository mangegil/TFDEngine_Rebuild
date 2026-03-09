#pragma once

#include <cstdint>

namespace TFD::DefeatMonitor
{
	void Install();
	void Shutdown();
	void ResetGrace();
	void ResetForLoad();
	void SetLoadTransition(bool active);
	bool IsCaptivePhase();

	bool GetCaptiveStateForSave();
	std::uint32_t GetCaptivePhaseForSave();
	void QueueLoadedProgressState(bool stateActive, std::uint32_t phaseRaw);
	void QueueDefaultProgressState();
	bool HasQueuedProgressState();
	void ApplyQueuedProgressState();
}
