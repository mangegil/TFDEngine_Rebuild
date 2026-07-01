#include "TFDDefeatBleedoutTickWiring.h"

#include <algorithm>
#include <chrono>

#include <spdlog/spdlog.h>

#include "TFDCaptive.h"
#include "TFDBleedout.h"
#include "TFDBleedoutGreet.h"
#include "TFDDefeatBattleObserveState.h"
#include "TFDDefeatBleedHealthGuard.h"
#include "TFDDefeatBleedoutDialogueWiring.h"
#include "TFDDefeatBleedoutRuntimeTimer.h"
#include "TFDDefeatBleedRuntimeState.h"
#include "TFDDefeatRuntimeActions.h"
#include "TFDDefeatStickyReopenGrace.h"
#include "TFDDialogueLifecycle.h"
#include "TFDPlayerBleedImmunityGuard.h"
#include "TFDPleasureRuntime.h"
#include "TFDSettings.h"
#include "TFDTame.h"

namespace TFD::DefeatBleedoutTickWiring
{
	namespace
	{
		using BleedDialogueOutcome = TFD::Bleedout::DialogueOutcome;

		static std::chrono::steady_clock::time_point Now()
		{
			return std::chrono::steady_clock::now();
		}

		static void MaintainBleedPrimaryCaptorBinding()
		{
			TFD::Bleedout::MaintainPrimaryCaptorBinding(
				TFD::DialogueLifecycle::IsDialogueOpen(),
				TFD::DefeatRuntimeActions::BuildBleedRuntimeHostStateRefs());
		}
	}

	bool Tick(RE::Actor* player)
	{
		if (!player || !TFD::DefeatBleedRuntimeState::IsInBleedState()) {
			return false;
		}

		if (TFD::DefeatBattleObserveState::Pending()) {
			TFD::Bleedout::RuntimeHost::TickBattleObservePending();
			return true;
		}
		if (TFD::DefeatBattleObserveState::Active()) {
			TFD::Bleedout::RuntimeHost::TickBattleObserve();
			return true;
		}
		if (TFD::DefeatStickyReopenGrace::Tick(player, Now())) {
			return true;
		}

		{
			const float runtimeSafeHp = TFD::DefeatBleedHealthGuard::ResolvePlayerBleedRuntimeSafeHealth(
				player,
				TFD::Settings::GetDefeatThresholdPct());
			TFD::DefeatBleedRuntimeState::MaxMinHp(runtimeSafeHp);
			TFD::PlayerBleedImmunityGuard::SetPlayerActive(true);
			TFD::DefeatBleedHealthGuard::ClampHealth(player, TFD::DefeatBleedRuntimeState::GetMinHp());
		}

		MaintainBleedPrimaryCaptorBinding();
		TFD::Bleedout::DefeatGlue::MaintainSpeakerKick();

		const bool dOpen = TFD::DialogueLifecycle::IsDialogueOpen();
		const int bleedSeconds = TFD::Settings::GetBleedWindowSeconds();
		const bool pleasureCommitted = TFD::Bleedout::GetDialogueOutcome() == BleedDialogueOutcome::Pleasure;
		const bool ostimBridgeBlocking = TFD::PleasureRuntime::IsBlocking();
		auto holdDecision = TFD::BleedoutGreet::EvaluateHold(dOpen, pleasureCommitted, ostimBridgeBlocking);
		if (holdDecision.hold) {
			const auto nowBleedHold = Now();
			if (dOpen) {
				TFD::DialogueLifecycle::ObserveBleedoutDialogueOpened("bleed_hold");
			}
			(void)nowBleedHold;
			TFD::DefeatBleedoutRuntimeTimer::Pause(TFD::BleedoutGreet::GetHoldReasonName(holdDecision.reason));
			TFD::DialogueLifecycle::SetDialogueOpenObserved(dOpen);
			return true;
		}

		// R130/R470A: no destructive Bleedout forcegreet timeout/rearm here.
		TFD::DefeatBleedoutRuntimeTimer::ResumeIfPaused();

		if (TFD::Bleedout::GetBleedSpeakerID() == 0) {
			const auto nowBleed = Now();
			const auto lastNoSpeakerAttempt = TFD::Tame::GetBleedNoSpeakerTameLastAttempt();
			if (lastNoSpeakerAttempt.time_since_epoch().count() == 0 ||
				(nowBleed - lastNoSpeakerAttempt) >= std::chrono::milliseconds(900)) {
				TFD::Tame::SetBleedNoSpeakerTameLastAttempt(nowBleed);
				const float bleedRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius());
				auto crowd = TFD::DefeatBleedoutDialogueWiring::CollectCrowd(bleedRadius, nullptr, false);
				const bool tameHeld = TFD::Tame::TryEnsureBleedNoSpeakerTameSession(crowd, "bleed_tick_no_speaker");
				if (tameHeld) {
					spdlog::info("[TFD][Defeat] bleed no-speaker tick tameHeld=1 crowdSize={}", crowd.size());
				}
			}
		}

