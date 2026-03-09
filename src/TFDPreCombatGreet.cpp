#include "TFDPreCombatGreet.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

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
			auto* st = player->AsActorState();
			return st && st->IsBleedingOut();
		}

		void ResolveTruceFaction()
		{
			if (!gTruceFaction) {
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

			if (auto* a = sender ? sender->As<RE::Actor>() : nullptr) {
				handle = a->GetHandle().native_handle();
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

				SKSE::ModCallbackEvent e{ name.c_str(), "", 0.0f, outSender };
				src->SendEvent(&e);
				});
		}

		void SetAggressionZero(RE::Actor* a, Pending& p)
		{
			if (!a) {
				return;
			}

			auto* avo = a->AsActorValueOwner();
			if (!avo) {
				return;
			}

			if (!p.savedAgg) {
				p.origAgg = avo->GetActorValue(RE::ActorValue::kAggression);
				p.savedAgg = true;
			}

			if (p.aggZero) {
				return;
			}

			avo->SetActorValue(RE::ActorValue::kAggression, 0.0f);
			p.aggZero = true;

			spdlog::info("[TFD][PreCombat] Aggression forced to 0 actor={:08X}", a->GetFormID());
		}

		void RestoreAggression(RE::Actor* a, Pending& p)
		{
			if (!a || !p.savedAgg || !p.aggZero) {
				return;
			}

			auto* avo = a->AsActorValueOwner();
			if (!avo) {
				return;
			}

			avo->SetActorValue(RE::ActorValue::kAggression, p.origAgg);
			p.aggZero = false;

			spdlog::info("[TFD][PreCombat] Aggression restored actor={:08X} value={}", a->GetFormID(), p.origAgg);
		}

		bool EnsureTruceFaction(RE::Actor* a, Pending& p)
		{
			if (!a) {
				p.truceApplied = false;
				return false;
			}

			ResolveTruceFaction();
			if (!gTruceFaction) {
				p.truceApplied = false;
				spdlog::warn("[TFD][PreCombat] Truce faction resolve failed actor={:08X}", a->GetFormID());
				return false;
			}

			const int beforeRank = a->GetFactionRank(gTruceFaction, false);
			int rank = beforeRank;

			if (rank != 1) {
				if (rank != -2) {
					a->RemoveFromFaction(gTruceFaction);
				}

				a->AddToFaction(gTruceFaction, 1);
				rank = a->GetFactionRank(gTruceFaction, false);
			}

			if (rank == 1) {
				if (!p.truceApplied || beforeRank != 1) {
					spdlog::info("[TFD][PreCombat] Truce faction applied actor={:08X} beforeRank={} afterRank=1",
						a->GetFormID(), beforeRank);
				}
				p.truceApplied = true;
				return true;
			}

			p.truceApplied = false;
			spdlog::warn("[TFD][PreCombat] Truce faction apply failed actor={:08X} beforeRank={} afterRank={}",
				a->GetFormID(), beforeRank, rank);
			return false;
		}

		void ClearTruceFaction(RE::Actor* a, Pending& p)
		{
			if (!a) {
				return;
			}

			ResolveTruceFaction();
			if (gTruceFaction) {
				const int rank = a->GetFactionRank(gTruceFaction, false);
				if (rank != -2) {
					a->RemoveFromFaction(gTruceFaction);
				}
			}

			if (p.truceApplied) {
				spdlog::info("[TFD][PreCombat] Truce faction cleared actor={:08X}", a->GetFormID());
			}
			p.truceApplied = false;
		}

		void StopCombatAndRelax(RE::Actor* a)
		{
			if (!a) {
				return;
			}

			if (a->IsInCombat()) {
				a->StopCombat();
			}
		}

		bool IsCandidate(RE::Actor* a, RE::PlayerCharacter* player)
		{
			if (!a || !player) {
				return false;
			}
			if (a->IsDead() || a->IsDisabled()) {
				return false;
			}
			if (!a->Is3DLoaded()) {
				return false;
			}
			if (a->GetFormID() == player->GetFormID()) {
				return false;
			}
			if (TFD::FactionMask::IsActive()) {
				return false;
			}
			if (IsPlayerDown()) {
				return false;
			}
			return true;
		}

		void CleanupOne(RE::Actor* a, Pending& p, double cooldownSec, const char* reason)
		{
			if (!a) {
				return;
			}

			if (p.pkgAssigned) {
				SendBridgeEvent("TFDPreCombatClear", a);
				p.pkgAssigned = false;
			}

			ClearTruceFaction(a, p);
			RestoreAggression(a, p);

			gCooldownUntil[a->GetHandle().native_handle()] = NowSec() + cooldownSec;

			spdlog::info("[TFD][PreCombat] cleanup reason={} actor={:08X}",
				reason ? reason : "unknown", a->GetFormID());
		}

		void ClearAllPending()
		{
			SendBridgeEvent("TFDPreCombatClearAll", nullptr);

			for (auto& it : gPending) {
				auto sp = RE::Actor::LookupByHandle(it.first);
				if (auto* a = sp.get()) {
					ClearTruceFaction(a, it.second);
					RestoreAggression(a, it.second);
				}
			}

			gPending.clear();
		}

		void ForceImmediateHostileReacquire(RE::Actor* a)
		{
			if (!a) {
				return;
			}

			const auto handle = a->GetHandle().native_handle();

			auto* task = SKSE::GetTaskInterface();
			if (!task) {
				return;
			}

			// chain task biar bridge clear sempat jalan dulu
			task->AddTask([handle]() {
				auto* task2 = SKSE::GetTaskInterface();
				if (!task2) {
					return;
				}

				task2->AddUITask([handle]() {
					auto sp = RE::Actor::LookupByHandle(handle);
					auto* actor = sp.get();
					auto* player = RE::PlayerCharacter::GetSingleton();
					if (!actor || !player) {
						return;
					}

					actor->SetBeenAttacked(true);
					actor->EvaluatePackage(true, true);
					actor->UpdateCombat();

					spdlog::info("[TFD][PreCombat] Forced hostile reacquire actor={:08X}", actor->GetFormID());
					});
				});
		}

		void AbortOnPlayerAttack(const RE::TESHitEvent* e)
		{
			if (!e) {
				return;
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}

			auto* targetRef = e->target.get();
			auto* causeRef = e->cause.get();
			if (!targetRef || !causeRef) {
				return;
			}

			auto* targetActor = targetRef->As<RE::Actor>();
			auto* causeActor = causeRef->As<RE::Actor>();
			if (!targetActor || !causeActor) {
				return;
			}

			// versi sempit: hit langsung player
			if (causeActor->GetFormID() != player->GetFormID()) {
				return;
			}

			const auto h = targetActor->GetHandle().native_handle();

			std::scoped_lock lk(gLock);

			auto it = gPending.find(h);
			if (it == gPending.end()) {
				return;
			}

			spdlog::info("[TFD][PreCombat] Player attacked claimed actor -> abort actor={:08X}",
				targetActor->GetFormID());

			// actor di alias harus langsung hilang, jadi nuke bridge precombat sekalian
			SendBridgeEvent("TFDPreCombatClearAll", nullptr);
			it->second.pkgAssigned = false;

			ClearTruceFaction(targetActor, it->second);
			RestoreAggression(targetActor, it->second);
			gCooldownUntil[h] = NowSec() + kCooldownAfterPlayerAttackSec;

			ForceImmediateHostileReacquire(targetActor);

			gPending.erase(it);
		}

		class HitSink : public RE::BSTEventSink<RE::TESHitEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(
				const RE::TESHitEvent* e,
				RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				AbortOnPlayerAttack(e);
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		HitSink gHitSink{};

		void TickUI()
		{
			struct Guard {
				~Guard() { gTickPending.clear(std::memory_order_release); }
			} guard;

			if (gSuspended.load(std::memory_order_acquire)) {
				std::scoped_lock lk(gLock);
				ClearAllPending();
				return;
			}

			if (TFD::FactionMask::IsActive() || IsPlayerDown()) {
				std::scoped_lock lk(gLock);
				ClearAllPending();
				return;
			}

			const bool dialogueOpen = IsDialogueOpen();

			std::scoped_lock lk(gLock);

			for (auto it = gPending.begin(); it != gPending.end();) {
				auto sp = RE::Actor::LookupByHandle(it->first);
				auto* a = sp.get();
				auto& p = it->second;

				if (!a) {
					it = gPending.erase(it);
					continue;
				}

				SetAggressionZero(a, p);
				if (!EnsureTruceFaction(a, p)) {
					CleanupOne(a, p, kCooldownAfterFailSec, "truce_rank1_failed");
					it = gPending.erase(it);
					continue;
				}

				if (a->IsInCombat()) {
					StopCombatAndRelax(a);
				}

				if (!p.pkgAssigned) {
					SendBridgeEvent("TFDPreCombatAssign", a);
					p.pkgAssigned = true;
					p.dialogSeen = false;
					spdlog::info("[TFD][PreCombat] assign actor={:08X}", a->GetFormID());
				}

				if (dialogueOpen) {
					p.dialogSeen = true;
					++it;
					continue;
				}

				if (p.dialogSeen && !dialogueOpen) {
					CleanupOne(a, p, kCooldownAfterDoneSec, "dialogue_closed");
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
			scripts->AddEventSink<RE::TESHitEvent>(&gHitSink);
		}

		gSuspended.store(false);
		gRunning.store(true);
		gWorker = std::thread(WorkerLoop);

		spdlog::info("[TFD][PreCombat] Install");
	}

	void Shutdown()
	{
		if (!gInstalled.exchange(false)) {
			return;
		}

		if (auto* scripts = RE::ScriptEventSourceHolder::GetSingleton()) {
			scripts->RemoveEventSink<RE::TESHitEvent>(&gHitSink);
		}

		gRunning.store(false);
		if (gWorker.joinable()) {
			gWorker.join();
		}

		{
			std::scoped_lock lk(gLock);
			ClearAllPending();
			gCooldownUntil.clear();
		}

		gSuspended.store(false);
		spdlog::info("[TFD][PreCombat] Shutdown");
	}

	void SetSuspended(bool suspended)
	{
		gSuspended.store(suspended, std::memory_order_release);

		if (suspended) {
			std::scoped_lock lk(gLock);
			ClearAllPending();
		}
	}

	bool IsSuspended()
	{
		return gSuspended.load(std::memory_order_acquire);
	}

	bool BeginForActor(RE::Actor* actor)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!IsCandidate(actor, player)) {
			return false;
		}

		const auto h = actor->GetHandle().native_handle();
		const double now = NowSec();

		std::scoped_lock lk(gLock);

		auto ct = gCooldownUntil.find(h);
		if (ct != gCooldownUntil.end() && now < ct->second) {
			return false;
		}

		auto existing = gPending.find(h);
		if (existing != gPending.end()) {
			return true;
		}

		if (!gPending.empty()) {
			ClearAllPending();
		}

		auto& p = gPending[h];
		p.expiresSec = now + kManualWindowSec;
		p.pkgAssigned = false;
		p.dialogSeen = false;
		p.savedAgg = false;
		p.origAgg = 0.0f;
		p.aggZero = false;
		p.truceApplied = false;

		SetAggressionZero(actor, p);
		if (!EnsureTruceFaction(actor, p)) {
			RestoreAggression(actor, p);
			gPending.erase(h);
			spdlog::warn("[TFD][PreCombat] BeginForActor abort: truce rank1 failed actor={:08X}", actor->GetFormID());
			return false;
		}

		StopCombatAndRelax(actor);
		SendBridgeEvent("TFDPreCombatAssign", actor);
		p.pkgAssigned = true;
		p.dialogSeen = false;

		spdlog::info("[TFD][PreCombat] BeginForActor actor={:08X} window={:.1f}s",
			actor->GetFormID(), kManualWindowSec);

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
		ClearAllPending();
		gCooldownUntil.clear();
		spdlog::info("[TFD][PreCombat] CancelAll");
	}
}