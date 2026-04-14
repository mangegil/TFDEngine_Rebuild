#pragma once

#include <chrono>
#include <unordered_set>
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>
#include <array>

#include "TFDHostilityController.h"

namespace RE
{
	class Actor;
	class TESForm;
	class TESQuest;
	class BGSRefAlias;
}

namespace TFD::Transition
{
	struct RuntimeHandlers;
	struct CaptiveHandlers;
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
		std::function<void(const char*)> clearDialogueOutcome;
		std::function<void()> resetBattleObserveTracking;
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



	struct SupportBridgeHandlers
	{
		std::function<bool()> queuePreCombatClearAll;
		std::function<bool()> queueTruceClearAll;
		std::function<bool()> queueInCombatClearAll;
		std::function<void()> cancelAllPreCombat;
	};

	struct RuntimeHostStateRefs
	{
		std::atomic_bool* inBleedState = nullptr;
		float* minHp = nullptr;
		std::chrono::steady_clock::time_point* bleedStart = nullptr;
		int* bleedLastSeconds = nullptr;
		bool* bleedPaused = nullptr;
		std::chrono::steady_clock::time_point* bleedPauseStarted = nullptr;
		std::chrono::steady_clock::time_point* bleedLastCalmPulse = nullptr;
		std::chrono::steady_clock::time_point* bleedLastCrowdAssign = nullptr;
		std::vector<std::uint32_t>* bleedCrowdAssigned = nullptr;
		std::unordered_set<std::uint32_t>* bleedRejectedSpeakerIds = nullptr;
		std::uint32_t* bleedSpeakerId = nullptr;
		std::chrono::steady_clock::time_point* bleedSpeakerKickLast = nullptr;
		int* bleedSpeakerKickCount = nullptr;
		int* bleedDialogueRetryCount = nullptr;
		std::function<bool()> isEscapeBreakBleedPending;
		std::function<void(bool)> setEscapeBreakBleedPending;
		bool* bleedPendingCaptiveOutcome = nullptr;
		bool* bleedPendingNonCaptiveOutcome = nullptr;
		bool* bleedBattleObservePending = nullptr;
		std::chrono::steady_clock::time_point* bleedBattleObservePendingUntil = nullptr;
		std::chrono::steady_clock::time_point* bleedBattleObservePendingLastRedirect = nullptr;
		int* bleedBattleObservePendingEmptyEnemyTicks = nullptr;
		bool* bleedBattleObserveActive = nullptr;
		std::chrono::steady_clock::time_point* bleedBattleObserveSince = nullptr;
		std::chrono::steady_clock::time_point* bleedBattleObserveLastRedirect = nullptr;
		int* bleedBattleObserveActiveEmptyEnemyTicks = nullptr;
	};

