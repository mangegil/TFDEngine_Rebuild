#pragma once

#include <chrono>
#include <functional>

namespace TFD::DefeatBleedoutRuntimeTimer
{
	using Clock = std::chrono::steady_clock;

	struct CountdownTickInput
	{
		int bleedSeconds = 0;
		bool suppressNotice = false;
		std::function<bool()> onTimeout;
	};

	Clock::time_point& BleedStart();
	int& BleedLastSeconds();
	bool& BleedPaused();
	Clock::time_point& BleedPauseStarted();
	Clock::time_point& BleedLastCalmPulse();

	void ResetAll();
	void ResetDialogueRuntimeClock();
	void SetStartNow();
	void SetCountdownDirty();
	bool Pause(const char* reason);
	bool ResumeIfPaused();
	bool TickCountdown(const CountdownTickInput& input);
}
