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

namespace TFD::Captive
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
	DoorController& DoorControllerRef();
	RE::ObjectRefHandle& MarkerRef();
	RE::FormID& CellFormIDRef();
	RE::FormID& LocationFormIDRef();
	bool& QueuedStateRef();
	PhaseValue& QueuedPhaseRef();

	struct EscapeTickHandlers
	{
		std::function<void()> updatePreCombatState;
		std::function<void(bool)> setPrevDialogueOpen;
		std::function<void()> clearLastAggressor;
		std::function<void(RE::Actor*)> setLastAggressor;
		std::function<RE::Actor*()> resolveAggressor;
		std::function<RE::Actor*(float)> findBestAggressor;
		std::function<void(bool)> setGraceActive;
	};

	PhaseValue PhaseFromRaw(std::uint32_t raw);
	std::uint32_t GetPhaseRaw(bool stateActive, PhaseValue phase);
	const char* GetPhaseName(bool stateActive, PhaseValue phase);
	bool IsFamily(bool stateActive, PhaseValue phase);

	bool GetStateFlag();
	PhaseValue GetPhase();
	std::uint32_t GetPhaseRaw();
	const char* GetPhaseName();
	bool IsFamily();
	bool IsActive();
	bool IsEscapeActive();
	bool IsStandardCaptiveActive();
	bool HasEscapeBreakRebleedPending();
	void SetEscapeBreakRebleedPending(bool pending);
	void QueueEscapeBreakRebleed(RE::Actor* preferredAggressor);
	void ClearEscapeBreakRebleed();
	RE::Actor* ResolveEscapeBreakPreferredAggressor(float radius, const std::function<RE::Actor*(float)>& fallbackResolver = {});
	void QueueLoadedState(bool stateActive, PhaseValue phase);
	void ClearQueuedLoadedState();
	bool GetQueuedStateFlag();
	PhaseValue GetQueuedPhase();
	void SealDoorIfPresent();

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
	bool NormalizeInvalidCaptivePair();
	bool TickCaptiveEscapePhase(RE::Actor* player, const EscapeTickHandlers& handlers);
	bool TickEscapeActivePhase(RE::Actor* player, const EscapeTickHandlers& handlers);
	bool TriggerPlayerAggressionEscape(RE::Actor* actor, const char* reason);

	bool BeginCaptorCallHotkey(RE::Actor* player, RE::Actor** outCaptor = nullptr);

	struct ApplyQueuedDefeatProgressHandlers
	{
		std::function<RE::Actor*()> getPlayer;
		std::function<bool()> isDialogueOpen;
		std::function<void(bool)> setPrevDialogueOpen;
	};

	void ApplyQueuedDefeatProgressState(const ApplyQueuedDefeatProgressHandlers& handlers);

	void ResetForLoad();
}