	struct RuntimeHostHandlers
	{
		std::function<void(const char*)> clearTerminalCommit;
		std::function<void(RE::Actor*, const char*)> clearBridgeAliasesForActor;
		std::function<void()> clearNoMarkerFallbackState;
		std::function<void(const char*)> releaseNoSpeakerTameSession;
		std::function<void(const char*)> clearBleedSupportBridgeAliases;
		std::function<void()> resetBattleObserveTracking;
		std::function<void()> releaseTruceSession;
		std::function<void(const char*)> resetGreetRuntime;
		std::function<void(const char*)> clearCaptorAliases;
		std::function<void(const char*)> clearDialogueOutcome;
		std::function<void(bool)> setPlayerBleedImmune;
		std::function<void(RE::Actor*, float)> clampHealth;
		std::function<std::vector<RE::Actor*>(float)> collectBleedStandingFollowers;
		std::function<float(RE::Actor*, const std::vector<RE::Actor*>&, float)> computeBleedBattleEnemyScanRadius;
		std::function<RE::Actor*()> resolveAggressor;
		std::function<RE::Actor*(float, double)> resolveLastEnemyTargetingPlayer;
		std::function<RE::Actor*(float)> findBestAggressor;
		std::function<bool(RE::Actor*)> isObserverAlly;
		std::function<std::vector<RE::Actor*>(RE::Actor*, float, RE::Actor*, const std::vector<RE::Actor*>&)> collectCurrentObservedEnemies;
		std::function<void(RE::Actor*, const std::vector<RE::Actor*>&, const std::vector<RE::Actor*>&, RE::Actor*)> updateObserverRoster;
		std::function<std::vector<RE::Actor*>()> collectStandingFollowersFromSnapshot;
		std::function<std::vector<RE::Actor*>()> collectStandingEnemiesFromSnapshot;
		std::function<bool()> hadValidObservedEnemy;
		std::function<void()> enterObservedBattleWin;
		std::function<void(const char*)> enterObservedLeftForDead;
		std::function<RE::Actor*(float, float, RE::Actor*)> findBestSpeaker;
		std::function<bool(RE::Actor*, RE::Actor*, float, float*)> isReasonableSpeaker;
		std::function<std::vector<RE::Actor*>(float, RE::Actor*, bool)> collectBleedoutCrowd;
		std::function<bool(RE::Actor*)> isCaptiveSupportedAggressor;
		std::function<bool(RE::Actor*)> applyAllowedFactionFromAggressor;
		std::function<bool()> resolveCaptiveMarkerForOutcome;
		std::function<bool(RE::Actor*, RE::Actor*, bool, float*)> canUseCaptiveFallbackHeuristic;
		std::function<bool(const std::vector<RE::Actor*>&, const char*)> tryEnsureNoSpeakerTameSession;
		std::function<void(RE::Actor*)> setLastAggressor;
		std::function<void(bool)> setPrevDialogueOpen;
		std::function<bool(RE::Actor*)> isBleedCrowdSupportedAggressor;
		std::function<bool(RE::Actor*, RE::Actor*)> isBleedSpaceCompatible;
		std::function<void(const char*)> debugNotification;
		std::function<void(RE::Actor*, float, const char*)> clearEnemyTargetsToPlayerForDefeat;
		std::function<RE::Actor*(float)> resolveEscapeBreakPreferredAggressor;
		std::function<bool(RE::Actor*, RE::Actor*, const char*)> startTruceSessionForSpeaker;
		std::function<bool(RE::Actor*, RE::Actor*, float*)> canUseAggressorForBleedoutGreet;
		std::function<void(RE::Actor*, RE::Actor*, const char*, bool)> applyDialogueOverdrive;
	};

	void StartRuntimeWindow(RuntimeHostStateRefs state, RE::Actor* player, RE::Actor* aggressor, const RuntimeHostHandlers& handlers);
	bool StartRuntimeBattleObservePending(RuntimeHostStateRefs state, RE::Actor* player, const RuntimeHostHandlers& handlers);
	void TickRuntimeBattleObservePending(RuntimeHostStateRefs state, RE::Actor* player, const RuntimeHostHandlers& handlers);
	void TickRuntimeBattleObserve(RuntimeHostStateRefs state, RE::Actor* player, const RuntimeHostHandlers& handlers);
	bool HandleRuntimePendingEscapeBreak(RuntimeHostStateRefs state, RE::Actor* player, const RuntimeHostHandlers& handlers);
	void MaintainRuntimeSpeakerKick(RuntimeHostStateRefs state, RE::Actor* player, const RuntimeHostHandlers& handlers);

