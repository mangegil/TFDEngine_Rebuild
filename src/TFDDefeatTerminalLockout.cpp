#include "TFDDefeatTerminalLockout.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

namespace TFD::DefeatTerminalLockout
{
	namespace
	{
		constexpr const char* kBleedoutOutcomePayEvent = "TFDBleedoutOutcomePay";
		constexpr const char* kBleedoutOutcomePleasureEvent = "TFDBleedoutOutcomePleasure";
		constexpr const char* kBleedoutOutcomeCaptiveEvent = "TFDBleedoutOutcomeCaptive";
		constexpr const char* kBleedoutOutcomeReleaseEvent = "TFDBleedoutOutcomeRelease";
		constexpr const char* kBleedoutOutcomeResetEvent = "TFDBleedoutOutcomeReset";
		constexpr const char* kBleedoutOutcomeDoNothingEvent = "TFDBleedoutOutcomeDoNothing";

		bool g_active = false;
		std::chrono::steady_clock::time_point g_until{};
		std::chrono::steady_clock::time_point g_lastLog{};
		std::string g_reason{};
		float g_armHpPct = -1.0f;

		std::chrono::steady_clock::time_point Now()
		{
			return std::chrono::steady_clock::now();
		}
	}

	bool IsBleedoutTerminalOutcomeEvent(const char* rawName)
	{
		if (!rawName || !rawName[0]) {
			return false;
		}
		return std::strcmp(rawName, kBleedoutOutcomePayEvent) == 0 ||
			std::strcmp(rawName, kBleedoutOutcomePleasureEvent) == 0 ||
			std::strcmp(rawName, kBleedoutOutcomeCaptiveEvent) == 0 ||
			std::strcmp(rawName, kBleedoutOutcomeReleaseEvent) == 0 ||
			std::strcmp(rawName, kBleedoutOutcomeResetEvent) == 0 ||
			std::strcmp(rawName, kBleedoutOutcomeDoNothingEvent) == 0;
	}

	double ResolveSeconds(const char* rawName, float numArg)
	{
		if (!rawName) {
			return 6.0;
		}

		if (_stricmp(rawName, "TFDBleedoutOutcomePay") == 0) {
			const double requested = numArg > 0.0f ? static_cast<double>(numArg) : 30.0;
			return std::clamp(requested, 20.0, 40.0);
		}

		if (_stricmp(rawName, "TFDBleedoutOutcomeRelease") == 0) {
			const double requested = numArg > 0.0f ? static_cast<double>(numArg) : 30.0;
			return std::clamp(requested, 20.0, 40.0);
		}

		if (_stricmp(rawName, "TFDBleedoutOutcomeDoNothing") == 0) {
			return 12.0;
		}

		if (_stricmp(rawName, "TFDBleedoutOutcomeCaptive") == 0 ||
			_stricmp(rawName, "TFDBleedoutOutcomePleasure") == 0) {
			return 6.0;
		}

		return 6.0;
	}

	void Arm(const char* eventName, double seconds, const char* reason)
	{
		const double duration = std::clamp(seconds, 1.0, 45.0);
		g_active = true;
		g_until = Now() + std::chrono::milliseconds(static_cast<int>(duration * 1000.0));
		g_lastLog = {};
		g_reason = reason && reason[0] ? reason : (eventName && eventName[0] ? eventName : "terminal_outcome");
		g_armHpPct = -1.0f;
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			const float hpNow = player->GetActorValue(RE::ActorValue::kHealth);
			const float hpMax = (std::max)(1.0f, player->GetPermanentActorValue(RE::ActorValue::kHealth));
			g_armHpPct = (hpNow / hpMax) * 100.0f;
		}
		spdlog::info("[TFD][Defeat][P18] post-terminal bleedout threshold lockout armed event={} seconds={:.1f} reason={} armHpPct={:.1f}",
			eventName && eventName[0] ? eventName : "<none>",
			duration,
			g_reason,
			g_armHpPct);
	}

	bool IsActive(const char* checkReason, float hpPct, float thresholdPct)
	{
		if (!g_active) {
			return false;
		}
		const auto now = Now();
		if (now >= g_until) {
			// Terminal Release/Pay can leave the player exactly on the defeat
			// threshold floor.  Hold while there is no fresh post-terminal damage.
			const bool stillAtThresholdFloor = hpPct <= (thresholdPct + 0.25f);
			const bool noFreshPostTerminalDamage = g_armHpPct >= 0.0f &&
				hpPct >= (g_armHpPct - 0.50f);
			if (stillAtThresholdFloor && noFreshPostTerminalDamage) {
				g_until = now + std::chrono::milliseconds(1500);
				if (g_lastLog.time_since_epoch().count() == 0 ||
					(now - g_lastLog) >= std::chrono::milliseconds(900)) {
					g_lastLog = now;
					spdlog::info("[TFD][Defeat][P18] post-terminal lockout held until player recovery check={} hpPct={:.1f} armHpPct={:.1f} threshold={:.1f} reason={}",
						checkReason && checkReason[0] ? checkReason : "threshold",
						hpPct,
						g_armHpPct,
						thresholdPct,
						g_reason.empty() ? "unknown" : g_reason.c_str());
				}
				return true;
			}

			g_active = false;
			g_until = {};
			g_lastLog = {};
			spdlog::info("[TFD][Defeat][P18] post-terminal bleedout threshold lockout expired reason={} hpPct={:.1f} armHpPct={:.1f} threshold={:.1f}",
				g_reason.empty() ? "unknown" : g_reason.c_str(),
				hpPct,
				g_armHpPct,
				thresholdPct);
			g_reason.clear();
			g_armHpPct = -1.0f;
			return false;
		}
		if (g_lastLog.time_since_epoch().count() == 0 ||
			(now - g_lastLog) >= std::chrono::milliseconds(900)) {
			g_lastLog = now;
			const auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(g_until - now).count();
			spdlog::info("[TFD][Defeat][P18] player threshold suppressed during post-terminal lockout check={} hpPct={:.1f} threshold={:.1f} remainingMs={} reason={}",
				checkReason && checkReason[0] ? checkReason : "threshold",
				hpPct,
				thresholdPct,
				static_cast<long long>(remainingMs),
				g_reason.empty() ? "unknown" : g_reason.c_str());
		}
		return true;
	}

	void Clear(const char* reason)
	{
		if (!g_active) {
			return;
		}
		spdlog::info("[TFD][Defeat][P18] post-terminal lockout cleared reason={} previousReason={}",
			reason && reason[0] ? reason : "clear",
			g_reason.empty() ? "unknown" : g_reason.c_str());
		g_active = false;
		g_until = {};
		g_lastLog = {};
		g_reason.clear();
		g_armHpPct = -1.0f;
	}
}
