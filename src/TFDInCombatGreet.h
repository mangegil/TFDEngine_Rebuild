#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

#include "TFDInteractionRouter.h"

namespace RE
{
	class Actor;
}

namespace TFD::InCombatGreet
{
	enum class State : unsigned int
	{
		Idle = 0,
		Armed,
		Running,
		AfterPleasure
	};

	struct StickyReopenProbe
	{
		std::uint32_t speakerFormID = 0;
		bool loaded = false;
		bool dead = false;
		float distance = 99999.0f;
		bool ready = false;
	};

	struct DialogueClosedContext
	{
		bool seenDialogue = false;
		bool prevDialogueOpen = false;
		bool pleasureBlocking = false;
		bool afterPleasure = false;
	};

	struct DialogueClosedHandlers
	{
		std::function<void()> onAfterPleasureHold;
		std::function<void()> onComplete;
		std::function<void(const StickyReopenProbe&)> onStickyReopenForced;
		std::function<void(const StickyReopenProbe&)> onStickyReopenUnavailable;
	};

	void Install();
	void Reset();
	bool BeginForActor(RE::Actor* speaker, TFD::InteractionRouter::Action* outAction = nullptr);
	bool BeginForPleasureCycleActor(RE::Actor* speaker, TFD::InteractionRouter::Action* outAction = nullptr, bool allowPacifiedBridge = false);
	bool IsPleasureCycleActiveForActor(RE::Actor* speaker);
	void ResetRuntime(const char* reason = nullptr);
	bool Begin(RE::Actor* speaker, const char* reason = nullptr);
	bool BeginAfterPleasure(RE::Actor* speaker, const char* reason = nullptr);
	void CancelAll(const char* reason);

	void NotifyDialogueOpened();
	bool HasSeenDialogue();
	void MarkStickyReopenPending(bool value, const char* reason = nullptr);
	bool HasStickyReopenPending();
	bool IsRetryDue(std::chrono::steady_clock::time_point now);
	void NoteStickyRetry(std::chrono::steady_clock::time_point nextRetry, const char* reason = nullptr);
	int GetRetryCount();

	bool TryStickyWatchdog(bool prevDialogueOpen,
		bool dialogueOpen,
		bool pleasureBlocking,
		std::uint32_t speakerFormID,
		std::chrono::steady_clock::time_point now,
		const std::function<bool(const char* reason)>& reopenFn);

	StickyReopenProbe BuildStickyReopenProbe(RE::Actor* player, std::uint32_t speakerFormID);
	bool HandleDialogueClosedFlow(const DialogueClosedContext& ctx,
		RE::Actor* player,
		std::uint32_t speakerFormID,
		const DialogueClosedHandlers& handlers);

	bool TickDialogueRuntime(bool& prevDialogueOpen,
		bool dialogueOpen,
		bool pleasureBlocking,
		RE::Actor* player);

	bool IsRunning();
	State GetState();
	const char* GetStateName();
	bool OwnsCurrentFlow();
}
