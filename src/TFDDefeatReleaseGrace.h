#pragma once

#include <atomic>
#include <chrono>
#include <functional>

namespace TFD::DefeatReleaseGrace
{
	struct Dependencies
	{
		std::function<void()> clearLeftForDeadCooldown;
		std::function<void(const char*)> clearAllReleaseFollowGrace;
	};

	void InstallProvider(Dependencies dependencies);
	void ShutdownProvider();
	bool HasProvider();

	std::atomic_bool& GraceActive();
	std::chrono::steady_clock::time_point& GraceUntil();

	bool IsActive();
	void SetSeconds(int seconds);
	void SetActive(bool active);
	void Clear(const char* reason = nullptr);
	void Reset(const char* reason = nullptr);
}
