#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string_view>

namespace RE
{
    class Actor;
    class TESForm;
}


namespace TFD::Bleedout
{
    enum class TerminalCommit : std::uint8_t;
}

namespace TFD::Transition
{
    enum class FallbackBranch : std::uint32_t;
}

namespace TFD::FlowController
{
    enum class RootFlow : std::uint8_t
    {
        None = 0,
        PreCombat,
        InCombat,
        Bleedout,
        Captive,
        Victory,
        Rescue,
        Recovery,
        LeftForDead
    };

    enum class ObservedMainState : std::uint8_t
    {
        Neutral = 0,
        Precombat,
        Incombat,
        Victory,
        Defeat,
        Captive
    };

    enum class DecisionGate : std::uint8_t
    {
        None = 0,
        PlayerBleedout,
        EnemyBleedout,
        Truce
    };

    enum class CaptiveMode : std::uint8_t
    {
        None = 0,
        Kidnapped,
        Surrendered,
        JoinedEnemy
    };

    enum class SubFlow : std::uint8_t
    {
        None = 0,

        PreCombatPayFollowup,
        PreCombatPleasure,
        PreCombatAfterPleasure,
        InCombatPayFollowup,
        InCombatPleasure,
        InCombatAfterPleasure,
        BleedoutPleasure,
        BleedoutAfterPleasure,
        VictoryPleasure,
        VictoryAfterPleasure,

        CaptiveIdle,
        CaptivePleasure,
        CaptiveAfterPleasure,
        WorkForEnemy,
        EscapeAttempt,
        EscapeFailed,
        Recapture,
        JoinedEnemyIdle
    };

    enum class PreCombatOutcome : std::uint8_t
    {
        None = 0,
        Pay,
        Pleasure,
        Fight,
        Captive,
        JoinEnemy,
        RecruitEnemy,
        Release,
        Follow,
        Cancel,
        Failed
    };

    enum class InCombatOutcome : std::uint8_t
    {
        None = 0,
        Pay,
        Pleasure,
        Fight,
        Captive,
        JoinEnemy,
        RecruitEnemy,
        Release,
        Follow,
        DoNothing,
        Cancel,
        Failed
    };

    enum class BleedoutOutcome : std::uint8_t
    {
        None = 0,
        Pay,
        Pleasure,
        Captive,
        Cancel,
        Failed
    };

    enum class VictoryOutcome : std::uint8_t
    {
        None = 0,
        RecruitEnemy,
        KillEnemy,
        Pleasure,
        Cancel,
        Failed
    };

    enum class CaptiveOutcome : std::uint8_t
    {
        None = 0,
        Pleasure,
        WorkForEnemy,
        EscapeStarted,
        EscapeSucceeded,
        EscapeFailed,
        Recaptured,
        Release,
        Cancel,
        Failed
    };

    struct Snapshot
    {
        RootFlow root{ RootFlow::None };
        RootFlow contextRoot{ RootFlow::None };
        DecisionGate gate{ DecisionGate::None };
        CaptiveMode captiveMode{ CaptiveMode::None };
        SubFlow sub{ SubFlow::None };
        std::uint32_t token{ 0 };
        std::uint32_t primaryActorFormID{ 0 };
        bool terminalResolved{ false };
        bool locked{ false };
    };

    enum class ObservedReasonFlag : std::uint32_t
    {
        None = 0,
        CaptiveRuntime = 1u << 0,
        CaptiveGlobal = 1u << 1,
        CaptiveRoot = 1u << 2,
        PlayerBleedRuntime = 1u << 3,
        DefeatGlobal = 1u << 4,
        BleedoutRoot = 1u << 5,
        ActiveHostile = 1u << 6,
        DefeatedLivingEnemy = 1u << 7,
        MutualLosHostile = 1u << 8,
        RootInCombat = 1u << 9,
        RootVictory = 1u << 10,
        RootPreCombat = 1u << 11
    };

    struct ObservedMainStateSnapshot
    {
        ObservedMainState observed{ ObservedMainState::Neutral };
        ObservedMainState rootProjected{ ObservedMainState::Neutral };
        ObservedMainState globalsProjected{ ObservedMainState::Neutral };

