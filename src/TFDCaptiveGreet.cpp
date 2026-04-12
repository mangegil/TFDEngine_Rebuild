#include "TFDCaptiveGreet.h"

#include <atomic>
#include <mutex>

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

#include "TFDCaptive.h"
#include "TFDInteractionRouter.h"

namespace TFD::CaptiveGreet
{
	namespace
	{
		struct RuntimeState
		{
			std::mutex lock{};
			bool sawDialogue = false;
		};

		std::atomic_bool g_installed{ false };
		std::atomic<State> g_state{ State::Idle };
		std::atomic<std::uint32_t> g_speakerFormID{ 0 };
		RuntimeState g_runtime{};

		void ResetRuntimeLocked()
		{
			g_runtime.sawDialogue = false;
		}
	}

	bool OwnsCurrentFlow()
	{
		return TFD::Captive::IsStandardCaptiveActive();
	}

	void Install()
	{
		if (g_installed.exchange(true, std::memory_order_acq_rel)) {
			return;
		}

		spdlog::info("[TFD][CaptiveGreet] Install");
	}

	void ResetRuntime(const char* reason)
	{
		std::scoped_lock lk(g_runtime.lock);
		ResetRuntimeLocked();
		spdlog::info("[TFD][CaptiveGreet] runtime reset reason={}", reason ? reason : "unknown");
	}

	void Reset()
	{
		Cancel("reset");
		ResetRuntime("reset");
	}

	bool Begin(RE::Actor* speaker, const char* reason)
	{
		const auto formID = speaker ? speaker->GetFormID() : 0u;
		if (!OwnsCurrentFlow()) {
			spdlog::warn("[TFD][CaptiveGreet] begin ignored speaker={:08X} reason={} flowOwnerMismatch=1", formID, reason ? reason : "captive");
			return false;
		}
		if (!speaker || speaker->IsDead() || speaker->IsDisabled()) {
			spdlog::warn("[TFD][CaptiveGreet] begin rejected speaker={:08X} reason={} invalidSpeaker=1", formID, reason ? reason : "captive");
			return false;
		}

		Cancel("begin_replace");
		ResetRuntime("begin");
		g_state.store(State::Armed, std::memory_order_release);
		g_speakerFormID.store(formID, std::memory_order_release);
		TFD::InteractionRouter::DialogueOpen::BeginCaptiveMarker(speaker);

		spdlog::info("[TFD][CaptiveGreet] Begin speaker={:08X} reason={}", formID, reason ? reason : "captive");
		return true;
	}

	bool BeginAfterPleasure(RE::Actor* speaker, const char* reason)
	{
		const auto formID = speaker ? speaker->GetFormID() : 0u;
		if (!OwnsCurrentFlow()) {
			spdlog::warn("[TFD][CaptiveGreet] after pleasure ignored speaker={:08X} reason={} flowOwnerMismatch=1", formID, reason ? reason : "after_pleasure");
			return false;
		}
		if (!speaker || speaker->IsDead() || speaker->IsDisabled()) {
			spdlog::warn("[TFD][CaptiveGreet] after pleasure rejected speaker={:08X} reason={} invalidSpeaker=1", formID, reason ? reason : "after_pleasure");
			return false;
		}

		Cancel("begin_after_pleasure_replace");
		ResetRuntime("begin_after_pleasure");
		g_state.store(State::AfterPleasure, std::memory_order_release);
		g_speakerFormID.store(formID, std::memory_order_release);
		TFD::InteractionRouter::DialogueOpen::BeginAfterPleasure(speaker);

		spdlog::info("[TFD][CaptiveGreet] AfterPleasure speaker={:08X} reason={}", formID, reason ? reason : "after_pleasure");
		return true;
	}

	void Cancel(const char* reason)
	{
		const auto state = g_state.exchange(State::Idle, std::memory_order_acq_rel);
		const auto formID = g_speakerFormID.exchange(0, std::memory_order_acq_rel);
		ResetRuntime("cancel");

		if (TFD::InteractionRouter::DialogueOpen::IsActive()) {
			const auto mode = TFD::InteractionRouter::DialogueOpen::GetMode();
			if (mode == TFD::InteractionRouter::DialogueOpen::Mode::CaptiveMarker || mode == TFD::InteractionRouter::DialogueOpen::Mode::AfterPleasure) {
				TFD::InteractionRouter::DialogueOpen::Cancel();
			}
		}

		if (state != State::Idle || formID != 0) {
			spdlog::info("[TFD][CaptiveGreet] Cancel speaker={:08X} reason={}", formID, reason ? reason : "unknown");
		}
	}

	void NotifyDialogueOpened()
	{
		std::scoped_lock lk(g_runtime.lock);
		g_runtime.sawDialogue = true;
		if (g_state.load(std::memory_order_acquire) == State::Armed) {
			g_state.store(State::Running, std::memory_order_release);
		}
	}

	bool HasSeenDialogue()
	{
		std::scoped_lock lk(g_runtime.lock);
		return g_runtime.sawDialogue;
	}

	bool IsActive()
	{
		return g_state.load(std::memory_order_acquire) != State::Idle;
	}

	State GetState()
	{
		return g_state.load(std::memory_order_acquire);
	}

	const char* GetStateName()
	{
		switch (GetState()) {
		case State::Armed:
			return "Armed";
		case State::Running:
			return "Running";
		case State::AfterPleasure:
			return "AfterPleasure";
		default:
			return "Idle";
		}
	}

	std::uint32_t GetSpeakerFormID()
	{
		return g_speakerFormID.load(std::memory_order_acquire);
	}
}
