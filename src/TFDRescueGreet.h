#pragma once

#include <cstdint>

namespace RE
{
	class Actor;
}

namespace TFD::RescueGreet
{
	enum class State : unsigned int
	{
		Idle = 0,
		PostTeleportPending,
		Armed,
		Running
	};

	void Install();
	void Reset();
	void ResetRuntime(const char* reason = nullptr);

	// Legacy direct begin. Kept for compatibility, but Rescue should normally use
	// ArmPostTeleportSavior() so the hard-open happens after teleport/loading.
	bool Begin(RE::Actor* speaker, const char* reason = nullptr);

	// R36D: Rescue-owned post-teleport queue. This does not route through the
	// generic InteractionRouter forcegreet machine.
	bool ArmPostTeleportSavior(RE::Actor* speaker, const char* source = nullptr, const char* reason = nullptr);
	void NotifyWorldReady(const char* reason = nullptr);
	bool RetryPendingHardOpen(const char* reason = nullptr);
	void NotifyDialogueMenuStateChanged(bool opening);
	bool IsActivationBlocked();
	bool IsPendingOrActive();
	bool IsPendingForSpeaker(RE::Actor* speaker);

	void Cancel(const char* reason = nullptr);

	void NotifyDialogueOpened();
	bool HasSeenDialogue();

	bool IsActive();
	State GetState();
	const char* GetStateName();
	std::uint32_t GetSpeakerFormID();
	bool OwnsCurrentFlow();
}
