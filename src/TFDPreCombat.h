#pragma once

#include <cstdint>

namespace TFD::PreCombat
{
	enum class State : std::uint32_t
	{
		Idle = 0,
		Claimed,
		InDialogue,
		Restoring
	};

	void Install();
	void Shutdown();

	bool IsActive();
	State GetState();

	// Manual trigger if needed, but normal use comes from built-in H polling.
	bool TryStartFromHotkey();
	void CancelAndRestore();

	const char* GetStateName();
}
