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
		static TFD::Bleedout::RuntimeHostHandlers BuildBleedRuntimeHostHandlers();
		static void ApplyBleedDialogueOverdrive(RE::Actor* player, RE::Actor* speaker, const char* reason, bool restartDialogue);
		static bool PromoteNextBleedSpeakerFromTruceQueue(RE::Actor* player, const char* reason, bool rejectCurrent);
		static void MaintainBleedPrimaryCaptorBinding();
		static void QueueNonCaptiveChoiceRequest(const char* reason);
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


		struct DefeatedEnemyRegistryCache
		{
			RE::TESQuest* quest{ nullptr };
			RE::TESFaction* faction{ nullptr };
			std::array<RE::BGSRefAlias*, 10> enemyAliases{};
			bool resolved{ false };
		};

		DefeatedEnemyRegistryCache g_defeatedEnemyRegistry{};


		static constexpr double kDefeatedEnemyKnockSeconds = 30.0;
		static constexpr double kDefeatedReentrySuppressSeconds = 6.0;
		std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_defeatedReentrySuppress{};
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
		static void ResolveDefeatedEnemyRegistry();
		static bool IsDefeatedEnemyCandidate(RE::Actor* actor);
		static bool IsDefeatedEnemyKnockedInternal(RE::Actor* actor);
		static bool IsDialogueCapableDefeatedEnemyInternal(RE::Actor* actor);
		static bool IsCreatureDefeatedEnemyInternal(RE::Actor* actor);
		static double GetDefeatedEnemyRemainingSecondsInternal(RE::Actor* actor);
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

		static void ClearAllDefeatedEnemyAliases(const char* reason);
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


		static void ResolveDefeatedEnemyRegistry()
		{
			if (g_defeatedEnemyRegistry.resolved) {
				return;
			}
			g_defeatedEnemyRegistry.resolved = true;
			g_defeatedEnemyRegistry.quest = RE::TESForm::LookupByEditorID<RE::TESQuest>("TFDDefeatedEnemyQuest");
			g_defeatedEnemyRegistry.faction = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDDefeatedFaction");
			if (!g_defeatedEnemyRegistry.quest) {
				spdlog::warn("[TFD][Defeat] defeated enemy registry quest not found");
				return;
			}

			for (auto* baseAlias : g_defeatedEnemyRegistry.quest->aliases) {
				auto* refAlias = skyrim_cast<RE::BGSRefAlias*>(baseAlias);
				if (!refAlias) {
					continue;
				}
				const auto aliasName = std::string(refAlias->aliasName.c_str());
				if (aliasName.rfind("Enemy", 0) != 0 || aliasName.size() < 6) {
					continue;
				}
				try {
					int slot = std::stoi(aliasName.substr(5));
					if (slot >= 1 && slot <= static_cast<int>(g_defeatedEnemyRegistry.enemyAliases.size())) {
						g_defeatedEnemyRegistry.enemyAliases[static_cast<std::array<RE::BGSRefAlias*, 10Ui64>::size_type>(slot) - 1] = refAlias;
					}
				}
				catch (...) {
				}
			}

			std::size_t found = 0;
			for (auto* a : g_defeatedEnemyRegistry.enemyAliases) {
				if (a) {
					++found;
				}
			}
			spdlog::info("[TFD][Defeat] defeated enemy registry resolved quest={:08X} faction={:08X} aliases={}",
				g_defeatedEnemyRegistry.quest ? g_defeatedEnemyRegistry.quest->GetFormID() : 0u,
				g_defeatedEnemyRegistry.faction ? g_defeatedEnemyRegistry.faction->GetFormID() : 0u,
				found);
		}

		static void WriteDefeatedEnemyAlias(RE::BGSRefAlias* alias, RE::Actor* actor)
		{
			ResolveDefeatedEnemyRegistry();
			if (!g_defeatedEnemyRegistry.quest || !alias) {
				return;
			}

			RE::ObjectRefHandle handle{};
			if (actor) {
				handle = actor->CreateRefHandle();
			}

			RE::BSWriteLockGuard lock(g_defeatedEnemyRegistry.quest->aliasAccessLock);
			auto it = g_defeatedEnemyRegistry.quest->refAliasMap.find(alias->aliasID);
			if (actor) {
				if (it != g_defeatedEnemyRegistry.quest->refAliasMap.end()) {
					it->second = handle;
				}
				else {
					g_defeatedEnemyRegistry.quest->refAliasMap.insert({ alias->aliasID, handle });
				}
			}
			else {
				if (it != g_defeatedEnemyRegistry.quest->refAliasMap.end()) {
					g_defeatedEnemyRegistry.quest->refAliasMap.erase(it);
				}
			}
		}

		static int FindDefeatedEnemyAliasSlot(RE::Actor* actor)
		{
			if (!actor) {
				return -1;
			}
			ResolveDefeatedEnemyRegistry();
			for (std::size_t i = 0; i < g_defeatedEnemyRegistry.enemyAliases.size(); ++i) {
				auto* alias = g_defeatedEnemyRegistry.enemyAliases[i];
				if (!alias) {
					continue;
				}
				auto* current = alias->GetActorReference();
				if (current && current->GetFormID() == actor->GetFormID()) {
					return static_cast<int>(i);
				}
			}
			return -1;
		}

		static void SyncDefeatedEnemyAlias(RE::Actor* actor, BleedLockEntry& entry)
		{
			if (!actor) {
				return;
			}
			ResolveDefeatedEnemyRegistry();
			if (g_defeatedEnemyRegistry.faction) {
				actor->AddToFaction(g_defeatedEnemyRegistry.faction, 0);
				entry.defeatedFactionApplied = true;
			}

			int slot = FindDefeatedEnemyAliasSlot(actor);
			if (slot >= 0) {
				entry.defeatedAliasSlot = slot;
				return;
			}

			for (std::size_t i = 0; i < g_defeatedEnemyRegistry.enemyAliases.size(); ++i) {
				auto* alias = g_defeatedEnemyRegistry.enemyAliases[i];
				if (!alias) {
					continue;
				}
				auto* current = alias->GetActorReference();
				if (current && current != actor) {
					continue;
				}
				WriteDefeatedEnemyAlias(alias, actor);
				entry.defeatedAliasSlot = static_cast<int>(i);
				spdlog::info("[TFD][Defeat] defeated enemy alias fill alias='{}' actor={:08X}",
					alias->aliasName.c_str(), actor->GetFormID());
				return;
			}
		}

		static void ClearDefeatedEnemyMirrorState(RE::Actor* actor, BleedLockEntry& entry, const char* reason)
		{
			ResolveDefeatedEnemyRegistry();
			if (entry.defeatedAliasSlot >= 0 && entry.defeatedAliasSlot < static_cast<int>(g_defeatedEnemyRegistry.enemyAliases.size())) {
				if (auto* alias = g_defeatedEnemyRegistry.enemyAliases[entry.defeatedAliasSlot]) {
					auto* current = alias->GetActorReference();
					if (!actor || !current || current->GetFormID() == actor->GetFormID()) {
						WriteDefeatedEnemyAlias(alias, nullptr);
						spdlog::info("[TFD][Defeat] defeated enemy alias clear alias='{}' actor={:08X} reason={}",
							alias->aliasName.c_str(), actor ? actor->GetFormID() : 0u, reason ? reason : "unknown");
					}
				}
			}
			entry.defeatedAliasSlot = -1;

			if (entry.defeatedFactionApplied && actor && g_defeatedEnemyRegistry.faction) {
				actor->RemoveFromFaction(g_defeatedEnemyRegistry.faction);
			}
			entry.defeatedFactionApplied = false;
			entry.defeatedManaged = false;
			entry.defeatedAutoDeathIssued = false;
			entry.defeatedFatalDamageApplied = false;
			entry.defeatedDeadline = {};
		}

		static void ClearAllDefeatedEnemyAliases(const char* reason)
		{
			ResolveDefeatedEnemyRegistry();
			for (auto* alias : g_defeatedEnemyRegistry.enemyAliases) {
				if (!alias) {
					continue;
				}
				auto* current = alias->GetActorReference();
				if (!current) {
					continue;
				}
				WriteDefeatedEnemyAlias(alias, nullptr);
				spdlog::info("[TFD][Defeat] defeated enemy alias clear alias='{}' actor={:08X} reason={}",
					alias->aliasName.c_str(), current->GetFormID(), reason ? reason : "unknown");
			}
		}

		static bool IsDefeatedReentrySuppressed(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			auto it = g_defeatedReentrySuppress.find(actor->GetFormID());
			if (it == g_defeatedReentrySuppress.end()) {
				return false;
			}
			if (Now() >= it->second) {
				g_defeatedReentrySuppress.erase(it);
				return false;
			}
			return true;
		}

		static void SuppressDefeatedReentry(RE::Actor* actor, double seconds, const char* reason)
		{
			if (!actor) {
				return;
			}
			const auto secs = (std::max)(0.5, seconds);
			g_defeatedReentrySuppress[actor->GetFormID()] = Now() + std::chrono::milliseconds(static_cast<int>(secs * 1000.0));
			spdlog::info("[TFD][Defeat] defeated reentry suppress actor={:08X} seconds={:.1f} reason={}",
				actor->GetFormID(),
				secs,
				reason ? reason : "unknown");
		}

		static bool IsDefeatedEnemyCandidate(RE::Actor* actor)
		{
			auto* player = Player();
			if (!actor || !player || actor == player || actor->IsDead() || actor->IsDisabled()) {
				return false;
			}
			if (IsDefeatedReentrySuppressed(actor)) {
				return false;
			}
			if (actor->IsPlayerTeammate() || IsActiveFollowerActor(actor)) {
				return false;
			}
			if (TFD::Tame::IsCompanion(actor) || TFD::Tame::HasActiveSession(actor)) {
				return false;
			}
			if (!IsBleedCrowdSupportedAggressor(actor)) {
				return false;
			}
			if (actor->IsHostileToActor(player)) {
				return true;
			}
			auto targetSp = actor->GetActorRuntimeData().currentCombatTarget.get();
			if (auto* current = targetSp.get()) {
				if (current == player || IsActiveFollowerActor(current)) {
					return true;
				}
			}
			if (g_bleedBattleObserver.enemyIds.find(actor->GetFormID()) != g_bleedBattleObserver.enemyIds.end()) {
				return true;
			}
			if (g_lastAggressor) {
				auto sp = RE::Actor::LookupByHandle(g_lastAggressor.native_handle());
				if (sp.get() == actor) {
					return true;
				}
			}
			return actor->IsInCombat();
		}

		static bool IsDefeatedEnemyKnockedInternal(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			auto it = g_bleedLocks.find(actor->GetFormID());
			if (it == g_bleedLocks.end()) {
				return false;
			}
			return it->second.kind == BleedLockKind::Other && it->second.defeatedManaged;
		}

		static bool IsDialogueCapableDefeatedEnemyInternal(RE::Actor* actor)
		{
			return IsDefeatedEnemyKnockedInternal(actor) && actor && IsCaptiveSupportedAggressor(actor);
		}

		static bool IsCreatureDefeatedEnemyInternal(RE::Actor* actor)
		{
			if (!IsDefeatedEnemyKnockedInternal(actor) || !actor) {
				return false;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeNPC")) {
				return false;
			}
			return ActorHasKeywordByEditorID(actor, "ActorTypeCreature") || ActorHasKeywordByEditorID(actor, "ActorTypeAnimal") || ActorHasKeywordByEditorID(actor, "ActorTypeDaedra") || ActorHasKeywordByEditorID(actor, "ActorTypeUndead");
		}

		static double GetDefeatedEnemyRemainingSecondsInternal(RE::Actor* actor)
		{
			if (!actor) {
				return 0.0;
			}
			auto it = g_bleedLocks.find(actor->GetFormID());
			if (it == g_bleedLocks.end() || !it->second.defeatedManaged) {
				return 0.0;
			}
			const auto now = Now();
			if (it->second.defeatedDeadline <= now) {
				return 0.0;
			}
			return std::chrono::duration<double>(it->second.defeatedDeadline - now).count();
		}

		static void ApplyDefeatedEnemyPassiveOverride(RE::Actor* actor, BleedLockEntry& entry)
		{
			auto* avo = actor ? actor->AsActorValueOwner() : nullptr;
			if (!avo) {
				return;
			}
			if (!entry.aggressionOverridden) {
				entry.savedAggression = avo->GetActorValue(RE::ActorValue::kAggression);
				entry.aggressionOverridden = true;
			}
			avo->SetActorValue(RE::ActorValue::kAggression, 0.0f);
		}

		static void RestoreDefeatedEnemyPassiveOverride(RE::Actor* actor, BleedLockEntry& entry)
		{
			if (!entry.aggressionOverridden) {
				return;
			}
			auto* avo = actor ? actor->AsActorValueOwner() : nullptr;
			if (avo) {
				avo->SetActorValue(RE::ActorValue::kAggression, entry.savedAggression);
			}
			entry.aggressionOverridden = false;
		}


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
			return TFD::TeammateManager::CollectStandingFollowers(radius);
		}

		static bool IsStandingObserverActor(RE::Actor* actor)
		{
			return actor && !actor->IsDisabled() && !actor->IsDead() && !IsActorBleedingOut(actor);
		}

		static bool IsObserverAlly(RE::Actor* actor)
		{
			if (!IsStandingObserverActor(actor)) {
				return false;
			}
			if (actor == Player()) {
				return false;
			}
			if (IsActiveFollowerActor(actor)) {
				return true;
			}
			if (TFD::Tame::IsCompanion(actor)) {
				return true;
			}
			return false;
		}

		static bool IsObserverEnemy(RE::Actor* actor, RE::Actor* player, const std::vector<RE::Actor*>& allies, bool hostileHint, bool inCombatHint)
		{
			if (!player || !IsStandingEnemyThresholdActor(actor) || actor == player) {
				return false;
			}
			if (IsObserverAlly(actor)) {
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

		static float ComputeBleedBattleEnemyScanRadius(RE::Actor* player, const std::vector<RE::Actor*>& allies, float baseRadius)
		{
			float scanRadius = (std::max)(2400.0f, baseRadius);
			if (!player) {
				return scanRadius;
			}

			for (auto* ally : allies) {
				if (!IsStandingAllyThresholdActor(ally)) {
					continue;
				}
				const float dist = Distance3D(player->GetPosition(), ally->GetPosition());
				scanRadius = (std::max)(scanRadius, dist + 1600.0f);
			}

			return (std::min)(scanRadius, 9000.0f);
		}

		static float MinDistanceToObserverSide(RE::Actor* actor, RE::Actor* player, const std::vector<RE::Actor*>& allies)
		{
			if (!actor || !player) {
				return std::numeric_limits<float>::max();
			}

			float best = Distance3D(actor->GetPosition(), player->GetPosition());
			for (auto* ally : allies) {
				if (!IsStandingAllyThresholdActor(ally)) {
					continue;
				}
				best = (std::min)(best, Distance3D(actor->GetPosition(), ally->GetPosition()));
			}
			return best;
		}

		static bool IsLikelyObservedEnemySeed(RE::Actor* actor, RE::Actor* player, const std::vector<RE::Actor*>& allies, float seedRadius, bool hostileHint, bool inCombatHint)
		{
			if (!IsValidBleedBattleEnemyRosterActor(actor, player)) {
				return false;
			}
			if (!IsBleedCrowdSupportedAggressor(actor)) {
				return false;
			}

			if (IsObserverEnemy(actor, player, allies, hostileHint, inCombatHint)) {
				return true;
			}

			auto* target = ResolveCurrentCombatTarget(actor);
			if (target == player) {
				return true;
			}

			bool targetsSide = false;
			bool hostileToSide = actor->IsHostileToActor(player);
			for (auto* ally : allies) {
				if (!IsStandingAllyThresholdActor(ally)) {
					continue;
				}
				if (target == ally) {
					targetsSide = true;
				}
				if (actor->IsHostileToActor(ally)) {
					hostileToSide = true;
				}
			}

			if (targetsSide || hostileToSide) {
				return true;
			}

			if (!hostileHint && !inCombatHint && !actor->IsInCombat()) {
				return false;
			}

			return MinDistanceToObserverSide(actor, player, allies) <= seedRadius;
		}

		static std::vector<RE::Actor*> CollectCurrentObservedEnemies(RE::Actor* player, float radius, RE::Actor* preferredEnemy, const std::vector<RE::Actor*>& allies)
		{
			std::vector<RE::Actor*> out;
			if (!player) {
				return out;
			}
			auto* pCell = player->GetParentCell();
			if (!pCell) {
				return out;
			}

			const float scanRadius = ComputeBleedBattleEnemyScanRadius(player, allies, radius);
			const float seedRadius = (std::max)(1800.0f, scanRadius * 0.55f);
			auto snapshot = TFD::Actor::BuildSnapshot(scanRadius, false);
			for (const auto& info : snapshot.actors) {
				auto* actor = info.get();
				const bool hostile = info.hostileToPlayer;
				const bool inCombat = info.inCombat;
				if (!actor || actor->GetParentCell() != pCell || !actor->Is3DLoaded()) {
					continue;
				}
				if (TFD::Actor::IsActorOutsider(snapshot, actor)) {
					continue;
				}
				if (IsLikelyObservedEnemySeed(actor, player, allies, seedRadius, hostile, inCombat)) {
					out.push_back(actor);
				}
			}

			if (preferredEnemy && IsLikelyObservedEnemySeed(preferredEnemy, player, allies, seedRadius, true, preferredEnemy->IsInCombat())) {
				const auto id = preferredEnemy->GetFormID();
				auto it = std::find_if(out.begin(), out.end(), [id](RE::Actor* a) { return a && a->GetFormID() == id; });
				if (it == out.end()) {
					out.push_back(preferredEnemy);
				}
			}
			else if (out.empty()) {
				auto* fallbackEnemy = ResolveLastEnemyTargetingPlayerInternal(scanRadius, 15.0);
				if (!fallbackEnemy) {
					fallbackEnemy = FindBestAggressor(scanRadius);
				}
				if (fallbackEnemy && IsLikelyObservedEnemySeed(fallbackEnemy, player, allies, seedRadius, true, fallbackEnemy->IsInCombat())) {
					out.push_back(fallbackEnemy);
				}
			}

			return out;
		}

		static bool IsValidBleedBattleEnemyRosterActor(RE::Actor* actor, RE::Actor* player)
		{
			if (!player || !actor || actor == player) {
				return false;
			}
			if (!IsStandingEnemyThresholdActor(actor)) {
				return false;
			}
			if (IsObserverAlly(actor)) {
				return false;
			}
			if (!actor->Is3DLoaded()) {
				return false;
			}
			auto* pCell = player->GetParentCell();
			if (pCell && actor->GetParentCell() != pCell) {
				return false;
			}
			return true;
		}

		static void UpdateBleedBattleObserverRoster(RE::Actor* player, const std::vector<RE::Actor*>& followers, const std::vector<RE::Actor*>& enemies, RE::Actor* preferredEnemy)
		{
			if (!player) {
				return;
			}

			for (auto* ally : followers) {
				if (ally && IsStandingAllyThresholdActor(ally)) {
					g_bleedBattleObserver.allyIds.insert(ally->GetFormID());
				}
			}

			for (auto* enemy : enemies) {
				if (enemy && IsValidBleedBattleEnemyRosterActor(enemy, player)) {
					g_bleedBattleObserver.enemyIds.insert(enemy->GetFormID());
				}
			}

			if (preferredEnemy && IsValidBleedBattleEnemyRosterActor(preferredEnemy, player)) {
				g_bleedBattleObserver.enemyIds.insert(preferredEnemy->GetFormID());
			}

			g_bleedBattleObserver.hadValidObservedEnemy = g_bleedBattleObserver.hadValidObservedEnemy || !g_bleedBattleObserver.enemyIds.empty();
		}

		static std::vector<RE::Actor*> CollectBleedStandingFollowersFromSnapshot()
		{
			std::vector<RE::Actor*> out;
			for (auto id : g_bleedBattleObserver.allyIds) {
				auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
				if (!IsStandingAllyThresholdActor(actor)) {
					continue;
				}
				out.push_back(actor);
			}
			return out;
		}

		static std::vector<RE::Actor*> CollectBleedStandingEnemiesFromSnapshot()
		{
			std::vector<RE::Actor*> out;
			for (auto id : g_bleedBattleObserver.enemyIds) {
				auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
				if (!IsStandingEnemyThresholdActor(actor)) {
					continue;
				}
				out.push_back(actor);
			}
			return out;
		}

		static RE::Actor* PickClosestObservedTarget(RE::Actor* source, const std::vector<RE::Actor*>& candidates)
		{
			if (!source || candidates.empty()) {
				return nullptr;
			}

			RE::Actor* best = nullptr;
			float bestDist = std::numeric_limits<float>::max();
			const auto srcPos = source->GetPosition();

			for (auto* actor : candidates) {
				if (!actor || !IsStandingObserverActor(actor) || actor == source) {
					continue;
				}

				const float dist = Distance3D(srcPos, actor->GetPosition());
				if (dist < bestDist) {
					bestDist = dist;
					best = actor;
				}
			}

			return best;
		}

		static bool CanRedirectBleedEnemyToFollowers(RE::Actor* actor, RE::Actor* player, const std::vector<RE::Actor*>& followers)
		{
			if (!actor || !player || followers.empty()) {
				return false;
			}
			if (!IsCombatSupportedAggressor(actor) || !IsStandingEnemyThresholdActor(actor) || IsObserverAlly(actor)) {
				return false;
			}

			auto* currentTarget = ResolveCurrentCombatTarget(actor);
			if (currentTarget == player) {
				return true;
			}

			for (auto* follower : followers) {
				if (!IsStandingAllyThresholdActor(follower)) {
					continue;
				}
				if (currentTarget == follower) {
					return true;
				}
				if (actor->IsHostileToActor(follower)) {
					return true;
				}
			}

			if (g_bleedBattleObserver.enemyIds.find(actor->GetFormID()) != g_bleedBattleObserver.enemyIds.end()) {
				return true;
			}

			return actor->IsInCombat() || actor->IsHostileToActor(player);
		}

		static RE::Actor* ResolveBleedRedirectTargetInternal(RE::Actor* actor)
		{
			if (!IsPlayerBleedHoldTargetBlocked()) {
				return nullptr;
			}

			auto* player = Player();
			if (!player || !actor || actor == player) {
				return nullptr;
			}

			auto followers = CollectBleedStandingFollowersFromSnapshot();
			if (followers.empty() && g_bleedBattleObservePending) {
				const float radius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 600.0f);
				followers = CollectBleedStandingFollowers(radius);
			}
			if (followers.empty()) {
				return nullptr;
			}

			if (!CanRedirectBleedEnemyToFollowers(actor, player, followers)) {
				return nullptr;
			}

			auto* target = PickClosestObservedTarget(actor, followers);
			if (!target || target == player) {
				return nullptr;
			}

			return target;
		}

		static RE::Actor* ResolveBleedFollowerAggroTargetInternal(RE::Actor* actor)
		{
			if (!IsPlayerBleedHoldTargetBlocked()) {
				return nullptr;
			}

			auto* player = Player();
			if (!player || !actor || actor == player) {
				return nullptr;
			}
			if (!IsStandingAllyThresholdActor(actor)) {
				return nullptr;
			}
			if (!IsActiveFollowerActor(actor) && !TFD::Tame::IsCompanion(actor)) {
				return nullptr;
			}

			auto enemies = CollectBleedStandingEnemiesFromSnapshot();
			if (enemies.empty() && g_bleedBattleObservePending) {
				const float radius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 600.0f);
				auto followers = CollectBleedStandingFollowers(radius);
				auto* preferredEnemy = ResolveAggressor();
				if (!preferredEnemy) {
					preferredEnemy = ResolveLastEnemyTargetingPlayerInternal(radius, 15.0);
				}
				if (!preferredEnemy) {
					preferredEnemy = FindBestAggressor(radius);
				}
				enemies = CollectCurrentObservedEnemies(player, radius, preferredEnemy, followers);
			}
			if (enemies.empty()) {
				return nullptr;
			}

			RE::Actor* best = nullptr;
			float bestScore = std::numeric_limits<float>::max();
			const auto actorPos = actor->GetPosition();

			for (auto* enemy : enemies) {
				if (!enemy || enemy == actor || !IsStandingEnemyThresholdActor(enemy) || IsObserverAlly(enemy)) {
					continue;
				}
				if (!enemy->Is3DLoaded()) {
					continue;
				}

				auto* enemyTarget = ResolveCurrentCombatTarget(enemy);
				const bool targetsActor = enemyTarget == actor;
				const bool hostileToActor = enemy->IsHostileToActor(actor);
				const bool inCombat = enemy->IsInCombat();
				if (!targetsActor && !hostileToActor && !inCombat) {
					continue;
				}

				float score = Distance3D(actorPos, enemy->GetPosition());
				if (targetsActor) {
					score -= 3000.0f;
				}
				else if (enemyTarget && IsActiveFollowerActor(enemyTarget)) {
					score -= 1200.0f;
				}
				if (hostileToActor) {
					score -= 400.0f;
				}
				if (inCombat) {
					score -= 150.0f;
				}
				if (g_bleedBattlePreferredEnemy) {
					auto preferredSp = RE::Actor::LookupByHandle(g_bleedBattlePreferredEnemy.native_handle());
					if (auto* preferred = preferredSp.get(); preferred && preferred == enemy) {
						score -= 200.0f;
					}
				}

				if (score < bestScore) {
					bestScore = score;
					best = enemy;
				}
			}

			return best;
		}

		static bool StartBleedBattleObservePending(RE::Actor* player)
		{
			return TFD::Bleedout::StartRuntimeBattleObservePending(BuildBleedRuntimeHostStateRefs(), player, BuildBleedRuntimeHostHandlers());
		}

		static void TickBleedBattleObservePending()
		{
			TFD::Bleedout::TickRuntimeBattleObservePending(BuildBleedRuntimeHostStateRefs(), Player(), BuildBleedRuntimeHostHandlers());
		}

		static void EnterObservedBattleWin()
		{
			g_lastAggressor.reset();
			g_bleedBattleObservePending = false;
			TFD::FlowController::HandleObservedBattleWin("battle_observe_win");
		}

		static void EnterObservedLeftForDead(const char* reason)
		{
			g_lastAggressor.reset();
			TFD::FlowController::HandleObservedLeftForDead(reason ? reason : "battle_observe_loss");
		}

		static void TickBleedBattleObserve()
		{
			TFD::Bleedout::TickRuntimeBattleObserve(BuildBleedRuntimeHostStateRefs(), Player(), BuildBleedRuntimeHostHandlers());
		}

		static RE::AlchemyItem* ResolveRecoveryPotionCandidate()
		{
			auto* player = Player();
			if (!player) {
				return nullptr;
			}

			RE::AlchemyItem* bestPotion = nullptr;
			const auto inv = player->GetInventory([](RE::TESBoundObject& obj) {
				if (!obj.Is(RE::FormType::AlchemyItem)) {
					return false;
				}
				auto* potion = obj.As<RE::AlchemyItem>();
				return potion && potion->IsMedicine() && !potion->IsPoison() && !potion->IsFood();
				}, true);

			for (const auto& [item, invData] : inv) {
				const auto& [count, entry] = invData;
				(void)entry;
				if (count <= 0) {
					continue;
				}
				auto* potion = item->As<RE::AlchemyItem>();
				if (!potion) {
					continue;
				}
				if (!bestPotion || potion->GetFormID() < bestPotion->GetFormID()) {
					bestPotion = potion;
				}
			}

			return bestPotion;
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
			auto enemies = CollectCurrentObservedEnemies(player, radius, preferredEnemy, allies);
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
			return ResolveRecoveryPotionCandidate() ? 1 : 0;
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
			const bool hasRecoveryFactor = (ResolveRecoveryPotionCandidate() != nullptr);
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
			if (kind == BleedLockKind::Other && IsDefeatedEnemyCandidate(actor)) {
				entry.defeatedManaged = true;
				if (!wasDefeatedManaged) {
					entry.defeatedDeadline = Now() + std::chrono::milliseconds(static_cast<int>(kDefeatedEnemyKnockSeconds * 1000.0));
					entry.defeatedAutoDeathIssued = false;
					entry.defeatedFatalDamageApplied = false;
				}
				ApplyDefeatedEnemyPassiveOverride(actor, entry);
				SyncDefeatedEnemyAlias(actor, entry);
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
					SuppressDefeatedReentry(actor, kDefeatedReentrySuppressSeconds, reason);
				}
				ClearDefeatedEnemyMirrorState(actor, entry, reason);
				RestoreDefeatedEnemyPassiveOverride(actor, entry);
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
					ClearDefeatedEnemyMirrorState(actor, entry, reason);
					RestoreDefeatedEnemyPassiveOverride(actor, entry);
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
			ClearAllDefeatedEnemyAliases(reason);
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
						ApplyDefeatedEnemyPassiveOverride(actor, entry);
						SyncDefeatedEnemyAlias(actor, entry);
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


		static void ClearBleedDialogueOutcome(const char* reason)
		{
			TFD::Bleedout::ClearDialogueOutcome(reason);
		}


		static void ClearCaptiveOrchestrationResidue(bool clearPendingFadeIn)
		{
			if (clearPendingFadeIn) {
				TFD::Transition::ClearPendingFadeIn();
			}
			TFD::Captive::ClearEscapeContext();
			TFD::Captive::ResetLockpickWatch();
			g_grace.store(false, std::memory_order_release);
			g_prevDialogueOpen = false;
			TFD::Captive::SetPrevLockpickOpen(false);
			TFD::Captive::SetRuntimeState(false, CaptivePhaseValue::None);
		}

		static RE::Actor* ResolveBleedRuntimeSpeaker()
		{
			return CurrentBleedSpeaker();
		}

		static void BeginBleedPleasureRuntime(RE::Actor* speaker, bool captive, const char* reason)
		{
			(void)TFD::PleasureRuntime::BeginPleasure(
				speaker,
				captive ? TFD::PleasureRuntime::SourceContext::Captive : TFD::PleasureRuntime::SourceContext::Bleedout,
				reason);
		}

		static void ExitBleedSystemEventRuntime()
		{
			g_inBleedState.store(false, std::memory_order_release);
			g_minHp = 0.0f;
			TFD::BleedoutGreet::ResetRuntime("bleed_reset");
			g_bleedLastSeconds = -1;
		}

		static TFD::Bleedout::PendingSystemEventHandlers BuildBaseBleedPendingSystemEventHandlers()
		{
			TFD::Bleedout::PendingSystemEventHandlers handlers{};
			handlers.clearDialogueOutcome = [](const char* r) { ClearBleedDialogueOutcome(r); };
			handlers.clearOutcomeWindow = [](const char* r) { TFD::Bleedout::ClearSystemEventOutcomeWindow(r); };
			handlers.completePayRelease = [](const char* r) {
				auto completion = BuildPayReleaseCompletionHandlers();
				(void)TFD::Bleedout::CompletePayRelease(r, completion);
			};
			handlers.releaseFlowHandoff = []() { TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::FlowHandoff); };
			handlers.resolveCaptiveMarker = []() -> bool { return TFD::Transition::ResolveCaptiveMarkerForOutcome(BuildTransitionRuntimeHandlers()); };
			handlers.doBlackoutTeleport = []() {
				(void)TFD::Bleedout::DoBlackoutTeleport("blackout_teleport", BuildBleedBlackoutHandlers());
			};
			handlers.setGraceSeconds = [](int seconds) { SetGraceSeconds(seconds); };
			handlers.releaseNoSpeakerTameSession = [](const char* r) { TFD::Tame::ReleaseBleedNoSpeakerTameSession(r); };
			handlers.exitBleedState = []() { ExitBleedSystemEventRuntime(); };
			handlers.enterNonCaptiveChoice = [](const char* r) {
				(void)TFD::Bleedout::EnterNonCaptiveChoice(r, BuildBleedNonCaptiveChoiceHandlers());
			};
			return handlers;
		}

		static TFD::Bleedout::DialogueCloseHandlers BuildBleedDialogueCloseHandlers()
		{
			return TFD::Bleedout::DialogueCloseHandlers{
				[](const char* r) { ClearBleedDialogueOutcome(r); },
				[](const char* r) {
					auto completion = BuildPayReleaseCompletionHandlers();
					(void)TFD::Bleedout::CompletePayRelease(r, completion);
				}
			};
		}

		static TFD::Bleedout::TimeoutHandlers BuildBleedTimeoutHandlers()
		{
			TFD::Bleedout::TimeoutHandlers handlers{};
			handlers.releaseTruceGeneric = []() { TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::Generic); };
			handlers.resolveCaptiveMarker = []() -> bool { return TFD::Transition::ResolveCaptiveMarkerForOutcome(BuildTransitionRuntimeHandlers()); };
			handlers.resetBleedRuntimeState = []() { ResetBleedRuntimeState(); };
			handlers.doBlackoutTeleport = []() { (void)TFD::Bleedout::DoBlackoutTeleport("blackout_teleport", BuildBleedBlackoutHandlers()); };
			handlers.setGraceSeconds = [](int seconds) { SetGraceSeconds(seconds); };
			handlers.enterNonCaptiveChoice = [](const char* r) { (void)TFD::Bleedout::EnterNonCaptiveChoice(r, BuildBleedNonCaptiveChoiceHandlers()); };
			return handlers;
		}

		static TFD::Bleedout::CompletionHandlers BuildBaseBleedCompletionHandlers()
		{
			TFD::Bleedout::CompletionHandlers handlers{};
			handlers.clearOutcomeWindow = [](const char* r) { TFD::Bleedout::ClearSystemEventOutcomeWindow(r); };
			handlers.tryBeginTerminalCommit = [](BleedTerminalCommit kind, const char* r) { return TFD::Bleedout::TryBeginTerminalCommit(kind, r); };
			handlers.clearPendingCinematicFadeIn = []() { TFD::Transition::ClearPendingFadeIn(); };
			handlers.clearBridgeAliases = [](const char* r) { TFD::Bleedout::ClearBridgeAliases(nullptr, r); };
			handlers.clearEscapeContext = []() { TFD::Captive::ClearEscapeContext(); };
			handlers.resetLockpickWatch = []() { TFD::Captive::ResetLockpickWatch(); };
			handlers.setGraceActive = [](bool active) { g_grace.store(active, std::memory_order_release); };
			handlers.setCaptiveRuntime = [](bool captive) { TFD::Captive::SetRuntimeState(captive, captive ? CaptivePhaseValue::Captive : CaptivePhaseValue::None); };
			handlers.setPlayerBleedImmune = [](bool immune) { SetPlayerBleedImmune(immune); };
			handlers.recoverPlayerForTransition = []() { TFD::Transition::RecoverPlayerForTransition(BuildTransitionRuntimeHandlers()); };
			handlers.getSweepRadius = []() { return TFD::Settings::GetSweepRadius(); };
			handlers.applyCalmBubble = [](float radius) { ApplyCalmBubble(radius); };
			handlers.updatePreCombatState = []() { UpdatePreCombatState(); };
			handlers.resolveRuntimeSpeaker = []() -> RE::Actor* { return ResolveBleedRuntimeSpeaker(); };
			handlers.beginPleasure = [](RE::Actor* speaker, bool captive, const char* why) { BeginBleedPleasureRuntime(speaker, captive, why); };
			handlers.setPrevDialogueOpen = [](bool v) { g_prevDialogueOpen = v; };
			handlers.setPrevLockpickOpen = [](bool v) { TFD::Captive::SetPrevLockpickOpen(v); };
			return handlers;
		}

		static TFD::Bleedout::CompletionHandlers BuildCaptivePleasureCompletionHandlers()
		{
			auto handlers = BuildBaseBleedCompletionHandlers();
			handlers.clearLastAggressor = []() { g_lastAggressor.reset(); };
			handlers.resetBleedRuntimeState = [](bool preserve) { ResetBleedRuntimeState(preserve); };
			handlers.syncPlayerCaptiveAlias = [](const char* r) { TFD::Captive::SyncPlayerAlias(Player(), r); };
			return handlers;
		}

		static TFD::Bleedout::CompletionHandlers BuildPayReleaseCompletionHandlers()
		{
			auto handlers = BuildBaseBleedCompletionHandlers();
			handlers.clearFactionState = []() { TFD::Actor::Ops::ClearAggressorFactionContext(); };
			handlers.clearLastAggressor = []() { g_lastAggressor.reset(); };
			handlers.resetBleedRuntimeState = [](bool preserve) {
				if (preserve) {
					ResetBleedRuntimeState(true);
				} else {
					ResetBleedRuntimeState();
				}
			};
			handlers.beginLeftForDeadCooldown = [](int secs) { TFD::Transition::BeginLeftForDeadCooldown(secs); };
			handlers.setGraceSeconds = [](int secs) { SetGraceSeconds(secs); };
			return handlers;
		}

		static TFD::Bleedout::CompletionHandlers BuildBleedPleasureCompletionHandlers()
		{
			auto handlers = BuildBaseBleedCompletionHandlers();
			handlers.transitionBleedRuntimeToPleasureCommit = [](const char* r) { TransitionBleedRuntimeToPleasureCommit(r); g_lastRouterCombatContextActive = false; };
			handlers.beginLeftForDeadCooldown = [](int secs) { TFD::Transition::BeginLeftForDeadCooldown(secs); };
			handlers.setGraceSeconds = [](int secs) { SetGraceSeconds(secs); };
			handlers.refreshPostDefeatGlobals = []() { RefreshPostDefeatGlobals(); };
			return handlers;
		}

		static TFD::Bleedout::NonCaptiveChoiceHandlers BuildBleedNonCaptiveChoiceHandlers()
		{
			return TFD::Bleedout::NonCaptiveChoiceHandlers{
				[&]() {
					const auto activeCommit = TFD::Bleedout::GetTerminalCommit();
					return activeCommit != BleedTerminalCommit::None && activeCommit != BleedTerminalCommit::NonCaptiveFallback;
				},
				[&]() {
					return TFD::Bleedout::GetTerminalCommitName(TFD::Bleedout::GetTerminalCommit());
				},
				[&](const char* r) { return BeginResolvedNoMarkerFallback(r); },
				[&](const char* r) { TFD::Bleedout::ClearBridgeAliases(nullptr, r); },
				[&]() { TFD::Transition::ClearPendingFadeIn(); },
				[&]() { TFD::Actor::Ops::ClearAggressorFactionContext(); },
				[&]() { TFD::Captive::ClearEscapeContext(); },
				[&]() { TFD::Captive::ResetLockpickWatch(); },
				[&](bool active) { g_grace.store(active, std::memory_order_release); },
				[&]() { g_lastAggressor.reset(); },
				[&]() { ResetBleedRuntimeState(); },
				[&](bool v) { g_prevDialogueOpen = v; },
				[&](bool v) { TFD::Captive::SetPrevLockpickOpen(v); },
				[&](bool captive) { TFD::Captive::SetRuntimeState(captive, captive ? CaptivePhaseValue::Captive : CaptivePhaseValue::None); },
				[&](bool immune) { SetPlayerBleedImmune(immune); },
				[&](const char* r) { QueueNonCaptiveChoiceRequest(r); },
				[&](const char* r) { TFD::FlowController::Controller::GetSingleton().ResetRuntime(r ? r : "noncaptive_choice"); }
			};
		}

		static TFD::Bleedout::BlackoutHandlers BuildBleedBlackoutHandlers()
		{
			TFD::Bleedout::BlackoutHandlers handlers{};
			handlers.hasBlockingCommit = [&]() -> bool {
				const auto activeCommit = TFD::Bleedout::GetTerminalCommit();
				return activeCommit != BleedTerminalCommit::None && activeCommit != BleedTerminalCommit::Captive;
			};
			handlers.getBlockingCommitName = [&]() -> const char* {
				return TFD::Bleedout::GetTerminalCommitName(TFD::Bleedout::GetTerminalCommit());
			};
			handlers.resolveCaptiveMarker = [&]() -> bool { return TFD::Transition::ResolveCaptiveMarkerForOutcome(BuildTransitionRuntimeHandlers()); };
			handlers.enterNonCaptiveChoice = [&](const char* r) { (void)TFD::Bleedout::EnterNonCaptiveChoice(r, BuildBleedNonCaptiveChoiceHandlers()); };
			handlers.tryBeginCaptiveCommit = [&](const char* r) -> bool {
				const auto activeCommit = TFD::Bleedout::GetTerminalCommit();
				if (activeCommit == BleedTerminalCommit::None) {
					return TFD::Bleedout::TryBeginTerminalCommit(BleedTerminalCommit::Captive, r);
				}
				return true;
			};
			handlers.resetBleedRuntimeState = [&]() { ResetBleedRuntimeState(); };
			handlers.clearBridgeAliases = [&](const char* r) { TFD::Bleedout::ClearBridgeAliases(nullptr, r); };
			handlers.clearLastAggressor = [&]() { g_lastAggressor.reset(); };
			handlers.beginCaptiveFlow = [&]() {
				RE::DebugNotification("TFDEngine: Blackout -> Captive (1h)");
				(void)TFD::FlowController::Controller::GetSingleton().BeginCaptive(ResolveBleedFlowActorFormID(), TFD::FlowController::CaptiveMode::Kidnapped, "bleed_blackout_teleport");
			};
			handlers.clearPendingCinematicFadeIn = [&]() { TFD::Transition::ClearPendingFadeIn(); };
			handlers.queueCaptiveFadeTransition = [&]() -> bool { return TFD::Transition::QueueRequest(TFD::Transition::Kind::Captive, false, "captive_blackout"); };
			handlers.showBlackoutFader = [&]() { TFD::Transition::ShowBlackoutFader(); };
			handlers.completeCaptiveTransitionNow = [&](const char* r) { if (!TFD::Transition::CompleteCaptiveTransitionNow(r, BuildTransitionRuntimeHandlers(), BuildTransitionCaptiveHandlers())) { (void)TFD::Bleedout::EnterNonCaptiveChoice("teleport_failed", BuildBleedNonCaptiveChoiceHandlers()); } };
			handlers.hideBlackoutFader = [&]() { TFD::Transition::HideBlackoutFader(); };
			return handlers;
		}

		static void SetGraceSeconds(int seconds)
		{
			g_grace.store(true, std::memory_order_release);
			g_graceUntil = Now() + std::chrono::seconds((std::max)(0, seconds));
		}

		static bool IsGraceActive()
		{
			if (!g_grace.load(std::memory_order_acquire)) return false;
			if (Now() >= g_graceUntil) {
				g_grace.store(false, std::memory_order_release);
				return false;
			}
			return true;
		}

		static void ClearLastEnemyTargetingPlayerInternal()
		{
			g_lastEnemyTargetingPlayer = RE::ActorHandle{};
			g_lastEnemyTargetingPlayerFormID = 0;
			g_lastEnemyTargetingPlayerSeen = {};
		}

		static void NoteEnemyTargetingPlayerInternal(RE::Actor* actor)
		{
			auto* player = Player();
			if (!player || !actor || actor == player) {
				return;
			}
			if (!IsStandingEnemyThresholdActor(actor) || IsObserverAlly(actor) || !actor->Is3DLoaded()) {
				return;
			}
			if (!actor->IsHostileToActor(player) && !actor->IsInCombat()) {
				return;
			}
			g_lastEnemyTargetingPlayer = actor->GetHandle();
			g_lastEnemyTargetingPlayerFormID = actor->GetFormID();
			g_lastEnemyTargetingPlayerSeen = Now();
		}

		static RE::Actor* ResolveLastEnemyTargetingPlayerInternal(float radius, double maxAgeSec)
		{
			auto* player = Player();
			if (!player || !g_lastEnemyTargetingPlayer) {
				return nullptr;
			}
			if (g_lastEnemyTargetingPlayerSeen.time_since_epoch().count() != 0) {
				auto age = std::chrono::duration<double>(Now() - g_lastEnemyTargetingPlayerSeen).count();
				if (age > maxAgeSec) {
					ClearLastEnemyTargetingPlayerInternal();
					return nullptr;
				}
			}
			auto sp = RE::Actor::LookupByHandle(g_lastEnemyTargetingPlayer.native_handle());
			auto* actor = sp.get();
			if (!actor || actor == player || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
				ClearLastEnemyTargetingPlayerInternal();
				return nullptr;
			}
			if (!IsCombatSupportedAggressor(actor) || !IsStandingEnemyThresholdActor(actor) || IsObserverAlly(actor)) {
				ClearLastEnemyTargetingPlayerInternal();
				return nullptr;
			}
			auto* pCell = player->GetParentCell();
			if (pCell && actor->GetParentCell() != pCell) {
				ClearLastEnemyTargetingPlayerInternal();
				return nullptr;
			}
			if (!actor->IsHostileToActor(player) && !actor->IsInCombat()) {
				ClearLastEnemyTargetingPlayerInternal();
				return nullptr;
			}
			if (radius > 0.0f) {
				const float dist = Distance3D(actor->GetPosition(), player->GetPosition());
				if (dist > radius) {
					return nullptr;
				}
			}
			return actor;
		}

		static RE::Actor* ResolveAggressor()
		{
			auto* player = Player();
			if (!player) {
				return nullptr;
			}
			const float maxAggressorDist = (std::max)(2400.0f, TFD::Settings::GetSweepRadius());
			if (auto* cached = ResolveLastEnemyTargetingPlayerInternal(maxAggressorDist, 15.0)) {
				float dist = -1.0f;
				if (IsReasonableCombatAggressor(cached, player, maxAggressorDist, &dist)) {
					g_lastAggressor = cached->GetHandle();
					return cached;
				}
			}
			if (g_lastAggressor) {
				auto sp = RE::Actor::LookupByHandle(g_lastAggressor.native_handle());
				float dist = -1.0f;
				if (auto* actor = sp.get(); IsReasonableCombatAggressor(actor, player, maxAggressorDist, &dist)) {
					return actor;
				}
				if (auto* actor = sp.get(); actor) {
					spdlog::info("[TFD][Defeat] discard stale last aggressor {:08X} reason=combat_invalid", actor->GetFormID());
				}
			}

			if (auto* recent = TFD::PreCombatGreet::ResolveRecentAggressorAndCache(maxAggressorDist, g_lastAggressor)) {
				return recent;
			}

			return nullptr;
		}

		static bool IsActorCloseAndFront(RE::Actor* actor, RE::Actor* player, float maxDist)
		{
			if (!actor || !player) {
				return false;
			}

			const auto pa = player->GetPosition();
			const auto pb = actor->GetPosition();

			const float dx = pb.x - pa.x;
			const float dy = pb.y - pa.y;
			const float d2 = dx * dx + dy * dy;
			if (d2 > (maxDist * maxDist)) {
				return false;
			}

			const float len = std::sqrt((std::max)(1.0f, d2));
			const float ang = player->GetAngleZ();
			const float fx = std::sin(ang);
			const float fy = std::cos(ang);
			const float nx = dx / len;
			const float ny = dy / len;
			const float dot = nx * fx + ny * fy;
			return dot >= 0.20f;
		}

		static bool IsReasonableBleedoutSpeaker(RE::Actor* actor, RE::Actor* player, float maxDist, float* outDistance)
		{
			(void)player;
			return TFD::Bleedout::IsReasonableSpeaker(actor, maxDist, outDistance, BuildBleedoutSpeakerHandlers(false));
		}

		static RE::Actor* FindBestBleedoutSpeaker(float radius, float maxDist, RE::Actor* preferred)
		{
			return TFD::Bleedout::FindBestSpeaker(radius, maxDist, preferred, BuildBleedoutSpeakerHandlers(false));
		}

		static bool CanUseAggressorForBleedoutGreet(RE::Actor* player, RE::Actor* aggressor, float& outDistance)
		{
			(void)player;
			return TFD::Bleedout::CanUseSpeakerForGreet(aggressor, 1400.0f, outDistance, BuildBleedoutSpeakerHandlers(false));
		}

		static bool CanUseCaptiveFallbackHeuristic(RE::Actor* player, RE::Actor* aggressor, bool hasCaptiveOutcome, float& outDistance)
		{
			outDistance = 99999.0f;
			if (!hasCaptiveOutcome || !player || !aggressor) {
				return false;
			}

			if (!IsReasonableBleedoutSpeaker(aggressor, player, 768.0f, &outDistance)) {
				return false;
			}

			auto* marker = TFD::Location::GetCachedCaptiveMarker();
			if (!marker) {
				return false;
			}

			return true;
		}

		static RE::Actor* FindBestAggressor(float radius)
		{
			auto* player = Player();
			if (!player) return nullptr;
			auto* pCell = player->GetParentCell();
			if (!pCell) return nullptr;
			if (auto* cached = ResolveLastEnemyTargetingPlayerInternal(radius, 15.0)) {
				return cached;
			}

			auto snapshot = TFD::Actor::BuildSnapshot(radius, false);
			RE::Actor* best = nullptr;
			float bestScore = std::numeric_limits<float>::max();
			for (const auto& info : snapshot.actors) {
				auto* a = info.get();
				const float actorDist = info.dist;
				const bool hostileToPlayer = info.hostileToPlayer;
				const bool inCombat = info.inCombat;
				if (!a || a->IsDead() || a->IsDisabled()) continue;
				if (TFD::Actor::IsActorOutsider(snapshot, a)) continue;
				if (!a->Is3DLoaded()) continue;
				if (a->GetFormID() == player->GetFormID()) continue;
				if (a->GetParentCell() != pCell) continue;
				if (!IsCombatSupportedAggressor(a)) continue;
				if (!IsStandingEnemyThresholdActor(a)) continue;
				if (!IsBleedSpaceCompatible(a, player)) continue;
				auto* currentTarget = TFD::Actor::GetCurrentTarget(snapshot, a);
				const bool targetsPlayer = currentTarget == player;
				const bool targetsFollower = currentTarget && IsActiveFollowerActor(currentTarget);
				if (!hostileToPlayer && !targetsPlayer && !targetsFollower && !inCombat) continue;

				float score = actorDist;
				if (hostileToPlayer) score -= 200.0f;
				if (inCombat) score -= 120.0f;
				if (targetsPlayer) score -= 320.0f;
				if (targetsFollower) score -= 240.0f;
				if (score < bestScore) {
					bestScore = score;
					best = a;
				}
			}
			return best;
		}

		static void ApplyCalmBubble(float radius)
		{
			auto* player = Player();
			if (!player) {
				return;
			}

			auto* pCell = player->GetParentCell();
			const float sweepRadius = (std::max)(radius, (std::max)(TFD::Settings::GetSweepRadius(), 12000.0f));
			TFD::HostilityController::StopCombatSweep(sweepRadius, true);
			TFD::HostilityController::ScheduleStopCombatWaves(sweepRadius, true, 12, 120);
			auto snapshot = TFD::Actor::BuildSnapshot(sweepRadius, false);
			std::size_t applied = 0;
			for (const auto& info : snapshot.actors) {
				auto* a = info.get();
				const bool hostile = info.hostileToPlayer;
				const bool inCombat = info.inCombat;
				if (!a || a->IsDead() || a->IsDisabled()) continue;
				if (TFD::Actor::IsActorOutsider(snapshot, a)) continue;
				if (!a->Is3DLoaded()) continue;
				if (a->GetFormID() == player->GetFormID()) continue;
				if (pCell && a->GetParentCell() != pCell) continue;
				if (!hostile && !inCombat && !a->IsInCombat()) continue;

				if (auto* process = RE::ProcessLists::GetSingleton()) {
					const bool runDetection = process->runDetection;
					process->runDetection = false;
					process->ClearCachedFactionFightReactions();
					process->StopCombatAndAlarmOnActor(a, false);
					process->runDetection = runDetection;
				}

				TFD::HostilityController::ApplyAggressionClamp(a);
				a->StopCombat();
				if (a->IsWeaponDrawn()) {
					a->DrawWeaponMagicHands(false);
				}
				a->EvaluatePackage(true, false);
				++applied;
			}

			spdlog::info("[TFD][Defeat] calm bubble same-cell applied={} radius={:.0f}", applied, sweepRadius);
		}

		static bool BeginResolvedNoMarkerFallback(const char* reason);

		static void StartBleedWindow(RE::Actor* player, RE::Actor* aggressor)
		{
			TFD::Bleedout::StartRuntimeWindow(BuildBleedRuntimeHostStateRefs(), player, aggressor, BuildBleedRuntimeHostHandlers());
		}

		static bool BeginBleedoutDialogueHotkey()
		{
			TFD::Bleedout::DialogueHotkeyHandlers handlers{};
			handlers.speaker = BuildBleedoutSpeakerHandlers(false);
			handlers.isBleedoutActive = []() { return g_inBleedState.load(std::memory_order_acquire); };
			handlers.isCaptiveEscapePhase = []() { return TFD::Captive::IsEscapeActive(); };
			handlers.isDialogueOpen = []() { return IsDialogueOpen(); };
			handlers.resolveSpeakerFromRuntime = []() -> RE::Actor* {
				if (CurrentBleedSpeakerID() == 0) {
					return nullptr;
				}
				return CurrentBleedSpeaker();
				};
			handlers.resolveAggressor = []() -> RE::Actor* { return ResolveAggressor(); };
			handlers.findBestAggressor = [](float radius) -> RE::Actor* { return FindBestAggressor(radius); };
			handlers.releaseNoSpeakerTameSession = [](const char* reason) { TFD::Tame::ReleaseBleedNoSpeakerTameSession(reason); };
			handlers.releaseTruceSession = []() { TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::Generic); };
			handlers.startTruceSessionForSpeaker = [](RE::Actor* player, RE::Actor* aggressor, const char* reason) {
				return TFD::HostilityController::StartBleedTruceSessionForSpeaker(player, aggressor, reason);
				};
			handlers.resetSpeakerKick = []() {
				ResetBleedSpeakerKickState();
				};
			handlers.resetGreetRuntime = [](const char* reason) { TFD::BleedoutGreet::ResetRuntime(reason); };
			handlers.beginGreet = [](RE::Actor* actor, const char* reason) { TFD::BleedoutGreet::Begin(actor, reason); };
			return TFD::Bleedout::BeginDialogueHotkey((std::max)(2400.0f, TFD::Settings::GetSweepRadius()), 1800.0f, handlers);
		}

		static bool BeginResolvedNoMarkerFallback(const char* reason)
		{
			if (!TFD::Bleedout::TryBeginTerminalCommit(BleedTerminalCommit::NonCaptiveFallback, reason ? reason : "noncaptive_fallback")) {
				return false;
			}
			ClearCaptiveOrchestrationResidue();

			auto* player = Player();
			if (player && player->IsWeaponDrawn()) {
				player->DrawWeaponMagicHands(false);
			}

			const auto branch = TFD::Transition::ResolveNoMarkerFallback(reason, BuildTransitionRuntimeHandlers());
			if (branch == TFD::Transition::FallbackBranch::None) {
				return false;
			}

			TFD::Bleedout::ClearBridgeAliases(nullptr, reason ? reason : "noncaptive");
			SetPlayerBleedImmune(false);
			ResetBleedRuntimeState();
			if (player && !player->IsDead() && !player->IsDisabled()) {
				player->NotifyAnimationGraph("BleedoutStart");
			}
			g_lastAggressor.reset();
			UpdatePreCombatState();

			spdlog::info("[TFD][Transition] committed no-marker fallback branch={} reason={}",
				TFD::Transition::GetBranchName(branch),
				reason ? reason : "unknown");

			if (branch == TFD::Transition::FallbackBranch::RescueCached) {
				if (!TFD::Transition::BeginRescueTransition(reason ? reason : "rescue_cached", BuildTransitionRuntimeHandlers())) {
					TFD::Transition::ForceLeftForDeadSolo(BuildTransitionRuntimeHandlers());
					TFD::Transition::BeginRecoverTransition("rescue_cached_fallback_left_for_dead", BuildTransitionRuntimeHandlers());
				}
				return true;
			}

			TFD::Transition::BeginRecoverTransition(reason ? reason : TFD::Transition::GetBranchName(branch), BuildTransitionRuntimeHandlers());
			return true;
		}

		static void PreparePlayerForBleedoutPleasureScene(const char* reason)
		{
			auto* p = Player();
			if (!p || p->IsDead() || p->IsDisabled()) {
				return;
			}

			ReleasePlayerBleedLock(reason ? reason : "bleed_pleasure_prepare", true);
			p->NotifyAnimationGraph("BleedoutStop");
			p->NotifyAnimationGraph("GetUpStart");

			const float hpMax = (std::max)(1.0f, p->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float threshPct = std::clamp(TFD::Settings::GetDefeatThresholdPct() / 100.0f, 0.05f, 0.95f);
			const float safeHealthPct = std::clamp(threshPct + 0.08f, 0.22f, 0.95f);
			const float healthTarget = (std::max)(35.0f, hpMax * safeHealthPct);
			const float hpNow = p->GetActorValue(RE::ActorValue::kHealth);
			if (hpNow < healthTarget) {
				p->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, (healthTarget - hpNow));
			}

			const float staminaMax = (std::max)(1.0f, p->GetPermanentActorValue(RE::ActorValue::kStamina));
			const float staminaTarget = (std::max)(30.0f, staminaMax * 0.45f);
			const float staminaNow = p->GetActorValue(RE::ActorValue::kStamina);
			if (staminaNow < staminaTarget) {
				p->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina, (staminaTarget - staminaNow));
			}

			if (p->IsInCombat()) {
				p->StopCombat();
			}
			if (p->IsWeaponDrawn()) {
				p->DrawWeaponMagicHands(false);
			}
			p->EvaluatePackage(false, true);
			p->EvaluatePackage(true, true);
			spdlog::info("[TFD][Defeat] player prepared for bleed pleasure scene reason={} hpTarget={:.2f} hpNow={:.2f}",
				reason ? reason : "unknown",
				healthTarget,
				p->GetActorValue(RE::ActorValue::kHealth));
		}

		static void PreparePlayerForCaptivePleasureScene(const char* reason)
		{
			PreparePlayerForBleedoutPleasureScene(reason ? reason : "captive_pleasure_prepare");
			TFD::Captive::SetRuntimeState(true, CaptivePhaseValue::Captive);
			TFD::Captive::SyncPlayerAlias(Player(), reason ? reason : "captive_pleasure_prepare");
		}

		static TFD::Bleedout::RuntimeHostStateRefs BuildBleedRuntimeHostStateRefs()
		{
			TFD::Bleedout::RuntimeHostStateRefs state{};
			state.inBleedState = &g_inBleedState;
			state.minHp = &g_minHp;
			state.bleedStart = &g_bleedStart;
			state.bleedLastSeconds = &g_bleedLastSeconds;
			state.bleedPaused = &g_bleedPaused;
			state.bleedPauseStarted = &g_bleedPauseStarted;
			state.bleedLastCalmPulse = &g_bleedLastCalmPulse;
			state.bleedLastCrowdAssign = &TFD::Bleedout::BleedLastCrowdAssignRef();
			state.bleedCrowdAssigned = &TFD::Bleedout::BleedCrowdAssignedRef();
			state.bleedRejectedSpeakerIds = &TFD::Bleedout::BleedRejectedSpeakerIdsRef();
			state.bleedSpeakerId = &TFD::Bleedout::BleedSpeakerIDRef();
			state.bleedSpeakerKickLast = &TFD::Bleedout::BleedSpeakerKickLastRef();
			state.bleedSpeakerKickCount = &TFD::Bleedout::BleedSpeakerKickCountRef();
			state.bleedDialogueRetryCount = &TFD::Bleedout::BleedDialogueRetryCountRef();
			state.isEscapeBreakBleedPending = []() { return TFD::Captive::HasEscapeBreakRebleedPending(); };
			state.setEscapeBreakBleedPending = [](bool pending) { TFD::Captive::SetEscapeBreakRebleedPending(pending); };
			state.bleedPendingCaptiveOutcome = &g_bleedPendingCaptiveOutcome;
			state.bleedPendingNonCaptiveOutcome = &g_bleedPendingNonCaptiveOutcome;
			state.bleedBattleObservePending = &g_bleedBattleObservePending;
			state.bleedBattleObservePendingUntil = &g_bleedBattleObservePendingUntil;
			state.bleedBattleObservePendingLastRedirect = &g_bleedBattleObservePendingLastRedirect;
			state.bleedBattleObservePendingEmptyEnemyTicks = &g_bleedBattleObservePendingEmptyEnemyTicks;
			state.bleedBattleObserveActive = &g_bleedBattleObserveActive;
			state.bleedBattleObserveSince = &g_bleedBattleObserveSince;
			state.bleedBattleObserveLastRedirect = &g_bleedBattleObserveLastRedirect;
			state.bleedBattleObserveActiveEmptyEnemyTicks = &g_bleedBattleObserveActiveEmptyEnemyTicks;
			return state;
		}

		static TFD::Transition::RuntimeHandlers BuildTransitionRuntimeHandlers()
		{
			TFD::Transition::RuntimeHandlers handlers{};
			handlers.getPlayer = []() -> RE::Actor* { return Player(); };
			handlers.resolveAggressor = []() -> RE::Actor* { return ResolveAggressor(); };
			handlers.findBestAggressor = [](float radius) -> RE::Actor* { return FindBestAggressor(radius); };
			handlers.isCombatSupportedAggressor = [](RE::Actor* actor) { return IsCombatSupportedAggressor(actor); };
			handlers.isActiveFollowerActor = [](RE::Actor* actor) { return IsActiveFollowerActor(actor); };
			handlers.isStandingAllyThresholdActor = [](RE::Actor* actor) { return IsStandingAllyThresholdActor(actor); };
			handlers.collectRegisteredTeammates = []() { return TFD::TeammateManager::CollectRegisteredTeammates(); };
			handlers.collectBleedoutCrowd = [](float radius, RE::Actor* preferred, bool preserveAssigned) { return CollectBleedoutCrowd(radius, preferred, preserveAssigned); };
			handlers.getBleedCrowdAssigned = []() {
				std::vector<RE::FormID> ids{};
				const auto assigned = TFD::Bleedout::GetBleedCrowdAssignedIDs();
				ids.reserve(assigned.size());
				for (auto id : assigned) {
					ids.push_back(static_cast<RE::FormID>(id));
				}
				return ids;
			};
			handlers.tryAbortPleasureDueToHostileIntrusion = [](float radius) { return TryAbortPleasureDueToHostileIntrusion(radius); };
			handlers.releasePlayerBleedLock = [](const char* reason, bool playGetUp) { ReleasePlayerBleedLock(reason, playGetUp); };
			handlers.setGraceSeconds = [](int secs) { SetGraceSeconds(secs); };
			handlers.setRescueStateValue = [](int value) { SetRescueStateValue(value); };
			handlers.refreshPostDefeatGlobals = []() { RefreshPostDefeatGlobals(); };
			handlers.updatePreCombatState = []() { UpdatePreCombatState(); };
			return handlers;
		}

		static TFD::Transition::CaptiveHandlers BuildTransitionCaptiveHandlers()
		{
			TFD::Transition::CaptiveHandlers handlers{};
			handlers.resetBleedRuntimeState = []() { ResetBleedRuntimeState(); };
			handlers.clearBridgeAliases = [](const char* reason) { TFD::Bleedout::ClearBridgeAliases(nullptr, reason); };
			handlers.clearLastAggressor = []() { g_lastAggressor.reset(); };
			handlers.beginCaptiveFlow = [](const char* why) {
				RE::DebugNotification("TFDEngine: Blackout -> Captive (1h)");
				(void)TFD::FlowController::Controller::GetSingleton().BeginCaptive(ResolveBleedFlowActorFormID(), TFD::FlowController::CaptiveMode::Kidnapped, why ? why : "captive_enter");
			};
			handlers.setCaptiveRuntimeCaptive = []() { TFD::Captive::SetRuntimeState(true, CaptivePhaseValue::Captive); };
			handlers.isDialogueOpen = []() { return IsDialogueOpen(); };
			handlers.setPrevDialogueOpen = [](bool v) { g_prevDialogueOpen = v; };
			handlers.captureCurrentLockpickMenuState = []() { TFD::Captive::CaptureCurrentLockpickMenuState(); };
			handlers.resetLockpickWatch = []() { TFD::Captive::ResetLockpickWatch(); };
			handlers.armEscapeContextFromCurrentState = []() { TFD::Captive::ArmEscapeContextFromCurrentState(Player()); };
			handlers.sealCaptiveDoorIfPresent = []() { TFD::Captive::SealDoorIfPresent(); };
			handlers.applyCalmBubble = [](float radius) { ApplyCalmBubble(radius); };
			handlers.queuePendingCaptiveConfiscation = [](const char* reason, bool starterKitWanted) { TFD::Captive::QueuePendingConfiscation(reason, starterKitWanted); };
			handlers.syncPlayerCaptiveAlias = [](RE::Actor* actor, const char* reason) { TFD::Captive::SyncPlayerAlias(actor, reason); };
			return handlers;
		}

		static TFD::Bleedout::RuntimeHostHandlers BuildBleedRuntimeHostHandlers()
		{
			TFD::Bleedout::RuntimeHostHandlers handlers{};
			handlers.clearTerminalCommit = [](const char* reason) { TFD::Bleedout::ClearTerminalCommit(reason); };
			handlers.clearBridgeAliasesForActor = [](RE::Actor* actor, const char* reason) { TFD::Bleedout::ClearBridgeAliases(actor, reason); };
			handlers.clearNoMarkerFallbackState = []() { TFD::Transition::ClearNoMarkerFallbackState(); };
			handlers.releaseNoSpeakerTameSession = [](const char* reason) { TFD::Tame::ReleaseBleedNoSpeakerTameSession(reason); };
			handlers.clearBleedSupportBridgeAliases = [](const char* reason) { ClearBleedSupportBridgeAliases(reason); };
			handlers.releaseTruceSession = []() { TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::Generic); };
			handlers.resetGreetRuntime = [](const char* reason) { TFD::BleedoutGreet::ResetRuntime(reason); };
			handlers.clearCaptorAliases = [](const char* reason) { TFD::Bleedout::ClearCaptorAliases(reason); };
			handlers.clearDialogueOutcome = [](const char* reason) { ClearBleedDialogueOutcome(reason); };
			handlers.resetBattleObserveTracking = []() { g_bleedBattlePreferredEnemy = {}; g_bleedBattleObserver = {}; };
			handlers.setPlayerBleedImmune = [](bool active) { SetPlayerBleedImmune(active); };
			handlers.clampHealth = [](RE::Actor* actor, float minHp) { ClampHealth(actor, minHp); };
			handlers.collectBleedStandingFollowers = [](float radius) { return CollectBleedStandingFollowers(radius); };
			handlers.computeBleedBattleEnemyScanRadius = [](RE::Actor* player, const std::vector<RE::Actor*>& followers, float radius) { return ComputeBleedBattleEnemyScanRadius(player, followers, radius); };
			handlers.resolveAggressor = []() -> RE::Actor* { return ResolveAggressor(); };
			handlers.resolveLastEnemyTargetingPlayer = [](float radius, double linger) -> RE::Actor* { return ResolveLastEnemyTargetingPlayerInternal(radius, linger); };
			handlers.findBestAggressor = [](float radius) -> RE::Actor* { return FindBestAggressor(radius); };
			handlers.isObserverAlly = [](RE::Actor* actor) { return IsObserverAlly(actor); };
			handlers.collectCurrentObservedEnemies = [](RE::Actor* player, float radius, RE::Actor* preferred, const std::vector<RE::Actor*>& followers) { return CollectCurrentObservedEnemies(player, radius, preferred, followers); };
			handlers.updateObserverRoster = [](RE::Actor* player, const std::vector<RE::Actor*>& followers, const std::vector<RE::Actor*>& enemies, RE::Actor* preferred) { UpdateBleedBattleObserverRoster(player, followers, enemies, preferred); };
			handlers.collectStandingFollowersFromSnapshot = []() { return CollectBleedStandingFollowersFromSnapshot(); };
			handlers.collectStandingEnemiesFromSnapshot = []() { return CollectBleedStandingEnemiesFromSnapshot(); };
			handlers.hadValidObservedEnemy = []() { return g_bleedBattleObserver.hadValidObservedEnemy; };
			handlers.enterObservedBattleWin = []() { EnterObservedBattleWin(); };
			handlers.enterObservedLeftForDead = [](const char* reason) { EnterObservedLeftForDead(reason); };
			handlers.findBestSpeaker = [](float radius, float maxDist, RE::Actor* preferred) -> RE::Actor* { return FindBestBleedoutSpeaker(radius, maxDist, preferred); };
			handlers.isReasonableSpeaker = [](RE::Actor* actor, RE::Actor* player, float maxDist, float* outDistance) { return IsReasonableBleedoutSpeaker(actor, player, maxDist, outDistance); };
			handlers.collectBleedoutCrowd = [](float radius, RE::Actor* preferred, bool preserveAssigned) { return CollectBleedoutCrowd(radius, preferred, preserveAssigned); };
			handlers.isCaptiveSupportedAggressor = [](RE::Actor* actor) { return IsCaptiveSupportedAggressor(actor); };
			handlers.applyAllowedFactionFromAggressor = [](RE::Actor* actor) { return TFD::Actor::Ops::ApplyAggressorFactionContext(actor); };
			handlers.resolveCaptiveMarkerForOutcome = []() { return TFD::Transition::ResolveCaptiveMarkerForOutcome(BuildTransitionRuntimeHandlers()); };
			handlers.canUseCaptiveFallbackHeuristic = [](RE::Actor* player, RE::Actor* aggressor, bool hasCaptiveOutcome, float* outDistance) {
				if (!outDistance) {
					float dummy = 0.0f;
					return CanUseCaptiveFallbackHeuristic(player, aggressor, hasCaptiveOutcome, dummy);
				}
				return CanUseCaptiveFallbackHeuristic(player, aggressor, hasCaptiveOutcome, *outDistance);
				};
			handlers.tryEnsureNoSpeakerTameSession = [](const std::vector<RE::Actor*>& actors, const char* reason) { return TFD::Tame::TryEnsureBleedNoSpeakerTameSession(actors, reason); };
			handlers.setLastAggressor = [](RE::Actor* actor) { g_lastAggressor = actor ? actor->GetHandle() : RE::ActorHandle{}; };
			handlers.debugNotification = [](const char* msg) { RE::DebugNotification(msg); };
			handlers.clearEnemyTargetsToPlayerForDefeat = [](RE::Actor* player, float radius, const char* reason) { ClearEnemyTargetsToPlayerForDefeat(player, radius, reason); };
			handlers.resolveEscapeBreakPreferredAggressor = [](float radius) -> RE::Actor* {
				return TFD::Captive::ResolveEscapeBreakPreferredAggressor(radius, [](float fallbackRadius) -> RE::Actor* {
					return FindBestAggressor(fallbackRadius);
				});
			};
			handlers.startTruceSessionForSpeaker = [](RE::Actor* player, RE::Actor* speaker, const char* reason) { return TFD::HostilityController::StartBleedTruceSessionForSpeaker(player, speaker, reason); };
			handlers.canUseAggressorForBleedoutGreet = [](RE::Actor* player, RE::Actor* aggressor, float* outDistance) {
				if (!outDistance) {
					float dummy = 0.0f;
					return CanUseAggressorForBleedoutGreet(player, aggressor, dummy);
				}
				return CanUseAggressorForBleedoutGreet(player, aggressor, *outDistance);
				};
			handlers.applyDialogueOverdrive = [](RE::Actor* player, RE::Actor* speaker, const char* reason, bool restart) { ApplyBleedDialogueOverdrive(player, speaker, reason, restart); };
			return handlers;
		}

		static bool HandlePendingEscapeBreakBleed()
		{
			return TFD::Bleedout::HandleRuntimePendingEscapeBreak(BuildBleedRuntimeHostStateRefs(), Player(), BuildBleedRuntimeHostHandlers());
		}

		static void MaintainBleedSpeakerKick()
		{
			TFD::Bleedout::MaintainRuntimeSpeakerKick(BuildBleedRuntimeHostStateRefs(), Player(), BuildBleedRuntimeHostHandlers());
		}

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
				} else if (g_prevDialogueOpen && TFD::CaptiveGreet::IsActive()) {
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

	void Install()
	{
		if (g_installed.exchange(true, std::memory_order_acq_rel)) return;
		g_running.store(true, std::memory_order_release);
		g_loadTransition.store(false, std::memory_order_release);
		TFD::Captive::ResetForLoad();
		ClearCaptiveOrchestrationResidue(false);
		SetPlayerBleedImmune(false);
		ResetBleedRuntimeState();
		TFD::FlowController::Controller::GetSingleton().ResetRuntime("defeat_install");
		TFD::FlowController::InstallPassiveRuntimeProviders(TFD::FlowController::PassiveRuntimeProviders{
			[]() { return g_inBleedState.load(std::memory_order_acquire); },
			[]() { return TFD::Actor::Ops::HasAnyReleaseFollowGrace(); },
			[]() -> RE::Actor* { return ResolveCurrentPassivePrimaryActor(); },
			[](RE::Actor* actor) { return IsActorCoveredByCurrentPassiveContext(actor); },
			[](RE::Actor* actor, const char* reason) { TFD::Actor::Ops::CancelReleaseFollowGraceFromPlayerAggression(actor, reason); },
			[](const char* reason) { TFD::Actor::Ops::ClearAllReleaseFollowGrace(reason); },
			[]() { TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::PlayerAggression); },
			[](const char* reason) { ClearAllBleedLocks(reason); },
			[](RE::Actor* actor, const char* reason) { TFD::Bleedout::ClearBridgeAliases(actor, reason); },
			[]() { ClearPendingDefeatedDialogueTargetInternal(); }
		});
		TFD::FlowController::InstallOutcomeRuntimeProviders(TFD::FlowController::OutcomeRuntimeProviders{
			[](RE::Actor* actor, double seconds, const char* reason) { TFD::Actor::Ops::ApplyReleaseFollowGraceToSpeakerAndCrowd(actor, seconds, reason); },
			[](RE::Actor* actor, const char* reason) { TFD::Actor::Ops::RemoveReleaseFollowGraceFromSpeakerAndCrowd(actor, reason); },
			[]() { return ResolveBleedFlowActorFormID(); },
			[]() { return g_inBleedState.load(std::memory_order_acquire); },
			[](const char* reason) { PreparePlayerForCaptivePleasureScene(reason); },
			[](const char* reason) { auto completion = BuildCaptivePleasureCompletionHandlers(); (void)TFD::Bleedout::CompleteCaptivePleasureHandoff(reason, completion); },
			[](const char* reason) { PreparePlayerForBleedoutPleasureScene(reason); },
			[](const char* reason) { auto completion = BuildBleedPleasureCompletionHandlers(); (void)TFD::Bleedout::CompleteBleedPleasureHandoff(reason ? reason : "bleed_pleasure_handoff", completion); }
		});
		TFD::TeammateManager::InstallRuntimeProviders(TFD::TeammateManager::RuntimeProviders{
			[](RE::Actor* actor) {
				auto it = g_bleedLocks.find(actor ? actor->GetFormID() : 0u);
				return actor && it != g_bleedLocks.end() && it->second.kind == BleedLockKind::Ally;
			},
			[](RE::Actor* actor) { return IsActorBleedingOut(actor); },
			[](RE::Actor* actor) { return IsDialogueCapableDefeatedEnemyInternal(actor); },
			[](RE::Actor* actor) { return GetDefeatedEnemyRemainingSecondsInternal(actor); },
			[](RE::Actor* actor, double seconds, const char* reason) { SuppressDefeatedReentry(actor, seconds, reason); },
			[](RE::Actor* actor, const char* reason, bool playGetUp) { ReleaseBleedLock(actor, reason, playGetUp); },
			[](RE::Actor* actor, float thresholdPct, float bonusPct, float minSafePct, float maxSafePct, float minAbsHp, const char* reason) { RestoreActorHealthToSafePct(actor, thresholdPct, bonusPct, minSafePct, maxSafePct, minAbsHp, reason); },
			[]() -> RE::Actor* { return ResolvePendingDefeatedDialogueTargetInternal(); },
			[]() { ClearPendingDefeatedDialogueTargetInternal(); }
		});
		TFD::HostilityController::InstallBleedTruceRuntimeProviders(TFD::HostilityController::BleedTruceRuntimeProviders{
			[](RE::Actor* player, RE::Actor* speaker, const char* reason) {
				return TFD::Bleedout::StartTruceSessionForSpeaker(player, speaker, reason, BuildBleedRuntimeHostStateRefs(), BuildBleedRuntimeHostHandlers());
			},
			[](TFD::Tame::ReleaseReason reason) {
				TFD::Bleedout::ReleaseTruceSession(reason);
			}
		});
		TFD::Tame::InstallRuntimeProviders(TFD::Tame::RuntimeProviders{
			[](RE::Actor* actor) { return IsCreatureDefeatedEnemyInternal(actor); },
			[](RE::Actor* actor) { return GetDefeatedEnemyRemainingSecondsInternal(actor); },
			[](RE::Actor* actor, double seconds, const char* reason) { SuppressDefeatedReentry(actor, seconds, reason); },
			[](RE::Actor* actor, const char* reason, bool playGetUp) { ReleaseBleedLock(actor, reason, playGetUp); },
			[](RE::Actor* actor, float thresholdPct, float bonusPct, float minSafePct, float maxSafePct, float minAbsHp, const char* reason) { RestoreActorHealthToSafePct(actor, thresholdPct, bonusPct, minSafePct, maxSafePct, minAbsHp, reason); },
			[](const char* reason) { TFD::Bleedout::ReleaseNoSpeakerTameSession(reason); },
			[](const std::vector<RE::Actor*>& actors, const char* reason) {
				auto* player = Player();
				if (!player) {
					return false;
				}
				return TFD::Bleedout::TryEnsureNoSpeakerTameSession(actors, player, reason, BuildBleedRuntimeHostHandlers());
			}
		});
		TFD::FlowController::InstallBattleObserverRuntimeProviders(TFD::FlowController::BattleObserverRuntimeProviders{
			[](const char* reason) { TFD::Bleedout::ClearBridgeAliases(nullptr, reason); },
			[]() { ClearCaptiveOrchestrationResidue(); },
			[]() { TFD::Actor::Ops::ClearAggressorFactionContext(); },
			[]() { RecoverVictoryTeammates(); },
			[]() { ResetBleedRuntimeState(); },
			[]() { ClearPendingDefeatedDialogueTargetInternal(); },
			[](bool immune) { SetPlayerBleedImmune(immune); },
			[](const char* reason) { QueueNonCaptiveChoiceRequest(reason); },
			[]() -> RE::Actor* {
				const float followerRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 400.0f);
				auto followers = ResolveFollowerCandidates(followerRadius);
				return followers.downed;
			},
			[](RE::Actor* follower) { TFD::Transition::ArmObservedLeftForDeadFallback(follower, BuildTransitionRuntimeHandlers()); },
			[](const char* reason) { TFD::Transition::BeginRecoverTransition(reason ? reason : "battle_observe_loss", BuildTransitionRuntimeHandlers()); },
			[]() -> const char* { return TFD::Transition::GetCurrentFallbackBranchName(); }
		});
		TFD::PleasureRuntime::Install();
		g_lastRouterCombatContextActive = false;
		ClearAllBleedLocks("install");
		TFD::Bleedout::ClearBridgeAliases(nullptr, "install");
		TFD::Actor::Ops::Initialize();
		TFD::Location::Initialize();
		if (auto* scripts = RE::ScriptEventSourceHolder::GetSingleton()) {
			scripts->AddEventSink<RE::TESHitEvent>(&g_passiveBreakEventSink);
		}
		if (auto* src = SKSE::GetModCallbackEventSource()) {
			src->AddEventSink(&g_bleedOutcomeEventSink);
			src->AddEventSink(&g_passiveBreakEventSink);
		}
		ClearPendingDefeatedDialogueTargetInternal();
		g_worker = std::thread([]() { WorkerLoop(); });
		TFD::DefeatMonitor::ApplyQueuedProgressState();
		SetRescueStateValue(0);
		RefreshPostDefeatGlobals();
		spdlog::info("[TFD][Defeat] monitor installed");
	}

	void Shutdown()
	{
		if (!g_installed.exchange(false, std::memory_order_acq_rel)) return;
		g_running.store(false, std::memory_order_release);
		if (g_worker.joinable()) g_worker.join();
		ClearCaptiveOrchestrationResidue(false);
		g_hasQueuedProgressState = false;
		TFD::Captive::ClearQueuedLoadedState();
		g_queuedBleedOutState = false;
		SetPlayerBleedImmune(false);
		ResetBleedRuntimeState();
		TFD::FlowController::Controller::GetSingleton().ResetRuntime("defeat_shutdown");
		TFD::FlowController::ResetPassiveRuntimeProviders();
		TFD::FlowController::ResetOutcomeRuntimeProviders();
		TFD::FlowController::ResetBattleObserverRuntimeProviders();
		TFD::TeammateManager::ResetRuntimeProviders();
		TFD::Tame::ResetRuntimeProviders();
		TFD::HostilityController::ResetBleedTruceRuntimeProviders();
		g_lastRouterCombatContextActive = false;
		ClearAllBleedLocks("shutdown");
		TFD::Bleedout::ClearBridgeAliases(nullptr, "shutdown");
		g_loadTransition.store(false, std::memory_order_release);
		TFD::Transition::ClearLeftForDeadCooldown(BuildTransitionRuntimeHandlers());
		if (auto* scripts = RE::ScriptEventSourceHolder::GetSingleton()) {
			scripts->RemoveEventSink<RE::TESHitEvent>(&g_passiveBreakEventSink);
		}
		if (auto* src = SKSE::GetModCallbackEventSource()) {
			src->RemoveEventSink(&g_bleedOutcomeEventSink);
			src->RemoveEventSink(&g_passiveBreakEventSink);
		}
		TFD::PleasureRuntime::Shutdown();
		ClearPendingDefeatedDialogueTargetInternal();
		SetRescueStateValue(0);
		RefreshPostDefeatGlobals();
		spdlog::info("[TFD][Defeat] monitor shutdown");
	}

	void ResetGrace()
	{
		g_grace.store(false, std::memory_order_release);
		TFD::Transition::ClearLeftForDeadCooldown(BuildTransitionRuntimeHandlers());
		TFD::Actor::Ops::ClearAllReleaseFollowGrace("reset_grace");
	}

	bool HandlePassiveInvalidationAgainstActor(RE::Actor* actor, const char* reason)
	{
		return TFD::FlowController::HandlePassiveInvalidationAgainstActor(actor, reason, false);
	}

	bool GetCaptiveStateForSave()
	{
		return TFD::Captive::GetStateFlag();
	}

	std::uint32_t GetCaptivePhaseForSave()
	{
		return TFD::Captive::GetPhaseRaw();
	}

	bool GetBleedOutStateForSave()
	{
		if (auto* player = Player()) {
			return ComputePlayerBleedOutState(player);
		}
		return g_queuedBleedOutState;
	}

	void QueueLoadedBleedOutState(bool active)
	{
		g_queuedBleedOutState = active;
		spdlog::info("[TFD][Defeat] QueueLoadedBleedOutState state={}", active ? 1 : 0);
	}

	void QueueLoadedProgressState(bool stateActive, std::uint32_t phaseRaw)
	{
		g_hasQueuedProgressState = true;
		CaptivePhaseValue phase = stateActive ? PhaseFromRaw(phaseRaw) : CaptivePhaseValue::None;
		if (stateActive && phase == CaptivePhaseValue::None) phase = CaptivePhaseValue::Escape;
		TFD::Captive::QueueLoadedState(stateActive, phase);
		spdlog::info("[TFD][Defeat] QueueLoadedProgressState state={} phase={} normalized={}", stateActive ? 1 : 0, phaseRaw, static_cast<int>(TFD::Captive::GetQueuedPhase()));
	}

	void QueueDefaultProgressState()
	{
		g_hasQueuedProgressState = true;
		TFD::Captive::ClearQueuedLoadedState();
		g_queuedBleedOutState = false;
		spdlog::info("[TFD][Defeat] QueueDefaultProgressState");
	}

	bool HasQueuedProgressState()
	{
		return g_hasQueuedProgressState;
	}

	void ApplyQueuedProgressState()
	{
		if (!g_hasQueuedProgressState) QueueDefaultProgressState();
		TFD::Transition::ClearLeftForDeadCooldown(BuildTransitionRuntimeHandlers());
		TFD::Captive::SetRuntimeState(TFD::Captive::GetQueuedStateFlag(), TFD::Captive::GetQueuedPhase());
		g_prevDialogueOpen = IsDialogueOpen();
		TFD::Captive::CaptureCurrentLockpickMenuState();
		if (TFD::Captive::GetQueuedStateFlag() && TFD::Captive::GetQueuedPhase() == CaptivePhaseValue::Captive) {
			TFD::Location::RescanCaptiveMarker();
			TFD::Captive::ArmEscapeContextFromCurrentState(Player());
		}
		else {
			TFD::Captive::ResetLockpickWatch();
			TFD::Captive::ClearEscapeContext();
		}
		SetPlayerBleedImmune(false);
		TFD::Bleedout::ClearBridgeAliases(nullptr, "apply_queued_state");
		SetRescueStateValue(0);
		RefreshPostDefeatGlobals();
		UpdatePreCombatState();
		spdlog::info("[TFD][Defeat] ApplyQueuedProgressState state={} phase={} bleed={}", TFD::Captive::GetQueuedStateFlag() ? 1 : 0, static_cast<int>(TFD::Captive::GetQueuedPhase()), g_queuedBleedOutState ? 1 : 0);
	}

	void ResetForLoad()
	{
		g_grace.store(false, std::memory_order_release);
		SetPlayerBleedImmune(false);
		ResetBleedRuntimeState();
		ClearAllBleedLocks("reset_for_load");
		TFD::Bleedout::ClearBridgeAliases(nullptr, "reset_for_load");
		g_lastAggressor = RE::ActorHandle{};
		ClearLastEnemyTargetingPlayerInternal();
		ClearCaptiveOrchestrationResidue(false);
		TFD::Actor::Ops::ClearAggressorFactionContext();
		TFD::HostilityController::ClearAggressionClamp();
		TFD::Transition::ClearLeftForDeadCooldown(BuildTransitionRuntimeHandlers());
		SetRescueStateValue(0);
		g_lastRouterCombatContextActive = false;
		TFD::PleasureRuntime::ResetForLoad("defeat_reset_for_load");
		RefreshPostDefeatGlobals();
		spdlog::info("[TFD][Defeat] ResetForLoad -> runtime only");
	}

	void SetLoadTransition(bool active)
	{
		g_loadTransition.store(active, std::memory_order_release);
		if (active) {
			SetPlayerBleedImmune(false);
			ClearAllBleedLocks("set_load_transition");
			TFD::Bleedout::ClearBridgeAliases(nullptr, "set_load_transition");
			TFD::Captive::ResetLockpickWatch();
			TFD::PleasureRuntime::ResetForLoad("defeat_set_load_transition");
			spdlog::info("[TFD][Defeat] SetLoadTransition(true)");
		}
		else {
			spdlog::info("[TFD][Defeat] SetLoadTransition(false)");
		}
	}




	bool IsLeftForDeadRecoveryActive()
	{
		return TFD::Transition::IsRecoveryActive();
	}

	static RE::Actor* ResolveCurrentPassivePrimaryActor()
	{
		const auto snapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
		if (snapshot.primaryActorFormID != 0) {
			if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(snapshot.primaryActorFormID)) {
				return actor;
			}
		}

		if (auto* speaker = TFD::PleasureRuntime::GetPrimarySpeaker()) {
			return speaker;
		}

		if (CurrentBleedSpeakerID() != 0) {
			if (auto* actor = CurrentBleedSpeaker()) {
				return actor;
			}
		}
		if (g_lastAggressor) {
			auto sp = RE::Actor::LookupByHandle(g_lastAggressor.native_handle());
			if (sp) {
				return sp.get();
			}
		}
		return nullptr;
	}

	static bool IsActorCoveredByCurrentPassiveContext(RE::Actor* actor)
	{
		if (!actor || actor == Player() || actor->IsDead() || actor->IsDisabled()) {
			return false;
		}

		if (TFD::HostilityController::IsActorTemporarilySuppressed(actor)) {
			return true;
		}

		if (TFD::PleasureRuntime::IsActorTracked(actor)) {
			return true;
		}

		if (auto* primary = ResolveCurrentPassivePrimaryActor()) {
			if (actor == primary) {
				return true;
			}
			if (TFD::Actor::SharesAllowedFactionExact(actor, primary)) {
				return true;
			}
		}

		return false;
	}

	bool IsPlayerBleedHoldTargetBlocked()
	{
		return g_inBleedState.load(std::memory_order_acquire) &&
			(g_bleedBattleObservePending || g_bleedBattleObserveActive);
	}

	bool IsObservedCombatCommitInProgress()
	{
		return g_observedCombatCommitDepth > 0;
	}

	bool IsThresholdDownedActor(RE::Actor* actor)
	{
		if (!actor) {
			return true;
		}
		auto* player = Player();
		if (actor == player) {
			return IsActorDownByThreshold(actor, TFD::Settings::GetDefeatThresholdPct());
		}
		if (IsActiveFollowerActor(actor) || TFD::Tame::IsCompanion(actor)) {
			return IsActorDownByThreshold(actor, TFD::Settings::GetAllyDownedThresholdPct());
		}
		return IsActorDownByThreshold(actor, TFD::Settings::GetEnemyDownedThresholdPct());
	}

	bool ReviveDownedAlly(RE::Actor* actor, float targetHealthPct)
	{
		if (!actor || actor == Player() || actor->IsDisabled() || actor->IsDead()) {
			return false;
		}

		const bool managedAlly = IsActiveFollowerActor(actor) || TFD::Tame::IsCompanion(actor) || TFD::Tame::HasActiveSession(actor);
		if (!managedAlly) {
			return false;
		}

		auto it = g_bleedLocks.find(actor->GetFormID());
		const bool hadAllyLock = it != g_bleedLocks.end() && it->second.kind == BleedLockKind::Ally;
		const bool downed = hadAllyLock || IsActorBleedingOut(actor) || IsActorDownByThreshold(actor, TFD::Settings::GetAllyDownedThresholdPct());

		const float desiredPct = std::clamp(targetHealthPct, 20.0f, 95.0f);
		const float desiredRatio = desiredPct / 100.0f;
		const float thresholdPct = TFD::Settings::GetAllyDownedThresholdPct();
		const float thresholdRatio = std::clamp(thresholdPct / 100.0f, 0.05f, 0.95f);
		const float bonusPct = (std::max)(0.0f, desiredRatio - thresholdRatio);

		if (hadAllyLock) {
			ReleaseBleedLock(actor, "ally_feed_revive", true);
		}
		else if (downed) {
			actor->NotifyAnimationGraph("BleedoutStop");
			actor->NotifyAnimationGraph("GetUpStart");
		}

		RestoreActorHealthToSafePct(actor, thresholdPct, bonusPct, desiredRatio, desiredRatio, 35.0f, downed ? "ally_feed_revive" : "ally_feed_heal");

		if (auto* process = RE::ProcessLists::GetSingleton()) {
			process->ClearCachedFactionFightReactions();
		}
		actor->EvaluatePackage(false, true);
		actor->EvaluatePackage(true, true);
		actor->UpdateCombat();
		if (auto* player = Player()) {
			player->UpdateCombat();
		}

		spdlog::info("[TFD][Defeat] ally feed revive actor={:08X} downed={} healPct={:.1f} allyLock={}",
			actor->GetFormID(),
			downed ? 1 : 0,
			desiredPct,
			hadAllyLock ? 1 : 0);
		return true;
	}

	bool IsThresholdCombatTargetValid(RE::Actor* actor)
	{
		if (!actor || actor->IsDisabled() || actor->IsDead()) {
			return false;
		}
		if (IsThresholdDownedActor(actor)) {
			return false;
		}
		if (TFD::HostilityController::IsActorTemporarilySuppressed(actor)) {
			return false;
		}
		return true;
	}

	void NoteEnemyTargetingPlayer(RE::Actor* actor)
	{
		NoteEnemyTargetingPlayerInternal(actor);
	}

	RE::Actor* ResolveBleedRedirectTarget(RE::Actor* actor)
	{
		return ResolveBleedRedirectTargetInternal(actor);
	}

	RE::Actor* ResolveBleedFollowerAggroTarget(RE::Actor* actor)
	{
		return ResolveBleedFollowerAggroTargetInternal(actor);
	}

	bool IsBleedoutActive()
	{
		return g_inBleedState.load(std::memory_order_acquire);
	}

	bool HandleBleedoutHotkey()
	{
		return BeginBleedoutDialogueHotkey();
	}


	bool IsDefeatedEnemyKnocked(RE::Actor* actor)
	{
		return IsDefeatedEnemyKnockedInternal(actor);
	}

	bool IsDialogueCapableDefeatedEnemy(RE::Actor* actor)
	{
		return IsDialogueCapableDefeatedEnemyInternal(actor);
	}

	bool IsCreatureDefeatedEnemy(RE::Actor* actor)
	{
		return IsCreatureDefeatedEnemyInternal(actor);
	}

	double GetDefeatedEnemyRemainingSeconds(RE::Actor* actor)
	{
		return GetDefeatedEnemyRemainingSecondsInternal(actor);
	}

	void SetPendingDefeatedDialogueTarget(RE::Actor* actor)
	{
		SetPendingDefeatedDialogueTargetInternal(actor);
	}

	bool RecruitDefeatedHumanoidAsTeammate(RE::Actor* actor)
	{
		return TFD::TeammateManager::RecruitDefeatedHumanoidAsTeammate(actor);
	}


}