#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

namespace RE
{
	class Actor;
	class TESForm;
}

namespace TFD::Bleedout
{
	enum class DialogueOutcome : std::uint8_t
	{
		None = 0,
		PayRelease,
		Pleasure,
		Captive
	};

	enum class TerminalCommit : std::uint8_t
	{
		None = 0,
		PayRelease,
		Pleasure,
		Captive,
		NonCaptiveFallback
	};

	void Install();
	void ResetForLoad();

	struct SpeakerLogicHandlers
	{
		std::function<RE::Actor*()> getPlayer;
		std::function<bool(RE::Actor*)> isStandingEnemyThresholdActor;
		std::function<bool(RE::Actor*)> isCaptiveSupportedAggressor;
		std::function<bool(RE::Actor*)> isBleedCrowdSupportedAggressor;
		std::function<bool(RE::Actor*, RE::Actor*)> isBleedSpaceCompatible;
		std::function<bool(RE::Actor*, RE::Actor*)> hasLineOfSightToPlayer;
		std::function<bool(RE::Actor*, RE::Actor*, float)> isActorCloseAndFront;
		std::function<RE::Actor*(RE::Actor*)> resolveCurrentCombatTarget;
		std::function<bool(RE::Actor*)> isActiveFollowerActor;
		std::function<RE::Actor*()> resolveLastAggressor;
		std::function<bool(RE::Actor*)> isPreservedAssigned;
	};

	struct DialogueHotkeyHandlers
	{
		SpeakerLogicHandlers speaker;
		std::function<bool()> isBleedoutActive;
		std::function<bool()> isCaptiveEscapePhase;
		std::function<bool()> isDialogueOpen;
		std::function<RE::Actor*()> resolveSpeakerFromRuntime;
		std::function<RE::Actor*()> resolveAggressor;
		std::function<RE::Actor*(float)> findBestAggressor;
		std::function<void(const char*)> releaseNoSpeakerTameSession;
		std::function<void()> releaseTruceSession;
		std::function<bool(RE::Actor*, RE::Actor*, const char*)> startTruceSessionForSpeaker;
		std::function<void()> resetSpeakerKick;
		std::function<void(const char*)> resetGreetRuntime;
		std::function<void(RE::Actor*, const char*)> beginGreet;
	};

	std::vector<RE::Actor*> CollectCrowd(float radius, RE::Actor* preferred, bool preserveAssigned, const SpeakerLogicHandlers& handlers);
	bool IsReasonableSpeaker(RE::Actor* actor, float maxDist, float* outDistance, const SpeakerLogicHandlers& handlers);
	RE::Actor* ChooseStrictSpeaker(float radius, float maxDist, RE::Actor* preferred, const SpeakerLogicHandlers& handlers);
	RE::Actor* FindBestSpeaker(float radius, float maxDist, RE::Actor* preferred, const SpeakerLogicHandlers& handlers);
	bool CanUseSpeakerForGreet(RE::Actor* aggressor, float maxDist, float& outDistance, const SpeakerLogicHandlers& handlers);
	bool BeginDialogueHotkey(float radius, float maxSpeakerDist, const DialogueHotkeyHandlers& handlers);


	struct RuntimeResetHandlers
	{
		std::function<void(const char*)> releasePlayerBleedLock;
		std::function<void()> releaseBleedTruceSession;
		std::function<void(const char*)> releaseNoSpeakerTameSession;
		std::function<void(const char*)> clearBridgeAliases;
		std::function<void(bool, const char*)> setBleedActive;
		std::function<void(const char*)> resetGreetRuntime;
		std::function<void(const char*)> resetSystemEventState;
		std::function<void(const char*)> clearCaptorAliases;
		std::function<void()> resetDialogueRuntimeState;
		std::function<void()> resetBattleObserveState;
		std::function<void()> clearEscapeBreakState;
		std::function<void()> clearLastEnemyTargetingPlayer;
		std::function<void(const char*)> clearOutcomeWindow;
		std::function<void(const char*)> resetPleasureRuntime;
	};

	struct RuntimePleasureCommitHandlers
	{
		std::function<void(const char*)> releaseNoSpeakerTameSession;
		std::function<void(const char*)> clearBridgeAliases;
		std::function<void(bool, const char*)> setBleedActive;
		std::function<void(const char*)> resetGreetRuntime;
		std::function<void()> resetDialogueRuntimeState;
		std::function<void()> resetBattleObserveState;
		std::function<void()> clearEscapeBreakState;
		std::function<void()> clearLastEnemyTargetingPlayer;
		std::function<void(const char*)> clearOutcomeWindow;
	};

	void ResetRuntimeState(bool preserveCaptive, const char* reason, const RuntimeResetHandlers& handlers);
	void TransitionRuntimeToPleasureCommit(const char* reason, std::uint32_t speakerId, bool preserveSession, std::uint32_t captorId, const RuntimePleasureCommitHandlers& handlers);

