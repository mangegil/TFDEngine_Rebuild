#include "TFDInCombat.h"

#include <atomic>
#include <mutex>
#include <string_view>

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

#include "TFDFlowController.h"

namespace TFD::InCombat
{
	namespace
	{
		std::atomic_bool g_installed{ false };
		std::atomic<State> g_state{ State::Idle };
		std::atomic<std::uint32_t> g_primaryActorFormID{ 0 };
		std::mutex g_lock{};
		DialogueOutcome g_dialogueOutcome = DialogueOutcome::None;

		void SetStateLocked(State state, std::uint32_t actorFormID)
		{
			g_state.store(state, std::memory_order_release);
			g_primaryActorFormID.store(actorFormID, std::memory_order_release);
		}
	}

	void Install()
	{
		if (g_installed.exchange(true, std::memory_order_acq_rel)) {
			return;
		}

		spdlog::info("[TFD][InCombat] Install");
	}

	void ResetForLoad()
	{
		std::scoped_lock lk(g_lock);
		SetStateLocked(State::Idle, 0);
		g_dialogueOutcome = DialogueOutcome::None;
		spdlog::info("[TFD][InCombat] ResetForLoad");
	}

	void Shutdown()
	{
		std::scoped_lock lk(g_lock);
		SetStateLocked(State::Idle, 0);
		g_dialogueOutcome = DialogueOutcome::None;
		g_installed.store(false, std::memory_order_release);
	}

	void ObserveCombat(std::uint32_t actorFormID, bool active, const char* reason)
	{
		auto& flow = TFD::FlowController::Controller::GetSingleton();
		std::scoped_lock lk(g_lock);

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
		spdlog::info("[TFD][InCombat] Complete reason={}", reason ? reason : "-");
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
		if (handlers.armOutcomeWindow) {
			handlers.armOutcomeWindow("mod_event_pay", 3.0);
		}
		if (handlers.setDialogueOutcome) {
			handlers.setDialogueOutcome(DialogueOutcome::PayRelease, "mod_event_pay");
		}
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
		const auto name = context.eventName ? std::string_view(context.eventName) : std::string_view{};
		const char* graceReason = name == std::string_view("TFDInCombatOutcomeFollow") ? "incombat_follow" : "incombat_release";
		if (handlers.applyGrace) {
			handlers.applyGrace(context.actor, context.durationSec > 0.0 ? context.durationSec : 20.0, graceReason);
		}
		spdlog::info("[TFD][InCombat] release/follow handled actor={:08X} reason={} duration={:.1f}",
			context.actor->GetFormID(),
			graceReason,
			context.durationSec > 0.0 ? context.durationSec : 20.0);
		return true;
	}

}
