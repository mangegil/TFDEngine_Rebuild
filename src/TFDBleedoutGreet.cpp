#include "TFDBleedoutGreet.h"

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <mutex>
#include <utility>

#include "TFDBleedout.h"
#include "TFDInteractionRouter.h"
#include "TFDPayModel.h"
#include "TFDPleasureRuntime.h"

namespace TFD::BleedoutGreet
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		struct RuntimeState
		{
			std::mutex lock{};
			bool sawDialogue = false;
			bool flowGreetConfirmed = false;
			std::uint32_t flowGreetSpeakerFormID = 0;
			bool initialHandoffArmed = false;
			Clock::time_point initialHandoffNextRetry{};
			int initialHandoffRetryCount = 0;
			bool stickyReopenPending = false;
			Clock::time_point nextRetry{};
			int retryCount = 0;

			bool afterPleasureArmed = false;
			bool afterPleasureSawDialogue = false;
			bool afterPleasureReopenPending = false;
			Clock::time_point afterPleasureNextRetry{};
			int afterPleasureRetryCount = 0;
		};

		RuntimeState g_runtime{};

		bool HasBleedoutOwnership()
		{
			return TFD::Bleedout::OwnsCurrentFlow();
		}

		void ResetRuntimeLocked()
		{
			g_runtime.sawDialogue = false;
			g_runtime.flowGreetConfirmed = false;
			g_runtime.flowGreetSpeakerFormID = 0;
			g_runtime.initialHandoffArmed = false;
			g_runtime.initialHandoffNextRetry = {};
			g_runtime.initialHandoffRetryCount = 0;
			g_runtime.stickyReopenPending = false;
			g_runtime.nextRetry = {};
			g_runtime.retryCount = 0;

			g_runtime.afterPleasureArmed = false;
			g_runtime.afterPleasureSawDialogue = false;
			g_runtime.afterPleasureReopenPending = false;
			g_runtime.afterPleasureNextRetry = {};
			g_runtime.afterPleasureRetryCount = 0;
		}

		bool HasNativeBleedoutOpenSucceeded()
		{
			return TFD::InteractionRouter::DialogueOpen::DidSucceed() &&
				TFD::InteractionRouter::DialogueOpen::GetMode() == TFD::InteractionRouter::DialogueOpen::Mode::Bleedout;
		}

		HoldDecision EvaluateHoldInternal(bool dialogueOpen, bool pleasureCommitted, bool ostimBridgeBlocking)
		{
			if (dialogueOpen) {
				return { true, HoldReason::Dialogue };
			}
			if (ostimBridgeBlocking) {
				return { true, HoldReason::OStimBridge };
			}
			if (pleasureCommitted) {
				return { true, HoldReason::PleasureCommit };
			}
			if (TFD::InteractionRouter::DialogueOpen::IsActive() && TFD::InteractionRouter::DialogueOpen::GetMode() == TFD::InteractionRouter::DialogueOpen::Mode::Bleedout) {
				return { true, HoldReason::DialogueReopen };
			}
			return { false, HoldReason::None };
		}
	}

	void Install()
	{
		spdlog::info("[TFD][BleedoutGreet] Install");
	}

	void Reset()
	{
		ResetRuntime("reset");
		if (TFD::InteractionRouter::DialogueOpen::IsActive() && OwnsCurrentFlow()) {
			TFD::InteractionRouter::DialogueOpen::Cancel();
		}
	}

	void ResetRuntime(const char* reason)
	{
		std::scoped_lock lk(g_runtime.lock);
		ResetRuntimeLocked();
		spdlog::info("[TFD][BleedoutGreet] runtime reset reason={}", reason ? reason : "unknown");
	}

	bool Begin(RE::Actor* speaker, const char* reason)
	{
		const auto speakerFormID = speaker ? speaker->GetFormID() : 0u;
		if (!HasBleedoutOwnership()) {
			spdlog::warn("[TFD][BleedoutGreet] begin ignored speaker={:08X} reason={} flowOwnerMismatch=1", speakerFormID, reason ? reason : "bleedout");
			return false;
		}

		// Bleedout uses the same shared pay text/global pipeline as PreCombat and InCombat.
		// Without this, the CK line that displays <Global=TFDPayGold> can stay at 0.
		bool quotePrimed = false;
		bool payPublished = false;
		if (speaker) {
			quotePrimed = TFD::PayModel::PrimeEncounterQuote(speaker, TFD::PayModel::PayContext::Bleedout);
			payPublished = TFD::PayModel::PublishSharedGold(speaker, TFD::PayModel::PayContext::Bleedout, "bleedout_dialogue_begin");
		}

		{
			std::scoped_lock lk(g_runtime.lock);
			g_runtime.flowGreetConfirmed = false;
			g_runtime.flowGreetSpeakerFormID = 0;
			g_runtime.initialHandoffArmed = speaker != nullptr;
			g_runtime.initialHandoffNextRetry = Clock::now() + std::chrono::milliseconds(1200);
			g_runtime.initialHandoffRetryCount = 0;
		}

		TFD::InteractionRouter::DialogueOpen::BeginBleedout(speaker);
		spdlog::info(
			"[TFD][BleedoutGreet][R100P] begin speaker={:08X} quotePrimed={} payPublished={} gold={} reason={}",
			speakerFormID,
			quotePrimed ? 1 : 0,
			payPublished ? 1 : 0,
			TFD::PayModel::GetCachedEncounterQuote(speaker, TFD::PayModel::PayContext::Bleedout),
			reason ? reason : "bleedout");
		return true;
	}

	bool BeginAfterPleasure(RE::Actor* speaker, const char* reason)
	{
		if (!HasBleedoutOwnership()) {
			spdlog::warn("[TFD][BleedoutGreet] after pleasure ignored speaker={:08X} reason={} flowOwnerMismatch=1", speaker ? speaker->GetFormID() : 0u, reason ? reason : "after_pleasure");
			return false;
		}
		MarkAfterPleasureArmed(reason ? reason : "after_pleasure");
		TFD::InteractionRouter::DialogueOpen::BeginAfterPleasure(speaker);
		spdlog::info("[TFD][BleedoutGreet] after pleasure speaker={:08X} reason={}", speaker ? speaker->GetFormID() : 0u, reason ? reason : "after_pleasure");
		return true;
	}

	void Cancel(const char* reason)
	{
		if (TFD::InteractionRouter::DialogueOpen::IsActive()) {
			spdlog::info("[TFD][BleedoutGreet] cancel reason={}", reason ? reason : "unknown");
			TFD::InteractionRouter::DialogueOpen::Cancel();
		}
	}

	void NotifyDialogueOpened()
	{
		std::scoped_lock lk(g_runtime.lock);
		g_runtime.sawDialogue = true;
		g_runtime.stickyReopenPending = false;
		if (g_runtime.afterPleasureArmed) {
			g_runtime.afterPleasureSawDialogue = true;
			g_runtime.afterPleasureReopenPending = false;
			g_runtime.afterPleasureNextRetry = {};
		}
	}

	void NotifyFlowGreetConfirmed(RE::Actor* speaker, const char* reason)
	{
		const auto speakerFormID = speaker ? speaker->GetFormID() : 0u;
		std::scoped_lock lk(g_runtime.lock);
		g_runtime.sawDialogue = true;
		g_runtime.flowGreetConfirmed = true;
		g_runtime.flowGreetSpeakerFormID = speakerFormID;
		g_runtime.initialHandoffArmed = false;
		g_runtime.initialHandoffNextRetry = {};
		g_runtime.stickyReopenPending = false;
		spdlog::info("[TFD][BleedoutGreet][CB07] flow greet confirmed speaker={:08X} reason={}",
			speakerFormID,
			reason ? reason : "unknown");
	}

	void MarkAfterPleasureArmed(const char* reason)
	{
		std::scoped_lock lk(g_runtime.lock);
		g_runtime.afterPleasureArmed = true;
		g_runtime.afterPleasureSawDialogue = false;
		g_runtime.afterPleasureReopenPending = false;
		g_runtime.afterPleasureNextRetry = {};
		g_runtime.afterPleasureRetryCount = 0;
		spdlog::info("[TFD][BleedoutGreet] after pleasure armed reason={}", reason ? reason : "unknown");
	}

	bool HasSeenDialogue()
	{
		std::scoped_lock lk(g_runtime.lock);
		return g_runtime.sawDialogue;
	}

	bool HasFlowGreetConfirmed()
	{
		std::scoped_lock lk(g_runtime.lock);
		return g_runtime.flowGreetConfirmed;
	}

	bool TryInitialHandoffWatchdog(bool hasTerminalCommit, bool dialogueOpen, bool pleasureBlocking, std::uint32_t speakerFormID,
		std::chrono::steady_clock::time_point now, const std::function<bool(const char* reason)>& reopenFn)
	{
		if (!HasBleedoutOwnership() || hasTerminalCommit || dialogueOpen || pleasureBlocking || speakerFormID == 0 || !reopenFn) {
			return false;
		}
		if (HasNativeBleedoutOpenSucceeded()) {
			spdlog::info("[TFD][BleedoutGreet][CB07] initial handoff retry suppressed reason=native_open_succeeded speaker={:08X}", speakerFormID);
			return false;
		}

		if (TFD::InteractionRouter::DialogueOpen::DidSucceed()) {
			std::scoped_lock lk(g_runtime.lock);
			g_runtime.initialHandoffArmed = false;
			g_runtime.initialHandoffNextRetry = {};
			spdlog::info("[TFD][BleedoutGreet][CB07] initial handoff watchdog suppressed reason=native_dialogue_open_succeeded speaker={:08X}", speakerFormID);
			return false;
		}

		bool shouldRetry = false;
		int retry = 0;
		{
			std::scoped_lock lk(g_runtime.lock);
			if (g_runtime.flowGreetConfirmed || !g_runtime.initialHandoffArmed) {
				return false;
			}
			if (g_runtime.initialHandoffRetryCount >= 4) {
				return false;
			}
			if (g_runtime.initialHandoffNextRetry.time_since_epoch().count() != 0 && now < g_runtime.initialHandoffNextRetry) {
				return false;
			}
			++g_runtime.initialHandoffRetryCount;
			retry = g_runtime.initialHandoffRetryCount;
			g_runtime.initialHandoffNextRetry = now + std::chrono::milliseconds(900);
			shouldRetry = true;
		}

		if (!shouldRetry) {
			return false;
		}

		const bool ok = reopenFn("initial_handoff_retry");
		spdlog::info("[TFD][BleedoutGreet][CB07] initial handoff watchdog {} speaker={:08X} retry={}",
			ok ? "reopen" : "unavailable",
			speakerFormID,
			retry);
		return ok;
	}

	void MarkStickyReopenPending(bool value, const char* reason)
	{
		std::scoped_lock lk(g_runtime.lock);
		g_runtime.stickyReopenPending = value;
		if (!value) {
			g_runtime.nextRetry = {};
		}
		spdlog::info("[TFD][BleedoutGreet] sticky pending={} reason={}", value ? 1 : 0, reason ? reason : "unknown");
	}

	bool HasStickyReopenPending()
	{
		std::scoped_lock lk(g_runtime.lock);
		return g_runtime.stickyReopenPending;
	}

	bool IsRetryDue(std::chrono::steady_clock::time_point now)
	{
		std::scoped_lock lk(g_runtime.lock);
		return g_runtime.nextRetry.time_since_epoch().count() == 0 || now >= g_runtime.nextRetry;
	}

	void NoteStickyRetry(std::chrono::steady_clock::time_point nextRetry, const char* reason)
	{
		std::scoped_lock lk(g_runtime.lock);
		++g_runtime.retryCount;
		g_runtime.nextRetry = nextRetry;
		g_runtime.stickyReopenPending = true;
		spdlog::info("[TFD][BleedoutGreet] sticky retry scheduled retry={} reason={}", g_runtime.retryCount, reason ? reason : "unknown");
	}

	int GetRetryCount()
	{
		std::scoped_lock lk(g_runtime.lock);
		return g_runtime.retryCount;
	}

	HoldDecision EvaluateHold(bool dialogueOpen, bool pleasureCommitted, bool ostimBridgeBlocking)
	{
		return EvaluateHoldInternal(dialogueOpen, pleasureCommitted, ostimBridgeBlocking);
	}

	const char* GetHoldReasonName(HoldReason reason)
	{
		switch (reason) {
		case HoldReason::Dialogue:
			return "dialogue";
		case HoldReason::DialogueReopen:
			return "dialogue_reopen";
		case HoldReason::OStimBridge:
			return "ostim_bridge";
		case HoldReason::PleasureCommit:
			return "pleasure_commit";
		default:
			return "none";
		}
	}


	bool TryStickyWatchdog(bool hasTerminalCommit, bool dialogueOpen, bool pleasureBlocking, std::uint32_t speakerFormID,
		std::chrono::steady_clock::time_point now, const std::function<bool(const char* reason)>& reopenFn)
	{
		if (!HasStickyReopenPending() || hasTerminalCommit || dialogueOpen || pleasureBlocking) {
			return false;
		}
		if (HasNativeBleedoutOpenSucceeded()) {
			MarkStickyReopenPending(false, "cb07_native_open_succeeded");
			spdlog::info("[TFD][BleedoutGreet][CB07] sticky watchdog suppressed reason=native_open_succeeded speaker={:08X}", speakerFormID);
			return false;
		}
		if (!IsRetryDue(now)) {
			return false;
		}
		if (reopenFn && reopenFn("sticky_watchdog_reopen")) {
			NoteStickyRetry(now + std::chrono::milliseconds(900), "sticky_watchdog_reopen");
			spdlog::info("[TFD][BleedoutGreet] sticky watchdog reopen speaker={:08X} retry={}",
				speakerFormID,
				GetRetryCount());
			return true;
		}
		if (speakerFormID != 0) {
			NoteStickyRetry(now + std::chrono::milliseconds(900), "sticky_watchdog_unavailable");
			spdlog::info("[TFD][BleedoutGreet] sticky watchdog unavailable speaker={:08X} retry={}",
				speakerFormID,
				GetRetryCount());
		}
		return false;
	}


