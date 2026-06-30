#include "TFDDefeatStickyReopenGrace.h"

#include <spdlog/spdlog.h>

#include "TFDBleedoutGreet.h"

namespace TFD::DefeatStickyReopenGrace
{
	namespace
	{
		bool g_active = false;
		std::chrono::steady_clock::time_point g_until{};
		std::uint32_t g_speakerID = 0;
		float g_distance = 99999.0f;
	}

	bool IsActive()
	{
		return g_active;
	}

	std::uint32_t SpeakerID()
	{
		return g_speakerID;
	}

	void Clear(const char* reason)
	{
		if (!g_active) {
			return;
		}
		spdlog::info("[TFD][Defeat][P19] bleed sticky reopen grace cleared reason={} speaker={:08X}",
			reason ? reason : "unknown",
			g_speakerID);
		g_active = false;
		g_until = {};
		g_speakerID = 0;
		g_distance = 99999.0f;
	}

	void CancelAfterTerminalOutcome(const char* eventName, const char* reason)
	{
		const bool wasActive = g_active;
		const std::uint32_t speakerID = g_speakerID;
		const char* why = reason && reason[0] ? reason : "terminal_outcome";

		Clear(why);
		TFD::BleedoutGreet::MarkStickyReopenPending(false, why);

		if (wasActive) {
			spdlog::info("[TFD][Defeat][P19] bleed sticky reopen cancelled by terminal outcome event={} speaker={:08X} reason={}",
				eventName && eventName[0] ? eventName : "unknown",
				speakerID,
				why);
		}
	}

	bool Tick(RE::Actor* player, std::chrono::steady_clock::time_point now)
	{
		(void)player;
		(void)now;
		if (!g_active) {
			return false;
		}

		// R470A/P19: DefeatMonitor is an HP/death guard only. Legacy sticky
		// reopen grace used to let this monitor reopen Bleedout forcegreet
		// after another owner had already moved the route. That overlap caused
		// stale speaker ownership and AfterPleasure flicker.
		spdlog::info("[TFD][Defeat][P19] bleed sticky reopen grace ignored monitor-only speaker={:08X}", g_speakerID);
		Clear("r470a_monitor_only_no_reopen");
		TFD::BleedoutGreet::MarkStickyReopenPending(false, "r470a_monitor_only_no_reopen");
		return false;
	}
}