        Snapshot flow{};

        std::uint32_t reasonFlags{ 0 };
        std::uint32_t reasonActorFormID{ 0 };
        std::uint32_t reasonTargetFormID{ 0 };

        std::uint32_t scannedActorCount{ 0 };
        std::uint32_t activeHostileCount{ 0 };
        std::uint32_t mutualLosHostileCount{ 0 };
        std::uint32_t defeatedLivingEnemyCount{ 0 };
        std::uint32_t playerSideStandingCount{ 0 };

        int preCombatGlobal{ 0 };
        int inCombatGlobal{ 0 };
        int victoryGlobal{ 0 };
        int defeatGlobal{ 0 };
        int captiveGlobal{ 0 };
        int pleasureGlobal{ 0 };
        int dialogueGlobal{ 0 };

        bool playerInCombat{ false };
        bool playerBleedRuntimeActive{ false };
        bool captiveRuntimeActive{ false };
        bool combatActiveFlag{ false };
    };


    enum class DialogueContextKind : std::uint8_t
    {
        None = 0,
        PreCombat,
        InCombat,
        Bleedout,
        Captive,
        AfterPleasure,
        JoinedEnemy
    };

    enum class PassiveHoldKind : std::uint8_t
    {
        None = 0,
        Dialogue,
        Grace,
        Pleasure,
        Captive,
        JoinedEnemy
    };

    struct PassiveRuntimeProviders
    {
        std::function<bool()> isBleedoutActive;
        std::function<bool()> hasReleaseFollowGrace;
        std::function<RE::Actor* ()> resolveFallbackPassivePrimaryActor;
        std::function<bool(RE::Actor*)> isActorCoveredByPassiveContext;
        std::function<void(RE::Actor*, const char*)> cancelReleaseFollowGraceForActor;
        std::function<void(const char*)> clearAllReleaseFollowGrace;
        std::function<void()> releaseBleedPlayerAggressionTruce;
        std::function<void(const char*)> clearAllBleedLocks;
        std::function<void(RE::Actor*, const char*)> clearBleedBridgeAliases;
        std::function<void()> clearPendingDialogueTarget;
    };

    struct ContinuousRuntimeProviders
    {
        std::function<bool()> preRuntimeFlowTick;
        std::function<bool(RE::Actor*)> postRuntimeFlowTick;
        std::function<bool()> isGamePaused;
        std::function<RE::Actor*()> resolvePlayer;
        std::function<void()> onNoPlayerTick;
        std::function<void(bool)> updateAmbientKidnapAvailability;
    };

    void InstallRuntime();
    void ResetRuntimeLifecycle();
    bool TickRuntime();
    void InstallContinuousRuntimeProviders(ContinuousRuntimeProviders providers);
    void ResetContinuousRuntimeProviders();

    void InstallPassiveRuntimeProviders(PassiveRuntimeProviders providers);
    void ResetPassiveRuntimeProviders();

    struct OutcomeRuntimeProviders
    {
        std::function<void(RE::Actor*, double, const char*)> applyReleaseFollowGrace;
        std::function<void(RE::Actor*, const char*)> removeReleaseFollowGrace;
        std::function<std::uint32_t()> resolveBleedFlowActorFormID;
        std::function<bool()> isBleedStateActive;
        std::function<void(const char*)> preparePlayerForCaptivePleasureScene;
        std::function<void(const char*)> completeCaptivePleasureHandoff;
        std::function<void(const char*)> preparePlayerForBleedoutPleasureScene;
        std::function<void(const char*)> completeBleedPleasureHandoff;
    };

    void InstallOutcomeRuntimeProviders(OutcomeRuntimeProviders providers);
    void ResetOutcomeRuntimeProviders();
    bool HandleOutcomeModEvent(const char* eventName, const char* strArg = nullptr, float numArg = 0.0f, RE::TESForm* sender = nullptr);
    bool HandlePassiveBreakHitEvent(RE::Actor* causeActor, RE::Actor* targetActor);
    bool HandlePassiveBreakModEvent(const char* eventName, const char* strArg = nullptr);