	std::atomic_bool& InBleedStateRef();
	float& MinHpRef();
	std::chrono::steady_clock::time_point& BleedStartRef();
	int& BleedLastSecondsRef();
	bool& BleedPausedRef();
	std::chrono::steady_clock::time_point& BleedPauseStartedRef();
	std::chrono::steady_clock::time_point& BleedLastCalmPulseRef();
	std::chrono::steady_clock::time_point& BleedLastCrowdAssignRef();
	std::vector<std::uint32_t>& BleedCrowdAssignedRef();
	std::chrono::steady_clock::time_point& BleedNoSpeakerTameLastAttemptRef();
	std::uint32_t& BleedSpeakerIDRef();
	std::chrono::steady_clock::time_point& BleedSpeakerKickLastRef();
	int& BleedSpeakerKickCountRef();
	int& BleedDialogueRetryCountRef();
	std::unordered_set<std::uint32_t>& BleedRejectedSpeakerIdsRef();
	bool& BleedPendingCaptiveOutcomeRef();
	bool& BleedPendingNonCaptiveOutcomeRef();
	bool& BleedBattleObservePendingRef();
	std::chrono::steady_clock::time_point& BleedBattleObservePendingUntilRef();
	std::chrono::steady_clock::time_point& BleedBattleObservePendingLastRedirectRef();
	int& BleedBattleObservePendingEmptyEnemyTicksRef();
	bool& BleedBattleObserveActiveRef();
	std::chrono::steady_clock::time_point& BleedBattleObserveSinceRef();
	std::chrono::steady_clock::time_point& BleedBattleObserveLastRedirectRef();
	int& BleedBattleObserveActiveEmptyEnemyTicksRef();

	void ClearBridgeAliases(RE::TESForm* sender, const char* reason);
	void ClearSupportBridgeAliases(const char* reason, const SupportBridgeHandlers& handlers);
	bool StartTruceSessionForSpeaker(RE::Actor* player, RE::Actor* speaker, const char* reason, RuntimeHostStateRefs state, const RuntimeHostHandlers& handlers);
	void ApplyDialogueOverdrive(RE::Actor* player, RE::Actor* speaker, const char* reason, bool restartDialogue, RuntimeHostStateRefs state, const RuntimeHostHandlers& handlers);
	bool PromoteNextSpeakerFromTruceQueue(RE::TESQuest* truceQuest, const std::array<RE::BGSRefAlias*, 10>& truceAliases, RE::Actor* player, const char* reason, bool rejectCurrent, RuntimeHostStateRefs state, const RuntimeHostHandlers& handlers);
	void MaintainPrimaryCaptorBinding(bool dialogueOpen, RuntimeHostStateRefs state);
	void ReleaseTruceSession(TFD::HostilityController::ReleaseReason reason, std::uint32_t* bleedSpeakerId = nullptr);
	void ReleaseNoSpeakerTameSession(const char* reason);
	bool TryEnsureNoSpeakerTameSession(const std::vector<RE::Actor*>& actors, RE::Actor* player, const char* reason, const RuntimeHostHandlers& handlers);
	std::uint32_t GetTruceSessionID();
	std::uint32_t GetNoSpeakerTameSessionID();
	std::uint32_t GetNoSpeakerTamePrimaryFormID();
	std::chrono::steady_clock::time_point GetNoSpeakerTameLastAttempt();
	void SetNoSpeakerTameLastAttempt(std::chrono::steady_clock::time_point when);
	std::uint32_t GetBleedSpeakerID();
	RE::Actor* GetBleedSpeakerActor();
	void ResetBleedSpeakerKick();
	int GetBleedDialogueRetryCount();
	void SetBleedDialogueRetryCount(int count);
	std::vector<std::uint32_t> GetBleedCrowdAssignedIDs();
	bool HasBleedCrowdAssignedID(std::uint32_t actorID);
	void ClearBleedCrowdAssigned();
	void ClearBleedRejectedSpeakerIds();
	bool HasBleedRejectedSpeakerID(std::uint32_t actorID);
	void AddBleedRejectedSpeakerID(std::uint32_t actorID);
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
		std::function<void()> clearFactionState;
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
		std::function<void()> clearFactionState;
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

// Consolidated from former TFDBleedoutBuilders / TFDBleedoutRuntimeHost staging modules
namespace TFD::Bleedout::Builders
{
    struct PendingSystemEventProvider
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
    };

    struct DialogueCloseProvider
    {
        std::function<void(const char*)> clearDialogueOutcome;
        std::function<void(const char*)> completePayRelease;
    };

    struct TimeoutProvider
    {
        std::function<void()> releaseTruceGeneric;
        std::function<bool()> resolveCaptiveMarker;
        std::function<void()> resetBleedRuntimeState;
        std::function<void()> doBlackoutTeleport;
        std::function<void(int)> setGraceSeconds;
        std::function<void(const char*)> enterNonCaptiveChoice;
    };

