#pragma once

#include <cstdint>

namespace TFD::DefeatSaveLoadBridge
{
	bool IsLoadTransitionActive();
	void ClearLoadTransition(const char* reason = nullptr);
	void ClearQueuedProgressState(const char* reason = nullptr);

	bool GetCaptiveStateForSave();
	std::uint32_t GetCaptivePhaseForSave();
	bool GetBleedOutStateForSave();

	void QueueLoadedBleedOutState(bool active);
	void QueueLoadedProgressState(bool stateActive, std::uint32_t phaseRaw);
	void QueueDefaultProgressState();
	bool HasQueuedProgressState();
	void ApplyQueuedProgressState();

	void ResetForLoad();
	void SetLoadTransition(bool active);
}
