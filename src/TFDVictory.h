#pragma once

#include <cstdint>

namespace TFD::Victory
{
	struct ObservedContext
	{
		bool hasPlayer{ false };
		bool playerDown{ false };
		bool combatContext{ false };
		bool hasEnemies{ false };
	};

	void SetStateValue(int value);
	int GetStateValue();
	bool IsActive();

	void ResetObservedContext();
	int ComputeObservedState(const ObservedContext& context);
	void RefreshObservedState(const ObservedContext& context);
}
