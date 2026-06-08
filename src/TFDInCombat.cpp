#include "TFDInCombat.h"

#include <atomic>
#include <mutex>
#include <string_view>
#include <vector>

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

#include "TFDFlowController.h"
#include "TFDInteractionRouter.h"
#include "TFDActor.h"
#include "TFDTeammateManager.h"
#include "TFDHostilityController.h"

namespace TFD::InCombat
{
	namespace
	{
		std::atomic_bool g_installed{ false };
		std::atomic<State> g_state{ State::Idle };
		std::atomic<std::uint32_t> g_primaryActorFormID{ 0 };
		std::mutex g_lock{};
		DialogueOutcome g_dialogueOutcome = DialogueOutcome::None;

		class HitSink final : public RE::BSTEventSink<RE::TESHitEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(
				const RE::TESHitEvent* ev,
				RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				(void)TFD::HostilityController::AbortActiveInCombatTruceOnHit(ev, "incombat_hit_damage_interrupt");
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		HitSink g_hitSink{};

		RE::Actor* ResolveActorByFormID(std::uint32_t actorFormID)
		{
			if (actorFormID == 0) {
				return nullptr;
			}
			return RE::TESForm::LookupByID<RE::Actor>(actorFormID);
		}

		bool IsTemporaryFollowOrPlayerSideActor(RE::Actor* actor)
		{
			return actor &&
				(TFD::Actor::Ops::HasTemporaryFollowLock(actor) ||
					actor->IsPlayerTeammate() ||
					TFD::TeammateManager::IsActiveFollowerActor(actor) ||
					TFD::TeammateManager::IsPlayerSideTeammateActor(actor));
		}

		void SetStateLocked(State state, std::uint32_t actorFormID)
		{
			g_state.store(state, std::memory_order_release);
			g_primaryActorFormID.store(actorFormID, std::memory_order_release);
		}

		RE::TESFaction* ResolveFactionByEditorID(const char* editorID)
		{
			if (!editorID || editorID[0] == '\0') {
				return nullptr;
			}
			return RE::TESForm::LookupByEditorID<RE::TESFaction>(editorID);
		}

		void ClearStaleLoadHelperFactions()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}

			auto* inCombatTruceFaction = ResolveFactionByEditorID("TFDInCombatTruceFaction");
			auto* pacifyFaction = ResolveFactionByEditorID("TFDPacifyFaction");
			auto* expiredTeammateFaction = ResolveFactionByEditorID("TFDExpiredTeammate");
			auto* tfdTeammateFaction = ResolveFactionByEditorID("TFDTeammateFaction");
			if (!inCombatTruceFaction && !pacifyFaction && !expiredTeammateFaction) {
				return;
			}

			const auto snapshot = TFD::Actor::BuildSnapshot(12000.0f, false);
			unsigned int changedCount = 0;
			for (const auto& info : snapshot.actors) {
				auto* actor = info.get();
				if (!actor || actor == player || actor->IsDead() || actor->IsDisabled()) {
					continue;
				}
				// R93X: do not use IsActiveFollowerActor() here. In older/rejected
				// release paths, TFDExpiredTeammate itself made hostile bandits look
				// like TFD teammates, so the load cleanup skipped exactly the dirty
				// actors it was supposed to clean. Only preserve real teammate state.
				const bool realTeammate =
					actor->IsPlayerTeammate() ||
					(tfdTeammateFaction && actor->IsInFaction(tfdTeammateFaction));
				if (realTeammate) {
					continue;
				}

				bool changed = false;
				if (inCombatTruceFaction && actor->IsInFaction(inCombatTruceFaction)) {
					actor->RemoveFromFaction(inCombatTruceFaction);
					changed = true;
				}
				if (pacifyFaction && actor->IsInFaction(pacifyFaction)) {
					actor->RemoveFromFaction(pacifyFaction);
					changed = true;
				}
				if (expiredTeammateFaction && actor->IsInFaction(expiredTeammateFaction)) {
					actor->RemoveFromFaction(expiredTeammateFaction);
					changed = true;
				}
				if (changed) {
					++changedCount;
					const bool hostileAfterCleanup = actor->IsHostileToActor(player) || info.hostileToPlayer;

					// P12LOAD: load cleanup must be a passive stale-state scrub.  It must not
					// mark actors/player as attacked, pulse critical detection, evaluate combat
					// packages, or dispatch TFDInCombatResumeCombat.  Those calls caused a clean
					// reload to actively wake the same hostile encounter that the user was trying
					// to reset.  Vanilla may still decide hostility after the load settles, but
					// TFD should not force it from the stale-faction cleanup path.
					spdlog::info(
						"[TFD][InCombat][P12LOAD] load cleanup stale truce/pacify/expired factions actor={:08X} dist={:.1f} hostileAfterCleanup={} wake=0 resume=0",
						actor->GetFormID(),
						info.dist,
						hostileAfterCleanup ? 1 : 0);
				}
			}

