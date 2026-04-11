#pragma once

#include <chrono>
#include <functional>

namespace TFD::TransitionRuntime
{
	enum class Kind
	{
		None = 0,
		Captive = 1,
		Rescue = 2,
		Recover = 3
	};

	const char* GetKindName(Kind kind);

	bool QueueRequest(Kind kind, bool fadeIn, const char* reason = nullptr);

	void ClearPendingFadeIn();
	void ArmPendingFadeIn(Kind kind, std::chrono::steady_clock::time_point notBefore, bool sawLoadingMenu = false);
	bool HasPendingFadeIn();
	void ProcessPendingFadeIn();

	bool IsAwaiting();
	void PollResult();

	bool BeginImmediate(Kind kind, const char* reason, const std::function<bool(const char*)>& completeNow);
	void BeginImmediateVoid(Kind kind, const char* reason, const std::function<void(const char*)>& completeNow);

	void ShowBlackoutFader();
	void HideBlackoutFader();
}
