#include "TFDPreCombatGreet.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include "TFDDefeatMonitor.h"
#include "TFDFactionMask.h"

namespace TFD::PreCombatGreet
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr double kManualWindowSec = 12.0;
		constexpr double kCooldownAfterDoneSec = 120.0;
		constexpr double kCooldownAfterFailSec = 5.0;
		constexpr double kCooldownAfterPlayerAttackSec = 1.0;

		struct Pending
		{
			double expiresSec{ 0.0 };
			bool savedAgg{ false };
			float origAgg{ 0.0f };
			bool aggZero{ false };
			bool truceApplied{ false };
			bool pkgAssigned{ false };
			bool dialogSeen{ false };
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

		RE::TESFaction* gTruceFaction = nullptr;
		bool gLoggedTruceFound = false;
		bool gLoggedTruceMissing = false;

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

		void ResolveTruceFaction()
		{
			if (gTruceFaction) {
				return;
			}

			gTruceFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDPreCombatTruceFaction");

			if (gTruceFaction && !gLoggedTruceFound) {
				gLoggedTruceFound = true;
				spdlog::info("[TFD][PreCombat] Truce faction resolved {:08X}", gTruceFaction->GetFormID());
			}

			if (!gTruceFaction && !gLoggedTruceMissing) {
				gLoggedTruceMissing = true;
				spdlog::warn("[TFD][PreCombat] TFDPreCombatTruceFaction not found");
			}
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

		void SetAggressionZero(RE::Actor* actor, Pending& pending)
		{
			if (!actor) {
				return;
			}

			auto* avo = actor->AsActorValueOwner();
			if (!avo) {
				return;
			}

			if (!pending.savedAgg) {
				pending.origAgg = avo->GetActorValue(RE::ActorValue::kAggression);
				pending.savedAgg = true;
			}

			if (pending.aggZero) {
				return;
			}

			avo->SetActorValue(RE::ActorValue::kAggression, 0.0f);
			pending.aggZero = true;

			spdlog::info("[TFD][PreCombat] Aggression forced to 0 actor={:08X}", actor->GetFormID());
		}

		void RestoreAggression(RE::Actor* actor, Pending& pending)
		{
			if (!actor || !pending.savedAgg || !pending.aggZero) {
				return;
			}

			auto* avo = actor->AsActorValueOwner();
			if (!avo) {
				return;
			}

			avo->SetActorValue(RE::ActorValue::kAggression, pending.origAgg);
			pending.aggZero = false;

			spdlog::info(
				"[TFD][PreCombat] Aggression restored actor={:08X} value={}",
				actor->GetFormID(),
				pending.origAgg);
		}

		bool EnsureTruceFaction(RE::Actor* actor, Pending& pending)
		{
			if (!actor) {
				pending.truceApplied = false;
				return false;
			}

			ResolveTruceFaction();
			if (!gTruceFaction) {
				pending.truceApplied = false;
				spdlog::warn("[TFD][PreCombat] Truce faction resolve failed actor={:08X}", actor->GetFormID());
				return false;
			}

			const int beforeRank = actor->GetFactionRank(gTruceFaction, false);
			int rank = beforeRank;

			if (rank != 1) {
				if (rank != -2) {
					actor->RemoveFromFaction(gTruceFaction);
				}

				actor->AddToFaction(gTruceFaction, 1);
				rank = actor->GetFactionRank(gTruceFaction, false);
			}

			if (rank == 1) {
				if (!pending.truceApplied || beforeRank != 1) {
					spdlog::info(
						"[TFD][PreCombat] Truce faction applied actor={:08X} beforeRank={} afterRank=1",
						actor->GetFormID(),
						beforeRank);
				}

				pending.truceApplied = true;
				return true;
			}

			pending.truceApplied = false;
			spdlog::warn(
				"[TFD][PreCombat] Truce faction apply failed actor={:08X} beforeRank={} afterRank={}",
				actor->GetFormID(),
				beforeRank,
				rank);
			return false;
		}

		void ClearTruceFaction(RE::Actor* actor, Pending& pending)
		{
			if (!actor) {
				return;
			}

			ResolveTruceFaction();
			if (gTruceFaction) {
				const int rank = actor->GetFactionRank(gTruceFaction, false);
				if (rank != -2) {
					actor->RemoveFromFaction(gTruceFaction);
				}
			}

			if (pending.truceApplied) {
				spdlog::info("[TFD][PreCombat] Truce faction cleared actor={:08X}", actor->GetFormID());
			}

			pending.truceApplied = false;
		}

		void StopCombatAndRelax(RE::Actor* actor)
		{
			if (!actor) {
				return;
			}

			if (actor->IsInCombat()) {
				actor->StopCombat();
			}
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

		void CleanupOne(RE::Actor* actor, Pending& pending, double cooldownSec, const char* reason)
		{
			if (!actor) {
				return;
			}

			if (pending.pkgAssigned) {
				SendBridgeEvent("TFDPreCombatClear", actor);
				pending.pkgAssigned = false;
			}

			ClearTruceFaction(actor, pending);
			RestoreAggression(actor, pending);

			gCooldownUntil[GetHandleId(actor)] = NowSec() + cooldownSec;

			spdlog::info(
				"[TFD][PreCombat] cleanup reason={} actor={:08X}",
				reason ? reason : "unknown",
				actor->GetFormID());
		}

		void ClearAllPendingLocked()
		{
			SendBridgeEvent("TFDPreCombatClearAll", nullptr);

			for (auto& [handle, pending] : gPending) {
				auto sp = RE::Actor::LookupByHandle(handle);
				if (auto* actor = sp.get()) {
					ClearTruceFaction(actor, pending);
					RestoreAggression(actor, pending);
				}
			}

			gPending.clear();
		}

		void ForceImmediateHostileReacquire(RE::Actor* actor)
		{
			if (!actor) {
				return;
			}

			const auto handle = GetHandleId(actor);
			auto* task = SKSE::GetTaskInterface();
			if (!task) {
				return;
			}

			task->AddTask([handle]() {
				auto* task2 = SKSE::GetTaskInterface();
				if (!task2) {
					return;
				}

				task2->AddUITask([handle]() {
					auto sp = RE::Actor::LookupByHandle(handle);
					auto* actorRef = sp.get();
					auto* player = RE::PlayerCharacter::GetSingleton();
					if (!actorRef || !player) {
						return;
					}

					actorRef->SetBeenAttacked(true);
					actorRef->EvaluatePackage(true, true);
					actorRef->UpdateCombat();

					spdlog::info("[TFD][PreCombat] Forced hostile reacquire actor={:08X}", actorRef->GetFormID());
					});
				});
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
				"[TFD][PreCombat] Player attacked claimed actor -> abort actor={:08X}",
				targetActor->GetFormID());

			SendBridgeEvent("TFDPreCombatClearAll", nullptr);
			it->second.pkgAssigned = false;

			ClearTruceFaction(targetActor, it->second);
			RestoreAggression(targetActor, it->second);

			gCooldownUntil[handle] = NowSec() + kCooldownAfterPlayerAttackSec;
			ForceImmediateHostileReacquire(targetActor);
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

			std::scoped_lock lk(gLock);

			for (auto it = gPending.begin(); it != gPending.end();) {
				auto sp = RE::Actor::LookupByHandle(it->first);
				auto* actor = sp.get();
				auto& pending = it->second;

				if (!actor) {
					it = gPending.erase(it);
					continue;
				}

				SetAggressionZero(actor, pending);

				if (!EnsureTruceFaction(actor, pending)) {
					CleanupOne(actor, pending, kCooldownAfterFailSec, "truce_rank1_failed");
					it = gPending.erase(it);
					continue;
				}

				if (actor->IsInCombat()) {
					StopCombatAndRelax(actor);
				}

				if (!pending.pkgAssigned) {
					SendBridgeEvent("TFDPreCombatAssign", actor);
					pending.pkgAssigned = true;
					pending.dialogSeen = false;
					spdlog::info("[TFD][PreCombat] assign actor={:08X}", actor->GetFormID());
				}

				if (dialogueOpen) {
					pending.dialogSeen = true;
					++it;
					continue;
				}

				if (pending.dialogSeen) {
					CleanupOne(actor, pending, kCooldownAfterDoneSec, "dialogue_closed");
					it = gPending.erase(it);
					continue;
				}

				if (now >= pending.expiresSec) {
					CleanupOne(actor, pending, kCooldownAfterFailSec, "manual_window_expired");
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

		ResolveTruceFaction();

		if (auto* scripts = RE::ScriptEventSourceHolder::GetSingleton()) {
			scripts->AddEventSink(&gHitSink);
		}

		gSuspended.store(false, std::memory_order_release);
		gRunning.store(true, std::memory_order_release);
		gWorker = std::thread(WorkerLoop);

		spdlog::info("[TFD][PreCombat] Install");
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

		spdlog::info("[TFD][PreCombat] Shutdown");
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

		if (!gPending.empty()) {
			ClearAllPendingLocked();
		}

		Pending pending{};
		pending.expiresSec = now + kManualWindowSec;

		auto [it, inserted] = gPending.emplace(handle, pending);
		if (!inserted) {
			return true;
		}

		auto& state = it->second;

		SetAggressionZero(actor, state);

		if (!EnsureTruceFaction(actor, state)) {
			RestoreAggression(actor, state);
			gPending.erase(it);

			spdlog::warn(
				"[TFD][PreCombat] BeginForActor abort: truce rank1 failed actor={:08X}",
				actor->GetFormID());
			return false;
		}

		StopCombatAndRelax(actor);
		SendBridgeEvent("TFDPreCombatAssign", actor);
		state.pkgAssigned = true;
		state.dialogSeen = false;

		spdlog::info(
			"[TFD][PreCombat] BeginForActor actor={:08X} window={:.1f}s",
			actor->GetFormID(),
			kManualWindowSec);

		return true;
	}

	void OnPreLoadGame()
	{
		SetSuspended(true);
		CancelAll();
		gTickPending.clear(std::memory_order_release);

		spdlog::info("[TFD][PreCombat] OnPreLoadGame -> suspended + cleared");
	}

	void OnPostLoadGame()
	{
		CancelAll();
		SetSuspended(false);
		gTickPending.clear(std::memory_order_release);

		spdlog::info("[TFD][PreCombat] OnPostLoadGame -> resumed clean");
	}

	void CancelAll()
	{
		std::scoped_lock lk(gLock);
		ClearAllPendingLocked();
		gCooldownUntil.clear();

		spdlog::info("[TFD][PreCombat] CancelAll");
	}
}