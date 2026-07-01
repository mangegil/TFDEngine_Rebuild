#include "TFDDefeatReleaseGrace.h"

#include <mutex>
#include <utility>

#include <spdlog/spdlog.h>

namespace TFD::DefeatReleaseGrace
{
	namespace
	{
		std::mutex g_providerLock;
		Dependencies g_dependencies{};
		bool g_hasProvider = false;

		std::atomic_bool g_grace{ false };
		std::chrono::steady_clock::time_point g_graceUntil{};

		static std::chrono::steady_clock::time_point Now()
		{
			return std::chrono::steady_clock::now();
		}

		Dependencies ResolveDependencies()
		{
			std::scoped_lock lock(g_providerLock);
			return g_dependencies;
		}
	}

	void InstallProvider(Dependencies dependencies)
	{
		std::scoped_lock lock(g_providerLock);
		g_dependencies = std::move(dependencies);
		g_hasProvider = true;
		spdlog::info("[TFD][ReleaseGrace][P28E] provider installed");
	}

	void ShutdownProvider()
	{
		std::scoped_lock lock(g_providerLock);
		g_dependencies = {};
		g_hasProvider = false;
		spdlog::info("[TFD][ReleaseGrace][P28E] provider cleared");
	}

	bool HasProvider()
	{
		std::scoped_lock lock(g_providerLock);
		return g_hasProvider;
	}

	std::atomic_bool& GraceActive()
	{
		return g_grace;
	}

	std::chrono::steady_clock::time_point& GraceUntil()
	{
		return g_graceUntil;
	}

	bool IsActive()
	{
		if (!g_grace.load(std::memory_order_acquire)) {
			return false;
		}
		if (Now() >= g_graceUntil) {
			g_grace.store(false, std::memory_order_release);
			g_graceUntil = {};
			return false;
		}
		return true;
	}

	void SetSeconds(int seconds)
	{
		if (seconds <= 0) {
			g_grace.store(false, std::memory_order_release);
			g_graceUntil = {};
			return;
		}
		g_graceUntil = Now() + std::chrono::seconds(seconds);
		g_grace.store(true, std::memory_order_release);
	}

	void SetActive(bool active)
	{
		// Preserve the old CaptiveTick behavior: this toggles only the active flag.
		// The timeout remains owned by SetSeconds/IsActive.
		g_grace.store(active, std::memory_order_release);
	}

	void Clear(const char* reason)
	{
		g_grace.store(false, std::memory_order_release);
		g_graceUntil = {};
		if (reason && reason[0] != '\0') {
			spdlog::debug("[TFD][ReleaseGrace][P28E] grace cleared reason={}", reason);
		}
	}

	void Reset(const char* reason)
	{
		Clear(reason ? reason : "reset_grace");
		const auto dependencies = ResolveDependencies();
		if (dependencies.clearLeftForDeadCooldown) {
			dependencies.clearLeftForDeadCooldown();
		}
		if (dependencies.clearAllReleaseFollowGrace) {
			dependencies.clearAllReleaseFollowGrace(reason ? reason : "reset_grace");
		}
	}
}
