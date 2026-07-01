#include "TFDDefeatBleedRuntimeState.h"

#include <algorithm>

namespace TFD::DefeatBleedRuntimeState
{
	namespace
	{
		std::atomic_bool g_inBleedState{ false };
		float g_minHp{ 0.0f };
		bool g_bleedPendingCaptiveOutcome = false;
		bool g_bleedPendingNonCaptiveOutcome = false;
	}

	std::atomic_bool& InBleedState()
	{
		return g_inBleedState;
	}

	float& MinHp()
	{
		return g_minHp;
	}

	bool& PendingCaptiveOutcome()
	{
		return g_bleedPendingCaptiveOutcome;
	}

	bool& PendingNonCaptiveOutcome()
	{
		return g_bleedPendingNonCaptiveOutcome;
	}

	bool IsInBleedState()
	{
		return g_inBleedState.load(std::memory_order_acquire);
	}

	void SetInBleedState(bool active)
	{
		g_inBleedState.store(active, std::memory_order_release);
		if (!active) {
			ResetMinHp();
		}
	}

	void ResetMinHp()
	{
		g_minHp = 0.0f;
	}

	void SetMinHp(float value)
	{
		g_minHp = value;
	}

	void MaxMinHp(float value)
	{
		g_minHp = (std::max)(g_minHp, value);
	}

	float GetMinHp()
	{
		return g_minHp;
	}

	void ClearPendingOutcomes()
	{
		g_bleedPendingCaptiveOutcome = false;
		g_bleedPendingNonCaptiveOutcome = false;
	}
}