	void ClearBridgeAliases(RE::TESForm* sender, const char* reason);
	void AssignBridgeActor(RE::Actor* actor);
	void PrimeBridgeActor(RE::Actor* actor, const char* reason);
	void ClearCaptorAliases(const char* reason);
	bool IsCaptorAliasPrimary(RE::Actor* actor);
	bool BindCaptorAliases(RE::Actor* actor, const char* reason);
	std::uint32_t GetActiveCaptorFormID();
	bool WasCaptorRecentlyBound(std::chrono::steady_clock::time_point now, std::chrono::milliseconds window);

	bool BeginWindow(RE::Actor* speaker, const char* reason = nullptr);
	bool ResolvePay(RE::Actor* actor, const char* reason = nullptr);
	bool ResolvePleasure(RE::Actor* actor, const char* reason = nullptr);
	bool ResolveCaptive(RE::Actor* actor, const char* reason = nullptr);
	bool CommitDialogueOutcome(DialogueOutcome outcome, RE::Actor* actor, const char* reason = nullptr);
	bool BeginAfterPleasure(RE::Actor* actor, const char* reason = nullptr);
	bool HandleAfterPleasureEnter(RE::Actor* actor, const char* reason = nullptr);
	bool CompleteAfterPleasure(const char* reason = nullptr);

	const char* GetDialogueOutcomeName(DialogueOutcome outcome);

	const char* GetTerminalCommitName(TerminalCommit kind);
	TerminalCommit GetTerminalCommit();
	bool HasTerminalCommit();
	bool TryBeginTerminalCommit(TerminalCommit kind, const char* reason = nullptr);
	void ClearTerminalCommit(const char* reason = nullptr);
	void SetDialogueOutcome(DialogueOutcome outcome, const char* reason = nullptr);
	void ClearDialogueOutcome(const char* reason = nullptr);
	DialogueOutcome GetDialogueOutcome();

	void ArmSystemEventOutcomeWindow(const char* reason = nullptr, double seconds = 2.5);
	void ClearSystemEventOutcomeWindow(const char* reason = nullptr);
	bool IsSystemEventOutcomeWindowActive();
	bool IsAwaitingSystemEventOutcome();
	bool IsSystemEventPendingForFallback(const char** outReason = nullptr);
	bool ShouldLogDeferredSystemEvent(std::chrono::steady_clock::time_point now);
	void NoteDeferredSystemEventLog(std::chrono::steady_clock::time_point now);
	void ResetSystemEventState(const char* reason = nullptr);
	bool ShouldDropSystemEventBecauseFallback(const char* eventName, bool terminalCommitActive, const char* terminalCommitName);

	struct PendingSystemEventContext
	{
		const char* runtimePhaseName = nullptr;
		bool runtimeActive = false;
		bool runtimeBlocking = false;
	};

	struct PendingSystemEventHandlers
	{
		std::function<void(const char*)> clearDialogueOutcome;
		std::function<void(const char*)> clearOutcomeWindow;
		std::function<void(const char*)> completePayRelease;
		std::function<void()> releaseFlowHandoff;
		std::function<bool()> resolveCaptiveMarker;
		std::function<void()> doBlackoutTeleport;
		std::function<void(int)> setGraceSeconds;
		std::function<void(const char*)> releaseNoSpeakerTameSession;
		std::function<void()> exitBleedState;
		std::function<void(const char*)> enterNonCaptiveChoice;
		std::function<int()> getRetryCount;
		std::function<void(int)> setRetryCount;
		std::function<bool(const char*)> promoteNextSpeaker;
	};


	struct OutcomeEventContext
	{
		const char* eventName = nullptr;
		const char* rawEventName = nullptr;
		RE::Actor* actor = nullptr;
		std::uint32_t actorFormID = 0;
		bool inBleedState = false;
		bool preserveCaptive = false;
	};

	struct OutcomeEventHandlers
	{
		std::function<bool(const char*)> shouldDropBecauseFallback;
		std::function<void(const char*, double)> armOutcomeWindow;
		std::function<void(DialogueOutcome, const char*)> setDialogueOutcome;
		std::function<void(const char*)> clearDialogueOutcome;
		std::function<void(const char*)> clearOutcomeWindow;
		std::function<bool(std::uint32_t, const char*)> beginCaptivePleasureFlow;
		std::function<void(const char*)> prepareCaptivePleasureScene;
		std::function<void(const char*)> completeCaptivePleasureHandoff;
		std::function<void(const char*)> prepareBleedoutPleasureScene;
		std::function<void(const char*)> completeBleedPleasureHandoff;
	};

