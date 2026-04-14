#include "TFDDefeatMonitor.h"
#include "TFDActor.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <string>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <tuple>
#include <array>
#include <random>

#include <RE/Skyrim.h>
#include <type_traits>
#include <RE/A/ActorValues.h>
#include <RE/L/LockpickingMenu.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include "TFDSettings.h"
#include "TFDLocation.h"
#include "TFDCaptive.h"
#include "TFDCaptiveGreet.h"
#include "TFDRescueGreet.h"
#include "TFDRescue.h"
#include "TFDVictory.h"
#include "TFDCaptiveDoorController.h"
#include "TFDHostilityController.h"

#include "TFDPreCombatGreet.h"
#include "TFDInCombat.h"
#include "TFDInCombatGreet.h"
#include "TFDTame.h"
#include "TFDFlowController.h"
#include "TFDInteractionRouter.h"
#include "TFDTransition.h"
#include "TFDBleedout.h"
#include "TFDBleedoutGreet.h"
#include "TFDPleasureRuntime.h"
#include "TFDTeammateManager.h"
#include "EditorIdCache.h"
#include "RE/B/BGSRefAlias.h"
#include "RE/T/TESQuest.h"

namespace TFD::DefeatMonitor
{
	static RE::Actor* ResolveCurrentPassivePrimaryActor();
	static bool IsActorCoveredByCurrentPassiveContext(RE::Actor* actor);

	namespace
	{
		static bool ActorHasLineOfSightToPlayer(RE::Actor* actor, RE::Actor* player);
		static float Distance3D(const RE::NiPoint3& a, const RE::NiPoint3& b);
		using CaptivePhaseValue = TFD::Captive::PhaseValue;

		constexpr const char* kBleedoutPrimeSpeakerEvent = "TFDBleedoutPrimeSpeaker";
		constexpr const char* kBleedoutOutcomePayEvent = "TFDBleedoutOutcomePay";
		constexpr const char* kBleedoutOutcomePleasureEvent = "TFDBleedoutOutcomePleasure";
		constexpr const char* kBleedoutOutcomeCaptiveEvent = "TFDBleedoutOutcomeCaptive";
		constexpr const char* kBleedoutOutcomeReleaseEvent = "TFDBleedoutOutcomeRelease";
		constexpr const char* kBleedoutOutcomeResetEvent = "TFDBleedoutOutcomeReset";
		constexpr const char* kInCombatOutcomeReleaseEvent = "TFDInCombatOutcomeRelease";
		constexpr const char* kInCombatOutcomeFollowEvent = "TFDInCombatOutcomeFollow";
		constexpr const char* kInCombatOutcomePayEvent = "TFDInCombatOutcomePay";
		constexpr const char* kInCombatOutcomePleasureEvent = "TFDInCombatOutcomePleasure";
		constexpr const char* kInCombatOutcomeCaptiveEvent = "TFDInCombatOutcomeCaptive";
		constexpr const char* kInCombatOutcomeResetEvent = "TFDInCombatOutcomeReset";
		constexpr const char* kPleasureOutcomeReleaseEvent = "TFDPleasureOutcomeRelease";
		constexpr const char* kAfterPleasureEnterEvent = "TFDAfterPleasureEnter";

		static std::uint32_t ResolveBleedFlowActorFormID();
		static TFD::Bleedout::SupportBridgeHandlers BuildBleedSupportBridgeHandlers();
		static TFD::Bleedout::RuntimeHostStateRefs BuildBleedRuntimeHostStateRefs();
		static TFD::Bleedout::DialogueHotkeyHandlers BuildBleedDialogueHotkeyHandlers();
		static TFD::Bleedout::RuntimeHostHandlers BuildLocalBleedRuntimeHostHandlers();
		static TFD::Bleedout::RuntimeHostHandlers BuildBleedRuntimeHostHandlers();
		static void ApplyBleedDialogueOverdrive(RE::Actor* player, RE::Actor* speaker, const char* reason, bool restartDialogue);
		static bool PromoteNextBleedSpeakerFromTruceQueue(RE::Actor* player, const char* reason, bool rejectCurrent);
		static void MaintainBleedPrimaryCaptorBinding();
		static void QueueNonCaptiveChoiceRequest(const char* reason);
		static bool IsStandingAllyThresholdActor(RE::Actor* actor);
		static TFD::Transition::RuntimeHandlers BuildTransitionRuntimeHandlers();
		static TFD::Transition::CaptiveHandlers BuildTransitionCaptiveHandlers();

		using BleedDialogueOutcome = TFD::Bleedout::DialogueOutcome;
		using BleedTerminalCommit = TFD::Bleedout::TerminalCommit;

		std::atomic_bool g_installed{ false };
		std::atomic_bool g_running{ false };
		std::atomic_bool g_loadTransition{ false };
		std::atomic_flag g_tickPending = ATOMIC_FLAG_INIT;
		std::thread g_worker{};

		static RE::TESGlobal* g_defeatStateGlobal = nullptr;
		static RE::TESGlobal* g_hostileStateGlobal = nullptr;
		static RE::TESGlobal* g_enemyFactionStateGlobal = nullptr;
		static RE::TESGlobal* g_enemyRaceStateGlobal = nullptr;
		static RE::TESGlobal* g_recoveryStateGlobal = nullptr;
		static RE::TESGlobal* g_leftForDeadStateGlobal = nullptr;
		static bool g_loggedDefeatStateGlobal = false;
		static bool g_loggedHostileStateGlobal = false;
		static bool g_loggedEnemyFactionStateGlobal = false;
		static bool g_loggedEnemyRaceStateGlobal = false;
		static bool g_loggedRecoveryStateGlobal = false;
		static bool g_loggedLeftForDeadStateGlobal = false;
		static std::chrono::steady_clock::time_point g_victoryCombatContextUntil{};
		static constexpr int kVictoryContextLingerMs = 2500;
		static bool g_lastRouterCombatContextActive = false;

		static inline std::chrono::steady_clock::time_point Now()
		{
			return std::chrono::steady_clock::now();
		}

		std::atomic_bool g_inBleedState{ false };
		float g_minHp{ 0.0f };
		bool g_playerBleedImmuneForced = false;
		bool g_playerWasEssential = false;
		bool g_playerWasInvulnerable = false;
		bool g_playerWasNoBleedoutRecovery = false;
		bool g_playerWasBaseInvulnerable = false;

		std::chrono::steady_clock::time_point g_bleedStart{};
		int g_bleedLastSeconds = -1;
		bool g_bleedPaused = false;
		std::chrono::steady_clock::time_point g_bleedPauseStarted{};
		std::chrono::steady_clock::time_point g_bleedLastCalmPulse{};

		std::atomic_bool g_grace{ false };
		std::chrono::steady_clock::time_point g_graceUntil{};

		static bool IsGraceActive()
		{
			if (!g_grace.load(std::memory_order_acquire)) {
				return false;
			}
			if (Now() >= g_graceUntil) {
				g_grace.store(false, std::memory_order_release);
				g_graceUntil = {};
				return false;
			}
			return true;
		}

		static RE::Actor* FindBestBleedoutSpeaker(float radius, float maxDist, RE::Actor* preferred)
		{
			(void)radius;
			(void)maxDist;
			(void)preferred;
			return nullptr;
		}

		static void SetGraceSeconds(int seconds)
		{
			if (seconds <= 0) {
				g_grace.store(false, std::memory_order_release);
				g_graceUntil = {};
				return;
			}
			g_graceUntil = Now() + std::chrono::seconds(seconds);
			g_grace.store(true, std::memory_order_release);
		}

		RE::ActorHandle g_lastAggressor{};

		static std::uint32_t CurrentBleedSpeakerID()
		{
			return TFD::Bleedout::GetBleedSpeakerID();
		}

		static RE::Actor* CurrentBleedSpeaker()
		{
			return TFD::Bleedout::GetBleedSpeakerActor();
		}

		static void ResetBleedSpeakerKickState()
		{
			TFD::Bleedout::ResetBleedSpeakerKick();
		}

		static int CurrentBleedDialogueRetryCount()
		{
			return TFD::Bleedout::GetBleedDialogueRetryCount();
		}

		static void SetCurrentBleedDialogueRetryCount(int count)
		{
			TFD::Bleedout::SetBleedDialogueRetryCount(count);
		}

		static void ClearBleedRejectedSpeakerSet()
		{
			TFD::Bleedout::ClearBleedRejectedSpeakerIds();
		}

		static bool HasBleedRejectedSpeakerID(std::uint32_t actorID)
		{
			return TFD::Bleedout::HasBleedRejectedSpeakerID(actorID);
		}

		static void AddBleedRejectedSpeakerID(std::uint32_t actorID)
		{
			TFD::Bleedout::AddBleedRejectedSpeakerID(actorID);
		}

		static std::uint32_t ResolveBleedFlowActorFormID()
		{
			const auto speakerId = CurrentBleedSpeakerID();
			if (speakerId != 0) {
				return speakerId;
			}
			return TFD::FlowController::Controller::GetSingleton().GetSnapshot().primaryActorFormID;
		}
		RE::ActorHandle g_lastEnemyTargetingPlayer{};
		RE::FormID g_lastEnemyTargetingPlayerFormID = 0;
		std::chrono::steady_clock::time_point g_lastEnemyTargetingPlayerSeen{};
		bool g_bleedPendingCaptiveOutcome = false;
		bool g_bleedPendingNonCaptiveOutcome = false;

		struct BleedBattleObserverState
		{
			bool hadValidObservedEnemy = false;
			std::unordered_set<RE::FormID> allyIds{};
			std::unordered_set<RE::FormID> enemyIds{};
		};

		bool g_bleedBattleObservePending = false;
		std::chrono::steady_clock::time_point g_bleedBattleObservePendingUntil{};
		std::chrono::steady_clock::time_point g_bleedBattleObservePendingLastRedirect{};
		int g_bleedBattleObservePendingEmptyEnemyTicks = 0;
		bool g_bleedBattleObserveActive = false;
		std::chrono::steady_clock::time_point g_bleedBattleObserveSince{};
		std::chrono::steady_clock::time_point g_bleedBattleObserveLastRedirect{};
		int g_bleedBattleObserveActiveEmptyEnemyTicks = 0;
		RE::ActorHandle g_bleedBattlePreferredEnemy{};
		BleedBattleObserverState g_bleedBattleObserver{};
		thread_local std::uint32_t g_observedCombatCommitDepth = 0;

		struct ObservedCombatCommitScope
		{
			ObservedCombatCommitScope()
			{
				++g_observedCombatCommitDepth;
			}

			~ObservedCombatCommitScope()
			{
				if (g_observedCombatCommitDepth > 0) {
					--g_observedCombatCommitDepth;
				}
			}
		};

		enum class BleedLockKind : std::uint8_t
		{
			Player = 0,
			Ally = 1,
			Other = 2
		};

		struct BleedLockEntry
		{
			RE::ActorHandle handle{};
			BleedLockKind kind{ BleedLockKind::Other };
			float thresholdPct{ 0.0f };
			float minHp{ 0.0f };
			float protectedHealth{ 0.0f };
			float lastHealthSample{ 0.0f };
			float healAccumulator{ 0.0f };
			float savedHealRate{ 0.0f };
			float savedHealRateMult{ 100.0f };
			float savedCombatHealRateMult{ 1.0f };
			float savedAggression{ 1.0f };
			bool regenOverridden{ false };
			bool aggressionOverridden{ false };
			bool defeatedManaged{ false };
			bool defeatedFactionApplied{ false };
			bool defeatedAutoDeathIssued{ false };
			bool defeatedFatalDamageApplied{ false };
			int defeatedAliasSlot{ -1 };
			std::chrono::steady_clock::time_point defeatedDeadline{};
			std::chrono::steady_clock::time_point lastPulse{};
			std::chrono::steady_clock::time_point lastDamageLog{};
			std::chrono::steady_clock::time_point lastHealGain{};
		};

		std::unordered_map<RE::FormID, BleedLockEntry> g_bleedLocks{};
		std::chrono::steady_clock::time_point g_bleedLockLastScan{};

		bool g_prevDialogueOpen = false;


		static constexpr double kDefeatedEnemyKnockSeconds = 30.0;
		static constexpr double kDefeatedReentrySuppressSeconds = 6.0;
		RE::ObjectRefHandle g_pendingDefeatedDialogueTarget{};
		std::chrono::steady_clock::time_point g_pendingDefeatedDialogueExpiry{};
		constexpr double kPendingDefeatedDialogueTargetSeconds = 20.0;

		static constexpr std::size_t kBleedBridgeMaxActors = 10;

		bool g_hasQueuedProgressState = false;
		bool g_queuedBleedOutState = false;

		static RE::PlayerCharacter* Player()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

		static void SetPlayerBleedImmune(bool enable)
		{
			auto* player = Player();
			if (!player) {
				return;
			}

			auto& boolFlags = player->GetActorRuntimeData().boolFlags;
			auto* actorBase = player->GetActorBase();
			auto* baseData = actorBase ? static_cast<RE::TESActorBaseData*>(actorBase) : nullptr;
			if (enable) {
				if (!g_playerBleedImmuneForced) {
					g_playerWasEssential = boolFlags.all(RE::Actor::BOOL_FLAGS::kEssential);
					g_playerWasInvulnerable = boolFlags.all(RE::Actor::BOOL_FLAGS::kProtected);
					g_playerWasNoBleedoutRecovery = boolFlags.all(RE::Actor::BOOL_FLAGS::kNoBleedoutRecovery);
					g_playerWasBaseInvulnerable = baseData && baseData->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kInvulnerable);
					g_playerBleedImmuneForced = true;
					spdlog::info("[TFD][Defeat] player bleed hard-invuln enabled essentialWas={} protectedWas={} noBleedoutWas={} baseInvulnWas={}",
						g_playerWasEssential ? 1 : 0,
						g_playerWasInvulnerable ? 1 : 0,
						g_playerWasNoBleedoutRecovery ? 1 : 0,
						g_playerWasBaseInvulnerable ? 1 : 0);
				}

				boolFlags.set(RE::Actor::BOOL_FLAGS::kEssential);
				boolFlags.set(RE::Actor::BOOL_FLAGS::kProtected);
				boolFlags.set(RE::Actor::BOOL_FLAGS::kCanSpeakToEssentialDown);
				boolFlags.set(RE::Actor::BOOL_FLAGS::kNoBleedoutRecovery);
				boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
				if (baseData) {
					baseData->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kInvulnerable);
				}
				return;
			}

			if (!g_playerBleedImmuneForced) {
				return;
			}