    enum class ObservedDefeatResolution : std::uint8_t
    {
        None = 0,
        ContinueObserve,
        NonCaptiveChoice,
        LeftForDead,
        Captive
    };

    enum class NonCaptiveFallbackResolution : std::uint8_t
    {
        None = 0,
        RecoveryFollower,
        RecoveryPotion,
        RescueCached,
        LeftForDeadSolo,
        LeftForDeadWithFollower
    };

    struct ObservedDefeatInput
    {
        bool conflictResolved{ false };
        bool hadValidObservedEnemy{ false };
        bool hasStandingPlayerSide{ false };
        bool hasStandingTeammate{ false };
        bool hasStandingHostileCoalition{ false };
        bool hasCaptiveMarker{ false };
        bool canUseCaptiveFallback{ false };
        bool forceCaptive{ false };
        std::uint32_t actorFormID{ 0 };
    };

    struct NonCaptiveFallbackInput
    {
        bool hasSavior{ false };
        bool hasStandingFollower{ false };
        bool hasDownedFollower{ false };
        bool hasRecoveryPotion{ false };
        bool hasCachedRescueDestination{ false };
    };


    struct NonCaptiveFallbackExecutionHandlers
    {
        std::function<bool(TFD::Bleedout::TerminalCommit, const char*)> tryBeginTerminalCommit;
        std::function<void()> clearCaptiveOrchestrationResidue;
        std::function<RE::Actor*()> getPlayer;
        std::function<void(const char*)> clearBridgeAliases;
        std::function<void(bool)> setPlayerBleedImmune;
        std::function<void()> resetBleedRuntimeState;
        std::function<void()> clearLastAggressor;
        std::function<void()> updatePreCombatState;
        std::function<TFD::Transition::FallbackBranch(const char*)> resolveNoMarkerFallback;
        std::function<const char*(TFD::Transition::FallbackBranch)> getBranchName;
        std::function<bool(const char*)> beginRescueTransition;
        std::function<void()> forceLeftForDeadSolo;
        std::function<void(const char*)> beginRecoverTransition;
    };

    struct BattleObserverRuntimeProviders
    {
        std::function<void(const char*)> clearBridgeAliases;
        std::function<void()> clearCaptiveResidue;
        std::function<void()> clearAllFactions;
        std::function<void()> recoverVictoryTeammates;
        std::function<void()> resetBleedRuntimeState;
        std::function<void()> clearPendingDialogueTarget;
        std::function<void(bool)> setPlayerBleedImmune;
        std::function<void(const char*)> queueNonCaptiveChoice;
        std::function<RE::Actor* ()> resolveObservedDownedFollower;
        std::function<void(RE::Actor*)> armObservedLeftForDeadFallback;
        std::function<void(const char*)> beginRecoverTransition;
        std::function<const char* ()> getCurrentFallbackBranchName;
    };

    void InstallBattleObserverRuntimeProviders(BattleObserverRuntimeProviders providers);
    void ResetBattleObserverRuntimeProviders();
	struct DefeatLifecycleProviders
	{
		PassiveRuntimeProviders passive;
		OutcomeRuntimeProviders outcome;
		BattleObserverRuntimeProviders battleObserver;
        ContinuousRuntimeProviders continuous;
	};

	void InstallDefeatLifecycleProviders(DefeatLifecycleProviders providers);
	void ResetDefeatLifecycleProviders();

    ObservedDefeatResolution EvaluateObservedDefeatResolution(const ObservedDefeatInput& input);
    NonCaptiveFallbackResolution EvaluateNonCaptiveFallback(const NonCaptiveFallbackInput& input);
    bool ApplyObservedDefeatResolution(const ObservedDefeatInput& input, std::string_view reason);
    bool ExecuteResolvedNoMarkerFallback(const char* reason, const NonCaptiveFallbackExecutionHandlers& handlers);
    void HandleObservedBattleWin(const char* reason = nullptr);
    void HandleObservedLeftForDead(const char* reason = nullptr);
    const char* ToString(NonCaptiveFallbackResolution value);

