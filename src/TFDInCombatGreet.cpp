#include "TFDInCombatGreet.h"

#include <atomic>
#include <cmath>
#include <mutex>

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

#include "TFDForceGreet.h"
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
		};

		std::atomic_bool g_installed{ false };
		std::atomic<State> g_state{ State::Idle };
		std::atomic<std::uint32_t> g_speakerFormID{ 0 };
		RuntimeState g_runtime{};

		void ResetRuntimeLocked()
		{
			g_runtime.sawDialogue = false;
			g_runtime.stickyReopenPending = false;
			g_runtime.nextRetry = {};
			g_runtime.retryCount = 0;
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
		g_state.store(State::Idle, std::memory_order_release);
		g_speakerFormID.store(0, std::memory_order_release);
		ResetRuntime("reset");
		spdlog::info("[TFD][InCombatGreet] Reset");
	}

	bool Begin(RE::Actor* speaker, const char* reason)
	{
		const auto formID = speaker ? speaker->GetFormID() : 0u;
		if (!OwnsCurrentFlow()) {
			spdlog::warn("[TFD][InCombatGreet] begin ignored speaker={:08X} reason={} flowOwnerMismatch=1", formID, reason ? reason : "incombat");
			return false;
		}
		g_state.store(State::Armed, std::memory_order_release);
		g_speakerFormID.store(formID, std::memory_order_release);
		TFD::ForceGreet::BeginPreCombatTruce(speaker);
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
		g_state.store(State::AfterPleasure, std::memory_order_release);
		g_speakerFormID.store(formID, std::memory_order_release);
		TFD::ForceGreet::BeginAfterPleasure(speaker);
		spdlog::info("[TFD][InCombatGreet] AfterPleasure speaker={:08X} reason={}", formID, reason ? reason : "after_pleasure");
		return true;
	}

	void CancelAll(const char* reason)
	{
		const auto formID = g_speakerFormID.exchange(0, std::memory_order_acq_rel);
		g_state.store(State::Idle, std::memory_order_release);
		spdlog::info("[TFD][InCombatGreet] CancelAll speaker={:08X} reason={}", formID, reason ? reason : "-");
	}

	void NotifyDialogueOpened()
	{
		std::scoped_lock lk(g_runtime.lock);
		g_runtime.sawDialogue = true;
		g_runtime.stickyReopenPending = false;
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
