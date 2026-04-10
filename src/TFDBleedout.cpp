#include "TFDBleedout.h"

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <utility>
#include <thread>

#include "TFDFlowController.h"

namespace TFD::Bleedout
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		std::uint32_t ActorFormID(RE::Actor* actor)
		{
			return actor ? actor->GetFormID() : 0u;
		}

		DialogueOutcome g_dialogueOutcome = DialogueOutcome::None;
		std::atomic<std::uint8_t> g_terminalCommit{ static_cast<std::uint8_t>(TerminalCommit::None) };
		bool g_awaitingSystemEventOutcome = false;
		Clock::time_point g_systemEventUntil{};
		Clock::time_point g_systemEventLastDeferredLog{};
	}

	void Install()
	{
		spdlog::info("[TFD][Bleedout] Install");
	}

	void ResetForLoad()
	{
		ClearTerminalCommit("reset_for_load");
		spdlog::info("[TFD][Bleedout] ResetForLoad");
	}



	const char* GetTerminalCommitName(TerminalCommit kind)
	{
		switch (kind) {
		case TerminalCommit::PayRelease:
			return "pay_release";
		case TerminalCommit::Pleasure:
			return "pleasure";
		case TerminalCommit::Captive:
			return "captive";
		case TerminalCommit::NonCaptiveFallback:
			return "noncaptive_fallback";
		default:
			return "none";
		}
	}

	TerminalCommit GetTerminalCommit()
	{
		return static_cast<TerminalCommit>(g_terminalCommit.load(std::memory_order_acquire));
	}

	bool HasTerminalCommit()
	{
		return GetTerminalCommit() != TerminalCommit::None;
	}

	bool TryBeginTerminalCommit(TerminalCommit kind, const char* reason)
	{
		std::uint8_t expected = static_cast<std::uint8_t>(TerminalCommit::None);
		const auto desired = static_cast<std::uint8_t>(kind);
		if (g_terminalCommit.compare_exchange_strong(expected, desired, std::memory_order_acq_rel)) {
			spdlog::info("[TFD][Bleedout] terminal commit claimed kind={} reason={}",
				GetTerminalCommitName(kind),
				reason ? reason : "unknown");
			return true;
		}

		auto current = static_cast<TerminalCommit>(expected);
		spdlog::info("[TFD][Bleedout] terminal commit blocked requested={} active={} reason={}",
			GetTerminalCommitName(kind),
			GetTerminalCommitName(current),
			reason ? reason : "unknown");
		return false;
	}

	void ClearTerminalCommit(const char* reason)
	{
		const auto prev = static_cast<TerminalCommit>(g_terminalCommit.exchange(static_cast<std::uint8_t>(TerminalCommit::None), std::memory_order_acq_rel));
		if (prev != TerminalCommit::None) {
			spdlog::info("[TFD][Bleedout] terminal commit cleared previous={} reason={}",
				GetTerminalCommitName(prev),
				reason ? reason : "unknown");
		}
	}

	const char* GetDialogueOutcomeName(DialogueOutcome outcome)
	{
		switch (outcome) {
		case DialogueOutcome::PayRelease:
			return "pay_release";
		case DialogueOutcome::Pleasure:
			return "pleasure";
		case DialogueOutcome::Captive:
			return "captive";
		default:
			return "none";
		}
	}

	void SetDialogueOutcome(DialogueOutcome outcome, const char* reason)
	{
		g_dialogueOutcome = outcome;
		spdlog::info("[TFD][Bleedout] dialogue outcome set={} reason={}",
			GetDialogueOutcomeName(outcome),
			reason ? reason : "unknown");
	}

	void ClearDialogueOutcome(const char* reason)
	{
		if (g_dialogueOutcome != DialogueOutcome::None) {
			spdlog::info("[TFD][Bleedout] dialogue outcome cleared={} reason={}",
				GetDialogueOutcomeName(g_dialogueOutcome),
				reason ? reason : "unknown");
		}
		g_dialogueOutcome = DialogueOutcome::None;
	}

	DialogueOutcome GetDialogueOutcome()
	{
		return g_dialogueOutcome;
	}

	void ArmSystemEventOutcomeWindow(const char* reason, double seconds)
	{
		const auto now = Clock::now();
		const auto clamped = (std::max)(0.25, seconds);
		const auto until = now + std::chrono::milliseconds(static_cast<int>(clamped * 1000.0));
		if (!g_awaitingSystemEventOutcome || until > g_systemEventUntil) {
			g_systemEventUntil = until;
		}
		g_awaitingSystemEventOutcome = true;
		spdlog::info("[TFD][Bleedout] arm system-event outcome window seconds={:.2f} reason={}",
			clamped,
			reason ? reason : "unknown");
	}

	void ClearSystemEventOutcomeWindow(const char* reason)
	{
		if (g_awaitingSystemEventOutcome) {
			spdlog::info("[TFD][Bleedout] clear system-event outcome window reason={}", reason ? reason : "unknown");
		}
		g_awaitingSystemEventOutcome = false;
		g_systemEventUntil = {};
		g_systemEventLastDeferredLog = {};
	}

	bool IsSystemEventOutcomeWindowActive()
	{
		if (!g_awaitingSystemEventOutcome) {
			return false;
		}
		if (Clock::now() >= g_systemEventUntil) {
			g_awaitingSystemEventOutcome = false;
			g_systemEventUntil = {};
			g_systemEventLastDeferredLog = {};
			return false;
		}
		return true;
	}

	bool IsAwaitingSystemEventOutcome()
	{
		return g_awaitingSystemEventOutcome;
	}

	bool IsSystemEventPendingForFallback(const char** outReason)
	{
		const bool active = IsSystemEventOutcomeWindowActive();
		if (outReason) {
			*outReason = active ? "system_event_outcome_window" : nullptr;
		}
		return active;
	}

	bool ShouldLogDeferredSystemEvent(std::chrono::steady_clock::time_point now)
	{
		return g_systemEventLastDeferredLog.time_since_epoch().count() == 0 ||
			(now - g_systemEventLastDeferredLog) >= std::chrono::milliseconds(750);
	}

	void NoteDeferredSystemEventLog(std::chrono::steady_clock::time_point now)
	{
		g_systemEventLastDeferredLog = now;
	}

	void ResetSystemEventState(const char* reason)
	{
		ClearDialogueOutcome(reason ? reason : "reset");
		ClearSystemEventOutcomeWindow(reason ? reason : "reset");
		ClearTerminalCommit(reason ? reason : "reset");
	}

	bool ShouldDropSystemEventBecauseFallback(const char* eventName, bool terminalCommitActive, const char* terminalCommitName)
	{
		if (!terminalCommitActive) {
			return false;
		}
		spdlog::info("[TFD][Bleedout] drop system event={} activeTerminalCommit={}",
			eventName ? eventName : "unknown",
			terminalCommitName ? terminalCommitName : "unknown");
		return true;
	}

	
	bool HandleOutcomePayEvent(const OutcomeEventContext& context, const OutcomeEventHandlers& handlers)
	{
		if (handlers.shouldDropBecauseFallback &&
			handlers.shouldDropBecauseFallback(context.rawEventName ? context.rawEventName : context.eventName)) {
			return true;
		}
		if (handlers.armOutcomeWindow) {
			handlers.armOutcomeWindow("mod_event_pay", 3.0);
		}
		if (context.inBleedState) {
			if (handlers.setDialogueOutcome) {
				handlers.setDialogueOutcome(DialogueOutcome::PayRelease, "mod_event_pay");
			}
			(void)CommitDialogueOutcome(DialogueOutcome::PayRelease, context.actor, "mod_event_pay");
		}
		return true;
	}

	bool HandleOutcomePleasureEvent(const OutcomeEventContext& context, const OutcomeEventHandlers& handlers)
	{
		if (handlers.shouldDropBecauseFallback &&
			handlers.shouldDropBecauseFallback(context.rawEventName ? context.rawEventName : context.eventName)) {
			return true;
		}
		if (handlers.armOutcomeWindow) {
			handlers.armOutcomeWindow("mod_event_pleasure", 12.0);
		}
		if (!context.inBleedState) {
			return true;
		}

		if (context.preserveCaptive) {
			if (handlers.beginCaptivePleasureFlow &&
				!handlers.beginCaptivePleasureFlow(context.actorFormID, "mod_event_pleasure")) {
				spdlog::warn("[TFD][Bleedout] ignore mod_event_pleasure reason=captive_flow_reject actor={:08X}", context.actorFormID);
				return true;
			}
			if (handlers.setDialogueOutcome) {
				handlers.setDialogueOutcome(DialogueOutcome::Pleasure, "mod_event_pleasure");
			}
			if (handlers.prepareCaptivePleasureScene) {
				handlers.prepareCaptivePleasureScene("mod_event_pleasure");
			}
			if (handlers.completeCaptivePleasureHandoff) {
				handlers.completeCaptivePleasureHandoff("mod_event_pleasure");
			}
			return true;
		}

		if (!CommitDialogueOutcome(DialogueOutcome::Pleasure, context.actor, "mod_event_pleasure")) {
			spdlog::warn("[TFD][Bleedout] ignore mod_event_pleasure reason=flow_reject actor={:08X}", context.actorFormID);
			return true;
		}
		if (handlers.setDialogueOutcome) {
			handlers.setDialogueOutcome(DialogueOutcome::Pleasure, "mod_event_pleasure");
		}
		if (handlers.prepareBleedoutPleasureScene) {
			handlers.prepareBleedoutPleasureScene("mod_event_pleasure");
		}
		if (handlers.completeBleedPleasureHandoff) {
			handlers.completeBleedPleasureHandoff("mod_event_pleasure");
		}
		return true;
	}

	bool HandleOutcomeCaptiveEvent(const OutcomeEventContext& context, const OutcomeEventHandlers& handlers)
	{
		if (handlers.shouldDropBecauseFallback &&
			handlers.shouldDropBecauseFallback(context.rawEventName ? context.rawEventName : context.eventName)) {
			return true;
		}
		if (handlers.armOutcomeWindow) {
			handlers.armOutcomeWindow("mod_event_captive", 3.0);
		}
		if (context.inBleedState) {
			if (!CommitDialogueOutcome(DialogueOutcome::Captive, context.actor, "mod_event_captive")) {
				spdlog::warn("[TFD][Bleedout] ignore mod_event_captive reason=flow_reject actor={:08X}", context.actorFormID);
				return true;
			}
			if (handlers.setDialogueOutcome) {
				handlers.setDialogueOutcome(DialogueOutcome::Captive, "mod_event_captive");
			}
		}
		return true;
	}

	bool HandleOutcomeResetEvent(const OutcomeEventContext&, const OutcomeEventHandlers& handlers)
	{
		if (handlers.clearDialogueOutcome) {
			handlers.clearDialogueOutcome("mod_event_reset");
		}
		if (handlers.clearOutcomeWindow) {
			handlers.clearOutcomeWindow("mod_event_reset");
		}
		return true;
	}

	static float ResolveCalmRadius(const CompletionHandlers& handlers)
	{
		const float sweep = handlers.getSweepRadius ? handlers.getSweepRadius() : 0.0f;
		return (std::max)(2200.0f, sweep);
	}

	bool CompleteCaptivePleasureHandoff(const char* reason, const CompletionHandlers& handlers)
	{
		const char* why = reason ? reason : "captive_pleasure_handoff";
		if (handlers.clearOutcomeWindow) {
			handlers.clearOutcomeWindow(why);
		}
		if (handlers.tryBeginTerminalCommit && !handlers.tryBeginTerminalCommit(TerminalCommit::Pleasure, why)) {
			return false;
		}
		if (handlers.clearPendingCinematicFadeIn) {
			handlers.clearPendingCinematicFadeIn();
		}
		if (handlers.clearBridgeAliases) {
			handlers.clearBridgeAliases(why);
		}
		if (handlers.clearEscapeContext) {
			handlers.clearEscapeContext();
		}
		if (handlers.resetLockpickWatch) {
			handlers.resetLockpickWatch();
		}
		if (handlers.setGraceActive) {
			handlers.setGraceActive(false);
		}
		if (handlers.clearLastAggressor) {
			handlers.clearLastAggressor();
		}
		if (handlers.resetBleedRuntimeState) {
			handlers.resetBleedRuntimeState(true);
		}
		if (handlers.setPrevDialogueOpen) {
			handlers.setPrevDialogueOpen(false);
		}
		if (handlers.setPrevLockpickOpen) {
			handlers.setPrevLockpickOpen(false);
		}
		if (handlers.setCaptiveRuntime) {
			handlers.setCaptiveRuntime(true);
		}
		if (handlers.syncPlayerCaptiveAlias) {
			handlers.syncPlayerCaptiveAlias(why);
		}
		if (handlers.setPlayerBleedImmune) {
			handlers.setPlayerBleedImmune(false);
		}
		if (handlers.recoverPlayerForTransition) {
			handlers.recoverPlayerForTransition();
		}
		if (handlers.applyCalmBubble) {
			handlers.applyCalmBubble(ResolveCalmRadius(handlers));
		}
		if (handlers.updatePreCombatState) {
			handlers.updatePreCombatState();
		}
		if (handlers.beginPleasure) {
			handlers.beginPleasure(handlers.resolveRuntimeSpeaker ? handlers.resolveRuntimeSpeaker() : nullptr, true, why);
		}
		spdlog::info("[TFD][Bleedout] captive pleasure handoff complete reason={}", why);
		return true;
	}

	bool CompletePayRelease(const char* reason, const CompletionHandlers& handlers)
	{
		const char* why = reason ? reason : "bleed_pay_release";
		if (handlers.clearOutcomeWindow) {
			handlers.clearOutcomeWindow(why);
		}
		if (handlers.tryBeginTerminalCommit && !handlers.tryBeginTerminalCommit(TerminalCommit::PayRelease, why)) {
			return false;
		}
		if (handlers.clearPendingCinematicFadeIn) {
			handlers.clearPendingCinematicFadeIn();
		}
		if (handlers.clearBridgeAliases) {
			handlers.clearBridgeAliases(why);
		}
		if (handlers.clearFactionMask) {
			handlers.clearFactionMask();
		}
		if (handlers.clearEscapeContext) {
			handlers.clearEscapeContext();
		}
		if (handlers.resetLockpickWatch) {
			handlers.resetLockpickWatch();
		}
		if (handlers.setGraceActive) {
			handlers.setGraceActive(false);
		}
		if (handlers.clearLastAggressor) {
			handlers.clearLastAggressor();
		}
		if (handlers.resetBleedRuntimeState) {
			handlers.resetBleedRuntimeState(false);
		}
		if (handlers.setPrevDialogueOpen) {
			handlers.setPrevDialogueOpen(false);
		}
		if (handlers.setPrevLockpickOpen) {
			handlers.setPrevLockpickOpen(false);
		}
		if (handlers.setCaptiveRuntime) {
			handlers.setCaptiveRuntime(false);
		}
		if (handlers.setPlayerBleedImmune) {
			handlers.setPlayerBleedImmune(false);
		}
		if (handlers.recoverPlayerForTransition) {
			handlers.recoverPlayerForTransition();
		}
		if (handlers.applyCalmBubble) {
			handlers.applyCalmBubble(ResolveCalmRadius(handlers));
		}
		if (handlers.beginLeftForDeadCooldown) {
			handlers.beginLeftForDeadCooldown(8);
		}
		if (handlers.setGraceSeconds) {
			handlers.setGraceSeconds(8);
		}
		if (handlers.updatePreCombatState) {
			handlers.updatePreCombatState();
		}
		spdlog::info("[TFD][Bleedout] pay release complete reason={}", why);
		return true;
	}

	bool CompleteBleedPleasureHandoff(const char* reason, const CompletionHandlers& handlers)
	{
		const char* why = reason ? reason : "bleed_pleasure_handoff";
		if (handlers.clearOutcomeWindow) {
			handlers.clearOutcomeWindow(why);
		}
		if (handlers.tryBeginTerminalCommit && !handlers.tryBeginTerminalCommit(TerminalCommit::Pleasure, why)) {
			return false;
		}
		if (handlers.clearPendingCinematicFadeIn) {
			handlers.clearPendingCinematicFadeIn();
		}
		if (handlers.clearBridgeAliases) {
			handlers.clearBridgeAliases(why);
		}
		if (handlers.clearEscapeContext) {
			handlers.clearEscapeContext();
		}
		if (handlers.resetLockpickWatch) {
			handlers.resetLockpickWatch();
		}
		if (handlers.setGraceActive) {
			handlers.setGraceActive(false);
		}
		if (handlers.transitionBleedRuntimeToPleasureCommit) {
			handlers.transitionBleedRuntimeToPleasureCommit(why);
		}
		if (handlers.setPrevDialogueOpen) {
			handlers.setPrevDialogueOpen(false);
		}
		if (handlers.setPrevLockpickOpen) {
			handlers.setPrevLockpickOpen(false);
		}
		if (handlers.setCaptiveRuntime) {
			handlers.setCaptiveRuntime(false);
		}
		if (handlers.setPlayerBleedImmune) {
			handlers.setPlayerBleedImmune(false);
		}
		if (handlers.recoverPlayerForTransition) {
			handlers.recoverPlayerForTransition();
		}
		if (handlers.applyCalmBubble) {
			handlers.applyCalmBubble(ResolveCalmRadius(handlers));
		}
		if (handlers.beginLeftForDeadCooldown) {
			handlers.beginLeftForDeadCooldown(8);
		}
		if (handlers.setGraceSeconds) {
			handlers.setGraceSeconds(8);
		}
		if (handlers.refreshPostDefeatGlobals) {
			handlers.refreshPostDefeatGlobals();
		}
		if (handlers.updatePreCombatState) {
			handlers.updatePreCombatState();
		}
		if (handlers.beginPleasure) {
			handlers.beginPleasure(handlers.resolveRuntimeSpeaker ? handlers.resolveRuntimeSpeaker() : nullptr, false, why);
		}
		spdlog::info("[TFD][Bleedout] pleasure handoff complete reason={}", why);
		return true;
	}


	bool TryHandlePayReleaseDialogueClosed(bool seenDialogue, bool prevDialogueOpen, const DialogueCloseHandlers& handlers)
	{
		if (!seenDialogue || !prevDialogueOpen || GetDialogueOutcome() != DialogueOutcome::PayRelease) {
			return false;
		}
		if (handlers.clearDialogueOutcome) {
			handlers.clearDialogueOutcome("dialogue_closed_pay_release");
		}
		if (handlers.completePayRelease) {
			handlers.completePayRelease("dialogue_closed_pay_release");
		}
		return true;
	}

	bool TryResolvePostDialogueSystemEvent(const char* reason, const PendingSystemEventHandlers& handlers)
	{
		if (!IsAwaitingSystemEventOutcome()) {
			return false;
		}

		switch (GetDialogueOutcome()) {
		case DialogueOutcome::PayRelease:
			if (handlers.clearDialogueOutcome) {
				handlers.clearDialogueOutcome(reason ? reason : "system_event_pay");
			}
			if (handlers.completePayRelease) {
				handlers.completePayRelease(reason ? reason : "system_event_pay");
			}
			return true;

		case DialogueOutcome::Captive:
			if (handlers.clearOutcomeWindow) {
				handlers.clearOutcomeWindow(reason ? reason : "system_event_captive");
			}
			if (handlers.clearDialogueOutcome) {
				handlers.clearDialogueOutcome(reason ? reason : "system_event_captive");
			}
			if (handlers.releaseFlowHandoff) {
				handlers.releaseFlowHandoff();
			}
			if (handlers.resolveCaptiveMarker && handlers.resolveCaptiveMarker()) {
				spdlog::info("[TFD][Bleedout] post-dialogue system event committed captive -> captive marker found");
				if (handlers.doBlackoutTeleport) {
					handlers.doBlackoutTeleport();
				}
				if (handlers.setGraceSeconds) {
					handlers.setGraceSeconds(4);
				}
			} else {
				spdlog::info("[TFD][Bleedout] post-dialogue system event committed captive -> no marker -> resolve fallback");
				if (handlers.releaseNoSpeakerTameSession) {
					handlers.releaseNoSpeakerTameSession("system_event_captive_no_marker");
				}
				if (handlers.exitBleedState) {
					handlers.exitBleedState();
				}
				if (handlers.enterNonCaptiveChoice) {
					handlers.enterNonCaptiveChoice("system_event_captive_no_marker");
				}
			}
			return true;

		default:
			break;
		}

		return false;
	}

	bool TryHandlePendingSystemEventFallback(const PendingSystemEventContext& context, const char* reason, const PendingSystemEventHandlers& handlers)
	{
		const char* pendingReason = nullptr;
		if (IsSystemEventPendingForFallback(&pendingReason)) {
			const auto now = Clock::now();
			if (ShouldLogDeferredSystemEvent(now)) {
				NoteDeferredSystemEventLog(now);
				spdlog::info("[TFD][Bleedout] fallback deferred pendingSystemEvent={} outcome={} runtimePhase={} runtimeActive={} runtimeBlocking={}",
					pendingReason ? pendingReason : "unknown",
					GetDialogueOutcomeName(GetDialogueOutcome()),
					context.runtimePhaseName ? context.runtimePhaseName : "unknown",
					context.runtimeActive ? 1 : 0,
					context.runtimeBlocking ? 1 : 0);
			}
			return true;
		}

		ClearSystemEventOutcomeWindow(reason ? reason : "system_event_window_expired");
		constexpr int kMaxBleedDialogueNoCommitRetries = 1;
		int retryCount = handlers.getRetryCount ? handlers.getRetryCount() : 0;
		if (retryCount < kMaxBleedDialogueNoCommitRetries && handlers.promoteNextSpeaker &&
			handlers.promoteNextSpeaker(reason ? reason : "system_event_window_expired")) {
			++retryCount;
			if (handlers.setRetryCount) {
				handlers.setRetryCount(retryCount);
			}
			spdlog::info("[TFD][Bleedout] system-event window expired -> retry next truce candidate attempt={}/{}",
				retryCount,
				kMaxBleedDialogueNoCommitRetries);
			return true;
		}
		if (retryCount >= kMaxBleedDialogueNoCommitRetries) {
			spdlog::info("[TFD][Bleedout] system-event window expired -> retry cap reached, fallback to captive resolution");
		}
		if (handlers.setRetryCount) {
			handlers.setRetryCount(0);
		}
		if (handlers.resolveCaptiveMarker && handlers.resolveCaptiveMarker()) {
			spdlog::info("[TFD][Bleedout] system-event window expired -> fallback captive marker found");
			if (handlers.doBlackoutTeleport) {
				handlers.doBlackoutTeleport();
			}
			if (handlers.setGraceSeconds) {
				handlers.setGraceSeconds(4);
			}
		} else {
			spdlog::info("[TFD][Bleedout] system-event window expired -> fallback captive marker missing -> resolve fallback");
			if (handlers.releaseNoSpeakerTameSession) {
				handlers.releaseNoSpeakerTameSession("system_event_window_expired_no_marker");
			}
			if (handlers.exitBleedState) {
				handlers.exitBleedState();
			}
			if (handlers.enterNonCaptiveChoice) {
				handlers.enterNonCaptiveChoice("system_event_window_expired_no_marker");
			}
		}
		return true;
	}



	bool HandleBleedTimeout(const TimeoutContext& context, const char* reason, const TimeoutHandlers& handlers)
	{
		const char* why = reason ? reason : "bleed_timeout";
		if (context.hasTerminalCommit) {
			spdlog::info("[TFD][Bleedout] bleed timeout -> suppressed activeTerminalCommit={}",
				context.terminalCommitName ? context.terminalCommitName : "unknown");
			return true;
		}
		if (context.pendingSystemReason) {
			spdlog::info("[TFD][Bleedout] bleed timeout -> suppressed pendingSystemEvent={}",
				context.pendingSystemReason);
			return true;
		}
		if (handlers.releaseTruceGeneric) {
			handlers.releaseTruceGeneric();
		}
		if (context.pendingCaptive && handlers.resolveCaptiveMarker && handlers.resolveCaptiveMarker()) {
			if (handlers.resetBleedRuntimeState) {
				handlers.resetBleedRuntimeState();
			}
			spdlog::info("[TFD][Bleedout] bleed timeout -> captive blackout");
			if (handlers.doBlackoutTeleport) {
				handlers.doBlackoutTeleport();
			}
			if (handlers.setGraceSeconds) {
				handlers.setGraceSeconds(4);
			}
		} else {
			spdlog::info("[TFD][Bleedout] bleed timeout -> resolve no-marker fallback");
			if (handlers.enterNonCaptiveChoice) {
				handlers.enterNonCaptiveChoice(why);
			}
		}
		return true;
	}

	bool EnterNonCaptiveChoice(const char* reason, const NonCaptiveChoiceHandlers& handlers)
	{
		const char* why = reason ? reason : "unknown";
		if (handlers.hasBlockingCommit && handlers.hasBlockingCommit()) {
			spdlog::info("[TFD][Bleedout] non-captive choice suppressed activeTerminalCommit={} reason={}",
				handlers.getBlockingCommitName ? handlers.getBlockingCommitName() : "unknown",
				why);
			return false;
		}
		if (handlers.beginResolvedNoMarkerFallback && handlers.beginResolvedNoMarkerFallback(why)) {
			if (handlers.resetFlowRuntime) {
				handlers.resetFlowRuntime(why);
			}
			return true;
		}

		if (handlers.clearBridgeAliases) {
			handlers.clearBridgeAliases(why);
		}
		if (handlers.clearPendingCinematicFadeIn) {
			handlers.clearPendingCinematicFadeIn();
		}
		if (handlers.clearFactionMask) {
			handlers.clearFactionMask();
		}
		if (handlers.clearEscapeContext) {
			handlers.clearEscapeContext();
		}
		if (handlers.resetLockpickWatch) {
			handlers.resetLockpickWatch();
		}
		if (handlers.setGraceActive) {
			handlers.setGraceActive(false);
		}
		if (handlers.clearLastAggressor) {
			handlers.clearLastAggressor();
		}
		if (handlers.resetBleedRuntimeState) {
			handlers.resetBleedRuntimeState();
		}
		if (handlers.setPrevDialogueOpen) {
			handlers.setPrevDialogueOpen(false);
		}
		if (handlers.setPrevLockpickOpen) {
			handlers.setPrevLockpickOpen(false);
		}
		if (handlers.setCaptiveRuntime) {
			handlers.setCaptiveRuntime(false);
		}
		if (handlers.setPlayerBleedImmune) {
			handlers.setPlayerBleedImmune(false);
		}
		if (handlers.queueLegacyRequest) {
			handlers.queueLegacyRequest(why);
		}
		spdlog::warn("[TFD][Bleedout] fallback to legacy non-captive choice reason={}", why);
		return true;
	}

	bool DoBlackoutTeleport(const char* reason, const BlackoutHandlers& handlers)
	{
		const char* why = reason ? reason : "blackout_teleport";
		if (handlers.hasBlockingCommit && handlers.hasBlockingCommit()) {
			spdlog::info("[TFD][Bleedout] blackout teleport suppressed activeTerminalCommit={}",
				handlers.getBlockingCommitName ? handlers.getBlockingCommitName() : "unknown");
			return false;
		}
		if (handlers.resolveCaptiveMarker && !handlers.resolveCaptiveMarker()) {
			if (handlers.enterNonCaptiveChoice) {
				handlers.enterNonCaptiveChoice("marker_not_found");
			}
			return false;
		}
		if (handlers.tryBeginCaptiveCommit && !handlers.tryBeginCaptiveCommit(why)) {
			return false;
		}
		if (handlers.resetBleedRuntimeState) {
			handlers.resetBleedRuntimeState();
		}
		if (handlers.clearBridgeAliases) {
			handlers.clearBridgeAliases(why);
		}
		if (handlers.clearLastAggressor) {
			handlers.clearLastAggressor();
		}
		if (handlers.beginCaptiveFlow) {
			handlers.beginCaptiveFlow();
		}
		if (handlers.clearPendingCinematicFadeIn) {
			handlers.clearPendingCinematicFadeIn();
		}
		if (handlers.queueCaptiveFadeTransition && handlers.queueCaptiveFadeTransition()) {
			return true;
		}
		if (handlers.showBlackoutFader) {
			handlers.showBlackoutFader();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		if (handlers.completeCaptiveTransitionNow) {
			handlers.completeCaptiveTransitionNow("captive_blackout_fallback");
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		if (handlers.hideBlackoutFader) {
			handlers.hideBlackoutFader();
		}
		return true;
	}


	bool BeginWindow(RE::Actor* speaker, const char* reason)
	{
		auto& flow = TFD::Flow::Controller::GetSingleton();
		const bool ok = flow.BeginPlayerBleedoutDecision(ActorFormID(speaker), reason ? reason : "bleed_window_start");
		spdlog::info("[TFD][Bleedout] BeginWindow actor={:08X} ok={} reason={}", ActorFormID(speaker), ok ? 1 : 0, reason ? reason : "bleed_window_start");
		return ok;
	}

	bool ResolvePay(RE::Actor* actor, const char* reason)
	{
		auto& flow = TFD::Flow::Controller::GetSingleton();
		const auto actorFormID = ActorFormID(actor);
		if (!flow.ResolveBleedoutOutcome(TFD::Flow::BleedoutOutcome::Pay, actorFormID, reason ? reason : "bleedout_pay")) {
			return false;
		}
		return flow.CompleteTerminalContext(reason ? reason : "bleedout_pay");
	}

	bool ResolvePleasure(RE::Actor* actor, const char* reason)
	{
		auto& flow = TFD::Flow::Controller::GetSingleton();
		return flow.ResolveBleedoutOutcome(TFD::Flow::BleedoutOutcome::Pleasure, ActorFormID(actor), reason ? reason : "bleedout_pleasure");
	}

	bool ResolveCaptive(RE::Actor* actor, const char* reason)
	{
		auto& flow = TFD::Flow::Controller::GetSingleton();
		return flow.ResolveBleedoutOutcome(TFD::Flow::BleedoutOutcome::Captive, ActorFormID(actor), reason ? reason : "bleedout_captive");
	}

	bool CommitDialogueOutcome(DialogueOutcome outcome, RE::Actor* actor, const char* reason)
	{
		switch (outcome) {
		case DialogueOutcome::PayRelease:
			return ResolvePay(actor, reason ? reason : "bleedout_pay");
		case DialogueOutcome::Pleasure:
			return ResolvePleasure(actor, reason ? reason : "bleedout_pleasure");
		case DialogueOutcome::Captive:
			return ResolveCaptive(actor, reason ? reason : "bleedout_captive");
		default:
			break;
		}
		return false;
	}

	bool BeginAfterPleasure(RE::Actor* actor, const char* reason)
	{
		auto& flow = TFD::Flow::Controller::GetSingleton();
		return flow.BeginAfterPleasure(ActorFormID(actor), reason ? reason : "bleedout_after_pleasure");
	}

	bool CompleteAfterPleasure(const char* reason)
	{
		auto& flow = TFD::Flow::Controller::GetSingleton();
		return flow.CompleteAfterPleasure(reason ? reason : "bleedout_after_pleasure_complete");
	}

	bool HandleAfterPleasureEnter(RE::Actor* actor, const char* reason)
	{
		return BeginAfterPleasure(actor, reason ? reason : "after_pleasure_enter");
	}

	bool IsActive()
	{
		return TFD::Flow::Controller::GetSingleton().IsBleedDecisionActive();
	}

	bool OwnsCurrentFlow()
	{
		const auto snapshot = TFD::Flow::Controller::GetSingleton().GetSnapshot();
		return snapshot.root == TFD::Flow::RootFlow::Bleedout ||
			snapshot.contextRoot == TFD::Flow::RootFlow::Bleedout ||
			snapshot.sub == TFD::Flow::SubFlow::BleedoutPleasure ||
			snapshot.sub == TFD::Flow::SubFlow::BleedoutAfterPleasure;
	}

	std::uint32_t ResolveActorFormID(RE::Actor* actor)
	{
		return ActorFormID(actor);
	}
}