    bool QueueBridgeModEvent(const char* eventName, RE::TESForm* sender = nullptr, const char* strArg = "", float numArg = 0.0f);

    DialogueContextKind GetDialogueContextKind();
    const char* GetDialogueContextName();
    bool IsDialogueContextActive();
    PassiveHoldKind GetPassiveHoldKind();
    const char* GetPassiveHoldName();
    bool IsPassiveHoldActive();
    bool IsPassiveHoldProtectedHandoff();
    bool IsPleasureLockActive();
    bool IsPreCombatBlocked();
    bool HandlePassiveInvalidationAgainstActor(RE::Actor* actor, const char* reason = nullptr, bool severeCrime = false);

    class Controller
    {
    public:
        static Controller& GetSingleton();

        Snapshot GetSnapshot() const;
        ObservedMainStateSnapshot GetObservedMainStateSnapshot() const;
        ObservedMainState GetObservedMainState() const;
        void TickObservedMainStateDiagnostic(RE::Actor* player, std::string_view reason);

        void ResetRuntime(std::string_view reason);
        void ResetForLoad(std::string_view reason);

        bool RequestPreCombat(std::uint32_t actorFormID, std::string_view reason);
        bool RequestInCombat(std::uint32_t actorFormID, std::string_view reason);
        bool RequestCaptive(std::uint32_t actorFormID, CaptiveMode mode, std::string_view reason);
        bool RequestVictory(std::uint32_t actorFormID, std::string_view reason);

        bool RequestTruceDecision(std::uint32_t actorFormID, std::string_view reason);
        bool RequestPlayerBleedoutDecision(std::uint32_t actorFormID, std::string_view reason);
        bool RequestEnemyBleedoutDecision(std::uint32_t actorFormID, std::string_view reason);

        bool RequestResolvePreCombatOutcome(PreCombatOutcome outcome, std::uint32_t actorFormID, std::string_view reason);
        bool RequestResolveInCombatOutcome(InCombatOutcome outcome, std::uint32_t actorFormID, std::string_view reason);
        bool RequestResolveBleedoutOutcome(BleedoutOutcome outcome, std::uint32_t actorFormID, std::string_view reason);
        bool RequestResolveVictoryOutcome(VictoryOutcome outcome, std::uint32_t actorFormID, std::string_view reason);
        bool RequestResolveCaptiveOutcome(CaptiveOutcome outcome, std::uint32_t actorFormID, std::string_view reason);
        bool RequestResolveInCombatPleasure(std::uint32_t actorFormID, std::string_view reason);
        bool RequestResolveInCombatTerminal(std::uint32_t actorFormID, std::string_view reason);

        bool RequestBeginAfterPleasure(std::uint32_t actorFormID, std::string_view reason);
        bool RequestCompleteAfterPleasure(std::string_view reason);
        bool RequestCompleteTerminalContext(std::string_view reason);
        bool RequestAbortPreCombat(std::uint32_t actorFormID, std::string_view reason);
        bool RequestCaptiveFromModEvent(std::uint32_t actorFormID, std::string_view reason);
        bool RequestCaptivePleasureFromModEvent(std::uint32_t actorFormID, std::string_view reason);

        bool BeginPreCombat(std::uint32_t actorFormID, std::string_view reason);
        bool BeginInCombat(std::uint32_t actorFormID, std::string_view reason);
        bool BeginCaptive(std::uint32_t actorFormID, CaptiveMode mode, std::string_view reason);
        bool BeginVictory(std::uint32_t actorFormID, std::string_view reason);

        bool BeginTruceDecision(std::uint32_t actorFormID, std::string_view reason);
        bool BeginPlayerBleedoutDecision(std::uint32_t actorFormID, std::string_view reason);
        bool BeginEnemyBleedoutDecision(std::uint32_t actorFormID, std::string_view reason);

