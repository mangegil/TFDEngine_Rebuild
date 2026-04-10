#pragma once

#include <cstdint>

namespace RE
{
	class Actor;
}

namespace TFD::DefeatMonitor
{
	void Install();
	void Shutdown();

	void ResetGrace();

	bool GetCaptiveStateForSave();
	std::uint32_t GetCaptivePhaseForSave();
	bool GetBleedOutStateForSave();

	void QueueLoadedBleedOutState(bool active);
	void QueueLoadedProgressState(bool stateActive, std::uint32_t phaseRaw);
	void QueueDefaultProgressState();
	bool HasQueuedProgressState();
	void ApplyQueuedProgressState();

	void ResetForLoad();
	void SetLoadTransition(bool active);
	void SetPendingDefeatedDialogueTarget(RE::Actor* actor);
	bool HandlePassiveInvalidationAgainstActor(RE::Actor* actor, const char* reason = nullptr);

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
	bool IsCaptivePhase();
	std::uint32_t GetCaptivePhaseRaw();
	const char* GetCaptivePhaseName();
	bool IsCaptiveFamily();
	bool IsBleedoutActive();
	bool HandleBleedoutHotkey();
	bool IsLeftForDeadRecoveryActive();
	DialogueContextKind GetDialogueContextKind();
	const char* GetDialogueContextName();
	bool IsDialogueContextActive();
	PassiveHoldKind GetPassiveHoldKind();
	const char* GetPassiveHoldName();
	bool IsPassiveHoldActive();
	bool IsPassiveHoldProtectedHandoff();
	bool IsPleasureLockActive();
	bool IsPreCombatBlocked();
	bool IsPlayerBleedHoldTargetBlocked();
	bool IsObservedCombatCommitInProgress();
	bool IsThresholdDownedActor(RE::Actor* actor);
	bool ReviveDownedAlly(RE::Actor* actor, float targetHealthPct = 55.0f);
	bool IsThresholdCombatTargetValid(RE::Actor* actor);
	bool HasReleaseFollowGraceForActor(RE::Actor* actor);
	void NoteEnemyTargetingPlayer(RE::Actor* actor);
	RE::Actor* ResolveBleedRedirectTarget(RE::Actor* actor);
	RE::Actor* ResolveBleedFollowerAggroTarget(RE::Actor* actor);

	bool IsDefeatedEnemyKnocked(RE::Actor* actor);
	bool IsDialogueCapableDefeatedEnemy(RE::Actor* actor);
	bool IsCreatureDefeatedEnemy(RE::Actor* actor);
	double GetDefeatedEnemyRemainingSeconds(RE::Actor* actor);
	bool RecruitDefeatedHumanoidAsTeammate(RE::Actor* actor);
	bool RecruitDefeatedCreatureAsTeammate(RE::Actor* actor, double nowSec);
}