StickyReopenProbe BuildStickyReopenProbe(RE::Actor* player, std::uint32_t speakerFormID)
{
	StickyReopenProbe probe{};
	probe.speakerFormID = speakerFormID;
	auto* reopenSpeaker = speakerFormID != 0 ? RE::TESForm::LookupByID<RE::Actor>(speakerFormID) : nullptr;
	if (!player || !reopenSpeaker) {
		return probe;
	}
	probe.loaded = reopenSpeaker->Is3DLoaded();
	probe.dead = reopenSpeaker->IsDead(false);
	if (probe.dead || !probe.loaded) {
		return probe;
	}
	const auto pp = player->GetPosition();
	const auto ap = reopenSpeaker->GetPosition();
	const float dx = ap.x - pp.x;
	const float dy = ap.y - pp.y;
	const float dz = ap.z - pp.z;
	probe.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
	probe.ready = probe.distance <= 2048.0f;
	return probe;
}

bool HandleDialogueClosedFlow(const DialogueClosedContext& ctx, RE::Actor* player, std::uint32_t speakerFormID, const DialogueClosedHandlers& handlers)
{
	return TryHandleDialogueClosed(
		ctx,
		handlers.onTerminalCommit,
		handlers.onPleasureBlocking,
		handlers.onCaptiveCommit,
		[&]() { return BuildStickyReopenProbe(player, speakerFormID); },
		handlers.onStickyReopenForced,
		handlers.onStickyReopenUnavailable);
}

