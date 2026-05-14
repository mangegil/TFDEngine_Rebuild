#include "TFDCaptive.h"
#include "TFDCaptiveGreet.h"
#include "TFDActor.h"

#include "TFDHostilityController.h"
#include "TFDTame.h"
#include "TFDSettings.h"
#include "TFDLocation.h"

#include "TFDFlowController.h"
#include "TFDInteractionRouter.h"
#include "TFDPleasureRuntime.h"
#include "TFDTransition.h"
#include "TFDDefeatMonitor.h"

#include <SKSE/SKSE.h>

#include <RE/L/LockpickingMenu.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <vector>

namespace TFD::Captive
{
	void EnsureCaptiveNavigationContext(RE::Actor* player, const char* reason);

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
			auto snapshot = TFD::Actor::BuildSnapshot(searchRadius, false);

			RE::Actor* best = nullptr;
			float bestScore = 1.0e30f;
			std::uint32_t considered = 0;
			std::uint32_t sameSpace = 0;
			std::uint32_t supported = 0;
			std::uint32_t suppressed = 0;
			std::uint32_t losCount = 0;

			for (const auto& info : snapshot.actors) {
				auto* a = info.get();
				if (!a) {
					continue;
				}
				++considered;
				if (!IsCaptorSupportedActor(a)) {
					continue;
				}
				++supported;
				if (!IsActorSameSpace(a, player)) {
					continue;
				}
				++sameSpace;
				if (info.dist > searchRadius) {
					continue;
				}

				const bool hasLOS = ActorHasLOS(a, player);
				if (hasLOS) {
					++losCount;
				}
				const bool isSuppressed = TFD::HostilityController::IsActorTemporarilySuppressed(a);
				if (isSuppressed) {
					++suppressed;
				}
				const bool hostile = info.hostileToPlayer || a->IsHostileToActor(player);
				const bool combat = info.inCombat || a->IsInCombat();

				// Calling Captor is a captive-camp request.  A caged player can have
				// a valid captor directly in front of the cell while LOS is blocked by
				// bars, doors, or collision.  Prefer actors already held by captive
				// suppression, then LOS/hostile/combat actors, but do not hard-require
				// LOS for the hotkey candidate.
				if (!isSuppressed && !hasLOS && !hostile && !combat) {
					continue;
				}

				float score = info.dist;
				if (isSuppressed) {
					score -= 10000.0f;
				}
				if (hasLOS) {
					score -= 3000.0f;
				}
				if (hostile) {
					score -= 140.0f;
				}
				if (combat) {
					score -= 100.0f;
				}

				if (score < bestScore) {
					bestScore = score;
					best = a;
				}
			}

			if (best) {
				spdlog::info("[TFD][Captive] Call Captor candidate selected actor={:08X} distScore={:.1f} considered={} supported={} sameSpace={} suppressed={} los={}",
					best->GetFormID(),
					bestScore,
					considered,
					supported,
					sameSpace,
					suppressed,
					losCount);
			} else {
				spdlog::warn("[TFD][Captive] Call Captor no candidate considered={} supported={} sameSpace={} suppressed={} los={} radius={:.1f}",
					considered,
					supported,
					sameSpace,
					suppressed,
					losCount,
					searchRadius);
			}

