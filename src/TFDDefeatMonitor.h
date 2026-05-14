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
	bool HandlePassiveInvalidationAgainstActor(RE::Actor* actor, const char* reason = nullptr);

	std::uint32_t GetCaptivePhaseRaw();
	const char* GetCaptivePhaseName();
	bool IsCaptiveFamily();
	bool IsThresholdDownedActor(RE::Actor* actor);
	bool IsThresholdCombatTargetValid(RE::Actor* actor);

	void ForceRecoverPlayerAfterCaptiveRecapture(const char* reason = nullptr);
}
