#pragma once

#include <cstdint>

namespace RE
{
	class Actor;
}

namespace TFD::CaptiveGreet
{
	enum class State : unsigned int
	{
		Idle = 0,
		Armed,
		Running,
		AfterPleasure
	};

	void Install();
	void Reset();
	void ResetRuntime(const char* reason = nullptr);
	bool Begin(RE::Actor* speaker, const char* reason = nullptr);
	bool BeginAfterPleasure(RE::Actor* speaker, const char* reason = nullptr);
	void Cancel(const char* reason = nullptr);

	void NotifyDialogueOpened();
	bool HasSeenDialogue();

	bool IsActive();
	State GetState();
	const char* GetStateName();
	std::uint32_t GetSpeakerFormID();
	bool OwnsCurrentFlow();
}