	struct CompletionHandlers
	{
		std::function<void(const char*)> clearOutcomeWindow;
		std::function<bool(TerminalCommit, const char*)> tryBeginTerminalCommit;
		std::function<void()> clearPendingCinematicFadeIn;
		std::function<void(const char*)> clearBridgeAliases;
		std::function<void()> clearFactionMask;
		std::function<void()> clearEscapeContext;
		std::function<void()> resetLockpickWatch;
		std::function<void(bool)> setGraceActive;
		std::function<void()> clearLastAggressor;
		std::function<void(bool)> resetBleedRuntimeState;
		std::function<void(const char*)> transitionBleedRuntimeToPleasureCommit;
		std::function<void(bool)> setCaptiveRuntime;
		std::function<void(const char*)> syncPlayerCaptiveAlias;
		std::function<void(bool)> setPlayerBleedImmune;
		std::function<void()> recoverPlayerForTransition;
		std::function<float()> getSweepRadius;
		std::function<void(float)> applyCalmBubble;
		std::function<void(int)> beginLeftForDeadCooldown;
		std::function<void(int)> setGraceSeconds;
		std::function<void()> refreshPostDefeatGlobals;
		std::function<void()> updatePreCombatState;
		std::function<RE::Actor*()> resolveRuntimeSpeaker;
		std::function<void(RE::Actor*, bool, const char*)> beginPleasure;
		std::function<void(bool)> setPrevDialogueOpen;
		std::function<void(bool)> setPrevLockpickOpen;
	};

	bool HandleOutcomePayEvent(const OutcomeEventContext& context, const OutcomeEventHandlers& handlers);
	bool HandleOutcomePleasureEvent(const OutcomeEventContext& context, const OutcomeEventHandlers& handlers);
	bool HandleOutcomeCaptiveEvent(const OutcomeEventContext& context, const OutcomeEventHandlers& handlers);
	bool HandleOutcomeResetEvent(const OutcomeEventContext& context, const OutcomeEventHandlers& handlers);

	bool CompleteCaptivePleasureHandoff(const char* reason, const CompletionHandlers& handlers);
	bool CompletePayRelease(const char* reason, const CompletionHandlers& handlers);
	bool CompleteBleedPleasureHandoff(const char* reason, const CompletionHandlers& handlers);

	bool TryResolvePostDialogueSystemEvent(const char* reason, const PendingSystemEventHandlers& handlers);
	bool TryHandlePendingSystemEventFallback(const PendingSystemEventContext& context, const char* reason, const PendingSystemEventHandlers& handlers);

	struct DialogueCloseHandlers
	{
		std::function<void(const char*)> clearDialogueOutcome;
		std::function<void(const char*)> completePayRelease;
	};

	bool TryHandlePayReleaseDialogueClosed(bool seenDialogue, bool prevDialogueOpen, const DialogueCloseHandlers& handlers);

	struct TimeoutContext
	{
		bool hasTerminalCommit = false;
		const char* terminalCommitName = nullptr;
		const char* pendingSystemReason = nullptr;
		bool pendingCaptive = false;
	};

	struct TimeoutHandlers
	{
		std::function<void()> releaseTruceGeneric;
		std::function<bool()> resolveCaptiveMarker;
		std::function<void()> resetBleedRuntimeState;
		std::function<void()> doBlackoutTeleport;
		std::function<void(int)> setGraceSeconds;
		std::function<void(const char*)> enterNonCaptiveChoice;
	};

	bool HandleBleedTimeout(const TimeoutContext& context, const char* reason, const TimeoutHandlers& handlers);


	struct NonCaptiveChoiceHandlers
	{
		std::function<bool()> hasBlockingCommit;
		std::function<const char*()> getBlockingCommitName;
		std::function<bool(const char*)> beginResolvedNoMarkerFallback;
		std::function<void(const char*)> clearBridgeAliases;
		std::function<void()> clearPendingCinematicFadeIn;
		std::function<void()> clearFactionMask;
		std::function<void()> clearEscapeContext;
		std::function<void()> resetLockpickWatch;
		std::function<void(bool)> setGraceActive;
		std::function<void()> clearLastAggressor;
		std::function<void()> resetBleedRuntimeState;
		std::function<void(bool)> setPrevDialogueOpen;
		std::function<void(bool)> setPrevLockpickOpen;
		std::function<void(bool)> setCaptiveRuntime;
		std::function<void(bool)> setPlayerBleedImmune;
		std::function<void(const char*)> queueLegacyRequest;
		std::function<void(const char*)> resetFlowRuntime;
	};

	struct BlackoutHandlers
	{
		std::function<bool()> hasBlockingCommit;
		std::function<const char*()> getBlockingCommitName;
		std::function<bool()> resolveCaptiveMarker;
		std::function<void(const char*)> enterNonCaptiveChoice;
		std::function<bool(const char*)> tryBeginCaptiveCommit;
		std::function<void()> resetBleedRuntimeState;
		std::function<void(const char*)> clearBridgeAliases;
		std::function<void()> clearLastAggressor;
		std::function<void()> beginCaptiveFlow;
		std::function<void()> clearPendingCinematicFadeIn;
		std::function<bool()> queueCaptiveFadeTransition;
		std::function<void()> showBlackoutFader;
		std::function<void(const char*)> completeCaptiveTransitionNow;
		std::function<void()> hideBlackoutFader;
	};

	bool EnterNonCaptiveChoice(const char* reason, const NonCaptiveChoiceHandlers& handlers);
	bool DoBlackoutTeleport(const char* reason, const BlackoutHandlers& handlers);

	bool IsActive();
	bool OwnsCurrentFlow();
	std::uint32_t ResolveActorFormID(RE::Actor* actor);
}
