#include "TFDCaptive.h"
#include "TFDCaptiveGreet.h"
#include "TFDActor.h"

#include "TFDHostilityController.h"
#include "TFDTame.h"
#include "TFDSettings.h"
#include "TFDLocation.h"
#include "TFDWorkNative.h"

#include "TFDFlowController.h"
#include "TFDInteractionRouter.h"
#include "TFDPleasureRuntime.h"
#include "TFDTransition.h"
#include "TFDDefeatMonitor.h"
#include "TFDForceGreetState.h"

#include <SKSE/SKSE.h>

#include <RE/A/ActorValues.h>
#include <RE/L/LockpickingMenu.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>
#include <atomic>
#include <cstring>
#include <mutex>

namespace TFD::Captive
{
	void EnsureCaptiveNavigationContext(RE::Actor* player, const char* reason);
	void SyncCaptiveNavigationAliases(const char* reason);
	RE::TESObjectREFR* ResolveBoundEscapeDoor();
	RE::TESObjectREFR* ResolveCaptorApproachTarget(RE::Actor* player);
	const char* GetCaptorApproachTargetName(RE::TESObjectREFR* target, RE::Actor* player);
	void SyncCaptorApproachPointAlias(RE::Actor* player, const char* reason);
	void ClearCaptorApproachPointAlias(const char* reason);

	namespace
	{
		RE::TESGlobal* g_captiveStateGlobal{ nullptr };
		RE::TESGlobal* g_workJobTypeGlobal{ nullptr };
		RE::TESGlobal* g_workAssignmentStateGlobal{ nullptr };

		static constexpr const char* kCaptorApproachReadyEvent = "TFDCaptiveApproachReady";
		std::atomic_bool g_captorApproachReadySinkInstalled{ false };
		std::mutex g_captorHandshakeLock;
		RE::FormID g_pendingCaptorCallActorFormID{ 0 };
		std::uint32_t g_pendingCaptorCallSerial{ 0 };
		std::chrono::steady_clock::time_point g_pendingCaptorCallStarted{};

		static constexpr const char* kRecoverGearContainerOpenedEvent = "TFDCaptiveLootContainerOpened";
		static constexpr std::array<const char*, 10> kWorkCraftingStationAliasNames{
			nullptr,
			"WorkForge",
			"WorkSmelter",
			"WorkTanningRack",
			"WorkSharpeningWheel",
			"WorkArmorWorkbench",
			"WorkChoppingBlock",
			"WorkCookingStation",
			"WorkAlchemyLab",
			"WorkEnchantingTable"
		};
		static constexpr auto kRecoverGearOpenNotifyCooldown = std::chrono::milliseconds(750);
		RE::FormID g_lastRecoverGearOpenNotifyRefID{ 0 };
		std::chrono::steady_clock::time_point g_lastRecoverGearOpenNotifyAt{};

		static RE::TESGlobal* ResolveCaptiveStateGlobal()
		{
			if (!g_captiveStateGlobal) {
				g_captiveStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDCaptiveState");
			}
			return g_captiveStateGlobal;
		}

		static RE::TESGlobal* ResolveWorkJobTypeGlobal()
		{
			if (!g_workJobTypeGlobal) {
				g_workJobTypeGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDWorkJobType");
			}
			return g_workJobTypeGlobal;
		}

		static RE::TESGlobal* ResolveWorkAssignmentStateGlobal()
		{
			if (!g_workAssignmentStateGlobal) {
				g_workAssignmentStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDWorkAssignmentState");
			}
			return g_workAssignmentStateGlobal;
		}

		static std::uint32_t ReadGlobalUInt(RE::TESGlobal* global)
		{
			if (!global) {
				return 0;
			}
			return static_cast<std::uint32_t>(std::max(0L, std::lround(global->value)));
		}

		static void ForceWorkGlobals(std::uint32_t jobType, std::uint32_t assignmentState, const char* reason)
		{
			if (auto* job = ResolveWorkJobTypeGlobal()) {
				job->value = static_cast<float>(jobType);
			}
			else {
				spdlog::warn("[TFD][Captive] TFDWorkJobType global unavailable job={} reason={}", jobType, reason ? reason : "unknown");
			}

			if (auto* assignment = ResolveWorkAssignmentStateGlobal()) {
				assignment->value = static_cast<float>(assignmentState);
			}
			else {
				spdlog::warn("[TFD][Captive] TFDWorkAssignmentState global unavailable assignment={} reason={}", assignmentState, reason ? reason : "unknown");
			}

			spdlog::info("[TFD][Captive] work globals forced job={} assignment={} reason={}", jobType, assignmentState, reason ? reason : "unknown");
		}

		static void ForceCaptiveStateGlobal(std::uint32_t value, const char* reason)
		{
			auto* global = ResolveCaptiveStateGlobal();
			if (!global) {
				spdlog::warn("[TFD][Captive] TFDCaptiveState global unavailable value={} reason={}", value, reason ? reason : "unknown");
				return;
			}
			const auto oldValue = static_cast<std::uint32_t>(std::lround(global->value));
			global->value = static_cast<float>(value);
			if (oldValue != value) {
				spdlog::info("[TFD][Captive] TFDCaptiveState forced old={} new={} reason={}", oldValue, value, reason ? reason : "unknown");
			}
		}

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

		static bool ActorHasNativeCaptiveRole(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}

			auto* faction = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDCaptiveFaction");
			if (!faction) {
				return false;
			}

			return actor->IsInFaction(faction);
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
			std::uint32_t captiveRole = 0;
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
				const bool hasCaptiveRole = ActorHasNativeCaptiveRole(a);
				if (hasCaptiveRole) {
					++captiveRole;
				}
				const bool hostile = info.hostileToPlayer || a->IsHostileToActor(player);
				const bool combat = info.inCombat || a->IsInCombat();

				// Calling Captor is a captive-camp request.  Do not require or
				// prioritize LOS here.  Jail bars, furniture, and collision can block
				// sight even when the selected actor is the correct captor.  Candidate
				// validity is based on captive role/suppression or hostile combat
				// context; LOS is diagnostic only.
				if (!hasCaptiveRole && !isSuppressed && !hostile && !combat) {
					continue;
				}

				float score = info.dist;
				if (isSuppressed) {
					score -= 10000.0f;
				}
				if (hasCaptiveRole) {
					score -= 7000.0f;
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
				spdlog::info("[TFD][Captive] Call Captor candidate selected actor={:08X} distScore={:.1f} considered={} supported={} sameSpace={} captiveRole={} suppressed={} los={}",
					best->GetFormID(),
					bestScore,
					considered,
					supported,
					sameSpace,
					captiveRole,
					suppressed,
					losCount);
			}
			else {
				spdlog::warn("[TFD][Captive] Call Captor no candidate considered={} supported={} sameSpace={} captiveRole={} suppressed={} los={} radius={:.1f}",
					considered,
					supported,
					sameSpace,
					captiveRole,
					suppressed,
					losCount,
					searchRadius);
			}

			return best;
		}

		static bool QueueDirectBridgeActorEvent(const char* eventName, RE::Actor* actor, const char* reason, float numArg = 0.0f)
		{
			if (!eventName || !eventName[0] || !actor) {
				return false;
			}

			auto* task = SKSE::GetTaskInterface();
			if (!task) {
				return false;
			}

			const std::string name{ eventName };
			const std::string sarg{ reason ? reason : "" };
			const float narg = numArg;
			const std::uint32_t actorHandle = actor->GetHandle().native_handle();
			const RE::FormID actorFormID = actor->GetFormID();

			task->AddTask([name, sarg, narg, actorHandle, actorFormID]() {
				RE::TESForm* outSender = nullptr;

				if (actorHandle != 0) {
					auto actorSp = RE::Actor::LookupByHandle(actorHandle);
					outSender = actorSp.get();
				}
				if (!outSender && actorFormID != 0) {
					outSender = RE::TESForm::LookupByID(actorFormID);
				}

				auto* src = SKSE::GetModCallbackEventSource();
				if (!src) {
					spdlog::warn("[TFD][Captive] direct bridge dispatch skipped no callback source event={} actor={:08X}", name, actorFormID);
					return;
				}

				SKSE::ModCallbackEvent e(name.c_str(), sarg.c_str(), narg, outSender);
				src->SendEvent(&e);
				spdlog::info("[TFD][Captive] direct bridge dispatch event={} actor={:08X} resolved={:08X} reason={}",
					name,
					actorFormID,
					outSender ? outSender->GetFormID() : 0u,
					sarg);
				});

			return true;
		}

		static void SendBridgeAssignActor(const char* eventName, RE::Actor* actor, const char* reason = "")
		{
			if (!eventName || !actor) {
				return;
			}

			// Calling Captor now follows the PreCombat-style ownership rule:
			// Papyrus owns alias binding and package warm-up.  Native only sends
			// the assignment request.  Send both direct-task and queued variants
			// because earlier fresh-game logs showed direct dispatch could be
			// missed; TFDCaptiveBridge ignores duplicate active requests.
			const bool directQueued = QueueDirectBridgeActorEvent(eventName, actor, reason ? reason : "", 0.0f);
			if (directQueued) {
				spdlog::info("[TFD][Captive] direct bridge assign queued event={} actor={:08X} reason={} policy=direct_task",
					eventName,
					actor->GetFormID(),
					reason ? reason : "");
			}

			const bool queued = TFD::FlowController::QueueBridgeModEvent(
				eventName,
				actor,
				reason ? reason : "",
				0.0f);
			if (queued) {
				spdlog::info("[TFD][Captive] queued bridge assign event={} actor={:08X} reason={} policy=queue_fallback",
					eventName,
					actor->GetFormID(),
					reason ? reason : "");
			}

			if (directQueued || queued) {
				return;
			}

			auto* src = SKSE::GetModCallbackEventSource();
			if (!src) {
				spdlog::warn("[TFD][Captive] bridge assign failed no callback source event={} actor={:08X} reason={}",
					eventName,
					actor->GetFormID(),
					reason ? reason : "");
				return;
			}

			SKSE::ModCallbackEvent e(eventName, reason ? reason : "", 0.0f, actor);
			src->SendEvent(&e);
			spdlog::info("[TFD][Captive] direct bridge assign fallback event={} actor={:08X} reason={}",
				eventName,
				actor->GetFormID(),
				reason ? reason : "");
		}

		static bool SendBridgeFormEvent(const char* eventName, RE::TESForm* sender, const char* reason = "", float numArg = 0.0f)
		{
			if (!eventName || !sender) {
				return false;
			}

			auto* src = SKSE::GetModCallbackEventSource();
			if (!src) {
				return false;
			}

			SKSE::ModCallbackEvent e(eventName, reason ? reason : "", numArg, sender);
			src->SendEvent(&e);
			return true;
		}


		static bool ApplyNativeCaptorRoleFaction(RE::Actor* actor, const char* reason);
		static void ApplyNativeCaptiveLocationRoleFactions(RE::Actor* player, const char* reason, bool force);

		static void ApplyCallCaptorCalmBubble(RE::Actor* player, RE::Actor* primaryTarget, float radius)
		{
			(void)player;
			(void)radius;
			if (!primaryTarget) {
				return;
			}

			// Calling Captor is not a Truce. The captive runtime has already
			// pacified the active location when the player entered the CaptiveMarker.
			// Do not run wide stop-combat waves here. Native has already seeded
			// ApproachPoint/OwnerCaptor; this only notifies Papyrus to maintain the
			// package and send TFDCaptiveApproachReady before native opens CaptiveGreet.

			primaryTarget->AllowPCDialogue(true);
			if (primaryTarget->IsInCombat()) {
				primaryTarget->StopCombat();
				primaryTarget->StopAlarmOnActor();
			}
			SendBridgeAssignActor("TFDCaptiveAssign", primaryTarget, "call_captor_hotkey");
			spdlog::info("[TFD][Captive] call captor bridge assign sent actor={:08X} policy=native_seed_plus_papyrus_maintain", primaryTarget->GetFormID());
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

		static bool HandleCaptorApproachReady(RE::Actor* actor, const char* reason, float token)
		{
			if (!actor) {
				spdlog::warn("[TFD][Captive][Handshake] ready event ignored: missing actor reason={} token={:.0f}",
					reason ? reason : "",
					token);
				return false;
			}

			RE::FormID expectedActorFormID = 0;
			std::uint32_t serial = 0;
			std::chrono::steady_clock::time_point started{};
			{
				std::scoped_lock lock(g_captorHandshakeLock);
				expectedActorFormID = g_pendingCaptorCallActorFormID;
				serial = g_pendingCaptorCallSerial;
				started = g_pendingCaptorCallStarted;
			}

			const auto actorFormID = actor->GetFormID();
			const double elapsedMs = started.time_since_epoch().count() != 0
				? static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(Now() - started).count())
				: -1.0;
			if (!expectedActorFormID) {
				spdlog::info("[TFD][Captive][Handshake] ready event ignored actor={:08X} expected=00000000 serial={} reason={} token={:.0f} cause=no_pending_direct_or_completed",
					actorFormID,
					serial,
					reason ? reason : "",
					token);
				return false;
			}

			if (expectedActorFormID != actorFormID) {
				spdlog::warn("[TFD][Captive][Handshake] ready event ignored actor={:08X} expected={:08X} serial={} reason={} token={:.0f}",
					actorFormID,
					expectedActorFormID,
					serial,
					reason ? reason : "",
					token);
				return false;
			}

			if (!IsStandardCaptiveActive()) {
				spdlog::warn("[TFD][Captive][Handshake] ready event ignored actor={:08X} reason={} token={:.0f} cause=not_standard_captive",
					actorFormID,
					reason ? reason : "",
					token);
				return false;
			}

			LogCallingCaptorOwnershipSnapshot(actor, "handshake_ready_before_greet");
			actor->AllowPCDialogue(true);
			if (actor->IsInCombat()) {
				actor->StopCombat();
			}
			if (actor->IsWeaponDrawn()) {
				// R244A: no forced weapon stance; Skyrim handles sheath/draw naturally. Disabled: actor->DrawWeaponMagicHands(false);
			}

			const bool began = TFD::CaptiveGreet::Begin(actor, "call_captor_package_ready");
			LogCallingCaptorOwnershipSnapshot(actor, began ? "handshake_after_greet_begin" : "handshake_greet_begin_failed");

			if (began) {
				{
					std::scoped_lock lock(g_captorHandshakeLock);
					if (g_pendingCaptorCallActorFormID == actorFormID) {
						g_pendingCaptorCallActorFormID = 0;
					}
				}
				spdlog::info("[TFD][Captive][Handshake] ready accepted actor={:08X} serial={} reason={} token={:.0f} elapsedMs={:.0f}",
					actorFormID,
					serial,
					reason ? reason : "",
					token,
					elapsedMs);
			}
			else {
				spdlog::warn("[TFD][Captive][Handshake] ready rejected by CaptiveGreet actor={:08X} serial={} reason={} token={:.0f} elapsedMs={:.0f}",
					actorFormID,
					serial,
					reason ? reason : "",
					token,
					elapsedMs);
			}

			return began;
		}