			if (changedCount > 0) {
				spdlog::info("[TFD][InCombat][R93Y] load cleanup changed actors={}", changedCount);
			}
		}
	}

	void Install()
	{
		if (g_installed.exchange(true, std::memory_order_acq_rel)) {
			return;
		}

		if (auto* scripts = RE::ScriptEventSourceHolder::GetSingleton()) {
			scripts->AddEventSink(&g_hitSink);
		}

		spdlog::info("[TFD][InCombat] Install");
	}

	void ResetForLoad()
	{
		{
			std::scoped_lock lk(g_lock);
			SetStateLocked(State::Idle, 0);
			g_dialogueOutcome = DialogueOutcome::None;
		}

		ClearStaleLoadHelperFactions();
		spdlog::info("[TFD][InCombat] ResetForLoad");
	}

	void Shutdown()
	{
		if (auto* scripts = RE::ScriptEventSourceHolder::GetSingleton()) {
			scripts->RemoveEventSink(&g_hitSink);
		}

		std::scoped_lock lk(g_lock);
		SetStateLocked(State::Idle, 0);
		g_dialogueOutcome = DialogueOutcome::None;
		g_installed.store(false, std::memory_order_release);
	}

	void ObserveCombat(std::uint32_t actorFormID, bool active, const char* reason)
	{
		auto& flow = TFD::FlowController::Controller::GetSingleton();
		std::scoped_lock lk(g_lock);

		auto* observedActor = ResolveActorByFormID(actorFormID);
		const bool observedTemporaryFollow = observedActor && TFD::Actor::Ops::HasTemporaryFollowLock(observedActor);
		const bool observedPlayerSide = observedActor &&
			(observedActor->IsPlayerTeammate() ||
				TFD::TeammateManager::IsActiveFollowerActor(observedActor) ||
				TFD::TeammateManager::IsPlayerSideTeammateActor(observedActor));
		if (observedTemporaryFollow || observedPlayerSide) {
			if (g_primaryActorFormID.load(std::memory_order_acquire) == actorFormID) {
				SetStateLocked(State::Idle, 0);
				g_dialogueOutcome = DialogueOutcome::None;
			}
			spdlog::info(
				"[TFD][InCombat][R144] observe ignored actor={:08X} active={} reason={} temporaryFollow={} playerSide={}",
				actorFormID,
				active ? 1 : 0,
				reason ? reason : "incombat_observe",
				observedTemporaryFollow ? 1 : 0,
				observedPlayerSide ? 1 : 0);
			return;
		}

		if (active) {
			flow.NotifyCombatStarted(actorFormID, reason ? reason : "incombat_observe_start");
			if (g_state.load(std::memory_order_acquire) == State::Idle) {
				SetStateLocked(State::Observed, actorFormID);
			}
			return;
		}

		flow.NotifyCombatEnded(reason ? reason : "incombat_observe_end");
		SetStateLocked(State::Idle, 0);
		g_dialogueOutcome = DialogueOutcome::None;
	}

	bool BeginTruce(std::uint32_t actorFormID, bool dialogueRequested, const char* reason)
	{
		auto& flow = TFD::FlowController::Controller::GetSingleton();
		std::scoped_lock lk(g_lock);

		auto* truceActor = ResolveActorByFormID(actorFormID);
		const bool truceTemporaryFollow = truceActor && TFD::Actor::Ops::HasTemporaryFollowLock(truceActor);
		const bool trucePlayerSide = truceActor &&
			(truceActor->IsPlayerTeammate() ||
				TFD::TeammateManager::IsActiveFollowerActor(truceActor) ||
				TFD::TeammateManager::IsPlayerSideTeammateActor(truceActor));
		if (truceTemporaryFollow || trucePlayerSide) {
			if (g_primaryActorFormID.load(std::memory_order_acquire) == actorFormID) {
				SetStateLocked(State::Idle, 0);
				g_dialogueOutcome = DialogueOutcome::None;
			}
			spdlog::info(
				"[TFD][InCombat][R144] BeginTruce blocked actor={:08X} dialogueRequested={} reason={} temporaryFollow={} playerSide={}",
				actorFormID,
				dialogueRequested ? 1 : 0,
				reason ? reason : "incombat_truce_begin",
				truceTemporaryFollow ? 1 : 0,
				trucePlayerSide ? 1 : 0);
			return false;
		}

		const auto why = reason ? reason : "incombat_truce_begin";
		const bool began = flow.BeginInCombat(actorFormID, why);
		bool gateAccepted = true;
		if (began && dialogueRequested) {
			gateAccepted = flow.BeginTruceDecision(actorFormID, "incombat_truce_dialogue_begin");
		}

		if (!began || !gateAccepted) {
			spdlog::warn("[TFD][InCombat] BeginTruce rejected actor={:08X} dialogueRequested={} began={} gateAccepted={}",
				actorFormID,
				dialogueRequested ? 1 : 0,
				began ? 1 : 0,
				gateAccepted ? 1 : 0);
			return false;
		}

		SetStateLocked(dialogueRequested ? State::TruceDialogue : State::Observed, actorFormID);
		spdlog::info("[TFD][InCombat] BeginTruce actor={:08X} dialogueRequested={} began={} gateAccepted={}",
			actorFormID,
			dialogueRequested ? 1 : 0,
			began ? 1 : 0,
			gateAccepted ? 1 : 0);
		return true;
	}

	void NoteAfterPleasure(std::uint32_t actorFormID, const char* reason)
	{
		std::scoped_lock lk(g_lock);
		SetStateLocked(State::AfterPleasure, actorFormID);
		spdlog::info("[TFD][InCombat] NoteAfterPleasure actor={:08X} reason={}",
			actorFormID,
			reason ? reason : "-");
	}

	void Complete(const char* reason)
	{
		std::scoped_lock lk(g_lock);
		SetStateLocked(State::Idle, 0);
		g_dialogueOutcome = DialogueOutcome::None;
		TFD::InteractionRouter::ClearInteractionStateValue();
		spdlog::info("[TFD][InCombat] Complete reason={} interactionCleared=1", reason ? reason : "-");
	}

	bool IsActive()
	{
		return g_state.load(std::memory_order_acquire) != State::Idle;
	}

	State GetState()
	{
		return g_state.load(std::memory_order_acquire);
	}

	std::uint32_t GetPrimaryActorFormID()
	{
		return g_primaryActorFormID.load(std::memory_order_acquire);
	}

	const char* GetStateName()
	{
		switch (GetState()) {
		case State::Idle:
			return "Idle";
		case State::Observed:
			return "Observed";
		case State::TruceDialogue:
			return "TruceDialogue";
		case State::AfterPleasure:
			return "AfterPleasure";
		default:
			return "Unknown";
		}
	}

	const char* GetDialogueOutcomeName(DialogueOutcome outcome)
	{
		switch (outcome) {
		case DialogueOutcome::PayRelease:
			return "pay_release";
		case DialogueOutcome::Pleasure:
			return "pleasure";
		case DialogueOutcome::Captive:
			return "captive";
		default:
			return "none";
		}
	}

	void SetDialogueOutcome(DialogueOutcome outcome, const char* reason)
	{
		std::scoped_lock lk(g_lock);
		g_dialogueOutcome = outcome;
		spdlog::info("[TFD][InCombat] dialogue outcome set={} reason={}",
			GetDialogueOutcomeName(outcome),
			reason ? reason : "unknown");
	}

	void ClearDialogueOutcome(const char* reason)
	{
		std::scoped_lock lk(g_lock);
		if (g_dialogueOutcome != DialogueOutcome::None) {
			spdlog::info("[TFD][InCombat] dialogue outcome cleared={} reason={}",
				GetDialogueOutcomeName(g_dialogueOutcome),
				reason ? reason : "unknown");
		}
		g_dialogueOutcome = DialogueOutcome::None;
	}

	DialogueOutcome GetDialogueOutcome()
	{
		std::scoped_lock lk(g_lock);
		return g_dialogueOutcome;
	}

	bool HandleOutcomePayEvent(const OutcomeEventContext& context, const OutcomeEventHandlers& handlers)
	{
		if (!context.inCombatState) {
			return true;
		}

		// P5PAY: InCombat Pay is a linked-topic branch selector, not PayRelease.
		// Keep dialogue outcome clear so dialogue close does not resolve Release
		// before the player chooses the real Pay follow-up topic.
		if (handlers.clearDialogueOutcome) {
			handlers.clearDialogueOutcome("mod_event_pay_branch");
		}
		auto* payActor = context.actor ? context.actor : ResolveActorByFormID(context.actorFormID);
		if (payActor) {
			TFD::HostilityController::ArmPayDialoguePassiveGuard(payActor, 30.0, "incombat_pay_linked_topic");
		}
		spdlog::info("[TFD][InCombat][P5PAY] pay branch accepted actor={:08X} keepDialogue=1 terminal=0 payRelease=0",
			context.actorFormID);
		return true;
	}

	bool HandleOutcomePleasureEvent(const OutcomeEventContext& context, const OutcomeEventHandlers& handlers)
	{
		if (!context.inCombatState) {
			return true;
		}
		if (handlers.armOutcomeWindow) {
			handlers.armOutcomeWindow("mod_event_pleasure", 12.0);
		}
		if (context.preserveCaptive) {
			if (handlers.beginCaptivePleasureFlow &&
				!handlers.beginCaptivePleasureFlow(context.actorFormID, "mod_event_pleasure")) {
				spdlog::warn("[TFD][InCombat] ignore mod_event_pleasure reason=captive_flow_reject actor={:08X}", context.actorFormID);
				return true;
			}
			if (handlers.setDialogueOutcome) {
				handlers.setDialogueOutcome(DialogueOutcome::Pleasure, "mod_event_pleasure");
			}
			if (handlers.prepareCaptivePleasureScene) {
				handlers.prepareCaptivePleasureScene("mod_event_pleasure");
			}
			if (handlers.completeCaptivePleasureHandoff) {
				handlers.completeCaptivePleasureHandoff("mod_event_pleasure");
			}
			return true;
		}
		if (handlers.setDialogueOutcome) {
			handlers.setDialogueOutcome(DialogueOutcome::Pleasure, "mod_event_pleasure");
		}
		if (handlers.prepareInCombatPleasureScene) {
			handlers.prepareInCombatPleasureScene("mod_event_pleasure");
		}
		if (handlers.completeInCombatPleasureHandoff) {
			handlers.completeInCombatPleasureHandoff("mod_event_pleasure");
		}
		return true;
	}

	bool HandleOutcomeCaptiveEvent(const OutcomeEventContext& context, const OutcomeEventHandlers& handlers)
	{
		if (!context.inCombatState) {
			return true;
		}
		if (handlers.armOutcomeWindow) {
			handlers.armOutcomeWindow("mod_event_captive", 3.0);
		}
		if (handlers.beginCaptiveFlow &&
			!handlers.beginCaptiveFlow(context.actorFormID, "mod_event_captive")) {
			spdlog::warn("[TFD][InCombat] ignore mod_event_captive reason=flow_reject actor={:08X}", context.actorFormID);
			return true;
		}
		if (handlers.setDialogueOutcome) {
			handlers.setDialogueOutcome(DialogueOutcome::Captive, "mod_event_captive");
		}
		return true;
	}

	bool HandleOutcomeResetEvent(const OutcomeEventContext&, const OutcomeEventHandlers& handlers)
	{
		if (handlers.clearDialogueOutcome) {
			handlers.clearDialogueOutcome("mod_event_reset");
		}
		return true;
	}

	bool DispatchOutcomeModEvent(const char* rawEventName,
		std::uint32_t actorFormID,
		bool preserveCaptive,
		const OutcomeEventHandlers& handlers)
	{
		const auto eventName = rawEventName ? std::string_view(rawEventName) : std::string_view{};
		if (eventName.empty()) {
			return false;
		}

		if (!actorFormID) {
			actorFormID = GetPrimaryActorFormID();
		}

		OutcomeEventContext context{};
		context.rawEventName = rawEventName;
		context.actorFormID = actorFormID;
		context.actor = actorFormID != 0 ? RE::TESForm::LookupByID<RE::Actor>(actorFormID) : nullptr;
		context.inCombatState = IsActive();
		context.preserveCaptive = preserveCaptive;

		if (eventName == std::string_view("TFDInCombatOutcomePay")) {
			context.eventName = "mod_event_pay";
			return HandleOutcomePayEvent(context, handlers);
		}
		if (eventName == std::string_view("TFDInCombatOutcomePleasure")) {
			context.eventName = "mod_event_pleasure";
			return HandleOutcomePleasureEvent(context, handlers);
		}
		if (eventName == std::string_view("TFDInCombatOutcomeCaptive")) {
			context.eventName = "mod_event_captive";
			return HandleOutcomeCaptiveEvent(context, handlers);
		}
		if (eventName == std::string_view("TFDInCombatOutcomeReset")) {
			context.eventName = "mod_event_reset";
			return HandleOutcomeResetEvent(context, handlers);
		}

		return false;
	}
	bool CompletePayRelease(const char* reason, const CompletionHandlers& handlers)
	{
		const auto* why = reason ? reason : "incombat_pay_release";
		if (handlers.clearDialogueOutcome) {
			handlers.clearDialogueOutcome(why);
		}
		if (handlers.completeFlow) {
			handlers.completeFlow(why);
		}
		if (handlers.cancelGreet) {
			handlers.cancelGreet(why);
		}
		spdlog::info("[TFD][InCombat] pay release complete reason={}", why);
		return true;
	}

	bool CompleteDialogueClosedFlow(const char* reason, const CompletionHandlers& handlers)
	{
		const auto* why = reason ? reason : "incombat_dialogue_closed";
		if (handlers.clearDialogueOutcome) {
			handlers.clearDialogueOutcome(why);
		}
		if (handlers.completeFlow) {
			handlers.completeFlow(why);
		}
		if (handlers.cancelGreet) {
			handlers.cancelGreet(why);
		}
		spdlog::info("[TFD][InCombat] dialogue closed complete reason={}", why);
		return true;
	}



	bool HandleAfterPleasureEnter(RE::Actor* actor, const char* reason, const AfterPleasureHandlers& handlers)
	{
		if (!actor) {
			return false;
		}
		const auto actorFormID = actor->GetFormID();
		const auto* why = reason ? reason : "after_pleasure_enter";
		if (handlers.noteAfterPleasure) {
			handlers.noteAfterPleasure(actorFormID, why);
		}
		if (handlers.beginAfterPleasureGreet) {
			(void)handlers.beginAfterPleasureGreet(actor, why);
		}
		spdlog::info("[TFD][InCombat] after pleasure handled actor={:08X} reason={}", actorFormID, why);
		return true;
	}

	bool HandleReleaseFollowEvent(const GraceEventContext& context, const GraceEventHandlers& handlers)
	{
		if (!context.actor) {
			return false;
		}

		// R93V: InCombat Release/Follow must not use the generic ReleaseFollowGrace
		// helper. That helper currently marks actors with TFDExpiredTeammate, which is
		// also consumed by TFDTeammateManager as a converted teammate marker. In the
		// InCombat Pay > Release path that caused released bandits to be pulled into
		// teammate aliases/contracts and remain pacified across saves.
		//
		// Papyrus already commits the local outcome:
		// - Release: stops combat/alarm for the speaker before this event.
		// - Follow: TemporaryFollowerQuest owns the follow behavior.
		// Native only needs to complete the flow and release the truce session.
		// Keep the handler for typed outcome logging, but intentionally do not call
		// handlers.applyGrace here.
		const auto name = context.eventName ? std::string_view(context.eventName) : std::string_view{};
		const char* terminalReason = name == std::string_view("TFDInCombatOutcomeFollow") ?
			"incombat_follow_terminal_no_teammate_grace" :
			"incombat_release_terminal_no_teammate_grace";

		(void)handlers;
		spdlog::info("[TFD][InCombat][R93V] release/follow handled actor={:08X} reason={} requestedDuration={:.1f} action=no_expired_teammate_grace",
			context.actor->GetFormID(),
			terminalReason,
			context.durationSec > 0.0 ? context.durationSec : 0.0);
		return true;
	}

}
