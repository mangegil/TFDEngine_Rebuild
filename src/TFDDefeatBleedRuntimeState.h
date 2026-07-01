#pragma once

#include <atomic>

namespace TFD::DefeatBleedRuntimeState
{
	std::atomic_bool& InBleedState();
	float& MinHp();
	bool& PendingCaptiveOutcome();
	bool& PendingNonCaptiveOutcome();

	bool IsInBleedState();
	void SetInBleedState(bool active);
	void ResetMinHp();
	void SetMinHp(float value);
	void MaxMinHp(float value);
	float GetMinHp();
	void ClearPendingOutcomes();
}
