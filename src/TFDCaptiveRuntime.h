#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
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

	void ResolveQuestRegistry();
	void WriteQuestAlias(RE::BGSRefAlias* alias, RE::TESObjectREFR* ref);
	void SyncPlayerAlias(RE::Actor* actor, const char* reason);
	void SyncStorageDebugAliases(const char* reason);
	void ClearStorageDebugAliases(const char* reason);
	bool EnsureStarterLockpicks(std::int32_t targetCount, const char* reason);
	bool TransferPlayerInventoryToStorage(RE::TESObjectREFR* target, const char* reason);
	void ClearPendingConfiscation(const char* reason);
	void QueuePendingConfiscation(const char* reason, bool starterKitWanted);
	void ProcessPendingConfiscation();
	void SetRuntimeState(bool stateActive, PhaseValue phase);

	void SetPrevLockpickOpen(bool open);
	void CaptureCurrentLockpickMenuState();
	void ResetLockpickWatch();
	RE::TESObjectREFR* ResolveBoundEscapeDoor();
	void BindDoor(RE::TESObjectREFR* door);
	void ClearEscapeContext();
	void ArmEscapeContextFromCurrentState(RE::Actor* player);
	bool IsDoorNearMarker(RE::TESObjectREFR* door);
	RE::TESObjectREFR* ResolveLockpickDoorCandidate(RE::TESObjectREFR* target);
	bool UpdateLockpickEscapeWatch(const std::function<void(const char*, RE::TESObjectREFR*)>& onEscapeCommit);
	bool TryCommitEscapeByRadius(RE::Actor* player);
	bool DidEscapeByLocation(RE::Actor* player, RE::FormID* oldLocationOut = nullptr, RE::FormID* newLocationOut = nullptr);

	bool BeginCaptorCallHotkey(RE::Actor* player, RE::Actor** outCaptor = nullptr);

	void ResetForLoad();
}