    struct BaseCompletionProvider
    {
        std::function<void(const char*)> clearOutcomeWindow;
        std::function<bool(TerminalCommit, const char*)> tryBeginTerminalCommit;
        std::function<void()> clearPendingCinematicFadeIn;
        std::function<void(const char*)> clearBridgeAliases;
        std::function<void()> clearEscapeContext;
        std::function<void()> resetLockpickWatch;
        std::function<void(bool)> setGraceActive;
        std::function<void(bool)> setCaptiveRuntime;
        std::function<void(bool)> setPlayerBleedImmune;
        std::function<void()> recoverPlayerForTransition;
        std::function<float()> getSweepRadius;
        std::function<void(float)> applyCalmBubble;
        std::function<void()> updatePreCombatState;
        std::function<RE::Actor*()> resolveRuntimeSpeaker;
        std::function<void(RE::Actor*, bool, const char*)> beginPleasure;
        std::function<void(bool)> setPrevDialogueOpen;
        std::function<void(bool)> setPrevLockpickOpen;
    };

    struct CaptivePleasureCompletionExtras
    {
        std::function<void()> clearLastAggressor;
        std::function<void(bool)> resetBleedRuntimeState;
        std::function<void(const char*)> syncPlayerCaptiveAlias;
    };

    struct PayReleaseCompletionExtras
    {
        std::function<void()> clearFactionState;
        std::function<void()> clearLastAggressor;
        std::function<void(bool)> resetBleedRuntimeState;
        std::function<void(int)> beginLeftForDeadCooldown;
        std::function<void(int)> setGraceSeconds;
    };

    struct BleedPleasureCompletionExtras
    {
        std::function<void(const char*)> transitionBleedRuntimeToPleasureCommit;
        std::function<void(int)> beginLeftForDeadCooldown;
        std::function<void(int)> setGraceSeconds;
        std::function<void()> refreshPostDefeatGlobals;
    };

    struct NonCaptiveChoiceProvider
    {
        std::function<bool()> hasBlockingCommit;
        std::function<const char*()> getBlockingCommitName;
        std::function<bool(const char*)> beginResolvedNoMarkerFallback;
        std::function<void(const char*)> clearBridgeAliases;
        std::function<void()> clearPendingCinematicFadeIn;
        std::function<void()> clearFactionState;
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

    struct BlackoutProvider
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

    struct TransitionRuntimeProvider
    {
        std::function<RE::Actor*()> getPlayer;
        std::function<RE::Actor*()> resolveAggressor;
        std::function<RE::Actor*(float)> findBestAggressor;
        std::function<bool(RE::Actor*)> isCombatSupportedAggressor;
        std::function<bool(RE::Actor*)> isActiveFollowerActor;
        std::function<bool(RE::Actor*)> isStandingAllyThresholdActor;
        std::function<std::vector<RE::Actor*>()> collectRegisteredTeammates;
        std::function<std::vector<RE::Actor*>(float, RE::Actor*, bool)> collectBleedoutCrowd;
        std::function<std::vector<RE::FormID>()> getBleedCrowdAssigned;
        std::function<bool(float)> tryAbortPleasureDueToHostileIntrusion;
        std::function<void(const char*, bool)> releasePlayerBleedLock;
        std::function<void(int)> setGraceSeconds;
        std::function<void(int)> setRescueStateValue;
        std::function<void()> refreshPostDefeatGlobals;
        std::function<void()> updatePreCombatState;
    };

    struct TransitionCaptiveProvider
    {
        std::function<void()> resetBleedRuntimeState;
        std::function<void(const char*)> clearBridgeAliases;
        std::function<void()> clearLastAggressor;
        std::function<void(const char*)> beginCaptiveFlow;
        std::function<void()> setCaptiveRuntimeCaptive;
        std::function<bool()> isDialogueOpen;
        std::function<void(bool)> setPrevDialogueOpen;
        std::function<void()> captureCurrentLockpickMenuState;
        std::function<void()> resetLockpickWatch;
        std::function<void()> armEscapeContextFromCurrentState;
        std::function<void()> sealCaptiveDoorIfPresent;
        std::function<void(float)> applyCalmBubble;
        std::function<void(const char*, bool)> queuePendingCaptiveConfiscation;
        std::function<void(RE::Actor*, const char*)> syncPlayerCaptiveAlias;
    };

