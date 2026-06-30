#include "TFDDefeatBleedoutRuntimeTimer.h"

#include <cstdio>

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

namespace TFD::DefeatBleedoutRuntimeTimer
{
	namespace
	{
		Clock::time_point g_bleedStart{};
		int g_bleedLastSeconds = -1;
		bool g_bleedPaused = false;
		Clock::time_point g_bleedPauseStarted{};
		Clock::time_point g_bleedLastCalmPulse{};
	}

	Clock::time_point& BleedStart()
	{
		return g_bleedStart;
	}

	int& BleedLastSeconds()
	{
		return g_bleedLastSeconds;
	}

	bool& BleedPaused()
	{
		return g_bleedPaused;
	}

	Clock::time_point& BleedPauseStarted()
	{
		return g_bleedPauseStarted;
	}

	Clock::time_point& BleedLastCalmPulse()
	{
		return g_bleedLastCalmPulse;
	}

	void ResetAll()
	{
		g_bleedStart = {};
		g_bleedLastSeconds = -1;
		g_bleedPaused = false;
		g_bleedPauseStarted = {};
		g_bleedLastCalmPulse = {};
	}

	void ResetDialogueRuntimeClock()
	{
		g_bleedPaused = false;
		g_bleedPauseStarted = {};
		g_bleedLastCalmPulse = {};
		g_bleedStart = Clock::now();
		g_bleedLastSeconds = -1;
	}

	void SetStartNow()
	{
		g_bleedStart = Clock::now();
		g_bleedLastSeconds = -1;
	}

	void SetCountdownDirty()
	{
		g_bleedLastSeconds = -1;
	}

	bool Pause(const char* reason)
	{
		if (g_bleedPaused) {
			return false;
		}

		g_bleedPaused = true;
		g_bleedPauseStarted = Clock::now();
		spdlog::info("[TFD][Defeat][P24] bleed countdown paused by {}", reason ? reason : "unknown");
		return true;
	}

	bool ResumeIfPaused()
	{
		if (!g_bleedPaused) {
			return false;
		}

		g_bleedStart += (Clock::now() - g_bleedPauseStarted);
		g_bleedPaused = false;
		g_bleedPauseStarted = {};
		g_bleedLastSeconds = -1;
		spdlog::info("[TFD][Defeat][P24] bleed countdown resumed after dialogue");
		return true;
	}

	bool TickCountdown(const CountdownTickInput& input)
	{
		const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - g_bleedStart).count();
		const int remain = input.bleedSeconds - static_cast<int>(elapsed);
		if (remain != g_bleedLastSeconds) {
			g_bleedLastSeconds = remain;
			if (remain > 0 && !input.suppressNotice) {
				char msg[96]{};
				std::snprintf(msg, sizeof(msg), "TFDEngine: Bleeding... (%ds)", remain);
				RE::DebugNotification(msg);
			}
		}

		if (remain <= 0) {
			g_bleedLastSeconds = -1;
			return input.onTimeout ? input.onTimeout() : false;
		}

		return false;
	}
}
