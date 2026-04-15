#include "TFDActor.h"

#include "TFDPreCombatGreet.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <string_view>

#include <RE/Skyrim.h>
#include <RE/A/ActorValues.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include "TFDDefeatMonitor.h"
#include "TFDInteractionRouter.h"
#include "TFDLocation.h"
#include "TFDHostilityController.h"
#include "TFDTame.h"
#include "TFDFlowController.h"
#include "TFDPleasureRuntime.h"
#include "TFDTransition.h"

namespace TFD::PreCombatGreet
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr double kManualWindowSec = 120.0;
		constexpr double kCooldownAfterDoneSec = 120.0;
		constexpr double kCooldownAfterFailSec = 5.0;
		constexpr double kCooldownAfterPlayerAttackSec = 1.0;
		constexpr double kRecentActorSoftAgeSec = 12.0;
		constexpr double kStickyReopenRetrySec = 0.90;
		constexpr double kStickyTerminalSuppressSec = 0.75;

		constexpr const char* kPreCombatOutcomePayEvent = "TFDPreCombatOutcomePay";
		constexpr const char* kPreCombatOutcomeFightEvent = "TFDPreCombatOutcomeFight";
		constexpr const char* kPreCombatOutcomeCaptiveEvent = "TFDPreCombatOutcomeCaptive";
		constexpr const char* kPreCombatOutcomeJoinEnemyEvent = "TFDPreCombatOutcomeJoinEnemy";
		constexpr const char* kPreCombatOutcomeRecruitEvent = "TFDPreCombatOutcomeRecruit";
		constexpr const char* kPreCombatOutcomeReleaseEvent = "TFDPreCombatOutcomeRelease";
		constexpr const char* kPreCombatOutcomePleasureEvent = "TFDPreCombatOutcomePleasure";

		struct Pending
		{
			RE::FormID truceSessionId{ 0 };
			TFD::InteractionRouter::Action action{ TFD::InteractionRouter::Action::None };

			double expiresSec{ 0.0 };
			bool dialogueRequested{ false };
			bool dialogSeen{ false };
			bool assignSent{ false };

			bool stickyReopenPending{ false };
			bool terminalChoiceCommitted{ false };
			bool payFollowupPending{ false };
			bool pleasureChoiceCommitted{ false };
			double nextStickyRetrySec{ 0.0 };
			double stickySuppressTerminalUntilSec{ 0.0 };
			double nextPreserveHandoffLogSec{ 0.0 };
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
		RE::FormID gRecentActorCellFormID = 0;
		RE::FormID gRecentActorWorldspaceFormID = 0;
		bool gRecentActorInterior = false;
		double gRecentActorLastSoftAgeLogSec = 0.0;

		void ResolvePreCombatTerminalOutcomeLocked(TFD::FlowController::PreCombatOutcome outcome, std::uint32_t actorFormID, const char* reason);
		void MarkTerminalChoiceCommittedLocked(Pending& pending, const char* reason);

		bool IsGraceEventName(std::string_view eventName)
		{
			return eventName == std::string_view("TFDPreCombatOutcomeRelease") ||
				eventName == std::string_view("TFDPreCombatOutcomeFollow") ||
				eventName == std::string_view("TFDPreCombatOutcomeReleaseEnd");
		}

		RE::Actor* ResolveActorFromEventArgRaw(const char* eventArg)
		{
			if (!eventArg || !*eventArg) {
				return nullptr;
			}

			char* end = nullptr;
			const auto raw = std::strtoul(eventArg, &end, 0);
			if (end == nullptr || end == eventArg) {
				return nullptr;
			}

			return RE::TESForm::LookupByID<RE::Actor>(static_cast<RE::FormID>(raw));
		}

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

		static RE::TESObjectCELL* GetPlayerParentCell()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			return player ? player->GetParentCell() : nullptr;
		}

		static RE::TESWorldSpace* GetCellWorldspace(RE::TESObjectCELL* cell)
		{
			if (!cell || cell->IsInteriorCell()) {
				return nullptr;
			}
			return cell->GetRuntimeData().worldSpace;
		}

		static bool IsPlayerStillInCachedSpace()
		{
			if (gRecentActorHandle == 0) {
				return false;
			}

			auto* cell = GetPlayerParentCell();
			if (!cell) {
				return false;
			}

			const bool isInterior = cell->IsInteriorCell();
			if (isInterior != gRecentActorInterior) {
				return false;
			}

			if (isInterior) {
				return cell->GetFormID() == gRecentActorCellFormID;
			}

			auto* ws = GetCellWorldspace(cell);
			const auto wsid = ws ? ws->GetFormID() : 0u;
			return wsid == gRecentActorWorldspaceFormID;
		}

		void CacheRecentActor(RE::Actor* actor, double holdSec, const char* reason)
		{
			if (!actor) {
				return;
			}

			gRecentActorHandle = actor->GetHandle().native_handle();
			gRecentActorCachedAtSec = NowSec();
			gRecentActorUntilSec = gRecentActorCachedAtSec + holdSec;
			if (auto* cell = GetPlayerParentCell()) {
				gRecentActorInterior = cell->IsInteriorCell();
				gRecentActorCellFormID = cell->GetFormID();
				auto* ws = GetCellWorldspace(cell);
				gRecentActorWorldspaceFormID = ws ? ws->GetFormID() : 0u;
			}
			else {
				gRecentActorInterior = false;
				gRecentActorCellFormID = 0;
				gRecentActorWorldspaceFormID = 0;
			}
			gRecentActorLastSoftAgeLogSec = 0.0;

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
			gRecentActorCellFormID = 0;
			gRecentActorWorldspaceFormID = 0;
			gRecentActorInterior = false;
			gRecentActorLastSoftAgeLogSec = 0.0;
		}

		bool HasStickyPendingLocked()
		{
			for (const auto& [handle, pending] : gPending) {
				if (pending.stickyReopenPending && pending.dialogueRequested && !pending.terminalChoiceCommitted) {
					return true;
				}
			}
			return false;
		}

		bool HasCommittedTerminalPendingLocked()
		{
			for (const auto& [handle, pending] : gPending) {
				(void)handle;
				if (!pending.dialogueRequested) {
					continue;
				}

				if (pending.terminalChoiceCommitted || pending.payFollowupPending) {
					return true;
				}
			}
			return false;
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
			default:
				return {};
			}
		}

		void ClearAllBridgeAliases()
		{
			SendBridgeEvent("TFDPreCombatClearAll", nullptr);
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

			if (TFD::FlowController::IsPreCombatBlocked()) {
				return false;
			}

			if (TFD::Transition::IsRecoveryActive()) {
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

		Pending* ResolveTerminalPendingLocked(RE::Actor* actor)
		{
			if (auto* pending = FindPendingLocked(actor)) {
				return pending;
			}

			if (gPending.size() != 1) {
				return nullptr;
			}

			return std::addressof(gPending.begin()->second);
		}

		RE::Actor* ResolveSinglePendingActorLocked()
		{
			if (gPending.size() != 1) {
				return nullptr;
			}

			auto sp = RE::Actor::LookupByHandle(gPending.begin()->first);
			return sp.get();
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

		std::uint32_t ResolveSinglePendingActorFormIDLocked()
		{
			if (gPending.size() != 1) {
				return 0;
			}

			auto it = gPending.begin();
			auto sp = RE::Actor::LookupByHandle(it->first);
			auto* actor = sp.get();
			return actor ? actor->GetFormID() : 0;
		}

		std::uint32_t ResolveFlowActorFormIDLocked(RE::Actor* preferred)
		{
			if (preferred) {
				return preferred->GetFormID();
			}

			const auto pendingFormID = ResolveSinglePendingActorFormIDLocked();
			if (pendingFormID != 0) {
				return pendingFormID;
			}

			return TFD::FlowController::Controller::GetSingleton().GetSnapshot().primaryActorFormID;
		}

		void MarkTerminalChoiceCommittedLocked(Pending& pending, const char* reason)
		{
			pending.terminalChoiceCommitted = true;
			pending.stickyReopenPending = false;
			pending.payFollowupPending = false;
			pending.pleasureChoiceCommitted = false;
			pending.stickySuppressTerminalUntilSec = 0.0;
			pending.nextStickyRetrySec = 0.0;
			pending.nextPreserveHandoffLogSec = 0.0;
			spdlog::info(
				"[TFD][PreCombatGreet] terminal choice committed action={} reason={}",
				TFD::InteractionRouter::ToString(pending.action),
				reason ? reason : "unknown");
		}

		void MarkPleasureChoiceCommittedLocked(Pending& pending, RE::Actor* actor, const char* reason)
		{
			pending.terminalChoiceCommitted = true;
			pending.stickyReopenPending = false;
			pending.payFollowupPending = false;
			pending.pleasureChoiceCommitted = true;
			pending.stickySuppressTerminalUntilSec = 0.0;
			pending.nextStickyRetrySec = 0.0;
			pending.nextPreserveHandoffLogSec = 0.0;
			if (actor) {
				CacheRecentActor(actor, 0.0, reason ? reason : "precombat_pleasure");
			}
			spdlog::info(
				"[TFD][PreCombatGreet] pleasure choice committed action={} actor={:08X} reason={}",
				TFD::InteractionRouter::ToString(pending.action),
				actor ? actor->GetFormID() : 0u,
				reason ? reason : "unknown");
		}

		bool ShouldSuppressTerminalEventLocked(const Pending& pending, RE::Actor* actor, const char* eventName)
		{
			if (!pending.dialogueRequested) {
				return false;
			}

			if (!pending.stickyReopenPending) {
				return false;
			}

			if (pending.terminalChoiceCommitted) {
				return false;
			}

			if (IsDialogueOpen()) {
				return false;
			}

			const double now = NowSec();
			if (now > pending.stickySuppressTerminalUntilSec) {
				return false;
			}

			spdlog::info(
				"[TFD][PreCombatGreet] suppress terminal event={} actor={:08X} action={} stickyUntil={:.2f} now={:.2f}",
				eventName ? eventName : "unknown",
				actor ? actor->GetFormID() : 0u,
				TFD::InteractionRouter::ToString(pending.action),
				pending.stickySuppressTerminalUntilSec,
				now);
			return true;
		}

		void ResolvePreCombatTerminalOutcomeLocked(TFD::FlowController::PreCombatOutcome outcome, std::uint32_t actorFormID, const char* reason)
		{
			if (actorFormID == 0) {
				return;
			}

			auto& flow = TFD::FlowController::Controller::GetSingleton();
			if (!flow.ResolvePreCombatOutcome(outcome, actorFormID, reason ? reason : "unknown")) {
				return;
			}

			if (outcome == TFD::FlowController::PreCombatOutcome::Pay ||
				outcome == TFD::FlowController::PreCombatOutcome::Cancel ||
				outcome == TFD::FlowController::PreCombatOutcome::Failed) {
				flow.CompleteTerminalContext(reason ? reason : "unknown");
			}
		}

		bool ShouldStickyReopenLocked(const Pending& pending)
		{
			(void)pending;
			// Final policy: only BleedOut stays sticky.
			// PreCombat must abort back to aggression when dialogue closes without a committed choice.
			return false;
		}

		bool IsDialogueOpenActiveForPreCombatLocked()
		{
			if (!TFD::InteractionRouter::DialogueOpen::IsActive()) {
				return false;
			}

			return TFD::InteractionRouter::DialogueOpen::GetMode() ==
				TFD::InteractionRouter::DialogueOpen::Mode::PreCombatTruce;
		}

		TFD::Tame::ReleaseReason ResolveDialogueClosedReleaseReasonLocked(const Pending& pending)
		{
			auto snapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();

			if (pending.action == TFD::InteractionRouter::Action::TrucePreCombat) {
				if (pending.terminalChoiceCommitted || pending.payFollowupPending || pending.pleasureChoiceCommitted) {
					return TFD::Tame::ReleaseReason::FlowHandoff;
				}

				if (snapshot.root == TFD::FlowController::RootFlow::Captive ||
					snapshot.root == TFD::FlowController::RootFlow::Victory ||
					snapshot.terminalResolved ||
					snapshot.root == TFD::FlowController::RootFlow::None) {
					return TFD::Tame::ReleaseReason::FlowHandoff;
				}
			}

			return TFD::Tame::ReleaseReason::DialogueClosed;
		}

		void BeginStickyReopenLocked(RE::Actor* actor, Pending& pending, const char* reason)
		{
			if (!actor) {
				return;
			}

			const double now = NowSec();
			pending.dialogSeen = false;
			pending.stickyReopenPending = true;
			pending.terminalChoiceCommitted = false;
			pending.expiresSec = now + kManualWindowSec;
			pending.nextStickyRetrySec = now + kStickyReopenRetrySec;
			pending.stickySuppressTerminalUntilSec = now + kStickyTerminalSuppressSec;
			pending.nextPreserveHandoffLogSec = 0.0;
			CacheRecentActor(actor, 0.0, reason ? reason : "sticky_reopen");

			if (pending.action == TFD::InteractionRouter::Action::TrucePreCombat) {
				TFD::InteractionRouter::DialogueOpen::BeginPreCombatTruce(actor);
			}

			spdlog::info(
				"[TFD][PreCombatGreet] sticky reopen actor={:08X} action={} reason={}",
				actor->GetFormID(),
				TFD::InteractionRouter::ToString(pending.action),
				reason ? reason : "unknown");
		}

		void ArmPreCombatPayFollowupLocked(RE::Actor* actor, Pending& pending, const char* reason)
		{
			if (!actor) {
				return;
			}

			const double now = NowSec();
			pending.payFollowupPending = true;
			pending.pleasureChoiceCommitted = false;
			pending.dialogSeen = true;
			pending.terminalChoiceCommitted = true;
			pending.stickyReopenPending = false;
			pending.nextStickyRetrySec = now;
			pending.stickySuppressTerminalUntilSec = now + kStickyTerminalSuppressSec;
			pending.nextPreserveHandoffLogSec = 0.0;
			CacheRecentActor(actor, 0.0, reason ? reason : "precombat_pay_followup");
			spdlog::info(
				"[TFD][PreCombatGreet] pay followup armed actor={:08X} action={} reason={}",
				actor->GetFormID(),
				TFD::InteractionRouter::ToString(pending.action),
				reason ? reason : "unknown");
		}

		bool HasProtectedPleasurePendingLocked()
		{
			for (auto& [handle, pending] : gPending) {
				if (!pending.dialogueRequested) {
					continue;
				}
				auto sp = RE::Actor::LookupByHandle(handle);
				auto* actor = sp.get();
				if (!actor) {
					continue;
				}
				if (TFD::PleasureRuntime::ShouldProtectPendingDialogue(actor)) {
					return true;
				}
			}
			return false;
		}

		void CleanupOne(
			RE::Actor* actor,
			Pending& pending,
			double cooldownSec,
			const char* reason,
			TFD::Tame::ReleaseReason releaseReason)
		{

			if (pending.truceSessionId != 0) {
				TFD::HostilityController::ReleaseSession(pending.truceSessionId, releaseReason);
				pending.truceSessionId = 0;
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

				if (pending.truceSessionId != 0) {
					TFD::HostilityController::ReleaseSession(pending.truceSessionId, TFD::Tame::ReleaseReason::Generic);
					pending.truceSessionId = 0;
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

			CleanupOne(targetActor, it->second, kCooldownAfterPlayerAttackSec, "player_attack", TFD::Tame::ReleaseReason::PlayerAggression);
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

				if (name == kPreCombatOutcomePayEvent ||
					name == kPreCombatOutcomeFightEvent ||
					name == kPreCombatOutcomeCaptiveEvent ||
					name == kPreCombatOutcomeJoinEnemyEvent ||
					name == kPreCombatOutcomeRecruitEvent ||
					name == kPreCombatOutcomeReleaseEvent ||
					name == kPreCombatOutcomePleasureEvent) {
					std::scoped_lock lk(gLock);
					RE::Actor* pendingActor = actor ? actor : ResolveSinglePendingActorLocked();
					Pending* matchedPending = ResolveTerminalPendingLocked(pendingActor);
					if (matchedPending && ShouldSuppressTerminalEventLocked(*matchedPending, pendingActor, rawName)) {
						return RE::BSEventNotifyControl::kContinue;
					}
					const auto actorFormID = ResolveFlowActorFormIDLocked(pendingActor ? pendingActor : actor);
					if (name == kPreCombatOutcomePayEvent) {
						if (pendingActor && matchedPending) {
							ArmPreCombatPayFollowupLocked(pendingActor, *matchedPending, "mod_event_precombat_pay");
						}
						spdlog::info("[TFD][PreCombatGreet] precombat pay accepted actor={:08X} -> waiting for followup branch", actorFormID);
					}
					else if (name == kPreCombatOutcomeFightEvent) {
						if (matchedPending) {
							MarkTerminalChoiceCommittedLocked(*matchedPending, rawName);
						}
						ResolvePreCombatTerminalOutcomeLocked(TFD::FlowController::PreCombatOutcome::Fight, actorFormID, "mod_event_precombat_fight");
					}
					else if (name == kPreCombatOutcomeCaptiveEvent) {
						if (matchedPending) {
							MarkTerminalChoiceCommittedLocked(*matchedPending, rawName);
						}
						ResolvePreCombatTerminalOutcomeLocked(TFD::FlowController::PreCombatOutcome::Captive, actorFormID, "mod_event_precombat_captive");
					}
					else if (name == kPreCombatOutcomeJoinEnemyEvent) {
						if (matchedPending) {
							MarkTerminalChoiceCommittedLocked(*matchedPending, rawName);
						}
						ResolvePreCombatTerminalOutcomeLocked(TFD::FlowController::PreCombatOutcome::JoinEnemy, actorFormID, "mod_event_precombat_join_enemy");
					}
					else if (name == kPreCombatOutcomeReleaseEvent) {
						if (matchedPending) {
							MarkTerminalChoiceCommittedLocked(*matchedPending, rawName);
						}
						ResolvePreCombatTerminalOutcomeLocked(TFD::FlowController::PreCombatOutcome::Cancel, actorFormID, "mod_event_precombat_release");
					}
					else if (name == kPreCombatOutcomePleasureEvent) {
						if (matchedPending) {
							MarkPleasureChoiceCommittedLocked(*matchedPending, pendingActor, rawName);
						}
						ResolvePreCombatTerminalOutcomeLocked(TFD::FlowController::PreCombatOutcome::Pleasure, actorFormID, "mod_event_precombat_pleasure");
					}
					else {
						if (matchedPending) {
							MarkTerminalChoiceCommittedLocked(*matchedPending, rawName);
						}
						ResolvePreCombatTerminalOutcomeLocked(TFD::FlowController::PreCombatOutcome::Cancel, actorFormID, "mod_event_precombat_recruit");
					}
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

			if (auto* player = RE::PlayerCharacter::GetSingleton(); player && player->IsInCombat()) {
				std::scoped_lock lk(gLock);
				if (gRecentActorHandle != 0 && !HasStickyPendingLocked()) {
					ClearRecentActor("player_entered_combat");
				}
			}

			if (TFD::Transition::IsRecoveryActive()) {
				std::scoped_lock lk(gLock);
				ClearAllPendingLocked();
				return;
			}

			if (IsPlayerDown()) {
				std::scoped_lock lk(gLock);
				ClearAllPendingLocked();
				return;
			}

			const double now = NowSec();

			if (TFD::FlowController::IsPreCombatBlocked()) {
				std::scoped_lock lk(gLock);
				const bool hasPending = !gPending.empty();
				const auto ctxKind = TFD::FlowController::GetDialogueContextKind();
				const auto holdKind = TFD::FlowController::GetPassiveHoldKind();
				const bool preserveAfterPleasureHandoff =
					ctxKind == TFD::FlowController::DialogueContextKind::AfterPleasure &&
					holdKind == TFD::FlowController::PassiveHoldKind::Pleasure;
				const bool preserveProtectedHandoff =
					preserveAfterPleasureHandoff ||
					HasCommittedTerminalPendingLocked() ||
					(TFD::FlowController::IsPassiveHoldProtectedHandoff() &&
						HasProtectedPleasurePendingLocked());

				if (!preserveProtectedHandoff) {
					if (hasPending) {
						spdlog::info("[TFD][PreCombatGreet] blocked ctx={} hold={} -> clear pending",
							TFD::FlowController::GetDialogueContextName(),
							TFD::FlowController::GetPassiveHoldName());
					}
					ClearAllPendingLocked();
					return;
				}

				if (hasPending) {
					bool shouldLog = false;
					for (auto& [handle, pending] : gPending) {
						(void)handle;
						if (now >= pending.nextPreserveHandoffLogSec) {
							pending.nextPreserveHandoffLogSec = now + 1.0;
							shouldLog = true;
							break;
						}
					}
					if (shouldLog) {
						spdlog::info("[TFD][PreCombatGreet] blocked ctx={} hold={} but preserve handoff",
							TFD::FlowController::GetDialogueContextName(),
							TFD::FlowController::GetPassiveHoldName());
					}
				}
			}

			const bool dialogueOpen = IsDialogueOpen();

			TFD::HostilityController::Update(now);

			std::scoped_lock lk(gLock);

			for (auto it = gPending.begin(); it != gPending.end();) {
				auto sp = RE::Actor::LookupByHandle(it->first);
				auto* actor = sp.get();
				auto& pending = it->second;

				if (!IsActorStillValid(actor)) {
					if (pending.truceSessionId != 0) {
						TFD::HostilityController::ReleaseSession(pending.truceSessionId, TFD::Tame::ReleaseReason::Generic);
					}
					it = gPending.erase(it);
					continue;
				}

				if (!TFD::HostilityController::IsSuppressed(actor)) {
					CleanupOne(actor, pending, kCooldownAfterFailSec, "truce_lost", TFD::Tame::ReleaseReason::Generic);
					it = gPending.erase(it);
					continue;
				}

				if (pending.dialogueRequested) {
					if (dialogueOpen) {
						pending.dialogSeen = true;
						pending.stickyReopenPending = false;
						pending.nextStickyRetrySec = 0.0;
						pending.stickySuppressTerminalUntilSec = 0.0;
						++it;
						continue;
					}

					if (pending.payFollowupPending) {
						if (!IsDialogueOpenActiveForPreCombatLocked()) {
							pending.payFollowupPending = false;
							BeginStickyReopenLocked(actor, pending, "precombat_pay_followup");
						}
						++it;
						continue;
					}

					if (pending.stickyReopenPending && !pending.terminalChoiceCommitted && now >= pending.nextStickyRetrySec) {
						if (IsDialogueOpenActiveForPreCombatLocked()) {
							pending.nextStickyRetrySec = now + kStickyReopenRetrySec;
							++it;
							continue;
						}
						BeginStickyReopenLocked(actor, pending, "sticky_watchdog");
						++it;
						continue;
					}

					if (pending.dialogSeen) {
						if (TFD::PleasureRuntime::ShouldProtectPendingDialogue(actor)) {
							CacheRecentActor(actor, 0.0, "pleasure_runtime_handoff");
							++it;
							continue;
						}

						if (ShouldStickyReopenLocked(pending)) {
							BeginStickyReopenLocked(actor, pending, "dialogue_closed_no_choice");
							++it;
							continue;
						}

						auto releaseReason = ResolveDialogueClosedReleaseReasonLocked(pending);
						CacheRecentActor(actor, 0.0, releaseReason == TFD::Tame::ReleaseReason::FlowHandoff ? "dialogue_handoff" : "dialogue_closed_abort");
						CleanupOne(actor, pending, kCooldownAfterDoneSec, releaseReason == TFD::Tame::ReleaseReason::FlowHandoff ? "dialogue_handoff" : "dialogue_closed_abort", releaseReason);
						it = gPending.erase(it);
						continue;
					}
				}
				else {
					if (actor->IsInCombat()) {
						CleanupOne(actor, pending, kCooldownAfterFailSec, "tame_broken", TFD::Tame::ReleaseReason::TameBroken);
						it = gPending.erase(it);
						continue;
					}
				}

				if (now >= pending.expiresSec) {
					CleanupOne(actor, pending, kCooldownAfterFailSec, "hidden_failsafe_expired", TFD::Tame::ReleaseReason::HardFailsafeExpired);
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

		if (TFD::Transition::IsRecoveryActive()) {
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

		if (result.action != TFD::InteractionRouter::Action::TrucePreCombat) {
			TFD::HostilityController::ReleaseSession(result.sessionId, TFD::Tame::ReleaseReason::Generic);
			spdlog::warn(
				"[TFD][PreCombatGreet] rejected non-precombat action actor={:08X} action={}",
				actor->GetFormID(),
				TFD::InteractionRouter::ToString(result.action));
			return false;
		}

		if (!gPending.empty()) {
			ClearAllPendingLocked();
		}

		Pending pending{};
		pending.truceSessionId = result.sessionId;
		pending.action = result.action;
		pending.expiresSec = now + kManualWindowSec;
		pending.dialogueRequested = result.dialogueRequested;
		pending.dialogSeen = false;
		pending.assignSent = false;
		pending.stickyReopenPending = false;
		pending.terminalChoiceCommitted = false;
		pending.nextStickyRetrySec = 0.0;
		pending.stickySuppressTerminalUntilSec = 0.0;

		if (result.dialogueRequested) {
			if (!TFD::HostilityController::CanOpenDialogue(actor)) {
				TFD::HostilityController::ReleaseSession(result.sessionId, TFD::Tame::ReleaseReason::Generic);
				return false;
			}
		}

		if (result.dialogueRequested) {
			CacheRecentActor(actor, 0.0, "begin");
		}

		auto& flow = TFD::FlowController::Controller::GetSingleton();
		if (result.action == TFD::InteractionRouter::Action::TrucePreCombat) {
			(void)flow.BeginPreCombat(actor->GetFormID(), "precombat_begin");
			if (result.dialogueRequested) {
				(void)flow.BeginTruceDecision(actor->GetFormID(), "precombat_dialogue_begin");
			}
		}

		gPending.emplace(handle, pending);

		if (result.dialogueRequested) {
			TFD::InteractionRouter::DialogueOpen::BeginPreCombatTruce(actor);
		}

		return true;
	}

	bool HandleGraceModEvent(const GraceEventContext& context, const GraceEventHandlers& handlers)
	{
		auto* actor = context.actor;
		if (!actor) {
			return false;
		}

		const std::string_view eventName = context.eventName ? std::string_view(context.eventName) : std::string_view{};
		if (!IsGraceEventName(eventName)) {
			return false;
		}
		if (eventName == std::string_view("TFDPreCombatOutcomeReleaseEnd")) {
			if (handlers.removeGrace) {
				handlers.removeGrace(actor, "precombat_release_end");
			}

			spdlog::info(
				"[TFD][PreCombatGreet] grace handled actor={:08X} reason=precombat_release_end",
				actor->GetFormID());
			return true;
		}

		const char* graceReason =
			eventName == std::string_view("TFDPreCombatOutcomeFollow") ?
			"precombat_follow" :
			"precombat_release";

		const double durationSec = context.durationSec > 0.0 ? context.durationSec : 20.0;
		if (handlers.applyGrace) {
			handlers.applyGrace(actor, durationSec, graceReason);
		}

		spdlog::info(
			"[TFD][PreCombatGreet] grace handled actor={:08X} reason={} duration={}",
			actor->GetFormID(),
			graceReason,
			durationSec);
		return true;
	}

	bool HandleGraceModEventRaw(const char* eventName, const char* eventArg, double durationSec, const GraceEventHandlers& handlers)
	{
		GraceEventContext context{};
		context.eventName = eventName;
		context.actor = ResolveActorFromEventArgRaw(eventArg);
		context.durationSec = durationSec > 0.0 ? durationSec : 20.0;
		return HandleGraceModEvent(context, handlers);
	}

	bool HandleModEventRaw(const char* eventName, const char* eventArg, double durationSec, const GraceEventHandlers& handlers)
	{
		const std::string_view name = eventName ? std::string_view(eventName) : std::string_view{};
		if (!IsGraceEventName(name)) {
			return false;
		}
		return HandleGraceModEventRaw(eventName, eventArg, durationSec, handlers);
	}

	bool HandleModCallbackEvent(const SKSE::ModCallbackEvent* ev, const GraceEventHandlers& handlers)
	{
		if (!ev) {
			return false;
		}

		const char* eventName = ev->eventName.c_str();
		const char* eventArg = ev->strArg.c_str();
		const double durationSec = ev->numArg > 0.0f ? static_cast<double>(ev->numArg) : 20.0;
		return HandleModEventRaw(eventName, eventArg, durationSec, handlers);
	}

	RE::Actor* ResolveRecentAggressor(float radius, double maxAgeSec)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return nullptr;
		}

		auto* actor = GetRecentActor(maxAgeSec);
		if (!actor || actor == player) {
			return nullptr;
		}
		if (actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
			return nullptr;
		}
		if (actor->IsPlayerTeammate() || TFD::Tame::IsCompanion(actor)) {
			return nullptr;
		}

		auto* playerCell = player->GetParentCell();
		auto* actorCell = actor->GetParentCell();
		if (playerCell && actorCell != playerCell) {
			return nullptr;
		}
		auto* playerWs = player->GetWorldspace();
		if (playerWs && actor->GetWorldspace() != playerWs) {
			return nullptr;
		}

		const auto pp = player->GetPosition();
		const auto ap = actor->GetPosition();
		const float dx = ap.x - pp.x;
		const float dy = ap.y - pp.y;
		const float dz = ap.z - pp.z;
		const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (radius > 0.0f && dist > radius) {
			return nullptr;
		}

		if (actor->IsHostileToActor(player) || actor->IsInCombat()) {
			spdlog::info("[TFD][PreCombatGreet] using recent aggressor actor={:08X} dist={:.1f}", actor->GetFormID(), dist);
			return actor;
		}

		auto targetSp = actor->GetActorRuntimeData().currentCombatTarget.get();
		auto* currentTarget = targetSp.get();
		if (currentTarget == player || (currentTarget && currentTarget->IsPlayerTeammate())) {
			spdlog::info("[TFD][PreCombatGreet] using recent aggressor actor={:08X} dist={:.1f} reason=current_target", actor->GetFormID(), dist);
			return actor;
		}

		return nullptr;
	}

	RE::Actor* ResolveRecentAggressorAndCache(float radius, RE::ActorHandle& cacheHandle, double maxAgeSec)
	{
		auto* actor = ResolveRecentAggressor(radius, maxAgeSec);
		if (actor) {
			cacheHandle = actor->GetHandle();
		}
		return actor;
	}

	RE::Actor* GetRecentActor(double maxAgeSec)
	{
		std::scoped_lock lk(gLock);

		if (gRecentActorHandle == 0) {
			return nullptr;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (player && player->IsInCombat()) {
			if (!HasStickyPendingLocked()) {
				ClearRecentActor("player_entered_combat");
				return nullptr;
			}
		}

		const double now = NowSec();
		if (maxAgeSec > 0.0 && (now - gRecentActorCachedAtSec) > maxAgeSec) {
			if (gRecentActorLastSoftAgeLogSec <= 0.0) {
				spdlog::info("[TFD][PreCombatGreet] recent actor age exceeds soft limit but retained actorHandle={:08X} age={:.1f}s limit={:.1f}s",
					gRecentActorHandle,
					now - gRecentActorCachedAtSec,
					maxAgeSec);
				gRecentActorLastSoftAgeLogSec = now;
			}
		}

		if (!IsPlayerStillInCachedSpace()) {
			ClearRecentActor("space_changed");
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

	void OnLoadingScreenClosed()
	{
		std::scoped_lock lk(gLock);
		ClearRecentActor("loading_screen_closed");
	}

	void OnCaptiveHandoffArrived()
	{
		std::scoped_lock lk(gLock);
		ClearRecentActor("captive_handoff_complete");
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