    void InstallPendingSystemEventProvider(PendingSystemEventProvider provider);
    void InstallDialogueCloseProvider(DialogueCloseProvider provider);
    void InstallTimeoutProvider(TimeoutProvider provider);
    void InstallBaseCompletionProvider(BaseCompletionProvider provider);
    void InstallCaptivePleasureCompletionExtras(CaptivePleasureCompletionExtras provider);
    void InstallPayReleaseCompletionExtras(PayReleaseCompletionExtras provider);
    void InstallBleedPleasureCompletionExtras(BleedPleasureCompletionExtras provider);
    void InstallNonCaptiveChoiceProvider(NonCaptiveChoiceProvider provider);
    void InstallBlackoutProvider(BlackoutProvider provider);
    void InstallTransitionRuntimeProvider(TransitionRuntimeProvider provider);
    void InstallTransitionCaptiveProvider(TransitionCaptiveProvider provider);
    void Reset();

    PendingSystemEventHandlers BuildPendingSystemEventHandlers();
    DialogueCloseHandlers BuildDialogueCloseHandlers();
    TimeoutHandlers BuildTimeoutHandlers();
    CompletionHandlers BuildCaptivePleasureCompletionHandlers();
    CompletionHandlers BuildPayReleaseCompletionHandlers();
    CompletionHandlers BuildBleedPleasureCompletionHandlers();
    NonCaptiveChoiceHandlers BuildNonCaptiveChoiceHandlers();
    BlackoutHandlers BuildBlackoutHandlers();
    TFD::Transition::RuntimeHandlers BuildTransitionRuntimeHandlers();
    TFD::Transition::CaptiveHandlers BuildTransitionCaptiveHandlers();
}


namespace TFD::Bleedout::RuntimeHost
{
	struct Provider
	{
		std::atomic_bool* inBleedState = nullptr;
		float* minHp = nullptr;
		std::chrono::steady_clock::time_point* bleedStart = nullptr;
		int* bleedLastSeconds = nullptr;
		bool* bleedPaused = nullptr;
		std::chrono::steady_clock::time_point* bleedPauseStarted = nullptr;
		std::chrono::steady_clock::time_point* bleedLastCalmPulse = nullptr;
		std::function<bool()> isEscapeBreakBleedPending;
		std::function<void(bool)> setEscapeBreakBleedPending;
		bool* bleedPendingCaptiveOutcome = nullptr;
		bool* bleedPendingNonCaptiveOutcome = nullptr;
		bool* bleedBattleObservePending = nullptr;
		std::chrono::steady_clock::time_point* bleedBattleObservePendingUntil = nullptr;
		std::chrono::steady_clock::time_point* bleedBattleObservePendingLastRedirect = nullptr;
		int* bleedBattleObservePendingEmptyEnemyTicks = nullptr;
		bool* bleedBattleObserveActive = nullptr;
		std::chrono::steady_clock::time_point* bleedBattleObserveSince = nullptr;
		std::chrono::steady_clock::time_point* bleedBattleObserveLastRedirect = nullptr;
		int* bleedBattleObserveActiveEmptyEnemyTicks = nullptr;
		std::function<TFD::Bleedout::RuntimeHostHandlers()> buildRuntimeHostHandlers;
		std::function<TFD::Bleedout::DialogueHotkeyHandlers()> buildDialogueHotkeyHandlers;
		std::function<RE::Actor*()> getPlayer;
		std::function<float()> getDialogueHotkeyRadius;
		std::function<float()> getDialogueHotkeyMaxSpeakerDist;
	};

	void InstallProvider(Provider provider);
	void Reset();

