#include "TFDPreCombat.h"

#include "RE/Skyrim.h"
#include "SKSE/API.h"
#include "SKSE/Events.h"
#include "SKSE/SKSE.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include <spdlog/spdlog.h>

#include "TFDActorScan.h"
#include "TFDDefeatMonitor.h"
#include "TFDFactionMask.h"
#include "TFDInteractionRouter.h"
#include "TFDPacify.h"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace TFD::PreCombat
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		struct Config
		{
			int enforceTickMs = 120;
			int hotkeyCooldownMs = 250;
		};

		struct Session
		{
			RE::ActorHandle target;
			RE::FormID pacifySessionId = 0;
			State state = State::Idle;
			TFD::InteractionRouter::Action action = TFD::InteractionRouter::Action::None;
			Clock::time_point started{};
			bool dialogueRequested = false;
			bool sawDialogueOpen = false;
			bool sentAssignEvent = false;
		};

		Config g_cfg{};
		Session g_session{};

		std::atomic_bool g_installed{ false };
		std::atomic_bool g_running{ false };
		std::atomic_flag g_tickPending = ATOMIC_FLAG_INIT;
		std::thread g_worker{};

		bool g_hotkeyWasDown = false;
		Clock::time_point g_lastHotkeyTry{};
		Clock::time_point g_t0 = Clock::now();

		RE::Actor* GetTarget()
		{
			auto sp = g_session.target.get();
			return sp.get();
		}

		double NowSec()
		{
			return std::chrono::duration<double>(Clock::now() - g_t0).count();
		}

		bool IsAnyBlockingMenuOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return false;
			}

			if (ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) return true;
			if (ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) return true;
			if (ui->IsMenuOpen(RE::Console::MENU_NAME)) return true;
			return false;
		}

		bool IsDialogueOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return false;
			}

			return ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME) || ui->IsMenuOpen("Dialogue Menu");
		}

		bool SendModEvent(const char* eventName, RE::TESForm* sender = nullptr, const char* strArg = "", float numArg = 0.0f)
		{
			if (!eventName) {
				return false;
			}

			auto* source = SKSE::GetModCallbackEventSource();
			if (!source) {
				spdlog::warn("[TFD][PreCombat] ModCallbackEventSource missing for {}", eventName);
				return false;
			}

			SKSE::ModCallbackEvent ev{};
			ev.eventName = RE::BSFixedString(eventName);
			ev.strArg = RE::BSFixedString(strArg ? strArg : "");
			ev.numArg = numArg;
			ev.sender = sender;
			source->SendEvent(&ev);
			return true;
		}

		bool IsCaptiveLikeBlocked()
		{
			return TFD::DefeatMonitor::IsPreCombatBlocked();
		}

		bool IsActorStillValid(RE::Actor* actor)
		{
			if (!actor) return false;
			if (actor->IsDead()) return false;
			if (actor->IsDisabled()) return false;
			if (!actor->Is3DLoaded()) return false;
			return true;
		}

		void BeginRestore()
		{
			if (g_session.state == State::Idle || g_session.state == State::Restoring) {
				return;
			}
			g_session.state = State::Restoring;
		}

		void FinishRestore()
		{
			auto* actor = GetTarget();

			if (g_session.pacifySessionId != 0) {
				TFD::Pacify::ReleaseSession(g_session.pacifySessionId);
			}

			if (actor && g_session.sentAssignEvent) {
				SendModEvent("TFDPreCombatClear", actor);
			}

			g_session = {};
			g_session.state = State::Idle;
		}

		bool StartSessionFromResult(RE::Actor* actor, const TFD::InteractionRouter::ExecuteResult& result)
		{
			if (!actor) {
				return false;
			}

			if (!result.executed || result.sessionId == 0) {
				return false;
			}

			g_session = {};
			g_session.target = actor->GetHandle();
			g_session.pacifySessionId = result.sessionId;
			g_session.state = State::Claimed;
			g_session.action = result.action;
			g_session.started = Clock::now();
			g_session.dialogueRequested = result.dialogueRequested;
			g_session.sawDialogueOpen = false;
			g_session.sentAssignEvent = false;

			if (result.dialogueRequested) {
				if (!TFD::Pacify::CanOpenDialogue(actor)) {
					spdlog::warn(
						"[TFD][PreCombat] router requested dialogue but CanOpenDialogue=false actor={:08X}",
						actor->GetFormID());

					TFD::Pacify::ReleaseSession(result.sessionId);
					g_session = {};
					g_session.state = State::Idle;
					return false;
				}

				SendModEvent("TFDPreCombatAssign", actor);
				g_session.sentAssignEvent = true;
			}

			spdlog::info(
				"[TFD][PreCombat] started action={} target={:08X} dialogueRequested={}",
				TFD::InteractionRouter::ToString(result.action),
				actor->GetFormID(),
				result.dialogueRequested ? 1 : 0);

			return true;
		}

		class HitSink : public RE::BSTEventSink<RE::TESHitEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* e, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				if (!e || g_session.state == State::Idle) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* player = RE::PlayerCharacter::GetSingleton();
				auto* claimed = GetTarget();
				if (!player || !claimed) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* target = e->target.get();
				auto* cause = e->cause.get();
				if (!target || !cause) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* causeActor = cause->As<RE::Actor>();
				if (!causeActor) {
					return RE::BSEventNotifyControl::kContinue;
				}

				if (causeActor->GetFormID() == player->GetFormID() &&
					target->GetFormID() == claimed->GetFormID()) {
					spdlog::info("[TFD][PreCombat] player attacked active target -> restore");
					BeginRestore();
				}

				return RE::BSEventNotifyControl::kContinue;
			}
		};

		HitSink g_hitSink{};

		void PollHotkeyAndMaybeStart()
		{
			if (IsAnyBlockingMenuOpen()) {
				g_hotkeyWasDown = false;
				return;
			}

			const bool keyDown = (::GetAsyncKeyState('H') & 0x8000) != 0;
			const bool pressedNow = keyDown && !g_hotkeyWasDown;
			g_hotkeyWasDown = keyDown;

			if (!pressedNow) {
				return;
			}

			if (g_session.state != State::Idle) {
				return;
			}

			const auto now = Clock::now();
			if (g_lastHotkeyTry.time_since_epoch().count() != 0) {
				if (now - g_lastHotkeyTry < std::chrono::milliseconds(g_cfg.hotkeyCooldownMs)) {
					return;
				}
			}
			g_lastHotkeyTry = now;

			TryStartFromHotkey();
		}

		void TickUI()
		{
			struct Guard
			{
				~Guard() { g_tickPending.clear(std::memory_order_release); }
			} guard;

			PollHotkeyAndMaybeStart();

			TFD::Pacify::Update(NowSec());

			if (g_session.state == State::Idle) {
				return;
			}

			if (IsCaptiveLikeBlocked() && g_session.state != State::InDialogue) {
				spdlog::info("[TFD][PreCombat] captive-like blocked -> restore");
				BeginRestore();
			}

			auto* actor = GetTarget();
			if (g_session.state != State::Restoring && !IsActorStillValid(actor)) {
				BeginRestore();
			}

			actor = GetTarget();
			if (g_session.state != State::Restoring && !actor) {
				g_session = {};
				g_session.state = State::Idle;
				return;
			}

			if (g_session.state != State::Restoring && actor && !TFD::Pacify::IsPacified(actor)) {
				BeginRestore();
			}

			switch (g_session.state) {
			case State::Claimed:
			{
				if (!actor) {
					BeginRestore();
					break;
				}

				if (g_session.dialogueRequested) {
					if (IsDialogueOpen()) {
						g_session.sawDialogueOpen = true;
						g_session.state = State::InDialogue;
						spdlog::info("[TFD][PreCombat] dialogue opened");
					}
				}
				else {
					// V1 Tame path: kalau creature sudah balik combat, lepas session lokal.
					if (actor->IsInCombat()) {
						spdlog::info("[TFD][PreCombat] tame target entered combat -> restore");
						BeginRestore();
					}
				}
				break;
			}
			case State::InDialogue:
				if (!IsDialogueOpen()) {
					spdlog::info("[TFD][PreCombat] dialogue closed -> restore");
					BeginRestore();
				}
				break;
			case State::Restoring:
				FinishRestore();
				break;
			default:
				break;
			}
		}

		void WorkerLoop()
		{
			while (g_running.load(std::memory_order_acquire)) {
				std::this_thread::sleep_for(std::chrono::milliseconds(80));

				if (g_tickPending.test_and_set(std::memory_order_acq_rel)) {
					continue;
				}

				if (auto* tasks = SKSE::GetTaskInterface()) {
					tasks->AddUITask([]() {
						TickUI();
						});
				}
				else {
					g_tickPending.clear(std::memory_order_release);
				}
			}
		}
	}

	void Install()
	{
		if (g_installed.exchange(true, std::memory_order_acq_rel)) {
			return;
		}

		if (auto* scripts = RE::ScriptEventSourceHolder::GetSingleton()) {
			scripts->AddEventSink<RE::TESHitEvent>(&g_hitSink);
		}

		g_running.store(true, std::memory_order_release);
		g_worker = std::thread(WorkerLoop);

		spdlog::info("[TFD][PreCombat] installed");
	}

	void Shutdown()
	{
		g_running.store(false, std::memory_order_release);

		if (g_worker.joinable()) {
			g_worker.join();
		}

		TFD::Pacify::ReleaseAll();

		g_installed.store(false, std::memory_order_release);
		spdlog::info("[TFD][PreCombat] shutdown");
	}

	bool IsActive()
	{
		return g_session.state != State::Idle;
	}

	State GetState()
	{
		return g_session.state;
	}

	bool TryStartFromHotkey()
	{
		if (IsCaptiveLikeBlocked()) {
			spdlog::info("[TFD][PreCombat] blocked by captive-like state");
			return false;
		}

		if (IsActive()) {
			return false;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return false;
		}

		auto* target = TFD::ActorScan::GetBestPreCombatCandidate();
		if (!target) {
			spdlog::info("[TFD][PreCombat] no precombat candidate");
			return false;
		}

		const double nowSec = NowSec();
		const bool isCaptivePhase = false;  // V1: CallingCaptor belum masuk jalur ini.

		const auto result = TFD::InteractionRouter::HandleHotkeyPress(
			player,
			target,
			isCaptivePhase,
			nowSec);

		spdlog::info(
			"[TFD][PreCombat] H target={:08X} action={} executed={} dialogueRequested={} fail={}",
			target->GetFormID(),
			TFD::InteractionRouter::ToString(result.action),
			result.executed ? 1 : 0,
			result.dialogueRequested ? 1 : 0,
			TFD::InteractionRouter::ToString(result.failReason));

		if (!result.executed) {
			return false;
		}

		return StartSessionFromResult(target, result);
	}

	void CancelAndRestore()
	{
		if (!IsActive()) {
			return;
		}
		BeginRestore();
	}

	const char* GetStateName()
	{
		switch (g_session.state) {
		case State::Idle:
			return "Idle";
		case State::Claimed:
			return "Claimed";
		case State::InDialogue:
			return "InDialogue";
		case State::Restoring:
			return "Restoring";
		default:
			return "Unknown";
		}
	}
}