bool TryHandleDialogueClosed(const DialogueClosedContext& ctx,
	const std::function<void()>& onTerminalCommit,
	const std::function<void()>& onPleasureBlocking,
	const std::function<void()>& onCaptiveCommit,
	const std::function<StickyReopenProbe()>& probeStickyReopen,
	const std::function<void(const StickyReopenProbe&)>& onStickyReopenForced,
	const std::function<void(const StickyReopenProbe&)>& onStickyReopenUnavailable)
{
	if (!ctx.seenDialogue || !ctx.prevDialogueOpen) {
		return false;
	}
	if (ctx.hasTerminalCommit) {
		if (onTerminalCommit) {
			onTerminalCommit();
		}
		return true;
	}
	if (ctx.pleasureBlocking) {
		if (onPleasureBlocking) {
			onPleasureBlocking();
		}
		return true;
	}
	if (ctx.captiveOutcome) {
		if (onCaptiveCommit) {
			onCaptiveCommit();
		}
		return true;
	}
	StickyReopenProbe probe{};
	if (probeStickyReopen) {
		probe = probeStickyReopen();
	}
	if (probe.ready) {
		MarkStickyReopenPending(true, "dialogue_closed_sticky_reopen");
		if (onStickyReopenForced) {
			onStickyReopenForced(probe);
		}
		return true;
	}
	if (onStickyReopenUnavailable) {
		onStickyReopenUnavailable(probe);
	}
	return true;
}

	bool TryAfterPleasureWatchdog(bool prevDialogueOpen, bool dialogueOpen, std::uint32_t speakerFormID,
		std::chrono::steady_clock::time_point now, const std::function<bool(const char* reason)>& reopenFn)
	{
		if (!HasBleedoutOwnership() || speakerFormID == 0 || !reopenFn) {
			return false;
		}
		if (TFD::PleasureRuntime::GetPhase() != TFD::PleasureRuntime::Phase::AfterPleasureDialogue) {
			return false;
		}

		bool shouldAttempt = false;
		int retry = 0;
		{
			std::scoped_lock lk(g_runtime.lock);
			if (!g_runtime.afterPleasureArmed) {
				return false;
			}
			if (dialogueOpen) {
				g_runtime.afterPleasureSawDialogue = true;
				g_runtime.afterPleasureReopenPending = false;
				g_runtime.afterPleasureNextRetry = {};
				return false;
			}
			if (prevDialogueOpen && g_runtime.afterPleasureSawDialogue && !g_runtime.afterPleasureReopenPending) {
				g_runtime.afterPleasureReopenPending = true;
				g_runtime.afterPleasureNextRetry = now;
				spdlog::info("[TFD][BleedoutGreet] after pleasure dialogue closed -> watchdog armed speaker={:08X}", speakerFormID);
			}
			if (!g_runtime.afterPleasureReopenPending || g_runtime.afterPleasureRetryCount >= 6) {
				return false;
			}
			if (g_runtime.afterPleasureNextRetry.time_since_epoch().count() != 0 && now < g_runtime.afterPleasureNextRetry) {
				return false;
			}
			++g_runtime.afterPleasureRetryCount;
			retry = g_runtime.afterPleasureRetryCount;
			g_runtime.afterPleasureNextRetry = now + std::chrono::milliseconds(450);
			shouldAttempt = true;
		}
		if (!shouldAttempt) {
			return false;
		}
		const bool ok = reopenFn("after_pleasure_watchdog_reopen");
		spdlog::info("[TFD][BleedoutGreet] after pleasure watchdog {} speaker={:08X} retry={}", ok ? "reopen" : "unavailable", speakerFormID, retry);
		return ok;
	}

	bool IsActive()
	{
		return TFD::InteractionRouter::DialogueOpen::IsActive() && OwnsCurrentFlow();
	}

	bool OwnsCurrentFlow()
	{
		if (!HasBleedoutOwnership()) {
			return false;
		}
		const auto mode = TFD::InteractionRouter::DialogueOpen::GetMode();
		return mode == TFD::InteractionRouter::DialogueOpen::Mode::Bleedout || mode == TFD::InteractionRouter::DialogueOpen::Mode::AfterPleasure;
	}
}
