#include "TFDPreCombat.h"

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"
#include "SKSE/API.h"
#include "SKSE/Events.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include <spdlog/spdlog.h>

#include "TFDActorScan.h"
#include "TFDAntiAggro.h"
#include "TFDFactionMask.h"

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
			float claimRadius = 1800.0f;
			int enforceTickMs = 120;
			int claimTimeoutMs = 8000;
			int hotkeyCooldownMs = 250;
		};

		struct Session
		{
			RE::ActorHandle target;
			State state = State::Idle;
			std::int32_t originalAggression = -1;
			Clock::time_point started{};
			Clock::time_point lastEnforce{};
			bool sawDialogueOpen = false;
		};

		Config g_cfg{};
		Session g_session{};
		std::atomic_bool g_installed{ false };
		std::atomic_bool g_running{ false };
		std::atomic_flag g_tickPending = ATOMIC_FLAG_INIT;
		std::thread g_worker{};
		bool g_hotkeyWasDown = false;
		Clock::time_point g_lastHotkeyTry{};

		static constexpr auto kPreCombatTruceFactionEditorID = "TFDPreCombatTruceFaction";
		RE::TESFaction* g_preCombatTruceFaction = nullptr;

		RE::Actor* GetTarget();
		void BeginRestore();

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

				if (causeActor->GetFormID() == player->GetFormID() && target->GetFormID() == claimed->GetFormID()) {
					spdlog::info("[TFD][PreCombat] player attacked claimed actor -> restore");
					BeginRestore();
				}

				return RE::BSEventNotifyControl::kContinue;
			}
		};

		HitSink g_hitSink{};

		RE::Actor* GetTarget()
		{
			auto sp = g_session.target.get();
			return sp.get();
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

		bool ResolveForms()
		{
			if (!g_preCombatTruceFaction) {
				g_preCombatTruceFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>(kPreCombatTruceFactionEditorID);
				if (g_preCombatTruceFaction) {
					spdlog::info("[TFD][PreCombat] truce faction resolved {:08X}", g_preCombatTruceFaction->GetFormID());
				} else {
					spdlog::warn("[TFD][PreCombat] truce faction missing (EditorID='{}')", kPreCombatTruceFactionEditorID);
				}
			}

			return g_preCombatTruceFaction != nullptr;
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
			if (TFD::FactionMask::IsActive()) {
				return true;
			}
			if (IsDialogueOpen()) {
				return true;
			}
			return false;
		}

		bool IsActorStillValid(RE::Actor* actor)
		{
			if (!actor) return false;
			if (actor->IsDead()) return false;
			if (actor->IsDisabled()) return false;
			if (!actor->Is3DLoaded()) return false;
			return true;
		}

		void ApplyLock(RE::Actor* actor)
		{
			if (!actor) {
				return;
			}

			ResolveForms();

			if (g_preCombatTruceFaction) {
				TFD::AntiAggro::EnsureFactionRank(actor, g_preCombatTruceFaction, 1);
			}

			TFD::AntiAggro::SetAggression(actor, 0);
			TFD::AntiAggro::StopCombatHard(actor);
			TFD::AntiAggro::SheatheIfNeeded(actor);
			actor->EvaluatePackage(true);
		}

		void RestoreActor(RE::Actor* actor)
		{
			if (!actor) {
				return;
			}

			if (g_preCombatTruceFaction) {
				TFD::AntiAggro::RemoveFactionIfPresent(actor, g_preCombatTruceFaction);
			}

			if (g_session.originalAggression >= 0) {
				TFD::AntiAggro::SetAggression(actor, g_session.originalAggression);
			}

			TFD::AntiAggro::StopCombatHard(actor);
			actor->EvaluatePackage(true);
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
			if (actor) {
				SendModEvent("TFDPreCombatClear", actor);
				RestoreActor(actor);
			}

			g_session = {};
			g_session.state = State::Idle;
		}

		bool StartClaim(RE::Actor* actor)
		{
			if (!IsActorStillValid(actor)) {
				return false;
			}

			if (IsCaptiveLikeBlocked()) {
				return false;
			}

			if (g_session.state != State::Idle) {
				return false;
			}

			g_session.target = actor->GetHandle();
			g_session.state = State::Claimed;
			g_session.originalAggression = TFD::AntiAggro::GetAggression(actor);
			g_session.started = Clock::now();
			g_session.lastEnforce = Clock::time_point{};
			g_session.sawDialogueOpen = false;

			ApplyLock(actor);
			SendModEvent("TFDPreCombatAssign", actor);

			spdlog::info("[TFD][PreCombat] claim actor={:08X}", actor->GetFormID());
			return true;
		}

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

			if (g_session.state == State::Idle) {
				return;
			}

			if (IsCaptiveLikeBlocked() && g_session.state != State::InDialogue) {
				spdlog::info("[TFD][PreCombat] blocked by captive-like state -> restore");
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

			switch (g_session.state) {
			case State::Claimed:
			{
				const auto now = Clock::now();

				if (now - g_session.lastEnforce >= std::chrono::milliseconds(g_cfg.enforceTickMs)) {
					ApplyLock(actor);
					g_session.lastEnforce = now;
				}

				if (IsDialogueOpen()) {
					g_session.sawDialogueOpen = true;
					g_session.state = State::InDialogue;
					spdlog::info("[TFD][PreCombat] dialogue opened");
					break;
				}

				if (now - g_session.started > std::chrono::milliseconds(g_cfg.claimTimeoutMs)) {
					spdlog::info("[TFD][PreCombat] claim timeout -> restore");
					BeginRestore();
				}
				break;
			}
			case State::InDialogue:
				ApplyLock(actor);
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
				} else {
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

		ResolveForms();

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

		auto* target = TFD::ActorScan::PickBestWarnTarget(g_cfg.claimRadius);
		if (!target) {
			spdlog::info("[TFD][PreCombat] no warn target");
			return false;
		}

		return StartClaim(target);
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
