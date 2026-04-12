#include "TFDCaptive.h"

#include "TFDActorScan.h"
#include "TFDHostilityController.h"
#include "TFDHostilityController.h"
#include "TFDTame.h"
#include "TFDSettings.h"
#include "TFDLocation.h"
#include "TFDFactionManager.h"

#include "TFDFlowController.h"

#include <SKSE/SKSE.h>

#include <RE/L/LockpickingMenu.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <vector>

namespace TFD::Captive
{
	namespace
	{
		static bool IsActorSameSpace(RE::Actor* actor, RE::Actor* player)
		{
			if (!actor || !player) {
				return false;
			}

			auto* actorCell = actor->GetParentCell();
			auto* playerCell = player->GetParentCell();
			if (!actorCell || !playerCell) {
				return false;
			}

			const bool actorInterior = actorCell->IsInteriorCell();
			const bool playerInterior = playerCell->IsInteriorCell();
			if (actorInterior != playerInterior) {
				return false;
			}

			if (playerInterior) {
				return actorCell == playerCell;
			}

			auto* actorWs = actor->GetWorldspace();
			auto* playerWs = player->GetWorldspace();
			return actorWs && playerWs && actorWs == playerWs;
		}

		static bool ActorHasLOS(RE::Actor* actor, RE::Actor* player)
		{
			if (!actor || !player) {
				return false;
			}
			bool hasLOSData = false;
			return actor->HasLineOfSight(player, hasLOSData);
		}

		static bool IsCaptorSupportedActor(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || actor == player) {
				return false;
			}
			if (actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
				return false;
			}
			if (actor->IsPlayerTeammate() || TFD::Tame::IsCompanion(actor)) {
				return false;
			}
			const bool isNPC = actor->HasKeywordString("ActorTypeNPC");
			const bool isCreature = actor->HasKeywordString("ActorTypeCreature");
			return isNPC || isCreature;
		}

		static RE::Actor* PickCaptorSameCellLoaded(RE::Actor* player, float radius)
		{
			if (!player) {
				return nullptr;
			}

			const float searchRadius = (std::max)(radius, 12288.0f);
			TFD::ActorScan::Rescan(searchRadius, false);

			RE::Actor* best = nullptr;
			float bestScore = 1.0e30f;

			const auto n = TFD::ActorScan::GetCount();
			for (int i = 0; i < n; ++i) {
				auto e = TFD::ActorScan::GetEntry(i);
				auto* a = TFD::ActorScan::GetActor(i);
				if (!a) continue;
				if (!IsCaptorSupportedActor(a)) continue;
				if (!IsActorSameSpace(a, player)) continue;
				if (e.dist > searchRadius) continue;
				if (!ActorHasLOS(a, player)) continue;

				float score = e.dist;
				if (e.hostile || a->IsHostileToActor(player)) score -= 140.0f;
				if (e.inCombat || a->IsInCombat()) score -= 100.0f;
				if (score < bestScore) {
					bestScore = score;
					best = a;
				}
			}

			return best;
		}

		static void SendBridgeEvent(const char* eventName)
		{
			if (!eventName) {
				return;
			}

			auto* src = SKSE::GetModCallbackEventSource();
			if (!src) {
				return;
			}

			SKSE::ModCallbackEvent e(eventName, "", 0.0f, nullptr);
			src->SendEvent(&e);
		}

		static void SendBridgeAssignActor(const char* eventName, RE::Actor* actor)
		{
			if (!eventName || !actor) {
				return;
			}

			auto* src = SKSE::GetModCallbackEventSource();
			if (!src) {
				return;
			}

			SKSE::ModCallbackEvent e(eventName, "", 0.0f, actor);
			src->SendEvent(&e);
		}

		static void ApplyCallCaptorCalmBubble(RE::Actor* player, RE::Actor* primaryTarget, float radius)
		{
			if (!player || !primaryTarget) {
				return;
			}

			const float sweepRadius = (std::max)(radius, (std::max)(TFD::Settings::GetSweepRadius(), 12000.0f));
			TFD::HostilityController::StopCombatSweep(sweepRadius, true);
			TFD::HostilityController::ScheduleStopCombatWaves(sweepRadius, true, 10, 120);
			TFD::ActorScan::Rescan(sweepRadius, false);

			auto* pCell = player->GetParentCell();
			const auto count = TFD::ActorScan::GetCount();
			for (int i = 0; i < count; ++i) {
				auto entry = TFD::ActorScan::GetEntry(i);
				auto* actor = TFD::ActorScan::GetActor(i);
				if (!actor || actor->IsDead() || actor->IsDisabled()) {
					continue;
				}
				if (!actor->Is3DLoaded()) {
					continue;
				}
				if (actor->GetFormID() == player->GetFormID()) {
					continue;
				}
				if (pCell && actor->GetParentCell() != pCell) {
					continue;
				}
				if (actor->GetFormID() != primaryTarget->GetFormID() && !entry.hostile && !entry.inCombat) {
					continue;
				}

				if (auto* process = RE::ProcessLists::GetSingleton()) {
					const bool runDetection = process->runDetection;
					process->runDetection = false;
					process->ClearCachedFactionFightReactions();
					process->StopCombatAndAlarmOnActor(actor, false);
					process->runDetection = runDetection;
				}
				actor->StopCombat();
				if (actor->IsWeaponDrawn()) {
					actor->DrawWeaponMagicHands(false);
				}
				actor->EvaluatePackage(true, false);
			}

			SendBridgeEvent("TFDCaptiveClearAll");
			SendBridgeAssignActor("TFDCaptiveAssign", primaryTarget);
		}

		static constexpr int kCaptiveConfiscationInitialDelayMs = 300;
		static constexpr int kCaptiveConfiscationRetryDelayMs = 250;
		static constexpr int kCaptiveConfiscationMaxAttempts = 20;
		static constexpr double kCaptiveEscapeDoorRadius = 512.0;

		static RE::Actor* Player()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

		static auto Now()
		{
			return std::chrono::steady_clock::now();
		}