        bool ResolvePreCombatOutcome(PreCombatOutcome outcome, std::uint32_t actorFormID, std::string_view reason);
        bool ResolveInCombatOutcome(InCombatOutcome outcome, std::uint32_t actorFormID, std::string_view reason);
        bool ResolveBleedoutOutcome(BleedoutOutcome outcome, std::uint32_t actorFormID, std::string_view reason);
        bool ResolveVictoryOutcome(VictoryOutcome outcome, std::uint32_t actorFormID, std::string_view reason);
        bool ResolveCaptiveOutcome(CaptiveOutcome outcome, std::uint32_t actorFormID, std::string_view reason);
        bool ResolveInCombatPleasure(std::uint32_t actorFormID, std::string_view reason);
        bool ResolveInCombatTerminal(std::uint32_t actorFormID, std::string_view reason);

        bool BeginAfterPleasure(std::uint32_t actorFormID, std::string_view reason);
        bool CompleteAfterPleasure(std::string_view reason);
        bool CompleteTerminalContext(std::string_view reason);
        bool AbortPreCombat(std::uint32_t actorFormID, std::string_view reason);

        void NotifyCombatStarted(std::uint32_t actorFormID, std::string_view reason);
        void NotifyCombatEnded(std::string_view reason);
        void NotifyPlayerBleedout(std::uint32_t actorFormID, std::string_view reason);
        void NotifyEnemyBleedout(std::uint32_t actorFormID, std::string_view reason);

        bool CanStartPreCombat() const;
        bool CanStartInCombatTruce() const;
        bool CanEnterCaptive() const;
        bool CanEnterVictory() const;
        bool IsCaptiveContext() const;
        bool IsJoinedEnemyMode() const;
        bool IsBleedDecisionActive() const;
        bool IsCombatOrBleedRootActive() const;
        bool IsRescueRootActive() const;
        bool IsRecoveryRootActive() const;
        bool IsLeftForDeadRootActive() const;
        bool IsCaptiveEscapeContextActive() const;
        bool IsPleasureSubFlowActive() const;
        bool IsAfterPleasureSubFlowActive() const;
        bool IsInCombatAfterPleasureContextActive() const;
        bool IsTerminalDialogueGateActive() const;
        bool BeginCaptiveFromModEvent(std::uint32_t actorFormID, std::string_view reason);
        bool BeginCaptivePleasureFromModEvent(std::uint32_t actorFormID, std::string_view reason);

        static const char* ToString(RootFlow value);
        static const char* ToString(ObservedMainState value);
        static const char* ToString(DecisionGate value);
        static const char* ToString(CaptiveMode value);
        static const char* ToString(SubFlow value);
        static const char* ToString(PreCombatOutcome value);
        static const char* ToString(InCombatOutcome value);
        static const char* ToString(BleedoutOutcome value);
        static const char* ToString(VictoryOutcome value);
        static const char* ToString(CaptiveOutcome value);

    private:
        Controller() = default;

        bool BeginRootLocked(RootFlow next, std::uint32_t actorFormID, std::string_view reason);
        bool TransitionRootLocked(RootFlow next, std::uint32_t actorFormID, std::string_view reason);
        bool BeginCaptiveLocked(std::uint32_t actorFormID, CaptiveMode mode, std::string_view reason);
        bool EnterTerminalContextLocked(RootFlow contextRoot, SubFlow sub, std::uint32_t actorFormID, std::string_view reason);
        bool CompleteTerminalContextLocked(std::string_view reason);
        bool GuardPreCombatOwnerLocked(std::uint32_t actorFormID, std::string_view op, std::string_view reason) const;
        bool RejectLocked(std::string_view op, std::string_view reason) const;
        void SetPrimaryActorLocked(std::uint32_t actorFormID);
        void ClearDecisionLocked();
        void ClearSubLocked();
        void BumpTokenLocked();
        void ClearTerminalLocked();
        void ClearAllLocked();
        void SetCaptiveIdleLocked();
        void RefreshFlowGlobalsLocked();

        mutable std::mutex _lock;
        Snapshot _snapshot{};
        bool _combatActive{ false };
        ObservedMainStateSnapshot _observedSnapshot{};
        bool _hasObservedSnapshot{ false };
    };
}

namespace TFD {
    namespace Flow = FlowController;
}
