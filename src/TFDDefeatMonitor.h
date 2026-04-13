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

	std::uint32_t GetCaptivePhaseRaw();
	const char* GetCaptivePhaseName();
	bool IsCaptiveFamily();
	bool IsBleedoutActive();
	bool HandleBleedoutHotkey();
	bool IsLeftForDeadRecoveryActive();
	bool IsPlayerBleedHoldTargetBlocked();
	bool IsObservedCombatCommitInProgress();
	bool IsThresholdDownedActor(RE::Actor* actor);
	bool ReviveDownedAlly(RE::Actor* actor, float targetHealthPct = 55.0f);
	bool IsThresholdCombatTargetValid(RE::Actor* actor);
	void NoteEnemyTargetingPlayer(RE::Actor* actor);
	RE::Actor* ResolveBleedRedirectTarget(RE::Actor* actor);
	RE::Actor* ResolveBleedFollowerAggroTarget(RE::Actor* actor);

	bool IsDefeatedEnemyKnocked(RE::Actor* actor);
	bool IsDialogueCapableDefeatedEnemy(RE::Actor* actor);
	bool IsCreatureDefeatedEnemy(RE::Actor* actor);
	double GetDefeatedEnemyRemainingSeconds(RE::Actor* actor);
	bool RecruitDefeatedHumanoidAsTeammate(RE::Actor* actor);
}