		static RE::TESObjectREFR* LookupRefByFormID(std::uint32_t formID)
		{
			if (formID == 0) {
				return nullptr;
			}
			return RE::TESForm::LookupByID<RE::TESObjectREFR>(formID);
		}

		static bool IsDoorRef(RE::TESObjectREFR* ref)
		{
			if (!ref) return false;
			auto* base = ref->GetBaseObject();
			if (!base) return false;
			return base->GetFormType() == RE::FormType::Door;
		}

		static bool IsRefLocked(RE::TESObjectREFR* ref)
		{
			if (!ref) return false;
			auto* lock = ref->GetLock();
			return lock && lock->IsLocked();
		}

		bool g_state = false;
		PhaseValue g_phase = PhaseValue::None;
		bool g_confiscationApplied = false;
		bool g_confiscationPending = false;
		bool g_starterLockpickPending = false;
		std::string g_pendingConfiscationReason{};
		int g_confiscationAttemptCount = 0;
		std::chrono::steady_clock::time_point g_confiscationNextAttempt{};
		QuestRegistryCache g_registry{};
		TFD::Captive::DoorController g_door{};
		RE::ObjectRefHandle g_marker{};
		RE::FormID g_cellFormID = 0;
		RE::FormID g_locationFormID = 0;
		bool g_queuedState = false;
		PhaseValue g_queuedPhase = PhaseValue::None;
		bool g_escapeBreakBleedPending = false;
		RE::ActorHandle g_escapeBreakPreferredAggressor{};
		bool g_prevLockpickOpen = false;
		bool g_escapeRadiusActive = false;
		std::chrono::steady_clock::time_point g_escapeRadiusSince{};
		RE::ObjectRefHandle g_boundEscapeDoor{};
		RE::ObjectRefHandle g_lockpickDoorCandidate{};
		bool g_lockpickDoorWasLocked = false;
	}

	bool& StateRef() { return g_state; }
	PhaseValue& PhaseRef() { return g_phase; }
	bool& ConfiscationAppliedRef() { return g_confiscationApplied; }
	bool& ConfiscationPendingRef() { return g_confiscationPending; }
	bool& StarterLockpickPendingRef() { return g_starterLockpickPending; }
	std::string& PendingConfiscationReasonRef() { return g_pendingConfiscationReason; }
	int& ConfiscationAttemptCountRef() { return g_confiscationAttemptCount; }
	std::chrono::steady_clock::time_point& ConfiscationNextAttemptRef() { return g_confiscationNextAttempt; }
	QuestRegistryCache& QuestRegistryRef() { return g_registry; }
	TFD::Captive::DoorController& DoorControllerRef() { return g_door; }
	RE::ObjectRefHandle& MarkerRef() { return g_marker; }
	RE::FormID& CellFormIDRef() { return g_cellFormID; }
	RE::FormID& LocationFormIDRef() { return g_locationFormID; }
	bool& QueuedStateRef() { return g_queuedState; }
	PhaseValue& QueuedPhaseRef() { return g_queuedPhase; }

	PhaseValue PhaseFromRaw(std::uint32_t raw)
	{
		switch (raw) {
		case 1: return PhaseValue::Captive;
		case 2: return PhaseValue::Escape;
		case 3: return PhaseValue::ReleasedWork;
		case 4: return PhaseValue::Scene;
		default: return PhaseValue::None;
		}
	}

	std::uint32_t GetPhaseRaw(bool stateActive, PhaseValue phase)
	{
		if (stateActive && phase == PhaseValue::None) {
			return 2u;
		}
		return static_cast<std::uint32_t>(static_cast<int>(phase));
	}

	const char* GetPhaseName(bool stateActive, PhaseValue phase)
	{
		switch (phase) {
		case PhaseValue::None:
			return stateActive ? "Escape" : "None";
		case PhaseValue::Captive:
			return "Captive";
		case PhaseValue::Escape:
			return "Escape";
		case PhaseValue::ReleasedWork:
			return "ReleasedWork";
		case PhaseValue::Scene:
			return "Scene";
		default:
			return "Unknown";
		}
	}

	bool IsFamily(bool stateActive, PhaseValue phase)
	{
		return stateActive || phase != PhaseValue::None;
	}

	bool GetStateFlag()
	{
		return g_state;
	}

	PhaseValue GetPhase()
	{
		return g_phase;
	}

	std::uint32_t GetPhaseRaw()
	{
		return GetPhaseRaw(g_state, g_phase);
	}

	const char* GetPhaseName()
	{
		return GetPhaseName(g_state, g_phase);
	}

	bool IsFamily()
	{
		return IsFamily(g_state, g_phase);
	}

	bool IsActive()
	{
		return IsFamily();
	}

	bool IsEscapeActive()
	{
		return g_phase == PhaseValue::Escape;
	}

	bool IsStandardCaptiveActive()
	{
		return g_phase == PhaseValue::Captive;
	}

	bool HasEscapeBreakRebleedPending()
	{
		return g_escapeBreakBleedPending;
	}

	void SetEscapeBreakRebleedPending(bool pending)
	{
		g_escapeBreakBleedPending = pending;
		if (!pending) {
			g_escapeBreakPreferredAggressor.reset();
		}
	}

	void QueueEscapeBreakRebleed(RE::Actor* preferredAggressor)
	{
		g_escapeBreakPreferredAggressor = preferredAggressor ? preferredAggressor->GetHandle() : RE::ActorHandle{};
		g_escapeBreakBleedPending = true;
	}

	void ClearEscapeBreakRebleed()
	{
		SetEscapeBreakRebleedPending(false);
	}

	RE::Actor* ResolveEscapeBreakPreferredAggressor(float radius, const std::function<RE::Actor*(float)>& fallbackResolver)
	{
		if (g_escapeBreakPreferredAggressor) {
			auto sp = RE::Actor::LookupByHandle(g_escapeBreakPreferredAggressor.native_handle());
			if (sp) {
				return sp.get();
			}
			g_escapeBreakPreferredAggressor.reset();
		}
		return fallbackResolver ? fallbackResolver(radius) : nullptr;
	}

	void QueueLoadedState(bool stateActive, PhaseValue phase)
	{
		g_queuedState = stateActive;
		g_queuedPhase = phase;
	}

	void ClearQueuedLoadedState()
	{
		g_queuedState = false;
		g_queuedPhase = PhaseValue::None;
	}

	bool GetQueuedStateFlag()
	{
		return g_queuedState;
	}

	PhaseValue GetQueuedPhase()
	{
		return g_queuedPhase;
	}

	void SealDoorIfPresent()
	{
		if (g_door.HasDoor()) {
			g_door.SealToInitial(true);
		}
	}

	void ResolveQuestRegistry()
	{
		if (g_registry.resolved) {
			return;
		}
		g_registry.resolved = true;
		g_registry.quest = RE::TESForm::LookupByEditorID<RE::TESQuest>("TFDCaptiveQuest");
		if (!g_registry.quest) {
			spdlog::warn("[TFD][Captive] captive quest not found");
			return;
		}

		for (auto* baseAlias : g_registry.quest->aliases) {
			auto* refAlias = skyrim_cast<RE::BGSRefAlias*>(baseAlias);
			if (!refAlias) {
				continue;
			}
			const auto aliasName = std::string(refAlias->aliasName.c_str());
			if (aliasName == "PlayerCaptive") {
				g_registry.playerCaptiveAlias = refAlias;
				continue;
			}
			if (aliasName == "LootTarget") {
				g_registry.lootTargetAlias = refAlias;
				continue;
			}
			if (aliasName.rfind("BossCaptor", 0) == 0 && aliasName.size() >= 11) {
				try {
					int slot = std::stoi(aliasName.substr(10));
					if (slot >= 1 && slot <= static_cast<int>(g_registry.bossCaptorAliases.size())) {
						g_registry.bossCaptorAliases[static_cast<std::size_t>(slot) - 1] = refAlias;
					}
				} catch (...) {}
				continue;
			}
			if (aliasName.rfind("BossContainer", 0) == 0 && aliasName.size() >= 14) {
				try {
					int slot = std::stoi(aliasName.substr(13));
					if (slot >= 1 && slot <= static_cast<int>(g_registry.bossContainerAliases.size())) {
						g_registry.bossContainerAliases[static_cast<std::size_t>(slot) - 1] = refAlias;
					}
				} catch (...) {}
				continue;
			}
			if (aliasName.rfind("Container", 0) == 0 && aliasName.size() >= 10) {
				try {
					int slot = std::stoi(aliasName.substr(9));
					if (slot >= 1 && slot <= static_cast<int>(g_registry.containerAliases.size())) {
						g_registry.containerAliases[static_cast<std::size_t>(slot) - 1] = refAlias;
					}
				} catch (...) {}
				continue;
			}
		}

		std::size_t bossCaptorCount = 0;
		std::size_t bossContainerCount = 0;
		std::size_t containerCount = 0;
		for (auto* alias : g_registry.bossCaptorAliases) { if (alias) ++bossCaptorCount; }
		for (auto* alias : g_registry.bossContainerAliases) { if (alias) ++bossContainerCount; }
		for (auto* alias : g_registry.containerAliases) { if (alias) ++containerCount; }

		spdlog::info("[TFD][Captive] captive quest registry resolved quest={:08X} playerAliasID={} bossCaptorAliases={} bossContainerAliases={} containerAliases={} lootTarget={}",
			g_registry.quest ? g_registry.quest->GetFormID() : 0u,
			g_registry.playerCaptiveAlias ? g_registry.playerCaptiveAlias->aliasID : static_cast<std::uint32_t>(0),
			bossCaptorCount,
			bossContainerCount,
			containerCount,
			g_registry.lootTargetAlias ? 1 : 0);
	}

	void WriteQuestAlias(RE::BGSRefAlias* alias, RE::TESObjectREFR* ref)
	{
		ResolveQuestRegistry();
		if (!g_registry.quest || !alias) {
			return;
		}

		RE::ObjectRefHandle handle{};
		if (ref) {
			handle = ref->CreateRefHandle();
		}

		RE::BSWriteLockGuard lock(g_registry.quest->aliasAccessLock);
		auto it = g_registry.quest->refAliasMap.find(alias->aliasID);
		if (ref) {
			if (it != g_registry.quest->refAliasMap.end()) {
				it->second = handle;
			} else {
				g_registry.quest->refAliasMap.insert({ alias->aliasID, handle });
			}
		} else if (it != g_registry.quest->refAliasMap.end()) {
			g_registry.quest->refAliasMap.erase(it);
		}
	}

	void SyncPlayerAlias(RE::Actor* actor, const char* reason)
	{
		ResolveQuestRegistry();
		if (!g_registry.quest || !g_registry.playerCaptiveAlias) {
			spdlog::warn("[TFD][Captive] player captive alias unavailable reason={}", reason ? reason : "unknown");
			return;
		}

		WriteQuestAlias(g_registry.playerCaptiveAlias, actor);
		spdlog::info("[TFD][Captive] PlayerCaptive alias {} actor={:08X} reason={}",
			actor ? "assigned" : "cleared",
			actor ? actor->GetFormID() : 0u,
			reason ? reason : "unknown");
	}

	void SyncStorageDebugAliases(const char* reason)
	{
		ResolveQuestRegistry();
		if (!g_registry.quest) {
			return;
		}

		TFD::Location::CaptiveStorageDebugSnapshot snapshot{};
		const bool hasSnapshot = TFD::Location::GetLastCaptiveStorageDebugSnapshot(snapshot);

		auto assignByFormID = [&](RE::BGSRefAlias* alias, std::uint32_t formID) {
			WriteQuestAlias(alias, LookupRefByFormID(formID));
		};

		for (std::size_t i = 0; i < g_registry.bossCaptorAliases.size(); ++i) {
			assignByFormID(g_registry.bossCaptorAliases[i], hasSnapshot ? snapshot.bossActorFormIDs[i] : 0u);
		}
		for (std::size_t i = 0; i < g_registry.bossContainerAliases.size(); ++i) {
			assignByFormID(g_registry.bossContainerAliases[i], hasSnapshot ? snapshot.bossContainerFormIDs[i] : 0u);
		}
		for (std::size_t i = 0; i < g_registry.containerAliases.size(); ++i) {
			assignByFormID(g_registry.containerAliases[i], hasSnapshot ? snapshot.containerFormIDs[i] : 0u);
		}
		assignByFormID(g_registry.lootTargetAlias, hasSnapshot ? snapshot.finalTargetFormID : 0u);

		spdlog::info("[TFD][Captive] debug aliases synced reason={} hasSnapshot={} boss1={:08X} bossContainer1={:08X} container1={:08X} lootTarget={:08X} targetKind={}",
			reason ? reason : "unknown",
			hasSnapshot ? 1 : 0,
			hasSnapshot ? snapshot.bossActorFormIDs[0] : 0u,
			hasSnapshot ? snapshot.bossContainerFormIDs[0] : 0u,
			hasSnapshot ? snapshot.containerFormIDs[0] : 0u,
			hasSnapshot ? snapshot.finalTargetFormID : 0u,
			hasSnapshot ? snapshot.finalTargetKind : 0u);
	}

	void ClearStorageDebugAliases(const char* reason)
	{
		ResolveQuestRegistry();
		if (!g_registry.quest) {
			return;
		}

		for (auto* alias : g_registry.bossCaptorAliases) {
			WriteQuestAlias(alias, nullptr);
		}
		for (auto* alias : g_registry.bossContainerAliases) {
			WriteQuestAlias(alias, nullptr);
		}
		for (auto* alias : g_registry.containerAliases) {
			WriteQuestAlias(alias, nullptr);
		}
		WriteQuestAlias(g_registry.lootTargetAlias, nullptr);

		spdlog::info("[TFD][Captive] debug aliases cleared reason={}", reason ? reason : "unknown");
	}

	static RE::TESBoundObject* ResolveLockpickItem()
	{
		static RE::TESBoundObject* cached = nullptr;
		static bool resolved = false;
		if (!resolved) {
			resolved = true;
			cached = RE::TESForm::LookupByEditorID<RE::TESBoundObject>("Lockpick");
			if (!cached) {
				spdlog::warn("[TFD][Captive] Lockpick form not found by EditorID");
			}
		}
		return cached;
	}

	static std::int32_t GetReferenceItemCount(RE::TESObjectREFR* ref, RE::TESBoundObject* item)
	{
		if (!ref || !item) {
			return 0;
		}
		const auto inv = ref->GetInventory([item](RE::TESBoundObject& obj) { return &obj == item; }, true);
		auto it = inv.find(item);
		if (it == inv.end()) {
			return 0;
		}
		return (std::max)(0, it->second.first);
	}

	static std::int32_t GetReferenceTotalInventoryCount(RE::TESObjectREFR* ref)
	{
		if (!ref) {
			return 0;
		}
		const auto inv = ref->GetInventory([](RE::TESBoundObject&) { return true; }, true);
		std::int32_t totalUnits = 0;
		for (const auto& [item, invData] : inv) {
			const auto& [count, entry] = invData;
			(void)entry;
			if (!item || count <= 0) {
				continue;
			}
			totalUnits += count;
		}
		return totalUnits;
	}

	static bool IsContainerStorageTarget(RE::TESObjectREFR* target)
	{
		if (!target) {
			return false;
		}
		auto* base = target->GetBaseObject();
		return base && base->As<RE::TESObjectCONT>();
	}

	bool EnsureStarterLockpicks(std::int32_t targetCount, const char* reason)
	{
		auto* player = Player();
		auto* lockpick = ResolveLockpickItem();
		if (!player || !lockpick || targetCount <= 0) {
			return false;
		}

		const auto currentCount = GetReferenceItemCount(player, lockpick);
		if (currentCount >= targetCount) {
			spdlog::info("[TFD][Captive] starter lockpick skipped current={} target={} reason={}", currentCount, targetCount, reason ? reason : "unknown");
			return false;
		}

		const auto addCount = targetCount - currentCount;
		player->AddObjectToContainer(lockpick, nullptr, addCount, nullptr);
		spdlog::info("[TFD][Captive] starter lockpick granted add={} finalTarget={} reason={}", addCount, targetCount, reason ? reason : "unknown");
		return true;
	}

	bool TransferPlayerInventoryToStorage(RE::TESObjectREFR* target, const char* reason)
	{
		struct PendingMove
		{
			RE::TESBoundObject* item{ nullptr };
			std::int32_t count{ 0 };
		};

		auto* player = Player();
		if (!player || !target || target == player) {
			spdlog::info("[TFD][Captive] confiscation skipped player={:08X} target={:08X} reason={}",
				player ? player->GetFormID() : 0u,
				target ? target->GetFormID() : 0u,
				reason ? reason : "unknown");
			return false;
		}

		if (!IsContainerStorageTarget(target)) {
			spdlog::warn("[TFD][Captive] confiscation rejected non-container target={:08X} base={:08X} reason={}",
				target->GetFormID(),
				target->GetBaseObject() ? target->GetBaseObject()->GetFormID() : 0u,
				reason ? reason : "unknown");
			return false;
		}

		const auto inv = player->GetInventory([](RE::TESBoundObject&) { return true; }, true);
		std::vector<PendingMove> pending{};
		pending.reserve(inv.size());

		std::int32_t totalStacks = 0;
		std::int32_t totalUnits = 0;
		for (const auto& [item, invData] : inv) {
			const auto& [count, entry] = invData;
			(void)entry;
			if (!item || count <= 0) {
				continue;
			}
			pending.push_back(PendingMove{ item, count });
			++totalStacks;
			totalUnits += count;
		}

		if (pending.empty() || totalUnits <= 0) {
			spdlog::info("[TFD][Captive] confiscation skipped empty_inventory target={:08X} reason={}", target->GetFormID(), reason ? reason : "unknown");
			return false;
		}

		std::int32_t removedUnits = 0;
		std::int32_t addedUnits = 0;
		std::int32_t movedStacks = 0;
		std::int32_t partialStacks = 0;
		std::int32_t failedStacks = 0;

		for (const auto& move : pending) {
			auto* item = move.item;
			if (!item || move.count <= 0) {
				continue;
			}

			const auto beforePlayer = GetReferenceItemCount(player, item);
			if (beforePlayer <= 0) {
				continue;
			}

			const auto requestCount = (std::min)(move.count, beforePlayer);
			const auto beforeTarget = GetReferenceItemCount(target, item);

			player->RemoveItem(item, requestCount, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, target);

			const auto afterPlayer = GetReferenceItemCount(player, item);
			const auto afterTarget = GetReferenceItemCount(target, item);

			const auto removedNow = (std::max)(0, beforePlayer - afterPlayer);
			const auto addedNow = (std::max)(0, afterTarget - beforeTarget);

			removedUnits += removedNow;
			addedUnits += addedNow;

			if (removedNow == requestCount && addedNow == requestCount) {
				++movedStacks;
				continue;
			}

			if (removedNow > 0 || addedNow > 0) {
				++partialStacks;
			} else {
				++failedStacks;
			}

			spdlog::warn("[TFD][Captive] confiscation stack mismatch item={:08X} requested={} removed={} added={} beforePlayer={} afterPlayer={} beforeTarget={} afterTarget={} reason={}",
				item->GetFormID(), requestCount, removedNow, addedNow, beforePlayer, afterPlayer, beforeTarget, afterTarget, reason ? reason : "unknown");
		}

		const auto playerUnitsAfter = GetReferenceTotalInventoryCount(player);
		spdlog::info("[TFD][Captive] confiscation moved_manual target={:08X} stacks={} units={} movedStacks={} partialStacks={} failedStacks={} removedUnits={} addedUnits={} playerUnitsAfter={} reason={}",
			target->GetFormID(), totalStacks, totalUnits, movedStacks, partialStacks, failedStacks, removedUnits, addedUnits, playerUnitsAfter, reason ? reason : "unknown");
		return removedUnits > 0 || addedUnits > 0;
	}

	void ClearPendingConfiscation(const char* reason)
	{
		const bool hadPending = g_confiscationPending || g_starterLockpickPending || !g_pendingConfiscationReason.empty();
		g_confiscationPending = false;
		g_starterLockpickPending = false;
		g_pendingConfiscationReason.clear();
		g_confiscationAttemptCount = 0;
		g_confiscationNextAttempt = {};
		if (hadPending) {
			spdlog::info("[TFD][Captive] confiscation pending cleared reason={}", reason ? reason : "unknown");
		}
	}

	void QueuePendingConfiscation(const char* reason, bool starterKitWanted)
	{
		g_confiscationPending = true;
		g_starterLockpickPending = starterKitWanted;
		g_pendingConfiscationReason = reason ? reason : "unknown";
		g_confiscationAttemptCount = 0;
		g_confiscationNextAttempt = Now() + std::chrono::milliseconds(kCaptiveConfiscationInitialDelayMs);
		spdlog::info("[TFD][Captive] confiscation pending queued starterKitWanted={} delayMs={} reason={}",
			starterKitWanted ? 1 : 0,
			kCaptiveConfiscationInitialDelayMs,
			g_pendingConfiscationReason.c_str());
	}

	void ProcessPendingConfiscation()
	{
		if (!g_confiscationPending) {
			return;
		}
		if (g_confiscationApplied) {
			ClearPendingConfiscation("already_applied");
			return;
		}
		if (!g_state || g_phase != PhaseValue::Captive) {
			ClearPendingConfiscation("not_in_captive_phase");
			return;
		}
		if (Now() < g_confiscationNextAttempt) {
			return;
		}

		auto* ui = RE::UI::GetSingleton();
		if (ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
			g_confiscationNextAttempt = Now() + std::chrono::milliseconds(kCaptiveConfiscationRetryDelayMs);
			return;
		}

		auto* player = Player();
		auto* marker = TFD::Location::GetCachedCaptiveMarker();
		auto* playerCell = player ? player->GetParentCell() : nullptr;
		auto* markerCell = marker ? marker->GetParentCell() : nullptr;
		if (!player || !playerCell || !marker || !markerCell || playerCell != markerCell) {
			g_confiscationNextAttempt = Now() + std::chrono::milliseconds(kCaptiveConfiscationRetryDelayMs);
			return;
		}

		const char* reason = g_pendingConfiscationReason.empty() ? "unknown" : g_pendingConfiscationReason.c_str();
		++g_confiscationAttemptCount;
		spdlog::info("[TFD][Captive] confiscation pending attempt={} cell={:08X} marker={:08X} reason={}",
			g_confiscationAttemptCount,
			playerCell->GetFormID(),
			marker->GetFormID(),
			reason);

		auto* storage = TFD::Location::ResolveNearestCaptiveStorageTarget(nullptr);
		SyncStorageDebugAliases(reason);
		if (!storage) {
			if (g_confiscationAttemptCount >= kCaptiveConfiscationMaxAttempts) {
				spdlog::warn("[TFD][Captive] confiscation aborted no_container_target attempts={} reason={}", g_confiscationAttemptCount, reason);
				ClearPendingConfiscation("no_container_target");
			} else {
				g_confiscationNextAttempt = Now() + std::chrono::milliseconds(kCaptiveConfiscationRetryDelayMs);
			}
			return;
		}

		if (!IsContainerStorageTarget(storage)) {
			if (g_confiscationAttemptCount >= kCaptiveConfiscationMaxAttempts) {
				spdlog::warn("[TFD][Captive] confiscation aborted non_container_target target={:08X} attempts={} reason={}",
					storage->GetFormID(), g_confiscationAttemptCount, reason);
				ClearPendingConfiscation("non_container_target");
			} else {
				g_confiscationNextAttempt = Now() + std::chrono::milliseconds(kCaptiveConfiscationRetryDelayMs);
			}
			return;
		}

		const bool moved = TransferPlayerInventoryToStorage(storage, reason);
		g_confiscationApplied = true;
		if (moved && g_starterLockpickPending) {
			EnsureStarterLockpicks(3, reason);
		}
		ClearPendingConfiscation(moved ? "completed" : "completed_no_items");
	}

	void SetRuntimeState(bool stateActive, PhaseValue phase)
	{
		g_state = stateActive;
		g_phase = phase;
		if (!stateActive) {
			SyncPlayerAlias(nullptr, "captive_exit");
			ClearStorageDebugAliases("captive_exit");
		}
		if (!stateActive || phase != PhaseValue::Captive) {
			g_confiscationApplied = false;
			ClearPendingConfiscation("captive_state_reset");
		}
	}

	void SetPrevLockpickOpen(bool open)
	{
		g_prevLockpickOpen = open;
	}

	void CaptureCurrentLockpickMenuState()
	{
		auto* ui = RE::UI::GetSingleton();
		g_prevLockpickOpen = ui && ui->IsMenuOpen(RE::LockpickingMenu::MENU_NAME);
	}

	void ResetLockpickWatch()
	{
		g_lockpickDoorCandidate.reset();
		g_lockpickDoorWasLocked = false;
		g_prevLockpickOpen = false;
	}

	RE::TESObjectREFR* ResolveBoundEscapeDoor()
	{
		if (!g_boundEscapeDoor) return nullptr;
		auto ptr = g_boundEscapeDoor.get();
		return ptr.get();
	}

	void BindDoor(RE::TESObjectREFR* door)
	{
		if (!door) return;
		g_door.Bind(door);
		g_boundEscapeDoor = door->GetHandle();
	}

	static RE::TESObjectREFR* FindNearestDoorNearCaptiveMarker()
	{
		RE::TESObjectREFR* marker = nullptr;
		if (g_marker) {
			auto ptr = g_marker.get();
			marker = ptr.get();
		}
		if (!marker) marker = TFD::Location::GetCachedCaptiveMarker();
		if (!marker) return nullptr;
		auto* cell = marker->GetParentCell();
		if (!cell) return nullptr;
		const auto mp = marker->GetPosition();
		RE::TESObjectREFR* best = nullptr;
		double bestDistSq = kCaptiveEscapeDoorRadius * kCaptiveEscapeDoorRadius;
		cell->ForEachReferenceInRange(mp, static_cast<float>(kCaptiveEscapeDoorRadius), [&](RE::TESObjectREFR* candidate) -> RE::BSContainer::ForEachResult {
			if (!candidate || candidate == marker) return RE::BSContainer::ForEachResult::kContinue;
			if (!IsDoorRef(candidate)) return RE::BSContainer::ForEachResult::kContinue;
			const auto rp = candidate->GetPosition();
			const double dx = static_cast<double>(rp.x - mp.x);
			const double dy = static_cast<double>(rp.y - mp.y);
			const double dz = static_cast<double>(rp.z - mp.z);
			const double distSq = dx * dx + dy * dy + dz * dz;
			if (distSq <= bestDistSq) {
				bestDistSq = distSq;
				best = candidate;
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		return best;
	}

	void ClearEscapeContext()
	{
		g_marker.reset();
		g_cellFormID = 0;
		g_locationFormID = 0;
		g_escapeRadiusActive = false;
		g_escapeRadiusSince = {};
		g_boundEscapeDoor.reset();
		g_door.Reset();
	}

	void ArmEscapeContextFromCurrentState(RE::Actor* player)
	{
		if (!player) return;
		auto* marker = TFD::Location::GetCachedCaptiveMarker();
		if (!marker) {
			TFD::Location::RescanCaptiveMarker();
			marker = TFD::Location::GetCachedCaptiveMarker();
		}
		g_marker = marker ? marker->GetHandle() : RE::ObjectRefHandle{};
		auto* cell = player->GetParentCell();
		g_cellFormID = cell ? cell->GetFormID() : 0;
		auto* loc = cell ? cell->GetLocation() : nullptr;
		g_locationFormID = loc ? loc->GetFormID() : 0;
		g_escapeRadiusActive = false;
		g_escapeRadiusSince = {};
		if (!g_door.HasDoor()) {
			if (auto* door = FindNearestDoorNearCaptiveMarker()) {
				BindDoor(door);
				spdlog::info("[TFD][Captive] Bound nearest captive door {:08X} on captive enter", door->GetFormID());
			}
		}
		spdlog::info("[TFD][Captive] Escape context armed marker={:08X} cell={:08X} loc={:08X}", marker ? marker->GetFormID() : 0, g_cellFormID, g_locationFormID);
	}

	bool IsDoorNearMarker(RE::TESObjectREFR* door)
	{
		if (!door || !IsDoorRef(door)) return false;
		RE::TESObjectREFR* marker = nullptr;
		if (g_marker) {
			auto ptr = g_marker.get();
			marker = ptr.get();
		}
		if (!marker) marker = TFD::Location::GetCachedCaptiveMarker();
		if (!marker) return true;
		const auto dp = door->GetPosition();
		const auto mp = marker->GetPosition();
		const double dx = static_cast<double>(dp.x - mp.x);
		const double dy = static_cast<double>(dp.y - mp.y);
		const double dz = static_cast<double>(dp.z - mp.z);
		const double distSq = dx * dx + dy * dy + dz * dz;
		return distSq <= (kCaptiveEscapeDoorRadius * kCaptiveEscapeDoorRadius);
	}

	RE::TESObjectREFR* ResolveLockpickDoorCandidate(RE::TESObjectREFR* target)
	{
		if (target && IsDoorRef(target) && IsDoorNearMarker(target)) return target;
		auto* boundDoor = ResolveBoundEscapeDoor();
		if (boundDoor && IsDoorRef(boundDoor) && IsDoorNearMarker(boundDoor)) return boundDoor;
		return nullptr;
	}

	bool UpdateLockpickEscapeWatch(const std::function<void(const char*, RE::TESObjectREFR*)>& onEscapeCommit)
	{
		if (!g_state || g_phase != PhaseValue::Captive) {
			ResetLockpickWatch();
			return false;
		}

		auto* ui = RE::UI::GetSingleton();
		const bool lockOpen = ui && ui->IsMenuOpen(RE::LockpickingMenu::MENU_NAME);
		if (lockOpen && !g_prevLockpickOpen) {
			auto* rawTarget = RE::LockpickingMenu::GetTargetReference();
			auto* target = ResolveLockpickDoorCandidate(rawTarget);
			if (target) {
				g_lockpickDoorCandidate = target->GetHandle();
				g_lockpickDoorWasLocked = IsRefLocked(target);
				if (g_lockpickDoorWasLocked) BindDoor(target);
				spdlog::info("[TFD][Captive] lockpick opened on door {:08X} wasLocked={} nearMarker=1 rawTarget={:08X}", target->GetFormID(), g_lockpickDoorWasLocked ? 1 : 0, rawTarget ? rawTarget->GetFormID() : 0);
			} else {
				g_lockpickDoorCandidate.reset();
				g_lockpickDoorWasLocked = false;
				auto* boundDoor = ResolveBoundEscapeDoor();
				if (rawTarget) {
					spdlog::info("[TFD][Captive] lockpick target {:08X} ignored (door={} nearMarker={} fallbackBoundDoor={:08X})", rawTarget->GetFormID(), IsDoorRef(rawTarget) ? 1 : 0, IsDoorNearMarker(rawTarget) ? 1 : 0, boundDoor ? boundDoor->GetFormID() : 0);
				} else {
					spdlog::info("[TFD][Captive] lockpick target null (fallbackBoundDoor={:08X})", boundDoor ? boundDoor->GetFormID() : 0);
				}
			}
		}

		if (g_door.HasDoor() && g_door.UpdateWatcher()) {
			if (onEscapeCommit) {
				onEscapeCommit("door_watch", nullptr);
			}
			g_prevLockpickOpen = lockOpen;
			return true;
		}

		if (!lockOpen && g_prevLockpickOpen) {
			RE::TESObjectREFR* door = nullptr;
			if (g_lockpickDoorCandidate) {
				auto ptr = g_lockpickDoorCandidate.get();
				door = ptr.get();
			}
			if (!door) door = ResolveBoundEscapeDoor();
			if (door && IsDoorRef(door) && g_lockpickDoorWasLocked && !IsRefLocked(door)) {
				if (onEscapeCommit) {
					onEscapeCommit("lockpick", door);
				}
				g_prevLockpickOpen = lockOpen;
				return true;
			}
			ResetLockpickWatch();
		}
		g_prevLockpickOpen = lockOpen;
		return false;
	}

	bool TryCommitEscapeByRadius(RE::Actor* player)
	{
		if (!g_state || g_phase != PhaseValue::Captive) {
			g_escapeRadiusActive = false;
			return false;
		}
		if (!player) return false;
		RE::TESObjectREFR* marker = nullptr;
		if (g_marker) {
			auto ptr = g_marker.get();
			marker = ptr.get();
		}
		if (!marker) marker = TFD::Location::GetCachedCaptiveMarker();
		if (!marker) return false;
		const auto pp = player->GetPosition();
		const auto mp = marker->GetPosition();
		const double dx = static_cast<double>(pp.x - mp.x);
		const double dy = static_cast<double>(pp.y - mp.y);
		const double dz = static_cast<double>(pp.z - mp.z);
		const double distSq = dx * dx + dy * dy + dz * dz;
		const bool outside = distSq > (512.0 * 512.0);
		if (!outside) {
			g_escapeRadiusActive = false;
			return false;
		}
		if (!g_escapeRadiusActive) {
			g_escapeRadiusActive = true;
			g_escapeRadiusSince = Now();
			return false;
		}
		const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(Now() - g_escapeRadiusSince).count();
		if (held >= 2000) {
			g_escapeRadiusActive = false;
			return true;
		}
		return false;
	}

	bool DidEscapeByLocation(RE::Actor* player, RE::FormID* oldLocationOut, RE::FormID* newLocationOut)
	{
		if (!g_state || g_phase != PhaseValue::Escape || !player) {
			return false;
		}
		auto* loc = TFD::Location::GetLocationFromRef(player);
		const auto curLoc = loc ? loc->GetFormID() : 0u;
		if (oldLocationOut) *oldLocationOut = g_locationFormID;
		if (newLocationOut) *newLocationOut = curLoc;
		return g_locationFormID != 0 && curLoc != 0 && curLoc != g_locationFormID;
	}

	bool NormalizeInvalidCaptivePair()
	{
		if (!g_state || g_phase != PhaseValue::None) {
			return false;
		}
		SetRuntimeState(true, PhaseValue::Escape);
		(void)TFD::FlowController::Controller::GetSingleton().ResolveCaptiveOutcome(TFD::FlowController::CaptiveOutcome::EscapeStarted, 0u, "normalize_invalid_captive_pair");
		spdlog::info("[TFD][Captive] Normalized invalid captive pair -> Escape");
		return true;
	}

	static bool EnterEscapeCommit(RE::Actor* player, const char* reason, RE::TESObjectREFR* door, const EscapeTickHandlers& handlers)
	{
		if (!g_state || g_phase != PhaseValue::Captive) {
			return false;
		}

		TFD::FactionManager::Clear();
		TFD::HostilityController::ClearAggressionClamp();
		if (handlers.setGraceActive) {
			handlers.setGraceActive(false);
		}
		SetRuntimeState(true, PhaseValue::Escape);
		(void)TFD::FlowController::Controller::GetSingleton().ResolveCaptiveOutcome(TFD::FlowController::CaptiveOutcome::EscapeStarted, 0u, reason ? reason : "escape_started");
		if (handlers.updatePreCombatState) {
			handlers.updatePreCombatState();
		}
		ResetLockpickWatch();
		if (handlers.setPrevDialogueOpen) {
			handlers.setPrevDialogueOpen(false);
		}
		if (door) {
			BindDoor(door);
		} else {
			door = ResolveBoundEscapeDoor();
		}
		if (player) {
			player->EvaluatePackage(true, false);
		}

		RE::Actor* aggressor = handlers.resolveAggressor ? handlers.resolveAggressor() : nullptr;
		if (!aggressor && handlers.findBestAggressor) {
			const float radius = (std::max)(1800.0f, TFD::Settings::GetSweepRadius());
			aggressor = handlers.findBestAggressor(radius);
			if (aggressor && handlers.setLastAggressor) {
				handlers.setLastAggressor(aggressor);
			}
		}
		if (aggressor) {
			aggressor->EvaluatePackage(true, false);
			spdlog::info("[TFD][Captive] Escape aggro nudge actor={:08X}", aggressor->GetFormID());
		} else {
			spdlog::info("[TFD][Captive] Escape aggro nudge skipped (no aggressor)");
		}

		spdlog::info("[TFD][Captive] EscapeCommit reason={} door={:08X}", reason ? reason : "unknown", door ? door->GetFormID() : 0);
		return true;
	}

	bool TickCaptiveEscapePhase(RE::Actor* player, const EscapeTickHandlers& handlers)
	{
		if (!g_state || g_phase != PhaseValue::Captive) {
			return false;
		}

		if (UpdateLockpickEscapeWatch(
				[&](const char* reason, RE::TESObjectREFR* door) {
					(void)EnterEscapeCommit(player, reason, door, handlers);
				})) {
			return true;
		}

		if (TryCommitEscapeByRadius(player)) {
			(void)EnterEscapeCommit(player, "marker_radius", nullptr, handlers);
			return true;
		}

		return false;
	}

	bool TickEscapeActivePhase(RE::Actor* player, const EscapeTickHandlers& handlers)
	{
		if (!g_state || g_phase != PhaseValue::Escape) {
			return true;
		}
		if (!player) {
			return false;
		}

		RE::FormID oldLoc = 0;
		RE::FormID newLoc = 0;
		if (DidEscapeByLocation(player, &oldLoc, &newLoc)) {
			SetRuntimeState(false, PhaseValue::None);
			(void)TFD::FlowController::Controller::GetSingleton().ResolveCaptiveOutcome(TFD::FlowController::CaptiveOutcome::EscapeSucceeded, 0u, "escape_resolved_location");
			(void)TFD::FlowController::Controller::GetSingleton().CompleteTerminalContext("escape_resolved_location");
			ClearEscapeContext();
			if (handlers.updatePreCombatState) {
				handlers.updatePreCombatState();
			}
			spdlog::info("[TFD][Captive] EscapeResolved by location old={:08X} new={:08X}", oldLoc, newLoc);
			return true;
		}

		const float hpNow = player->GetActorValue(RE::ActorValue::kHealth);
		const float hpMax = (std::max)(1.0f, player->GetPermanentActorValue(RE::ActorValue::kHealth));
		const float pct = (hpNow / hpMax) * 100.0f;
		const float thresh = TFD::Settings::GetDefeatThresholdPct();
		if (pct > thresh) {
			return false;
		}

		RE::Actor* preferred = handlers.resolveAggressor ? handlers.resolveAggressor() : nullptr;
		if (!preferred && handlers.findBestAggressor) {
			const float reacquireRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius());
			preferred = handlers.findBestAggressor(reacquireRadius);
		}

		(void)TFD::FlowController::Controller::GetSingleton().ResolveCaptiveOutcome(TFD::FlowController::CaptiveOutcome::EscapeFailed, preferred ? preferred->GetFormID() : 0u, "escape_broken_threshold");
		QueueEscapeBreakRebleed(preferred);
		if (handlers.clearLastAggressor) {
			handlers.clearLastAggressor();
		}
		SetRuntimeState(true, PhaseValue::Captive);
		ClearEscapeContext();
		if (handlers.updatePreCombatState) {
			handlers.updatePreCombatState();
		}
		spdlog::info("[TFD][Captive] Escape broken by defeat threshold pct={:.1f} thresh={:.1f} -> revert to captive and schedule rebleed preferred={:08X}", pct, thresh, preferred ? preferred->GetFormID() : 0);
		return true;
	}

	bool TriggerPlayerAggressionEscape(RE::Actor* actor, const char* reason)
	{
		if (!g_state) {
			return false;
		}
		SetRuntimeState(true, PhaseValue::Escape);
		(void)TFD::FlowController::Controller::GetSingleton().ResolveCaptiveOutcome(TFD::FlowController::CaptiveOutcome::EscapeStarted, actor ? actor->GetFormID() : 0u, reason ? reason : "player_aggression_captive");
		return true;
	}

	bool BeginCaptorCallHotkey(RE::Actor* player, RE::Actor** outCaptor)
	{
		if (outCaptor) {
			*outCaptor = nullptr;
		}
		if (!player) {
			return false;
		}

		auto* captor = PickCaptorSameCellLoaded(player, 12288.0f);
		if (!captor) {
			return false;
		}

		ApplyCallCaptorCalmBubble(player, captor, 12288.0f);
		if (outCaptor) {
			*outCaptor = captor;
		}
		return true;
	}

	void ResetForLoad()
	{
		g_state = false;
		g_phase = PhaseValue::None;
		g_confiscationApplied = false;
		g_confiscationPending = false;
		g_starterLockpickPending = false;
		g_pendingConfiscationReason.clear();
		g_confiscationAttemptCount = 0;
		g_confiscationNextAttempt = {};
		g_registry = {};
		g_door.Reset();
		g_marker.reset();
		g_cellFormID = 0;
		g_locationFormID = 0;
		g_queuedState = false;
		g_queuedPhase = PhaseValue::None;
		g_escapeBreakBleedPending = false;
		g_escapeBreakPreferredAggressor.reset();
		g_prevLockpickOpen = false;
		g_escapeRadiusActive = false;
		g_escapeRadiusSince = {};
		g_boundEscapeDoor.reset();
		g_lockpickDoorCandidate.reset();
		g_lockpickDoorWasLocked = false;
	}
}
