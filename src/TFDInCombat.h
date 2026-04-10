#pragma once

#include <cstdint>
#include <functional>

namespace RE
{
	class Actor;
}

namespace TFD::InCombat
{
	enum class State : std::uint32_t
	{
		Idle = 0,
		Observed,
		TruceDialogue,
		AfterPleasure
	};

	enum class DialogueOutcome : std::uint8_t
	{
		None = 0,
		PayRelease,
		Pleasure,
		Captive
	};

	void Install();
	void ResetForLoad();
	void Shutdown();

	void ObserveCombat(std::uint32_t actorFormID, bool active, const char* reason);
	bool BeginTruce(std::uint32_t actorFormID, bool dialogueRequested, const char* reason);
	void NoteAfterPleasure(std::uint32_t actorFormID, const char* reason);
	void Complete(const char* reason);

	bool IsActive();
	State GetState();
	std::uint32_t GetPrimaryActorFormID();
	const char* GetStateName();

	const char* GetDialogueOutcomeName(DialogueOutcome outcome);
	void SetDialogueOutcome(DialogueOutcome outcome, const char* reason = nullptr);
	void ClearDialogueOutcome(const char* reason = nullptr);
	DialogueOutcome GetDialogueOutcome();

	struct OutcomeEventContext
	{
		const char* eventName = nullptr;
		const char* rawEventName = nullptr;
		RE::Actor* actor = nullptr;
		std::uint32_t actorFormID = 0;
		bool inCombatState = false;
		bool preserveCaptive = false;
	};

	struct OutcomeEventHandlers
	{
		std::function<void(const char*, double)> armOutcomeWindow;
		std::function<void(DialogueOutcome, const char*)> setDialogueOutcome;
		std::function<void(const char*)> clearDialogueOutcome;
		std::function<bool(std::uint32_t, const char*)> beginCaptiveFlow;
		std::function<bool(std::uint32_t, const char*)> beginCaptivePleasureFlow;
		std::function<void(const char*)> prepareCaptivePleasureScene;
		std::function<void(const char*)> completeCaptivePleasureHandoff;
		std::function<void(const char*)> prepareInCombatPleasureScene;
		std::function<void(const char*)> completeInCombatPleasureHandoff;
	};

	bool HandleOutcomePayEvent(const OutcomeEventContext& context, const OutcomeEventHandlers& handlers);
	bool HandleOutcomePleasureEvent(const OutcomeEventContext& context, const OutcomeEventHandlers& handlers);
	bool HandleOutcomeCaptiveEvent(const OutcomeEventContext& context, const OutcomeEventHandlers& handlers);
	bool HandleOutcomeResetEvent(const OutcomeEventContext& context, const OutcomeEventHandlers& handlers);

	struct CompletionHandlers
	{
		std::function<void(const char*)> clearDialogueOutcome;
		std::function<void(const char*)> completeFlow;
		std::function<void(const char*)> cancelGreet;
	};

	bool CompletePayRelease(const char* reason, const CompletionHandlers& handlers);
	bool CompleteDialogueClosedFlow(const char* reason, const CompletionHandlers& handlers);


	struct AfterPleasureHandlers
	{
		std::function<void(std::uint32_t, const char*)> noteAfterPleasure;
		std::function<bool(RE::Actor*, const char*)> beginAfterPleasureGreet;
	};

	bool HandleAfterPleasureEnter(RE::Actor* actor, const char* reason, const AfterPleasureHandlers& handlers);

	struct GraceEventContext
	{
		const char* eventName = nullptr;
		RE::Actor* actor = nullptr;
		double durationSec = 20.0;
	};

	struct GraceEventHandlers
	{
		std::function<void(RE::Actor*, double, const char*)> applyGrace;
	};

	bool HandleReleaseFollowEvent(const GraceEventContext& context, const GraceEventHandlers& handlers);
}
