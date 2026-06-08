#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

#include <RE/Skyrim.h>

namespace TFD::Bleedout
{
    enum class TerminalCommit : std::uint8_t;

    namespace Builders
    {
        struct NonCaptiveChoiceProvider;
        struct BlackoutProvider;
        struct TransitionRuntimeProvider;
        struct TransitionCaptiveProvider;
    }
}

namespace TFD::Transition
{
	enum class Kind
	{
		None = 0,
		Captive = 1,
		Rescue = 2,
		Recover = 3
	};

	enum class FallbackBranch : std::uint32_t
	{
		None = 0,
		RecoveryFollower = 1,
		RecoveryPotion = 2,
		RescueCached = 3,
		LeftForDeadSolo = 4,
		LeftForDeadWithFollower = 5
	};

	struct RuntimeHandlers
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

	struct CaptiveHandlers
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

	const char* GetKindName(Kind kind);
	const char* GetBranchName(FallbackBranch branch);
	FallbackBranch GetCurrentFallbackBranch();
	const char* GetCurrentFallbackBranchName();

	bool QueueRequest(Kind kind, bool fadeIn, const char* reason = nullptr);

	void ClearPendingFadeIn();
	void ArmPendingFadeIn(Kind kind, std::chrono::steady_clock::time_point notBefore, bool sawLoadingMenu = false);
	bool HasPendingFadeIn();
	void ProcessPendingFadeIn();

	bool IsAwaiting();
	void PollResult();

	bool BeginImmediate(Kind kind, const char* reason, const std::function<bool(const char*)>& completeNow);
	void BeginImmediateVoid(Kind kind, const char* reason, const std::function<void(const char*)>& completeNow);

	void ShowBlackoutFader();
	void HideBlackoutFader();

	void ClearNoMarkerFallbackState();
	void AbortCalmWindowForCombat(RE::Actor* actor = nullptr, const char* reason = nullptr);
	bool IsRecoveryActive();
	bool HasRecoveryPotionAvailable();
	void BeginLeftForDeadCooldown(int seconds);
	bool IsLeftForDeadCooldownActive(const RuntimeHandlers& handlers);
	void TickLeftForDeadCooldown(const RuntimeHandlers& handlers);
	void ClearLeftForDeadCooldown(const RuntimeHandlers& handlers);
	void MaintainCalmWindow(const RuntimeHandlers& handlers);
	void RecoverPlayerForTransition(const RuntimeHandlers& handlers);
	void RecoverPlayerAfterTeleport(const RuntimeHandlers& handlers);
	FallbackBranch ResolveNoMarkerFallback(const char* reason, const RuntimeHandlers& handlers);
	void ArmObservedLeftForDeadFallback(RE::Actor* follower, const RuntimeHandlers& handlers);
	void ForceLeftForDeadSolo(const RuntimeHandlers& handlers);
	bool BeginRescueTransition(const char* reason, const RuntimeHandlers& handlers);
	void BeginRecoverTransition(const char* reason, const RuntimeHandlers& handlers);

	bool ResolveCaptiveMarkerForOutcome(const RuntimeHandlers& handlers);
	bool TeleportPlayerToCachedMarkerNow(const RuntimeHandlers& handlers);
	bool CompleteCaptiveTransitionNow(const char* reason, const RuntimeHandlers& handlers, const CaptiveHandlers& captiveHandlers);
}

// Consolidated from former TFDDefeatTransitionGlue staging module
namespace TFD::Transition::DefeatGlue
{
    struct RuntimeProviders
    {
        std::function<bool(TFD::Bleedout::TerminalCommit, const char*)> tryBeginTerminalCommit;
        std::function<void()> clearCaptiveOrchestrationResidue;
        std::function<RE::Actor*()> getPlayer;
        std::function<void(const char*)> clearBridgeAliases;
        std::function<void(bool)> setPlayerBleedImmune;
        std::function<void()> resetBleedRuntimeState;
        std::function<void()> clearLastAggressor;
        std::function<void()> updatePreCombatState;
    };

    void InstallProviders(
        TFD::Bleedout::Builders::NonCaptiveChoiceProvider nonCaptiveChoice,
        TFD::Bleedout::Builders::BlackoutProvider blackout,
        TFD::Bleedout::Builders::TransitionRuntimeProvider transitionRuntime,
        TFD::Bleedout::Builders::TransitionCaptiveProvider transitionCaptive,
        RuntimeProviders runtime);

    void Reset();

    TFD::Transition::RuntimeHandlers BuildTransitionRuntimeHandlers();
    TFD::Transition::CaptiveHandlers BuildTransitionCaptiveHandlers();
    bool BeginResolvedNoMarkerFallback(const char* reason);
}

