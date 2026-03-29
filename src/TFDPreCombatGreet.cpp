
#include "TFDPreCombatGreet.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <string_view>

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
		constexpr double kRecentActorHoldSec = 12.0;
		constexpr double kPleasureStartHoldSec = 20.0;
		constexpr double kPleasureSceneHoldSec = 900.0;

		constexpr const char* kPleasureStartPendingEvent = "TFDPreCombatPleasureStartPending";
		constexpr const char* kPleasureStartedEvent = "TFDPreCombatPleasureStarted";
		constexpr const char* kPleasureFailedEvent = "TFDPreCombatPleasureFailed";
		constexpr const char* kPleasureEndedEvent = "TFDPreCombatPleasureEnded";

		struct Pending
		{
			RE::FormID pacifySessionId{ 0 };
			TFD::InteractionRouter::Action action{ TFD::InteractionRouter::Action::None };

			double expiresSec{ 0.0 };
			bool dialogueRequested{ false };
			bool dialogSeen{ false };
			bool assignSent{ false };
			bool pleasureHandoff{ false };
			bool pleasureSceneActive{ false };
			double nextDebugLogSec{ 0.0 };
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

		std::uint32_t gRecentActorHandle = 0;
		double gRecentActorCachedAtSec = 0.0;
		double gRecentActorUntilSec = 0.0;

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

		void CacheRecentActor(RE::Actor* actor, double holdSec, const char* reason)
		{
			if (!actor) {
				return;
			}

			gRecentActorHandle = actor->GetHandle().native_handle();
			gRecentActorCachedAtSec = NowSec();
			gRecentActorUntilSec = gRecentActorCachedAtSec + holdSec;

			spdlog::info(
				"[TFD][PreCombatGreet] recent actor cached actor={:08X} hold={:.1f}s reason={}",
				actor->GetFormID(),
				holdSec,
				reason ? reason : "unknown");
		}

		void ClearRecentActor(const char* reason)
		{
			if (gRecentActorHandle == 0) {
				return;
			}

			spdlog::info(
				"[TFD][PreCombatGreet] recent actor cleared actorHandle={:08X} reason={}",
				gRecentActorHandle,
				reason ? reason : "unknown");

			gRecentActorHandle = 0;
			gRecentActorCachedAtSec = 0.0;
			gRecentActorUntilSec = 0.0;
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

		struct BridgeEventNames
		{
			const char* assign{ nullptr };
			const char* clear{ nullptr };
			const char* clearAll{ nullptr };
		};

		BridgeEventNames GetBridgeEventNames(TFD::InteractionRouter::Action action)
		{
			switch (action) {
			case TFD::InteractionRouter::Action::TrucePreCombat:
				return { "TFDPreCombatAssign", "TFDPreCombatClear", "TFDPreCombatClearAll" };
			case TFD::InteractionRouter::Action::TruceInCombat:
				return { "TFDInCombatAssign", "TFDInCombatClear", "TFDInCombatClearAll" };
			default:
				return {};
			}
		}

		void ClearAllBridgeAliases()
		{
			SendBridgeEvent("TFDPreCombatClearAll", nullptr);
			SendBridgeEvent("TFDInCombatClearAll", nullptr);
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

		bool IsPleasureDialogueHandoffAction(TFD::InteractionRouter::Action action)
		{
			return action == TFD::InteractionRouter::Action::TrucePreCombat;
		}

		Pending* FindPendingLocked(RE::Actor* actor)
		{
			if (!actor) {
				return nullptr;
			}

			auto it = gPending.find(GetHandleId(actor));
			if (it == gPending.end()) {
				return nullptr;
			}

			return std::addressof(it->second);
		}

		void ArmPleasureHandoffLocked(RE::Actor* actor)
		{
			auto* pending = FindPendingLocked(actor);
			if (!pending) {
				return;
			}

			if (!pending->dialogueRequested || !IsPleasureDialogueHandoffAction(pending->action)) {
				return;
			}

			pending->pleasureHandoff = true;
			pending->pleasureSceneActive = false;
			pending->expiresSec = (std::max)(pending->expiresSec, NowSec() + kPleasureStartHoldSec);

			CacheRecentActor(actor, kRecentActorHoldSec, "pleasure_start_pending");
			spdlog::info(
				"[TFD][PreCombatGreet] pleasure handoff armed actor={:08X} session={} action={}",
				actor->GetFormID(),
				pending->pacifySessionId,
				TFD::InteractionRouter::ToString(pending->action));
		}

		void MarkPleasureStartedLocked(RE::Actor* actor)
		{
			auto* pending = FindPendingLocked(actor);
			if (!pending) {
				return;
			}

			if (!pending->dialogueRequested || !IsPleasureDialogueHandoffAction(pending->action)) {
				return;
			}

			pending->pleasureHandoff = true;
			pending->pleasureSceneActive = true;
			pending->expiresSec = (std::max)(pending->expiresSec, NowSec() + kPleasureSceneHoldSec);

			CacheRecentActor(actor, kRecentActorHoldSec, "pleasure_scene_started");
			spdlog::info(
				"[TFD][PreCombatGreet] pleasure scene started actor={:08X} session={} action={}",
				actor->GetFormID(),
				pending->pacifySessionId,
				TFD::InteractionRouter::ToString(pending->action));
		}

		RE::Actor* ResolvePleasureEventActor(const SKSE::ModCallbackEvent* ev)
		{
			if (!ev) {
				return nullptr;
			}

			if (auto* actor = ev->sender ? ev->sender->As<RE::Actor>() : nullptr) {
				return actor;
			}

			const auto* rawArg = ev->strArg.c_str();
			if (!rawArg || rawArg[0] == '\0') {
				return nullptr;
			}

			try {
				const auto actorId = static_cast<RE::FormID>(std::stoul(rawArg, nullptr, 10));
				return RE::TESForm::LookupByID<RE::Actor>(actorId);
			}
			catch (...) {
				return nullptr;
			}
		}

		void LogInCombatDialogueState(const char* tag, RE::Actor* actor, bool dialogueOpen)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!actor || !player) {
				return;
			}

			auto* actorCell = actor->GetParentCell();
			auto* playerCell = player->GetParentCell();
			const bool sameCell = actorCell && playerCell && actorCell == playerCell;
			const float dist = actor->GetPosition().GetDistance(player->GetPosition());
			const bool hostile = actor->IsHostileToActor(player);

			spdlog::info(
				"[TFD][InCombatDebug] {} actor={:08X} inCombat={} weaponDrawn={} hostile={} loaded={} sameCell={} pacified={} dialogueOpen={} dist={:.1f}",
				tag ? tag : "state",
				actor->GetFormID(),
				actor->IsInCombat() ? 1 : 0,
				actor->IsWeaponDrawn() ? 1 : 0,
				hostile ? 1 : 0,
				actor->Is3DLoaded() ? 1 : 0,
				sameCell ? 1 : 0,
				TFD::Pacify::IsPacified(actor) ? 1 : 0,
				dialogueOpen ? 1 : 0,
				dist);
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
				const auto bridge = GetBridgeEventNames(pending.action);
				if (bridge.clear) {
					SendBridgeEvent(bridge.clear, actor);
				}
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
			ClearAllBridgeAliases();

			for (auto& [handle, pending] : gPending) {
				auto sp = RE::Actor::LookupByHandle(handle);
				auto* actor = sp.get();

				if (pending.pacifySessionId != 0) {
					TFD::Pacify::ReleaseSession(pending.pacifySessionId, TFD::Pacify::ReleaseReason::Generic);
					pending.pacifySessionId = 0;
				}

				if (pending.assignSent) {
					const auto bridge = GetBridgeEventNames(pending.action);
					if (bridge.clear) {
						SendBridgeEvent(bridge.clear, actor);
					}
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

			ClearAllBridgeAliases();

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

		class PleasureEventSink final : public RE::BSTEventSink<SKSE::ModCallbackEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* ev, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
			{
				if (!ev) {
					return RE::BSEventNotifyControl::kContinue;
				}

				const auto* rawName = ev->eventName.c_str();
				const std::string_view name = rawName ? std::string_view(rawName) : std::string_view{};
				if (name.empty()) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* actor = ResolvePleasureEventActor(ev);
				if (!actor) {
					return RE::BSEventNotifyControl::kContinue;
				}

				if (name == kPleasureStartPendingEvent) {
					std::scoped_lock lk(gLock);
					ArmPleasureHandoffLocked(actor);
					return RE::BSEventNotifyControl::kContinue;
				}

				if (name == kPleasureStartedEvent) {
					std::scoped_lock lk(gLock);
					MarkPleasureStartedLocked(actor);
					return RE::BSEventNotifyControl::kContinue;
				}

				if (name == kPleasureFailedEvent || name == kPleasureEndedEvent) {
					std::scoped_lock lk(gLock);

					auto it = gPending.find(GetHandleId(actor));
					if (it == gPending.end()) {
						return RE::BSEventNotifyControl::kContinue;
					}

					auto& pending = it->second;
					if (!pending.dialogueRequested || !IsPleasureDialogueHandoffAction(pending.action)) {
						return RE::BSEventNotifyControl::kContinue;
					}

					if (name == kPleasureFailedEvent) {
						CacheRecentActor(actor, kRecentActorHoldSec, "pleasure_failed");
						CleanupOne(actor, pending, kCooldownAfterFailSec, "pleasure_failed", TFD::Pacify::ReleaseReason::DialogueClosed);
					}
					else {
						CacheRecentActor(actor, kRecentActorHoldSec, "pleasure_ended");
						CleanupOne(actor, pending, kCooldownAfterDoneSec, "pleasure_ended", TFD::Pacify::ReleaseReason::Generic);
					}

					gPending.erase(it);
					return RE::BSEventNotifyControl::kContinue;
				}

				return RE::BSEventNotifyControl::kContinue;
			}
		};

		PleasureEventSink gPleasureEventSink{};

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

				if (pending.action == TFD::InteractionRouter::Action::TruceInCombat && now >= pending.nextDebugLogSec) {
					LogInCombatDialogueState("tick", actor, dialogueOpen);
					pending.nextDebugLogSec = now + 0.75;
				}

				if (!TFD::Pacify::IsPacified(actor)) {
					if (pending.action == TFD::InteractionRouter::Action::TruceInCombat) {
						LogInCombatDialogueState("pacify_lost", actor, dialogueOpen);
					}
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
						if (pending.pleasureHandoff && IsPleasureDialogueHandoffAction(pending.action)) {
							CacheRecentActor(actor, kRecentActorHoldSec, pending.pleasureSceneActive ? "pleasure_scene_active" : "pleasure_handoff_wait");
							++it;
							continue;
						}

						if (pending.action == TFD::InteractionRouter::Action::TruceInCombat) {
							LogInCombatDialogueState("dialogue_closed", actor, dialogueOpen);
						}
						CacheRecentActor(actor, kRecentActorHoldSec, "dialogue_closed");
						CleanupOne(actor, pending, kCooldownAfterDoneSec, "dialogue_closed", TFD::Pacify::ReleaseReason::DialogueClosed);
						it = gPending.erase(it);
						continue;
					}
				}
				else {
					if (actor->IsInCombat()) {
						if (pending.action == TFD::InteractionRouter::Action::TruceInCombat) {
							LogInCombatDialogueState("tame_broken", actor, dialogueOpen);
						}
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
		if (auto* src = SKSE::GetModCallbackEventSource()) {
			src->AddEventSink(&gPleasureEventSink);
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
		if (auto* src = SKSE::GetModCallbackEventSource()) {
			src->RemoveEventSink(&gPleasureEventSink);
		}

		gRunning.store(false, std::memory_order_release);

		if (gWorker.joinable()) {
			gWorker.join();
		}

		{
			std::scoped_lock lk(gLock);
			ClearAllPendingLocked();
			gCooldownUntil.clear();
			ClearRecentActor("shutdown");
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
			ClearRecentActor("suspend");
		}
	}

	bool IsSuspended()
	{
		return gSuspended.load(std::memory_order_acquire);
	}

	bool BeginForActor(RE::Actor* actor, TFD::InteractionRouter::Action* outAction)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();

		if (outAction) {
			*outAction = TFD::InteractionRouter::Action::None;
		}

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

		auto existingIt = gPending.find(handle);
		if (existingIt != gPending.end()) {
			if (outAction) {
				*outAction = existingIt->second.action;
			}
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

		if (outAction) {
			*outAction = result.action;
		}

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
		pending.pleasureHandoff = false;
		pending.pleasureSceneActive = false;
		pending.nextDebugLogSec = now;

		if (result.action == TFD::InteractionRouter::Action::TruceInCombat) {
			LogInCombatDialogueState("begin_after_router", actor, IsDialogueOpen());
			spdlog::info(
				"[TFD][InCombatDebug] begin session actor={:08X} sessionId={} canOpenDialogue={}",
				actor->GetFormID(),
				result.sessionId,
				TFD::Pacify::CanOpenDialogue(actor) ? 1 : 0);
		}

		if (result.dialogueRequested) {
			if (!TFD::Pacify::CanOpenDialogue(actor)) {
				TFD::Pacify::ReleaseSession(result.sessionId, TFD::Pacify::ReleaseReason::Generic);
				return false;
			}

			const auto bridge = GetBridgeEventNames(result.action);
			if (bridge.assign) {
				if (result.action == TFD::InteractionRouter::Action::TruceInCombat) {
					if (player->IsInCombat()) {
						player->StopCombat();
					}
				}

				SendBridgeEvent(bridge.assign, actor);
				pending.assignSent = true;

				if (result.action == TFD::InteractionRouter::Action::TruceInCombat) {
					LogInCombatDialogueState("assign_sent", actor, IsDialogueOpen());
				}
			}
		}

		if (result.dialogueRequested) {
			CacheRecentActor(actor, kRecentActorHoldSec, "begin");
		}

		gPending.emplace(handle, pending);
		return true;
	}

	RE::Actor* GetRecentActor(double maxAgeSec)
	{
		std::scoped_lock lk(gLock);

		if (gRecentActorHandle == 0) {
			return nullptr;
		}

		const double now = NowSec();

		if (gRecentActorUntilSec > 0.0 && now > gRecentActorUntilSec) {
			ClearRecentActor("expired");
			return nullptr;
		}

		if (maxAgeSec > 0.0 && (now - gRecentActorCachedAtSec) > maxAgeSec) {
			ClearRecentActor("age_limit");
			return nullptr;
		}

		auto sp = RE::Actor::LookupByHandle(gRecentActorHandle);
		auto* actor = sp.get();
		if (!actor || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
			ClearRecentActor("invalid_actor");
			return nullptr;
		}

		return actor;
	}

	void OnPreLoadGame()
	{
		SetSuspended(true);
		CancelAll();
		gTickPending.clear(std::memory_order_release);

		{
			std::scoped_lock lk(gLock);
			ClearRecentActor("pre_load");
		}

		spdlog::info("[TFD][PreCombatGreet] OnPreLoadGame -> suspended + cleared");
	}

	void OnPostLoadGame()
	{
		CancelAll();
		SetSuspended(false);
		gTickPending.clear(std::memory_order_release);

		{
			std::scoped_lock lk(gLock);
			ClearRecentActor("post_load");
		}

		spdlog::info("[TFD][PreCombatGreet] OnPostLoadGame -> resumed clean");
	}

	void CancelAll()
	{
		std::scoped_lock lk(gLock);
		ClearAllPendingLocked();
		gCooldownUntil.clear();
		ClearRecentActor("cancel_all");

		spdlog::info("[TFD][PreCombatGreet] CancelAll");
	}
}