			if (!g_playerWasEssential) {
				boolFlags.reset(RE::Actor::BOOL_FLAGS::kEssential);
			}
			if (!g_playerWasInvulnerable) {
				boolFlags.reset(RE::Actor::BOOL_FLAGS::kProtected);
			}
			if (!g_playerWasNoBleedoutRecovery) {
				boolFlags.reset(RE::Actor::BOOL_FLAGS::kNoBleedoutRecovery);
			}
			boolFlags.reset(RE::Actor::BOOL_FLAGS::kCanSpeakToEssentialDown);
			boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
			if (baseData && !g_playerWasBaseInvulnerable) {
				baseData->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kInvulnerable);
			}
			g_playerBleedImmuneForced = false;
			g_playerWasEssential = false;
			g_playerWasInvulnerable = false;
			g_playerWasNoBleedoutRecovery = false;
			g_playerWasBaseInvulnerable = false;
			spdlog::info("[TFD][Defeat] player bleed hard-invuln released");
		}

		static RE::BGSKeyword* LookupKeyword(const char* editorID)
		{
			if (!editorID || !editorID[0]) {
				return nullptr;
			}
			return RE::TESForm::LookupByEditorID<RE::BGSKeyword>(editorID);
		}

		static bool ActorHasKeywordByEditorID(RE::Actor* actor, const char* editorID)
		{
			if (!actor) {
				return false;
			}
			auto* kw = LookupKeyword(editorID);
			return kw && actor->HasKeyword(kw);
		}

		static bool ActorHasLineOfSightToPlayer(RE::Actor* actor, RE::Actor* player)
		{
			if (!actor || !player) {
				return false;
			}
			bool hasLOSData = false;
			return actor->HasLineOfSight(player, hasLOSData);
		}

		static bool IsCaptiveSupportedAggressor(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}

			if (ActorHasKeywordByEditorID(actor, "ActorTypeNPC")) {
				return true;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeCreature")) {
				return false;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeAnimal")) {
				return false;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeDragon")) {
				return false;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeDaedra")) {
				return false;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeGhost")) {
				return false;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeUndead")) {
				return false;
			}

			return false;
		}

		static bool IsActiveFollowerActor(RE::Actor* actor);
		static bool IsStandingEnemyThresholdActor(RE::Actor* actor);

		static bool IsCombatSupportedAggressor(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			if (actor == Player()) {
				return false;
			}
			if (actor->IsDead() || actor->IsDisabled()) {
				return false;
			}
			if (IsActiveFollowerActor(actor) || TFD::Tame::IsCompanion(actor)) {
				return false;
			}
			return true;
		}

		static bool IsReasonableCombatAggressor(RE::Actor* actor, RE::Actor* player, float maxDist, float* outDistance = nullptr)
		{
			if (outDistance) {
				*outDistance = -1.0f;
			}
			if (!actor || !player) {
				return false;
			}
			if (!IsCombatSupportedAggressor(actor)) {
				return false;
			}
			if (!IsStandingEnemyThresholdActor(actor) || !actor->Is3DLoaded()) {
				return false;
			}
			auto* playerCell = player->GetParentCell();
			auto* actorCell = actor->GetParentCell();
			if (playerCell && actorCell != playerCell) {
				return false;
			}
			auto* playerWs = player->GetWorldspace();
			if (playerWs && actor->GetWorldspace() != playerWs) {
				return false;
			}
			const auto pp = player->GetPosition();
			const auto ap = actor->GetPosition();
			const float dx = ap.x - pp.x;
			const float dy = ap.y - pp.y;
			const float dz = ap.z - pp.z;
			const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (outDistance) {
				*outDistance = dist;
			}
			if (maxDist > 0.0f && dist > maxDist) {
				return false;
			}
			if (actor->IsHostileToActor(player) || actor->IsInCombat()) {
				return true;
			}
			auto targetSp = actor->GetActorRuntimeData().currentCombatTarget.get();
			auto* currentTarget = targetSp.get();
			if (currentTarget == player) {
				return true;
			}
			if (currentTarget && IsActiveFollowerActor(currentTarget)) {
				return true;
			}
			return false;
		}

		static bool IsBleedCrowdSupportedAggressor(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			if (IsActiveFollowerActor(actor)) {
				return false;
			}

			if (ActorHasKeywordByEditorID(actor, "ActorTypeDragon") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeGhost")) {
				return false;
			}

			if (ActorHasKeywordByEditorID(actor, "ActorTypeNPC") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeCreature") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeAnimal") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeUndead") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeDaedra")) {
				return true;
			}

			return false;
		}

		static RE::Actor* ResolveAggressor();
		static RE::Actor* ResolveLastEnemyTargetingPlayerInternal(float radius = 0.0f, double maxAgeSec = 15.0);
		static void NoteEnemyTargetingPlayerInternal(RE::Actor* actor);
		static void ClearLastEnemyTargetingPlayerInternal();
		static RE::Actor* FindBestAggressor(float radius);
		static void SetPendingDefeatedDialogueTargetInternal(RE::Actor* actor);
		static RE::Actor* ResolvePendingDefeatedDialogueTargetInternal();
		static void ClearPendingDefeatedDialogueTargetInternal();

		static void SetPendingDefeatedDialogueTargetInternal(RE::Actor* actor)
		{
			g_pendingDefeatedDialogueTarget = {};
			g_pendingDefeatedDialogueExpiry = {};
			if (!actor) {
				return;
			}
			g_pendingDefeatedDialogueTarget = actor->GetHandle();
			g_pendingDefeatedDialogueExpiry = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(kPendingDefeatedDialogueTargetSeconds * 1000.0));
		}

		static RE::Actor* ResolvePendingDefeatedDialogueTargetInternal()
		{
			if (!g_pendingDefeatedDialogueTarget || std::chrono::steady_clock::now() >= g_pendingDefeatedDialogueExpiry) {
				g_pendingDefeatedDialogueTarget = {};
				g_pendingDefeatedDialogueExpiry = {};
				return nullptr;
			}
			auto actor = g_pendingDefeatedDialogueTarget.get().get();
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				g_pendingDefeatedDialogueTarget = {};
				g_pendingDefeatedDialogueExpiry = {};
				return nullptr;
			}
			return actor->As<RE::Actor>();
		}

		static void ClearPendingDefeatedDialogueTargetInternal()
		{
			g_pendingDefeatedDialogueTarget = {};
			g_pendingDefeatedDialogueExpiry = {};
		}

		static bool IsTrackedDefeatedEnemyHook(RE::Actor* actor)
		{
			return actor && g_bleedBattleObserver.enemyIds.find(actor->GetFormID()) != g_bleedBattleObserver.enemyIds.end();
		}

		static bool IsLastAggressorHook(RE::Actor* actor)
		{
			if (!actor || !g_lastAggressor) {
				return false;
			}
			auto sp = RE::Actor::LookupByHandle(g_lastAggressor.native_handle());
			return sp.get() == actor;
		}

		static std::vector<RE::Actor*> CollectBleedStandingFollowers(float radius);
		static bool IsStandingObserverActor(RE::Actor* actor);
		static bool IsObserverAlly(RE::Actor* actor);
		static bool IsObserverEnemy(RE::Actor* actor, RE::Actor* player, const std::vector<RE::Actor*>& allies, bool hostileHint, bool inCombatHint);
		static bool IsValidBleedBattleEnemyRosterActor(RE::Actor* actor, RE::Actor* player);
		static std::vector<RE::Actor*> CollectBleedStandingFollowersFromSnapshot();
		static std::vector<RE::Actor*> CollectBleedStandingEnemiesFromSnapshot();
		static bool StartBleedBattleObservePending(RE::Actor* player);
		static void TickBleedBattleObservePending();
		static void TickBleedBattleObserve();
		static RE::Actor* ResolveBleedRedirectTargetInternal(RE::Actor* actor);
		static void EnterObservedBattleWin();
		static void ReleaseBleedLock(RE::Actor* actor, const char* reason, bool playGetUp);
		static void EnterObservedLeftForDead(const char* reason);
		static bool IsActorBleedingOut(RE::Actor* actor);
		static RE::Actor* ResolveCurrentCombatTarget(RE::Actor* actor);
		static bool TryAbortPleasureDueToHostileIntrusion(float radius);
		static void QueueNonCaptiveChoiceRequest(const char* reason);
		static void ClampHealth(RE::Actor* actor, float minHp);
		static bool ComputePlayerBleedOutState(RE::Actor* player);
		static void ResolveMonitorGlobals();
		static void RefreshPostDefeatGlobals();
		static void SetRescueStateValue(int value);
		static void RecoverVictoryTeammates();

		static bool SendBridgeModEvent(const char* eventName, RE::TESForm* sender = nullptr, const char* strArg = "", float numArg = 0.0f)
		{
			return TFD::FlowController::QueueBridgeModEvent(eventName, sender, strArg, numArg);
		}

		static bool IsActorCloseAndFront(RE::Actor* actor, RE::Actor* player, float maxDist);
		static bool IsBleedSpaceCompatible(RE::Actor* actor, RE::Actor* player);

		static void ClearBleedSupportBridgeAliases(const char* reason);

		static bool IsBleedSpaceCompatible(RE::Actor* actor, RE::Actor* player)
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

		static TFD::Bleedout::SupportBridgeHandlers BuildBleedSupportBridgeHandlers()
		{
			TFD::Bleedout::SupportBridgeHandlers handlers{};
			handlers.queuePreCombatClearAll = []() { return SendBridgeModEvent("TFDPreCombatClearAll", nullptr); };
			handlers.queueTruceClearAll = []() { return SendBridgeModEvent("TFDTruceClearAll", nullptr); };
			handlers.queueInCombatClearAll = []() { return SendBridgeModEvent("TFDInCombatClearAll", nullptr); };
			handlers.cancelAllPreCombat = []() { TFD::PreCombatGreet::CancelAll(); };
			return handlers;
		}

		static void ClearBleedSupportBridgeAliases(const char* reason)
		{
			TFD::Bleedout::ClearSupportBridgeAliases(reason, BuildBleedSupportBridgeHandlers());
		}

		static TFD::Bleedout::SpeakerLogicHandlers BuildBleedoutSpeakerHandlers(bool preserveAssigned = false)
		{
			TFD::Bleedout::SpeakerLogicHandlers handlers{};
			handlers.getPlayer = []() -> RE::Actor* { return Player(); };
			handlers.isStandingEnemyThresholdActor = [](RE::Actor* actor) { return IsStandingEnemyThresholdActor(actor); };
			handlers.isCaptiveSupportedAggressor = [](RE::Actor* actor) { return IsCaptiveSupportedAggressor(actor); };
			handlers.isBleedCrowdSupportedAggressor = [](RE::Actor* actor) { return IsBleedCrowdSupportedAggressor(actor); };
			handlers.isBleedSpaceCompatible = [](RE::Actor* actor, RE::Actor* player) { return IsBleedSpaceCompatible(actor, player); };
			handlers.hasLineOfSightToPlayer = [](RE::Actor* actor, RE::Actor* player) { return ActorHasLineOfSightToPlayer(actor, player); };
			handlers.isActorCloseAndFront = [](RE::Actor* actor, RE::Actor* player, float maxDist) { return IsActorCloseAndFront(actor, player, maxDist); };
			handlers.resolveCurrentCombatTarget = [](RE::Actor* actor) -> RE::Actor* { return ResolveCurrentCombatTarget(actor); };
			handlers.isActiveFollowerActor = [](RE::Actor* actor) { return IsActiveFollowerActor(actor); };
			handlers.resolveLastAggressor = []() -> RE::Actor* {
				if (!g_lastAggressor) {
					return nullptr;
				}
				auto sp = RE::Actor::LookupByHandle(g_lastAggressor.native_handle());
				return sp.get();
				};
			handlers.isPreservedAssigned = [preserveAssigned](RE::Actor* actor) {
				if (!preserveAssigned || !actor) {
					return false;
				}
				return TFD::Bleedout::HasBleedCrowdAssignedID(actor->GetFormID());
				};
			return handlers;
		}

		static std::vector<RE::Actor*> CollectBleedoutCrowd(float radius, RE::Actor* preferred, bool preserveAssigned = false)
		{
			return TFD::Bleedout::CollectCrowd(radius, preferred, preserveAssigned, BuildBleedoutSpeakerHandlers(preserveAssigned));
		}

		static void ReleasePlayerBleedLock(const char* reason, bool playGetUp);

		static void ResetBleedRuntimeState(bool preserveCaptive = false)
		{
			TFD::Bleedout::RuntimeResetHandlers handlers{};
			handlers.releasePlayerBleedLock = [&](const char* reason) { ReleasePlayerBleedLock(reason, false); };
			handlers.releaseBleedTruceSession = [&]() { TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::Generic); };
			handlers.releaseNoSpeakerTameSession = [&](const char* reason) { TFD::Tame::ReleaseBleedNoSpeakerTameSession(reason); };
			handlers.clearBridgeAliases = [&](const char* reason) { ClearBleedSupportBridgeAliases(reason); };
			handlers.setBleedActive = [&](bool active, const char*) {
				g_inBleedState.store(active, std::memory_order_release);
				if (!active) {
					g_minHp = 0.0f;
				}
				};
			handlers.resetGreetRuntime = [&](const char* reason) { TFD::BleedoutGreet::ResetRuntime(reason); };
			handlers.resetSystemEventState = [&](const char* reason) { TFD::Bleedout::ResetSystemEventState(reason); };
			handlers.clearCaptorAliases = [&](const char* reason) { TFD::Bleedout::ClearCaptorAliases(reason); };
			handlers.clearDialogueOutcome = [&](const char* reason) { TFD::Bleedout::ClearDialogueOutcome(reason); };
			handlers.resetBattleObserveTracking = [&]() {
				g_bleedBattlePreferredEnemy.reset();
				g_bleedBattleObserver = {};
				};
			handlers.resetDialogueRuntimeState = [&]() {
				g_bleedPendingCaptiveOutcome = false;
				g_bleedPendingNonCaptiveOutcome = false;
				g_bleedPaused = false;
				g_bleedPauseStarted = {};
				ResetBleedSpeakerKickState();
				SetCurrentBleedDialogueRetryCount(0);
				g_bleedLastCalmPulse = {};
				TFD::Bleedout::BleedLastCrowdAssignRef() = {};
				TFD::Bleedout::ClearBleedCrowdAssigned();
				ClearBleedRejectedSpeakerSet();
				g_bleedStart = Now();
				g_bleedLastSeconds = -1;
				};
			handlers.resetBattleObserveState = [&]() {
				g_bleedBattleObservePending = false;
				g_bleedBattleObservePendingUntil = {};
				g_bleedBattleObservePendingLastRedirect = {};
				g_bleedBattleObservePendingEmptyEnemyTicks = 0;
				g_bleedBattleObserveActive = false;
				g_bleedBattleObserveSince = {};
				g_bleedBattleObserveLastRedirect = {};
				g_bleedBattleObserveActiveEmptyEnemyTicks = 0;
				};
			handlers.clearEscapeBreakState = [&]() { TFD::Captive::ClearEscapeBreakRebleed(); };
			handlers.clearLastEnemyTargetingPlayer = [&]() { ClearLastEnemyTargetingPlayerInternal(); };
			handlers.clearOutcomeWindow = [&](const char* reason) { TFD::Bleedout::ClearSystemEventOutcomeWindow(reason); };
			handlers.resetPleasureRuntime = [&](const char* reason) { TFD::PleasureRuntime::ResetRuntime(reason); };

			TFD::Bleedout::ResetRuntimeState(preserveCaptive, "reset_bleed_runtime", handlers);
		}

		static bool TryAbortPleasureDueToHostileIntrusion(float radius)
		{
			if (!TFD::PleasureRuntime::IsActive() && !TFD::PleasureRuntime::IsBlocking()) {
				return false;
			}

			auto* player = Player();
			auto* speaker = TFD::PleasureRuntime::GetPrimarySpeaker();
			if (!player || !speaker || speaker == player || speaker->IsDead() || speaker->IsDisabled()) {
				return false;
			}

			auto snapshot = TFD::Actor::BuildSnapshot(radius, false);
			for (const auto& info : snapshot.actors) {
				auto* actor = info.get();
				const bool hostileToPlayer = info.hostileToPlayer;
				const bool inCombat = info.inCombat;
				if (!actor || actor == player || actor == speaker) {
					continue;
				}
				if (actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
					continue;
				}
				if (IsActiveFollowerActor(actor) || TFD::PleasureRuntime::IsActorTracked(actor)) {
					continue;
				}
				if (player->GetParentCell() && actor->GetParentCell() != player->GetParentCell()) {
					continue;
				}

				const bool hostileToSpeaker = actor->IsHostileToActor(speaker);
				if (!hostileToPlayer || !hostileToSpeaker || !inCombat) {
					continue;
				}

				auto* target = ResolveCurrentCombatTarget(actor);
				const bool targetingPlayer = target == player;
				const bool targetingSpeaker = target == speaker;
				const float playerDist = Distance3D(actor->GetPosition(), player->GetPosition());
				const float speakerDist = Distance3D(actor->GetPosition(), speaker->GetPosition());
				const float bestDist = (std::min)(playerDist, speakerDist);
				if (!(targetingPlayer || targetingSpeaker) && bestDist > 900.0f) {
					continue;
				}

				spdlog::warn(
					"[TFD][PleasureAbort] hostile intrusion actor={:08X} speaker={:08X} phase={} targetingPlayer={} targetingSpeaker={} target={:08X}",
					actor->GetFormID(),
					speaker->GetFormID(),
					TFD::PleasureRuntime::GetPhaseName(),
					targetingPlayer ? 1 : 0,
					targetingSpeaker ? 1 : 0,
					target ? target->GetFormID() : 0u);

				TFD::PleasureRuntime::Break("hostile_intrusion", true, true, true);
				TFD::FlowController::Controller::GetSingleton().ResetRuntime("hostile_intrusion");
				ResetBleedRuntimeState();
				TFD::HostilityController::ClearAggressionClamp();
				TFD::Actor::Ops::ClearAggressorFactionContext();
				ClearPendingDefeatedDialogueTargetInternal();
				return true;
			}

			return false;
		}

		static void TransitionBleedRuntimeToPleasureCommit(const char* reason)
		{
			TFD::Bleedout::TransitionRuntimeToPleasureCommit(
				reason,
				static_cast<std::uint32_t>(CurrentBleedSpeakerID()),
				TFD::Bleedout::GetTruceSessionID() != 0,
				static_cast<std::uint32_t>(TFD::Bleedout::GetActiveCaptorFormID()),
				TFD::Bleedout::RuntimePleasureCommitHandlers{
					[&](const char* why) { TFD::Tame::ReleaseBleedNoSpeakerTameSession(why); },
					[&](const char* why) { ClearBleedSupportBridgeAliases(why); },
					[&](bool active, const char*) {
						g_inBleedState.store(active, std::memory_order_release);
						if (!active) {
							g_minHp = 0.0f;
						}
					},
					[&](const char* why) { TFD::BleedoutGreet::ResetRuntime(why); },
					[&]() {
						g_bleedPendingCaptiveOutcome = false;
						g_bleedPendingNonCaptiveOutcome = false;
						g_bleedPaused = false;
						g_bleedPauseStarted = {};
						ResetBleedSpeakerKickState();
						SetCurrentBleedDialogueRetryCount(0);
						g_bleedLastCalmPulse = {};
						TFD::Bleedout::BleedLastCrowdAssignRef() = {};
						TFD::Bleedout::ClearBleedCrowdAssigned();
						ClearBleedRejectedSpeakerSet();
						g_bleedStart = Now();
						g_bleedLastSeconds = -1;
					},
					[&]() {
						g_bleedBattleObservePending = false;
						g_bleedBattleObservePendingUntil = {};
						g_bleedBattleObservePendingLastRedirect = {};
						g_bleedBattleObservePendingEmptyEnemyTicks = 0;
						g_bleedBattleObserveActive = false;
						g_bleedBattleObserveSince = {};
						g_bleedBattleObserveLastRedirect = {};
						g_bleedBattleObserveActiveEmptyEnemyTicks = 0;
						g_bleedBattlePreferredEnemy.reset();
						g_bleedBattleObserver = {};
					},
					[&]() { TFD::Captive::ClearEscapeBreakRebleed(); },
					[&]() { ClearLastEnemyTargetingPlayerInternal(); },
					[&](const char* why) { TFD::Bleedout::ClearSystemEventOutcomeWindow(why); }
				});
		}

		static CaptivePhaseValue PhaseFromRaw(std::uint32_t raw)
		{
			return TFD::Captive::PhaseFromRaw(raw);
		}

		static RE::Actor* ResolveAggressor();
		static RE::Actor* FindBestAggressor(float radius);
		static RE::Actor* FindBestBleedoutSpeaker(float radius, float maxDist, RE::Actor* preferred = nullptr);
		static bool IsReasonableBleedoutSpeaker(RE::Actor* actor, RE::Actor* player, float maxDist, float* outDistance = nullptr);
		static void SetGraceSeconds(int seconds);
		static bool IsGraceActive();
		static void UpdatePreCombatState();
		static bool IsActorCloseAndFront(RE::Actor* actor, RE::Actor* player, float maxDist);
		static bool CanUseAggressorForBleedoutGreet(RE::Actor* player, RE::Actor* aggressor, float& outDistance);
		static void PreparePlayerForBleedoutPleasureScene(const char* reason);
		static void PreparePlayerForCaptivePleasureScene(const char* reason);
		static void StartBleedWindow(RE::Actor* player, RE::Actor* aggressor);
		static bool BeginBleedoutDialogueHotkey();
		static bool HandlePendingEscapeBreakBleed();
		static void MaintainBleedSpeakerKick();
		static void ClearBleedDialogueOutcome(const char* reason);
		static bool BeginResolvedNoMarkerFallback(const char* reason);
		static bool IsDialogueOpen();
		static void ApplyCalmBubble(float radius);
		static void ClearCaptiveOrchestrationResidue(bool clearPendingFadeIn = true);
		static RE::Actor* ResolveBleedRuntimeSpeaker();
		static void BeginBleedPleasureRuntime(RE::Actor* speaker, bool captive, const char* reason);
		static void ExitBleedSystemEventRuntime();
		static TFD::Bleedout::PendingSystemEventHandlers BuildBaseBleedPendingSystemEventHandlers();
		static TFD::Bleedout::DialogueCloseHandlers BuildBleedDialogueCloseHandlers();
		static TFD::Bleedout::TimeoutHandlers BuildBleedTimeoutHandlers();
		static TFD::Bleedout::CompletionHandlers BuildBaseBleedCompletionHandlers();
		static TFD::Bleedout::CompletionHandlers BuildCaptivePleasureCompletionHandlers();
		static TFD::Bleedout::CompletionHandlers BuildPayReleaseCompletionHandlers();
		static TFD::Bleedout::CompletionHandlers BuildBleedPleasureCompletionHandlers();
		static TFD::Bleedout::NonCaptiveChoiceHandlers BuildBleedNonCaptiveChoiceHandlers();
		static TFD::Bleedout::BlackoutHandlers BuildBleedBlackoutHandlers();

		static void StartBleedWindow(RE::Actor* player, RE::Actor* aggressor)
		{
			TFD::Bleedout::RuntimeHost::StartWindow(player, aggressor);
		}

		static bool BeginBleedoutDialogueHotkey()
		{
			return TFD::Bleedout::DefeatGlue::BeginDialogueHotkey();
		}

		static bool HandlePendingEscapeBreakBleed()
		{
			return TFD::Bleedout::DefeatGlue::HandlePendingEscapeBreak();
		}

		static void MaintainBleedSpeakerKick()
		{
			TFD::Bleedout::DefeatGlue::MaintainSpeakerKick();
		}

		static void ClearBleedDialogueOutcome(const char* reason)
		{
			TFD::Bleedout::ClearDialogueOutcome(reason);
		}

		static void ClearCaptiveOrchestrationResidue(bool clearPendingFadeIn)
		{
			TFD::Bleedout::DefeatGlue::ClearCaptiveOrchestrationResidue(clearPendingFadeIn);
		}

		static RE::Actor* ResolveBleedRuntimeSpeaker()
		{
			return TFD::Bleedout::DefeatGlue::ResolveBleedRuntimeSpeaker();
		}

		static void BeginBleedPleasureRuntime(RE::Actor* speaker, bool captive, const char* reason)
		{
			TFD::Bleedout::DefeatGlue::BeginBleedPleasureRuntime(speaker, captive, reason);
		}

		static void ExitBleedSystemEventRuntime()
		{
			TFD::Bleedout::DefeatGlue::ExitSystemEventRuntime();
		}

		static TFD::Bleedout::RuntimeHostStateRefs BuildBleedRuntimeHostStateRefs()
		{
			return TFD::Bleedout::RuntimeHost::BuildStateRefs();
		}

		static TFD::Bleedout::DialogueHotkeyHandlers BuildBleedDialogueHotkeyHandlers()
		{
			return TFD::Bleedout::DefeatGlue::BuildDialogueHotkeyHandlers();
		}

		static TFD::Bleedout::RuntimeHostHandlers BuildLocalBleedRuntimeHostHandlers()
		{
			return TFD::Bleedout::DefeatGlue::BuildLocalRuntimeHostHandlers();
		}

		static TFD::Bleedout::RuntimeHostHandlers BuildBleedRuntimeHostHandlers()
		{
			return TFD::Bleedout::RuntimeHost::BuildHandlers();
		}

		static TFD::Transition::RuntimeHandlers BuildTransitionRuntimeHandlers()
		{
			return TFD::Bleedout::Builders::BuildTransitionRuntimeHandlers();
		}

		static TFD::Transition::CaptiveHandlers BuildTransitionCaptiveHandlers()
		{
			return TFD::Bleedout::Builders::BuildTransitionCaptiveHandlers();
		}

		static bool IsStandingObserverActor(RE::Actor* actor)
		{
			return actor && !actor->IsDisabled() && !actor->IsDead() && !IsActorBleedingOut(actor);
		}

		static bool IsObserverAlly(RE::Actor* actor)
		{
			return TFD::Bleedout::DefeatGlue::IsObserverAlly(actor);
		}

		static bool IsObserverEnemy(RE::Actor* actor, RE::Actor* player, const std::vector<RE::Actor*>& allies, bool hostileHint, bool inCombatHint)
		{
			if (!player || !actor || actor == player) {
				return false;
			}
			if (!IsStandingEnemyThresholdActor(actor) || IsObserverAlly(actor)) {
				return false;
			}

			auto* target = ResolveCurrentCombatTarget(actor);
			if (target == player) {
				return true;
			}

			bool hostileToSide = actor->IsHostileToActor(player);
			for (auto* ally : allies) {
				if (!IsStandingAllyThresholdActor(ally)) {
					continue;
				}
				if (target == ally) {
					return true;
				}
				if (actor->IsHostileToActor(ally)) {
					hostileToSide = true;
				}
			}

			if (!hostileHint && !inCombatHint && !actor->IsInCombat() && !hostileToSide) {
				return false;
			}
			return hostileToSide || hostileHint || inCombatHint || actor->IsInCombat();
		}

		static bool IsValidBleedBattleEnemyRosterActor(RE::Actor* actor, RE::Actor* player)
		{
			if (!player || !actor || actor == player) {
				return false;
			}
			if (!IsStandingEnemyThresholdActor(actor) || IsObserverAlly(actor) || !actor->Is3DLoaded()) {
				return false;
			}
			auto* pCell = player->GetParentCell();
			if (pCell && actor->GetParentCell() != pCell) {
				return false;
			}
			return true;
		}

		static std::vector<RE::Actor*> CollectBleedStandingFollowersFromSnapshot()
		{
			return TFD::Bleedout::DefeatGlue::CollectStandingFollowersFromSnapshot();
		}

		static std::vector<RE::Actor*> CollectBleedStandingEnemiesFromSnapshot()
		{
			return TFD::Bleedout::DefeatGlue::CollectStandingEnemiesFromSnapshot();
		}

		static void NoteEnemyTargetingPlayerInternal(RE::Actor* actor)
		{
			if (!actor || !IsCombatSupportedAggressor(actor)) {
				return;
			}
			g_lastEnemyTargetingPlayer = actor->GetHandle();
			g_lastEnemyTargetingPlayerFormID = actor->GetFormID();
			g_lastEnemyTargetingPlayerSeen = Now();
		}

		static void ClearLastEnemyTargetingPlayerInternal()
		{
			g_lastEnemyTargetingPlayer = {};
			g_lastEnemyTargetingPlayerFormID = 0;
			g_lastEnemyTargetingPlayerSeen = {};
		}

		static RE::Actor* ResolveLastEnemyTargetingPlayerInternal(float radius, double maxAgeSec)
		{
			const auto now = Now();
			if (g_lastEnemyTargetingPlayerFormID != 0 && g_lastEnemyTargetingPlayerSeen != std::chrono::steady_clock::time_point{}) {
				const auto age = std::chrono::duration<double>(now - g_lastEnemyTargetingPlayerSeen).count();
				if (age <= maxAgeSec) {
					auto resolveCached = [&](RE::Actor* actor) -> RE::Actor* {
						float dist = -1.0f;
						if (IsReasonableCombatAggressor(actor, Player(), radius, &dist)) {
							return actor;
						}
						return nullptr;
					};
					if (g_lastEnemyTargetingPlayer) {
						if (auto actor = g_lastEnemyTargetingPlayer.get().get()) {
							if (auto* resolved = resolveCached(actor->As<RE::Actor>()); resolved) {
								return resolved;
							}
						}
					}
					if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(g_lastEnemyTargetingPlayerFormID)) {
						if (auto* resolved = resolveCached(actor); resolved) {
							g_lastEnemyTargetingPlayer = resolved->GetHandle();
							return resolved;
						}
					}
				}
			}

			auto* recent = TFD::PreCombatGreet::ResolveRecentAggressor(radius, maxAgeSec);
			if (recent && IsCombatSupportedAggressor(recent)) {
				NoteEnemyTargetingPlayerInternal(recent);
				return recent;
			}
			return nullptr;
		}

		static RE::Actor* FindBestAggressor(float radius)
		{
			auto* player = Player();
			if (!player) {
				return nullptr;
			}

			auto snapshot = TFD::Actor::BuildSnapshot((std::max)(radius, 1600.0f), false);
			RE::Actor* best = nullptr;
			float bestScore = std::numeric_limits<float>::max();
			for (const auto& info : snapshot.actors) {
				auto* actor = info.get();
				float dist = -1.0f;
				if (!IsReasonableCombatAggressor(actor, player, radius, &dist)) {
					continue;
				}
				float score = dist;
				auto* target = ResolveCurrentCombatTarget(actor);
				if (target == player) score -= 3000.0f;
				else if (target && IsActiveFollowerActor(target)) score -= 1200.0f;
				if (actor->IsHostileToActor(player)) score -= 400.0f;
				if (actor->IsInCombat()) score -= 150.0f;
				if (score < bestScore) {
					bestScore = score;
					best = actor;
				}
			}
			if (best) {
				g_lastAggressor = best->GetHandle();
				NoteEnemyTargetingPlayerInternal(best);
			}
			return best;
		}

		static RE::Actor* ResolveAggressor()
		{
			const float radius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 400.0f);
			auto resolveIfReasonable = [&](RE::Actor* actor) -> RE::Actor* {
				float dist = -1.0f;
				return IsReasonableCombatAggressor(actor, Player(), radius, &dist) ? actor : nullptr;
			};

			if (g_lastAggressor) {
				if (auto actor = g_lastAggressor.get().get()) {
					if (auto* resolved = resolveIfReasonable(actor->As<RE::Actor>()); resolved) {
						return resolved;
					}
				}
			}
			if (auto* actor = ResolveLastEnemyTargetingPlayerInternal(radius, 15.0); actor) {
				return actor;
			}
			if (auto* actor = TFD::PreCombatGreet::ResolveRecentAggressor(radius, 12.0); actor && IsCombatSupportedAggressor(actor)) {
				g_lastAggressor = actor->GetHandle();
				NoteEnemyTargetingPlayerInternal(actor);
				return actor;
			}
			return FindBestAggressor(radius);
		}

		static bool StartBleedBattleObservePending(RE::Actor* player)
		{
			return TFD::Bleedout::RuntimeHost::StartBattleObservePending(player);
		}

		static void TickBleedBattleObservePending()
		{
			TFD::Bleedout::RuntimeHost::TickBattleObservePending();
		}

		static void TickBleedBattleObserve()
		{
			TFD::Bleedout::RuntimeHost::TickBattleObserve();
		}

		static RE::Actor* ResolveBleedRedirectTargetInternal(RE::Actor* actor)
		{
			return TFD::Bleedout::DefeatGlue::ResolveBleedRedirectTarget(actor);
		}

		static void EnterObservedBattleWin()
		{
			TFD::Bleedout::DefeatGlue::HandleObservedBattleWin();
		}

		static void EnterObservedLeftForDead(const char* reason)
		{
			TFD::Bleedout::DefeatGlue::HandleObservedLeftForDead(reason);
		}

		static bool IsActorCloseAndFront(RE::Actor* actor, RE::Actor* player, float maxDist)
		{
			if (!actor || !player) {
				return false;
			}

			const auto playerPos = player->GetPosition();
			const auto actorPos = actor->GetPosition();
			const float dx = actorPos.x - playerPos.x;
			const float dy = actorPos.y - playerPos.y;
			const float d2 = dx * dx + dy * dy;
			if (d2 > (maxDist * maxDist)) {
				return false;
			}
			if (d2 <= 1.0f) {
				return true;
			}

			const float len = std::sqrt(d2);
			const float ang = player->GetAngleZ();
			const float fx = std::sin(ang);
			const float fy = std::cos(ang);
			const float nx = dx / len;
			const float ny = dy / len;
			return (nx * fx + ny * fy) >= 0.20f;
		}

		static bool IsReasonableBleedoutSpeaker(RE::Actor* actor, RE::Actor* player, float maxDist, float* outDistance)
		{
			if (outDistance) {
				*outDistance = -1.0f;
			}
			if (!actor || !player || !IsBleedCrowdSupportedAggressor(actor) || !IsBleedSpaceCompatible(actor, player)) {
				return false;
			}
			float dist = -1.0f;
			if (!IsReasonableCombatAggressor(actor, player, maxDist, &dist)) {
				return false;
			}
			if (outDistance) {
				*outDistance = dist;
			}
			return ActorHasLineOfSightToPlayer(actor, player) || IsActorCloseAndFront(actor, player, (std::min)(maxDist, 900.0f));
		}

		static bool CanUseAggressorForBleedoutGreet(RE::Actor* player, RE::Actor* aggressor, float& outDistance)
		{
			outDistance = -1.0f;
			return IsReasonableBleedoutSpeaker(aggressor, player, 1800.0f, &outDistance);
		}

		static void PreparePlayerForBleedoutPleasureScene(const char* reason)
		{
			TFD::Bleedout::DefeatGlue::PreparePlayerForBleedoutPleasureScene(reason);
		}

		static void PreparePlayerForCaptivePleasureScene(const char* reason)
		{
			TFD::Bleedout::DefeatGlue::PreparePlayerForCaptivePleasureScene(reason);
		}

		static bool BeginResolvedNoMarkerFallback(const char* reason)
		{
			return TFD::Transition::DefeatGlue::BeginResolvedNoMarkerFallback(reason);
		}

		static void ApplyCalmBubble(float radius)
		{
			auto* player = Player();
			auto* primary = ResolveAggressor();
			if (!player || !primary) {
				return;
			}

			const float sweepRadius = (std::max)(radius, (std::max)(TFD::Settings::GetSweepRadius(), 12000.0f));
			TFD::HostilityController::StopCombatSweep(sweepRadius, true);
			TFD::HostilityController::ScheduleStopCombatWaves(sweepRadius, true, 10, 120);
			auto snapshot = TFD::Actor::BuildSnapshot(sweepRadius, false);
			auto* playerCell = player->GetParentCell();
			for (const auto& info : snapshot.actors) {
				auto* actor = info.get();
				if (!actor || actor == player || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
					continue;
				}
				if (playerCell && actor->GetParentCell() != playerCell) {
					continue;
				}
				if (actor != primary && !info.hostileToPlayer && !info.inCombat) {
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
		}

		static TFD::Bleedout::PendingSystemEventHandlers BuildBaseBleedPendingSystemEventHandlers()
		{
			return TFD::Bleedout::Builders::BuildPendingSystemEventHandlers();
		}

		static TFD::Bleedout::DialogueCloseHandlers BuildBleedDialogueCloseHandlers()
		{
			return TFD::Bleedout::Builders::BuildDialogueCloseHandlers();
		}

		static TFD::Bleedout::TimeoutHandlers BuildBleedTimeoutHandlers()
		{
			return TFD::Bleedout::Builders::BuildTimeoutHandlers();
		}

		static TFD::Bleedout::CompletionHandlers BuildBaseBleedCompletionHandlers()
		{
			return TFD::Bleedout::Builders::BuildCaptivePleasureCompletionHandlers();
		}

		static TFD::Bleedout::CompletionHandlers BuildCaptivePleasureCompletionHandlers()
		{
			return TFD::Bleedout::Builders::BuildCaptivePleasureCompletionHandlers();
		}

		static TFD::Bleedout::CompletionHandlers BuildPayReleaseCompletionHandlers()
		{
			return TFD::Bleedout::Builders::BuildPayReleaseCompletionHandlers();
		}

		static TFD::Bleedout::CompletionHandlers BuildBleedPleasureCompletionHandlers()
		{
			return TFD::Bleedout::Builders::BuildBleedPleasureCompletionHandlers();
		}

		static TFD::Bleedout::NonCaptiveChoiceHandlers BuildBleedNonCaptiveChoiceHandlers()
		{
			return TFD::Bleedout::Builders::BuildNonCaptiveChoiceHandlers();
		}

		static TFD::Bleedout::BlackoutHandlers BuildBleedBlackoutHandlers()
		{
			return TFD::Bleedout::Builders::BuildBlackoutHandlers();
		}

		static bool IsActorBleedingOut(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			if (auto* state = actor->AsActorState()) {
				return state->IsBleedingOut();
			}
			return false;
		}

		static float GetActorHealthPct(RE::Actor* actor)
		{
			if (!actor) {
				return 0.0f;
			}
			const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float hpNow = (std::max)(0.0f, actor->GetActorValue(RE::ActorValue::kHealth));
			return (hpNow / hpMax) * 100.0f;
		}

		static bool HasActiveBleedLock(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			return g_bleedLocks.find(actor->GetFormID()) != g_bleedLocks.end();
		}

		static bool IsActorDownByThreshold(RE::Actor* actor, float thresholdPct)
		{
			if (!actor || actor->IsDisabled() || actor->IsDead() || IsActorBleedingOut(actor)) {
				return true;
			}
			if (HasActiveBleedLock(actor)) {
				return true;
			}
			return GetActorHealthPct(actor) <= std::clamp(thresholdPct, 2.0f, 95.0f);
		}

		static bool IsStandingAllyThresholdActor(RE::Actor* actor)
		{
			if (!actor || actor == Player()) {
				return false;
			}
			return !IsActorDownByThreshold(actor, TFD::Settings::GetAllyDownedThresholdPct());
		}

		static bool IsStandingEnemyThresholdActor(RE::Actor* actor)
		{
			if (!actor || actor == Player()) {
				return false;
			}
			return !IsActorDownByThreshold(actor, TFD::Settings::GetEnemyDownedThresholdPct());
		}

		static void ApplyBleedDialogueOverdrive(RE::Actor* player, RE::Actor* speaker, const char* reason, bool restartDialogue)
		{
			TFD::Bleedout::ApplyDialogueOverdrive(player, speaker, reason, restartDialogue, BuildBleedRuntimeHostStateRefs(), BuildBleedRuntimeHostHandlers());
		}

		static bool PromoteNextBleedSpeakerFromTruceQueue(RE::Actor* player, const char* reason, bool rejectCurrent)
		{
			if (!player) {
				player = Player();
			}
			if (!player) {
				return false;
			}

			if (rejectCurrent && CurrentBleedSpeakerID() != 0) {
				AddBleedRejectedSpeakerID(CurrentBleedSpeakerID());
			}

			for (auto* actor : TFD::Actor::Ops::CollectTruceActors()) {
				if (!actor || actor == player || actor->IsDead() || actor->IsDisabled()) {
					continue;
				}
				const auto actorID = actor->GetFormID();
				if (HasBleedRejectedSpeakerID(actorID)) {
					continue;
				}
				if (!IsBleedCrowdSupportedAggressor(actor) || !IsBleedSpaceCompatible(actor, player)) {
					continue;
				}
				if (TFD::HostilityController::StartBleedTruceSessionForSpeaker(player, actor, reason ? reason : "truce_queue_promote")) {
					TFD::BleedoutGreet::ResetRuntime("bleed_reset");
					g_prevDialogueOpen = false;
					g_bleedPaused = false;
					g_bleedPauseStarted = {};
					ResetBleedSpeakerKickState();
					g_bleedLastSeconds = -1;
					g_bleedStart = Now();
					spdlog::info("[TFD][Defeat] promoted next bleed speaker actor={:08X} reason={}",
						actorID,
						reason ? reason : "unknown");
					return true;
				}
				AddBleedRejectedSpeakerID(actorID);
			}

			return false;
		}

		static void MaintainBleedPrimaryCaptorBinding()
		{
			TFD::Bleedout::MaintainPrimaryCaptorBinding(IsDialogueOpen(), BuildBleedRuntimeHostStateRefs());
		}



		// Temporary split module: defeated enemy registry / passive override cluster

		static bool IsActiveFollowerActor(RE::Actor* actor)
		{
			return TFD::TeammateManager::IsActiveFollowerActor(actor);
		}

		static RE::Actor* ResolveCurrentCombatTarget(RE::Actor* actor)
		{
			if (!actor) {
				return nullptr;
			}
			auto sp = actor->GetActorRuntimeData().currentCombatTarget.get();
			return sp.get();
		}

		static bool IsPlayerSideActorForRouter(RE::Actor* actor, RE::Actor* player)
		{
			if (!actor || !player) {
				return false;
			}
			if (actor == player) {
				return true;
			}
			return IsActiveFollowerActor(actor) || TFD::Tame::IsCompanion(actor);
		}

		static bool IsActorActivelyTargetingPlayerSideForRouter(RE::Actor* actor, RE::Actor* player)
		{
			if (!actor || !player) {
				return false;
			}
			auto* currentTarget = ResolveCurrentCombatTarget(actor);
			return IsPlayerSideActorForRouter(currentTarget, player);
		}

		static std::vector<RE::Actor*> CollectLiveStandingObservedEnemies(RE::Actor* player, float radius, RE::Actor* preferredEnemy, const std::vector<RE::Actor*>& allies);

		static bool RedirectOrClearEnemyPlayerTargetForDefeat(RE::Actor* enemy, RE::Actor* player, const char* reason)
		{
			if (!enemy || !player || enemy == player) {
				return false;
			}

			auto* target = ResolveCurrentCombatTarget(enemy);
			if (target != player) {
				return false;
			}

			RE::Actor* replacement = ResolveBleedRedirectTargetInternal(enemy);
			if (replacement && replacement != enemy && replacement != player && IsThresholdCombatTargetValid(replacement)) {
				enemy->GetActorRuntimeData().currentCombatTarget = replacement->GetHandle();
				if (!enemy->IsAIEnabled()) {
					enemy->EnableAI(true);
				}
				enemy->SetBeenAttacked(true);
				replacement->SetBeenAttacked(true);
				(void)enemy->RequestDetectionLevel(replacement, RE::DETECTION_PRIORITY::kCritical);
				(void)replacement->RequestDetectionLevel(enemy, RE::DETECTION_PRIORITY::kCritical);
				if (auto* process = RE::ProcessLists::GetSingleton()) {
					process->ClearCachedFactionFightReactions();
				}
				enemy->EvaluatePackage(false, true);
				enemy->EvaluatePackage(true, true);
				replacement->EvaluatePackage(false, true);
				replacement->EvaluatePackage(true, true);

				spdlog::info("[TFD][Defeat] redirected enemy target off player actor={:08X} new={:08X} reason={}",
					enemy->GetFormID(),
					replacement->GetFormID(),
					reason ? reason : "defeat_transition");
				return true;
			}

			enemy->GetActorRuntimeData().currentCombatTarget = RE::ActorHandle{};
			if (auto* process = RE::ProcessLists::GetSingleton()) {
				process->ClearCachedFactionFightReactions();
			}
			enemy->EvaluatePackage(false, true);
			enemy->EvaluatePackage(true, true);
			enemy->UpdateCombat();
			player->UpdateCombat();

			spdlog::info("[TFD][Defeat] cleared enemy target off player actor={:08X} reason={}",
				enemy->GetFormID(),
				reason ? reason : "defeat_transition");
			return true;
		}

		static void ClearEnemyTargetsToPlayerForDefeat(RE::Actor* player, float radius, const char* reason)
		{
			if (!player) {
				return;
			}

			std::vector<RE::Actor*> enemies;
			if (auto* pCell = player->GetParentCell()) {
				auto snapshot = TFD::Actor::BuildSnapshot(radius, false);
				enemies.reserve(snapshot.actors.size());
				for (const auto& info : snapshot.actors) {
					auto* actor = info.get();
					if (!actor || actor == player || !actor->Is3DLoaded()) {
						continue;
					}
					if (actor->GetParentCell() != pCell) {
						continue;
					}
					if (!IsStandingEnemyThresholdActor(actor)) {
						continue;
					}
					if (ResolveCurrentCombatTarget(actor) != player) {
						continue;
					}
					enemies.push_back(actor);
				}
			}

			if (enemies.empty()) {
				if (auto* fallbackEnemy = ResolveLastEnemyTargetingPlayerInternal(radius, 15.0); fallbackEnemy && ResolveCurrentCombatTarget(fallbackEnemy) == player) {
					enemies.push_back(fallbackEnemy);
				}
			}

			std::uint32_t changed = 0;
			for (auto* enemy : enemies) {
				if (RedirectOrClearEnemyPlayerTargetForDefeat(enemy, player, reason)) {
					++changed;
				}
			}

			if (changed > 0) {
				spdlog::info("[TFD][Defeat] player target detach pass changed={} reason={}",
					changed,
					reason ? reason : "defeat_transition");
			}
		}

		static float Distance3D(const RE::NiPoint3& a, const RE::NiPoint3& b)
		{
			const float dx = a.x - b.x;
			const float dy = a.y - b.y;
			const float dz = a.z - b.z;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		}

		struct FollowerResolution
		{
			RE::Actor* standing{ nullptr };
			RE::Actor* downed{ nullptr };
		};

		static FollowerResolution ResolveFollowerCandidates(float radius)
		{
			auto external = TFD::TeammateManager::ResolveFollowerCandidates(radius);
			FollowerResolution result{};
			result.standing = external.standing;
			result.downed = external.downed;
			return result;
		}

		static std::vector<RE::Actor*> CollectBleedStandingFollowers(float radius)
		{
			std::vector<RE::Actor*> out;
			std::unordered_set<RE::FormID> seen;

			auto snapshot = TFD::Actor::BuildSnapshot(radius, false);
			for (auto* actor : TFD::Actor::ResolveStandingPlayerSideActors(snapshot, false)) {
				if (!IsStandingAllyThresholdActor(actor)) {
					continue;
				}
				if (seen.insert(actor->GetFormID()).second) {
					out.push_back(actor);
				}
			}

			for (auto* actor : TFD::TeammateManager::CollectStandingFollowers(radius)) {
				if (!IsStandingAllyThresholdActor(actor)) {
					continue;
				}
				if (seen.insert(actor->GetFormID()).second) {
					out.push_back(actor);
				}
			}

			return out;
		}


		static void ResolveMonitorGlobals()
		{
			if (!g_defeatStateGlobal) {
				g_defeatStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDDefeatState");
				if (g_defeatStateGlobal && !g_loggedDefeatStateGlobal) {
					g_loggedDefeatStateGlobal = true;
					spdlog::info("[TFD][Defeat] TFDDefeatState resolved {:08X}", g_defeatStateGlobal->GetFormID());
				}
			}
			if (!g_hostileStateGlobal) {
				g_hostileStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDHostileState");
				if (g_hostileStateGlobal && !g_loggedHostileStateGlobal) {
					g_loggedHostileStateGlobal = true;
					spdlog::info("[TFD][Defeat] TFDHostileState resolved {:08X}", g_hostileStateGlobal->GetFormID());
				}
			}
			if (!g_enemyFactionStateGlobal) {
				g_enemyFactionStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDEnemyFactionState");
				if (g_enemyFactionStateGlobal && !g_loggedEnemyFactionStateGlobal) {
					g_loggedEnemyFactionStateGlobal = true;
					spdlog::info("[TFD][Defeat] TFDEnemyFactionState resolved {:08X}", g_enemyFactionStateGlobal->GetFormID());
				}
			}
			if (!g_enemyRaceStateGlobal) {
				g_enemyRaceStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDEnemyRaceState");
				if (g_enemyRaceStateGlobal && !g_loggedEnemyRaceStateGlobal) {
					g_loggedEnemyRaceStateGlobal = true;
					spdlog::info("[TFD][Defeat] TFDEnemyRaceState resolved {:08X}", g_enemyRaceStateGlobal->GetFormID());
				}
			}
			if (!g_recoveryStateGlobal) {
				g_recoveryStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDRecoveryState");
				if (g_recoveryStateGlobal && !g_loggedRecoveryStateGlobal) {
					g_loggedRecoveryStateGlobal = true;
					spdlog::info("[TFD][Defeat] TFDRecoveryState resolved {:08X}", g_recoveryStateGlobal->GetFormID());
				}
			}
			if (!g_leftForDeadStateGlobal) {
				g_leftForDeadStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDLeftForDeadState");
				if (g_leftForDeadStateGlobal && !g_loggedLeftForDeadStateGlobal) {
					g_loggedLeftForDeadStateGlobal = true;
					spdlog::info("[TFD][Defeat] TFDLeftForDeadState resolved {:08X}", g_leftForDeadStateGlobal->GetFormID());
				}
			}
		}

		static void QueueNonCaptiveChoiceRequest(const char* reason)
		{
			(void)TFD::Bleedout::EnterNonCaptiveChoice(reason ? reason : "queued_non_captive_choice", BuildBleedNonCaptiveChoiceHandlers());
		}

		static bool ComputePlayerBleedOutState(RE::Actor* player)
		{
			if (!player || player->IsDead() || player->IsDisabled()) {
				return false;
			}

			const float hpNow = player->GetActorValue(RE::ActorValue::kHealth);
			const float hpMax = (std::max)(1.0f, player->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float pct = (hpNow / hpMax) * 100.0f;
			const float thresh = TFD::Settings::GetDefeatThresholdPct();
			return pct <= thresh;
		}

		static void SetGlobalInt(RE::TESGlobal* global, int value)
		{
			if (global) {
				global->value = static_cast<float>(value);
			}
		}

		static std::vector<RE::Actor*> CollectLiveStandingObservedEnemies(RE::Actor* player, float radius, RE::Actor* preferredEnemy, const std::vector<RE::Actor*>& allies)
		{
			std::vector<RE::Actor*> out;
			auto enemies = TFD::Bleedout::DefeatGlue::CollectCurrentObservedEnemies(player, radius, preferredEnemy, allies);
			out.reserve(enemies.size());
			for (auto* enemy : enemies) {
				if (enemy && IsValidBleedBattleEnemyRosterActor(enemy, player)) {
					out.push_back(enemy);
				}
			}
			return out;
		}

		static bool IsVictoryCombatContextActive(RE::Actor* player, const std::vector<RE::Actor*>& enemies)
		{
			if (!player) {
				return false;
			}
			if (!enemies.empty() || player->IsInCombat()) {
				return true;
			}
			auto& flow = TFD::FlowController::Controller::GetSingleton();
			if (flow.IsCombatOrBleedRootActive()) {
				return true;
			}
			if (flow.IsCaptiveEscapeContextActive()) {
				return true;
			}
			return false;
		}

		static bool IsDefeatCombatContextActive(RE::Actor* player, const std::vector<RE::Actor*>& enemies)
		{
			if (IsVictoryCombatContextActive(player, enemies)) {
				return true;
			}
			if (g_inBleedState.load(std::memory_order_acquire)) {
				return true;
			}
			if (g_bleedBattleObservePending || g_bleedBattleObserveActive) {
				return true;
			}
			return false;
		}

		static bool IsRouterCombatContextActive(RE::Actor* player, const std::vector<RE::Actor*>& enemies)
		{
			if (!player) {
				return false;
			}

			for (auto* enemy : enemies) {
				if (IsActorActivelyTargetingPlayerSideForRouter(enemy, player)) {
					return true;
				}
			}

			return false;
		}
		static std::uint32_t ResolveRouterCombatActorFormID(RE::Actor* player, const std::vector<RE::Actor*>& enemies)
		{
			for (auto* enemy : enemies) {
				if (IsActorActivelyTargetingPlayerSideForRouter(enemy, player)) {
					return enemy->GetFormID();
				}
			}
			const auto flowActorFormID = ResolveBleedFlowActorFormID();
			if (flowActorFormID != 0) {
				return flowActorFormID;
			}
			return TFD::FlowController::Controller::GetSingleton().GetSnapshot().primaryActorFormID;
		}

		static int ComputeDefeatState(RE::Actor* player, bool combatContext)
		{
			if (!player || !combatContext) {
				return 0;
			}
			return ComputePlayerBleedOutState(player) ? 2 : 1;
		}

		static int ComputeVictoryState(RE::Actor* player, bool combatContext, const std::vector<RE::Actor*>& enemies)
		{
			if (!player) {
				g_victoryCombatContextUntil = {};
				return 0;
			}

			if (ComputePlayerBleedOutState(player)) {
				g_victoryCombatContextUntil = {};
				return 0;
			}

			if (combatContext) {
				g_victoryCombatContextUntil = Now() + std::chrono::milliseconds(kVictoryContextLingerMs);
			}
			if (!enemies.empty()) {
				return 1;
			}
			if (g_victoryCombatContextUntil != std::chrono::steady_clock::time_point{} && Now() < g_victoryCombatContextUntil) {
				return 2;
			}
			return 0;
		}

		static int ComputeHostileState(RE::Actor* player, const std::vector<RE::Actor*>& enemies)
		{
			if (!player || ComputePlayerBleedOutState(player)) {
				return 0;
			}

			const int count = static_cast<int>(enemies.size());
			if (count <= 0) {
				return 0;
			}
			if (count == 1) {
				return 1;
			}
			return 2;
		}

		static std::uint32_t ComputeEnemyRaceKey(RE::Actor* actor)
		{
			if (!actor) {
				return 0;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeNPC")) {
				return 1;
			}
			if (auto* race = actor->GetRace()) {
				return race->GetFormID();
			}
			return 0;
		}

		static int ComputeEnemyRaceState(const std::vector<RE::Actor*>& enemies)
		{
			if (enemies.empty()) {
				return 0;
			}
			if (enemies.size() == 1) {
				return 1;
			}
			std::uint32_t firstKey = 0;
			for (auto* enemy : enemies) {
				const auto key = ComputeEnemyRaceKey(enemy);
				if (key == 0) {
					continue;
				}
				if (firstKey == 0) {
					firstKey = key;
					continue;
				}
				if (key != firstKey) {
					return 2;
				}
			}
			return 1;
		}

		static int ComputeEnemyFactionState(const std::vector<RE::Actor*>& enemies)
		{
			if (enemies.empty()) {
				return 0;
			}
			if (enemies.size() == 1) {
				return 1;
			}
			for (std::size_t i = 0; i < enemies.size(); ++i) {
				auto* lhs = enemies[i];
				if (!lhs) {
					continue;
				}
				for (std::size_t j = i + 1; j < enemies.size(); ++j) {
					auto* rhs = enemies[j];
					if (!rhs) {
						continue;
					}
					if (lhs->IsHostileToActor(rhs) || rhs->IsHostileToActor(lhs)) {
						return 2;
					}
				}
			}
			return 1;
		}

		static int ComputeRecoveryState()
		{
			return TFD::Transition::HasRecoveryPotionAvailable() ? 1 : 0;
		}

		static int ComputeLeftForDeadState(RE::Actor* player)
		{
			(void)player;
			const float followerRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 400.0f);
			auto followers = ResolveFollowerCandidates(followerRadius);
			const bool hasLivingFollower = (followers.standing != nullptr);
			const bool hasRescueMarker = (TFD::Location::ResolveMostRecentCachedRescueDestination(true) != nullptr) ||
				(TFD::Location::ResolveMostRecentCachedRescueDestination(false) != nullptr);
			const bool hasRescueFactor = hasLivingFollower && hasRescueMarker;
			const bool hasRecoveryFactor = TFD::Transition::HasRecoveryPotionAvailable();
			return (hasRescueFactor || hasRecoveryFactor) ? 0 : 1;
		}

		static void SetRescueStateValue(int value)
		{
			TFD::Rescue::SetStateValue(value);
		}

		static void RefreshPostDefeatGlobals()
		{
			ResolveMonitorGlobals();
			auto* player = Player();
			const float radius = (std::max)(2200.0f, TFD::Settings::GetSweepRadius() + 200.0f);
			auto followers = CollectBleedStandingFollowers(radius);
			auto enemies = CollectLiveStandingObservedEnemies(player, radius, nullptr, followers);
			const bool defeatContext = IsDefeatCombatContextActive(player, enemies);
			const bool victoryContext = IsVictoryCombatContextActive(player, enemies);
			const bool routerCombatContext = IsRouterCombatContextActive(player, enemies);
			const auto routerCombatActorFormID = ResolveRouterCombatActorFormID(player, enemies);
			const bool pleasurePassiveLock = TFD::PleasureRuntime::IsPassiveLockActive();
			if (pleasurePassiveLock) {
				if (g_lastRouterCombatContextActive) {
					TFD::InCombat::ObserveCombat(0, false, "pleasure_passive_lock");
				}
				g_lastRouterCombatContextActive = false;
				SetGlobalInt(g_defeatStateGlobal, 0);
				TFD::Victory::SetStateValue(0);
				SetGlobalInt(g_hostileStateGlobal, 0);
				SetGlobalInt(g_enemyFactionStateGlobal, 0);
				SetGlobalInt(g_enemyRaceStateGlobal, 0);
				SetGlobalInt(g_recoveryStateGlobal, ComputeRecoveryState());
				SetGlobalInt(g_leftForDeadStateGlobal, 0);
				return;
			}
			TFD::InCombat::ObserveCombat(routerCombatActorFormID, routerCombatContext, "observed_combat_tick");
			g_lastRouterCombatContextActive = routerCombatContext;
			SetGlobalInt(g_defeatStateGlobal, ComputeDefeatState(player, defeatContext));
			TFD::Victory::SetStateValue(ComputeVictoryState(player, victoryContext, enemies));
			SetGlobalInt(g_hostileStateGlobal, ComputeHostileState(player, enemies));
			SetGlobalInt(g_enemyFactionStateGlobal, ComputeEnemyFactionState(enemies));
			SetGlobalInt(g_enemyRaceStateGlobal, ComputeEnemyRaceState(enemies));
			SetGlobalInt(g_recoveryStateGlobal, ComputeRecoveryState());
			SetGlobalInt(g_leftForDeadStateGlobal, ComputeLeftForDeadState(player));
		}

		static void UpdatePreCombatState()
		{
			// ownership moved to TFDFlowController.cpp
		}

		static bool IsDialogueOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
		}

		static RE::Actor* ResolveAggressor();
		static RE::Actor* FindBestAggressor(float radius);

		static TFD::Captive::EscapeTickHandlers BuildCaptiveEscapeTickHandlers()
		{
			TFD::Captive::EscapeTickHandlers handlers{};
			handlers.updatePreCombatState = []() { UpdatePreCombatState(); };
			handlers.setPrevDialogueOpen = [](bool open) { g_prevDialogueOpen = open; };
			handlers.clearLastAggressor = []() { g_lastAggressor.reset(); };
			handlers.setLastAggressor = [](RE::Actor* actor) { g_lastAggressor = actor ? actor->GetHandle() : RE::ActorHandle{}; };
			handlers.resolveAggressor = []() -> RE::Actor* { return ResolveAggressor(); };
			handlers.findBestAggressor = [](float radius) -> RE::Actor* { return FindBestAggressor(radius); };
			handlers.setGraceActive = [](bool active) { g_grace.store(active, std::memory_order_release); };
			return handlers;
		}

		static void ClampHealth(RE::Actor* actor, float minHp)
		{
			if (!actor) return;
			const float hp = actor->GetActorValue(RE::ActorValue::kHealth);
			if (hp < minHp) {
				actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, (minHp - hp));
			}
		}

		static void ClampHealthCeiling(RE::Actor* actor, float maxHp)
		{
			if (!actor) {
				return;
			}
			const float hp = actor->GetActorValue(RE::ActorValue::kHealth);
			if (hp > maxHp + 0.001f) {
				actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, -(hp - maxHp));
			}
		}

		static float ResolveActorHealthForPct(RE::Actor* actor, float pct)
		{
			if (!actor) {
				return 0.0f;
			}
			const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
			return hpMax * std::clamp(pct / 100.0f, 0.0f, 1.0f);
		}

		static void ForcePlayerBleedAlive(RE::Actor* actor, BleedLockEntry& entry, const char* reason)
		{
			if (!actor) {
				return;
			}

			SetPlayerBleedImmune(true);
			auto& boolFlags = actor->GetActorRuntimeData().boolFlags;
			const bool wasDead = actor->IsDead(false);
			const bool wasKillMove = boolFlags.all(RE::Actor::BOOL_FLAGS::kIsInKillMove);

			if (wasDead) {
				actor->Resurrect(false, true);
			}

			boolFlags.set(RE::Actor::BOOL_FLAGS::kEssential);
			boolFlags.set(RE::Actor::BOOL_FLAGS::kProtected);
			boolFlags.set(RE::Actor::BOOL_FLAGS::kCanSpeakToEssentialDown);
			boolFlags.set(RE::Actor::BOOL_FLAGS::kNoBleedoutRecovery);
			boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);

			const float hardFloorHp = (std::max)(1.0f, entry.minHp);
			ClampHealth(actor, hardFloorHp);
			float hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
			if (hpNow + 0.001f < hardFloorHp) {
				actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, hardFloorHp - hpNow);
				hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
			}
			entry.protectedHealth = (std::max)(entry.protectedHealth, hpNow);
			entry.lastHealthSample = hpNow;

			actor->NotifyAnimationGraph("BleedoutStart");
			actor->EvaluatePackage(false, true);
			actor->EvaluatePackage(true, true);

			spdlog::warn("[TFD][Defeat] player bleed anti-death reassert actor={:08X} reason={} resurrected={} killmove={}",
				actor->GetFormID(),
				reason ? reason : "unknown",
				wasDead ? 1 : 0,
				wasKillMove ? 1 : 0);
		}

		static void EnforcePlayerBleedInvulnerability(RE::Actor* actor, BleedLockEntry& entry)
		{
			if (!actor) {
				return;
			}

			if (actor->IsDead(false) || actor->GetActorRuntimeData().boolFlags.all(RE::Actor::BOOL_FLAGS::kIsInKillMove)) {
				ForcePlayerBleedAlive(actor, entry, actor->IsDead(false) ? "dead_state" : "killmove_state");
			}

			SetPlayerBleedImmune(true);

			const float hardFloorHp = (std::max)(1.0f, entry.minHp);
			const float protectedHp = (std::max)(entry.protectedHealth, hardFloorHp);
			ClampHealth(actor, protectedHp);

			float hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
			if (hpNow + 0.001f < protectedHp) {
				const float delta = protectedHp - hpNow;
				actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, delta);
				const auto now = Now();
				if (entry.lastDamageLog.time_since_epoch().count() == 0 || (now - entry.lastDamageLog) >= std::chrono::milliseconds(150)) {
					spdlog::info("[TFD][Defeat] player bleed zeroed health damage actor={:08X} from={:.2f} restore={:.2f} target={:.2f}",
						actor->GetFormID(),
						hpNow,
						delta,
						protectedHp);
					entry.lastDamageLog = now;
				}
				hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
			}

			if (hpNow > entry.protectedHealth + 0.001f) {
				entry.protectedHealth = hpNow;
			}

			entry.lastHealthSample = hpNow;
		}

		static void ApplyBleedRegenOverride(RE::Actor* actor, BleedLockEntry& entry)
		{
			auto* avo = actor ? actor->AsActorValueOwner() : nullptr;
			if (!avo) {
				return;
			}

			if (!entry.regenOverridden) {
				entry.savedHealRate = avo->GetActorValue(RE::ActorValue::kHealRate);
				entry.savedHealRateMult = avo->GetActorValue(RE::ActorValue::kHealRateMult);
				entry.savedCombatHealRateMult = avo->GetActorValue(RE::ActorValue::kCombatHealthRegenMultiply);
				entry.regenOverridden = true;
			}

			avo->SetActorValue(RE::ActorValue::kHealRate, 0.0f);
			avo->SetActorValue(RE::ActorValue::kHealRateMult, 0.0f);
			avo->SetActorValue(RE::ActorValue::kCombatHealthRegenMultiply, 0.0f);
		}

		static void RestoreBleedRegenOverride(RE::Actor* actor, const BleedLockEntry& entry)
		{
			if (!entry.regenOverridden) {
				return;
			}

			auto* avo = actor ? actor->AsActorValueOwner() : nullptr;
			if (!avo) {
				return;
			}

			avo->SetActorValue(RE::ActorValue::kHealRate, entry.savedHealRate);
			avo->SetActorValue(RE::ActorValue::kHealRateMult, entry.savedHealRateMult);
			avo->SetActorValue(RE::ActorValue::kCombatHealthRegenMultiply, entry.savedCombatHealRateMult);
		}

		static void RestoreActorHealthToSafePct(RE::Actor* actor, float thresholdPct, float bonusPct, float minSafePct, float maxSafePct, float minAbsHp, const char* reason)
		{
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				return;
			}
			const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
			const float thresh = std::clamp(thresholdPct / 100.0f, 0.05f, 0.95f);
			const float safePct = std::clamp(thresh + bonusPct, minSafePct, maxSafePct);
			const float target = (std::max)(minAbsHp, hpMax * safePct);
			if (hpNow + 0.001f < target) {
				actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, (target - hpNow));
				spdlog::info("[TFD][Defeat] recover actor hp actor={:08X} reason={} from={:.2f} to={:.2f} threshPct={:.1f}",
					actor->GetFormID(),
					reason ? reason : "unknown",
					hpNow,
					target,
					thresholdPct);
			}
		}

		static void RecoverVictoryTeammates()
		{
			TFD::TeammateManager::RecoverVictoryTeammates();
		}

		static float ResolveBleedLockThresholdPct(RE::Actor* actor)
		{
			if (!actor) {
				return 0.0f;
			}
			if (actor == Player()) {
				return TFD::Settings::GetDefeatThresholdPct();
			}
			if (IsActiveFollowerActor(actor)) {
				return TFD::Settings::GetAllyDownedThresholdPct();
			}
			return TFD::Settings::GetEnemyDownedThresholdPct();
		}

		static BleedLockKind ResolveBleedLockKind(RE::Actor* actor)
		{
			if (!actor) {
				return BleedLockKind::Other;
			}
			if (actor == Player()) {
				return BleedLockKind::Player;
			}
			if (IsActiveFollowerActor(actor)) {
				return BleedLockKind::Ally;
			}
			return BleedLockKind::Other;
		}

		static bool ShouldEnterBleedLock(RE::Actor* actor, float thresholdPct)
		{
			if (!actor || actor->IsDisabled() || actor->IsDead()) {
				return false;
			}
			return GetActorHealthPct(actor) <= std::clamp(thresholdPct, 2.0f, 95.0f);
		}

		static void EnterBleedLock(RE::Actor* actor, BleedLockKind kind, float thresholdPct, const char* reason)
		{
			if (!actor || actor->IsDisabled() || actor->IsDead()) {
				return;
			}

			const auto formID = actor->GetFormID();
			auto& entry = g_bleedLocks[formID];
			const bool wasNew = !entry.handle;
			const bool wasDefeatedManaged = entry.defeatedManaged;
			entry.handle = actor->GetHandle();
			entry.kind = kind;
			entry.thresholdPct = std::clamp(thresholdPct, 2.0f, 95.0f);
			entry.lastHealthSample = actor->GetActorValue(RE::ActorValue::kHealth);
			if (kind == BleedLockKind::Player) {
				const float maxHp = actor->GetPermanentActorValue(RE::ActorValue::kHealth);
				entry.minHp = (std::max)(1.0f, maxHp * 0.02f);
				g_minHp = (std::max)(g_minHp, entry.minHp);
				SetPlayerBleedImmune(true);
				ClampHealth(actor, entry.minHp);
				const float zeroDamageAnchor = (std::max)(entry.minHp, ResolveActorHealthForPct(actor, (std::max)(entry.thresholdPct, 5.0f)));
				entry.protectedHealth = (std::max)(zeroDamageAnchor, actor->GetActorValue(RE::ActorValue::kHealth));
				entry.lastHealthSample = entry.protectedHealth;
				ClampHealth(actor, entry.protectedHealth);
			}

			ApplyBleedRegenOverride(actor, entry);
			if (kind == BleedLockKind::Other && TFD::Actor::Ops::IsDefeatedEnemyCandidate(actor)) {
				entry.defeatedManaged = true;
				if (!wasDefeatedManaged) {
					entry.defeatedDeadline = Now() + std::chrono::milliseconds(static_cast<int>(kDefeatedEnemyKnockSeconds * 1000.0));
					entry.defeatedAutoDeathIssued = false;
					entry.defeatedFatalDamageApplied = false;
				}
				TFD::Actor::Ops::ApplyDefeatedEnemyPassiveOverride(actor, entry.savedAggression, entry.aggressionOverridden);
				TFD::Actor::Ops::SyncDefeatedEnemyMirror(actor, entry.defeatedAliasSlot, entry.defeatedFactionApplied);
			}

			if (!wasNew) {
				return;
			}

			if (!actor->IsDead()) {
				if (kind != BleedLockKind::Player) {
					if (actor->IsInCombat()) {
						actor->StopCombat();
					}
					if (auto* process = RE::ProcessLists::GetSingleton()) {
						process->StopCombatAndAlarmOnActor(actor, false);
					}
					if (actor->IsWeaponDrawn()) {
						actor->DrawWeaponMagicHands(false);
					}
				}
				actor->NotifyAnimationGraph("BleedoutStart");
				actor->EvaluatePackage(false, true);
				actor->EvaluatePackage(true, true);
				entry.lastPulse = Now();
			}

			if (wasNew) {
				spdlog::info("[TFD][Defeat] bleed lock enter actor={:08X} kind={} threshold={:.1f} reason={}",
					formID,
					static_cast<int>(kind),
					entry.thresholdPct,
					reason ? reason : "unknown");
			}
		}

		static void ReleaseBleedLock(RE::Actor* actor, const char* reason, bool playGetUp)
		{
			if (!actor) {
				return;
			}
			const auto formID = actor->GetFormID();
			auto it = g_bleedLocks.find(formID);
			if (it == g_bleedLocks.end()) {
				return;
			}
			auto entry = it->second;
			const auto kind = entry.kind;
			g_bleedLocks.erase(it);
			RestoreBleedRegenOverride(actor, entry);
			if (entry.defeatedManaged) {
				if (actor && reason && std::strstr(reason, "recruit")) {
					TFD::Actor::Ops::SuppressDefeatedEnemyReentry(actor, kDefeatedReentrySuppressSeconds, reason);
				}
				TFD::Actor::Ops::ClearDefeatedEnemyState(actor, entry.defeatedAliasSlot, entry.defeatedFactionApplied, entry.defeatedManaged, entry.defeatedAutoDeathIssued, entry.defeatedFatalDamageApplied, entry.defeatedDeadline, entry.savedAggression, entry.aggressionOverridden, reason);
			}
			if (kind == BleedLockKind::Player) {
				g_minHp = 0.0f;
				SetPlayerBleedImmune(false);
			}
			if (playGetUp && actor && !actor->IsDead() && !actor->IsDisabled()) {
				actor->NotifyAnimationGraph("BleedoutStop");
				actor->NotifyAnimationGraph("GetUpStart");
				actor->EvaluatePackage(false, true);
				actor->EvaluatePackage(true, true);
			}
			spdlog::info("[TFD][Defeat] bleed lock release actor={:08X} reason={} getUp={}",
				formID,
				reason ? reason : "unknown",
				playGetUp ? 1 : 0);
		}

		static void ReleasePlayerBleedLock(const char* reason, bool playGetUp)
		{
			ReleaseBleedLock(Player(), reason, playGetUp);
		}

		static void ClearAllBleedLocks(const char* reason)
		{
			for (auto& [formID, entry] : g_bleedLocks) {
				auto sp = entry.handle.get();
				auto* actor = sp.get();
				RestoreBleedRegenOverride(actor, entry);
				if (entry.defeatedManaged) {
					TFD::Actor::Ops::ClearDefeatedEnemyState(actor, entry.defeatedAliasSlot, entry.defeatedFactionApplied, entry.defeatedManaged, entry.defeatedAutoDeathIssued, entry.defeatedFatalDamageApplied, entry.defeatedDeadline, entry.savedAggression, entry.aggressionOverridden, reason);
				}
				if (entry.kind == BleedLockKind::Player) {
					g_minHp = 0.0f;
					SetPlayerBleedImmune(false);
				}
				if (actor && !actor->IsDead() && !actor->IsDisabled()) {
					actor->EvaluatePackage(false, true);
					actor->EvaluatePackage(true, true);
				}
				spdlog::info("[TFD][Defeat] bleed lock clear actor={:08X} reason={}", formID, reason ? reason : "unknown");
			}
			g_bleedLocks.clear();
			g_bleedLockLastScan = {};
			TFD::Actor::Ops::ClearAllDefeatedEnemyMirrors(reason);
		}

		static void ScanBleedLockCandidates()
		{
			auto* player = Player();
			if (!player) {
				return;
			}

			const float playerThreshold = ResolveBleedLockThresholdPct(player);
			if (ShouldEnterBleedLock(player, playerThreshold)) {
				EnterBleedLock(player, BleedLockKind::Player, playerThreshold, "threshold_scan_player");
			}

			const float radius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 600.0f);
			for (auto* actor : TFD::TeammateManager::CollectKnownTeammates(radius)) {
				const float threshold = ResolveBleedLockThresholdPct(actor);
				if (ShouldEnterBleedLock(actor, threshold)) {
					EnterBleedLock(actor, ResolveBleedLockKind(actor), threshold, "threshold_scan_follower");
				}
			}

			auto snapshot = TFD::Actor::BuildSnapshot(radius, false);
			for (const auto& info : snapshot.actors) {
				auto* actor = info.get();
				if (!actor || actor == player || actor->IsDisabled() || actor->IsDead()) {
					continue;
				}
				if (IsActiveFollowerActor(actor)) {
					continue;
				}
				const float threshold = ResolveBleedLockThresholdPct(actor);
				if (ShouldEnterBleedLock(actor, threshold)) {
					EnterBleedLock(actor, ResolveBleedLockKind(actor), threshold, "threshold_scan_other");
				}
			}
		}

		static void TickBleedLocks()
		{
			const auto now = Now();
			if (g_bleedLockLastScan.time_since_epoch().count() == 0 || (now - g_bleedLockLastScan) >= std::chrono::milliseconds(250)) {
				g_bleedLockLastScan = now;
				ScanBleedLockCandidates();
			}

			std::vector<std::tuple<RE::FormID, RE::Actor*, const char*, bool>> releases;
			for (auto& [formID, entry] : g_bleedLocks) {
				auto sp = entry.handle.get();
				auto* actor = sp.get();
				if (!actor || actor->IsDisabled()) {
					releases.emplace_back(formID, actor, "invalid", false);
					continue;
				}

				if (entry.kind != BleedLockKind::Player && actor->IsDead()) {
					releases.emplace_back(formID, actor, "dead", false);
					continue;
				}

				ApplyBleedRegenOverride(actor, entry);

				if (entry.kind == BleedLockKind::Player) {
					EnforcePlayerBleedInvulnerability(actor, entry);
					if (actor->IsDead(false)) {
						ForcePlayerBleedAlive(actor, entry, "tick_dead_state");
						continue;
					}
				}
				else if (entry.kind == BleedLockKind::Ally) {
					const float holdCeilingHp = (std::max)(1.0f, ResolveActorHealthForPct(actor, (std::max)(2.0f, entry.thresholdPct - 0.5f)));
					const float hpBeforeClamp = actor->GetActorValue(RE::ActorValue::kHealth);

					ClampHealthCeiling(actor, holdCeilingHp);

					const float hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
					if (hpNow > holdCeilingHp + 0.001f) {
						spdlog::info("[TFD][Defeat] ally bleed clamp actor={:08X} from={:.2f} to={:.2f} thresholdPct={:.1f}",
							actor->GetFormID(),
							hpBeforeClamp,
							holdCeilingHp,
							entry.thresholdPct);
					}

					entry.healAccumulator = 0.0f;
					entry.lastHealGain = {};

					if (actor->IsInCombat()) {
						actor->StopCombat();
					}
					if (auto* process = RE::ProcessLists::GetSingleton()) {
						process->StopCombatAndAlarmOnActor(actor, false);
					}
					if (actor->IsWeaponDrawn()) {
						actor->DrawWeaponMagicHands(false);
					}
					actor->EvaluatePackage(false, true);
					actor->EvaluatePackage(true, true);

					entry.lastHealthSample = hpNow;
				}
				else {
					if (!entry.defeatedManaged && GetActorHealthPct(actor) > (entry.thresholdPct + 8.0f)) {
						releases.emplace_back(formID, actor, "recovered", true);
						continue;
					}
					if (entry.defeatedManaged) {
						TFD::Actor::Ops::ApplyDefeatedEnemyPassiveOverride(actor, entry.savedAggression, entry.aggressionOverridden);
						TFD::Actor::Ops::SyncDefeatedEnemyMirror(actor, entry.defeatedAliasSlot, entry.defeatedFactionApplied);
					}
					if (actor->IsInCombat()) {
						actor->StopCombat();
					}
					if (auto* process = RE::ProcessLists::GetSingleton()) {
						process->StopCombatAndAlarmOnActor(actor, false);
					}
					if (actor->IsWeaponDrawn()) {
						actor->DrawWeaponMagicHands(false);
					}
					if (entry.defeatedManaged && !IsDialogueOpen() && entry.defeatedDeadline.time_since_epoch().count() != 0 && now >= entry.defeatedDeadline) {
						if (!entry.defeatedAutoDeathIssued) {
							if (actor->IsEssential() || actor->IsProtected()) {
								spdlog::warn("[TFD][Defeat] defeated enemy auto-death skipped actor={:08X} reason=protected_or_essential", actor->GetFormID());
								releases.emplace_back(formID, actor, "timeout_skip_kill", true);
								continue;
							}
							entry.defeatedAutoDeathIssued = true;
							entry.defeatedFatalDamageApplied = false;
							entry.defeatedDeadline = now + std::chrono::milliseconds(450);
							entry.lastPulse = now;
							actor->NotifyAnimationGraph("BleedoutStop");
							actor->EvaluatePackage(false, true);
							actor->EvaluatePackage(true, true);
							spdlog::info("[TFD][Defeat] defeated enemy auto-death queued actor={:08X}", actor->GetFormID());
							continue;
						}

						if (!entry.defeatedFatalDamageApplied) {
							if (actor->IsDead()) {
								spdlog::info("[TFD][Defeat] defeated enemy auto-death confirmed actor={:08X}", actor->GetFormID());
								releases.emplace_back(formID, actor, "timeout_dead", false);
								continue;
							}
							const float hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
							const float fatalDamage = (std::max)(25.0f, hpNow + 5000.0f);
							actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, -fatalDamage);
							entry.defeatedFatalDamageApplied = true;
							entry.defeatedDeadline = now + std::chrono::milliseconds(1500);
							spdlog::info("[TFD][Defeat] defeated enemy auto-death damage actor={:08X} hpBefore={:.2f} damage={:.2f}",
								actor->GetFormID(),
								hpNow,
								fatalDamage);
							continue;
						}

						if (actor->IsDead()) {
							spdlog::info("[TFD][Defeat] defeated enemy auto-death confirmed actor={:08X}", actor->GetFormID());
							releases.emplace_back(formID, actor, "timeout_dead", false);
							continue;
						}

						if ((now - entry.defeatedDeadline) >= std::chrono::milliseconds(1500)) {
							spdlog::warn("[TFD][Defeat] defeated enemy auto-death fallback kill actor={:08X}", actor->GetFormID());
							actor->KillImmediate();
							releases.emplace_back(formID, actor, "timeout_dead_fallback", false);
							continue;
						}
					}
				}

				const bool suppressBleedPulse = entry.defeatedManaged && entry.defeatedAutoDeathIssued;
				const auto pulseInterval = entry.kind == BleedLockKind::Ally ? std::chrono::milliseconds(250) : std::chrono::milliseconds(250);
				const bool pulseDue = entry.lastPulse.time_since_epoch().count() == 0 || (now - entry.lastPulse) >= pulseInterval;
				const bool shouldPulse = [&]() {
					if (suppressBleedPulse) {
						return false;
					}
					if (entry.kind == BleedLockKind::Ally) {
						return pulseDue;
					}
					return !IsActorBleedingOut(actor) || pulseDue;
					}();
				if (shouldPulse) {
					actor->NotifyAnimationGraph("BleedoutStart");
					if (entry.kind == BleedLockKind::Ally) {
						actor->EvaluatePackage(false, true);
						actor->EvaluatePackage(true, true);
					}
					entry.lastPulse = now;
				}
			}

			for (auto& [formID, actor, reason, playGetUp] : releases) {
				if (actor) {
					ReleaseBleedLock(actor, reason, playGetUp);
				}
				else {
					auto it = g_bleedLocks.find(formID);
					if (it != g_bleedLocks.end()) {
						if (it->second.kind == BleedLockKind::Player) {
							g_minHp = 0.0f;
							SetPlayerBleedImmune(false);
						}
						spdlog::info("[TFD][Defeat] bleed lock release actor={:08X} reason={} getUp=0", formID, reason ? reason : "unknown");
						g_bleedLocks.erase(it);
					}
				}
			}
		}


		// Temporary split module: bleed dialogue/runtime orchestration

		static void TickUI()
		{
			struct Guard {
				~Guard() { g_tickPending.clear(std::memory_order_release); }
			} guard;
			if (!TFD::Settings::GetEnabled()) return;
			if (g_loadTransition.load(std::memory_order_acquire)) return;
			auto* ui = RE::UI::GetSingleton();
			TFD::Transition::PollResult();
			TFD::Transition::ProcessPendingFadeIn();
			TFD::Captive::ProcessPendingConfiscation();
			if (TFD::Transition::IsAwaiting() || TFD::Transition::HasPendingFadeIn()) {
				TFD::Transition::MaintainCalmWindow(BuildTransitionRuntimeHandlers());
				UpdatePreCombatState();
				return;
			}
			RefreshPostDefeatGlobals();
			(void)TFD::Captive::NormalizeInvalidCaptivePair();
			const bool captiveBleedOverlay = TFD::Captive::HasEscapeBreakRebleedPending() || g_inBleedState.load(std::memory_order_acquire);
			const bool inCombatDialogueOpen = IsDialogueOpen();
			if (!captiveBleedOverlay && TFD::InCombat::IsActive() && inCombatDialogueOpen) {
				TFD::InCombatGreet::NotifyDialogueOpened();
			}
			if (!captiveBleedOverlay && TFD::Rescue::IsActive() && inCombatDialogueOpen) {
				TFD::RescueGreet::NotifyDialogueOpened();
			}
			if (TFD::Captive::IsStandardCaptiveActive()) {
				const bool dialogOpen = IsDialogueOpen();
				if (dialogOpen) {
					TFD::CaptiveGreet::NotifyDialogueOpened();
				}
				else if (g_prevDialogueOpen && TFD::CaptiveGreet::IsActive()) {
					TFD::CaptiveGreet::Cancel("dialogue_closed");
				}
				if (!dialogOpen && g_prevDialogueOpen) {
					spdlog::info("[TFD][Captive] Dialogue closed -> no implicit action");
				}
				g_prevDialogueOpen = dialogOpen;
				if (!captiveBleedOverlay) {
					(void)TFD::Captive::TickCaptiveEscapePhase(Player(), BuildCaptiveEscapeTickHandlers());
				}
			}
			else if (TFD::Captive::IsEscapeActive()) {
				if (!TFD::Captive::TickEscapeActivePhase(Player(), BuildCaptiveEscapeTickHandlers())) {
					return;
				}
			}

			if (TFD::Captive::IsActive() && !captiveBleedOverlay) {
				return;
			}

			if (ui && ui->GameIsPaused()) return;
			auto* player = Player();
			if (!player) {
				RefreshPostDefeatGlobals();
				return;
			}
			TFD::InteractionRouter::DialogueOpen::Tick();
			TFD::PleasureRuntime::Tick();
			TFD::Location::UpdateAmbientKidnapAvailability(false);
			TFD::Actor::Ops::MaintainReleaseFollowGrace();
			if (HandlePendingEscapeBreakBleed()) {
				return;
			}
			TickBleedLocks();
			UpdatePreCombatState();
			if (TFD::Transition::IsLeftForDeadCooldownActive(BuildTransitionRuntimeHandlers())) {
				TFD::Transition::TickLeftForDeadCooldown(BuildTransitionRuntimeHandlers());
				return;
			}
			if (IsGraceActive()) return;
			if (!captiveBleedOverlay && !g_inBleedState.load(std::memory_order_acquire) && TFD::InCombat::IsActive()) {
				const auto nowInCombat = Now();
				if (TFD::InCombatGreet::TryStickyWatchdog(g_prevDialogueOpen, inCombatDialogueOpen, TFD::PleasureRuntime::IsBlocking(), TFD::InCombat::GetPrimaryActorFormID(), nowInCombat,
					[&](const char* reopenReason) -> bool {
						auto* reopenSpeaker = RE::TESForm::LookupByID<RE::Actor>(TFD::InCombat::GetPrimaryActorFormID());
						if (!reopenSpeaker) {
							return false;
						}
						return TFD::InCombatGreet::Begin(reopenSpeaker, reopenReason);
					})) {
					return;
				}

				if (TFD::InCombatGreet::HandleDialogueClosedFlow(
					TFD::InCombatGreet::DialogueClosedContext{
						TFD::InCombatGreet::HasSeenDialogue(),
						g_prevDialogueOpen,
						TFD::PleasureRuntime::IsBlocking(),
						TFD::InCombat::GetState() == TFD::InCombat::State::AfterPleasure
					},
					player,
					TFD::InCombat::GetPrimaryActorFormID(),
					TFD::InCombatGreet::DialogueClosedHandlers{
						[&]() {
							g_prevDialogueOpen = false;
							spdlog::info("[TFD][Defeat] incombat dialogue closed -> hold after pleasure/runtime");
						},
						[&]() {
							g_prevDialogueOpen = false;
							(void)TFD::InCombat::CompleteDialogueClosedFlow(
								"dialogue_closed_complete",
								TFD::InCombat::CompletionHandlers{
									[&](const char* r) { TFD::InCombat::ClearDialogueOutcome(r); },
									[&](const char* r) { TFD::InCombat::Complete(r); },
									[&](const char* r) { TFD::InCombatGreet::CancelAll(r); }
								});
							spdlog::info("[TFD][Defeat] incombat dialogue closed -> complete");
						},
						[&](const TFD::InCombatGreet::StickyReopenProbe& probe) {
							g_prevDialogueOpen = false;
							auto* reopenSpeaker = probe.speakerFormID != 0 ? RE::TESForm::LookupByID<RE::Actor>(probe.speakerFormID) : nullptr;
							if (reopenSpeaker) {
								(void)TFD::InCombatGreet::Begin(reopenSpeaker, "dialogue_closed_sticky_reopen");
							}
							spdlog::info("[TFD][Defeat] incombat dialogue closed -> sticky reopen speaker={:08X} dist={:.1f}", probe.speakerFormID, probe.distance);
						},
						[&](const TFD::InCombatGreet::StickyReopenProbe& probe) {
							g_prevDialogueOpen = false;
							(void)TFD::InCombat::CompleteDialogueClosedFlow(
								"dialogue_closed_no_sticky_reopen",
								TFD::InCombat::CompletionHandlers{
									[&](const char* r) { TFD::InCombat::ClearDialogueOutcome(r); },
									[&](const char* r) { TFD::InCombat::Complete(r); },
									[&](const char* r) { TFD::InCombatGreet::CancelAll(r); }
								});
							spdlog::info("[TFD][Defeat] incombat dialogue closed -> complete no sticky reopen speaker={:08X} loaded={} dead={} dist={:.1f}",
								probe.speakerFormID,
								probe.loaded ? 1 : 0,
								probe.dead ? 1 : 0,
								probe.distance);
						}
					})) {
					return;
				}
			}

			if (g_inBleedState.load(std::memory_order_acquire)) {
				if (g_bleedBattleObservePending) {
					TickBleedBattleObservePending();
					return;
				}
				if (g_bleedBattleObserveActive) {
					TickBleedBattleObserve();
					return;
				}
				if (g_minHp > 0.0f) {
					SetPlayerBleedImmune(true);
					ClampHealth(player, g_minHp);
				}
				MaintainBleedPrimaryCaptorBinding();
				MaintainBleedSpeakerKick();
				const bool dOpen = IsDialogueOpen();
				const int bleedSeconds = TFD::Settings::GetBleedWindowSeconds();
				const bool pleasureCommitted = TFD::Bleedout::GetDialogueOutcome() == BleedDialogueOutcome::Pleasure;
				const bool ostimBridgeBlocking = TFD::PleasureRuntime::IsBlocking();
				auto holdDecision = TFD::BleedoutGreet::EvaluateHold(dOpen, pleasureCommitted, ostimBridgeBlocking);
				if (holdDecision.hold) {
					const auto nowBleedHold = Now();
					if (dOpen) {
						TFD::BleedoutGreet::NotifyDialogueOpened();
					}
					else if (TFD::BleedoutGreet::TryAfterPleasureWatchdog(g_prevDialogueOpen, dOpen, CurrentBleedSpeakerID(), nowBleedHold,
						[&](const char* reopenReason) -> bool {
							auto* reopenSpeaker = CurrentBleedSpeaker();
							float reopenDist = 99999.0f;
							if (!(player && reopenSpeaker && CanUseAggressorForBleedoutGreet(player, reopenSpeaker, reopenDist))) {
								return false;
							}
							ApplyBleedDialogueOverdrive(player, reopenSpeaker, reopenReason, true);
							g_bleedPaused = true;
							g_bleedPauseStarted = nowBleedHold;
							g_bleedLastSeconds = -1;
							spdlog::info("[TFD][Defeat] bleed after pleasure watchdog reopen speaker={:08X} dist={:.1f}",
								reopenSpeaker->GetFormID(),
								reopenDist);
							return true;
						})) {
						g_prevDialogueOpen = dOpen;
						return;
					}
					if (!g_bleedPaused) {
						g_bleedPaused = true;
						g_bleedPauseStarted = nowBleedHold;
						spdlog::info("[TFD][Defeat] bleed countdown paused by {}", TFD::BleedoutGreet::GetHoldReasonName(holdDecision.reason));
					}
					g_prevDialogueOpen = dOpen;
					return;
				}
				if (g_bleedPaused && TFD::Bleedout::GetDialogueOutcome() == BleedDialogueOutcome::None) {
					const auto nowBleed = Now();
					if (TFD::BleedoutGreet::TryTimeoutRearm(g_prevDialogueOpen, TFD::Bleedout::HasTerminalCommit(), TFD::PleasureRuntime::IsBlocking(), CurrentBleedSpeakerID(), nowBleed,
						[&](const char* reopenReason) -> bool {
							auto* timeoutSpeaker = CurrentBleedSpeaker();
							float timeoutDist = 99999.0f;
							if (!(player && timeoutSpeaker && CanUseAggressorForBleedoutGreet(player, timeoutSpeaker, timeoutDist))) {
								return false;
							}
							ApplyBleedDialogueOverdrive(player, timeoutSpeaker, reopenReason, true);
							g_bleedPauseStarted = nowBleed;
							g_bleedLastSeconds = -1;
							ResetBleedSpeakerKickState();
							spdlog::info("[TFD][Defeat] bleed dialogue overdrive timeout rearm speaker={:08X} dist={:.1f}",
								timeoutSpeaker->GetFormID(),
								timeoutDist);
							return true;
						})) {
						return;
					}
				}
				if (g_bleedPaused) {
					g_bleedStart += (Now() - g_bleedPauseStarted);
					g_bleedPaused = false;
					g_bleedPauseStarted = {};
					g_bleedLastSeconds = -1;
					spdlog::info("[TFD][Defeat] bleed countdown resumed after dialogue");
				}

				{
					const auto nowBleed = Now();
					if (TFD::BleedoutGreet::TryStickyWatchdog(TFD::Bleedout::HasTerminalCommit(), IsDialogueOpen(), TFD::PleasureRuntime::IsBlocking(), CurrentBleedSpeakerID(), nowBleed,
						[&](const char* reopenReason) -> bool {
							auto* reopenSpeaker = CurrentBleedSpeaker();
							float reopenDist = 99999.0f;
							if (!(player && reopenSpeaker && CanUseAggressorForBleedoutGreet(player, reopenSpeaker, reopenDist))) {
								return false;
							}
							ApplyBleedDialogueOverdrive(player, reopenSpeaker, reopenReason, true);
							spdlog::info("[TFD][Defeat] bleed sticky watchdog reopen speaker={:08X} dist={:.1f}",
								reopenSpeaker->GetFormID(),
								reopenDist);
							return true;
						})) {
						return;
					}
				}

				if (CurrentBleedSpeakerID() == 0) {
					const auto nowBleed = Now();
					const auto lastNoSpeakerAttempt = TFD::Tame::GetBleedNoSpeakerTameLastAttempt();
					if (lastNoSpeakerAttempt.time_since_epoch().count() == 0 || (nowBleed - lastNoSpeakerAttempt) >= std::chrono::milliseconds(900)) {
						TFD::Tame::SetBleedNoSpeakerTameLastAttempt(nowBleed);
						const float bleedRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius());
						auto crowd = CollectBleedoutCrowd(bleedRadius, nullptr, false);
						const bool tameHeld = TFD::Tame::TryEnsureBleedNoSpeakerTameSession(crowd, "bleed_tick_no_speaker");
						if (tameHeld) {
							spdlog::info("[TFD][Defeat] bleed no-speaker tick tameHeld=1 crowdSize={}", crowd.size());
						}
					}
				}
				if (TFD::Bleedout::TryHandlePayReleaseDialogueClosed(
					TFD::BleedoutGreet::HasSeenDialogue(),
					g_prevDialogueOpen,
					BuildBleedDialogueCloseHandlers())) {
					g_prevDialogueOpen = false;
					return;
				}
				if (TFD::BleedoutGreet::HandleDialogueClosedFlow(
					TFD::BleedoutGreet::DialogueClosedContext{
						TFD::BleedoutGreet::HasSeenDialogue(),
						g_prevDialogueOpen,
						TFD::Bleedout::HasTerminalCommit(),
						TFD::PleasureRuntime::IsBlocking(),
						TFD::Bleedout::GetDialogueOutcome() == BleedDialogueOutcome::Captive
					},
					player,
					CurrentBleedSpeakerID(),
					TFD::BleedoutGreet::DialogueClosedHandlers{
						[&]() {
							g_prevDialogueOpen = false;
							g_bleedLastSeconds = -1;
							spdlog::info("[TFD][Defeat] bleed dialogue closed -> fallback suppressed activeTerminalCommit={}",
								TFD::Bleedout::GetTerminalCommitName(TFD::Bleedout::GetTerminalCommit()));
						},
						[&]() {
							g_prevDialogueOpen = false;
							g_bleedLastSeconds = -1;
							spdlog::info("[TFD][Defeat] bleed dialogue closed -> hold while pleasure runtime phase={} speaker={:08X}",
								TFD::PleasureRuntime::GetPhaseName(),
								TFD::PleasureRuntime::GetPrimarySpeaker() ? TFD::PleasureRuntime::GetPrimarySpeaker()->GetFormID() : 0u);
						},
						[&]() {
							g_prevDialogueOpen = false;
							ClearBleedDialogueOutcome("dialogue_closed_captive_commit");
							TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::FlowHandoff);
							if (TFD::Transition::ResolveCaptiveMarkerForOutcome(BuildTransitionRuntimeHandlers())) {
								spdlog::info("[TFD][Defeat] bleedout dialogue closed after captive outcome -> captive marker found");
								(void)TFD::Bleedout::DoBlackoutTeleport("blackout_teleport", BuildBleedBlackoutHandlers());
								SetGraceSeconds(4);
							}
 else {
  spdlog::info("[TFD][Defeat] bleedout dialogue closed after captive outcome -> no marker -> resolve fallback");
  TFD::Tame::ReleaseBleedNoSpeakerTameSession("dialogue_closed_captive_no_marker");
  g_inBleedState.store(false, std::memory_order_release);
  g_minHp = 0.0f;
  TFD::BleedoutGreet::ResetRuntime("bleed_reset");
  g_bleedLastSeconds = -1;
  (void)TFD::Bleedout::EnterNonCaptiveChoice("dialogue_closed_captive_no_marker", BuildBleedNonCaptiveChoiceHandlers());
}
},
[&](const TFD::BleedoutGreet::StickyReopenProbe& probe) {
	g_prevDialogueOpen = false;
	TFD::BleedoutGreet::ResetRuntime("bleed_reset");
	ResetBleedSpeakerKickState();
	g_bleedLastSeconds = -1;
	TFD::Bleedout::ClearSystemEventOutcomeWindow("dialogue_closed_sticky_reopen");
	auto* reopenSpeaker = probe.speakerFormID != 0 ? RE::TESForm::LookupByID<RE::Actor>(probe.speakerFormID) : nullptr;
	if (player && reopenSpeaker) {
		ApplyBleedDialogueOverdrive(player, reopenSpeaker, "dialogue_closed_sticky_reopen", true);
	}
	g_bleedPaused = true;
	g_bleedPauseStarted = Now();
	spdlog::info("[TFD][Defeat] bleedout dialogue closed without committed outcome -> sticky reopen forced speaker={:08X} dist={:.1f}",
		probe.speakerFormID,
		probe.distance);
},
[&](const TFD::BleedoutGreet::StickyReopenProbe& probe) {
	g_prevDialogueOpen = false;
	TFD::BleedoutGreet::ResetRuntime("bleed_reset");
	ResetBleedSpeakerKickState();
	g_bleedLastSeconds = -1;
	TFD::Bleedout::ClearSystemEventOutcomeWindow("dialogue_closed_sticky_reopen");
	if (probe.speakerFormID != 0) {
		spdlog::info("[TFD][Defeat] bleedout dialogue closed without committed outcome -> sticky reopen unavailable speaker={:08X} loaded={} dead={} dist={:.1f}",
			probe.speakerFormID,
			probe.loaded ? 1 : 0,
			probe.dead ? 1 : 0,
			probe.distance);
	}
else {
 spdlog::info("[TFD][Defeat] bleedout dialogue closed without committed outcome -> no speaker available for sticky reopen");
}
}
					})) {
					return;
				}

				if (TFD::Bleedout::IsAwaitingSystemEventOutcome() && !g_prevDialogueOpen) {
					if (TFD::Bleedout::HasTerminalCommit()) {
						TFD::BleedoutGreet::MarkStickyReopenPending(false, "terminal_commit_active");
						TFD::Bleedout::ClearSystemEventOutcomeWindow("terminal_commit_active");
						return;
					}
					if (TFD::Bleedout::TryResolvePostDialogueSystemEvent("post_dialogue_system_event", BuildBaseBleedPendingSystemEventHandlers())) {
						return;
					}
					if ([&]() {
						TFD::Bleedout::PendingSystemEventContext context{};
						context.runtimePhaseName = TFD::PleasureRuntime::GetPhaseName();
						context.runtimeActive = TFD::PleasureRuntime::IsActive();
						context.runtimeBlocking = TFD::PleasureRuntime::IsBlocking();
						auto handlers = BuildBaseBleedPendingSystemEventHandlers();
						handlers.getRetryCount = []() -> int { return CurrentBleedDialogueRetryCount(); };
						handlers.setRetryCount = [](int count) { SetCurrentBleedDialogueRetryCount(count); };
						handlers.promoteNextSpeaker = [player](const char* r) -> bool { return PromoteNextBleedSpeakerFromTruceQueue(player, r, false); };
						return TFD::Bleedout::TryHandlePendingSystemEventFallback(context, "post_dialogue_system_event_window_expired", handlers);
						}()) {
						return;
					}
				}

				const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(Now() - g_bleedStart).count();
				const int remain = bleedSeconds - static_cast<int>(elapsed);
				if (remain != g_bleedLastSeconds) {
					g_bleedLastSeconds = remain;
					if (remain > 0) {
						char msg[96]{};
						std::snprintf(msg, sizeof(msg), "TFDEngine: Bleeding... (%ds)", remain);
						RE::DebugNotification(msg);
					}
				}
				if (remain <= 0) {
					g_bleedLastSeconds = -1;
					const char* pendingSystemReason = nullptr;
					(void)TFD::Bleedout::IsSystemEventPendingForFallback(&pendingSystemReason);
					TFD::Bleedout::TimeoutContext timeoutContext{
						TFD::Bleedout::HasTerminalCommit(),
						TFD::Bleedout::GetTerminalCommitName(TFD::Bleedout::GetTerminalCommit()),
						pendingSystemReason,
						g_bleedPendingCaptiveOutcome
					};
					auto timeoutHandlers = BuildBleedTimeoutHandlers();
					if (TFD::Bleedout::HandleBleedTimeout(timeoutContext, "bleed_timeout", timeoutHandlers)) {
						return;
					}
				}
				return;
			}
			const float hpNow = player->GetActorValue(RE::ActorValue::kHealth);
			const float hpMax = (std::max)(1.0f, player->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float pct = (hpNow / hpMax) * 100.0f;
			const float thresh = TFD::Settings::GetDefeatThresholdPct();
			if (pct <= thresh) {
				EnterBleedLock(player, BleedLockKind::Player, thresh, "player_threshold");
				const float scanRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius());
				ClearEnemyTargetsToPlayerForDefeat(player, scanRadius, "player_threshold");
				auto* aggressor = ResolveAggressor();
				if (!aggressor) {
					aggressor = FindBestAggressor(scanRadius);
				}
				auto coalitionSnapshot = TFD::Actor::BuildSnapshot(scanRadius, false);
				const bool unresolvedBattle = !TFD::Actor::IsConflictResolved(coalitionSnapshot);
				const bool playerSideStanding = TFD::Actor::HasStandingTeammateOnPlayerSide(coalitionSnapshot);
				const bool hostileCoalitionStanding = TFD::Actor::HasStandingHostileCoalition(coalitionSnapshot);
				if (unresolvedBattle && hostileCoalitionStanding) {
					if (aggressor && !IsObserverAlly(aggressor)) {
						g_lastAggressor = aggressor->GetHandle();
					}
					spdlog::info("[TFD][Defeat] delay outcome unresolved battle coalitions={} playerSideStanding={}",
						coalitionSnapshot.activeCoalitionCount,
						playerSideStanding ? 1 : 0);
					if (StartBleedBattleObservePending(player)) {
						SetGraceSeconds(1);
						return;
					}
				}
				auto standingFollowers = CollectBleedStandingFollowers(scanRadius);
				if (!standingFollowers.empty()) {
					if (aggressor && !IsObserverAlly(aggressor)) {
						g_lastAggressor = aggressor->GetHandle();
					}
					if (StartBleedBattleObservePending(player)) {
						SetGraceSeconds(1);
						return;
					}
				}
				if (aggressor && IsObserverAlly(aggressor)) {
					aggressor = nullptr;
				}
				if (aggressor && !IsObserverAlly(aggressor)) {
					g_lastAggressor = aggressor->GetHandle();
				}
				aggressor = FindBestBleedoutSpeaker(scanRadius, 768.0f, aggressor);
				if (aggressor && !IsObserverAlly(aggressor)) {
					g_lastAggressor = aggressor->GetHandle();
				}
				if (!aggressor) {
					spdlog::info("[TFD][Defeat] no dialogue-capable aggressor and no standing follower -> bleed countdown without speaker");
				}
				StartBleedWindow(player, aggressor);
				SetGraceSeconds(1);
				return;
			}
		}


		class BleedOutcomeEventSink final : public RE::BSTEventSink<SKSE::ModCallbackEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* ev, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
			{
				if (!ev) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto* rawName = ev->eventName.c_str();
				if (!rawName || !rawName[0]) {
					return RE::BSEventNotifyControl::kContinue;
				}

				if (TFD::FlowController::HandleOutcomeModEvent(rawName, ev->strArg.c_str(), ev->numArg, ev->sender)) {
					return RE::BSEventNotifyControl::kContinue;
				}

				return RE::BSEventNotifyControl::kContinue;
			}
		};

		BleedOutcomeEventSink g_bleedOutcomeEventSink{};

		class PassiveBreakEventSink final : public RE::BSTEventSink<RE::TESHitEvent>, public RE::BSTEventSink<SKSE::ModCallbackEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* ev, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				if (!ev) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* causeActor = ev->cause ? ev->cause.get()->As<RE::Actor>() : nullptr;
				auto* targetActor = ev->target ? ev->target.get()->As<RE::Actor>() : nullptr;
				(void)TFD::FlowController::HandlePassiveBreakHitEvent(causeActor, targetActor);
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* ev, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
			{
				if (!ev) {
					return RE::BSEventNotifyControl::kContinue;
				}

				(void)TFD::FlowController::HandlePassiveBreakModEvent(ev->eventName.c_str(), ev->strArg.c_str());
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		PassiveBreakEventSink g_passiveBreakEventSink{};

		static void WorkerLoop()
		{
			while (g_running.load(std::memory_order_acquire)) {
				if (!g_tickPending.test_and_set(std::memory_order_acq_rel)) {
					auto* task = SKSE::GetTaskInterface();
					if (task) task->AddTask([]() { TickUI(); });
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
		}
	}

	// Temporary split module: install / shutdown / queued progress / load lifecycle
#include "TFDDefeatLifecycle.inl"

// Temporary split module: public bridge / query wrappers
// #include "TFDDefeatPublicBridge.inl"


	bool IsThresholdDownedActor(RE::Actor* actor)
	{
		if (!actor) {
			return true;
		}

		float thresholdPct = TFD::Settings::GetEnemyDownedThresholdPct();
		if (actor == Player()) {
			thresholdPct = TFD::Settings::GetDefeatThresholdPct();
		} else if (IsActiveFollowerActor(actor)) {
			thresholdPct = TFD::Settings::GetAllyDownedThresholdPct();
		}

		return IsActorDownByThreshold(actor, thresholdPct);
	}

	bool IsThresholdCombatTargetValid(RE::Actor* actor)
	{
		if (!actor) {
			return false;
		}
		if (actor->IsDisabled() || actor->IsDead()) {
			return false;
		}
		return !IsThresholdDownedActor(actor);
	}

	RE::Actor* ResolveCurrentPassivePrimaryActor()
	{
		return nullptr;
	}

	bool IsActorCoveredByCurrentPassiveContext(RE::Actor* actor)
	{
		return false;
	}

}
