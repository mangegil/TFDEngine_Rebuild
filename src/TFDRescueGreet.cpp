#include "TFDRescueGreet.h"

#include <atomic>
#include <mutex>

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

#include "TFDInteractionRouter.h"
#include "TFDRescue.h"

namespace TFD::RescueGreet
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
		return TFD::Rescue::IsActive();
	}

	void Install()
	{
		if (g_installed.exchange(true, std::memory_order_acq_rel)) {
			return;
		}

		spdlog::info("[TFD][RescueGreet] Install");
	}

	void ResetRuntime(const char* reason)
	{
		std::scoped_lock lk(g_runtime.lock);
		ResetRuntimeLocked();
		spdlog::info("[TFD][RescueGreet] runtime reset reason={}", reason ? reason : "unknown");
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
			spdlog::warn("[TFD][RescueGreet] begin ignored speaker={:08X} reason={} flowOwnerMismatch=1", formID, reason ? reason : "rescue");
			return false;
		}
		if (!speaker || speaker->IsDead() || speaker->IsDisabled()) {
			spdlog::warn("[TFD][RescueGreet] begin rejected speaker={:08X} reason={} invalidSpeaker=1", formID, reason ? reason : "rescue");
			return false;
		}

		Cancel("begin_replace");
		ResetRuntime("begin");
		g_state.store(State::Armed, std::memory_order_release);
		g_speakerFormID.store(formID, std::memory_order_release);
		TFD::InteractionRouter::DialogueOpen::BeginRescue(speaker);

		spdlog::info("[TFD][RescueGreet] Begin speaker={:08X} reason={}", formID, reason ? reason : "rescue");
		return true;
	}

	void Cancel(const char* reason)
	{
		const auto state = g_state.exchange(State::Idle, std::memory_order_acq_rel);
		const auto formID = g_speakerFormID.exchange(0, std::memory_order_acq_rel);
		ResetRuntime("cancel");

		if (TFD::InteractionRouter::DialogueOpen::IsActive() && TFD::InteractionRouter::DialogueOpen::GetMode() == TFD::InteractionRouter::DialogueOpen::Mode::Rescue) {
			TFD::InteractionRouter::DialogueOpen::Cancel();
		}

		if (state != State::Idle || formID != 0) {
			spdlog::info("[TFD][RescueGreet] Cancel speaker={:08X} reason={}", formID, reason ? reason : "unknown");
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
		default:
			return "Idle";
		}
	}

	std::uint32_t GetSpeakerFormID()
	{
		return g_speakerFormID.load(std::memory_order_acquire);
	}
}
