#include "TFDInCombatGreet.h"

#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <string_view>

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

#include "TFDInteractionRouter.h"
#include "TFDHostilityController.h"
#include "TFDDefeatMonitor.h"
#include "SKSE/SKSE.h"
#include "TFDInCombat.h"

namespace TFD::InCombatGreet
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		struct RuntimeState
		{
			std::mutex lock{};
			bool sawDialogue = false;
			bool stickyReopenPending = false;
			Clock::time_point nextRetry{};
			int retryCount = 0;
			RE::FormID pacifySessionId = 0;
			bool assignSent = false;
		};

		std::atomic_bool g_installed{ false };
		std::atomic<State> g_state{ State::Idle };
		std::atomic<std::uint32_t> g_speakerFormID{ 0 };
		RuntimeState g_runtime{};

		double NowSec()
		{
			return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
		}

		void SendBridgeEvent(const char* eventName, RE::TESForm* sender)
		{
			if (!eventName) {
				return;
			}

			auto* task = SKSE::GetTaskInterface();
			if (!task) {
				return;
			}

			const std::string name{ eventName };
			std::uint32_t handle = 0;
			if (auto* actor = sender ? sender->As<RE::Actor>() : nullptr) {
				handle = actor->GetHandle().native_handle();
			}

			task->AddTask([name, handle]() {
				RE::TESForm* outSender = nullptr;
				if (handle != 0) {
					auto sp = RE::Actor::LookupByHandle(handle);
					outSender = sp.get();
					if (!outSender) {
						return;
					}
				}

				auto* src = SKSE::GetModCallbackEventSource();
				if (!src) {
					return;
				}

				SKSE::ModCallbackEvent ev{ name.c_str(), "", 0.0f, outSender };
				src->SendEvent(&ev);
			});
		}

		bool IsCandidate(RE::Actor* actor, RE::PlayerCharacter* player)
		{
			if (!actor || !player) {
				return false;
			}
			if (actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
				return false;
			}
			if (actor->GetFormID() == player->GetFormID()) {
				return false;
			}
			if (TFD::DefeatMonitor::IsLeftForDeadRecoveryActive()) {
				return false;
			}
			return true;
		}

		TFD::HostilityController::ReleaseReason ResolveReleaseReason(const char* reason)
		{
			const auto text = reason ? std::string_view(reason) : std::string_view{};
			if (text.find("dialogue_closed") != std::string_view::npos) {
				return TFD::HostilityController::ReleaseReason::DialogueClosed;
			}
			if (text.find("handoff") != std::string_view::npos) {
				return TFD::HostilityController::ReleaseReason::FlowHandoff;
			}
			return TFD::HostilityController::ReleaseReason::Generic;
		}

		void ResetRuntimeLocked()
		{
			g_runtime.sawDialogue = false;
			g_runtime.stickyReopenPending = false;
			g_runtime.nextRetry = {};
			g_runtime.retryCount = 0;
			g_runtime.pacifySessionId = 0;
			g_runtime.assignSent = false;
		}

		void ReleaseTrackedSession(TFD::HostilityController::ReleaseReason releaseReason)
		{
			RE::FormID sessionId = 0;
			bool assignSent = false;
			{
				std::scoped_lock lk(g_runtime.lock);
				sessionId = g_runtime.pacifySessionId;
				assignSent = g_runtime.assignSent;
				g_runtime.pacifySessionId = 0;
				g_runtime.assignSent = false;
			}

			const auto speakerFormID = g_speakerFormID.load(std::memory_order_acquire);
			auto* speaker = speakerFormID != 0 ? RE::TESForm::LookupByID<RE::Actor>(speakerFormID) : nullptr;
			if (assignSent) {
				if (speaker) {
					SendBridgeEvent("TFDInCombatClear", speaker);
				} else {
					SendBridgeEvent("TFDInCombatClearAll", nullptr);
				}
			}
			if (sessionId != 0) {
				TFD::HostilityController::ReleaseSession(sessionId, releaseReason);
			}
		}

		bool CompleteDialogueClosedInternal(const char* reason)
		{
			return TFD::InCombat::CompleteDialogueClosedFlow(
				reason,
				TFD::InCombat::CompletionHandlers{
					[](const char* r) { TFD::InCombat::ClearDialogueOutcome(r); },
					[](const char* r) { TFD::InCombat::Complete(r); },
					[](const char* r) { TFD::InCombatGreet::CancelAll(r); } });
		}

		bool TryBeginStickyReopen(std::uint32_t speakerFormID, const char* reason)
		{
			auto* reopenSpeaker = speakerFormID != 0 ? RE::TESForm::LookupByID<RE::Actor>(speakerFormID) : nullptr;
			if (!reopenSpeaker) {
				return false;
			}
			return TFD::InCombatGreet::Begin(reopenSpeaker, reason);
		}
	}

	bool OwnsCurrentFlow()
	{
		return TFD::InCombat::IsActive();
	}

	void Install()
	{
		if (g_installed.exchange(true, std::memory_order_acq_rel)) {
			return;
		}

		spdlog::info("[TFD][InCombatGreet] Install");
	}

	void ResetRuntime(const char* reason)
	{
		std::scoped_lock lk(g_runtime.lock);
		ResetRuntimeLocked();
		spdlog::info("[TFD][InCombatGreet] runtime reset reason={}", reason ? reason : "unknown");
	}

	void Reset()
	{
		ReleaseTrackedSession(TFD::HostilityController::ReleaseReason::Generic);
		g_state.store(State::Idle, std::memory_order_release);
		g_speakerFormID.store(0, std::memory_order_release);
		ResetRuntime("reset");
		spdlog::info("[TFD][InCombatGreet] Reset");
	}

	bool BeginForActor(RE::Actor* speaker, TFD::InteractionRouter::Action* outAction)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (outAction) {
			*outAction = TFD::InteractionRouter::Action::None;
		}
		if (!IsCandidate(speaker, player)) {
			return false;
		}

		const auto now = NowSec();
		const auto result = TFD::InteractionRouter::HandleHotkeyPress(player, speaker, false, now);
		if (outAction) {
			*outAction = result.action;
		}

		spdlog::info(
			"[TFD][InCombatGreet] BeginForActor actor={:08X} action={} executed={} dialogueRequested={} fail={}",
			speaker ? speaker->GetFormID() : 0u,
			TFD::InteractionRouter::ToString(result.action),
			result.executed ? 1 : 0,
			result.dialogueRequested ? 1 : 0,
			TFD::InteractionRouter::ToString(result.failReason));

		if (!result.executed || result.sessionId == 0 || result.action != TFD::InteractionRouter::Action::TruceInCombat) {
			if (result.sessionId != 0 && result.action != TFD::InteractionRouter::Action::TruceInCombat) {
				TFD::HostilityController::ReleaseSession(result.sessionId, TFD::HostilityController::ReleaseReason::Generic);
			}
			return false;
		}

		if (result.dialogueRequested && !TFD::HostilityController::CanOpenDialogue(speaker)) {
			TFD::HostilityController::ReleaseSession(result.sessionId, TFD::HostilityController::ReleaseReason::Generic);
			return false;
		}

		CancelAll("begin_replace");

		if (!TFD::InCombat::BeginTruce(speaker->GetFormID(), result.dialogueRequested, "incombat_truce_begin")) {
			TFD::HostilityController::ReleaseSession(result.sessionId, TFD::HostilityController::ReleaseReason::Generic);
			return false;
		}

		if (result.dialogueRequested) {
			if (player && player->IsInCombat()) {
				player->StopCombat();
			}
			SendBridgeEvent("TFDInCombatAssign", speaker);
			if (!Begin(speaker, "incombat_dialogue_begin")) {
				SendBridgeEvent("TFDInCombatClear", speaker);
				TFD::HostilityController::ReleaseSession(result.sessionId, TFD::HostilityController::ReleaseReason::Generic);
				TFD::InCombat::Complete("incombat_greet_begin_failed");
				return false;
			}
		}
		else {
			ResetRuntime("begin_no_dialogue");
			g_speakerFormID.store(speaker->GetFormID(), std::memory_order_release);
		}

		{
			std::scoped_lock lk(g_runtime.lock);
			g_runtime.pacifySessionId = result.sessionId;
			g_runtime.assignSent = result.dialogueRequested;
		}

		return true;
	}

	bool Begin(RE::Actor* speaker, const char* reason)
	{
		const auto formID = speaker ? speaker->GetFormID() : 0u;
		if (!OwnsCurrentFlow()) {
			spdlog::warn("[TFD][InCombatGreet] begin ignored speaker={:08X} reason={} flowOwnerMismatch=1", formID, reason ? reason : "incombat");
			return false;
		}

		ResetRuntime("begin");
		g_state.store(State::Armed, std::memory_order_release);
		g_speakerFormID.store(formID, std::memory_order_release);
		TFD::InteractionRouter::DialogueOpen::BeginInCombatTruce(speaker);

		spdlog::info("[TFD][InCombatGreet] Begin speaker={:08X} reason={}", formID, reason ? reason : "incombat");
		return true;
	}

	bool BeginAfterPleasure(RE::Actor* speaker, const char* reason)
	{
		const auto formID = speaker ? speaker->GetFormID() : 0u;
		if (!OwnsCurrentFlow()) {
			spdlog::warn("[TFD][InCombatGreet] after pleasure ignored speaker={:08X} reason={} flowOwnerMismatch=1", formID, reason ? reason : "after_pleasure");
			return false;
		}

		ResetRuntime("begin_after_pleasure");
		g_state.store(State::AfterPleasure, std::memory_order_release);
		g_speakerFormID.store(formID, std::memory_order_release);
		TFD::InteractionRouter::DialogueOpen::BeginAfterPleasure(speaker);

		spdlog::info("[TFD][InCombatGreet] AfterPleasure speaker={:08X} reason={}", formID, reason ? reason : "after_pleasure");
		return true;
	}

	void CancelAll(const char* reason)
	{
		const auto formID = g_speakerFormID.exchange(0, std::memory_order_acq_rel);
		ReleaseTrackedSession(ResolveReleaseReason(reason));
		ResetRuntime("cancel");
		TFD::InteractionRouter::DialogueOpen::Cancel();
		g_state.store(State::Idle, std::memory_order_release);

		spdlog::info("[TFD][InCombatGreet] CancelAll speaker={:08X} reason={}", formID, reason ? reason : "-");
	}

	void NotifyDialogueOpened()
	{
		std::scoped_lock lk(g_runtime.lock);
		g_runtime.sawDialogue = true;
		g_runtime.stickyReopenPending = false;

		if (g_state.load(std::memory_order_acquire) == State::Armed) {
			g_state.store(State::Running, std::memory_order_release);
		}
	}

	bool HasSeenDialogue()
	{
		std::scoped_lock lk(g_runtime.lock);
		return g_runtime.sawDialogue;
	}

	void MarkStickyReopenPending(bool value, const char* reason)
	{
		std::scoped_lock lk(g_runtime.lock);
		g_runtime.stickyReopenPending = value;
		if (!value) {
			g_runtime.nextRetry = {};
		}
		spdlog::info("[TFD][InCombatGreet] sticky pending={} reason={}", value ? 1 : 0, reason ? reason : "unknown");
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
		spdlog::info("[TFD][InCombatGreet] sticky retry scheduled retry={} reason={}", g_runtime.retryCount, reason ? reason : "unknown");
	}

	int GetRetryCount()
	{
		std::scoped_lock lk(g_runtime.lock);
		return g_runtime.retryCount;
	}

	bool TryStickyWatchdog(bool prevDialogueOpen,
		bool dialogueOpen,
		bool pleasureBlocking,
		std::uint32_t speakerFormID,
		std::chrono::steady_clock::time_point now,
		const std::function<bool(const char* reason)>& reopenFn)
	{
		if (!prevDialogueOpen || !HasStickyReopenPending() || dialogueOpen || pleasureBlocking || speakerFormID == 0) {
			return false;
		}
		if (!IsRetryDue(now)) {
			return false;
		}
		if (reopenFn && reopenFn("sticky_watchdog_reopen")) {
			NoteStickyRetry(now + std::chrono::milliseconds(900), "sticky_watchdog_reopen");
			spdlog::info("[TFD][InCombatGreet] sticky watchdog reopen speaker={:08X} retry={}",
				speakerFormID,
				GetRetryCount());
			return true;
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

	bool HandleDialogueClosedFlow(const DialogueClosedContext& ctx,
		RE::Actor* player,
		std::uint32_t speakerFormID,
		const DialogueClosedHandlers& handlers)
	{
		if (!ctx.seenDialogue || !ctx.prevDialogueOpen) {
			return false;
		}
		if (ctx.pleasureBlocking || ctx.afterPleasure) {
			if (handlers.onAfterPleasureHold) {
				handlers.onAfterPleasureHold();
			}
			return true;
		}
		auto probe = BuildStickyReopenProbe(player, speakerFormID);
		if (probe.ready) {
			MarkStickyReopenPending(true, "dialogue_closed_sticky_reopen");
			if (handlers.onStickyReopenForced) {
				handlers.onStickyReopenForced(probe);
			}
			return true;
		}
		if (handlers.onStickyReopenUnavailable) {
			handlers.onStickyReopenUnavailable(probe);
		}
		if (handlers.onComplete) {
			handlers.onComplete();
		}
		return true;
	}

	bool TickDialogueRuntime(bool& prevDialogueOpen,
		bool dialogueOpen,
		bool pleasureBlocking,
		RE::Actor* player)
	{
		if (dialogueOpen) {
			NotifyDialogueOpened();
			return false;
		}

		const auto now = Clock::now();
		const auto speakerFormID = TFD::InCombat::GetPrimaryActorFormID();

		if (TryStickyWatchdog(
			prevDialogueOpen,
			dialogueOpen,
			pleasureBlocking,
			speakerFormID,
			now,
			[&](const char* reopenReason) -> bool {
				return TryBeginStickyReopen(speakerFormID, reopenReason);
			})) {
			prevDialogueOpen = false;
			return true;
		}

		if (HandleDialogueClosedFlow(
			DialogueClosedContext{
				HasSeenDialogue(),
				prevDialogueOpen,
				pleasureBlocking,
				TFD::InCombat::GetState() == TFD::InCombat::State::AfterPleasure },
				player,
				speakerFormID,
				DialogueClosedHandlers{
					[&]() {
						prevDialogueOpen = false;
						spdlog::info("[TFD][InCombatGreet] dialogue closed -> hold after pleasure/runtime");
					},
					[&]() {
						prevDialogueOpen = false;
						(void)CompleteDialogueClosedInternal("dialogue_closed_complete");
						spdlog::info("[TFD][InCombatGreet] dialogue closed -> complete");
					},
					[&](const StickyReopenProbe& probe) {
						prevDialogueOpen = false;
						if (TryBeginStickyReopen(probe.speakerFormID, "dialogue_closed_sticky_reopen")) {
							spdlog::info("[TFD][InCombatGreet] dialogue closed -> sticky reopen speaker={:08X} dist={:.1f}",
								probe.speakerFormID,
								probe.distance);
						}
 else {
  (void)CompleteDialogueClosedInternal("dialogue_closed_sticky_reopen_unavailable");
  spdlog::warn("[TFD][InCombatGreet] dialogue closed -> sticky reopen missing speaker={:08X}",
	  probe.speakerFormID);
}
},
[&](const StickyReopenProbe& probe) {
	prevDialogueOpen = false;
	(void)CompleteDialogueClosedInternal("dialogue_closed_no_sticky_reopen");
	spdlog::info("[TFD][InCombatGreet] dialogue closed -> complete no sticky reopen speaker={:08X} loaded={} dead={} dist={:.1f}",
		probe.speakerFormID,
		probe.loaded ? 1 : 0,
		probe.dead ? 1 : 0,
		probe.distance);
}
			})) {
			return true;
		}

		return false;
	}

	bool IsRunning()
	{
		return GetState() != State::Idle;
	}

	State GetState()
	{
		return g_state.load(std::memory_order_acquire);
	}

	const char* GetStateName()
	{
		switch (GetState()) {
		case State::Idle:
			return "Idle";
		case State::Armed:
			return "Armed";
		case State::Running:
			return "Running";
		case State::AfterPleasure:
			return "AfterPleasure";
		default:
			return "Unknown";
		}
	}
}