#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

#include <RE/Skyrim.h>
#include "TFDCaptiveDoorController.h"
#include "RE/B/BGSRefAlias.h"
#include "RE/T/TESQuest.h"

namespace TFD::CaptiveRuntime
{
	enum class PhaseValue : int
	{
		None = 0,
		Captive = 1,
		Escape = 2,
		ReleasedWork = 3,
		Scene = 4
	};

	struct QuestRegistryCache
	{
		RE::TESQuest* quest{ nullptr };
		RE::BGSRefAlias* playerCaptiveAlias{ nullptr };
		std::array<RE::BGSRefAlias*, 3> bossCaptorAliases{};
		std::array<RE::BGSRefAlias*, 3> bossContainerAliases{};
		std::array<RE::BGSRefAlias*, 3> containerAliases{};
		RE::BGSRefAlias* lootTargetAlias{ nullptr };
		bool resolved{ false };
	};

	bool& StateRef();
	PhaseValue& PhaseRef();
	bool& ConfiscationAppliedRef();
	bool& ConfiscationPendingRef();
	bool& StarterLockpickPendingRef();
	std::string& PendingConfiscationReasonRef();
	int& ConfiscationAttemptCountRef();
	std::chrono::steady_clock::time_point& ConfiscationNextAttemptRef();
	QuestRegistryCache& QuestRegistryRef();
	TFD::CaptiveDoorController& DoorControllerRef();
	RE::ObjectRefHandle& MarkerRef();
	RE::FormID& CellFormIDRef();
	RE::FormID& LocationFormIDRef();
	bool& QueuedStateRef();
	PhaseValue& QueuedPhaseRef();

	PhaseValue PhaseFromRaw(std::uint32_t raw);
	std::uint32_t GetPhaseRaw(bool stateActive, PhaseValue phase);
	const char* GetPhaseName(bool stateActive, PhaseValue phase);
	bool IsFamily(bool stateActive, PhaseValue phase);

	bool IsActive();
bool IsEscapeActive();
bool IsStandardCaptiveActive();

void ResetForLoad();
}