		class CaptorApproachReadySink final : public RE::BSTEventSink<SKSE::ModCallbackEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* ev, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
			{
				if (!ev) {
					return RE::BSEventNotifyControl::kContinue;
				}

				const auto* rawName = ev->eventName.c_str();
				if (!rawName || std::strcmp(rawName, kCaptorApproachReadyEvent) != 0) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* actor = ev->sender ? ev->sender->As<RE::Actor>() : nullptr;
				(void)HandleCaptorApproachReady(actor, ev->strArg.c_str(), ev->numArg);
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		CaptorApproachReadySink g_captorApproachReadySink{};

		static void EnsureCaptorApproachReadySinkRegistered()
		{
			if (g_captorApproachReadySinkInstalled.load(std::memory_order_acquire)) {
				return;
			}

			auto* src = SKSE::GetModCallbackEventSource();
			if (!src) {
				spdlog::warn("[TFD][Captive][Handshake] ready sink registration skipped: no callback source");
				return;
			}

			src->AddEventSink(&g_captorApproachReadySink);
			g_captorApproachReadySinkInstalled.store(true, std::memory_order_release);
			spdlog::info("[TFD][Captive][Handshake] ready sink registered event={}", kCaptorApproachReadyEvent);
		}

		static void ArmPendingCaptorCallHandshake(RE::Actor* actor, const char* reason)
		{
			if (!actor) {
				return;
			}

			std::uint32_t serial = 0;
			{
				std::scoped_lock lock(g_captorHandshakeLock);
				g_pendingCaptorCallActorFormID = actor->GetFormID();
				g_pendingCaptorCallStarted = Now();
				serial = ++g_pendingCaptorCallSerial;
			}

			spdlog::info("[TFD][Captive][Handshake] armed actor={:08X} serial={} reason={}",
				actor->GetFormID(),
				serial,
				reason ? reason : "unknown");
		}

		static void ClearPendingCaptorCallHandshake(RE::FormID actorFormID, const char* reason)
		{
			std::uint32_t serial = 0;
			bool cleared = false;
			{
				std::scoped_lock lock(g_captorHandshakeLock);
				if (g_pendingCaptorCallActorFormID != 0 &&
					(actorFormID == 0 || g_pendingCaptorCallActorFormID == actorFormID)) {
					g_pendingCaptorCallActorFormID = 0;
					g_pendingCaptorCallStarted = {};
					serial = g_pendingCaptorCallSerial;
					cleared = true;
				}
			}

			if (cleared) {
				spdlog::info("[TFD][Captive][Handshake] cleared actor={:08X} serial={} reason={}",
					actorFormID,
					serial,
					reason ? reason : "unknown");
			}
		}

		static bool BeginCaptorCallApproachGreet(RE::Actor* actor, const char* reason)
		{
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				return false;
			}

			LogCallingCaptorOwnershipSnapshot(actor, "approach_no_teleport_begin_before_greet");
			actor->AllowPCDialogue(true);
			if (actor->IsInCombat()) {
				actor->StopCombat();
			}

			const bool began = TFD::CaptiveGreet::Begin(actor, reason ? reason : "call_captor_approach_no_teleport");
			LogCallingCaptorOwnershipSnapshot(actor, began ? "approach_no_teleport_after_greet_begin" : "approach_no_teleport_greet_begin_failed");
			spdlog::info("[TFD][Captive][Handshake] direct approach-window greet actor={:08X} began={} reason={} policy=walk_to_player_then_open_under_360_no_moveto",
				actor->GetFormID(),
				began ? 1 : 0,
				reason ? reason : "unknown");
			return began;
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
		std::uint32_t g_queuedWorkBossFormID = 0;
		std::uint32_t g_queuedWorkJobType = 0;
		std::uint32_t g_queuedWorkAssignmentState = 0;
		bool g_escapeBreakBleedPending = false;
		RE::ActorHandle g_escapeBreakPreferredAggressor{};
		bool g_prevLockpickOpen = false;
		bool g_escapeRadiusActive = false;
		std::chrono::steady_clock::time_point g_escapeRadiusSince{};
		RE::ObjectRefHandle g_boundEscapeDoor{};
		RE::ObjectRefHandle g_lockpickDoorCandidate{};
		bool g_lockpickDoorWasLocked = false;
		std::chrono::steady_clock::time_point g_returnToCaptiveGuardUntil{};
		std::uint32_t g_returnToCaptiveGuardDepth = 0;


		bool g_escapeBleedoutActive = false;
		bool g_recaptureCommitActive = false;
		std::chrono::steady_clock::time_point g_recaptureCommitStarted{};
		std::chrono::steady_clock::time_point g_lastRecaptureCompleted{};
		std::uint32_t g_lastRecaptureActorID = 0;
		std::uint32_t g_stashCycleID = 0;
		bool g_recoverGearStashActive = false;
		bool g_recoverGearStashOpened = false;
		RE::FormID g_recoverGearStashTargetFormID = 0;
		std::int32_t g_recoverGearStashBaseUnits = 0;
		struct NativeCaptiveRoleEntry
		{
			RE::ActorHandle actor{};
			RE::FormID formID = 0;
			bool addedByTFD = false;
		};

		struct NativeWorkingRoleEntry
		{
			RE::ActorHandle actor{};
			RE::FormID formID = 0;
			bool addedByTFD = false;
		};

		std::vector<NativeCaptiveRoleEntry> g_nativeCaptiveRoleActors{};
		std::vector<NativeWorkingRoleEntry> g_nativeWorkingRoleActors{};
		RE::ActorHandle g_currentWorkBoss{};
		RE::FormID g_currentWorkBossFormID = 0;
		std::chrono::steady_clock::time_point g_workBossResumeAt{};
		std::chrono::steady_clock::time_point g_nextCaptiveRoleFactionSweep{};
		std::chrono::steady_clock::time_point g_nextCaptiveRoleFactionNoopLog{};
		std::chrono::steady_clock::time_point g_releasedWorkWeaponDrawnNextLog{};
		std::chrono::steady_clock::time_point g_nextReleasedWorkAliasSelfHeal{};
		bool g_releasedWorkEscapeBreakResourcesCleared = false;
		static constexpr auto kCaptiveRoleFactionSweepInterval = std::chrono::milliseconds(1500);
		static constexpr auto kCaptiveRoleFactionNoopLogInterval = std::chrono::milliseconds(10000);
		static constexpr auto kReleasedWorkAliasSelfHealInterval = std::chrono::milliseconds(5000);
		static constexpr auto kRecaptureDuplicateGuard = std::chrono::milliseconds(8000);

		struct WorkResourceAliasSnapshot
		{
			bool valid = false;
			RE::FormID mineRefId = 0;
			RE::FormID craftingRefId = 0;
			std::array<RE::FormID, 10> stationRefIds{};
		};

		WorkResourceAliasSnapshot g_lastWorkResourceAliasSnapshot{};

		static std::uint32_t ActorFormID(RE::Actor* actor)
		{
			return actor ? actor->GetFormID() : 0u;
		}

		static bool ReasonEquals(const char* reason, const char* expected)
		{
			return reason && expected && std::strcmp(reason, expected) == 0;
		}

		static bool WorkResourceAliasSnapshotMatches(
			RE::FormID mineRefId,
			RE::FormID craftingRefId,
			const std::array<RE::FormID, 10>& stationRefIds)
		{
			if (!g_lastWorkResourceAliasSnapshot.valid) {
				return false;
			}

			return g_lastWorkResourceAliasSnapshot.mineRefId == mineRefId &&
				g_lastWorkResourceAliasSnapshot.craftingRefId == craftingRefId &&
				g_lastWorkResourceAliasSnapshot.stationRefIds == stationRefIds;
		}

		static void RememberWorkResourceAliasSnapshot(
			RE::FormID mineRefId,
			RE::FormID craftingRefId,
			const std::array<RE::FormID, 10>& stationRefIds)
		{
			g_lastWorkResourceAliasSnapshot.valid = true;
			g_lastWorkResourceAliasSnapshot.mineRefId = mineRefId;
			g_lastWorkResourceAliasSnapshot.craftingRefId = craftingRefId;
			g_lastWorkResourceAliasSnapshot.stationRefIds = stationRefIds;
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

		static RE::TESFaction* ResolveNativeWorkingCaptiveFaction()
		{
			static RE::TESFaction* cached = nullptr;
			static bool resolved = false;
			if (!resolved) {
				resolved = true;
				cached = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDWorkingCaptiveFaction");
				if (!cached) {
					spdlog::warn("[TFD][Captive] native working captive faction missing editorId=TFDWorkingCaptiveFaction");
				}
			}
			return cached;
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
					}
					else {
						++preserved;
					}
				}
			}

			g_nativeCaptiveRoleActors.clear();
			g_nextCaptiveRoleFactionSweep = {};

