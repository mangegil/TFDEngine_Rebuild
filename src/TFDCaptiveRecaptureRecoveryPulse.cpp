#include "TFDCaptiveRecaptureRecoveryPulse.h"

#include <algorithm>
#include <chrono>
#include <string>

#include <spdlog/spdlog.h>

#include "TFDFlowController.h"

namespace TFD::CaptiveRecaptureRecoveryPulse
{
	namespace
	{
		bool g_pending = false;
		std::chrono::steady_clock::time_point g_until{};
		std::chrono::steady_clock::time_point g_lastPulse{};
		std::string g_reason{};

		std::chrono::steady_clock::time_point Now()
		{
			return std::chrono::steady_clock::now();
		}

		void QueueBridgeCleanup(const char* reason)
		{
			const char* why = reason ? reason : "captive_recapture_cleanup";
			(void)TFD::FlowController::QueueBridgeModEvent("TFDBleedoutClearAll", nullptr, why, 1.0f);
			(void)TFD::FlowController::QueueBridgeModEvent("TFDTruceClearAll", nullptr, why, 1.0f);
			(void)TFD::FlowController::QueueBridgeModEvent("TFDTruceHardClearAll", nullptr, why, 1.0f);
			(void)TFD::FlowController::QueueBridgeModEvent("TFDSystemEventClearAfterPleasure", nullptr, why, 1.0f);
		}

		void Recover(const Context& context, const char* reason)
		{
			if (context.recoverPlayer) {
				context.recoverPlayer(reason ? reason : "captive_recapture_recover_player");
			}
		}
	}

	void Arm(const Context& context, const char* reason)
	{
		const char* why = reason ? reason : "captive_recapture_post_teleport";
		g_pending = true;
		g_until = Now() + std::chrono::milliseconds(4500);
		g_lastPulse = {};
		g_reason = why;
		QueueBridgeCleanup(why);
		Recover(context, why);
	}

	void Tick(const Context& context)
	{
		if (!g_pending) {
			return;
		}

		const auto now = Now();
		if (now > g_until) {
			Reset();
			return;
		}

		if (g_lastPulse.time_since_epoch().count() != 0 &&
			(now - g_lastPulse) < std::chrono::milliseconds(350)) {
			return;
		}
		g_lastPulse = now;

		const char* why = g_reason.empty() ? "captive_recapture_recover_pulse" : g_reason.c_str();
		Recover(context, why);
		QueueBridgeCleanup(why);

		auto* player = context.getPlayer ? context.getPlayer() : nullptr;
		const float defeatThresholdPct = context.getDefeatThresholdPct ? context.getDefeatThresholdPct() : 0.0f;
		const bool bleedingOut = player && context.isActorBleedingOut ? context.isActorBleedingOut(player) : false;
		const float healthPct = player && context.getActorHealthPct ? context.getActorHealthPct(player) : 0.0f;
		if (player && !bleedingOut && healthPct > std::clamp(defeatThresholdPct + 2.0f, 2.0f, 99.0f)) {
			Reset();
			spdlog::info("[TFD][CaptiveRecapture] recovery pulse completed hpPct={:.1f}", healthPct);
		}
	}

	void Reset()
	{
		g_pending = false;
		g_reason.clear();
		g_until = {};
		g_lastPulse = {};
	}

	bool IsPending()
	{
		return g_pending;
	}
}