			return best;
		}

		static void SendBridgeAssignActor(const char* eventName, RE::Actor* actor, const char* reason = "")
		{
			if (!eventName || !actor) {
				return;
			}

			auto* src = SKSE::GetModCallbackEventSource();
			if (!src) {
				return;
			}

			SKSE::ModCallbackEvent e(eventName, reason ? reason : "", 0.0f, actor);
			src->SendEvent(&e);
		}

		static void SendBridgeFormEvent(const char* eventName, RE::TESForm* sender, const char* reason = "", float numArg = 0.0f)
		{
			if (!eventName || !sender) {
				return;
			}

			auto* src = SKSE::GetModCallbackEventSource();
			if (!src) {
				return;
			}

			SKSE::ModCallbackEvent e(eventName, reason ? reason : "", numArg, sender);
			src->SendEvent(&e);
		}

		static void ApplyNativeCaptorRoleFaction(RE::Actor* actor, const char* reason);
		static void ApplyNativeCaptiveLocationRoleFactions(RE::Actor* player, const char* reason, bool force);

		static void ApplyCallCaptorCalmBubble(RE::Actor* player, RE::Actor* primaryTarget, float radius)
		{
			if (!player || !primaryTarget) {
				return;
			}

			const float sweepRadius = (std::max)(radius, (std::max)(TFD::Settings::GetSweepRadius(), 12000.0f));
			TFD::HostilityController::StopCombatSweep(sweepRadius, true);
			TFD::HostilityController::ScheduleStopCombatWaves(sweepRadius, true, 10, 120);
			auto snapshot = TFD::Actor::BuildSnapshot(sweepRadius, false);

			auto* pCell = player->GetParentCell();
			for (const auto& info : snapshot.actors) {
				auto* actor = info.get();
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
				if (actor->GetFormID() != primaryTarget->GetFormID() && !info.hostileToPlayer && !info.inCombat) {
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

			// Do not clear the whole captive bridge here.  ClearAll also wipes
			// CaptiveMarker, EscapeDoor, and LootTarget aliases, which can break
			// the active captive cycle and recover-item objective.  AssignActor
			// is allowed to replace only OwnerCaptor.
			//
			// TFDCaptiveFaction is a dialogue-condition role for every eligible
			// actor in the active captive location, not just OwnerCaptor.  CK
			// dialogue can then distinguish captive-location actors from normal
			// hostiles while native still owns forcegreet/session routing.
			ApplyNativeCaptiveLocationRoleFactions(player, "call_captor_hotkey", true);
			ApplyNativeCaptorRoleFaction(primaryTarget, "call_captor_hotkey_primary");
			SendBridgeAssignActor("TFDCaptiveAssign", primaryTarget, "call_captor_hotkey");
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

		static constexpr auto kCaptorCallCooldown = std::chrono::milliseconds(5000);
		static std::chrono::steady_clock::time_point g_captorCallCooldownUntil{};

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
		bool g_playerCaptiveAliasMissingLogged = false;
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

		bool g_escapeBleedoutActive = false;
		bool g_recaptureCommitActive = false;
		std::chrono::steady_clock::time_point g_recaptureCommitStarted{};
		std::chrono::steady_clock::time_point g_lastRecaptureCompleted{};
		std::uint32_t g_lastRecaptureActorID = 0;
		std::uint32_t g_stashCycleID = 0;
		struct NativeCaptiveRoleEntry
		{
			RE::ActorHandle actor{};
			RE::FormID formID = 0;
			bool addedByTFD = false;
		};

		RE::ActorHandle g_nativeCaptorRoleActor{};
		bool g_nativeCaptorRoleFactionAdded = false;
		std::vector<NativeCaptiveRoleEntry> g_nativeCaptiveRoleActors{};
		std::chrono::steady_clock::time_point g_nextCaptiveRoleFactionSweep{};
		static constexpr auto kCaptiveRoleFactionSweepInterval = std::chrono::milliseconds(1500);
		static constexpr auto kRecaptureDuplicateGuard = std::chrono::milliseconds(8000);

		static std::uint32_t ActorFormID(RE::Actor* actor)
		{
			return actor ? actor->GetFormID() : 0u;
		}

		static RE::TESFaction* ResolveNativeCaptiveFaction()
		{
			static RE::TESFaction* cached = nullptr;
			static bool resolved = false;
			if (!resolved) {
				resolved = true;
				cached = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDCaptiveFaction");
				if (!cached) {
					spdlog::warn("[TFD][Captive] native captor role faction missing editorId=TFDCaptiveFaction");
				}
			}
			return cached;
		}

		static RE::Actor* ResolveNativeCaptorRoleActor()
		{
			if (!g_nativeCaptorRoleActor) {
				return nullptr;
			}
			auto sp = RE::Actor::LookupByHandle(g_nativeCaptorRoleActor.native_handle());
			if (!sp) {
				g_nativeCaptorRoleActor.reset();
				g_nativeCaptorRoleFactionAdded = false;
				return nullptr;
			}
			return sp.get();
		}

		static bool IsNativeCaptiveFactionTracked(RE::FormID formID)
		{
			if (formID == 0) {
				return false;
			}
			for (const auto& entry : g_nativeCaptiveRoleActors) {
				if (entry.formID == formID) {
					return true;
				}
			}
			return false;
		}

		static void ClearNativeCaptorRoleFaction(const char* reason)
		{
			auto* faction = ResolveNativeCaptiveFaction();
			std::uint32_t removed = 0;
			std::uint32_t preserved = 0;
			std::uint32_t stale = 0;

			if (faction) {
				for (auto& entry : g_nativeCaptiveRoleActors) {
					RE::Actor* actor = nullptr;
					if (entry.actor) {
						auto sp = RE::Actor::LookupByHandle(entry.actor.native_handle());
						actor = sp ? sp.get() : nullptr;
					}
					if (!actor && entry.formID != 0) {
						actor = RE::TESForm::LookupByID<RE::Actor>(entry.formID);
					}
					if (!actor) {
						++stale;
						continue;
					}

					if (entry.addedByTFD && actor->IsInFaction(faction)) {
						actor->RemoveFromFaction(faction);
						++removed;
					} else {
						++preserved;
					}
				}
			}

			g_nativeCaptiveRoleActors.clear();
			g_nativeCaptorRoleActor.reset();
			g_nativeCaptorRoleFactionAdded = false;
			g_nextCaptiveRoleFactionSweep = {};

			spdlog::info("[TFD][Captive] native captive location role faction cleared removed={} preserved={} stale={} reason={}",
				removed,
				preserved,
				stale,
				reason ? reason : "unknown");
		}

		static void ApplyNativeCaptorRoleFaction(RE::Actor* actor, const char* reason)
		{
			if (!actor) {
				return;
			}

			auto* faction = ResolveNativeCaptiveFaction();
			if (!faction) {
				return;
			}

			const RE::FormID formID = actor->GetFormID();
			const bool hadFaction = actor->IsInFaction(faction);
			if (!hadFaction) {
				actor->AddToFaction(faction, 0);
			}

			if (!IsNativeCaptiveFactionTracked(formID)) {
				NativeCaptiveRoleEntry entry{};
				entry.actor = actor->GetHandle();
				entry.formID = formID;
				entry.addedByTFD = !hadFaction;
				g_nativeCaptiveRoleActors.push_back(entry);
			}

			g_nativeCaptorRoleActor = actor->GetHandle();
			g_nativeCaptorRoleFactionAdded = !hadFaction;
			spdlog::info("[TFD][Captive] native captive role faction applied actor={:08X} added={} hadFaction={} tracked={} reason={}",
				actor->GetFormID(),
				(!hadFaction) ? 1 : 0,
				hadFaction ? 1 : 0,
				static_cast<unsigned>(g_nativeCaptiveRoleActors.size()),
				reason ? reason : "unknown");
		}

		static bool IsActorInNativeCaptiveLocationScope(RE::Actor* actor, RE::Actor* player)
		{
			if (!actor || !player) {
				return false;
			}

			if (g_locationFormID != 0) {
				auto* cell = actor->GetParentCell();
				auto* loc = cell ? cell->GetLocation() : nullptr;
				if (loc && loc->GetFormID() == g_locationFormID) {
					return true;
				}
			}

			return IsActorSameSpace(actor, player);
		}

		static void ApplyNativeCaptiveLocationRoleFactions(RE::Actor* player, const char* reason, bool force)
		{
			if (!g_state || !player) {
				return;
			}

			const auto now = Now();
			if (!force && g_nextCaptiveRoleFactionSweep != std::chrono::steady_clock::time_point{} && now < g_nextCaptiveRoleFactionSweep) {
				return;
			}
			g_nextCaptiveRoleFactionSweep = now + kCaptiveRoleFactionSweepInterval;

			EnsureCaptiveNavigationContext(player, reason ? reason : "native_captive_faction_sweep");

			const float sweepRadius = (std::max)(TFD::Settings::GetSweepRadius(), 12000.0f);
			auto snapshot = TFD::Actor::BuildSnapshot(sweepRadius, false);
			std::uint32_t applied = 0;
			std::uint32_t considered = 0;
			std::uint32_t inScope = 0;

			for (const auto& info : snapshot.actors) {
				auto* actor = info.get();
				if (!actor) {
					continue;
				}
				++considered;
				if (!IsCaptorSupportedActor(actor)) {
					continue;
				}
				if (!IsActorInNativeCaptiveLocationScope(actor, player)) {
					continue;
				}
				++inScope;
				ApplyNativeCaptorRoleFaction(actor, reason ? reason : "native_captive_location");
				++applied;
			}

			spdlog::info("[TFD][Captive] native captive location faction sweep applied={} inScope={} considered={} tracked={} loc={:08X} reason={}",
				applied,
				inScope,
				considered,
				static_cast<unsigned>(g_nativeCaptiveRoleActors.size()),
				g_locationFormID,
				reason ? reason : "unknown");
		}
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
		return g_state && g_phase == PhaseValue::Captive;
	}

	bool IsCaptorCallCooldownActive(float* remainingSeconds)
	{
		const auto now = Now();
		if (g_captorCallCooldownUntil <= now) {
			if (remainingSeconds) {
				*remainingSeconds = 0.0f;
			}
			return false;
		}

		if (remainingSeconds) {
			*remainingSeconds = std::chrono::duration<float>(g_captorCallCooldownUntil - now).count();
		}
		return true;
	}

	void ClearCaptorCallCooldown()
	{
		g_captorCallCooldownUntil = {};
	}

	bool IsCaptivePassiveHoldActive()
	{
		return g_state && (g_phase == PhaseValue::Captive || g_phase == PhaseValue::ReleasedWork || g_phase == PhaseValue::Scene);
	}

	bool IsEscapeBleedoutActive()
	{
		return g_escapeBleedoutActive;
	}

	bool IsRecaptureCommitActive()
	{
		return g_recaptureCommitActive;
	}

	bool IsRecaptureRecentlyCommitted()
	{
		return g_lastRecaptureCompleted.time_since_epoch().count() != 0 &&
			(Now() - g_lastRecaptureCompleted) < kRecaptureDuplicateGuard;
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
			if (aliasName == "CaptiveMarker") {
				g_registry.captiveMarkerAlias = refAlias;
				continue;
			}
			if (aliasName == "EscapeDoor") {
				g_registry.escapeDoorAlias = refAlias;
				continue;
			}
			if (aliasName == "LootTarget") {
				g_registry.lootTargetAlias = refAlias;
				continue;
			}
			if (aliasName == "OwnerCaptor") {
				g_registry.bossCaptorAliases[0] = refAlias;
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

		spdlog::info("[TFD][Captive] captive quest registry resolved quest={:08X} playerAliasID={} captiveMarkerAlias={} escapeDoorAlias={} bossCaptorAliases={} bossContainerAliases={} containerAliases={} lootTarget={}",
			g_registry.quest ? g_registry.quest->GetFormID() : 0u,
			g_registry.playerCaptiveAlias ? g_registry.playerCaptiveAlias->aliasID : static_cast<std::uint32_t>(0),
			g_registry.captiveMarkerAlias ? g_registry.captiveMarkerAlias->aliasID : static_cast<std::uint32_t>(0),
			g_registry.escapeDoorAlias ? g_registry.escapeDoorAlias->aliasID : static_cast<std::uint32_t>(0),
			bossCaptorCount,
			bossContainerCount,
			containerCount,
			g_registry.lootTargetAlias ? 1 : 0);

		if (!g_registry.playerCaptiveAlias) {
			spdlog::info("[TFD][Captive] PlayerCaptive alias not present in TFDCaptiveQuest; using ESP bridge aliases for captor/storage diagnostics");
		}
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
		if (!g_registry.quest) {
			return;
		}
		if (!g_registry.playerCaptiveAlias) {
			if (!g_playerCaptiveAliasMissingLogged) {
				g_playerCaptiveAliasMissingLogged = true;
				spdlog::info("[TFD][Captive] player captive alias sync skipped: alias missing in ESP reason={}", reason ? reason : "unknown");
			}
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
		// Recapture is a new stash cycle even when the same BossContainer is reused.
		// Without this reset, ProcessPendingConfiscation can discard the recapture
		// queue as already_applied from the previous captive cycle.
		g_confiscationApplied = false;
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

		const auto storageUnitsBefore = GetReferenceTotalInventoryCount(storage);
		const bool moved = TransferPlayerInventoryToStorage(storage, reason);
		const auto storageUnitsAfter = GetReferenceTotalInventoryCount(storage);
		g_confiscationApplied = true;
		if (moved) {
			++g_stashCycleID;
			if (g_stashCycleID == 0) {
				++g_stashCycleID;
			}
			const auto stashedUnits = (std::max)(0, storageUnitsAfter - storageUnitsBefore);
			const std::string cycleArg = std::to_string(g_stashCycleID);
			SendBridgeFormEvent("TFDCaptiveLootStashReady", storage, cycleArg.c_str(), static_cast<float>(storageUnitsBefore));
			spdlog::info("[TFD][Captive] stash cycle published cycle={} target={:08X} preUnits={} afterUnits={} stashedUnits={} reason={}",
				g_stashCycleID,
				storage ? storage->GetFormID() : 0u,
				storageUnitsBefore,
				storageUnitsAfter,
				stashedUnits,
				reason ? reason : "unknown");
		}
		if (g_starterLockpickPending) {
			EnsureStarterLockpicks(3, moved ? reason : "completed_no_items_starter_kit");
		}
		ClearPendingConfiscation(moved ? "completed" : "completed_no_items");
	}

	void SyncCaptiveNavigationAliases(const char* reason);
	void ClearCaptiveNavigationAliases(const char* reason);
	void EnsureCaptiveNavigationContext(RE::Actor* player, const char* reason);

	void SetRuntimeState(bool stateActive, PhaseValue phase)
	{
		g_state = stateActive;
		g_phase = phase;
		if (!stateActive) {
			ClearNativeCaptorRoleFaction("runtime_state_not_captive");
		}
		if (!IsCaptivePassiveHoldActive()) {
			TFD::HostilityController::ResetCaptiveSuppression();
		}
		if (!stateActive) {
			g_escapeBleedoutActive = false;
			g_recaptureCommitActive = false;
			g_recaptureCommitStarted = {};
			ClearCaptorCallCooldown();
			SyncPlayerAlias(nullptr, "captive_exit");
			ClearStorageDebugAliases("captive_exit");
			ClearCaptiveNavigationAliases("captive_exit");
			(void)TFD::FlowController::QueueBridgeModEvent(
				"TFDSystemEventClearAfterPleasure",
				nullptr,
				"captive_exit",
				1.0f);
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
		SyncCaptiveNavigationAliases("bind_door");
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

	void SyncCaptiveNavigationAliases(const char* reason)
	{
		ResolveQuestRegistry();
		if (!g_registry.quest) {
			return;
		}

		RE::TESObjectREFR* marker = nullptr;
		if (g_marker) {
			auto markerPtr = g_marker.get();
			marker = markerPtr.get();
		}
		if (!marker) {
			marker = TFD::Location::GetCachedCaptiveMarker();
		}

		RE::TESObjectREFR* door = ResolveBoundEscapeDoor();
		WriteQuestAlias(g_registry.captiveMarkerAlias, marker);
		WriteQuestAlias(g_registry.escapeDoorAlias, door);

		spdlog::info("[TFD][Captive] navigation aliases synced reason={} marker={:08X} escapeDoor={:08X} markerAlias={} doorAlias={}",
			reason ? reason : "unknown",
			marker ? marker->GetFormID() : 0u,
			door ? door->GetFormID() : 0u,
			g_registry.captiveMarkerAlias ? 1 : 0,
			g_registry.escapeDoorAlias ? 1 : 0);
	}

	void ClearCaptiveNavigationAliases(const char* reason)
	{
		ResolveQuestRegistry();
		if (!g_registry.quest) {
			return;
		}

		WriteQuestAlias(g_registry.captiveMarkerAlias, nullptr);
		WriteQuestAlias(g_registry.escapeDoorAlias, nullptr);
		spdlog::info("[TFD][Captive] navigation aliases cleared reason={} markerAlias={} doorAlias={}",
			reason ? reason : "unknown",
			g_registry.captiveMarkerAlias ? 1 : 0,
			g_registry.escapeDoorAlias ? 1 : 0);
	}

	static RE::TESObjectREFR* ResolveCaptiveMarkerRef()
	{
		if (g_marker) {
			auto ptr = g_marker.get();
			if (auto* marker = ptr.get()) {
				return marker;
			}
		}
		return TFD::Location::GetCachedCaptiveMarker();
	}

	static RE::FormID ResolveLocationIDFromRef(RE::TESObjectREFR* ref)
	{
		if (!ref) {
			return 0u;
		}
		if (auto* loc = TFD::Location::GetLocationFromRef(ref)) {
			return loc->GetFormID();
		}
		if (auto* cell = ref->GetParentCell()) {
			if (auto* loc = cell->GetLocation()) {
				return loc->GetFormID();
			}
		}
		return 0u;
	}

	static RE::FormID ResolveCaptiveAnchorLocationID()
	{
		if (auto* marker = ResolveCaptiveMarkerRef()) {
			if (const RE::FormID markerLoc = ResolveLocationIDFromRef(marker); markerLoc != 0u) {
				return markerLoc;
			}
		}
		return g_locationFormID;
	}

	static RE::FormID ResolveCaptiveAnchorCellID()
	{
		if (auto* marker = ResolveCaptiveMarkerRef()) {
			if (auto* cell = marker->GetParentCell()) {
				return cell->GetFormID();
			}
		}
		return g_cellFormID;
	}

	static bool HasPlayerLeftCaptiveLocation(RE::Actor* player, RE::FormID* oldLocationOut = nullptr, RE::FormID* newLocationOut = nullptr)
	{
		if (!g_state || !player) {
			return false;
		}

		const RE::FormID captiveLoc = ResolveCaptiveAnchorLocationID();
		const RE::FormID playerLoc = ResolveLocationIDFromRef(player);
		if (oldLocationOut) {
			*oldLocationOut = captiveLoc;
		}
		if (newLocationOut) {
			*newLocationOut = playerLoc;
		}

		return captiveLoc != 0u && playerLoc != 0u && playerLoc != captiveLoc;
	}

	static bool ResolveCaptiveExitToFree(RE::Actor* player, const char* reason, const RuntimeTickHandlers& handlers)
	{
		if (!g_state || !player) {
			return false;
		}
		if (g_recaptureCommitActive || g_escapeBleedoutActive) {
			return false;
		}
		if (g_phase == PhaseValue::Scene) {
			return false;
		}

		RE::FormID oldLoc = 0;
		RE::FormID newLoc = 0;
		if (!HasPlayerLeftCaptiveLocation(player, &oldLoc, &newLoc)) {
			return false;
		}

		const char* why = reason ? reason : "escape_resolved_location";
		TFD::HostilityController::ResetCaptiveSuppression();
		TFD::HostilityController::ClearAggressionClamp();
		TFD::Actor::Ops::ClearAggressorFactionContext();
		ResetLockpickWatch();
		ClearEscapeBreakRebleed();

		const auto beforePhase = g_phase;
		SetRuntimeState(false, PhaseValue::None);
		(void)TFD::FlowController::Controller::GetSingleton().ResolveCaptiveOutcome(TFD::FlowController::CaptiveOutcome::EscapeSucceeded, 0u, why);
		(void)TFD::FlowController::Controller::GetSingleton().CompleteTerminalContext(why);
		ClearEscapeContext();
		(void)TFD::FlowController::QueueBridgeModEvent("TFDBleedoutClearAll", nullptr, why, 1.0f);
		(void)TFD::FlowController::QueueBridgeModEvent("TFDTruceHardClearAll", nullptr, why, 1.0f);
		(void)TFD::FlowController::QueueBridgeModEvent("TFDSystemEventClearAfterPleasure", nullptr, why, 1.0f);
		if (handlers.escape.updatePreCombatState) {
			handlers.escape.updatePreCombatState();
		}

		spdlog::info("[TFD][Captive] CaptiveExitToFree by location reason={} phase={} oldLoc={:08X} newLoc={:08X}",
			why,
			GetPhaseName(true, beforePhase),
			oldLoc,
			newLoc);
		return true;
	}

	void EnsureCaptiveNavigationContext(RE::Actor* player, const char* reason)
	{
		if (!player) {
			return;
		}

		bool changed = false;
		RE::TESObjectREFR* marker = nullptr;
		if (g_marker) {
			auto markerPtr = g_marker.get();
			marker = markerPtr.get();
		}
		if (!marker) {
			marker = TFD::Location::GetCachedCaptiveMarker();
			if (!marker) {
				TFD::Location::RescanCaptiveMarker();
				marker = TFD::Location::GetCachedCaptiveMarker();
			}
			if (marker) {
				g_marker = marker->GetHandle();
				changed = true;
			}
		}

		RE::FormID cellID = 0;
		RE::FormID locID = 0;
		if (marker) {
			cellID = ResolveCaptiveAnchorCellID();
			locID = ResolveCaptiveAnchorLocationID();
		}

		// The escape context belongs to the CaptiveMarker location, not to the
		// player's current location.  Updating this anchor while the player is
		// escaping makes ``leave captive location`` impossible to detect and keeps
		// the HUD stuck in Captive after a successful escape.
		if (cellID == 0 || locID == 0) {
			auto* cell = player->GetParentCell();
			if (cellID == 0) {
				cellID = cell ? cell->GetFormID() : 0;
			}
			if (locID == 0) {
				auto* loc = cell ? cell->GetLocation() : nullptr;
				locID = loc ? loc->GetFormID() : 0;
			}
		}

		if (g_cellFormID != cellID || g_locationFormID != locID) {
			g_cellFormID = cellID;
			g_locationFormID = locID;
			changed = true;
		}

		if (!ResolveBoundEscapeDoor()) {
			if (auto* door = FindNearestDoorNearCaptiveMarker()) {
				BindDoor(door);
				changed = true;
				spdlog::info("[TFD][Captive] Bound nearest captive door {:08X} reason={}",
					door->GetFormID(),
					reason ? reason : "ensure_navigation");
			}
		}

		if (changed) {
			RE::TESObjectREFR* door = ResolveBoundEscapeDoor();
			spdlog::info("[TFD][Captive] Escape context refreshed reason={} marker={:08X} cell={:08X} loc={:08X} door={:08X}",
				reason ? reason : "ensure_navigation",
				marker ? marker->GetFormID() : 0u,
				g_cellFormID,
				g_locationFormID,
				door ? door->GetFormID() : 0u);
			SyncCaptiveNavigationAliases(reason ? reason : "ensure_navigation");
		}
	}

	void ClearEscapeContext()
	{
		ClearCaptiveNavigationAliases("clear_escape_context");
		ClearNativeCaptorRoleFaction("clear_escape_context");
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
		g_cellFormID = ResolveCaptiveAnchorCellID();
		g_locationFormID = ResolveCaptiveAnchorLocationID();
		if (g_cellFormID == 0 || g_locationFormID == 0) {
			auto* cell = player->GetParentCell();
			if (g_cellFormID == 0) {
				g_cellFormID = cell ? cell->GetFormID() : 0;
			}
			if (g_locationFormID == 0) {
				auto* loc = cell ? cell->GetLocation() : nullptr;
				g_locationFormID = loc ? loc->GetFormID() : 0;
			}
		}
		g_escapeRadiusActive = false;
		g_escapeRadiusSince = {};
		if (!g_door.HasDoor()) {
			if (auto* door = FindNearestDoorNearCaptiveMarker()) {
				BindDoor(door);
				spdlog::info("[TFD][Captive] Bound nearest captive door {:08X} on captive enter", door->GetFormID());
			}
		}
		EnsureCaptiveNavigationContext(player, "arm_escape_context");
		SyncCaptiveNavigationAliases("arm_escape_context");
		ApplyNativeCaptiveLocationRoleFactions(player, "arm_escape_context", true);
		RE::TESObjectREFR* boundDoor = ResolveBoundEscapeDoor();
		spdlog::info("[TFD][Captive] Escape context armed marker={:08X} cell={:08X} loc={:08X} door={:08X}",
			marker ? marker->GetFormID() : 0,
			g_cellFormID,
			g_locationFormID,
			boundDoor ? boundDoor->GetFormID() : 0u);
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
		return HasPlayerLeftCaptiveLocation(player, oldLocationOut, newLocationOut);
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
		if (g_escapeBleedoutActive || g_recaptureCommitActive) {
			spdlog::info("[TFD][Captive] EscapeCommit ignored during recapture/escape-bleedout reason={} escapeBleedout={} recaptureCommit={}",
				reason ? reason : "unknown",
				g_escapeBleedoutActive ? 1 : 0,
				g_recaptureCommitActive ? 1 : 0);
			return false;
		}

		g_escapeBleedoutActive = false;
		TFD::Actor::Ops::ClearAggressorFactionContext();
		TFD::HostilityController::ResetCaptiveSuppression();
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
		if (g_escapeBleedoutActive || g_recaptureCommitActive) {
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
		if (g_recaptureCommitActive) {
			return false;
		}
		if (g_escapeBleedoutActive) {
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
		g_escapeBleedoutActive = true;
		ResetLockpickWatch();
		if (handlers.clearLastAggressor) {
			handlers.clearLastAggressor();
		}
		if (handlers.updatePreCombatState) {
			handlers.updatePreCombatState();
		}
		spdlog::info("[TFD][Captive] Escape broken by defeat threshold pct={:.1f} thresh={:.1f} -> escape-bleedout active, schedule rebleed preferred={:08X}", pct, thresh, preferred ? preferred->GetFormID() : 0);
		return true;
	}

	bool TickRuntime(RE::Actor* player, bool captiveBleedOverlay, const RuntimeTickHandlers& handlers)
	{
		ProcessPendingConfiscation();
		(void)NormalizeInvalidCaptivePair();

		if (IsCaptivePassiveHoldActive()) {
			if (IsStandardCaptiveActive()) {
				EnsureCaptiveNavigationContext(player, "standard_captive_tick");
			}
			TFD::HostilityController::TickCaptiveSuppression();
		} else {
			TFD::HostilityController::ResetCaptiveSuppression();
		}

		// Captive runtime intentionally consumes the DefeatMonitor tick while active.
		// Service shared native dialogue/pleasure runtimes here too, otherwise
		// Calling Captor can arm DialogueOpen but never reach SetDialogueWithPlayer,
		// and Captive Pleasure can leave the OStim handoff watchdog idle.
		if (ResolveCaptiveExitToFree(player, "escape_resolved_location", handlers)) {
			return false;
		}

		if (IsActive()) {
			// TFDCaptiveFaction is a dialogue-condition tag for the active captive
			// location.  Do not keep sweeping it while Escape is active/outside the
			// captive location; that can retarget the anchor to the player's new
			// location and leave the HUD stuck in Captive.
			if (IsCaptivePassiveHoldActive()) {
				ApplyNativeCaptiveLocationRoleFactions(player, "captive_runtime_tick", false);
			}
			TFD::InteractionRouter::DialogueOpen::Tick();
			TFD::PleasureRuntime::Tick();
		}

		if (IsStandardCaptiveActive()) {
			const bool dialogOpen = handlers.isDialogueOpen ? handlers.isDialogueOpen() : false;
			const bool prevDialogueOpen = handlers.getPrevDialogueOpen ? handlers.getPrevDialogueOpen() : false;
			if (dialogOpen) {
				TFD::CaptiveGreet::NotifyDialogueOpened();
			} else if (prevDialogueOpen && TFD::CaptiveGreet::IsActive()) {
				TFD::CaptiveGreet::Cancel("dialogue_closed");
			}
			if (!dialogOpen && prevDialogueOpen) {
				spdlog::info("[TFD][Captive] Dialogue closed -> no implicit action");
			}
			if (handlers.setPrevDialogueOpen) {
				handlers.setPrevDialogueOpen(dialogOpen);
			}
			if (!captiveBleedOverlay) {
				(void)TickCaptiveEscapePhase(player, handlers.escape);
			}
		} else if (IsEscapeActive()) {
			// Once Escape has crossed into the EscapeFailed/bleedout overlay,
			// Captive must stop consuming the DefeatMonitor tick.  The bleedout
			// runtime owns the forcegreet / recapture window from here; consuming
			// the tick leaves g_escapeBreakBleedPending armed but never serviced.
			if (captiveBleedOverlay || g_escapeBleedoutActive || g_recaptureCommitActive) {
				return true;
			}

			if (!TickEscapeActivePhase(player, handlers.escape)) {
				return false;
			}
		}

		if (IsActive() && !captiveBleedOverlay && !HasEscapeBreakRebleedPending()) {
			return false;
		}

		return true;
	}

	bool TriggerPlayerAggressionEscape(RE::Actor* actor, const char* reason)
	{
		if (!g_state) {
			return false;
		}
		if (g_escapeBleedoutActive || g_recaptureCommitActive) {
			spdlog::info("[TFD][Captive] aggression escape ignored during recapture/escape-bleedout actor={:08X} reason={}",
				actor ? actor->GetFormID() : 0u,
				reason ? reason : "unknown");
			return false;
		}
		SetRuntimeState(true, PhaseValue::Escape);
		TFD::HostilityController::ResetCaptiveSuppression();
		TFD::HostilityController::ClearAggressionClamp();
		TFD::Actor::Ops::ClearAggressorFactionContext();
		(void)TFD::FlowController::Controller::GetSingleton().ResolveCaptiveOutcome(TFD::FlowController::CaptiveOutcome::EscapeStarted, actor ? actor->GetFormID() : 0u, reason ? reason : "player_aggression_captive");
		return true;
	}

	static std::uint32_t ResolveRecaptureActorFormID(RE::Actor* preferredCaptor)
	{
		if (preferredCaptor) {
			return preferredCaptor->GetFormID();
		}
		const auto snapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
		if (snapshot.primaryActorFormID != 0) {
			return snapshot.primaryActorFormID;
		}
		if (g_lastRecaptureActorID != 0) {
			return g_lastRecaptureActorID;
		}
		return 0;
	}

	bool CommitRecapture(RE::Actor* preferredCaptor, const char* reason)
	{
		const char* why = reason ? reason : "recapture_commit";
		const auto now = Now();
		if (g_recaptureCommitActive) {
			spdlog::info("[TFD][Captive] CommitRecapture duplicate ignored active=1 reason={} actor={:08X}",
				why,
				ActorFormID(preferredCaptor));
			return true;
		}
		if (IsRecaptureRecentlyCommitted() && IsStandardCaptiveActive()) {
			spdlog::info("[TFD][Captive] CommitRecapture duplicate ignored recent=1 elapsedMs={} reason={} actor={:08X}",
				static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastRecaptureCompleted).count()),
				why,
				ActorFormID(preferredCaptor));
			return true;
		}

		g_recaptureCommitActive = true;
		g_recaptureCommitStarted = now;
		const std::uint32_t actorFormID = ResolveRecaptureActorFormID(preferredCaptor);
		g_lastRecaptureActorID = actorFormID;

		ClearEscapeBreakRebleed();
		g_escapeBleedoutActive = false;
		ResetLockpickWatch();
		TFD::HostilityController::ResetCaptiveSuppression();
		TFD::HostilityController::ClearAggressionClamp();
		TFD::Actor::Ops::ClearAggressorFactionContext();

		auto& flow = TFD::FlowController::Controller::GetSingleton();
		const auto before = flow.GetSnapshot();
		if (before.root == TFD::FlowController::RootFlow::Captive &&
			before.gate == TFD::FlowController::DecisionGate::PlayerBleedout) {
			(void)flow.ResolveBleedoutOutcome(TFD::FlowController::BleedoutOutcome::Captive, actorFormID, why);
		} else if (before.root == TFD::FlowController::RootFlow::Captive) {
			(void)flow.ResolveCaptiveOutcome(TFD::FlowController::CaptiveOutcome::Recaptured, actorFormID, why);
		}

		auto runtimeHandlers = TFD::Transition::DefeatGlue::BuildTransitionRuntimeHandlers();
		auto captiveHandlers = TFD::Transition::DefeatGlue::BuildTransitionCaptiveHandlers();
		bool completed = false;
		if (TFD::Transition::ResolveCaptiveMarkerForOutcome(runtimeHandlers)) {
			completed = TFD::Transition::CompleteCaptiveTransitionNow(why, runtimeHandlers, captiveHandlers);
		} else {
			spdlog::warn("[TFD][Captive] CommitRecapture failed marker resolution reason={} actor={:08X}", why, actorFormID);
		}

		if (completed) {
			if (actorFormID != 0) {
				(void)flow.RequestCaptive(actorFormID, TFD::FlowController::CaptiveMode::Kidnapped, why);
			}
			SetRuntimeState(true, PhaseValue::Captive);
			CaptureCurrentLockpickMenuState();
			ResetLockpickWatch();
			if (auto* player = Player()) {
				ArmEscapeContextFromCurrentState(player);
				SyncPlayerAlias(player, why);
			}
			SealDoorIfPresent();
			(void)TFD::FlowController::QueueBridgeModEvent(
				"TFDSystemEventClearAfterPleasure",
				nullptr,
				why,
				1.0f);
			(void)TFD::FlowController::QueueBridgeModEvent(
				"TFDBleedoutClearAll",
				nullptr,
				why,
				1.0f);
			(void)TFD::FlowController::QueueBridgeModEvent(
				"TFDTruceClearAll",
				nullptr,
				why,
				1.0f);
			(void)TFD::FlowController::QueueBridgeModEvent(
				"TFDTruceHardClearAll",
				nullptr,
				why,
				1.0f);
			TFD::DefeatMonitor::ForceRecoverPlayerAfterCaptiveRecapture(why);
			g_lastRecaptureCompleted = Now();
			spdlog::info("[TFD][Captive] CommitRecapture complete actor={:08X} reason={} clearBleedAliases=1 clearCrowdAliases=1 hardCrowdClear=1 recoverPlayer=1", actorFormID, why);
		} else {
			spdlog::warn("[TFD][Captive] CommitRecapture incomplete actor={:08X} reason={}", actorFormID, why);
		}

		g_recaptureCommitActive = false;
		g_recaptureCommitStarted = {};
		return completed;
	}

	bool BeginCaptorCallHotkey(RE::Actor* player, RE::Actor** outCaptor)
	{
		if (outCaptor) {
			*outCaptor = nullptr;
		}
		if (!player) {
			spdlog::warn("[TFD][Captive] Call Captor rejected: missing player");
			return false;
		}

		float remainingSeconds = 0.0f;
		if (IsCaptorCallCooldownActive(&remainingSeconds)) {
			spdlog::info("[TFD][Captive] Call Captor rejected: cooldown remaining={:.2f}s", remainingSeconds);
			return false;
		}

		EnsureCaptiveNavigationContext(player, "call_captor_hotkey");

		auto* captor = PickCaptorSameCellLoaded(player, 12288.0f);
		if (!captor) {
			return false;
		}

		ApplyCallCaptorCalmBubble(player, captor, 12288.0f);
		if (!TFD::CaptiveGreet::Begin(captor, "call_captor_hotkey")) {
			spdlog::warn("[TFD][Captive] Call Captor greet failed actor={:08X}", captor->GetFormID());
			return false;
		}

		g_captorCallCooldownUntil = Now() + kCaptorCallCooldown;
		spdlog::info("[TFD][Captive] Call Captor begin actor={:08X} cooldownMs={} reason=call_captor_hotkey", captor->GetFormID(), static_cast<int>(kCaptorCallCooldown.count()));
		if (outCaptor) {
			*outCaptor = captor;
		}
		return true;
	}

	void ApplyQueuedDefeatProgressState(const ApplyQueuedDefeatProgressHandlers& handlers)
	{
		SetRuntimeState(GetQueuedStateFlag(), GetQueuedPhase());
		if (handlers.setPrevDialogueOpen && handlers.isDialogueOpen) {
			handlers.setPrevDialogueOpen(handlers.isDialogueOpen());
		}
		CaptureCurrentLockpickMenuState();
		if (GetQueuedStateFlag() && (GetQueuedPhase() == PhaseValue::Captive || GetQueuedPhase() == PhaseValue::ReleasedWork)) {
			TFD::Location::RescanCaptiveMarker();
			if (handlers.getPlayer) {
				ArmEscapeContextFromCurrentState(handlers.getPlayer());
			}
		} else {
			ResetLockpickWatch();
			ClearEscapeContext();
		}
	}

	void ResetForLoad()
	{
		ClearNativeCaptorRoleFaction("reset_for_load");
		TFD::HostilityController::ResetCaptiveSuppression();
		g_state = false;
		g_phase = PhaseValue::None;
		g_confiscationApplied = false;
		g_confiscationPending = false;
		g_starterLockpickPending = false;
		g_pendingConfiscationReason.clear();
		g_confiscationAttemptCount = 0;
		g_confiscationNextAttempt = {};
		ClearCaptiveNavigationAliases("reset_for_load");
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
		g_escapeBleedoutActive = false;
		g_recaptureCommitActive = false;
		g_recaptureCommitStarted = {};
		g_lastRecaptureCompleted = {};
		g_lastRecaptureActorID = 0;
		g_stashCycleID = 0;
	}
}