	TFD::Bleedout::RuntimeHostStateRefs BuildStateRefs();
	TFD::Bleedout::RuntimeHostHandlers BuildHandlers();

	void StartWindow(RE::Actor* player, RE::Actor* aggressor);
	bool BeginDialogueHotkey();
	bool StartBattleObservePending(RE::Actor* player);
	void TickBattleObservePending();
	void TickBattleObserve();
	bool HandlePendingEscapeBreak();
	void MaintainSpeakerKick();
}



namespace TFD::Bleedout::DefeatGlue
{
	struct Provider
	{
		std::atomic_bool* graceActive = nullptr;
		std::chrono::steady_clock::time_point* graceUntil = nullptr;
		bool* prevDialogueOpen = nullptr;
		std::function<void()> clearPendingFadeIn;
		std::function<void()> clearEscapeContext;
		std::function<void()> resetLockpickWatch;
		std::function<void(bool)> setPrevLockpickOpen;
		std::function<void()> clearCaptiveRuntime;
		std::function<RE::Actor*()> getPlayer;
		std::function<void(const char*, bool)> releasePlayerBleedLock;
		std::function<std::uint32_t()> currentBleedSpeakerId;
		std::function<RE::Actor*()> currentBleedSpeaker;
		std::function<TFD::Bleedout::SpeakerLogicHandlers()> buildSpeakerHandlers;
		std::function<bool()> isDialogueOpen;
		std::function<RE::Actor*()> resolveAggressor;
		std::function<RE::Actor*(float)> findBestAggressor;
		std::function<void(const char*)> releaseNoSpeakerTameSession;
		std::function<void()> releaseTruceSession;
		std::function<bool(RE::Actor*, RE::Actor*, const char*)> startTruceSessionForSpeaker;
		std::function<void()> resetSpeakerKick;
		std::function<void(const char*)> resetGreetRuntime;
		std::function<void(RE::Actor*, const char*)> beginGreet;
		std::function<void(const char*)> clearBleedSupportBridgeAliases;
		std::function<void()> resetBattleObserveTracking;
		std::function<bool()> isObservedCombatCommitInProgress;
		std::function<void(RE::Actor*)> noteEnemyTargetingPlayer;
		std::function<void(bool)> setPlayerBleedImmune;
		std::function<void(RE::Actor*, float)> clampHealth;
		std::function<std::vector<RE::Actor*>(float)> collectBleedStandingFollowers;
		std::function<float(RE::Actor*, const std::vector<RE::Actor*>&, float)> computeBleedBattleEnemyScanRadius;
		std::function<RE::Actor*(float, double)> resolveLastEnemyTargetingPlayer;
		std::function<bool(RE::Actor*)> isObserverAlly;
		std::function<std::vector<RE::Actor*>(RE::Actor*, float, RE::Actor*, const std::vector<RE::Actor*>&)> collectCurrentObservedEnemies;
		std::function<void(RE::Actor*, const std::vector<RE::Actor*>&, const std::vector<RE::Actor*>&, RE::Actor*)> updateObserverRoster;
		std::function<std::vector<RE::Actor*>()> collectStandingFollowersFromSnapshot;
		std::function<std::vector<RE::Actor*>()> collectStandingEnemiesFromSnapshot;
		std::function<bool()> hadValidObservedEnemy;
		std::function<void()> enterObservedBattleWin;
		std::function<void(const char*)> enterObservedLeftForDead;
		std::function<RE::Actor*(float, float, RE::Actor*)> findBestSpeaker;
		std::function<bool(RE::Actor*, RE::Actor*, float, float*)> isReasonableSpeaker;
		std::function<std::vector<RE::Actor*>(float, RE::Actor*, bool)> collectBleedoutCrowd;
		std::function<bool(RE::Actor*)> isCaptiveSupportedAggressor;
		std::function<bool(RE::Actor*)> applyAllowedFactionFromAggressor;
		std::function<bool()> resolveCaptiveMarkerForOutcome;
		std::function<bool(RE::Actor*, RE::Actor*, bool, float*)> canUseCaptiveFallbackHeuristic;
		std::function<bool(const std::vector<RE::Actor*>&, const char*)> tryEnsureNoSpeakerTameSession;
		std::function<void(RE::Actor*)> setLastAggressor;
		std::function<bool(RE::Actor*)> isBleedCrowdSupportedAggressor;
		std::function<bool(RE::Actor*, RE::Actor*)> isBleedSpaceCompatible;
		std::function<void(const char*)> debugNotification;
		std::function<void(RE::Actor*, float, const char*)> clearEnemyTargetsToPlayerForDefeat;
		std::function<RE::Actor*(float)> resolveEscapeBreakPreferredAggressor;
		std::function<bool(RE::Actor*, RE::Actor*, float*)> canUseAggressorForBleedoutGreet;
		std::function<void(RE::Actor*, RE::Actor*, const char*, bool)> applyDialogueOverdrive;
		std::function<bool(RE::Actor*)> isStandingAllyThresholdActor;
		std::function<bool(RE::Actor*)> isStandingEnemyThresholdActor;
	};

