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
		RE::BGSRefAlias* captiveMarkerAlias{ nullptr };
		RE::BGSRefAlias* escapeDoorAlias{ nullptr };
		RE::BGSRefAlias* approachPointAlias{ nullptr };
		RE::BGSRefAlias* escapeRouteAlias{ nullptr };
		RE::BGSRefAlias* bossAlias{ nullptr };
		RE::BGSRefAlias* workMineAlias{ nullptr };
		RE::BGSRefAlias* workCraftingStationAlias{ nullptr };
		std::array<RE::BGSRefAlias*, 10> workCraftingStationAliases{};
		RE::BGSRefAlias* workItemAlias{ nullptr };
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

	struct RuntimeTickHandlers
	{
		std::function<bool()> isDialogueOpen;
		std::function<bool()> getPrevDialogueOpen;
		std::function<void(bool)> setPrevDialogueOpen;
		EscapeTickHandlers escape;
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
	bool IsReleasedWorkActive();
	bool IsCurrentWorkBoss(RE::Actor* actor);
	bool IsReleasedWorkActorInScope(RE::Actor* actor);
	bool EnsureReleasedWorkDialogueActor(RE::Actor* actor, const char* reason);
	bool PromoteReleasedWorkBoss(RE::Actor* actor, const char* reason);
	RE::Actor* GetCurrentWorkBoss();
	bool IsCaptivePassiveHoldActive();
	bool IsEscapeBleedoutActive();
	bool IsRecaptureCommitActive();
	bool IsRecaptureRecentlyCommitted();
	bool CommitRecapture(RE::Actor* preferredCaptor, const char* reason);
	bool HasEscapeBreakRebleedPending();
	void SetEscapeBreakRebleedPending(bool pending);
	void QueueEscapeBreakRebleed(RE::Actor* preferredAggressor);
	void ClearEscapeBreakRebleed();
	RE::Actor* ResolveEscapeBreakPreferredAggressor(float radius, const std::function<RE::Actor*(float)>& fallbackResolver = {});
	void QueueLoadedState(bool stateActive, PhaseValue phase);
	void QueueLoadedWorkSession(std::uint32_t bossFormID, std::uint32_t jobType, std::uint32_t assignmentState);
	std::uint32_t GetWorkBossFormIDForSave();
	std::uint32_t GetWorkJobTypeForSave();
	std::uint32_t GetWorkAssignmentStateForSave();
	void ClearQueuedLoadedState();
	bool GetQueuedStateFlag();
	PhaseValue GetQueuedPhase();
	void SealDoorIfPresent();

	void ResolveQuestRegistry();
	void WriteQuestAlias(RE::BGSRefAlias* alias, RE::TESObjectREFR* ref, const char* reason = nullptr);
	void SyncPlayerAlias(RE::Actor* actor, const char* reason);
	void SyncStorageDebugAliases(const char* reason);
	void ClearStorageDebugAliases(const char* reason);
	RE::TESObjectREFR* ResolveRecoverGearLootTarget();
	bool IsRecoverGearLootTarget(RE::TESObjectREFR* ref);
	bool NotifyRecoverGearContainerOpened(RE::TESObjectREFR* ref, const char* reason);
	void BeginReleasedWorkRuntime(RE::Actor* actor, const char* reason);
	void SyncCaptiveWorkResourceAliases(const char* reason);
	void ClearCaptiveWorkResourceAliases(const char* reason, bool clearItemAlias = false);
	void HandleReleasedWorkNoJob(RE::Actor* actor, double cooldownSeconds, const char* reason);
	void ClearReleasedWorkRuntime(const char* reason);
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
	RE::TESObjectREFR* ResolveCaptorApproachTarget(RE::Actor* player);
	const char* GetCaptorApproachTargetName(RE::TESObjectREFR* target, RE::Actor* player);
	void SyncCaptorApproachPointAlias(RE::Actor* player, const char* reason);
	void ClearCaptorApproachPointAlias(const char* reason);
	void BindDoor(RE::TESObjectREFR* door);
	void ClearEscapeContext();
	void ArmEscapeContextFromCurrentState(RE::Actor* player);
	void BeginReturnToCaptiveTransitionGuard(const char* reason, double seconds = 4.0);
	void EndReturnToCaptiveTransitionGuard(const char* reason);
	bool IsReturnToCaptiveTransitionGuardActive();
	bool IsDoorNearMarker(RE::TESObjectREFR* door);
	RE::TESObjectREFR* ResolveLockpickDoorCandidate(RE::TESObjectREFR* target);
	bool UpdateLockpickEscapeWatch(const std::function<void(const char*, RE::TESObjectREFR*)>& onEscapeCommit);
	bool TryCommitEscapeByRadius(RE::Actor* player);
	bool DidEscapeByLocation(RE::Actor* player, RE::FormID* oldLocationOut = nullptr, RE::FormID* newLocationOut = nullptr);
	bool NormalizeInvalidCaptivePair();
	bool TickCaptiveEscapePhase(RE::Actor* player, const EscapeTickHandlers& handlers);
	bool TickEscapeActivePhase(RE::Actor* player, const EscapeTickHandlers& handlers);
	bool TickRuntime(RE::Actor* player, bool captiveBleedOverlay, const RuntimeTickHandlers& handlers);
	bool TriggerPlayerAggressionEscape(RE::Actor* actor, const char* reason);

	bool IsCaptorCallCooldownActive(float* remainingSeconds = nullptr);
	void ClearCaptorCallCooldown();

	void RefreshCaptorApproachAI(RE::Actor* actor, const char* reason);
	void LogCallingCaptorOwnershipSnapshot(RE::Actor* actor, const char* reason);

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