			spdlog::info("[TFD][Captive] native captive location role faction cleared removed={} preserved={} stale={} reason={}",
				removed,
				preserved,
				stale,
				reason ? reason : "unknown");
		}

		static bool IsNativeWorkingFactionTracked(RE::FormID formID)
		{
			if (formID == 0) {
				return false;
			}
			for (const auto& entry : g_nativeWorkingRoleActors) {
				if (entry.formID == formID) {
					return true;
				}
			}
			return false;
		}

		static void ClearNativeWorkingCaptiveFaction(const char* reason)
		{
			auto* faction = ResolveNativeWorkingCaptiveFaction();
			std::uint32_t removed = 0;
			std::uint32_t preserved = 0;
			std::uint32_t stale = 0;

			if (faction) {
				for (auto& entry : g_nativeWorkingRoleActors) {
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
					}
					else {
						++preserved;
					}
				}
			}

			g_nativeWorkingRoleActors.clear();
			if (removed > 0 || preserved > 0 || stale > 0) {
				spdlog::info("[TFD][Captive] working captive faction cleared removed={} preserved={} stale={} reason={}",
					removed,
					preserved,
					stale,
					reason ? reason : "unknown");
			}
		}

		static void ApplyNativeWorkingCaptiveFaction(RE::Actor* actor, const char* reason)
		{
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				return;
			}

			auto* faction = ResolveNativeWorkingCaptiveFaction();
			if (!faction) {
				return;
			}

			const RE::FormID formID = actor->GetFormID();
			const bool hadFaction = actor->IsInFaction(faction);
			if (!hadFaction) {
				actor->AddToFaction(faction, 0);
			}

			if (!IsNativeWorkingFactionTracked(formID)) {
				NativeWorkingRoleEntry entry{};
				entry.actor = actor->GetHandle();
				entry.formID = formID;
				entry.addedByTFD = !hadFaction;
				g_nativeWorkingRoleActors.push_back(entry);
			}

			spdlog::info("[TFD][Captive] working captive faction applied actor={:08X} added={} hadFaction={} tracked={} reason={}",
				actor->GetFormID(),
				(!hadFaction) ? 1 : 0,
				hadFaction ? 1 : 0,
				static_cast<unsigned>(g_nativeWorkingRoleActors.size()),
				reason ? reason : "unknown");
		}

		static void RemoveNativeWorkingCaptiveFaction(RE::Actor* actor, const char* reason)
		{
			if (!actor) {
				return;
			}

			auto* faction = ResolveNativeWorkingCaptiveFaction();
			if (!faction) {
				return;
			}

			const RE::FormID formID = actor->GetFormID();
			const bool hadFaction = actor->IsInFaction(faction);
			if (hadFaction) {
				actor->RemoveFromFaction(faction);
			}

			const auto oldSize = g_nativeWorkingRoleActors.size();
			g_nativeWorkingRoleActors.erase(
				std::remove_if(
					g_nativeWorkingRoleActors.begin(),
					g_nativeWorkingRoleActors.end(),
					[formID](const NativeWorkingRoleEntry& entry) { return entry.formID == formID; }),
				g_nativeWorkingRoleActors.end());

			spdlog::info("[TFD][Captive][C46] working captive faction removed non-boss actor={:08X} hadFaction={} removedTracked={} reason={}",
				formID,
				hadFaction ? 1 : 0,
				oldSize != g_nativeWorkingRoleActors.size() ? 1 : 0,
				reason ? reason : "unknown");
		}

		static RE::Actor* LookupActorByFormID(RE::FormID formID)
		{
			if (formID == 0) {
				return nullptr;
			}
			return RE::TESForm::LookupByID<RE::Actor>(formID);
		}

		static std::vector<RE::FormID> GetSnapshotBossActorIDs()
		{
			std::vector<RE::FormID> result{};
			TFD::Location::CaptiveStorageDebugSnapshot snapshot{};
			if (!TFD::Location::GetLastCaptiveStorageDebugSnapshot(snapshot)) {
				return result;
			}

			for (auto formID : snapshot.bossActorFormIDs) {
				if (formID == 0) {
					continue;
				}
				if (std::find(result.begin(), result.end(), formID) == result.end()) {
					result.push_back(formID);
				}
			}
			return result;
		}

		static bool IsActorInBossSnapshot(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			const auto actorID = actor->GetFormID();
			for (auto bossID : GetSnapshotBossActorIDs()) {
				if (bossID == actorID) {
					return true;
				}
			}
			return false;
		}

		static RE::Actor* SelectWorkBossActor(RE::Actor* fallbackActor, RE::FormID excludeFormID, bool allowFallback)
		{
			auto bossIDs = GetSnapshotBossActorIDs();
			for (auto bossID : bossIDs) {
				if (bossID == 0 || bossID == excludeFormID) {
					continue;
				}
				auto* actor = LookupActorByFormID(bossID);
				if (actor && !actor->IsDead() && !actor->IsDisabled()) {
					return actor;
				}
			}

			if (allowFallback) {
				if (fallbackActor && !fallbackActor->IsDead() && !fallbackActor->IsDisabled()) {
					return fallbackActor;
				}
				for (auto bossID : bossIDs) {
					auto* actor = LookupActorByFormID(bossID);
					if (actor && !actor->IsDead() && !actor->IsDisabled()) {
						return actor;
					}
				}
			}

			return nullptr;
		}

		static void WriteCurrentWorkBossAlias(RE::Actor* boss, const char* reason)
		{
			ResolveQuestRegistry();
			WriteQuestAlias(g_registry.bossAlias, boss);
			if (boss) {
				g_currentWorkBoss = boss->GetHandle();
				g_currentWorkBossFormID = boss->GetFormID();
			}
			else {
				g_currentWorkBoss.reset();
				g_currentWorkBossFormID = 0;
			}

			spdlog::info("[TFD][Captive] work boss alias {} actor={:08X} aliasPresent={} reason={}",
				boss ? "assigned" : "cleared",
				boss ? boss->GetFormID() : 0u,
				g_registry.bossAlias ? 1 : 0,
				reason ? reason : "unknown");
		}

		static void ApplyWorkingFactionToCurrentBoss(RE::Actor* boss, const char* reason)
		{
			ClearNativeWorkingCaptiveFaction(reason ? reason : "working_refresh");
			TFD::WorkNative::ClearWorkDemandFactions(reason ? reason : "working_refresh_clear_demand");
			WriteCurrentWorkBossAlias(boss, reason ? reason : "working_refresh");
			if (boss) {
				ApplyNativeWorkingCaptiveFaction(boss, reason);
				TFD::WorkNative::RefreshWorkDemandFactionsForBoss(boss, reason ? reason : "working_refresh_apply_demand");
			}
			spdlog::info("[TFD][Captive] working faction current boss refresh actor={:08X} reason={}",
				boss ? boss->GetFormID() : 0u,
				reason ? reason : "unknown");
		}

		static void ApplyWorkingFactionToBossActors(RE::Actor* fallbackActor, const char* reason)
		{
			RE::Actor* preferred = nullptr;
			if (fallbackActor && IsActorInBossSnapshot(fallbackActor)) {
				preferred = fallbackActor;
			}
			else {
				preferred = SelectWorkBossActor(nullptr, 0, true);
			}
			if (!preferred) {
				preferred = fallbackActor;
			}

			g_workBossResumeAt = {};
			ApplyWorkingFactionToCurrentBoss(preferred, reason ? reason : "released_work_runtime");
		}

		static void ProcessReleasedWorkBossResume(const char* reason)
		{
			if (!g_state || g_phase != PhaseValue::ReleasedWork) {
				return;
			}
			if (g_workBossResumeAt.time_since_epoch().count() == 0) {
				return;
			}
			if (Now() < g_workBossResumeAt) {
				return;
			}

			g_workBossResumeAt = {};
			RE::Actor* boss = LookupActorByFormID(g_currentWorkBossFormID);
			if (!boss) {
				boss = SelectWorkBossActor(nullptr, 0, true);
			}
			(void)TFD::Location::RefreshCaptiveWorkResourceState(true, reason ? reason : "work_no_job_cooldown_resume");
			SyncCaptiveWorkResourceAliases(reason ? reason : "work_no_job_cooldown_resume");
			ApplyWorkingFactionToCurrentBoss(boss, reason ? reason : "work_no_job_cooldown_resume");
			ForceWorkGlobals(0, 1, "work_no_job_cooldown_resume");
		}

		static bool ApplyNativeCaptorRoleFaction(RE::Actor* actor, const char* reason)
		{
			if (!actor) {
				return false;
			}

			auto* faction = ResolveNativeCaptiveFaction();
			if (!faction) {
				return false;
			}

			const RE::FormID formID = actor->GetFormID();
			const bool hadFaction = actor->IsInFaction(faction);
			const bool trackedBefore = IsNativeCaptiveFactionTracked(formID);
			if (!hadFaction) {
				actor->AddToFaction(faction, 0);
			}

			if (!trackedBefore) {
				NativeCaptiveRoleEntry entry{};
				entry.actor = actor->GetHandle();
				entry.formID = formID;
				entry.addedByTFD = !hadFaction;
				g_nativeCaptiveRoleActors.push_back(entry);
			}

			const bool changed = !hadFaction || !trackedBefore;
			const bool quietNoop = !changed;
			if (!quietNoop) {
				spdlog::info("[TFD][Captive] native captive role faction applied actor={:08X} added={} hadFaction={} tracked={} reason={}",
					actor->GetFormID(),
					(!hadFaction) ? 1 : 0,
					hadFaction ? 1 : 0,
					static_cast<unsigned>(g_nativeCaptiveRoleActors.size()),
					reason ? reason : "unknown");
			}

			return changed;
		}

		static bool RemoveNativeCaptorRoleFactionForActor(RE::Actor* actor, const char* reason)
		{
			if (!actor) {
				return false;
			}

			auto* faction = ResolveNativeCaptiveFaction();
			if (!faction) {
				return false;
			}

			const RE::FormID formID = actor->GetFormID();
			bool removedFaction = false;
			bool removedTracked = false;
			bool preservedExternal = false;

			g_nativeCaptiveRoleActors.erase(
				std::remove_if(
					g_nativeCaptiveRoleActors.begin(),
					g_nativeCaptiveRoleActors.end(),
					[&](const NativeCaptiveRoleEntry& entry) {
						if (entry.formID != formID) {
							return false;
						}
						removedTracked = true;
						if (entry.addedByTFD && actor->IsInFaction(faction)) {
							actor->RemoveFromFaction(faction);
							removedFaction = true;
						}
						else if (actor->IsInFaction(faction)) {
							preservedExternal = true;
						}
						return true;
					}),
				g_nativeCaptiveRoleActors.end());

			if (removedTracked || removedFaction || preservedExternal) {
				spdlog::info("[TFD][Captive][R221A] native captive role faction skipped/removed for combat break actor={:08X} removedFaction={} removedTracked={} preservedExternal={} reason={}",
					formID,
					removedFaction ? 1 : 0,
					removedTracked ? 1 : 0,
					preservedExternal ? 1 : 0,
					reason ? reason : "unknown");
			}

			return removedFaction || removedTracked;
		}

		static bool ShouldSkipNativeCaptiveRoleApplyForCombatBreak(RE::Actor* actor, RE::Actor* player, const char* reason)
		{
			if (!actor || !player || !reason || std::strcmp(reason, "captive_runtime_tick") != 0) {
				return false;
			}

			auto& flow = TFD::FlowController::Controller::GetSingleton();
			const auto snapshot = flow.GetSnapshot();
			const bool phaseEscape = g_phase == PhaseValue::Escape;
			const bool inCombatEscapeBreak = snapshot.contextRoot == TFD::FlowController::RootFlow::Captive &&
				snapshot.sub == TFD::FlowController::SubFlow::InCombatEscapeBreak;
			const bool bleedoutEscapeBreak = snapshot.contextRoot == TFD::FlowController::RootFlow::Captive &&
				snapshot.sub == TFD::FlowController::SubFlow::BleedoutEscapeBreak;
			const bool captiveThreatOverlay = inCombatEscapeBreak || bleedoutEscapeBreak;

			if (!phaseEscape && !captiveThreatOverlay) {
				return false;
			}

			auto target = actor->GetActorRuntimeData().currentCombatTarget.get();
			const bool targetingPlayer = target && target->GetFormID() == player->GetFormID();
			const bool hostile = actor->IsHostileToActor(player);
			const bool inCombat = actor->IsInCombat();
			const bool weaponDrawn = actor->IsWeaponDrawn();
			const bool primaryEscapeBreak = captiveThreatOverlay && snapshot.primaryActorFormID == actor->GetFormID();

			// R271A: Captive is the base context, while InCombatEscapeBreak and
			// BleedoutEscapeBreak are threat overlays above it. During those overlays
			// Captive runtime must not refresh TFDCaptiveFaction/package ownership;
			// combat or bleedout owns hostility, stance, and forcegreet settlement.
			if (captiveThreatOverlay) {
				const bool removed = RemoveNativeCaptorRoleFactionForActor(actor,
					bleedoutEscapeBreak ? "captive_runtime_tick_bleedout_escape_break_suspend" : "captive_runtime_tick_incombat_escape_break_suspend");
				spdlog::info("[TFD][Captive][R271A] native captive role apply suspended for captive threat overlay actor={:08X} overlay={} phaseEscape={} hostile={} targetingPlayer={} inCombat={} weaponDrawn={} primary={} removed={} reason={}",
					actor->GetFormID(),
					bleedoutEscapeBreak ? "BleedoutEscapeBreak" : "InCombatEscapeBreak",
					phaseEscape ? 1 : 0,
					hostile ? 1 : 0,
					targetingPlayer ? 1 : 0,
					inCombat ? 1 : 0,
					weaponDrawn ? 1 : 0,
					primaryEscapeBreak ? 1 : 0,
					removed ? 1 : 0,
					reason ? reason : "unknown");
				return true;
			}

			const bool activeCombatOwner = targetingPlayer || inCombat || hostile || weaponDrawn;
			if (!activeCombatOwner) {
				return false;
			}

			(void)RemoveNativeCaptorRoleFactionForActor(actor, "captive_runtime_tick_escape_phase_combat_owner");
			spdlog::info("[TFD][Captive][R271A] native captive role apply skipped for escape phase combat owner actor={:08X} phaseEscape={} hostile={} targetingPlayer={} inCombat={} weaponDrawn={} reason={}",
				actor->GetFormID(),
				phaseEscape ? 1 : 0,
				hostile ? 1 : 0,
				targetingPlayer ? 1 : 0,
				inCombat ? 1 : 0,
				weaponDrawn ? 1 : 0,
				reason ? reason : "unknown");

			return true;
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
			std::uint32_t combatBreakSkipped = 0;

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
				if (ShouldSkipNativeCaptiveRoleApplyForCombatBreak(actor, player, reason)) {
					++combatBreakSkipped;
					continue;
				}
				if (ApplyNativeCaptorRoleFaction(actor, reason ? reason : "native_captive_location")) {
					++applied;
				}
			}

			const bool quietNoopSweep = applied == 0 && !force;
			if (quietNoopSweep) {
				if (g_nextCaptiveRoleFactionNoopLog != std::chrono::steady_clock::time_point{} && now < g_nextCaptiveRoleFactionNoopLog) {
					return;
				}
				g_nextCaptiveRoleFactionNoopLog = now + kCaptiveRoleFactionNoopLogInterval;
			}

			spdlog::info("[TFD][Captive][R164] native captive location faction sweep applied={} inScope={} considered={} skippedCombatBreak={} tracked={} loc={:08X} reason={}{}",
				applied,
				inScope,
				considered,
				combatBreakSkipped,
				static_cast<unsigned>(g_nativeCaptiveRoleActors.size()),
				g_locationFormID,
				reason ? reason : "unknown",
				quietNoopSweep ? " throttle=noop" : "");
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

	bool IsReleasedWorkActive()
	{
		return g_state && g_phase == PhaseValue::ReleasedWork;
	}

	RE::Actor* GetCurrentWorkBoss()
	{
		if (!IsReleasedWorkActive()) {
			return nullptr;
		}

		if (g_currentWorkBoss) {
			auto sp = RE::Actor::LookupByHandle(g_currentWorkBoss.native_handle());
			if (sp) {
				return sp.get();
			}
		}

		if (g_currentWorkBossFormID != 0) {
			return LookupActorByFormID(g_currentWorkBossFormID);
		}

		return nullptr;
	}

	bool IsCurrentWorkBoss(RE::Actor* actor)
	{
		auto* boss = GetCurrentWorkBoss();
		return actor && boss && actor->GetFormID() == boss->GetFormID();
	}

	bool IsReleasedWorkActorInScope(RE::Actor* actor)
	{
		if (!IsReleasedWorkActive() || !actor) {
			return false;
		}
		auto* player = Player();
		if (!player) {
			return false;
		}
		return IsActorInNativeCaptiveLocationScope(actor, player);
	}

	static bool ShouldBlockReleasedWorkDialogueForEscapeBreak(RE::Actor* actor, const char* reason)
	{
		if (!actor) {
			return false;
		}

		auto* player = Player();
		auto& flow = TFD::FlowController::Controller::GetSingleton();
		const auto snapshot = flow.GetSnapshot();
		const bool inCombatEscapeBreak = snapshot.contextRoot == TFD::FlowController::RootFlow::Captive &&
			snapshot.sub == TFD::FlowController::SubFlow::InCombatEscapeBreak;
		if (!inCombatEscapeBreak || !player) {
			return false;
		}

		auto target = actor->GetActorRuntimeData().currentCombatTarget.get();
		const bool targetingPlayer = target && target->GetFormID() == player->GetFormID();
		const bool hostile = actor->IsHostileToActor(player);
		const bool inCombat = actor->IsInCombat();
		const bool weaponDrawn = actor->IsWeaponDrawn();
		const bool primaryInCombatEscapeBreak = snapshot.primaryActorFormID == actor->GetFormID();
		const bool shouldBlock = targetingPlayer || inCombat || primaryInCombatEscapeBreak;

		if (shouldBlock) {
			spdlog::info("[TFD][Captive][R230A] work dialogue actor prepare blocked for active escape-break combat actor={:08X} hostile={} targetingPlayer={} inCombat={} weaponDrawn={} primary={} reason={}",
				actor->GetFormID(),
				hostile ? 1 : 0,
				targetingPlayer ? 1 : 0,
				inCombat ? 1 : 0,
				weaponDrawn ? 1 : 0,
				primaryInCombatEscapeBreak ? 1 : 0,
				reason ? reason : "released_work_dialogue_actor");
		}

		return shouldBlock;
	}

	void ClearReleasedWorkDialogueActorForCombatBreak(RE::Actor* actor, const char* reason)
	{
		if (!actor) {
			return;
		}

		const char* useReason = reason ? reason : "released_work_combat_break";
		RemoveNativeWorkingCaptiveFaction(actor, useReason);
		actor->SetDialogueWithPlayer(false, false, nullptr);
		spdlog::info("[TFD][Captive][R222A] work dialogue actor cleared for escape-break combat actor={:08X} reason={} action=no_stop_combat",
			actor->GetFormID(),
			useReason);
	}

	bool EnsureReleasedWorkDialogueActor(RE::Actor* actor, const char* reason)
	{
		const char* useReason = reason ? reason : "released_work_dialogue_actor";
		if (!IsReleasedWorkActive() || !actor || actor->IsDead() || actor->IsDisabled()) {
			spdlog::info("[TFD][Captive] work dialogue actor prepare rejected actor={:08X} active={} dead={} disabled={} reason={}",
				actor ? actor->GetFormID() : 0u,
				IsReleasedWorkActive() ? 1 : 0,
				actor && actor->IsDead() ? 1 : 0,
				actor && actor->IsDisabled() ? 1 : 0,
				useReason);
			return false;
		}

		if (!IsReleasedWorkActorInScope(actor)) {
			spdlog::info("[TFD][Captive] work dialogue actor prepare rejected out_of_scope actor={:08X} reason={}",
				actor->GetFormID(),
				useReason);
			return false;
		}

		if (ShouldBlockReleasedWorkDialogueForEscapeBreak(actor, useReason)) {
			ClearReleasedWorkDialogueActorForCombatBreak(actor, useReason);
			return false;
		}

		// WorkBoss owns the quest objective and marker.  Do not let random
		// in-scope captors become job givers by receiving TFDWorkingCaptiveFaction.
		if (!IsCurrentWorkBoss(actor)) {
			RemoveNativeWorkingCaptiveFaction(actor, useReason);
			actor->SetDialogueWithPlayer(false, false, nullptr);
			spdlog::info("[TFD][Captive][C46] work dialogue actor rejected non-boss actor={:08X} currentBoss={:08X} reason={}",
				actor->GetFormID(),
				g_currentWorkBossFormID,
				useReason);
			return false;
		}

		ApplyNativeWorkingCaptiveFaction(actor, useReason);

		if (!actor->IsAIEnabled()) {
			actor->EnableAI(true);
		}
		actor->AllowPCDialogue(true);
		actor->StopCombat();
		if (auto* process = RE::ProcessLists::GetSingleton()) {
			process->StopCombatAndAlarmOnActor(actor, false);
		}
		if (actor->IsWeaponDrawn()) {
			// R244A: no forced weapon stance; Skyrim handles sheath/draw naturally. Disabled: actor->DrawWeaponMagicHands(false);
		}
		actor->EvaluatePackage(false, true);
		actor->EvaluatePackage(true, true);

		auto* boss = GetCurrentWorkBoss();
		spdlog::info("[TFD][Captive] work dialogue actor prepared actor={:08X} currentBoss={:08X} isBoss={} markerStable=1 reason={}",
			actor->GetFormID(),
			boss ? boss->GetFormID() : 0u,
			boss && boss->GetFormID() == actor->GetFormID() ? 1 : 0,
			useReason);
		return true;
	}

	bool PromoteReleasedWorkBoss(RE::Actor* actor, const char* reason)
	{
		const char* useReason = reason ? reason : "released_work_promote_boss";
		if (!IsReleasedWorkActive() || !actor || actor->IsDead() || actor->IsDisabled()) {
			spdlog::info("[TFD][Captive] work boss promote rejected actor={:08X} active={} dead={} disabled={} reason={}",
				actor ? actor->GetFormID() : 0u,
				IsReleasedWorkActive() ? 1 : 0,
				actor && actor->IsDead() ? 1 : 0,
				actor && actor->IsDisabled() ? 1 : 0,
				useReason);
			return false;
		}

		if (!IsReleasedWorkActorInScope(actor)) {
			spdlog::info("[TFD][Captive] work boss promote rejected out_of_scope actor={:08X} reason={}",
				actor->GetFormID(),
				useReason);
			return false;
		}

		g_workBossResumeAt = {};
		ApplyWorkingFactionToCurrentBoss(actor, useReason);
		if (!actor->IsAIEnabled()) {
			actor->EnableAI(true);
		}
		actor->AllowPCDialogue(true);
		actor->StopCombat();
		if (auto* process = RE::ProcessLists::GetSingleton()) {
			process->StopCombatAndAlarmOnActor(actor, false);
		}
		if (actor->IsWeaponDrawn()) {
			// R244A: no forced weapon stance; Skyrim handles sheath/draw naturally. Disabled: actor->DrawWeaponMagicHands(false);
		}
		actor->EvaluatePackage(false, true);
		actor->EvaluatePackage(true, true);

		spdlog::info("[TFD][Captive] work boss promoted actor={:08X} reason={}", actor->GetFormID(), useReason);
		return true;
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

	RE::Actor* ResolveEscapeBreakPreferredAggressor(float radius, const std::function<RE::Actor* (float)>& fallbackResolver)
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

	void QueueLoadedWorkSession(std::uint32_t bossFormID, std::uint32_t jobType, std::uint32_t assignmentState)
	{
		g_queuedWorkBossFormID = bossFormID;
		g_queuedWorkJobType = jobType;
		g_queuedWorkAssignmentState = assignmentState;
		spdlog::info("[TFD][Captive] QueueLoadedWorkSession boss={:08X} job={} assignment={}",
			g_queuedWorkBossFormID,
			g_queuedWorkJobType,
			g_queuedWorkAssignmentState);
	}

	std::uint32_t GetWorkBossFormIDForSave()
	{
		if (g_currentWorkBossFormID != 0) {
			return g_currentWorkBossFormID;
		}
		if (auto* boss = GetCurrentWorkBoss()) {
			return boss->GetFormID();
		}
		return 0;
	}

	std::uint32_t GetWorkJobTypeForSave()
	{
		return ReadGlobalUInt(ResolveWorkJobTypeGlobal());
	}

	std::uint32_t GetWorkAssignmentStateForSave()
	{
		return ReadGlobalUInt(ResolveWorkAssignmentStateGlobal());
	}

	void ClearQueuedLoadedState()
	{
		g_queuedState = false;
		g_queuedPhase = PhaseValue::None;
		g_queuedWorkBossFormID = 0;
		g_queuedWorkJobType = 0;
		g_queuedWorkAssignmentState = 0;
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
			if (aliasName == "ApproachPoint") {
				g_registry.approachPointAlias = refAlias;
				continue;
			}
			if (aliasName == "EscapeRoute") {
				g_registry.escapeRouteAlias = refAlias;
				continue;
			}
			if (aliasName == "LootTarget") {
				g_registry.lootTargetAlias = refAlias;
				continue;
			}
			if (aliasName == "Boss") {
				g_registry.bossAlias = refAlias;
				continue;
			}
			if (aliasName == "Mine" || aliasName == "WorkMine") {
				g_registry.workMineAlias = refAlias;
				continue;
			}
			if (aliasName == "CraftingStation" || aliasName == "WorkCraftingStation") {
				g_registry.workCraftingStationAlias = refAlias;
				continue;
			}
			for (std::size_t station = 1; station < kWorkCraftingStationAliasNames.size(); ++station) {
				if (aliasName == kWorkCraftingStationAliasNames[station]) {
					g_registry.workCraftingStationAliases[station] = refAlias;
					break;
				}
			}
			if (std::find(g_registry.workCraftingStationAliases.begin(), g_registry.workCraftingStationAliases.end(), refAlias) != g_registry.workCraftingStationAliases.end()) {
				continue;
			}
			if (aliasName == "Item") {
				g_registry.workItemAlias = refAlias;
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
				}
				catch (...) {}
				continue;
			}
			if (aliasName.rfind("BossContainer", 0) == 0 && aliasName.size() >= 14) {
				try {
					int slot = std::stoi(aliasName.substr(13));
					if (slot >= 1 && slot <= static_cast<int>(g_registry.bossContainerAliases.size())) {
						g_registry.bossContainerAliases[static_cast<std::size_t>(slot) - 1] = refAlias;
					}
				}
				catch (...) {}
				continue;
			}
			if (aliasName.rfind("Container", 0) == 0 && aliasName.size() >= 10) {
				try {
					int slot = std::stoi(aliasName.substr(9));
					if (slot >= 1 && slot <= static_cast<int>(g_registry.containerAliases.size())) {
						g_registry.containerAliases[static_cast<std::size_t>(slot) - 1] = refAlias;
					}
				}
				catch (...) {}
				continue;
			}
		}

		std::size_t bossCaptorCount = 0;
		std::size_t bossContainerCount = 0;
		std::size_t containerCount = 0;
		std::size_t workStationAliasCount = 0;
		for (auto* alias : g_registry.bossCaptorAliases) { if (alias) ++bossCaptorCount; }
		for (auto* alias : g_registry.bossContainerAliases) { if (alias) ++bossContainerCount; }
		for (auto* alias : g_registry.containerAliases) { if (alias) ++containerCount; }
		for (std::size_t station = 1; station < g_registry.workCraftingStationAliases.size(); ++station) { if (g_registry.workCraftingStationAliases[station]) ++workStationAliasCount; }

		spdlog::info("[TFD][Captive][W17] captive quest registry resolved quest={:08X} playerAliasID={} captiveMarkerAlias={} escapeDoorAlias={} approachPointAlias={} escapeRouteAlias={} bossAlias={} mineAlias={} craftingAlias={} workStationAliases={} itemAlias={} bossCaptorAliases={} bossContainerAliases={} containerAliases={} lootTarget={}",
			g_registry.quest ? g_registry.quest->GetFormID() : 0u,
			g_registry.playerCaptiveAlias ? g_registry.playerCaptiveAlias->aliasID : static_cast<std::uint32_t>(0),
			g_registry.captiveMarkerAlias ? g_registry.captiveMarkerAlias->aliasID : static_cast<std::uint32_t>(0),
			g_registry.escapeDoorAlias ? g_registry.escapeDoorAlias->aliasID : static_cast<std::uint32_t>(0),
			g_registry.approachPointAlias ? g_registry.approachPointAlias->aliasID : static_cast<std::uint32_t>(0),
			g_registry.escapeRouteAlias ? g_registry.escapeRouteAlias->aliasID : static_cast<std::uint32_t>(0),
			g_registry.bossAlias ? 1 : 0,
			g_registry.workMineAlias ? 1 : 0,
			g_registry.workCraftingStationAlias ? 1 : 0,
			workStationAliasCount,
			g_registry.workItemAlias ? 1 : 0,
			bossCaptorCount,
			bossContainerCount,
			containerCount,
			g_registry.lootTargetAlias ? 1 : 0);

		if (!g_registry.playerCaptiveAlias) {
			spdlog::info("[TFD][Captive] PlayerCaptive alias not present in TFDCaptiveQuest; using ESP bridge aliases for captor/storage diagnostics");
		}
	}

	void WriteQuestAlias(RE::BGSRefAlias* alias, RE::TESObjectREFR* ref, const char* reason)
	{
		ResolveQuestRegistry();
		if (!g_registry.quest || !alias) {
			return;
		}

		const bool isOwnerCaptorAlias = g_registry.bossCaptorAliases[0] && alias == g_registry.bossCaptorAliases[0];
		auto* beforeRef = isOwnerCaptorAlias ? alias->GetReference() : nullptr;

		RE::ObjectRefHandle handle{};
		if (ref) {
			handle = ref->CreateRefHandle();
		}

		{
			RE::BSWriteLockGuard lock(g_registry.quest->aliasAccessLock);
			auto it = g_registry.quest->refAliasMap.find(alias->aliasID);
			if (ref) {
				if (it != g_registry.quest->refAliasMap.end()) {
					it->second = handle;
				}
				else {
					g_registry.quest->refAliasMap.insert({ alias->aliasID, handle });
				}
			}
			else if (it != g_registry.quest->refAliasMap.end()) {
				g_registry.quest->refAliasMap.erase(it);
			}
		}

		if (isOwnerCaptorAlias) {
			auto* afterRef = alias->GetReference();
			spdlog::info("[TFD][Captive][OwnerCaptorTrace] source=native action={} reason={} before={:08X} after={:08X} requested={:08X}",
				ref ? "force" : "clear",
				reason ? reason : "write_quest_alias",
				beforeRef ? beforeRef->GetFormID() : 0u,
				afterRef ? afterRef->GetFormID() : 0u,
				ref ? ref->GetFormID() : 0u);
		}
	}


	RE::TESObjectREFR* ResolveRecoverGearLootTarget()
	{
		ResolveQuestRegistry();

		if (g_registry.lootTargetAlias) {
			if (auto* aliasRef = g_registry.lootTargetAlias->GetReference()) {
				return aliasRef;
			}
		}

		TFD::Location::CaptiveStorageDebugSnapshot snapshot{};
		if (TFD::Location::GetLastCaptiveStorageDebugSnapshot(snapshot) && snapshot.finalTargetFormID != 0) {
			return RE::TESForm::LookupByID<RE::TESObjectREFR>(snapshot.finalTargetFormID);
		}

		return nullptr;
	}

	bool IsRecoverGearLootTarget(RE::TESObjectREFR* ref)
	{
		if (!ref) {
			return false;
		}

		auto* target = ResolveRecoverGearLootTarget();
		if (target && target == ref) {
			return true;
		}

		TFD::Location::CaptiveStorageDebugSnapshot snapshot{};
		if (TFD::Location::GetLastCaptiveStorageDebugSnapshot(snapshot)) {
			const auto refID = ref->GetFormID();
			if (snapshot.finalTargetFormID != 0 && refID == snapshot.finalTargetFormID) {
				return true;
			}
			for (const auto id : snapshot.bossContainerFormIDs) {
				if (id != 0 && refID == id) {
					return true;
				}
			}
			for (const auto id : snapshot.containerFormIDs) {
				if (id != 0 && refID == id) {
					return true;
				}
			}
		}

		return false;
	}

	bool NotifyRecoverGearContainerOpened(RE::TESObjectREFR* ref, const char* reason)
	{
		if (!ref) {
			return false;
		}

		if (!IsRecoverGearLootTarget(ref)) {
			return false;
		}

		const auto now = Now();
		const auto refID = ref->GetFormID();
		if (g_recoverGearStashActive &&
			!g_recoverGearStashOpened &&
			(g_recoverGearStashTargetFormID == 0 || g_recoverGearStashTargetFormID == refID)) {
			g_recoverGearStashOpened = true;
			spdlog::info("[TFD][Captive][C47] recover gear stash marked opened ref={:08X} cycle={} reason={}",
				refID,
				g_stashCycleID,
				reason ? reason : "unknown");
		}
		if (g_lastRecoverGearOpenNotifyRefID == refID &&
			g_lastRecoverGearOpenNotifyAt != std::chrono::steady_clock::time_point{} &&
			now < g_lastRecoverGearOpenNotifyAt + kRecoverGearOpenNotifyCooldown) {
			spdlog::info("[TFD][Captive] recover gear container open notify suppressed duplicate ref={:08X} reason={}",
				refID,
				reason ? reason : "unknown");
			return true;
		}

		g_lastRecoverGearOpenNotifyRefID = refID;
		g_lastRecoverGearOpenNotifyAt = now;

		const bool directSent = SendBridgeFormEvent(kRecoverGearContainerOpenedEvent, ref, reason ? reason : "container_open", 0.0f);
		const bool queued = TFD::FlowController::QueueBridgeModEvent(
			kRecoverGearContainerOpenedEvent,
			ref,
			reason ? reason : "container_open",
			0.0f);

		spdlog::info("[TFD][Captive] recover gear container opened ref={:08X} reason={} direct={} queued={} state={} phase={}",
			refID,
			reason ? reason : "unknown",
			directSent ? 1 : 0,
			queued ? 1 : 0,
			StateRef() ? 1 : 0,
			static_cast<int>(PhaseRef()));

		return directSent || queued;
	}



	static float DistanceSquaredRefs(RE::TESObjectREFR* a, RE::TESObjectREFR* b)
	{
		if (!a || !b) {
			return std::numeric_limits<float>::max();
		}

		const auto pa = a->GetPosition();
		const auto pb = b->GetPosition();
		const float dx = pa.x - pb.x;
		const float dy = pa.y - pb.y;
		const float dz = pa.z - pb.z;
		return (dx * dx) + (dy * dy) + (dz * dz);
	}


	static int ReadDiagnosticGlobal(const char* editorID)
	{
		if (!editorID || !editorID[0]) {
			return -9999;
		}

		auto* global = RE::TESForm::LookupByEditorID<RE::TESGlobal>(editorID);
		if (!global) {
			return -9999;
		}
		return static_cast<int>(std::lround(global->value));
	}

	static RE::TESObjectREFR* ReadAliasRefForDiagnostic(RE::BGSRefAlias* alias)
	{
		if (!alias) {
			return nullptr;
		}

		return alias->GetReference();
	}

	static std::uint32_t RefIDForDiagnostic(RE::TESObjectREFR* ref)
	{
		return ref ? ref->GetFormID() : 0u;
	}


	void LogCallingCaptorOwnershipSnapshot(RE::Actor* actor, const char* reason)
	{
		ResolveQuestRegistry();

		auto* player = Player();
		auto* ownerRef = ReadAliasRefForDiagnostic(g_registry.bossCaptorAliases[0]);
		auto* ownerActor = ownerRef ? ownerRef->As<RE::Actor>() : nullptr;
		auto* targetRef = ResolveCaptorApproachTarget(player);
		auto* bossRef = ReadAliasRefForDiagnostic(g_registry.bossAlias);
		auto* markerRef = ReadAliasRefForDiagnostic(g_registry.captiveMarkerAlias);

		const float playerDist = (actor && player) ? std::sqrt(DistanceSquaredRefs(actor, player)) : -1.0f;
		const float targetDist = (actor && targetRef) ? std::sqrt(DistanceSquaredRefs(actor, targetRef)) : -1.0f;

		auto* actorCell = actor ? actor->GetParentCell() : nullptr;
		auto* playerCell = player ? player->GetParentCell() : nullptr;
		const bool sameCell = actorCell && playerCell && actorCell == playerCell;
		const bool sameSpace = actor && player && IsActorSameSpace(actor, player);
		const bool ownerMatches = actor && ownerActor && ownerActor == actor;
		const bool targetIsPlayer = player && targetRef && targetRef == player;
		const char* targetName = GetCaptorApproachTargetName(targetRef, player);
		const bool hasCaptiveRole = actor && ActorHasNativeCaptiveRole(actor);
		const bool suppressed = actor && TFD::HostilityController::IsActorTemporarilySuppressed(actor);
		const bool canOpenDialogue = actor && TFD::HostilityController::CanOpenDialogue(actor);
		const bool hostile = actor && player && actor->IsHostileToActor(player);

		spdlog::info(
			"[TFD][Captive][CallDiag] reason={} actor={:08X} owner={:08X} ownerMatch={} boss={:08X} movementTarget={} target={:08X} targetIsPlayer={} marker={:08X} globals[captive={} pre={} in={} defeat={} victory={} dialogue={}] actorState[dead={} disabled={} loaded3d={} ai={} combat={} hostile={} weapon={} suppressed={} captiveRole={} canOpenDialogue={}] dist[player={:.1f} target={:.1f}] cell[actor={:08X} player={:08X} sameCell={} sameSpace={}]",
			reason ? reason : "unknown",
			actor ? actor->GetFormID() : 0u,
			RefIDForDiagnostic(ownerRef),
			ownerMatches ? 1 : 0,
			RefIDForDiagnostic(bossRef),
			targetName,
			RefIDForDiagnostic(targetRef),
			targetIsPlayer ? 1 : 0,
			RefIDForDiagnostic(markerRef),
			ReadDiagnosticGlobal("TFDCaptiveState"),
			ReadDiagnosticGlobal("TFDPreCombatState"),
			ReadDiagnosticGlobal("TFDInCombatState"),
			ReadDiagnosticGlobal("TFDDefeatState"),
			ReadDiagnosticGlobal("TFDVictoryState"),
			ReadDiagnosticGlobal("TFDDialogueState"),
			(actor && actor->IsDead()) ? 1 : 0,
			(actor && actor->IsDisabled()) ? 1 : 0,
			(actor && actor->Is3DLoaded()) ? 1 : 0,
			(actor && actor->IsAIEnabled()) ? 1 : 0,
			(actor && actor->IsInCombat()) ? 1 : 0,
			hostile ? 1 : 0,
			(actor && actor->IsWeaponDrawn()) ? 1 : 0,
			suppressed ? 1 : 0,
			hasCaptiveRole ? 1 : 0,
			canOpenDialogue ? 1 : 0,
			playerDist,
			targetDist,
			actorCell ? actorCell->GetFormID() : 0u,
			playerCell ? playerCell->GetFormID() : 0u,
			sameCell ? 1 : 0,
			sameSpace ? 1 : 0);
	}


	void RefreshCaptorApproachAI(RE::Actor* actor, const char* reason)
	{
		if (!actor || actor->IsDead() || actor->IsDisabled()) {
			return;
		}

		if (!actor->IsAIEnabled()) {
			actor->EnableAI(true);
		}

		actor->AllowPCDialogue(true);
		if (actor->IsInCombat()) {
			actor->StopCombat();
		}
		if (actor->IsWeaponDrawn()) {
			// R244A: no forced weapon stance; Skyrim handles sheath/draw naturally. Disabled: actor->DrawWeaponMagicHands(false);
		}

		auto* player = Player();
		auto* targetRef = ResolveCaptorApproachTarget(player);
		const char* targetName = GetCaptorApproachTargetName(targetRef, player);
		LogCallingCaptorOwnershipSnapshot(actor, reason ? reason : "refresh_before_eval");
		actor->EvaluatePackage(false, true);
		LogCallingCaptorOwnershipSnapshot(actor, "refresh_after_eval_soft");
		actor->EvaluatePackage(true, true);
		LogCallingCaptorOwnershipSnapshot(actor, "refresh_after_eval_hard");
		spdlog::info("[TFD][Captive] captor approach package refreshed actor={:08X} movementTarget={} target={:08X} reason={}",
			actor->GetFormID(),
			targetName,
			targetRef ? targetRef->GetFormID() : 0u,
			reason ? reason : "unknown");
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

		WriteQuestAlias(g_registry.playerCaptiveAlias, actor, reason ? reason : "set_player_captive_alias");
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

		assignByFormID(g_registry.bossAlias, hasSnapshot ? snapshot.bossActorFormIDs[0] : 0u);
		LogCallingCaptorOwnershipSnapshot(nullptr, "sync_storage_before_owner_clear");

		// OwnerCaptor is not a passive storage/debug alias.  It drives the
		// TFDCaptiveApproach package and must stay empty until the player
		// explicitly uses Calling Captor.  Older code mirrored the location boss
		// snapshot into bossCaptorAliases[0] (OwnerCaptor), which let Actor A own
		// the approach package before the hotkey, then Actor B had to steal the
		// same alias during Calling Captor.  Keep Boss for work/storage diagnostics
		// and clear OwnerCaptor/BossCaptor slots here so fresh-game ownership starts
		// from a clean alias.
		for (auto* alias : g_registry.bossCaptorAliases) {
			WriteQuestAlias(alias, nullptr, reason ? reason : "sync_storage_owner_clear");
		}
		LogCallingCaptorOwnershipSnapshot(nullptr, "sync_storage_after_owner_clear");

		for (std::size_t i = 0; i < g_registry.bossContainerAliases.size(); ++i) {
			assignByFormID(g_registry.bossContainerAliases[i], hasSnapshot ? snapshot.bossContainerFormIDs[i] : 0u);
		}
		for (std::size_t i = 0; i < g_registry.containerAliases.size(); ++i) {
			assignByFormID(g_registry.containerAliases[i], hasSnapshot ? snapshot.containerFormIDs[i] : 0u);
		}
		assignByFormID(g_registry.lootTargetAlias, hasSnapshot ? snapshot.finalTargetFormID : 0u);

		spdlog::info("[TFD][Captive] debug aliases synced reason={} hasSnapshot={} boss1={:08X} ownerCaptorCleared={} bossContainer1={:08X} container1={:08X} lootTarget={:08X} targetKind={}",
			reason ? reason : "unknown",
			hasSnapshot ? 1 : 0,
			hasSnapshot ? snapshot.bossActorFormIDs[0] : 0u,
			g_registry.bossCaptorAliases[0] ? 1 : 0,
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
			WriteQuestAlias(alias, nullptr, reason ? reason : "clear_storage_owner_clear");
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

	void SyncCaptiveWorkResourceAliases(const char* reason)
	{
		ResolveQuestRegistry();
		if (!g_registry.quest) {
			return;
		}

		auto* mineRef = TFD::Location::GetLastCaptiveWorkMiningRef();
		auto* craftRef = TFD::Location::GetLastCaptiveWorkCraftingRef();
		const RE::FormID mineRefId = mineRef ? mineRef->GetFormID() : 0u;
		const RE::FormID craftingRefId = craftRef ? craftRef->GetFormID() : 0u;

		std::array<RE::TESObjectREFR*, 10> stationRefs{};
		std::array<RE::FormID, 10> stationRefIds{};
		std::uint32_t stationAliasCount = 0;
		for (std::size_t station = 1; station < g_registry.workCraftingStationAliases.size(); ++station) {
			if (g_registry.workCraftingStationAliases[station]) {
				++stationAliasCount;
			}
			stationRefs[station] = TFD::Location::GetLastCaptiveWorkCraftingRefForState(static_cast<int>(station));
			stationRefIds[station] = stationRefs[station] ? stationRefs[station]->GetFormID() : 0u;
		}

		const bool releasedWorkTick = ReasonEquals(reason, "released_work_tick");
		const bool workUpdateTick = ReasonEquals(reason, "work_update");
		const bool unchanged = WorkResourceAliasSnapshotMatches(mineRefId, craftingRefId, stationRefIds);
		const auto now = Now();
		if ((releasedWorkTick || workUpdateTick) && unchanged &&
			g_nextReleasedWorkAliasSelfHeal != std::chrono::steady_clock::time_point{} &&
			now < g_nextReleasedWorkAliasSelfHeal) {
			return;
		}

		g_nextReleasedWorkAliasSelfHeal = now + kReleasedWorkAliasSelfHealInterval;

		WriteQuestAlias(g_registry.workMineAlias, mineRef, reason ? reason : "sync_work_mine_alias");
		WriteQuestAlias(g_registry.workCraftingStationAlias, craftRef, reason ? reason : "sync_work_crafting_alias");
		for (std::size_t station = 1; station < g_registry.workCraftingStationAliases.size(); ++station) {
			WriteQuestAlias(g_registry.workCraftingStationAliases[station], stationRefs[station], reason ? reason : "sync_work_station_alias");
		}

		RememberWorkResourceAliasSnapshot(mineRefId, craftingRefId, stationRefIds);

		spdlog::info("[TFD][Captive][R158] work resource aliases synced reason={} unchanged={} mineAlias={} mineRef={:08X} craftingAlias={} craftingRef={:08X} stationAliases={} stationRefs[forge={:08X} smelter={:08X} tanning={:08X} sharpening={:08X} workbench={:08X} chopping={:08X} cooking={:08X} alchemy={:08X} enchanting={:08X}]",
			reason ? reason : "unknown",
			unchanged ? 1 : 0,
			g_registry.workMineAlias ? 1 : 0,
			mineRefId,
			g_registry.workCraftingStationAlias ? 1 : 0,
			craftingRefId,
			stationAliasCount,
			stationRefIds[1],
			stationRefIds[2],
			stationRefIds[3],
			stationRefIds[4],
			stationRefIds[5],
			stationRefIds[6],
			stationRefIds[7],
			stationRefIds[8],
			stationRefIds[9]);
	}


	void ClearCaptiveWorkResourceAliases(const char* reason, bool clearItemAlias)
	{
		ResolveQuestRegistry();
		if (!g_registry.quest) {
			return;
		}

		WriteQuestAlias(g_registry.workMineAlias, nullptr, reason ? reason : "clear_work_mine_alias");
		WriteQuestAlias(g_registry.workCraftingStationAlias, nullptr, reason ? reason : "clear_work_crafting_alias");
		std::uint32_t stationAliasCount = 0;
		for (std::size_t station = 1; station < g_registry.workCraftingStationAliases.size(); ++station) {
			if (g_registry.workCraftingStationAliases[station]) {
				++stationAliasCount;
			}
			WriteQuestAlias(g_registry.workCraftingStationAliases[station], nullptr, reason ? reason : "clear_work_station_alias");
		}
		if (clearItemAlias) {
			WriteQuestAlias(g_registry.workItemAlias, nullptr, reason ? reason : "clear_work_item_alias");
		}

		g_lastWorkResourceAliasSnapshot = {};
		g_nextReleasedWorkAliasSelfHeal = {};

		spdlog::info("[TFD][Captive] work resource aliases cleared reason={} mineAlias={} craftingAlias={} stationAliases={} itemAliasCleared={}",
			reason ? reason : "unknown",
			g_registry.workMineAlias ? 1 : 0,
			g_registry.workCraftingStationAlias ? 1 : 0,
			stationAliasCount,
			clearItemAlias ? 1 : 0);
	}


	static bool HasUnclaimedRecoverGearStash();
	static bool ConfiscatePlayerInventoryToWorkStorage(const char* reason);
	static bool PublishRecoverGearStashCycle(RE::TESObjectREFR* storage, std::int32_t storageUnitsBefore, std::int32_t storageUnitsAfter, const char* reason);

	void BeginReleasedWorkRuntime(RE::Actor* actor, const char* reason)
	{
		const char* useReason = reason ? reason : "released_work_runtime";
		g_releasedWorkEscapeBreakResourcesCleared = false;
		auto* player = Player();
		if (player) {
			EnsureCaptiveNavigationContext(player, useReason);
			SyncCaptiveNavigationAliases(useReason);
		}

		ForceCaptiveStateGlobal(GetPhaseRaw(true, PhaseValue::ReleasedWork), useReason);
		TFD::CaptiveGreet::Cancel("released_work_enter_cancel_calling_captor");

		auto* door = ResolveBoundEscapeDoor();
		if (door && DoorControllerRef().HasDoor()) {
			DoorControllerRef().UnlockForRelease(3.0, true, false);
			ResetLockpickWatch();
			spdlog::info("[TFD][Captive] released work door unlocked/opened door={:08X} reason={}",
				door->GetFormID(),
				useReason);
		}
		else {
			spdlog::warn("[TFD][Captive] released work door unlock skipped door={:08X} hasDoor={} reason={}",
				door ? door->GetFormID() : 0u,
				DoorControllerRef().HasDoor() ? 1 : 0,
				useReason);
		}

		// ReleasedWork is still inside the captive ecosystem.
		// Keep TFDCaptiveFaction on every eligible actor in the CaptiveMarker
		// location; TFDWorkingCaptiveFaction is only an additional marker for the
		// current Work Boss.  Calling Captor must be blocked by CK/global state
		// conditions, not by stripping the captive-location faction.
		if (player) {
			ApplyNativeCaptiveLocationRoleFactions(player, "released_work_enter_keep_captive_location_roles", true);
		}

		// Refresh the captive-location special-ref snapshot so BossCaptor aliases and
		// the working-dialogue faction point at the location leader, not necessarily
		// the ordinary captor who opened the Work outcome.
		(void)TFD::Location::ResolveNearestCaptiveStorageTarget(actor);
		SyncStorageDebugAliases(useReason);

		// C47: ReleasedWork itself is not automatically a new captivity cycle.
		// If the original Get-your-gear stash is still unclaimed, preserve it and
		// do not duplicate the objective.  If the player already opened/recovered
		// that stash, then a later failed escape -> Work is a new confiscation
		// moment: the player may have all gear back in inventory, so move it back
		// to the BossContainer and publish a fresh Get-your-gear cycle.
		if (HasUnclaimedRecoverGearStash()) {
			spdlog::info("[TFD][Captive][C47] released work gear stash preserved active unclaimed target={:08X} baseUnits={} cycle={} reason={}",
				g_recoverGearStashTargetFormID,
				g_recoverGearStashBaseUnits,
				g_stashCycleID,
				useReason);
		}
		else {
			const bool stashed = ConfiscatePlayerInventoryToWorkStorage("released_work_enter_reconfiscate_after_recovered_gear");
			spdlog::info("[TFD][Captive][C47] released work gear restash check stashed={} opened={} active={} target={:08X} baseUnits={} cycle={} reason={}",
				stashed ? 1 : 0,
				g_recoverGearStashOpened ? 1 : 0,
				g_recoverGearStashActive ? 1 : 0,
				g_recoverGearStashTargetFormID,
				g_recoverGearStashBaseUnits,
				g_stashCycleID,
				useReason);
		}

		// R187: Work Boss dialogue must be ready before the player activates the Boss.
		// Refresh resource globals/aliases first, then publish exact-offer demand
		// factions for the final Boss.  The GREET/TIF is now light-only and no longer
		// repairs late setup after dialogue open.
		(void)TFD::Location::RefreshCaptiveWorkResourceState(true, useReason);
		SyncCaptiveWorkResourceAliases(useReason);
		ApplyWorkingFactionToBossActors(actor, useReason);
	}

	void HandleReleasedWorkNoJob(RE::Actor* actor, double cooldownSeconds, const char* reason)
	{
		if (!g_state || g_phase != PhaseValue::ReleasedWork) {
			spdlog::info("[TFD][Captive] work no-job ignored phase={} active={} actor={:08X} reason={}",
				GetPhaseName(),
				g_state ? 1 : 0,
				actor ? actor->GetFormID() : 0u,
				reason ? reason : "unknown");
			return;
		}

		(void)TFD::Location::ResolveNearestCaptiveStorageTarget(actor);
		const auto excluded = actor ? actor->GetFormID() : g_currentWorkBossFormID;

		// R189: No Job redirect must be exact-offer-aware.  Do not blindly cycle to
		// the next LocRefType Boss, because that can ping-pong between two empty
		// bosses while a third Boss still has Crafting/Cooking/Pleasure/Improve.
		// WorkNative owns the prepared offer list; if it cannot find a Boss with a
		// valid offer, this is final No Job for the current Work pass.
		auto* nextBoss = TFD::WorkNative::SelectNextBossWithExactWorkOffer(actor, excluded, reason ? reason : "work_no_job_exact_redirect");
		if (nextBoss) {
			g_workBossResumeAt = {};
			ApplyWorkingFactionToCurrentBoss(nextBoss, reason ? reason : "work_no_job_next_exact_boss");
			ForceWorkGlobals(0, 1, "work_no_job_redirect_next_exact_boss");
			spdlog::info("[TFD][Captive][R189] work no-job redirected exact-offer from={:08X} to={:08X} assignment=TalkBoss reason={}",
				excluded,
				nextBoss->GetFormID(),
				reason ? reason : "unknown");
			return;
		}

		RE::Actor* sameBoss = actor ? actor : LookupActorByFormID(g_currentWorkBossFormID);
		if (!sameBoss) {
			sameBoss = SelectWorkBossActor(nullptr, 0, true);
		}
		WriteCurrentWorkBossAlias(sameBoss, reason ? reason : "work_no_job_same_boss");
		ClearNativeWorkingCaptiveFaction(reason ? reason : "work_no_job_cooldown");
		const auto cooldown = cooldownSeconds > 0.0 ? cooldownSeconds : 10.0;
		g_workBossResumeAt = Now() + std::chrono::milliseconds(static_cast<int>(cooldown * 1000.0));
		ForceWorkGlobals(0, 7, "work_no_job_cooldown_same_boss");
		spdlog::info("[TFD][Captive][C38] work no-job cooldown boss={:08X} seconds={:.2f} assignment=Cooldown reason={}",
			sameBoss ? sameBoss->GetFormID() : 0u,
			cooldown,
			reason ? reason : "unknown");
	}

	void ClearReleasedWorkRuntime(const char* reason)
	{
		ClearNativeWorkingCaptiveFaction(reason ? reason : "clear_released_work_runtime");
		TFD::WorkNative::ClearWorkDemandFactions(reason ? reason : "clear_released_work_runtime");
		WriteCurrentWorkBossAlias(nullptr, reason ? reason : "clear_released_work_runtime");
		ClearCaptiveWorkResourceAliases(reason ? reason : "clear_released_work_runtime", true);
		g_workBossResumeAt = {};
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
			}
			else {
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

	static bool HasUnclaimedRecoverGearStash()
	{
		if (!g_recoverGearStashActive || g_recoverGearStashTargetFormID == 0) {
			return false;
		}

		auto* target = RE::TESForm::LookupByID<RE::TESObjectREFR>(g_recoverGearStashTargetFormID);
		if (!target) {
			const bool preserve = !g_recoverGearStashOpened;
			spdlog::warn("[TFD][Captive][C47] recover gear stash claim check target missing target={:08X} opened={} preserve={} cycle={}",
				g_recoverGearStashTargetFormID,
				g_recoverGearStashOpened ? 1 : 0,
				preserve ? 1 : 0,
				g_stashCycleID);
			return preserve;
		}

		const auto currentUnits = GetReferenceTotalInventoryCount(target);
		const bool hasUnclaimedUnits = currentUnits > g_recoverGearStashBaseUnits;
		spdlog::info("[TFD][Captive][C47] recover gear stash claim check target={:08X} currentUnits={} baseUnits={} opened={} unclaimed={} cycle={}",
			g_recoverGearStashTargetFormID,
			currentUnits,
			g_recoverGearStashBaseUnits,
			g_recoverGearStashOpened ? 1 : 0,
			hasUnclaimedUnits ? 1 : 0,
			g_stashCycleID);
		return hasUnclaimedUnits;
	}

	static bool ConfiscatePlayerInventoryToWorkStorage(const char* reason)
	{
		const char* useReason = reason ? reason : "released_work_enter_reconfiscate_after_recovered_gear";
		auto* storage = TFD::Location::ResolveNearestCaptiveStorageTarget(nullptr);
		SyncStorageDebugAliases(useReason);
		if (!storage) {
			spdlog::warn("[TFD][Captive][C47] released work gear restash skipped no_container reason={}", useReason);
			return false;
		}
		if (!IsContainerStorageTarget(storage)) {
			spdlog::warn("[TFD][Captive][C47] released work gear restash rejected non_container target={:08X} reason={}",
				storage->GetFormID(),
				useReason);
			return false;
		}

		const auto storageUnitsBefore = GetReferenceTotalInventoryCount(storage);
		const bool moved = TransferPlayerInventoryToStorage(storage, useReason);
		const auto storageUnitsAfter = GetReferenceTotalInventoryCount(storage);
		if (moved) {
			(void)PublishRecoverGearStashCycle(storage, storageUnitsBefore, storageUnitsAfter, useReason);
		}
		return moved;
	}

	static bool PublishRecoverGearStashCycle(RE::TESObjectREFR* storage, std::int32_t storageUnitsBefore, std::int32_t storageUnitsAfter, const char* reason)
	{
		if (!storage) {
			return false;
		}

		++g_stashCycleID;
		if (g_stashCycleID == 0) {
			++g_stashCycleID;
		}

		g_recoverGearStashActive = true;
		g_recoverGearStashOpened = false;
		g_recoverGearStashTargetFormID = storage->GetFormID();
		g_recoverGearStashBaseUnits = storageUnitsBefore;

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

		return true;
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
			}
			else {
				g_confiscationNextAttempt = Now() + std::chrono::milliseconds(kCaptiveConfiscationRetryDelayMs);
			}
			return;
		}

		if (!IsContainerStorageTarget(storage)) {
			if (g_confiscationAttemptCount >= kCaptiveConfiscationMaxAttempts) {
				spdlog::warn("[TFD][Captive] confiscation aborted non_container_target target={:08X} attempts={} reason={}",
					storage->GetFormID(), g_confiscationAttemptCount, reason);
				ClearPendingConfiscation("non_container_target");
			}
			else {
				g_confiscationNextAttempt = Now() + std::chrono::milliseconds(kCaptiveConfiscationRetryDelayMs);
			}
			return;
		}

		const auto storageUnitsBefore = GetReferenceTotalInventoryCount(storage);
		const bool moved = TransferPlayerInventoryToStorage(storage, reason);
		const auto storageUnitsAfter = GetReferenceTotalInventoryCount(storage);
		g_confiscationApplied = true;
		if (moved) {
			PublishRecoverGearStashCycle(storage, storageUnitsBefore, storageUnitsAfter, reason);
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
		if (stateActive && phase == PhaseValue::ReleasedWork) {
			g_releasedWorkEscapeBreakResourcesCleared = false;
		}
		if (stateActive && phase == PhaseValue::Escape) {
			// C52: EscapeStarted means active escape ownership has returned to
			// Captive.  Any stale escape-bleedout latch from a previous failed
			// escape must not keep blocking location-exit resolution.
			if (g_escapeBleedoutActive || g_escapeBreakBleedPending) {
				spdlog::info("[TFD][Captive][C52] clearing stale escape-bleedout latch on escape state escapeBleedout={} rebleedPending={}",
					g_escapeBleedoutActive ? 1 : 0,
					g_escapeBreakBleedPending ? 1 : 0);
			}
			g_escapeBleedoutActive = false;
			ClearEscapeBreakRebleed();
		}
		ForceCaptiveStateGlobal(GetPhaseRaw(stateActive, phase), "set_runtime_state");
		if (!stateActive) {
			ClearNativeCaptorRoleFaction("runtime_state_not_captive");
		}
		if (!stateActive || phase != PhaseValue::ReleasedWork) {
			ClearReleasedWorkRuntime("runtime_state_leave_work");
			TFD::Location::ClearCaptiveWorkResourceState("runtime_state_leave_work");
		}
		if (!IsCaptivePassiveHoldActive()) {
			TFD::HostilityController::ResetCaptiveSuppression();
		}
		if (!stateActive) {
			TFD::ForceGreetState::ResetCaptive();
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

	static RE::TESObjectREFR* FindNearestLockedDoorNearCaptiveMarker()
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
			if (!IsRefLocked(candidate)) return RE::BSContainer::ForEachResult::kContinue;
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


	static bool IsValidCaptorApproachPoint(RE::TESObjectREFR* ref, RE::Actor* player)
	{
		if (!ref) {
			return false;
		}
		if (player && ref == player) {
			return true;
		}
		return IsDoorRef(ref);
	}

	static RE::TESObjectREFR* ResolveCaptorApproachTargetFresh(RE::Actor* player)
	{
		ResolveQuestRegistry();

		// Main desired case: a locked captive door near CaptiveMarker wins.
		// Binding it keeps EscapeDoor and later Papyrus properties coherent.
		if (auto* markerDoor = FindNearestLockedDoorNearCaptiveMarker()) {
			if (ResolveBoundEscapeDoor() != markerDoor) {
				BindDoor(markerDoor);
			}
			return markerDoor;
		}

		// If CK/native already filled EscapeDoor, trust it as the explicit door target.
		if (g_registry.escapeDoorAlias) {
			auto* aliasDoor = g_registry.escapeDoorAlias->GetReference();
			if (aliasDoor && IsDoorRef(aliasDoor)) {
				g_boundEscapeDoor = aliasDoor->GetHandle();
				return aliasDoor;
			}
		}

		if (auto* boundDoor = ResolveBoundEscapeDoor()) {
			if (IsDoorRef(boundDoor)) {
				return boundDoor;
			}
		}

		return player ? static_cast<RE::TESObjectREFR*>(player) : static_cast<RE::TESObjectREFR*>(Player());
	}

	RE::TESObjectREFR* ResolveCaptorApproachTarget(RE::Actor* player)
	{
		ResolveQuestRegistry();

		if (g_registry.approachPointAlias) {
			auto* aliasTarget = g_registry.approachPointAlias->GetReference();
			if (IsValidCaptorApproachPoint(aliasTarget, player)) {
				return aliasTarget;
			}
		}

		return ResolveCaptorApproachTargetFresh(player);
	}

	const char* GetCaptorApproachTargetName(RE::TESObjectREFR* target, RE::Actor* player)
	{
		if (!target) {
			return "None";
		}
		if (player && target == player) {
			return "PlayerRef";
		}
		if (IsDoorRef(target)) {
			return "EscapeDoor";
		}
		return "Reference";
	}

	void SyncCaptorApproachPointAlias(RE::Actor* player, const char* reason)
	{
		ResolveQuestRegistry();
		if (!g_registry.quest || !g_registry.approachPointAlias) {
			spdlog::warn("[TFD][Captive] ApproachPoint alias unavailable; TFDCaptiveApproach may have no target reason={}", reason ? reason : "unknown");
			return;
		}

		auto* target = ResolveCaptorApproachTargetFresh(player);
		WriteQuestAlias(g_registry.approachPointAlias, target, reason ? reason : "sync_captor_approach_point");
		spdlog::info("[TFD][Captive] ApproachPoint synced targetMode={} target={:08X} reason={}",
			GetCaptorApproachTargetName(target, player),
			target ? target->GetFormID() : 0u,
			reason ? reason : "unknown");
	}

	static void SyncCaptorApproachPointAliasToPlayer(RE::Actor* player, const char* reason)
	{
		ResolveQuestRegistry();
		if (!g_registry.quest || !g_registry.approachPointAlias) {
			spdlog::warn("[TFD][Captive] ApproachPoint alias unavailable; TFDCaptiveApproach may have no player target reason={}", reason ? reason : "unknown");
			return;
		}

		auto* target = player ? static_cast<RE::TESObjectREFR*>(player) : static_cast<RE::TESObjectREFR*>(Player());
		WriteQuestAlias(g_registry.approachPointAlias, target, reason ? reason : "sync_captor_approach_point_player");
		spdlog::info("[TFD][Captive] ApproachPoint synced targetMode=PlayerRef target={:08X} reason={} policy=calling_captor_walk_to_player_no_teleport",
			target ? target->GetFormID() : 0u,
			reason ? reason : "unknown");
	}

	void ClearCaptorApproachPointAlias(const char* reason)
	{
		ResolveQuestRegistry();
		if (!g_registry.quest || !g_registry.approachPointAlias) {
			return;
		}
		WriteQuestAlias(g_registry.approachPointAlias, nullptr, reason ? reason : "clear_captor_approach_point");
		spdlog::info("[TFD][Captive] ApproachPoint cleared reason={}", reason ? reason : "unknown");
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
		WriteQuestAlias(g_registry.escapeRouteAlias, door);

		spdlog::info("[TFD][Captive] navigation aliases synced reason={} marker={:08X} escapeDoor={:08X} markerAlias={} doorAlias={} routeAlias={}",
			reason ? reason : "unknown",
			marker ? marker->GetFormID() : 0u,
			door ? door->GetFormID() : 0u,
			g_registry.captiveMarkerAlias ? 1 : 0,
			g_registry.escapeDoorAlias ? 1 : 0,
			g_registry.escapeRouteAlias ? 1 : 0);
	}


	void ClearCaptiveNavigationAliases(const char* reason)
	{
		ResolveQuestRegistry();
		if (!g_registry.quest) {
			return;
		}

		WriteQuestAlias(g_registry.captiveMarkerAlias, nullptr);
		WriteQuestAlias(g_registry.escapeDoorAlias, nullptr);
		WriteQuestAlias(g_registry.approachPointAlias, nullptr);
		WriteQuestAlias(g_registry.escapeRouteAlias, nullptr);
		spdlog::info("[TFD][Captive] navigation aliases cleared reason={} markerAlias={} doorAlias={} approachPointAlias={} routeAlias={}",
			reason ? reason : "unknown",
			g_registry.captiveMarkerAlias ? 1 : 0,
			g_registry.escapeDoorAlias ? 1 : 0,
			g_registry.approachPointAlias ? 1 : 0,
			g_registry.escapeRouteAlias ? 1 : 0);
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

		if (captiveLoc == 0u) {
			return false;
		}

		if (playerLoc != 0u) {
			return playerLoc != captiveLoc;
		}

		// C52: Outside wilderness cells can report no owning Location even after
		// the player has clearly left the CaptiveMarker cell.  Treat that as a
		// successful location escape when the player cell differs from the anchor
		// cell; otherwise EscapeAttempt can stay stuck forever with playerLoc=0.
		const RE::FormID captiveCell = ResolveCaptiveAnchorCellID();
		auto* playerCell = player->GetParentCell();
		const RE::FormID currentCell = playerCell ? playerCell->GetFormID() : 0u;
		return captiveCell != 0u && currentCell != 0u && currentCell != captiveCell;
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
		ClearReleasedWorkRuntime("clear_escape_context");
		TFD::Location::ClearCaptiveWorkResourceState("clear_escape_context");
		g_marker.reset();
		g_cellFormID = 0;
		g_locationFormID = 0;
		g_escapeRadiusActive = false;
		g_escapeRadiusSince = {};
		g_boundEscapeDoor.reset();
		g_door.Reset();
		g_returnToCaptiveGuardDepth = 0;
		g_returnToCaptiveGuardUntil = {};
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

	void BeginReturnToCaptiveTransitionGuard(const char* reason, double seconds)
	{
		if (seconds < 0.50) {
			seconds = 0.50;
		}
		if (seconds > 10.00) {
			seconds = 10.00;
		}

		const auto ms = static_cast<int>(seconds * 1000.0);
		const auto until = Now() + std::chrono::milliseconds(ms);
		if (g_returnToCaptiveGuardUntil < until) {
			g_returnToCaptiveGuardUntil = until;
		}
		++g_returnToCaptiveGuardDepth;
		spdlog::info(
			"[TFD][Captive] return-to-captive guard begin reason={} depth={} ms={}",
			reason ? reason : "unknown",
			g_returnToCaptiveGuardDepth,
			ms);
	}

	void EndReturnToCaptiveTransitionGuard(const char* reason)
	{
		if (g_returnToCaptiveGuardDepth > 0) {
			--g_returnToCaptiveGuardDepth;
		}

		// Keep a tiny settle window so a stale open/unlock edge from the captive door
		// cannot win the same frame after CompleteCaptiveTransitionNow teleports/locks.
		const auto settleUntil = Now() + std::chrono::milliseconds(350);
		if (g_returnToCaptiveGuardUntil < settleUntil) {
			g_returnToCaptiveGuardUntil = settleUntil;
		}

		spdlog::info(
			"[TFD][Captive] return-to-captive guard end reason={} depth={} settleMs=350",
			reason ? reason : "unknown",
			g_returnToCaptiveGuardDepth);
	}

	bool IsReturnToCaptiveTransitionGuardActive()
	{
		return Now() < g_returnToCaptiveGuardUntil;
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
			}
			else {
				g_lockpickDoorCandidate.reset();
				g_lockpickDoorWasLocked = false;
				auto* boundDoor = ResolveBoundEscapeDoor();
				if (rawTarget) {
					spdlog::info("[TFD][Captive] lockpick target {:08X} ignored (door={} nearMarker={} fallbackBoundDoor={:08X})", rawTarget->GetFormID(), IsDoorRef(rawTarget) ? 1 : 0, IsDoorNearMarker(rawTarget) ? 1 : 0, boundDoor ? boundDoor->GetFormID() : 0);
				}
				else {
					spdlog::info("[TFD][Captive] lockpick target null (fallbackBoundDoor={:08X})", boundDoor ? boundDoor->GetFormID() : 0);
				}
			}
		}

		const bool returnGuardActive = IsReturnToCaptiveTransitionGuardActive();
		if (g_door.HasDoor()) {
			const bool doorTriggered = g_door.UpdateWatcher();
			if (doorTriggered) {
				if (returnGuardActive) {
					spdlog::info("[TFD][Captive] EscapeCommit suppressed reason=return_to_captive_guard source=door_watch");
					g_prevLockpickOpen = lockOpen;
					return false;
				}
				if (onEscapeCommit) {
					onEscapeCommit("door_watch", nullptr);
				}
				g_prevLockpickOpen = lockOpen;
				return true;
			}
		}

		if (!lockOpen && g_prevLockpickOpen) {
			RE::TESObjectREFR* door = nullptr;
			if (g_lockpickDoorCandidate) {
				auto ptr = g_lockpickDoorCandidate.get();
				door = ptr.get();
			}
			if (!door) door = ResolveBoundEscapeDoor();
			if (door && IsDoorRef(door) && g_lockpickDoorWasLocked && !IsRefLocked(door)) {
				if (returnGuardActive) {
					spdlog::info("[TFD][Captive] EscapeCommit suppressed reason=return_to_captive_guard source=lockpick door={:08X}", door->GetFormID());
					ResetLockpickWatch();
					g_prevLockpickOpen = lockOpen;
					return false;
				}
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
		if (IsReturnToCaptiveTransitionGuardActive()) {
			g_escapeRadiusActive = false;
			return false;
		}
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
		}
		else {
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
		const auto rehostileCount = TFD::HostilityController::BreakCaptivePassiveForCombat(
			player,
			aggressor,
			TFD::HostilityController::ReleaseReason::PlayerAggression,
			reason ? reason : "escape_commit",
			true,
			false);

		if (aggressor) {
			spdlog::info("[TFD][Captive] Escape aggro witness actor={:08X} rehostileCount={}", aggressor->GetFormID(), static_cast<unsigned int>(rehostileCount));
		}
		else {
			spdlog::info("[TFD][Captive] Escape aggro witness skipped (no aggressor) rehostileCount={}", static_cast<unsigned int>(rehostileCount));
		}

		spdlog::info("[TFD][Captive] EscapeCommit reason={} door={:08X} rehostileCount={}", reason ? reason : "unknown", door ? door->GetFormID() : 0, static_cast<unsigned int>(rehostileCount));
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

	static bool IsCaptivePleasureSceneHandoffActive()
	{
		if (!TFD::PleasureRuntime::IsActive()) {
			return false;
		}

		if (TFD::PleasureRuntime::GetSourceContext() != TFD::PleasureRuntime::SourceContext::Captive) {
			return false;
		}

		const auto phase = TFD::PleasureRuntime::GetPhase();
		return phase == TFD::PleasureRuntime::Phase::PleasureStartPending ||
			phase == TFD::PleasureRuntime::Phase::PleasureActive ||
			phase == TFD::PleasureRuntime::Phase::PleasureEnding ||
			phase == TFD::PleasureRuntime::Phase::AfterPleasureAwaitQuest ||
			phase == TFD::PleasureRuntime::Phase::AfterPleasureDialogue;
	}

	static bool TryTriggerReleasedWorkWeaponDrawnEscape(RE::Actor* player, const RuntimeTickHandlers& handlers)
	{
		if (!IsReleasedWorkActive() || !player || !player->IsWeaponDrawn()) {
			g_releasedWorkWeaponDrawnNextLog = {};
			return false;
		}
		if (g_escapeBleedoutActive || g_recaptureCommitActive) {
			return false;
		}
		if (IsCaptivePleasureSceneHandoffActive()) {
			const auto now = Now();
			if (g_releasedWorkWeaponDrawnNextLog == std::chrono::steady_clock::time_point{} || now >= g_releasedWorkWeaponDrawnNextLog) {
				g_releasedWorkWeaponDrawnNextLog = now + std::chrono::milliseconds(1500);
				spdlog::info(
					"[TFD][Captive][R234B] released work weapon draw ignored reason=captive_pleasure_handoff phase={} source={}",
					TFD::PleasureRuntime::GetPhaseName(),
					TFD::PleasureRuntime::GetSourceContextName());
			}
			return false;
		}

		auto* witnessHint = GetCurrentWorkBoss();
		const bool seen = TFD::HostilityController::HasVisibleCaptiveCombatWitness(
			player,
			witnessHint,
			"released_work_player_weapon_drawn_check");

		if (!seen) {
			const auto now = Now();
			if (g_releasedWorkWeaponDrawnNextLog == std::chrono::steady_clock::time_point{} || now >= g_releasedWorkWeaponDrawnNextLog) {
				g_releasedWorkWeaponDrawnNextLog = now + std::chrono::milliseconds(1500);
				spdlog::info(
					"[TFD][Captive] released work weapon draw ignored reason=no_visible_witness boss={:08X}",
					witnessHint ? witnessHint->GetFormID() : 0u);
			}
			return false;
		}

		const char* reason = "released_work_player_weapon_drawn_seen";
		TFD::PleasureRuntime::Break(reason, true, true, true);
		ClearReleasedWorkRuntime(reason);
		TFD::Location::ClearCaptiveWorkResourceState(reason);
		ResetLockpickWatch();
		if (handlers.setPrevDialogueOpen) {
			handlers.setPrevDialogueOpen(false);
		}
		if (handlers.escape.setGraceActive) {
			handlers.escape.setGraceActive(false);
		}
		SetRuntimeState(true, PhaseValue::Escape);
		(void)TFD::FlowController::Controller::GetSingleton().ResolveCaptiveOutcome(
			TFD::FlowController::CaptiveOutcome::EscapeStarted,
			witnessHint ? witnessHint->GetFormID() : 0u,
			reason);

		const auto rehostileCount = TFD::HostilityController::BreakCaptivePassiveForCombat(
			player,
			nullptr,
			TFD::HostilityController::ReleaseReason::PlayerArmed,
			reason,
			true,
			false);

		if (handlers.escape.updatePreCombatState) {
			handlers.escape.updatePreCombatState();
		}

		spdlog::info(
			"[TFD][Captive] released work weapon draw -> escape boss={:08X} rehostileCount={}",
			witnessHint ? witnessHint->GetFormID() : 0u,
			static_cast<unsigned int>(rehostileCount));
		return true;
	}

	bool TickRuntime(RE::Actor* player, bool captiveBleedOverlay, const RuntimeTickHandlers& handlers)
	{
		ProcessPendingConfiscation();
		(void)NormalizeInvalidCaptivePair();

		const bool escapeBreakOverlay = TFD::FlowController::Controller::GetSingleton().IsCaptiveCombatEscapeBreakContextActive();

		if (!escapeBreakOverlay && TryTriggerReleasedWorkWeaponDrawnEscape(player, handlers)) {
			return false;
		}

		if (IsCaptivePassiveHoldActive()) {
			if (IsStandardCaptiveActive()) {
				EnsureCaptiveNavigationContext(player, "standard_captive_tick");
			}
			if (!escapeBreakOverlay) {
				TFD::HostilityController::TickCaptiveSuppression();
				ProcessReleasedWorkBossResume("released_work_tick");
				if (g_phase == PhaseValue::ReleasedWork) {
					(void)TFD::Location::RefreshCaptiveWorkResourceState(false, "released_work_tick");
					SyncCaptiveWorkResourceAliases("released_work_tick");
				}
			}
			else {
				// R303A: once ReleasedWork has handed off to combat / bleedout /
				// pleasure-failed ownership, Work is no longer allowed to maintain
				// resource aliases. Re-scanning here re-latches TFDCraftingState and
				// keeps the Papyrus work_update loop alive after native has already
				// left Work. Clear once and let the new owner rebuild Work explicitly
				// if the player later selects a Work outcome.
				TFD::HostilityController::ResetCaptiveSuppression();
				if (g_phase == PhaseValue::ReleasedWork && !g_releasedWorkEscapeBreakResourcesCleared) {
					g_releasedWorkEscapeBreakResourcesCleared = true;
					ClearReleasedWorkRuntime("released_work_escape_break_handoff");
					TFD::Location::ClearCaptiveWorkResourceState("released_work_escape_break_handoff");
					spdlog::info("[TFD][Captive][R303A] released work resource updater stopped for escape-break overlay phase={} reason=released_work_escape_break_handoff",
						GetPhaseName());
				}
			}
		}
		else {
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
			// TFDCaptiveFaction is the captive-location identity tag and must stay
			// present until the player actually leaves/escapes the CaptiveMarker
			// location.  ResolveCaptiveExitToFree runs before this block, so a real
			// location escape clears the faction before any new sweep can happen.
			if (g_state && (g_phase == PhaseValue::Captive ||
				g_phase == PhaseValue::ReleasedWork ||
				g_phase == PhaseValue::Scene ||
				g_phase == PhaseValue::Escape)) {
				ApplyNativeCaptiveLocationRoleFactions(player, "captive_runtime_tick", false);
			}
			TFD::InteractionRouter::DialogueOpen::Tick();
			TFD::PleasureRuntime::Tick();
		}

		if (IsStandardCaptiveActive()) {
			const bool captivePleasureSceneHandoff = IsCaptivePleasureSceneHandoffActive();
			const bool dialogOpen = handlers.isDialogueOpen ? handlers.isDialogueOpen() : false;
			const bool prevDialogueOpen = handlers.getPrevDialogueOpen ? handlers.getPrevDialogueOpen() : false;
			if (dialogOpen) {
				TFD::CaptiveGreet::NotifyDialogueOpened();
			}
			else if (prevDialogueOpen && TFD::CaptiveGreet::IsActive()) {
				TFD::CaptiveGreet::Cancel("dialogue_closed");
			}
			if (!dialogOpen && prevDialogueOpen) {
				spdlog::info("[TFD][Captive] Dialogue closed -> no implicit action");
			}
			if (handlers.setPrevDialogueOpen) {
				handlers.setPrevDialogueOpen(dialogOpen);
			}
			if (!captiveBleedOverlay && !captivePleasureSceneHandoff) {
				(void)TickCaptiveEscapePhase(player, handlers.escape);
			}
			else if (captivePleasureSceneHandoff) {
				spdlog::info(
					"[TFD][Captive][R234B] standard captive escape tick suppressed reason=captive_pleasure_handoff phase={} source={}",
					TFD::PleasureRuntime::GetPhaseName(),
					TFD::PleasureRuntime::GetSourceContextName());
			}
		}
		else if (IsEscapeActive()) {
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
			if (escapeBreakOverlay) {
				// R227A: InCombatEscapeBreak keeps Captive as context, but combat/defeat
				// must own behavior.  Do not consume the DefeatMonitor tick here,
				// otherwise player HP threshold is never scanned and the player can die
				// during escape without a Bleedout forcegreet.
				static auto s_nextEscapeBreakThresholdPassLog = std::chrono::steady_clock::time_point{};
				const auto now = Now();
				if (s_nextEscapeBreakThresholdPassLog == std::chrono::steady_clock::time_point{} || now >= s_nextEscapeBreakThresholdPassLog) {
					s_nextEscapeBreakThresholdPassLog = now + std::chrono::milliseconds(1250);
					spdlog::info("[TFD][Captive][R227A] captive escape-break allowed defeat threshold tick phase={} bleedOverlay={} rebleedPending={} player={:08X}",
						static_cast<int>(g_phase),
						captiveBleedOverlay ? 1 : 0,
						HasEscapeBreakRebleedPending() ? 1 : 0,
						player ? player->GetFormID() : 0u);
				}
				return true;
			}
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
		(void)TFD::FlowController::Controller::GetSingleton().ResolveCaptiveOutcome(TFD::FlowController::CaptiveOutcome::EscapeStarted, actor ? actor->GetFormID() : 0u, reason ? reason : "player_aggression_captive");

		auto* player = Player();
		const auto rehostileCount = TFD::HostilityController::BreakCaptivePassiveForCombat(
			player,
			actor,
			TFD::HostilityController::ReleaseReason::PlayerAggression,
			reason ? reason : "player_aggression_captive",
			true,
			true);
		spdlog::info("[TFD][Captive] aggression escape committed actor={:08X} rehostileCount={} reason={}",
			actor ? actor->GetFormID() : 0u,
			static_cast<unsigned int>(rehostileCount),
			reason ? reason : "player_aggression_captive");
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

		(void)TFD::FlowController::QueueBridgeModEvent(
			"TFDSystemEventForceClearRoute",
			preferredCaptor,
			"recapture_commit_alias_clear",
			1.0f);
		(void)TFD::FlowController::QueueBridgeModEvent(
			"TFDBleedoutUnassign",
			preferredCaptor,
			"recapture_commit_alias_clear",
			0.0f);
		(void)TFD::FlowController::QueueBridgeModEvent(
			"TFDTruceHardClearAll",
			preferredCaptor,
			"recapture_commit_alias_clear",
			1.0f);

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
		}
		else if (before.root == TFD::FlowController::RootFlow::Captive) {
			(void)flow.ResolveCaptiveOutcome(TFD::FlowController::CaptiveOutcome::Recaptured, actorFormID, why);
		}

		auto runtimeHandlers = TFD::Transition::DefeatGlue::BuildTransitionRuntimeHandlers();
		auto captiveHandlers = TFD::Transition::DefeatGlue::BuildTransitionCaptiveHandlers();
		bool completed = false;
		if (TFD::Transition::ResolveCaptiveMarkerForOutcome(runtimeHandlers)) {
			completed = TFD::Transition::CompleteCaptiveTransitionNow(why, runtimeHandlers, captiveHandlers);
		}
		else {
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
				"TFDSystemEventForceClearRoute",
				preferredCaptor,
				why,
				1.0f);
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
			spdlog::info("[TFD][Captive][R202B] CommitRecapture complete actor={:08X} reason={} clearSystemRoute=1 clearBleedAliases=1 clearCrowdAliases=1 hardCrowdClear=1 recoverPlayer=1", actorFormID, why);
		}
		else {
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
		ApplyNativeCaptiveLocationRoleFactions(player, "call_captor_hotkey_forced_sweep", true);

		auto* captor = PickCaptorSameCellLoaded(player, 12288.0f);
		if (!captor) {
			return false;
		}

		TFD::ForceGreetState::ResetCaptive();
		EnsureCaptorApproachReadySinkRegistered();
		ArmPendingCaptorCallHandshake(captor, "call_captor_hotkey");

		// Calling Captor movement contract:
		// Hotkey only selects/arms the captor. The actor must walk to the player via
		// the CK alias package; native starts a pending CaptiveMarker open and waits
		// for player distance <= 360. No MoveTo/teleport assist is allowed here.
		SyncCaptorApproachPointAliasToPlayer(player, "call_captor_native_seed_player_approach_point");
		WriteQuestAlias(g_registry.bossCaptorAliases[0], captor, "call_captor_native_owner_seed");
		captor->AllowPCDialogue(true);
		LogCallingCaptorOwnershipSnapshot(captor, "call_captor_after_native_seed_before_papyrus_assign");
		ApplyCallCaptorCalmBubble(player, captor, 12288.0f);
		LogCallingCaptorOwnershipSnapshot(captor, "call_captor_after_bridge_assign_with_native_seed");

		const bool directBegan = BeginCaptorCallApproachGreet(captor, "call_captor_hotkey_walk_to_player");
		if (directBegan) {
			ClearPendingCaptorCallHandshake(captor->GetFormID(), "direct_approach_window_greet_started");
		}
		else {
			spdlog::warn("[TFD][Captive][Handshake] Call Captor direct approach-window begin failed actor={:08X}; keeping Papyrus ready fallback",
				captor->GetFormID());
		}

		g_captorCallCooldownUntil = Now() + kCaptorCallCooldown;
		spdlog::info("[TFD][Captive][Handshake] Call Captor pending actor={:08X} cooldownMs={} reason=call_captor_hotkey policy=walk_to_player_then_open_under_360_no_moveto directBegan={}",
			captor->GetFormID(),
			static_cast<int>(kCaptorCallCooldown.count()),
			directBegan ? 1 : 0);

		if (outCaptor) {
			*outCaptor = captor;
		}
		return true;
	}

	void ApplyQueuedDefeatProgressState(const ApplyQueuedDefeatProgressHandlers& handlers)
	{
		const bool queuedState = GetQueuedStateFlag();
		const PhaseValue queuedPhase = GetQueuedPhase();

		SetRuntimeState(queuedState, queuedPhase);

		// Keep FlowController in sync with the restored captive globals.
		// Before this, TFDCaptiveState could restore to Captive, while the
		// SMF Flow Indicator stayed at Root=None / Neutral after loading a save.
		// Calling Captor then worked, but the flow snapshot stayed stale until a
		// fresh outcome was selected.
		auto& flow = TFD::FlowController::Controller::GetSingleton();
		if (queuedState) {
			(void)flow.RequestCaptive(0, TFD::FlowController::CaptiveMode::Kidnapped, "apply_queued_captive_load");

			switch (queuedPhase) {
			case PhaseValue::ReleasedWork:
			{
				RE::Actor* restoredBoss = LookupActorByFormID(g_queuedWorkBossFormID);
				const std::uint32_t primaryID = restoredBoss ? restoredBoss->GetFormID() : g_queuedWorkBossFormID;
				(void)flow.RequestResolveCaptiveOutcome(TFD::FlowController::CaptiveOutcome::WorkForEnemy, primaryID, "apply_queued_released_work_load");

				// Rehydrate ReleasedWork as a session, not just as a phase.
				// This restores Boss alias, TFDWorkingCaptiveFaction, door/work resources,
				// and the native work boss cache before Papyrus refreshes objectives.
				BeginReleasedWorkRuntime(restoredBoss, "apply_queued_released_work_load_runtime");

				if (g_queuedWorkAssignmentState != 0 || g_queuedWorkJobType != 0) {
					ForceWorkGlobals(g_queuedWorkJobType, g_queuedWorkAssignmentState, "apply_queued_released_work_load");
				}
				else {
					// Legacy v2 saves only stored phase=ReleasedWork. Fall back to idle Work.
					ForceWorkGlobals(0, 1, "apply_queued_released_work_load_legacy_idle");
				}

				// Quest objective markers are CK/Papyrus-owned. Native can restore the
				// runtime boss/working faction immediately, but the Quest Target for
				// objective 20 must be refreshed after the Boss alias is valid. Reuse the
				// already-registered captive work mod event with a load-restore reason so
				// TFDSystemEventQuestScript does not start a new Work outcome.
				if (restoredBoss) {
					const bool restoreQueued = TFD::FlowController::QueueBridgeModEvent(
						"TFDCaptiveRequestWork",
						restoredBoss,
						"restore_work_load",
						static_cast<float>(g_queuedWorkAssignmentState));
					spdlog::info("[TFD][Captive] queued Papyrus WorkSession objective restore boss={:08X} job={} assignment={} queued={}",
						restoredBoss->GetFormID(),
						g_queuedWorkJobType,
						g_queuedWorkAssignmentState,
						restoreQueued ? 1 : 0);
				}
				else {
					spdlog::warn("[TFD][Captive] skipped Papyrus WorkSession objective restore: no restored boss bossForm={:08X} job={} assignment={}",
						g_queuedWorkBossFormID,
						g_queuedWorkJobType,
						g_queuedWorkAssignmentState);
				}
				break;
			}
			case PhaseValue::Escape:
				(void)flow.RequestResolveCaptiveOutcome(TFD::FlowController::CaptiveOutcome::EscapeStarted, 0, "apply_queued_escape_load");
				break;
			case PhaseValue::Scene:
				(void)flow.RequestResolveCaptiveOutcome(TFD::FlowController::CaptiveOutcome::Pleasure, 0, "apply_queued_scene_load");
				break;
			case PhaseValue::Captive:
			default:
				break;
			}
		}
		else {
			flow.ResetRuntime("apply_queued_neutral_load");
		}

		if (handlers.setPrevDialogueOpen && handlers.isDialogueOpen) {
			handlers.setPrevDialogueOpen(handlers.isDialogueOpen());
		}
		CaptureCurrentLockpickMenuState();
		if (queuedState && (queuedPhase == PhaseValue::Captive || queuedPhase == PhaseValue::ReleasedWork)) {
			TFD::Location::RescanCaptiveMarker();
			if (handlers.getPlayer) {
				ArmEscapeContextFromCurrentState(handlers.getPlayer());
			}
		}
		else {
			ResetLockpickWatch();
			ClearEscapeContext();
		}
	}

	void ResetForLoad()
	{
		TFD::ForceGreetState::ResetCaptive();
		ClearNativeCaptorRoleFaction("reset_for_load");
		ClearReleasedWorkRuntime("reset_for_load");
		TFD::Location::ClearCaptiveWorkResourceState("reset_for_load");
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
		// Do not clear g_queuedState/g_queuedPhase here.
		// On save load, SKSE serialization queues the saved captive phase first,
		// then the post-load transient reset runs before ApplyQueuedProgressState().
		// Clearing the queued values here makes a saved Captive/ReleasedWork game
		// wake up as Neutral, which breaks Calling Captor with "No Valid target".
		spdlog::info("[TFD][Captive] ResetForLoad preserving queued captive state state={} phase={} workBoss={:08X} workJob={} workAssign={}",
			g_queuedState ? 1 : 0,
			static_cast<int>(g_queuedPhase),
			g_queuedWorkBossFormID,
			g_queuedWorkJobType,
			g_queuedWorkAssignmentState);
		g_escapeBreakBleedPending = false;
		g_escapeBreakPreferredAggressor.reset();
		g_prevLockpickOpen = false;
		g_escapeRadiusActive = false;
		g_escapeRadiusSince = {};
		g_boundEscapeDoor.reset();
		g_lockpickDoorCandidate.reset();
		g_lockpickDoorWasLocked = false;
		g_returnToCaptiveGuardDepth = 0;
		g_returnToCaptiveGuardUntil = {};
		g_escapeBleedoutActive = false;
		g_recaptureCommitActive = false;
		g_recaptureCommitStarted = {};
		g_lastRecaptureCompleted = {};
		g_lastRecaptureActorID = 0;
		g_stashCycleID = 0;
		g_recoverGearStashActive = false;
		g_recoverGearStashOpened = false;
		g_recoverGearStashTargetFormID = 0;
		g_recoverGearStashBaseUnits = 0;
	}
}