	void InstallProvider(Provider provider);
	void Reset();

	void ClearCaptiveOrchestrationResidue(bool clearPendingFadeIn);
	RE::Actor* ResolveBleedRuntimeSpeaker();
	void BeginBleedPleasureRuntime(RE::Actor* speaker, bool captive, const char* reason);
	void ExitSystemEventRuntime();
	TFD::Bleedout::DialogueHotkeyHandlers BuildDialogueHotkeyHandlers();
	bool BeginDialogueHotkey();
	bool IsBleedoutActive();
	bool IsPlayerBleedHoldTargetBlocked();
	bool IsObservedCombatCommitInProgress();
	void NoteEnemyTargetingPlayer(RE::Actor* actor);
	void PreparePlayerForBleedoutPleasureScene(const char* reason);
	void PreparePlayerForCaptivePleasureScene(const char* reason);
	TFD::Bleedout::RuntimeHostHandlers BuildLocalRuntimeHostHandlers();
	bool HandlePendingEscapeBreak();
	void MaintainSpeakerKick();
	bool IsObserverAlly(RE::Actor* actor);
	float ComputeObservedEnemyScanRadius(RE::Actor* player, const std::vector<RE::Actor*>& allies, float baseRadius);
	std::vector<RE::Actor*> CollectCurrentObservedEnemies(RE::Actor* player, float radius, RE::Actor* preferredEnemy, const std::vector<RE::Actor*>& allies);
	void UpdateObservedBattleRoster(RE::Actor* player, const std::vector<RE::Actor*>& followers, const std::vector<RE::Actor*>& enemies, RE::Actor* preferredEnemy);
	std::vector<RE::Actor*> CollectStandingFollowersFromSnapshot();
	std::vector<RE::Actor*> CollectStandingEnemiesFromSnapshot();
	RE::Actor* ResolveBleedRedirectTarget(RE::Actor* actor);
	RE::Actor* ResolveBleedFollowerAggroTarget(RE::Actor* actor);
	void ResetObservedRuntimeTracking();
	bool HadValidObservedEnemy();
	void HandleObservedBattleWin(const char* reason = nullptr);
	void HandleObservedLeftForDead(const char* reason = nullptr);
}

namespace TFD::Bleedout
{
	struct DefeatLifecycleProviders
	{
		Builders::PendingSystemEventProvider pendingSystemEvent;
		Builders::DialogueCloseProvider dialogueClose;
		Builders::TimeoutProvider timeout;
		Builders::BaseCompletionProvider baseCompletion;
		Builders::CaptivePleasureCompletionExtras captivePleasureCompletion;
		Builders::PayReleaseCompletionExtras payReleaseCompletion;
		Builders::BleedPleasureCompletionExtras bleedPleasureCompletion;
		RuntimeHost::Provider runtimeHost;
		DefeatGlue::Provider defeatGlue;
	};

	void InstallDefeatLifecycleProviders(DefeatLifecycleProviders providers);
	void ResetDefeatLifecycleProviders();
}