		const bool bleedDialogueSeen = TFD::BleedoutGreet::HasSeenDialogue();
		const bool bleedTerminalCommit = TFD::Bleedout::HasTerminalCommit();
		const bool bleedPleasureBlocking = TFD::PleasureRuntime::IsBlocking();
		const bool bleedCaptiveOutcome = TFD::Bleedout::GetDialogueOutcome() == BleedDialogueOutcome::Captive;
		if (bleedDialogueSeen && TFD::DialogueLifecycle::WasDialogueOpen() && !dOpen) {
			TFD::DialogueLifecycle::SetDialogueOpenObserved(false);
			TFD::DefeatBleedoutRuntimeTimer::SetCountdownDirty();
			TFD::DefeatStickyReopenGrace::Clear("r470a_monitor_only_dialogue_closed");
			TFD::BleedoutGreet::MarkStickyReopenPending(false, "r470a_monitor_only_dialogue_closed");
			spdlog::info(
				"[TFD][Defeat][R470A] bleed dialogue closed observed monitor-only terminal={} pleasureBlocking={} captiveOutcome={} speaker={:08X}",
				bleedTerminalCommit ? 1 : 0,
				bleedPleasureBlocking ? 1 : 0,
				bleedCaptiveOutcome ? 1 : 0,
				TFD::Bleedout::GetBleedSpeakerID());

			if (bleedTerminalCommit || bleedPleasureBlocking || bleedCaptiveOutcome) {
				return true;
			}
		}

		if (TFD::Bleedout::IsAwaitingSystemEventOutcome() && !TFD::DialogueLifecycle::WasDialogueOpen()) {
			const char* pendingSystemReason = nullptr;
			(void)TFD::Bleedout::IsSystemEventPendingForFallback(&pendingSystemReason);
			if (pendingSystemReason && pendingSystemReason[0]) {
				spdlog::info(
					"[TFD][Defeat][R470A] pending bleed system event observed monitor-only reason={} terminal={} speaker={:08X}",
					pendingSystemReason,
					TFD::Bleedout::HasTerminalCommit() ? 1 : 0,
					TFD::Bleedout::GetBleedSpeakerID());
				return true;
			}
		}

		const bool suppressCaptiveRecaptureNotice =
			TFD::Captive::IsEscapeBleedoutActive() ||
			TFD::Captive::IsRecaptureCommitActive() ||
			TFD::Captive::IsRecaptureRecentlyCommitted();

		if (TFD::DefeatBleedoutRuntimeTimer::TickCountdown(TFD::DefeatBleedoutRuntimeTimer::CountdownTickInput{
			bleedSeconds,
			suppressCaptiveRecaptureNotice,
			[]() -> bool {
				const char* pendingSystemReason = nullptr;
				(void)TFD::Bleedout::IsSystemEventPendingForFallback(&pendingSystemReason);
				TFD::Bleedout::TimeoutContext timeoutContext{
					TFD::Bleedout::HasTerminalCommit(),
					TFD::Bleedout::GetTerminalCommitName(TFD::Bleedout::GetTerminalCommit()),
					pendingSystemReason,
					TFD::DefeatBleedRuntimeState::PendingCaptiveOutcome()
				};
				auto timeoutHandlers = TFD::Bleedout::Builders::BuildTimeoutHandlers();
				return TFD::Bleedout::HandleBleedTimeout(timeoutContext, "bleed_timeout", timeoutHandlers);
			}
		})) {
			return true;
		}

		return true;
	}
}
