#include "TFDPreCombatGreet.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include "TFDDefeatMonitor.h"
#include "TFDFactionMask.h"
#include "TFDInteractionRouter.h"
#include "TFDPacify.h"

namespace TFD::PreCombatGreet
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr double kManualWindowSec = 120.0;
		constexpr double kCooldownAfterDoneSec = 120.0;
		constexpr double kCooldownAfterFailSec = 5.0;
		constexpr double kCooldownAfterPlayerAttackSec = 1.0;

		struct Pending
		{
			RE::FormID pacifySessionId{ 0 };
			TFD::InteractionRouter::Action action{ TFD::InteractionRouter::Action::None };

			double expiresSec{ 0.0 };
			bool dialogueRequested{ false };
			bool dialogSeen{ false };
			bool assignSent{ false };
		};

		std::atomic_bool gInstalled{ false };
		std::atomic_bool gSuspended{ false };
		std::atomic_bool gRunning{ false };
		std::atomic_flag gTickPending = ATOMIC_FLAG_INIT;
		std::thread gWorker{};

		std::mutex gLock;
		std::unordered_map<std::uint32_t, Pending> gPending;
		std::unordered_map<std::uint32_t, double> gCooldownUntil;
		Clock::time_point gT0 = Clock::now();

		double NowSec()
		{
			return std::chrono::duration<double>(Clock::now() - gT0).count();
		}

		std::uint32_t GetHandleId(RE::Actor* actor)
		{
			return actor ? actor->GetHandle().native_handle() : 0;
		}

		bool IsDialogueOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
		}

		bool IsPlayerDown()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return false;
			}

			auto* state = player->AsActorState();
			return state && state->IsBleedingOut();
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

			if (actor->IsDead() || actor->IsDisabled()) {
				return false;
			}

			if (!actor->Is3DLoaded()) {
				return false;
			}

			if (actor->GetFormID() == player->GetFormID()) {
				return false;
			}

			if (TFD::FactionMask::IsActive()) {
				return false;
			}

			if (TFD::DefeatMonitor::IsLeftForDeadRecoveryActive()) {
				return false;
			}

			if (IsPlayerDown()) {
				return false;
			}

			return true;
		}

		bool IsActorStillValid(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}

			if (actor->IsDead()) {
				return false;
			}

			if (actor->IsDisabled()) {
				return false;
			}

			if (!actor->Is3DLoaded()) {
				return false;
			}

			return true;
		}

		void CleanupOne(
			RE::Actor* actor,
			Pending& pending,
			double cooldownSec,
			const char* reason,
			TFD::Pacify::ReleaseReason pacifyReason)
		{
			if (pending.pacifySessionId != 0) {
				TFD::Pacify::ReleaseSession(pending.pacifySessionId, pacifyReason);
				pending.pacifySessionId = 0;
			}

			if (pending.assignSent) {
				SendBridgeEvent("TFDPreCombatClear", actor);
				pending.assignSent = false;
			}

			if (actor) {
				gCooldownUntil[GetHandleId(actor)] = NowSec() + cooldownSec;

				spdlog::info(
					"[TFD][PreCombatGreet] cleanup reason={} actor={:08X} action={}",
					reason ? reason : "unknown",
					actor->GetFormID(),
					TFD::InteractionRouter::ToString(pending.action));
			}
			else {
				spdlog::info(
					"[TFD][PreCombatGreet] cleanup reason={} actor=<none> action={}",
					reason ? reason : "unknown",
					TFD::InteractionRouter::ToString(pending.action));
			}
		}

		void ClearAllPendingLocked()
		{
			SendBridgeEvent("TFDPreCombatClearAll", nullptr);

			for (auto& [handle, pending] : gPending) {
				auto sp = RE::Actor::LookupByHandle(handle);
				auto* actor = sp.get();

				if (pending.pacifySessionId != 0) {
					TFD::Pacify::ReleaseSession(pending.pacifySessionId, TFD::Pacify::ReleaseReason::Generic);
					pending.pacifySessionId = 0;
				}

				if (pending.assignSent) {
					SendBridgeEvent("TFDPreCombatClear", actor);
					pending.assignSent = false;
				}
			}

			gPending.clear();
		}

		void AbortOnPlayerAttack(const RE::TESHitEvent* ev)
		{
			if (!ev) {
				return;
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}

			auto* targetRef = ev->target.get();
			auto* causeRef = ev->cause.get();
			if (!targetRef || !causeRef) {
				return;
			}

			auto* targetActor = targetRef->As<RE::Actor>();
			auto* causeActor = causeRef->As<RE::Actor>();
			if (!targetActor || !causeActor) {
				return;
			}

			if (causeActor->GetFormID() != player->GetFormID()) {
				return;
			}

			const auto handle = GetHandleId(targetActor);

			std::scoped_lock lk(gLock);

			auto it = gPending.find(handle);
			if (it == gPending.end()) {
				return;
			}

			spdlog::info(
				"[TFD][PreCombatGreet] player attacked active target -> abort actor={:08X}",
				targetActor->GetFormID());

			SendBridgeEvent("TFDPreCombatClearAll", nullptr);

			CleanupOne(targetActor, it->second, kCooldownAfterPlayerAttackSec, "player_attack", TFD::Pacify::ReleaseReason::PlayerAggression);
			gPending.erase(it);
		}

		class HitSink : public RE::BSTEventSink<RE::TESHitEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(
				const RE::TESHitEvent* ev,
				RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				AbortOnPlayerAttack(ev);
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		HitSink gHitSink{};

		void TickUI()
		{
			struct Guard
			{
				~Guard()
				{
					gTickPending.clear(std::memory_order_release);
				}
			} guard;

			if (gSuspended.load(std::memory_order_acquire)) {
				std::scoped_lock lk(gLock);
				ClearAllPendingLocked();
				return;
			}

			if (TFD::DefeatMonitor::IsLeftForDeadRecoveryActive()) {
				std::scoped_lock lk(gLock);
				ClearAllPendingLocked();
				return;
			}

			if (TFD::FactionMask::IsActive() || IsPlayerDown()) {
				std::scoped_lock lk(gLock);
				ClearAllPendingLocked();
				return;
			}

			const bool dialogueOpen = IsDialogueOpen();
			const double now = NowSec();

			TFD::Pacify::Update(now);

			std::scoped_lock lk(gLock);

			for (auto it = gPending.begin(); it != gPending.end();) {
				auto sp = RE::Actor::LookupByHandle(it->first);
				auto* actor = sp.get();
				auto& pending = it->second;

				if (!IsActorStillValid(actor)) {
					if (pending.pacifySessionId != 0) {
						TFD::Pacify::ReleaseSession(pending.pacifySessionId, TFD::Pacify::ReleaseReason::Generic);
					}
					it = gPending.erase(it);
					continue;
				}

				if (!TFD::Pacify::IsPacified(actor)) {
					CleanupOne(actor, pending, kCooldownAfterFailSec, "pacify_lost", TFD::Pacify::ReleaseReason::Generic);
					it = gPending.erase(it);
					continue;
				}

				if (pending.dialogueRequested) {
					if (dialogueOpen) {
						pending.dialogSeen = true;
						++it;
						continue;
					}

					if (pending.dialogSeen) {
						CleanupOne(actor, pending, kCooldownAfterDoneSec, "dialogue_closed", TFD::Pacify::ReleaseReason::DialogueClosed);
						it = gPending.erase(it);
						continue;
					}
				}
				else {
					// Tame path: no dialogue. Kalau target sudah balik full combat, lepaskan.
					if (actor->IsInCombat()) {
						CleanupOne(actor, pending, kCooldownAfterFailSec, "tame_broken", TFD::Pacify::ReleaseReason::TameBroken);
						it = gPending.erase(it);
						continue;
					}
				}

				if (now >= pending.expiresSec) {
					CleanupOne(actor, pending, kCooldownAfterFailSec, "hidden_failsafe_expired", TFD::Pacify::ReleaseReason::HardFailsafeExpired);
					it = gPending.erase(it);
					continue;
				}

				++it;
			}
		}

		void WorkerLoop()
		{
			while (gRunning.load(std::memory_order_acquire)) {
				std::this_thread::sleep_for(std::chrono::milliseconds(60));

				if (!gInstalled.load(std::memory_order_acquire)) {
					continue;
				}

				if (gTickPending.test_and_set(std::memory_order_acq_rel)) {
					continue;
				}

				auto* tasks = SKSE::GetTaskInterface();
				if (!tasks) {
					gTickPending.clear(std::memory_order_release);
					continue;
				}

				tasks->AddUITask([]() { TickUI(); });
			}
		}
	}

	void Install()
	{
		if (gInstalled.exchange(true)) {
			return;
		}

		if (auto* scripts = RE::ScriptEventSourceHolder::GetSingleton()) {
			scripts->AddEventSink(&gHitSink);
		}

		gSuspended.store(false, std::memory_order_release);
		gRunning.store(true, std::memory_order_release);
		gWorker = std::thread(WorkerLoop);

		spdlog::info("[TFD][PreCombatGreet] Install");
	}

	void Shutdown()
	{
		if (!gInstalled.exchange(false)) {
			return;
		}

		if (auto* scripts = RE::ScriptEventSourceHolder::GetSingleton()) {
			scripts->RemoveEventSink(&gHitSink);
		}

		gRunning.store(false, std::memory_order_release);

		if (gWorker.joinable()) {
			gWorker.join();
		}

		{
			std::scoped_lock lk(gLock);
			ClearAllPendingLocked();
			gCooldownUntil.clear();
		}

		gSuspended.store(false, std::memory_order_release);

		spdlog::info("[TFD][PreCombatGreet] Shutdown");
	}

	void SetSuspended(bool suspended)
	{
		gSuspended.store(suspended, std::memory_order_release);

		if (suspended) {
			std::scoped_lock lk(gLock);
			ClearAllPendingLocked();
		}
	}

	bool IsSuspended()
	{
		return gSuspended.load(std::memory_order_acquire);
	}

	bool BeginForActor(RE::Actor* actor)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();

		if (TFD::DefeatMonitor::IsLeftForDeadRecoveryActive()) {
			return false;
		}

		if (!IsCandidate(actor, player)) {
			return false;
		}

		const auto handle = GetHandleId(actor);
		const double now = NowSec();

		std::scoped_lock lk(gLock);

		auto cooldownIt = gCooldownUntil.find(handle);
		if (cooldownIt != gCooldownUntil.end() && now < cooldownIt->second) {
			return false;
		}

		if (gPending.find(handle) != gPending.end()) {
			return true;
		}

		const bool isCaptivePhase = false;
		const auto result = TFD::InteractionRouter::HandleHotkeyPress(
			player,
			actor,
			isCaptivePhase,
			now);

		spdlog::info(
			"[TFD][PreCombatGreet] BeginForActor actor={:08X} action={} executed={} dialogueRequested={} fail={}",
			actor->GetFormID(),
			TFD::InteractionRouter::ToString(result.action),
			result.executed ? 1 : 0,
			result.dialogueRequested ? 1 : 0,
			TFD::InteractionRouter::ToString(result.failReason));

		if (!result.executed || result.sessionId == 0) {
			return false;
		}

		if (!gPending.empty()) {
			ClearAllPendingLocked();
		}

		Pending pending{};
		pending.pacifySessionId = result.sessionId;
		pending.action = result.action;
		pending.expiresSec = now + kManualWindowSec;
		pending.dialogueRequested = result.dialogueRequested;
		pending.dialogSeen = false;
		pending.assignSent = false;

		if (result.dialogueRequested) {
			if (!TFD::Pacify::CanOpenDialogue(actor)) {
				TFD::Pacify::ReleaseSession(result.sessionId, TFD::Pacify::ReleaseReason::Generic);
				return false;
			}

			SendBridgeEvent("TFDPreCombatAssign", actor);
			pending.assignSent = true;
		}

		gPending.emplace(handle, pending);
		return true;
	}

	void OnPreLoadGame()
	{
		SetSuspended(true);
		CancelAll();
		gTickPending.clear(std::memory_order_release);

		spdlog::info("[TFD][PreCombatGreet] OnPreLoadGame -> suspended + cleared");
	}

	void OnPostLoadGame()
	{
		CancelAll();
		SetSuspended(false);
		gTickPending.clear(std::memory_order_release);

		spdlog::info("[TFD][PreCombatGreet] OnPostLoadGame -> resumed clean");
	}

	void CancelAll()
	{
		std::scoped_lock lk(gLock);
		ClearAllPendingLocked();
		gCooldownUntil.clear();

		spdlog::info("[TFD][PreCombatGreet] CancelAll");
	}
}