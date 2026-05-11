#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

namespace RE
{
	class Actor;
}

namespace TFD::BleedoutGreet
{
	enum class HoldReason : std::uint8_t
	{
		None = 0,
		Dialogue,
		DialogueReopen,
		OStimBridge,
		PleasureCommit
	};

	struct HoldDecision
	{
		bool hold = false;
		HoldReason reason = HoldReason::None;
	};

struct DialogueClosedContext
{
	bool seenDialogue = false;
	bool prevDialogueOpen = false;
	bool hasTerminalCommit = false;
	bool pleasureBlocking = false;
	bool captiveOutcome = false;
};

struct StickyReopenProbe
{
	std::uint32_t speakerFormID = 0;
	bool loaded = false;
	bool dead = false;
	float distance = 99999.0f;
	bool ready = false;
};

struct DialogueClosedHandlers
{
	std::function<void()> onTerminalCommit;
	std::function<void()> onPleasureBlocking;
	std::function<void()> onCaptiveCommit;
	std::function<void(const StickyReopenProbe&)> onStickyReopenForced;
	std::function<void(const StickyReopenProbe&)> onStickyReopenUnavailable;
};

	void Install();
	void Reset();
	void ResetRuntime(const char* reason = nullptr);

	bool Begin(RE::Actor* speaker, const char* reason = nullptr);
	bool BeginAfterPleasure(RE::Actor* speaker, const char* reason = nullptr);
	void Cancel(const char* reason = nullptr);

	void NotifyDialogueOpened();
	void MarkAfterPleasureArmed(const char* reason = nullptr);
	bool HasSeenDialogue();
	void MarkStickyReopenPending(bool value, const char* reason = nullptr);
	bool HasStickyReopenPending();
	bool IsRetryDue(std::chrono::steady_clock::time_point now);
	void NoteStickyRetry(std::chrono::steady_clock::time_point nextRetry, const char* reason = nullptr);
	int GetRetryCount();

	HoldDecision EvaluateHold(bool dialogueOpen, bool pleasureCommitted, bool ostimBridgeBlocking);
	const char* GetHoldReasonName(HoldReason reason);
	bool TryStickyWatchdog(bool hasTerminalCommit, bool dialogueOpen, bool pleasureBlocking, std::uint32_t speakerFormID,
		std::chrono::steady_clock::time_point now, const std::function<bool(const char* reason)>& reopenFn);
	bool TryAfterPleasureWatchdog(bool prevDialogueOpen, bool dialogueOpen, std::uint32_t speakerFormID,
		std::chrono::steady_clock::time_point now, const std::function<bool(const char* reason)>& reopenFn);


StickyReopenProbe BuildStickyReopenProbe(RE::Actor* player, std::uint32_t speakerFormID);
bool HandleDialogueClosedFlow(const DialogueClosedContext& ctx, RE::Actor* player, std::uint32_t speakerFormID, const DialogueClosedHandlers& handlers);
bool TryHandleDialogueClosed(const DialogueClosedContext& ctx,
	const std::function<void()>& onTerminalCommit,
	const std::function<void()>& onPleasureBlocking,
	const std::function<void()>& onCaptiveCommit,
	const std::function<StickyReopenProbe()>& probeStickyReopen,
	const std::function<void(const StickyReopenProbe&)>& onStickyReopenForced,
	const std::function<void(const StickyReopenProbe&)>& onStickyReopenUnavailable);

	bool IsActive();
	bool OwnsCurrentFlow();
}
