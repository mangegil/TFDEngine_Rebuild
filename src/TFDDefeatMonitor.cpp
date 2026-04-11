#include "TFDDefeatMonitor.h"

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
#include "TFDCaptiveRuntime.h"
#include "TFDRescueRuntime.h"
#include "TFDVictoryRuntime.h"
#include "TFDCaptiveDoorController.h"
#include "TFDAntiAggro.h"
#include "TFDFactionMask.h"
#include "TFDActorScan.h"
#include "TFDAggressionClamp.h"
#include "TFDPreCombatGreet.h"
#include "TFDInCombat.h"
#include "TFDInCombatGreet.h"
#include "TFDPacify.h"
#include "TFDFlowController.h"
#include "TFDForceGreet.h"
#include "TFDBleedout.h"
#include "TFDBleedoutGreet.h"
#include "TFDPleasureRuntime.h"
#include "EditorIdCache.h"
#include "RE/B/BGSRefAlias.h"
#include "RE/T/TESQuest.h"

namespace TFD::DefeatMonitor
{
	static RE::Actor* ResolveCurrentPassivePrimaryActor();
	static bool IsActorCoveredByCurrentPassiveContext(RE::Actor* actor);
	static bool InvalidatePassiveStateForActor(RE::Actor* actor, const char* reason, bool severeCrime);

	namespace
	{
		static bool ActorHasLineOfSightToPlayer(RE::Actor* actor, RE::Actor* player);
		static float Distance3D(const RE::NiPoint3& a, const RE::NiPoint3& b);
		using CaptivePhaseValue = TFD::CaptiveRuntime::PhaseValue;

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
		constexpr const char* kPassiveBreakCrimeEvent = "TFDPassiveBreakCrime";
		constexpr const char* kPassiveBreakPickpocketEvent = "TFDPassiveBreakPickpocket";

		static std::uint32_t ResolveBleedFlowActorFormID();
		static void ResolveTruceQuestRegistry();
		static bool StartBleedTruceSessionForSpeaker(RE::Actor* player, RE::Actor* speaker, const char* reason);
		static TFD::Bleedout::RuntimeHostStateRefs BuildBleedRuntimeHostStateRefs();
		static TFD::Bleedout::RuntimeHostHandlers BuildBleedRuntimeHostHandlers();
		static void ApplyBleedForceGreetOverdrive(RE::Actor* player, RE::Actor* speaker, const char* reason, bool restartForceGreet);
		static bool PromoteNextBleedSpeakerFromTruceQueue(RE::Actor* player, const char* reason, bool rejectCurrent);
		static void MaintainBleedPrimaryCaptorBinding();
		static RE::TESFaction* ResolveReleaseFollowHelperFaction();
		static RE::TESFaction* ResolvePacifyHelperFaction();
		static bool ActorHasActiveDialoguePhaseFaction(RE::Actor* actor);
		static std::vector<RE::Actor*> CollectReleaseFollowGraceActors(RE::Actor* speaker);
		static void RemoveReleaseFollowGraceFromActor(RE::Actor* actor, const char* reason);
		static void RemoveReleaseFollowGraceFromSpeakerAndCrowd(RE::Actor* speaker, const char* reason);
		static void ApplyReleaseFollowGraceToActor(RE::Actor* actor, double durationSeconds, const char* reason);
		static void ApplyReleaseFollowGraceToSpeakerAndCrowd(RE::Actor* speaker, double durationSeconds, const char* reason);
		static void MaintainReleaseFollowGrace();
		static void ClearAllReleaseFollowGrace(const char* reason);
		static bool HasReleaseFollowGrace(RE::Actor* actor);
		static bool SuppressReleaseFollowTargetingToPlayer(RE::Actor* actor, RE::Actor* player, const char* reason);
		static void CancelReleaseFollowGraceFromPlayerAggression(RE::Actor* actor, const char* reason);

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
		std::chrono::steady_clock::time_point g_bleedLastCrowdAssign{};

		std::vector<RE::FormID> g_bleedCrowdAssigned{};
		RE::FormID g_bleedTruceSessionId = 0;
		RE::FormID g_bleedNoSpeakerTameSessionId = 0;
		RE::FormID g_bleedNoSpeakerTamePrimaryId = 0;
		std::chrono::steady_clock::time_point g_bleedNoSpeakerTameLastAttempt{};
		RE::FormID g_bleedSpeakerId = 0;
		std::chrono::steady_clock::time_point g_bleedSpeakerKickLast{};
		int g_bleedSpeakerKickCount = 0;
		bool g_escapeBreakBleedPending = false;
		RE::ActorHandle g_escapeBreakPreferredAggressor{};

		std::atomic_bool g_grace{ false };
		std::chrono::steady_clock::time_point g_graceUntil{};

		bool g_leftForDeadActive = false;
		std::chrono::steady_clock::time_point g_leftForDeadUntil{};
		std::chrono::steady_clock::time_point g_leftForDeadNextPulse{};
		std::chrono::steady_clock::time_point g_leftForDeadPleasureDeferLast{};
		bool g_leftForDeadNeedsAggroKick = false;

		RE::ActorHandle g_lastAggressor{};

		static std::uint32_t ResolveBleedFlowActorFormID()
		{
			if (g_bleedSpeakerId != 0) {
				return g_bleedSpeakerId;
			}
			return TFD::Flow::Controller::GetSingleton().GetSnapshot().primaryActorFormID;
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

		bool& g_captiveState = TFD::CaptiveRuntime::StateRef();
		CaptivePhaseValue& g_captivePhase = TFD::CaptiveRuntime::PhaseRef();
		bool g_prevDialogueOpen = false;
		bool g_prevLockpickOpen = false;
		bool& g_captiveConfiscationApplied = TFD::CaptiveRuntime::ConfiscationAppliedRef();
		bool& g_captiveConfiscationPending = TFD::CaptiveRuntime::ConfiscationPendingRef();
		bool& g_captiveStarterLockpickPending = TFD::CaptiveRuntime::StarterLockpickPendingRef();
		std::string& g_captivePendingConfiscationReason = TFD::CaptiveRuntime::PendingConfiscationReasonRef();
		int& g_captiveConfiscationAttemptCount = TFD::CaptiveRuntime::ConfiscationAttemptCountRef();
		std::chrono::steady_clock::time_point& g_captiveConfiscationNextAttempt = TFD::CaptiveRuntime::ConfiscationNextAttemptRef();
		static constexpr int kCaptiveConfiscationInitialDelayMs = 300;
		static constexpr int kCaptiveConfiscationRetryDelayMs = 250;
		static constexpr int kCaptiveConfiscationMaxAttempts = 20;

		struct TeammateRegistryCache
		{
			RE::TESQuest* quest{ nullptr };
			RE::TESFaction* currentFollowerFaction{ nullptr };
			RE::TESFaction* playerFollowerFaction{ nullptr };
			std::array<RE::BGSRefAlias*, 10> teammateAliases{};
			bool resolved{ false };
		};

		TeammateRegistryCache g_teammateRegistry{};

		struct DefeatedEnemyRegistryCache
		{
			RE::TESQuest* quest{ nullptr };
			RE::TESFaction* faction{ nullptr };
			std::array<RE::BGSRefAlias*, 10> enemyAliases{};
			bool resolved{ false };
		};

		DefeatedEnemyRegistryCache g_defeatedEnemyRegistry{};

		using CaptiveQuestRegistryCache = TFD::CaptiveRuntime::QuestRegistryCache;
		CaptiveQuestRegistryCache& g_captiveQuestRegistry = TFD::CaptiveRuntime::QuestRegistryRef();


		struct TruceQuestRegistryCache
		{
			RE::TESQuest* quest{ nullptr };
			std::array<RE::BGSRefAlias*, 10> truceAliases{};
			bool resolved{ false };
		};

		TruceQuestRegistryCache g_truceQuestRegistry{};

		struct ReleaseFollowGraceEntry
		{
			RE::ActorHandle actor{};
			std::chrono::steady_clock::time_point expiresAt{};
			std::string source{};
		};

		std::unordered_map<RE::FormID, ReleaseFollowGraceEntry> g_releaseFollowGraceEntries{};
		std::unordered_set<RE::FormID> g_bleedRejectedSpeakerIds{};
		int g_bleedDialogueRetryCount = 0;
		static constexpr double kDefeatedEnemyKnockSeconds = 30.0;
		static constexpr double kDefeatedReentrySuppressSeconds = 6.0;
		std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_defeatedReentrySuppress{};
		RE::ObjectRefHandle g_pendingDefeatedDialogueTarget{};
		std::chrono::steady_clock::time_point g_pendingDefeatedDialogueExpiry{};
		constexpr double kPendingDefeatedDialogueTargetSeconds = 20.0;
		constexpr const char* kDefeatedHumanoidRecruitEvent = "TFDDefeatedHumanoidRecruit";
		constexpr const char* kHumanoidTeammateAssignEvent = "TFDHumanoidTeammateAssign";

		enum class NoMarkerFallbackBranch : std::uint32_t
		{
			None = 0,
			RecoveryFollower = 1,
			RecoveryPotion = 2,
			RescueCached = 3,
			LeftForDeadSolo = 4,
			LeftForDeadWithFollower = 5
		};

		struct NoMarkerFallbackState
		{
			NoMarkerFallbackBranch branch{ NoMarkerFallbackBranch::None };
			RE::ActorHandle follower{};
			RE::ObjectRefHandle destination{};
			RE::NiPoint3 fallbackPos{};
			bool hasFallbackPos{ false };
			float angleZ{ 0.0f };
			RE::FormID potionFormId{ 0 };
		};

		NoMarkerFallbackState g_noMarkerFallback{};
		RE::ActorHandle g_allyHoldFollower{};
		bool g_allyHoldActive = false;
		std::vector<RE::FormID> g_lockedFallbackCrowdIds{};
		std::unordered_set<RE::FormID> g_bleedFollowerDownIds{};

		RE::ObjectRefHandle g_lockpickDoorCandidate{};
		bool g_lockpickDoorWasLocked = false;
		TFD::CaptiveDoorController& g_captiveDoor = TFD::CaptiveRuntime::DoorControllerRef();
		RE::ObjectRefHandle& g_captiveMarker = TFD::CaptiveRuntime::MarkerRef();
		RE::FormID& g_captiveCellFormID = TFD::CaptiveRuntime::CellFormIDRef();
		RE::FormID& g_captiveLocationFormID = TFD::CaptiveRuntime::LocationFormIDRef();
		bool g_escapeRadiusActive = false;
		std::chrono::steady_clock::time_point g_escapeRadiusSince{};
		RE::ObjectRefHandle g_boundEscapeDoor{};

		static constexpr double kCaptiveEscapeDoorRadius = 512.0;
		static constexpr std::size_t kBleedBridgeMaxActors = 10;

		bool g_hasQueuedProgressState = false;
		bool& g_queuedCaptiveState = TFD::CaptiveRuntime::QueuedStateRef();
		CaptivePhaseValue& g_queuedCaptivePhase = TFD::CaptiveRuntime::QueuedPhaseRef();
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
			if (IsActiveFollowerActor(actor) || TFD::Pacify::IsCompanion(actor)) {
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
		static void ClearNoMarkerFallbackState();
		static void MaintainFollowerHold();
		static void MaintainTransitionCalmWindow();
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
		static void RecoverVictoryTeammates();
		static void ReleaseBleedLock(RE::Actor* actor, const char* reason, bool playGetUp);
		static void EnterObservedLeftForDead(const char* reason);
		static void RestoreFollowerAfterTransition(RE::Actor* actor);
		static void ResolveTeammateRegistry();
		static bool IsRegisteredTeammateActor(RE::Actor* actor);
		static std::vector<RE::Actor*> CollectRegisteredTeammates();
		static bool IsActorBleedingOut(RE::Actor* actor);
		static std::vector<RE::Actor*> CollectKnownTeammates(float radius);
		static RE::Actor* ResolveCurrentCombatTarget(RE::Actor* actor);
		static bool TryAbortPleasureDueToHostileIntrusion(float radius);
		static void ClearPendingCinematicFadeIn();
		static void ClearEscapeContext();
		static void QueueNonCaptiveChoiceRequest(const char* reason);
		static void ResolveLeftForDeadDestination(NoMarkerFallbackState& state);
		static void ClampHealth(RE::Actor* actor, float minHp);
		static bool ComputePlayerBleedOutState(RE::Actor* player);
		static void ResolveMonitorGlobals();
		static void RefreshPostDefeatGlobals();
		static void SetRescueStateValue(int value);

		static void QueuePostRecoveryAggroKick(const char* reason)
		{
			auto* player = Player();
			if (!player) {
				return;
			}

			auto* pCell = player->GetParentCell();
			if (!pCell) {
				return;
			}

			const float radius = (std::max)(2200.0f, TFD::Settings::GetSweepRadius() + 400.0f);
			RE::Actor* primary = ResolveAggressor();
			if (!primary) {
				primary = FindBestAggressor(radius);
			}

			std::unordered_set<RE::FormID> queuedIds;
			std::size_t queued = 0;

			auto queueOne = [&](RE::Actor* actor, bool drawWeapon) {
				if (!actor || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
					return;
				}
				if (actor->GetFormID() == player->GetFormID()) {
					return;
				}
				if (actor->GetParentCell() != pCell) {
					return;
				}
				if (!IsCombatSupportedAggressor(actor)) {
					return;
				}

				const auto [_, inserted] = queuedIds.insert(actor->GetFormID());
				if (!inserted) {
					return;
				}

				const bool kickedNow = TFD::Pacify::ForceDetectionAndCombatRefresh(
					actor,
					player,
					TFD::Pacify::ReleaseReason::DialogueClosed,
					drawWeapon);
				TFD::Pacify::QueueDetectionAndCombatRefresh(
					actor,
					player,
					TFD::Pacify::ReleaseReason::DialogueClosed,
					drawWeapon);
				++queued;

				spdlog::info(
					"[TFD][Defeat] post-recovery aggro kick actor={:08X} drawWeapon={} immediate={} reason={}",
					actor->GetFormID(),
					drawWeapon ? 1 : 0,
					kickedNow ? 1 : 0,
					reason ? reason : "unknown");
				};

			if (primary) {
				queueOne(primary, true);
			}

			TFD::ActorScan::Rescan(radius, false);
			const auto n = TFD::ActorScan::GetCount();
			for (int i = 0; i < n; ++i) {
				auto entry = TFD::ActorScan::GetEntry(i);
				auto sp = entry.actor.get();
				auto* actor = sp.get();
				if (!actor || actor == primary) {
					continue;
				}
				if (entry.dist > radius) {
					continue;
				}
				if (!entry.hostile && !entry.inCombat && !actor->IsInCombat() && !actor->IsHostileToActor(player)) {
					continue;
				}

				queueOne(actor, true);

				if (queued >= 8) {
					break;
				}
			}

			if (queued > 0) {
				player->EvaluatePackage(false, true);
				player->EvaluatePackage(true, true);
				player->UpdateCombat();
			}

			spdlog::info(
				"[TFD][Defeat] post-recovery aggro kick queued={} primary={:08X} reason={}",
				queued,
				primary ? primary->GetFormID() : 0u,
				reason ? reason : "unknown");
		}

		static void FinishLeftForDeadRecovery()
		{
			RE::Actor* followerToRestore = nullptr;
			if (g_allyHoldFollower) {
				auto sp = RE::Actor::LookupByHandle(g_allyHoldFollower.native_handle());
				followerToRestore = sp.get();
			}
			else if (g_noMarkerFallback.follower) {
				auto sp = RE::Actor::LookupByHandle(g_noMarkerFallback.follower.native_handle());
				followerToRestore = sp.get();
			}
			TFD::AggressionClamp::Clear();
			TFD::FactionMask::Clear();
			if (g_leftForDeadNeedsAggroKick) {
				QueuePostRecoveryAggroKick("left_for_dead_recovery_finished");
			}
			if (followerToRestore) {
				RestoreFollowerAfterTransition(followerToRestore);
			}
			g_leftForDeadNeedsAggroKick = false;
			g_leftForDeadActive = false;
			g_leftForDeadUntil = {};
			g_leftForDeadNextPulse = {};
			g_allyHoldFollower.reset();
			g_allyHoldActive = false;
			g_lockedFallbackCrowdIds.clear();
			g_noMarkerFallback = {};
			g_bleedFollowerDownIds.clear();
			spdlog::info("[TFD][Defeat] Transition recovery finished");
		}

		static bool IsLeftForDeadCooldownActive()
		{
			if (!g_leftForDeadActive) {
				return false;
			}
			const auto now = Now();
			if (now >= g_leftForDeadUntil) {
				if (TFD::PleasureRuntime::IsPassiveLockActive()) {
					g_leftForDeadUntil = now + std::chrono::seconds(1);
					if (g_leftForDeadPleasureDeferLast.time_since_epoch().count() == 0 ||
						(now - g_leftForDeadPleasureDeferLast) >= std::chrono::seconds(2)) {
						g_leftForDeadPleasureDeferLast = now;
						spdlog::info("[TFD][Defeat] transition recovery deferred by pleasure lock phase={} active={} blocking={}",
							TFD::PleasureRuntime::GetPhaseName(),
							TFD::PleasureRuntime::IsActive() ? 1 : 0,
							TFD::PleasureRuntime::IsBlocking() ? 1 : 0);
					}
					return true;
				}
				FinishLeftForDeadRecovery();
				return false;
			}
			return true;
		}

		static void ClearLeftForDeadCooldown()
		{
			if (g_leftForDeadActive) {
				FinishLeftForDeadRecovery();
				return;
			}
			g_leftForDeadActive = false;
			g_leftForDeadUntil = {};
			g_leftForDeadNextPulse = {};
			g_leftForDeadNeedsAggroKick = false;
			ClearNoMarkerFallbackState();
			SetRescueStateValue(0);
			RefreshPostDefeatGlobals();
		}

		static void BeginLeftForDeadCooldown(int seconds)
		{
			if (seconds <= 0) {
				ClearLeftForDeadCooldown();
				return;
			}
			const auto now = Now();
			g_leftForDeadActive = true;
			g_leftForDeadUntil = now + std::chrono::seconds(seconds);
			g_leftForDeadNextPulse = now;
			g_leftForDeadPleasureDeferLast = {};
		}

		static void ShowBlackoutFader()
		{
			auto* queue = RE::UIMessageQueue::GetSingleton();
			auto* strings = RE::InterfaceStrings::GetSingleton();
			if (!queue || !strings) {
				return;
			}
			queue->AddMessage(strings->faderMenu, RE::UI_MESSAGE_TYPE::kShow, nullptr);
			queue->ProcessCommands();
		}

		static void HideBlackoutFader()
		{
			auto* queue = RE::UIMessageQueue::GetSingleton();
			auto* strings = RE::InterfaceStrings::GetSingleton();
			if (!queue || !strings) {
				return;
			}
			queue->AddMessage(strings->faderMenu, RE::UI_MESSAGE_TYPE::kHide, nullptr);
			queue->ProcessCommands();
		}

		static bool SendBridgeModEvent(const char* eventName, RE::TESForm* sender = nullptr, const char* strArg = "", float numArg = 0.0f)
		{
			if (!eventName || !eventName[0]) {
				return false;
			}

			auto* task = SKSE::GetTaskInterface();
			if (!task) {
				spdlog::warn("[TFD][BleedBridge] SendBridgeModEvent failed: no task interface event={}", eventName);
				return false;
			}

			const std::string name{ eventName };
			const std::string sarg{ strArg ? strArg : "" };
			const float narg = numArg;

			std::uint32_t actorHandle = 0;
			RE::FormID senderFormID = 0;

			if (sender) {
				senderFormID = sender->GetFormID();
				if (auto* actor = sender->As<RE::Actor>()) {
					actorHandle = actor->GetHandle().native_handle();
				}
			}

			task->AddTask([name, sarg, narg, actorHandle, senderFormID]() {
				RE::TESForm* outSender = nullptr;

				if (actorHandle != 0) {
					auto actorSp = RE::Actor::LookupByHandle(actorHandle);
					outSender = actorSp.get();
					if (!outSender && senderFormID != 0) {
						outSender = RE::TESForm::LookupByID(senderFormID);
					}
				}
				else if (senderFormID != 0) {
					outSender = RE::TESForm::LookupByID(senderFormID);
				}

				auto* src = SKSE::GetModCallbackEventSource();
				if (!src) {
					spdlog::warn("[TFD][BleedBridge] Dispatch skipped: no callback source event={} sender={:08X}", name, senderFormID);
					return;
				}

				SKSE::ModCallbackEvent ev{ name.c_str(), sarg.c_str(), narg, outSender };
				src->SendEvent(&ev);

				spdlog::info("[TFD][BleedBridge] Dispatch event={} sender={:08X} resolved={:08X}",
					name,
					senderFormID,
					outSender ? outSender->GetFormID() : 0u);
				});

			return true;
		}

		static bool IsActorCloseAndFront(RE::Actor* actor, RE::Actor* player, float maxDist);
		static bool IsBleedSpaceCompatible(RE::Actor* actor, RE::Actor* player);

		static void SendPlayerSaviorAssign(RE::Actor* actor)
		{
			if (!actor) {
				return;
			}
			const bool queued = SendBridgeModEvent("TFDPlayerSaviorAssign", actor);
			spdlog::info("[TFD][SaviorBridge] Assign actor={:08X} queued={}", actor->GetFormID(), queued);
		}

		static void ClearPlayerSavior(RE::TESForm* sender = nullptr, const char* reason = nullptr)
		{
			const bool queued = SendBridgeModEvent("TFDPlayerSaviorClear", sender);
			spdlog::info("[TFD][SaviorBridge] Clear queued={} reason={}", queued, reason ? reason : "unknown");
		}

		static void ClearBleedSupportBridgeAliases(const char* reason);
		static void ReleaseBleedTruceSession(TFD::Pacify::ReleaseReason reason);
		static void ReleaseBleedNoSpeakerTameSession(const char* reason);
		static bool TryEnsureBleedNoSpeakerTameSession(const std::vector<RE::Actor*>& actors, const char* reason);

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

		static void ClearBleedSupportBridgeAliases(const char* reason)
		{
			const bool preCombatQueued = SendBridgeModEvent("TFDPreCombatClearAll", nullptr);
			const bool truceQueued = SendBridgeModEvent("TFDTruceClearAll", nullptr);
			const bool inCombatQueued = SendBridgeModEvent("TFDInCombatClearAll", nullptr);
			TFD::PreCombatGreet::CancelAll();
			spdlog::info("[TFD][BleedBridge] ClearSupport reason={} preCombatQueued={} truceQueued={} inCombatQueued={}",
				reason ? reason : "unknown",
				preCombatQueued ? 1 : 0,
				truceQueued ? 1 : 0,
				inCombatQueued ? 1 : 0);
		}

		static void ReleaseBleedTruceSession(TFD::Pacify::ReleaseReason reason)
		{
			if (g_bleedTruceSessionId != 0) {
				TFD::Pacify::ReleaseSession(g_bleedTruceSessionId, reason);
				spdlog::info("[TFD][Defeat] bleed truce session released id={} reason={}",
					g_bleedTruceSessionId,
					TFD::Pacify::ToString(reason));
				g_bleedTruceSessionId = 0;
			}
			g_bleedSpeakerId = 0;
		}

		static void ReleaseBleedNoSpeakerTameSession(const char* reason)
		{
			if (g_bleedNoSpeakerTameSessionId != 0) {
				TFD::Pacify::ReleaseSession(g_bleedNoSpeakerTameSessionId, TFD::Pacify::ReleaseReason::Generic);
				spdlog::info("[TFD][Defeat] bleed no-speaker tame session released id={} primary={:08X} reason={}",
					g_bleedNoSpeakerTameSessionId,
					g_bleedNoSpeakerTamePrimaryId,
					reason ? reason : "unknown");
				g_bleedNoSpeakerTameSessionId = 0;
			}
			g_bleedNoSpeakerTamePrimaryId = 0;
			g_bleedNoSpeakerTameLastAttempt = {};
		}

		static RE::Actor* ChooseBleedNoSpeakerTamePrimary(const std::vector<RE::Actor*>& actors)
		{
			auto* player = Player();
			if (!player) {
				return nullptr;
			}

			RE::Actor* best = nullptr;
			float bestScore = std::numeric_limits<float>::max();
			for (auto* actor : actors) {
				if (!actor || actor->IsDead() || actor->IsDisabled()) {
					continue;
				}
				if (IsCaptiveSupportedAggressor(actor)) {
					continue;
				}
				const auto pp = player->GetPosition();
				const auto ap = actor->GetPosition();
				const float dx = ap.x - pp.x;
				const float dy = ap.y - pp.y;
				const float dz = ap.z - pp.z;
				const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

				float score = dist;
				if (actor->IsHostileToActor(player)) score -= 120.0f;
				if (actor->IsInCombat()) score -= 80.0f;
				if (score < bestScore) {
					bestScore = score;
					best = actor;
				}
			}

			return best;
		}

		static bool TryEnsureBleedNoSpeakerTameSession(const std::vector<RE::Actor*>& actors, const char* reason)
		{
			auto* player = Player();
			if (!player) {
				return false;
			}

			bool hasAny = false;
			for (auto* actor : actors) {
				if (!actor || actor->IsDead() || actor->IsDisabled()) {
					continue;
				}
				hasAny = true;
				if (IsCaptiveSupportedAggressor(actor)) {
					return false;
				}
			}
			if (!hasAny) {
				return false;
			}

			auto* primary = ChooseBleedNoSpeakerTamePrimary(actors);
			if (!primary) {
				return false;
			}

			if (g_bleedNoSpeakerTamePrimaryId == primary->GetFormID() &&
				TFD::Pacify::IsPacified(primary) &&
				TFD::Pacify::GetMode(primary) == TFD::Pacify::Mode::Tame) {
				return true;
			}

			if (g_bleedNoSpeakerTameSessionId != 0) {
				ReleaseBleedNoSpeakerTameSession("restart");
			}

			player->DrawWeaponMagicHands(false);
			auto sessionId = TFD::Pacify::BeginTameSession(player, primary, 0.0, false);
			if (!sessionId.has_value()) {
				spdlog::info("[TFD][Defeat] bleed no-speaker tame session rejected primary={:08X} reason={}",
					primary->GetFormID(),
					reason ? reason : "unknown");
				return false;
			}

			g_bleedNoSpeakerTameSessionId = *sessionId;
			g_bleedNoSpeakerTamePrimaryId = primary->GetFormID();
			spdlog::info("[TFD][Defeat] bleed no-speaker tame session id={} primary={:08X} crowdSize={} reason={}",
				g_bleedNoSpeakerTameSessionId,
				g_bleedNoSpeakerTamePrimaryId,
				actors.size(),
				reason ? reason : "unknown");
			return true;
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
				return std::find(g_bleedCrowdAssigned.begin(), g_bleedCrowdAssigned.end(), actor->GetFormID()) != g_bleedCrowdAssigned.end();
			};
			return handlers;
		}

		static std::vector<RE::Actor*> CollectBleedoutCrowd(float radius, RE::Actor* preferred, bool preserveAssigned = false)
		{
			return TFD::Bleedout::CollectCrowd(radius, preferred, preserveAssigned, BuildBleedoutSpeakerHandlers(preserveAssigned));
		}

		static void ReleasePlayerBleedLock(const char* reason, bool playGetUp);

		static const char* BleedTerminalCommitName(BleedTerminalCommit kind)
		{
			return TFD::Bleedout::GetTerminalCommitName(kind);
		}

		static BleedTerminalCommit GetBleedTerminalCommit()
		{
			return TFD::Bleedout::GetTerminalCommit();
		}

		static bool HasBleedTerminalCommit()
		{
			return TFD::Bleedout::HasTerminalCommit();
		}

		static bool TryBeginBleedTerminalCommit(BleedTerminalCommit kind, const char* reason)
		{
			return TFD::Bleedout::TryBeginTerminalCommit(kind, reason);
		}

		static void ClearBleedTerminalCommit(const char* reason)
		{
			TFD::Bleedout::ClearTerminalCommit(reason);
		}

		static void ArmBleedSystemEventOutcomeWindow(const char* reason, double seconds = 2.5);
		static void ClearBleedSystemEventOutcomeWindow(const char* reason);
		static bool IsBleedSystemEventPendingForFallback(const char** outReason = nullptr);
		static bool ResolveBleedPendingSystemEventFallback(RE::Actor* player, const char* reason);
		static bool ResolveBleedPostDialogueSystemEventOutcome(RE::Actor* player, const char* reason);
		static bool ShouldDropBleedSystemEventBecauseFallback(const char* eventName);
		static void ArmBleedSystemEventOutcomeWindow(const char* reason, double seconds)
		{
			TFD::Bleedout::ArmSystemEventOutcomeWindow(reason, seconds);
		}

		static void ClearBleedSystemEventOutcomeWindow(const char* reason)
		{
			TFD::Bleedout::ClearSystemEventOutcomeWindow(reason);
		}

		static bool IsBleedSystemEventPendingForFallback(const char** outReason)
		{
			return TFD::Bleedout::IsSystemEventPendingForFallback(outReason);
		}

		static void ResetBleedRuntimeState(bool preserveCaptive = false)
		{
			TFD::Bleedout::RuntimeResetHandlers handlers{};
			handlers.releasePlayerBleedLock = [&](const char* reason) { ReleasePlayerBleedLock(reason, false); };
			handlers.releaseBleedTruceSession = [&]() { ReleaseBleedTruceSession(TFD::Pacify::ReleaseReason::Generic); };
			handlers.releaseNoSpeakerTameSession = [&](const char* reason) { ReleaseBleedNoSpeakerTameSession(reason); };
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
				g_bleedSpeakerKickLast = {};
				g_bleedSpeakerKickCount = 0;
				g_bleedDialogueRetryCount = 0;
				g_bleedLastCalmPulse = {};
				g_bleedLastCrowdAssign = {};
				g_bleedCrowdAssigned.clear();
				g_bleedRejectedSpeakerIds.clear();
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
			handlers.clearEscapeBreakState = [&]() {
				g_escapeBreakBleedPending = false;
				g_escapeBreakPreferredAggressor.reset();
			};
			handlers.clearLastEnemyTargetingPlayer = [&]() { ClearLastEnemyTargetingPlayerInternal(); };
			handlers.clearOutcomeWindow = [&](const char* reason) { ClearBleedSystemEventOutcomeWindow(reason); };
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

			TFD::ActorScan::Rescan(radius, false);
			const auto count = TFD::ActorScan::GetCount();
			for (int i = 0; i < count; ++i) {
				auto entry = TFD::ActorScan::GetEntry(i);
				auto actorSP = entry.actor.get();
				auto* actor = actorSP.get();
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

				const bool hostileToPlayer = entry.hostile || actor->IsHostileToActor(player);
				const bool hostileToSpeaker = actor->IsHostileToActor(speaker);
				const bool inCombat = entry.inCombat || actor->IsInCombat();
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
				TFD::Flow::Controller::GetSingleton().ResetRuntime("hostile_intrusion");
				ResetBleedRuntimeState();
				TFD::AggressionClamp::Clear();
				TFD::FactionMask::Clear();
				ClearPendingDefeatedDialogueTargetInternal();
				return true;
			}

			return false;
		}

		static void TransitionBleedRuntimeToPleasureCommit(const char* reason)
		{
			TFD::Bleedout::TransitionRuntimeToPleasureCommit(
				reason,
				static_cast<std::uint32_t>(g_bleedSpeakerId),
				g_bleedTruceSessionId != 0,
				static_cast<std::uint32_t>(TFD::Bleedout::GetActiveCaptorFormID()),
				TFD::Bleedout::RuntimePleasureCommitHandlers{
					[&](const char* why) { ReleaseBleedNoSpeakerTameSession(why); },
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
						g_bleedSpeakerKickLast = {};
						g_bleedSpeakerKickCount = 0;
						g_bleedDialogueRetryCount = 0;
						g_bleedLastCalmPulse = {};
						g_bleedLastCrowdAssign = {};
						g_bleedCrowdAssigned.clear();
						g_bleedRejectedSpeakerIds.clear();
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
					[&]() {
						g_escapeBreakBleedPending = false;
						g_escapeBreakPreferredAggressor.reset();
					},
					[&]() { ClearLastEnemyTargetingPlayerInternal(); },
					[&](const char* why) { ClearBleedSystemEventOutcomeWindow(why); }
				});
		}

		static void AdvanceGameHoursSoft(float hours)
		{
			if (hours <= 0.0f) {
				return;
			}
			auto* calendar = RE::Calendar::GetSingleton();
			if (!calendar) {
				return;
			}
			const float dayDelta = hours / 24.0f;
			calendar->rawDaysPassed += dayDelta;
			if (calendar->gameDaysPassed) {
				calendar->gameDaysPassed->value = calendar->rawDaysPassed;
			}
			if (calendar->gameHour) {
				float hour = std::fmod(calendar->gameHour->value + hours, 24.0f);
				if (hour < 0.0f) {
					hour += 24.0f;
				}
				calendar->gameHour->value = hour;
			}
		}

		static CaptivePhaseValue PhaseFromRaw(std::uint32_t raw)
		{
			return TFD::CaptiveRuntime::PhaseFromRaw(raw);
		}

		static RE::Actor* ResolveAggressor();
		static RE::Actor* FindBestAggressor(float radius);
		static RE::Actor* FindBestBleedoutSpeaker(float radius, float maxDist, RE::Actor* preferred = nullptr);
		static bool IsReasonableBleedoutSpeaker(RE::Actor* actor, RE::Actor* player, float maxDist, float* outDistance = nullptr);
		static void SetGraceSeconds(int seconds);
		static void RecoverPlayerForTransition();
		static void MaintainTransitionCalmWindow();
		static void UpdatePreCombatState();
		static bool IsActorCloseAndFront(RE::Actor* actor, RE::Actor* player, float maxDist);
		static bool CanUseAggressorForBleedoutGreet(RE::Actor* player, RE::Actor* aggressor, float& outDistance);
		static RE::TESObjectREFR* LookupRefByFormID(std::uint32_t formID);
		static RE::BGSLocation* GetLocationFromRef(RE::TESObjectREFR* ref);
		static RE::TESObjectREFR* ResolveBestRescueDestination(RE::BGSLocation* safeLoc);
		static bool BeginRescueTransition(const char* reason);
		static void BeginRecoverTransition(const char* reason);
		static void DoBlackoutTeleport();
		static void CompleteBleedPayRelease(const char* reason);
		static void PreparePlayerForBleedoutPleasureScene(const char* reason);
		static void PreparePlayerForCaptivePleasureScene(const char* reason);
		static void CompleteCaptivePleasureHandoff(const char* reason);
		static void CompleteBleedPleasureHandoff(const char* reason);
		static void StartBleedWindow(RE::Actor* player, RE::Actor* aggressor);
		static bool BeginBleedoutDialogueHotkey();
		static bool HandlePendingEscapeBreakBleed();
		static void MaintainBleedSpeakerKick();
		static void SetBleedDialogueOutcome(BleedDialogueOutcome outcome, const char* reason);
		static void ClearBleedDialogueOutcome(const char* reason);
		static RE::Actor* ResolveActorFromEventArg(const std::string_view& arg);
		static bool IsTransitionAwaiting();
		enum class CinematicTransitionKind
		{
			None = 0,
			Captive = 1,
			Rescue = 2,
			Recover = 3
		};

		static bool g_pendingCinematicFadeIn = false;
		static CinematicTransitionKind g_pendingCinematicFadeInKind = CinematicTransitionKind::None;
		static std::chrono::steady_clock::time_point g_pendingCinematicFadeInNotBefore{};
		static bool g_pendingCinematicFadeInSawLoadingMenu = false;

		static bool QueueCinematicTransitionRequest(CinematicTransitionKind kind, bool fadeIn, const char* reason);
		static void CompleteCaptiveTransitionNow(const char* reason);
		static bool CompleteRescueTransitionNow(const char* reason);
		static void CompleteRecoverTransitionNow(const char* reason);
		static bool TransferPlayerInventoryToCaptiveStorage(RE::TESObjectREFR* target, const char* reason);
		static void SyncCaptiveStorageDebugAliases(const char* reason);
		static void EnterNonCaptiveChoice(const char* reason);
		static bool BeginResolvedNoMarkerFallback(const char* reason);
		static void RecoverPlayerAfterTeleport();
		static void SetCaptiveRuntime(bool stateActive, CaptivePhaseValue phase);
		static bool IsDialogueOpen();
		static bool IsLockpickingOpen();
		static void ResetLockpickWatch();
		static void ArmEscapeContextFromCurrentState();
		static void ApplyCalmBubble(float radius);

		static const char* NoMarkerBranchName(NoMarkerFallbackBranch branch)
		{
			switch (branch) {
			case NoMarkerFallbackBranch::RecoveryFollower: return "recovery_follower";
			case NoMarkerFallbackBranch::RecoveryPotion: return "recovery_potion";
			case NoMarkerFallbackBranch::RescueCached: return "rescue_cached";
			case NoMarkerFallbackBranch::LeftForDeadSolo: return "left_for_dead_solo";
			case NoMarkerFallbackBranch::LeftForDeadWithFollower: return "left_for_dead_with_follower";
			default: return "none";
			}
		}

		static void ClearNoMarkerFallbackState()
		{
			g_noMarkerFallback = {};
			g_allyHoldFollower.reset();
			g_allyHoldActive = false;
			g_lockedFallbackCrowdIds.clear();
			g_bleedFollowerDownIds.clear();
			ClearPlayerSavior(nullptr, "clear_no_marker_fallback");
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

		static bool StartBleedTruceSessionForSpeaker(RE::Actor* player, RE::Actor* speaker, const char* reason)
		{
			if (!player || !speaker || speaker->IsDead() || speaker->IsDisabled()) {
				return false;
			}
			const auto speakerID = speaker->GetFormID();
			const bool sameActiveCaptor = TFD::Bleedout::GetActiveCaptorFormID() != 0 && TFD::Bleedout::GetActiveCaptorFormID() == speakerID;
			ClearBleedSupportBridgeAliases(reason ? reason : "bleed_retry");
			g_bleedSpeakerId = speakerID;
			g_lastAggressor = speaker->GetHandle();
			if (!sameActiveCaptor) {
				TFD::Bleedout::ClearBridgeAliases(speaker, reason ? reason : "bleed_retry");
			}
			else {
				spdlog::info("[TFD][BleedBridge] ClearAll skipped reason={} actor={:08X} sameActiveCaptor=1",
					reason ? reason : "bleed_retry",
					speakerID);
			}
			TFD::Bleedout::AssignBridgeActor(speaker);
			TFD::Bleedout::BindCaptorAliases(speaker, reason ? reason : "bleed_retry");
			TFD::Bleedout::PrimeBridgeActor(speaker, reason ? reason : "bleed_retry");
			if (!speaker->IsAIEnabled()) {
				speaker->EnableAI(true);
			}
			speaker->AllowPCDialogue(true);
			speaker->SetDialogueWithPlayer(false, false, nullptr);
			if (speaker->IsInCombat()) {
				speaker->StopCombat();
			}
			if (auto* process = RE::ProcessLists::GetSingleton()) {
				process->StopCombatAndAlarmOnActor(speaker, false);
			}
			if (speaker->IsWeaponDrawn()) {
				speaker->DrawWeaponMagicHands(false);
			}
			auto sessionId = TFD::Pacify::BeginTruceInCombatSession(player, speaker, 0.0, true, false, true);
			if (!sessionId.has_value() && g_inBleedState.load(std::memory_order_relaxed)) {
				spdlog::info("[TFD][Defeat] bleed speaker restart retry ignoreSpent actor={:08X} reason={}",
					speaker->GetFormID(),
					reason ? reason : "unknown");
				sessionId = TFD::Pacify::BeginTruceInCombatSession(player, speaker, 0.0, true, true, true);
			}
			if (!sessionId.has_value()) {
				spdlog::warn("[TFD][Defeat] bleed speaker restart rejected actor={:08X} reason={}",
					speaker->GetFormID(),
					reason ? reason : "unknown");
				return false;
			}
			g_bleedTruceSessionId = *sessionId;
			ApplyBleedForceGreetOverdrive(player, speaker, reason ? reason : "bleed_retry", false);
			speaker->EvaluatePackage(false, true);
			speaker->EvaluatePackage(true, true);
			spdlog::info("[TFD][Defeat] bleed speaker restart id={} actor={:08X} reason={}",
				g_bleedTruceSessionId,
				speaker->GetFormID(),
				reason ? reason : "unknown");
			return true;
		}

		static void ApplyBleedForceGreetOverdrive(RE::Actor* player, RE::Actor* speaker, const char* reason, bool restartForceGreet)
		{
			if (!player || !speaker || speaker->IsDead() || speaker->IsDisabled()) {
				return;
			}

			if (!speaker->IsAIEnabled()) {
				speaker->EnableAI(true);
			}
			speaker->AllowPCDialogue(true);
			speaker->SetDialogueWithPlayer(false, false, nullptr);

			if (speaker->IsInCombat()) {
				speaker->StopCombat();
			}
			if (auto* process = RE::ProcessLists::GetSingleton()) {
				process->StopCombatAndAlarmOnActor(speaker, false);
			}
			if (speaker->IsWeaponDrawn()) {
				speaker->DrawWeaponMagicHands(false);
			}

			if (!TFD::Bleedout::IsCaptorAliasPrimary(speaker)) {
				TFD::Bleedout::BindCaptorAliases(speaker, reason ? reason : "bleed_forcegreet_overdrive");
			}
			TFD::Bleedout::PrimeBridgeActor(speaker, reason ? reason : "bleed_forcegreet_overdrive");

			// Bleedout forcegreet must stay speaker-only.
			// Do not assign support crowd, do not broadcast truce/in-combat bridge events,
			// and do not refresh bleed crowd aliases while the dialogue handshake is happening.
			g_bleedCrowdAssigned.clear();
			g_bleedLastCrowdAssign = Now();

			std::optional<RE::FormID> burstId;
			const bool preserveDialogueSession = g_inBleedState.load(std::memory_order_relaxed);
			if (!preserveDialogueSession) {
				burstId = TFD::Pacify::BeginCellTruceBurst(player, speaker, 0.0, 2.5, 12000.0f);
			}
			else {
				spdlog::info("[TFD][Defeat] bleed forcegreet preserve dialogue session speaker={:08X} reason={}",
					speaker->GetFormID(),
					reason ? reason : "unknown");
			}

			player->DrawWeaponMagicHands(false);
			player->EvaluatePackage(false, true);
			player->EvaluatePackage(true, true);
			speaker->EvaluatePackage(false, true);
			speaker->EvaluatePackage(true, true);

			if (restartForceGreet) {
				TFD::BleedoutGreet::Begin(speaker, reason ? reason : "bleed_forcegreet_overdrive");
			}

			spdlog::info("[TFD][Defeat] bleed forcegreet overdrive speaker={:08X} crowdSize=1 burst={} restartFG={} reason={}",
				speaker->GetFormID(),
				burstId.has_value() ? 1 : 0,
				restartForceGreet ? 1 : 0,
				reason ? reason : "unknown");
		}

		static bool PromoteNextBleedSpeakerFromTruceQueue(RE::Actor* player, const char* reason, bool rejectCurrent)
		{
			ResolveTruceQuestRegistry();
			if (!player) {
				player = Player();
			}
			if (!player || !g_truceQuestRegistry.quest) {
				return false;
			}

			if (rejectCurrent && g_bleedSpeakerId != 0) {
				g_bleedRejectedSpeakerIds.insert(g_bleedSpeakerId);
			}

			for (auto* alias : g_truceQuestRegistry.truceAliases) {
				if (!alias) {
					continue;
				}
				auto* actor = alias->GetActorReference();
				if (!actor || actor == player || actor->IsDead() || actor->IsDisabled()) {
					continue;
				}
				const auto actorID = actor->GetFormID();
				if (g_bleedRejectedSpeakerIds.find(actorID) != g_bleedRejectedSpeakerIds.end()) {
					continue;
				}
				if (!IsBleedCrowdSupportedAggressor(actor) || !IsBleedSpaceCompatible(actor, player)) {
					continue;
				}
				if (StartBleedTruceSessionForSpeaker(player, actor, reason ? reason : "truce_queue_promote")) {
					TFD::BleedoutGreet::ResetRuntime("bleed_reset");
					g_prevDialogueOpen = false;
					g_bleedPaused = false;
					g_bleedPauseStarted = {};
					g_bleedSpeakerKickLast = {};
					g_bleedSpeakerKickCount = 0;
					g_bleedLastSeconds = -1;
					g_bleedStart = Now();
					spdlog::info("[TFD][Defeat] promoted next bleed speaker actor={:08X} reason={}",
						actorID,
						reason ? reason : "unknown");
					return true;
				}
				g_bleedRejectedSpeakerIds.insert(actorID);
			}

			return false;
		}

		static void MaintainBleedPrimaryCaptorBinding()
		{
			if (!g_inBleedState.load(std::memory_order_acquire) || g_bleedSpeakerId == 0 || g_bleedTruceSessionId == 0) {
				return;
			}
			if (IsDialogueOpen()) {
				return;
			}
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(g_bleedSpeakerId);
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				return;
			}
			if (TFD::Bleedout::IsCaptorAliasPrimary(actor)) {
				return;
			}
			const auto now = Now();
			if (TFD::Bleedout::WasCaptorRecentlyBound(now, std::chrono::milliseconds(1500))) {
				return;
			}
			TFD::Bleedout::BindCaptorAliases(actor, "maintain_primary_missing");
		}

		static void ResolveTruceQuestRegistry()
		{
			if (g_truceQuestRegistry.resolved) {
				return;
			}
			g_truceQuestRegistry.resolved = true;
			g_truceQuestRegistry.quest = RE::TESForm::LookupByEditorID<RE::TESQuest>("TFDTruceQuest");
			if (!g_truceQuestRegistry.quest) {
				spdlog::warn("[TFD][TruceQuest] truce quest not found");
				return;
			}

			for (auto* baseAlias : g_truceQuestRegistry.quest->aliases) {
				auto* refAlias = skyrim_cast<RE::BGSRefAlias*>(baseAlias);
				if (!refAlias) {
					continue;
				}
				const auto aliasName = std::string(refAlias->aliasName.c_str());
				std::size_t prefixLen = 0;
				if (aliasName.rfind("Crowd", 0) == 0 && aliasName.size() > 5) {
					prefixLen = 5;
				}
				else if (aliasName.rfind("Truce", 0) == 0 && aliasName.size() > 5) {
					prefixLen = 5;
				}
				else {
					continue;
				}
				try {
					int slot = std::stoi(aliasName.substr(prefixLen));
					if (slot >= 1 && slot <= static_cast<int>(g_truceQuestRegistry.truceAliases.size())) {
						g_truceQuestRegistry.truceAliases[static_cast<std::size_t>(slot) - 1] = refAlias;
					}
				}
				catch (...) {}
			}

			std::size_t found = 0;
			for (auto* alias : g_truceQuestRegistry.truceAliases) {
				if (alias) {
					++found;
				}
			}
			spdlog::info("[TFD][TruceQuest] truce quest resolved quest={:08X} truceAliases={}",
				g_truceQuestRegistry.quest ? g_truceQuestRegistry.quest->GetFormID() : 0u,
				found);
		}

		static void ResolveTeammateRegistry()
		{
			if (g_teammateRegistry.resolved) {
				return;
			}
			g_teammateRegistry.resolved = true;
			g_teammateRegistry.quest = RE::TESForm::LookupByEditorID<RE::TESQuest>("TFDPlayerTeammateQuest");
			g_teammateRegistry.currentFollowerFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>("CurrentFollowerFaction");
			g_teammateRegistry.playerFollowerFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>("PlayerFollowerFaction");
			if (!g_teammateRegistry.quest) {
				spdlog::warn("[TFD][Defeat] teammate registry quest not found");
				return;
			}
			for (auto* baseAlias : g_teammateRegistry.quest->aliases) {
				auto* refAlias = skyrim_cast<RE::BGSRefAlias*>(baseAlias);
				if (!refAlias) {
					continue;
				}
				const auto aliasName = std::string(refAlias->aliasName.c_str());
				if (aliasName.rfind("Teammate", 0) != 0 || aliasName.size() < 10) {
					continue;
				}
				try {
					int slot = std::stoi(aliasName.substr(9));
					if (slot >= 1 && slot <= 10) {
						g_teammateRegistry.teammateAliases[static_cast<std::array<RE::BGSRefAlias*, 10Ui64>::size_type>(slot) - 1] = refAlias;
					}
				}
				catch (...) {
				}
			}
			std::size_t found = 0;
			for (auto* a : g_teammateRegistry.teammateAliases) {
				if (a) {
					++found;
				}
			}
			spdlog::info("[TFD][Defeat] teammate registry resolved quest={:08X} aliases={} currentFollowerFaction={:08X} playerFollowerFaction={:08X}",
				g_teammateRegistry.quest ? g_teammateRegistry.quest->GetFormID() : 0u,
				found,
				g_teammateRegistry.currentFollowerFaction ? g_teammateRegistry.currentFollowerFaction->GetFormID() : 0u,
				g_teammateRegistry.playerFollowerFaction ? g_teammateRegistry.playerFollowerFaction->GetFormID() : 0u);
		}

		static bool IsRegisteredTeammateActor(RE::Actor* actor)
		{
			if (!actor || actor->IsDisabled()) {
				return false;
			}
			ResolveTeammateRegistry();
			for (auto* alias : g_teammateRegistry.teammateAliases) {
				if (!alias) {
					continue;
				}
				auto* slotActor = alias->GetActorReference();
				if (slotActor && slotActor == actor) {
					return true;
				}
			}
			return false;
		}

		static std::vector<RE::Actor*> CollectRegisteredTeammates()
		{
			ResolveTeammateRegistry();
			std::vector<RE::Actor*> out;
			out.reserve(10);
			for (auto* alias : g_teammateRegistry.teammateAliases) {
				if (!alias) {
					continue;
				}
				auto* actor = alias->GetActorReference();
				if (!actor || actor->IsDisabled()) {
					continue;
				}
				out.push_back(actor);
			}
			return out;
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

		static void ResolveCaptiveQuestRegistry()
		{
			if (g_captiveQuestRegistry.resolved) {
				return;
			}
			g_captiveQuestRegistry.resolved = true;
			g_captiveQuestRegistry.quest = RE::TESForm::LookupByEditorID<RE::TESQuest>("TFDCaptiveQuest");
			if (!g_captiveQuestRegistry.quest) {
				spdlog::warn("[TFD][Captive] captive quest not found");
				return;
			}

			for (auto* baseAlias : g_captiveQuestRegistry.quest->aliases) {
				auto* refAlias = skyrim_cast<RE::BGSRefAlias*>(baseAlias);
				if (!refAlias) {
					continue;
				}
				const auto aliasName = std::string(refAlias->aliasName.c_str());
				if (aliasName == "PlayerCaptive") {
					g_captiveQuestRegistry.playerCaptiveAlias = refAlias;
					continue;
				}
				if (aliasName == "LootTarget") {
					g_captiveQuestRegistry.lootTargetAlias = refAlias;
					continue;
				}
				if (aliasName.rfind("BossCaptor", 0) == 0 && aliasName.size() >= 11) {
					try {
						int slot = std::stoi(aliasName.substr(10));
						if (slot >= 1 && slot <= static_cast<int>(g_captiveQuestRegistry.bossCaptorAliases.size())) {
							g_captiveQuestRegistry.bossCaptorAliases[static_cast<std::array<RE::BGSRefAlias*, 3Ui64>::size_type>(slot) - 1] = refAlias;
						}
					}
					catch (...) {}
					continue;
				}
				if (aliasName.rfind("BossContainer", 0) == 0 && aliasName.size() >= 14) {
					try {
						int slot = std::stoi(aliasName.substr(13));
						if (slot >= 1 && slot <= static_cast<int>(g_captiveQuestRegistry.bossContainerAliases.size())) {
							g_captiveQuestRegistry.bossContainerAliases[static_cast<std::array<RE::BGSRefAlias*, 3Ui64>::size_type>(slot) - 1] = refAlias;
						}
					}
					catch (...) {}
					continue;
				}
				if (aliasName.rfind("Container", 0) == 0 && aliasName.size() >= 10) {
					try {
						int slot = std::stoi(aliasName.substr(9));
						if (slot >= 1 && slot <= static_cast<int>(g_captiveQuestRegistry.containerAliases.size())) {
							g_captiveQuestRegistry.containerAliases[static_cast<std::array<RE::BGSRefAlias*, 3Ui64>::size_type>(slot) - 1] = refAlias;
						}
					}
					catch (...) {}
					continue;
				}
			}

			std::size_t bossCaptorCount = 0;
			std::size_t bossContainerCount = 0;
			std::size_t containerCount = 0;
			for (auto* alias : g_captiveQuestRegistry.bossCaptorAliases) {
				if (alias) {
					++bossCaptorCount;
				}
			}
			for (auto* alias : g_captiveQuestRegistry.bossContainerAliases) {
				if (alias) {
					++bossContainerCount;
				}
			}
			for (auto* alias : g_captiveQuestRegistry.containerAliases) {
				if (alias) {
					++containerCount;
				}
			}

			spdlog::info("[TFD][Captive] captive quest registry resolved quest={:08X} playerAliasID={} bossCaptorAliases={} bossContainerAliases={} containerAliases={} lootTarget={}",
				g_captiveQuestRegistry.quest ? g_captiveQuestRegistry.quest->GetFormID() : 0u,
				g_captiveQuestRegistry.playerCaptiveAlias ? g_captiveQuestRegistry.playerCaptiveAlias->aliasID : static_cast<std::uint32_t>(0),
				bossCaptorCount,
				bossContainerCount,
				containerCount,
				g_captiveQuestRegistry.lootTargetAlias ? 1 : 0);
		}

		static void WriteCaptiveQuestAlias(RE::BGSRefAlias* alias, RE::TESObjectREFR* ref)
		{
			ResolveCaptiveQuestRegistry();
			if (!g_captiveQuestRegistry.quest || !alias) {
				return;
			}

			RE::ObjectRefHandle handle{};
			if (ref) {
				handle = ref->CreateRefHandle();
			}

			RE::BSWriteLockGuard lock(g_captiveQuestRegistry.quest->aliasAccessLock);
			auto it = g_captiveQuestRegistry.quest->refAliasMap.find(alias->aliasID);
			if (ref) {
				if (it != g_captiveQuestRegistry.quest->refAliasMap.end()) {
					it->second = handle;
				}
				else {
					g_captiveQuestRegistry.quest->refAliasMap.insert({ alias->aliasID, handle });
				}
			}
			else {
				if (it != g_captiveQuestRegistry.quest->refAliasMap.end()) {
					g_captiveQuestRegistry.quest->refAliasMap.erase(it);
				}
			}
		}

		static void WritePlayerCaptiveAlias(RE::Actor* actor)
		{
			ResolveCaptiveQuestRegistry();
			WriteCaptiveQuestAlias(g_captiveQuestRegistry.playerCaptiveAlias, actor);
		}

		static void SyncPlayerCaptiveAlias(RE::Actor* actor, const char* reason)
		{
			ResolveCaptiveQuestRegistry();
			if (!g_captiveQuestRegistry.quest || !g_captiveQuestRegistry.playerCaptiveAlias) {
				spdlog::warn("[TFD][Captive] player captive alias unavailable reason={}", reason ? reason : "unknown");
				return;
			}

			WritePlayerCaptiveAlias(actor);
			spdlog::info("[TFD][Captive] PlayerCaptive alias {} actor={:08X} reason={}",
				actor ? "assigned" : "cleared",
				actor ? actor->GetFormID() : 0u,
				reason ? reason : "unknown");
		}

		static void SyncCaptiveStorageDebugAliases(const char* reason)
		{
			ResolveCaptiveQuestRegistry();
			if (!g_captiveQuestRegistry.quest) {
				return;
			}

			TFD::Location::CaptiveStorageDebugSnapshot snapshot{};
			const bool hasSnapshot = TFD::Location::GetLastCaptiveStorageDebugSnapshot(snapshot);

			auto assignByFormID = [&](RE::BGSRefAlias* alias, std::uint32_t formID) {
				RE::TESObjectREFR* ref = nullptr;
				if (formID != 0) {
					ref = LookupRefByFormID(formID);
				}
				WriteCaptiveQuestAlias(alias, ref);
				};

			for (std::size_t i = 0; i < g_captiveQuestRegistry.bossCaptorAliases.size(); ++i) {
				assignByFormID(g_captiveQuestRegistry.bossCaptorAliases[i], hasSnapshot ? snapshot.bossActorFormIDs[i] : 0u);
			}
			for (std::size_t i = 0; i < g_captiveQuestRegistry.bossContainerAliases.size(); ++i) {
				assignByFormID(g_captiveQuestRegistry.bossContainerAliases[i], hasSnapshot ? snapshot.bossContainerFormIDs[i] : 0u);
			}
			for (std::size_t i = 0; i < g_captiveQuestRegistry.containerAliases.size(); ++i) {
				assignByFormID(g_captiveQuestRegistry.containerAliases[i], hasSnapshot ? snapshot.containerFormIDs[i] : 0u);
			}
			assignByFormID(g_captiveQuestRegistry.lootTargetAlias, hasSnapshot ? snapshot.finalTargetFormID : 0u);

			spdlog::info("[TFD][Captive] debug aliases synced reason={} hasSnapshot={} boss1={:08X} bossContainer1={:08X} container1={:08X} lootTarget={:08X} targetKind={}",
				reason ? reason : "unknown",
				hasSnapshot ? 1 : 0,
				hasSnapshot ? snapshot.bossActorFormIDs[0] : 0u,
				hasSnapshot ? snapshot.bossContainerFormIDs[0] : 0u,
				hasSnapshot ? snapshot.containerFormIDs[0] : 0u,
				hasSnapshot ? snapshot.finalTargetFormID : 0u,
				hasSnapshot ? snapshot.finalTargetKind : 0u);
		}

		static void ClearCaptiveStorageDebugAliases(const char* reason)
		{
			ResolveCaptiveQuestRegistry();
			if (!g_captiveQuestRegistry.quest) {
				return;
			}

			for (auto* alias : g_captiveQuestRegistry.bossCaptorAliases) {
				WriteCaptiveQuestAlias(alias, nullptr);
			}
			for (auto* alias : g_captiveQuestRegistry.bossContainerAliases) {
				WriteCaptiveQuestAlias(alias, nullptr);
			}
			for (auto* alias : g_captiveQuestRegistry.containerAliases) {
				WriteCaptiveQuestAlias(alias, nullptr);
			}
			WriteCaptiveQuestAlias(g_captiveQuestRegistry.lootTargetAlias, nullptr);

			spdlog::info("[TFD][Captive] debug aliases cleared reason={}",
				reason ? reason : "unknown");
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
			if (TFD::Pacify::IsCompanion(actor) || TFD::Pacify::HasActiveTameSession(actor)) {
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
		static bool HasFollowerAnchorFaction(RE::Actor* actor)
		{
			if (!actor || actor->IsDisabled()) {
				return false;
			}
			ResolveTeammateRegistry();
			if (g_teammateRegistry.currentFollowerFaction && actor->IsInFaction(g_teammateRegistry.currentFollowerFaction)) {
				return true;
			}
			if (g_teammateRegistry.playerFollowerFaction && actor->IsInFaction(g_teammateRegistry.playerFollowerFaction)) {
				return true;
			}
			return false;
		}

		static std::vector<RE::Actor*> CollectKnownTeammates(float radius)
		{
			auto* player = Player();
			std::vector<RE::Actor*> out;
			if (!player) {
				return out;
			}

			std::unordered_set<RE::FormID> seen;
			const float maxRadius = radius > 0.0f ? (std::max)(radius, 5000.0f) : 5000.0f;

			for (auto* actor : CollectRegisteredTeammates()) {
				if (!actor || actor == player || actor->IsDisabled()) {
					continue;
				}
				if (radius > 0.0f) {
					auto apos = actor->GetPosition();
					auto ppos = player->GetPosition();
					const float dx = apos.x - ppos.x;
					const float dy = apos.y - ppos.y;
					const float dz = apos.z - ppos.z;
					const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
					if (dist > maxRadius) {
						continue;
					}
				}
				seen.insert(actor->GetFormID());
				out.push_back(actor);
			}

			TFD::ActorScan::Rescan(maxRadius, false);
			const auto count = TFD::ActorScan::GetCount();
			auto* pCell = player->GetParentCell();
			for (int i = 0; i < count; ++i) {
				auto entry = TFD::ActorScan::GetEntry(i);
				auto sp = entry.actor.get();
				auto* actor = sp.get();
				if (!actor || actor == player || actor->IsDisabled()) {
					continue;
				}
				if (actor->GetParentCell() != pCell) {
					continue;
				}
				if (entry.dist > maxRadius) {
					continue;
				}
				if (!actor->IsPlayerTeammate() && !HasFollowerAnchorFaction(actor)) {
					continue;
				}
				if (!seen.insert(actor->GetFormID()).second) {
					continue;
				}
				out.push_back(actor);
			}

			return out;
		}

		static bool IsActiveFollowerActor(RE::Actor* actor)
		{
			if (!actor || actor->IsDisabled()) {
				return false;
			}
			if (IsRegisteredTeammateActor(actor)) {
				return true;
			}
			if (actor->IsPlayerTeammate()) {
				return true;
			}
			return HasFollowerAnchorFaction(actor);
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
			return IsActiveFollowerActor(actor) || TFD::Pacify::IsCompanion(actor);
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

		static bool IsHumanoidSaviorCandidate(RE::Actor* actor)
		{
			if (!actor || !IsStandingAllyThresholdActor(actor)) {
				return false;
			}
			if (!IsActiveFollowerActor(actor)) {
				return false;
			}
			if (!ActorHasKeywordByEditorID(actor, "ActorTypeNPC")) {
				return false;
			}
			return true;
		}

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
				TFD::ActorScan::Rescan(radius, false);
				const auto count = TFD::ActorScan::GetCount();
				enemies.reserve(static_cast<std::size_t>(count));
				for (int i = 0; i < count; ++i) {
					auto entry = TFD::ActorScan::GetEntry(i);
					auto sp = entry.actor.get();
					auto* actor = sp.get();
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

		static RE::Actor* ResolveBestHumanoidSavior(float radius)
		{
			auto* player = Player();
			if (!player) {
				return nullptr;
			}

			RE::Actor* best = nullptr;
			float bestDist = std::numeric_limits<float>::max();

			for (auto* actor : CollectRegisteredTeammates()) {
				if (!IsHumanoidSaviorCandidate(actor) || actor == player) {
					continue;
				}

				const float dist = Distance3D(actor->GetPosition(), player->GetPosition());
				if (radius > 0.0f && dist > (std::max)(radius, 5000.0f)) {
					continue;
				}

				if (dist < bestDist) {
					bestDist = dist;
					best = actor;
				}
			}

			return best;
		}

		static void RestoreFollowerAfterTransition(RE::Actor* actor)
		{
			if (!actor || actor->IsDisabled()) {
				return;
			}
			actor->AllowPCDialogue(true);
			actor->SetDialogueWithPlayer(false, false, nullptr);
			if (!actor->IsDead()) {
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
			actor->EvaluatePackage(false, true);
			actor->EvaluatePackage(true, true);
		}

		static void ApplyFollowerHold(RE::Actor* actor)
		{
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				return;
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
			actor->EvaluatePackage(false, true);
			actor->EvaluatePackage(true, true);
		}

		static void MaintainFollowerHold()
		{
			if (!g_allyHoldActive || !g_allyHoldFollower) {
				return;
			}
			auto sp = RE::Actor::LookupByHandle(g_allyHoldFollower.native_handle());
			auto* actor = sp.get();
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				g_allyHoldFollower.reset();
				g_allyHoldActive = false;
				return;
			}
			ApplyFollowerHold(actor);
		}

		static void SetFollowerHold(RE::Actor* actor)
		{
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				g_allyHoldFollower.reset();
				g_allyHoldActive = false;
				return;
			}
			g_allyHoldFollower = actor->GetHandle();
			g_allyHoldActive = true;
			ApplyFollowerHold(actor);
		}

		static float Distance3D(const RE::NiPoint3& a, const RE::NiPoint3& b)
		{
			const float dx = a.x - b.x;
			const float dy = a.y - b.y;
			const float dz = a.z - b.z;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		}

		static float ComputeYawFromVector(float dx, float dy)
		{
			return std::atan2(dx, dy);
		}

		static RE::NiPoint3 ComputeCrowdCenterPoint()
		{
			RE::NiPoint3 center{};
			auto* player = Player();
			if (player) {
				center = player->GetPosition();
			}

			std::size_t count = 0;
			for (auto id : g_lockedFallbackCrowdIds) {
				auto* actorRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				auto* actor = actorRef ? actorRef->As<RE::Actor>() : nullptr;
				if (!actor || actor->IsDead() || actor->IsDisabled()) {
					continue;
				}
				const auto pos = actor->GetPosition();
				center.x += pos.x;
				center.y += pos.y;
				center.z += pos.z;
				++count;
			}

			if (count > 0) {
				const float denom = static_cast<float>(count + (player ? 1 : 0));
				center.x /= denom;
				center.y /= denom;
				center.z /= denom;
			}

			return center;
		}

		static float MinDistanceToLockedCrowd(const RE::NiPoint3& pos)
		{
			float best = std::numeric_limits<float>::max();
			for (auto id : g_lockedFallbackCrowdIds) {
				auto* actorRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				auto* actor = actorRef ? actorRef->As<RE::Actor>() : nullptr;
				if (!actor || actor->IsDead() || actor->IsDisabled()) {
					continue;
				}
				best = (std::min)(best, Distance3D(pos, actor->GetPosition()));
			}
			return best;
		}

		struct FollowerResolution
		{
			RE::Actor* standing{ nullptr };
			RE::Actor* downed{ nullptr };
		};

		static FollowerResolution ResolveFollowerCandidates(float radius)
		{
			FollowerResolution result{};
			auto* player = Player();
			if (!player) {
				return result;
			}

			float bestStandingDist = std::numeric_limits<float>::max();
			float bestDownedDist = std::numeric_limits<float>::max();

			for (auto* actor : CollectRegisteredTeammates()) {
				if (!actor || actor == player) {
					continue;
				}
				const float dist = Distance3D(actor->GetPosition(), player->GetPosition());
				if (radius > 0.0f && dist > (std::max)(radius, 5000.0f)) {
					continue;
				}
				const bool wasDownedThisEvent = g_bleedFollowerDownIds.find(actor->GetFormID()) != g_bleedFollowerDownIds.end();
				const bool downed = wasDownedThisEvent || IsActorDownByThreshold(actor, TFD::Settings::GetAllyDownedThresholdPct());
				if (!downed) {
					if (dist < bestStandingDist) {
						bestStandingDist = dist;
						result.standing = actor;
					}
				}
				else if (dist < bestDownedDist) {
					bestDownedDist = dist;
					result.downed = actor;
				}
			}

			return result;
		}

		static std::vector<RE::Actor*> CollectBleedStandingFollowers(float radius)
		{
			std::vector<RE::Actor*> out;
			for (auto* actor : CollectKnownTeammates(radius)) {
				if (!IsStandingAllyThresholdActor(actor)) {
					continue;
				}
				out.push_back(actor);
			}
			return out;
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
			if (TFD::Pacify::IsCompanion(actor)) {
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
			TFD::ActorScan::Rescan(scanRadius, false);
			const auto count = TFD::ActorScan::GetCount();
			for (int i = 0; i < count; ++i) {
				auto e = TFD::ActorScan::GetEntry(i);
				auto sp = e.actor.get();
				auto* actor = sp.get();
				if (!actor || actor->GetParentCell() != pCell || !actor->Is3DLoaded()) {
					continue;
				}
				if (IsLikelyObservedEnemySeed(actor, player, allies, seedRadius, e.hostile, e.inCombat)) {
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
			if (!IsActiveFollowerActor(actor) && !TFD::Pacify::IsCompanion(actor)) {
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
			TFD::Bleedout::ClearBridgeAliases(nullptr, "battle_observe_win");
			ClearPendingCinematicFadeIn();
			TFD::FactionMask::Clear();
			ClearEscapeContext();
			ResetLockpickWatch();
			g_grace.store(false, std::memory_order_release);
			g_lastAggressor.reset();
			g_bleedBattleObservePending = false;
			g_bleedBattleObservePending = false;
			RecoverVictoryTeammates();
			ResetBleedRuntimeState();
			g_prevDialogueOpen = false;
			g_prevLockpickOpen = false;
			ClearPendingDefeatedDialogueTargetInternal();
			SetCaptiveRuntime(false, CaptivePhaseValue::None);
			SetPlayerBleedImmune(false);
			QueueNonCaptiveChoiceRequest("battle_observe_win");
			spdlog::info("[TFD][Defeat] battle observe resolved -> non-captive choice");
		}

		static void EnterObservedLeftForDead(const char* reason)
		{
			const float followerRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 400.0f);
			auto followers = ResolveFollowerCandidates(followerRadius);
			g_noMarkerFallback = {};
			if (followers.downed) {
				g_noMarkerFallback.branch = NoMarkerFallbackBranch::LeftForDeadWithFollower;
				g_noMarkerFallback.follower = followers.downed->GetHandle();
			}
			else {
				g_noMarkerFallback.branch = NoMarkerFallbackBranch::LeftForDeadSolo;
			}
			ResolveLeftForDeadDestination(g_noMarkerFallback);

			TFD::Bleedout::ClearBridgeAliases(nullptr, reason ? reason : "battle_observe_loss");
			ClearPendingCinematicFadeIn();
			TFD::FactionMask::Clear();
			ClearEscapeContext();
			ResetLockpickWatch();
			g_grace.store(false, std::memory_order_release);
			g_lastAggressor.reset();
			ResetBleedRuntimeState();
			g_prevDialogueOpen = false;
			g_prevLockpickOpen = false;
			SetCaptiveRuntime(false, CaptivePhaseValue::None);
			SetPlayerBleedImmune(false);
			BeginRecoverTransition(reason ? reason : "battle_observe_loss");
			spdlog::info("[TFD][Defeat] battle observe resolved -> left for dead branch={}", NoMarkerBranchName(g_noMarkerFallback.branch));
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

		static RE::TESObjectREFR* ResolveCachedRescueDestinationForFallback()
		{
			auto* player = Player();
			const bool preferInterior = player && player->GetParentCell() ? player->GetParentCell()->IsInteriorCell() : true;
			if (auto* dest = TFD::Location::ResolveMostRecentCachedRescueDestination(preferInterior)) {
				return dest;
			}
			if (auto* dest = TFD::Location::ResolveMostRecentCachedRescueDestination(!preferInterior)) {
				return dest;
			}
			return nullptr;
		}

		static RE::BGSLocationRefType* ResolveWETravelRefType()
		{
			static RE::BGSLocationRefType* cached = nullptr;
			static bool tried = false;
			if (tried) {
				return cached;
			}
			tried = true;
			cached = RE::TESForm::LookupByEditorID<RE::BGSLocationRefType>("WETravel");
			if (!cached) {
				cached = RE::TESForm::LookupByEditorID<RE::BGSLocationRefType>("WETravelMarker");
			}
			return cached;
		}

		static void AddLocationChainSimple(std::vector<RE::BGSLocation*>& list, RE::BGSLocation* start)
		{
			for (auto* cur = start; cur; cur = cur->parentLoc) {
				bool seen = false;
				for (auto* existing : list) {
					if (existing == cur) {
						seen = true;
						break;
					}
				}
				if (!seen) {
					list.push_back(cur);
				}
			}
		}

		static bool IsGenericMarkerRef(RE::TESObjectREFR* ref, bool& heading)
		{
			heading = false;
			if (!ref) {
				return false;
			}
			auto* base = ref->GetBaseObject();
			if (!base) {
				return false;
			}
			const auto eid = TFD::Util::GetEditorId(base);
			if (eid == "XMarkerHeading") {
				heading = true;
				return true;
			}
			return eid == "XMarker";
		}

		static void LockCurrentBleedCrowdSnapshot(RE::Actor* preferredSpeaker)
		{
			g_lockedFallbackCrowdIds.clear();
			if (!g_bleedCrowdAssigned.empty()) {
				g_lockedFallbackCrowdIds = g_bleedCrowdAssigned;
			}
			if (g_lockedFallbackCrowdIds.empty()) {
				const float radius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius());
				auto crowd = CollectBleedoutCrowd(radius, preferredSpeaker, true);
				for (auto* actor : crowd) {
					if (actor) {
						g_lockedFallbackCrowdIds.push_back(actor->GetFormID());
					}
				}
			}
			if (preferredSpeaker) {
				const auto preferredId = preferredSpeaker->GetFormID();
				if (std::find(g_lockedFallbackCrowdIds.begin(), g_lockedFallbackCrowdIds.end(), preferredId) == g_lockedFallbackCrowdIds.end()) {
					g_lockedFallbackCrowdIds.insert(g_lockedFallbackCrowdIds.begin(), preferredId);
				}
			}
		}

		static void ApplyFallbackFacing(RE::Actor* actor, float angleZ)
		{
			if (!actor) {
				return;
			}
			actor->data.angle.z = angleZ;
		}

		static void ComputeLocalLeftForDeadFallback(RE::NiPoint3& outPos, float& outAngleZ)
		{
			auto* player = Player();
			if (!player) {
				outPos = {};
				outAngleZ = 0.0f;
				return;
			}
			const auto playerPos = player->GetPosition();
			auto crowdCenter = ComputeCrowdCenterPoint();
			float dx = playerPos.x - crowdCenter.x;
			float dy = playerPos.y - crowdCenter.y;
			const float len = std::sqrt(dx * dx + dy * dy);
			if (len < 1.0f) {
				dx = -std::sin(player->GetAngleZ());
				dy = -std::cos(player->GetAngleZ());
			}
			else {
				dx /= len;
				dy /= len;
			}
			const bool exterior = player->GetParentCell() ? player->GetParentCell()->IsExteriorCell() : true;
			const float dist = exterior ? 2300.0f : 384.0f;
			outPos = playerPos;
			outPos.x += dx * dist;
			outPos.y += dy * dist;
			outAngleZ = ComputeYawFromVector(dx, dy);
		}

		static void ResolveLeftForDeadDestination(NoMarkerFallbackState& state)
		{
			state.destination.reset();
			state.hasFallbackPos = false;
			state.angleZ = 0.0f;

			auto* player = Player();
			auto* playerCell = player ? player->GetParentCell() : nullptr;
			if (!player || !playerCell) {
				return;
			}

			const bool exterior = playerCell->IsExteriorCell();
			const auto playerPos = player->GetPosition();
			const auto crowdCenter = ComputeCrowdCenterPoint();
			auto* playerLoc = GetLocationFromRef(player);

			RE::TESObjectREFR* bestRef = nullptr;
			float bestScore = -1.0e30f;
			float bestAngle = 0.0f;

			auto considerRef = [&](RE::TESObjectREFR* ref, int tier) {
				if (!ref || ref->IsDisabled()) {
					return;
				}
				auto* refCell = ref->GetParentCell();
				if (!refCell) {
					return;
				}
				if (exterior) {
					if (ref->GetWorldspace() != player->GetWorldspace()) {
						return;
					}
				}
				else if (refCell != playerCell) {
					return;
				}
				auto* refLoc = GetLocationFromRef(ref);
				if (playerLoc && refLoc && refLoc != playerLoc) {
					return;
				}
				const auto refPos = ref->GetPosition();
				const float playerDist = Distance3D(playerPos, refPos);
				const float crowdDist = MinDistanceToLockedCrowd(refPos);
				const float minPlayer = exterior ? 2048.0f : 256.0f;
				const float maxPlayer = exterior ? 4096.0f : 2048.0f;
				const float minCrowd = exterior ? 2048.0f : 512.0f;
				if (playerDist < minPlayer || playerDist > maxPlayer) {
					return;
				}
				if (crowdDist != std::numeric_limits<float>::max() && crowdDist < minCrowd) {
					return;
				}
				const float ideal = exterior ? 3072.0f : 1024.0f;
				float score = (tier == 1 ? 600.0f : (tier == 2 ? 300.0f : 0.0f));
				if (crowdDist != std::numeric_limits<float>::max()) {
					score += crowdDist * 2.5f;
				}
				score -= std::abs(playerDist - ideal);
				score -= Distance3D(refPos, crowdCenter) * 0.15f;
				if (score > bestScore) {
					bestScore = score;
					bestRef = ref;
					bestAngle = ComputeYawFromVector(refPos.x - crowdCenter.x, refPos.y - crowdCenter.y);
				}
				};

			if (auto* weTravel = ResolveWETravelRefType()) {
				std::vector<RE::BGSLocation*> chain;
				AddLocationChainSimple(chain, playerLoc);
				for (auto* loc : chain) {
					if (!loc) {
						continue;
					}
					for (std::uint32_t i = 0; i < loc->specialRefs.size(); ++i) {
						const auto& sref = loc->specialRefs[i];
						if (!sref.type || sref.type->GetFormID() != weTravel->GetFormID()) {
							continue;
						}
						auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(sref.refData.refID);
						considerRef(ref, 1);
					}
				}
			}

			playerCell->ForEachReference([&](RE::TESObjectREFR* ref) {
				bool heading = false;
				if (!IsGenericMarkerRef(ref, heading)) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				considerRef(ref, heading ? 2 : 3);
				return RE::BSContainer::ForEachResult::kContinue;
				});

			if (bestRef) {
				state.destination = bestRef->GetHandle();
				state.angleZ = bestAngle;
				spdlog::info("[TFD][Defeat] left-for-dead anchor selected ref={:08X} branch={} score={:.1f}",
					bestRef->GetFormID(),
					NoMarkerBranchName(state.branch),
					bestScore);
				return;
			}

			ComputeLocalLeftForDeadFallback(state.fallbackPos, state.angleZ);
			state.hasFallbackPos = true;
			spdlog::info("[TFD][Defeat] left-for-dead anchor fallback=local branch={} pos=({:.1f},{:.1f},{:.1f})",
				NoMarkerBranchName(state.branch), state.fallbackPos.x, state.fallbackPos.y, state.fallbackPos.z);
		}

		static NoMarkerFallbackBranch ResolveNoMarkerFallback(const char* reason)
		{
			(void)reason;
			g_noMarkerFallback = {};

			RE::Actor* preferredSpeaker = nullptr;
			if (g_lastAggressor) {
				auto sp = RE::Actor::LookupByHandle(g_lastAggressor.native_handle());
				preferredSpeaker = sp.get();
			}
			LockCurrentBleedCrowdSnapshot(preferredSpeaker);

			const float followerRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 400.0f);
			auto* savior = ResolveBestHumanoidSavior(followerRadius);
			auto followers = ResolveFollowerCandidates(followerRadius);
			if (savior) {
				if (auto* rescueDest = ResolveCachedRescueDestinationForFallback()) {
					g_noMarkerFallback.branch = NoMarkerFallbackBranch::RescueCached;
					g_noMarkerFallback.follower = savior->GetHandle();
					g_noMarkerFallback.destination = rescueDest->GetHandle();
				}
				else {
					g_noMarkerFallback.branch = NoMarkerFallbackBranch::RecoveryFollower;
					g_noMarkerFallback.follower = savior->GetHandle();
				}
			}
			else if (followers.standing) {
				g_noMarkerFallback.branch = NoMarkerFallbackBranch::RecoveryFollower;
				g_noMarkerFallback.follower = followers.standing->GetHandle();
			}
			else if (followers.downed) {
				g_noMarkerFallback.branch = NoMarkerFallbackBranch::LeftForDeadWithFollower;
				g_noMarkerFallback.follower = followers.downed->GetHandle();
				ResolveLeftForDeadDestination(g_noMarkerFallback);
			}
			else if (auto* potion = ResolveRecoveryPotionCandidate()) {
				g_noMarkerFallback.branch = NoMarkerFallbackBranch::RecoveryPotion;
				g_noMarkerFallback.potionFormId = potion->GetFormID();
			}
			else if (auto* rescueDest = ResolveCachedRescueDestinationForFallback()) {
				g_noMarkerFallback.branch = NoMarkerFallbackBranch::RescueCached;
				g_noMarkerFallback.destination = rescueDest->GetHandle();
			}
			else {
				g_noMarkerFallback.branch = NoMarkerFallbackBranch::LeftForDeadSolo;
				ResolveLeftForDeadDestination(g_noMarkerFallback);
			}

			RE::FormID followerId = 0;
			if (g_noMarkerFallback.follower) {
				auto followerSp = RE::Actor::LookupByHandle(g_noMarkerFallback.follower.native_handle());
				if (auto* follower = followerSp.get()) {
					followerId = follower->GetFormID();
				}
			}
			RE::FormID destId = 0;
			if (g_noMarkerFallback.destination) {
				auto destSp = g_noMarkerFallback.destination.get();
				if (auto* dest = destSp.get()) {
					destId = dest->GetFormID();
				}
			}
			spdlog::info("[TFD][Defeat] no-marker fallback resolved branch={} follower={:08X} potion={:08X} dest={:08X} crowdLocked={}",
				NoMarkerBranchName(g_noMarkerFallback.branch),
				followerId,
				g_noMarkerFallback.potionFormId,
				destId,
				g_lockedFallbackCrowdIds.size());

			return g_noMarkerFallback.branch;
		}

		static void ApplyLeftForDeadWakeState(RE::Actor* actor, bool followerStyle)
		{
			if (!actor) {
				return;
			}
			actor->NotifyAnimationGraph("BleedoutStop");
			actor->NotifyAnimationGraph("GetUpStart");
			const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float threshPct = std::clamp(TFD::Settings::GetDefeatThresholdPct() / 100.0f, 0.05f, 0.95f);
			const float safePct = std::clamp(threshPct + (followerStyle ? 0.14f : 0.12f), followerStyle ? 0.34f : 0.32f, 0.85f);
			const float targetHp = (std::max)(followerStyle ? 32.0f : 45.0f, hpMax * safePct);
			const float hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
			if (hpNow < targetHp) {
				actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, targetHp - hpNow);
			}
			const float staminaMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kStamina));
			const float staminaTarget = (std::max)(20.0f, staminaMax * (followerStyle ? 0.28f : 0.35f));
			const float staminaNow = actor->GetActorValue(RE::ActorValue::kStamina);
			if (staminaNow < staminaTarget) {
				actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina, staminaTarget - staminaNow);
			}
			if (actor->IsInCombat()) {
				actor->StopCombat();
			}
			actor->DrawWeaponMagicHands(false);
		}

		static void MoveFollowerNearPlayerForLeftForDead(RE::Actor* follower)
		{
			auto* player = Player();
			if (!player || !follower) {
				return;
			}
			const bool interior = player->GetParentCell() ? player->GetParentCell()->IsInteriorCell() : false;
			const float offset = interior ? 128.0f : 220.0f;
			const float yaw = player->GetAngleZ();
			RE::NiPoint3 pos = player->GetPosition();
			pos.x += std::cos(yaw) * offset;
			pos.y -= std::sin(yaw) * offset;
			if (!follower->IsDead()) {
				follower->MoveTo(player);
			}
			follower->SetPosition(pos, true);
			ApplyFallbackFacing(follower, yaw);
			if (!follower->IsDead()) {
				ApplyLeftForDeadWakeState(follower, true);
				SetFollowerHold(follower);
			}
		}

		static void ExecuteLeftForDeadWake(const char* reason)
		{
			auto* player = Player();
			if (!player) {
				return;
			}
			if (g_noMarkerFallback.destination) {
				auto refSp = g_noMarkerFallback.destination.get();
				if (auto* dest = refSp.get()) {
					player->MoveTo(dest);
				}
			}
			else if (g_noMarkerFallback.hasFallbackPos) {
				player->SetPosition(g_noMarkerFallback.fallbackPos, true);
			}
			ApplyFallbackFacing(player, g_noMarkerFallback.angleZ);
			ApplyLeftForDeadWakeState(player, false);
			if (g_noMarkerFallback.branch == NoMarkerFallbackBranch::LeftForDeadWithFollower && g_noMarkerFallback.follower) {
				auto followerSp = RE::Actor::LookupByHandle(g_noMarkerFallback.follower.native_handle());
				if (auto* follower = followerSp.get()) {
					MoveFollowerNearPlayerForLeftForDead(follower);
				}
			}
			MaintainTransitionCalmWindow();
			g_leftForDeadNeedsAggroKick = false;
			BeginLeftForDeadCooldown(5);
			SetGraceSeconds(5);
			SetRescueStateValue(0);
			RefreshPostDefeatGlobals();
			UpdatePreCombatState();
			spdlog::info("[TFD][Transition] left-for-dead complete branch={} reason={}",
				NoMarkerBranchName(g_noMarkerFallback.branch),
				reason ? reason : "unknown");
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

		static const char* CinematicKindName(CinematicTransitionKind kind)
		{
			switch (kind) {
			case CinematicTransitionKind::Captive: return "captive";
			case CinematicTransitionKind::Rescue: return "rescue";
			case CinematicTransitionKind::Recover: return "recover";
			default: return "none";
			}
		}

		static bool QueueCinematicTransitionRequest(CinematicTransitionKind kind, bool fadeIn, const char* reason)
		{
			spdlog::info("[TFD][Transition] cinematic request skipped (globals removed) kind={} phase={} reason={}",
				CinematicKindName(kind), fadeIn ? "fadein" : "fadeout", reason ? reason : "unknown");
			return false;
		}

		static void ClearPendingCinematicFadeIn()
		{
			g_pendingCinematicFadeIn = false;
			g_pendingCinematicFadeInKind = CinematicTransitionKind::None;
			g_pendingCinematicFadeInNotBefore = {};
			g_pendingCinematicFadeInSawLoadingMenu = false;
		}

		static void ProcessPendingCinematicFadeIn()
		{
			if (!g_pendingCinematicFadeIn || g_pendingCinematicFadeInKind == CinematicTransitionKind::None) {
				return;
			}

			auto* ui = RE::UI::GetSingleton();
			const bool loadingOpen = ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
			if (loadingOpen) {
				g_pendingCinematicFadeInSawLoadingMenu = true;
				return;
			}

			if (Now() < g_pendingCinematicFadeInNotBefore) {
				return;
			}

			auto* player = Player();
			auto* cell = player ? player->GetParentCell() : nullptr;
			if (!player || !cell) {
				return;
			}

			const auto kind = g_pendingCinematicFadeInKind;
			const bool sawLoading = g_pendingCinematicFadeInSawLoadingMenu;
			ClearPendingCinematicFadeIn();

			spdlog::info("[TFD][Transition] fadein ready kind={} cell={:08X} path={}",
				CinematicKindName(kind), cell->GetFormID(), sawLoading ? "after_loading_close" : "after_settle");

			if (!QueueCinematicTransitionRequest(kind, true, sawLoading ? "fadein_after_loading_close" : "fadein_after_settle")) {
				HideBlackoutFader();
			}
		}

		static bool ResolveCaptiveMarkerForOutcome()
		{
			auto* aggressor = ResolveAggressor();
			bool resolved = false;
			if (aggressor) {
				resolved = TFD::Location::RescanCaptiveMarkerWithAggressor(aggressor, true);
				if (!resolved) {
					resolved = TFD::Location::RescanCaptiveMarkerWithAggressor(aggressor, false);
				}
			}
			if (!resolved) {
				resolved = TFD::Location::RescanCaptiveMarker();
			}
			return resolved && TFD::Location::GetCachedCaptiveMarker();
		}

		static bool TeleportPlayerToCachedMarkerNow()
		{
			auto* player = Player();
			auto* marker = TFD::Location::GetCachedCaptiveMarker();
			if (!player || !marker) {
				return false;
			}
			player->MoveTo(marker);
			spdlog::info("[TFD][Location] Direct MoveTo cached marker {:08X}", marker->GetFormID());
			return true;
		}

		static void QueueNonCaptiveChoiceRequest(const char* reason)
		{
			spdlog::info("[TFD][Transition] non-captive choice skipped (globals removed) reason={}",
				reason ? reason : "unknown");
		}

		static bool IsTransitionAwaiting()
		{
			return false;
		}

		static void MaintainTransitionCalmWindow()
		{
			if (g_bleedBattleObserveActive) {
				return;
			}
			auto* player = Player();
			if (!player) {
				return;
			}
			const float radius = (std::max)(3200.0f, TFD::Settings::GetSweepRadius() + 1200.0f);
			if (TryAbortPleasureDueToHostileIntrusion(radius)) {
				return;
			}
			if (player->IsInCombat()) {
				player->StopCombat();
			}
			player->DrawWeaponMagicHands(false);
			TFD::AntiAggro::SweepOnce(radius, false);

			std::size_t applied = 0;
			if (!g_lockedFallbackCrowdIds.empty()) {
				for (auto id : g_lockedFallbackCrowdIds) {
					auto* actorRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
					auto* actor = actorRef ? actorRef->As<RE::Actor>() : nullptr;
					if (!actor || actor->IsDead() || actor->IsDisabled() || IsActiveFollowerActor(actor)) {
						continue;
					}
					TFD::AggressionClamp::Apply(actor);
					actor->StopCombat();
					actor->EvaluatePackage(false, true);
					actor->EvaluatePackage(true, true);
					++applied;
				}
			}
			else {
				TFD::ActorScan::Rescan(radius, false);
				const auto count = TFD::ActorScan::GetCount();
				for (int i = 0; i < count; ++i) {
					auto entry = TFD::ActorScan::GetEntry(i);
					auto actorSP = entry.actor.get();
					auto* actor = actorSP.get();
					if (!actor || actor->IsDead() || actor->IsDisabled() || IsActiveFollowerActor(actor)) {
						continue;
					}
					TFD::AggressionClamp::Apply(actor);
					actor->StopCombat();
					actor->EvaluatePackage(false, true);
					actor->EvaluatePackage(true, true);
					++applied;
				}
			}

			MaintainFollowerHold();
			spdlog::info("[TFD][Transition] calm window maintained applied={} branch={} lockedCrowd={}",
				applied,
				NoMarkerBranchName(g_noMarkerFallback.branch),
				g_lockedFallbackCrowdIds.size());
		}

		static RE::TESObjectREFR* LookupRefByFormID(std::uint32_t formID)
		{
			if (formID == 0) {
				return nullptr;
			}
			return RE::TESForm::LookupByID<RE::TESObjectREFR>(formID);
		}

		static RE::TESObjectREFR* ResolveBestRescueDestination(RE::BGSLocation* safeLoc)
		{
			if (!safeLoc) {
				return nullptr;
			}

			TFD::Location::ApprovedBed bed{};
			if (TFD::Location::GetBestApprovedBedForLocation(safeLoc, bed)) {
				if (auto* ref = LookupRefByFormID(bed.bedRefId)) {
					return ref;
				}
			}

			TFD::Location::SafeCheckpoint cp{};
			if (TFD::Location::GetLastSafeCheckpointForLocation(safeLoc, cp)) {
				if (auto* ref = LookupRefByFormID(cp.insideEntranceRefId)) {
					return ref;
				}
				if (auto* ref = LookupRefByFormID(cp.centerMarkerRefId)) {
					return ref;
				}
				if (auto* ref = LookupRefByFormID(cp.entryDoorRefId)) {
					return ref;
				}
			}

			if (auto* ref = TFD::Location::ResolvePreferredRescueDestination(safeLoc, true)) {
				return ref;
			}

			return nullptr;
		}

		static bool CompleteRescueTransitionNow(const char* reason)
		{
			auto* player = Player();
			if (!player) {
				return false;
			}

			RE::BGSLocation* safeLoc = nullptr;
			RE::TESObjectREFR* dest = nullptr;

			if (g_noMarkerFallback.branch == NoMarkerFallbackBranch::RescueCached && g_noMarkerFallback.destination) {
				auto destSp = g_noMarkerFallback.destination.get();
				dest = destSp.get();
				safeLoc = GetLocationFromRef(dest);
			}

			if (!dest) {
				safeLoc = TFD::Location::ResolveRescueTargetLocationFromRef(player);
				dest = safeLoc ? ResolveBestRescueDestination(safeLoc) : nullptr;

				if ((!safeLoc || !dest)) {
					auto* fallbackLoc = TFD::Location::GetMostRecentCachedSafeLocation();
					if (fallbackLoc) {
						auto* fallbackDest = ResolveBestRescueDestination(fallbackLoc);
						if (fallbackDest) {
							spdlog::info("[TFD][Transition] rescue fallback to recent cache reason={} currentLoc={:08X} fallbackLoc={:08X}",
								reason ? reason : "unknown",
								safeLoc ? safeLoc->GetFormID() : 0,
								fallbackLoc->GetFormID());
							safeLoc = fallbackLoc;
							dest = fallbackDest;
						}
					}
				}
			}

			if (!dest) {
				spdlog::info("[TFD][Transition] rescue unavailable reason={} cause=no_destination", reason ? reason : "unknown");
				return false;
			}

			player->MoveTo(dest);
			std::this_thread::sleep_for(std::chrono::milliseconds(120));
			RecoverPlayerForTransition();
			MaintainTransitionCalmWindow();
			g_leftForDeadNeedsAggroKick = false;
			const int grace = (g_noMarkerFallback.branch == NoMarkerFallbackBranch::RescueCached) ? 1 : 4;
			BeginLeftForDeadCooldown(grace);
			SetGraceSeconds(grace);
			SetRescueStateValue(1);
			RefreshPostDefeatGlobals();
			UpdatePreCombatState();
			if (g_noMarkerFallback.follower) {
				auto followerSp = RE::Actor::LookupByHandle(g_noMarkerFallback.follower.native_handle());
				if (auto* follower = followerSp.get()) {
					SendPlayerSaviorAssign(follower);
					follower->EvaluatePackage(false, true);
					follower->EvaluatePackage(true, true);
					spdlog::info("[TFD][Transition] rescue savior assigned {:08X}", follower->GetFormID());
				}
			}

			spdlog::info("[TFD][Transition] rescue complete reason={} safeLoc={:08X} dest={:08X} branch={}",
				reason ? reason : "unknown", safeLoc ? safeLoc->GetFormID() : 0u, dest->GetFormID(), NoMarkerBranchName(g_noMarkerFallback.branch));
			return true;
		}

		static bool BeginRescueTransition(const char* reason)
		{
			SetRescueStateValue(0);
			ClearPendingCinematicFadeIn();
			if (QueueCinematicTransitionRequest(CinematicTransitionKind::Rescue, false, reason)) {
				return true;
			}
			ShowBlackoutFader();
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			const bool ok = CompleteRescueTransitionNow(reason);
			std::this_thread::sleep_for(std::chrono::milliseconds(400));
			HideBlackoutFader();
			return ok;
		}

		static void CompleteRecoverTransitionNow(const char* reason)
		{
			ClearPlayerSavior(nullptr, "recover_transition");
			if (g_noMarkerFallback.branch == NoMarkerFallbackBranch::RecoveryFollower) {
				auto followerSp = RE::Actor::LookupByHandle(g_noMarkerFallback.follower.native_handle());
				auto* follower = followerSp.get();
				if (!follower || follower->IsDead() || IsActorBleedingOut(follower)) {
					g_noMarkerFallback.branch = follower ? NoMarkerFallbackBranch::LeftForDeadWithFollower : NoMarkerFallbackBranch::LeftForDeadSolo;
					ResolveLeftForDeadDestination(g_noMarkerFallback);
					ExecuteLeftForDeadWake(reason ? reason : "recovery_follower_degraded_lfd");
					return;
				}
				RecoverPlayerForTransition();
				SetFollowerHold(follower);
				MaintainTransitionCalmWindow();
				g_leftForDeadNeedsAggroKick = false;
				BeginLeftForDeadCooldown(3);
				SetGraceSeconds(3);
				SetRescueStateValue(0);
				RefreshPostDefeatGlobals();
				UpdatePreCombatState();
				spdlog::info("[TFD][Transition] recover complete branch={} reason={} follower={:08X}",
					NoMarkerBranchName(g_noMarkerFallback.branch), reason ? reason : "unknown", follower->GetFormID());
				return;
			}

			if (g_noMarkerFallback.branch == NoMarkerFallbackBranch::RecoveryPotion) {
				RecoverPlayerForTransition();
				MaintainTransitionCalmWindow();
				g_leftForDeadNeedsAggroKick = false;
				BeginLeftForDeadCooldown(3);
				SetGraceSeconds(3);
				SetRescueStateValue(0);
				RefreshPostDefeatGlobals();
				UpdatePreCombatState();
				spdlog::info("[TFD][Transition] recover complete branch={} reason={} potion={:08X}",
					NoMarkerBranchName(g_noMarkerFallback.branch), reason ? reason : "unknown", g_noMarkerFallback.potionFormId);
				return;
			}

			if (g_noMarkerFallback.branch == NoMarkerFallbackBranch::LeftForDeadSolo ||
				g_noMarkerFallback.branch == NoMarkerFallbackBranch::LeftForDeadWithFollower) {
				ExecuteLeftForDeadWake(reason);
				return;
			}

			RecoverPlayerForTransition();
			MaintainTransitionCalmWindow();
			g_leftForDeadNeedsAggroKick = false;
			BeginLeftForDeadCooldown(3);
			SetGraceSeconds(3);
			SetRescueStateValue(0);
			RefreshPostDefeatGlobals();
			UpdatePreCombatState();
			spdlog::info("[TFD][Transition] recover complete branch={} reason={}", NoMarkerBranchName(g_noMarkerFallback.branch), reason ? reason : "unknown");
		}

		static void BeginRecoverTransition(const char* reason)
		{
			SetRescueStateValue(0);
			ClearPendingCinematicFadeIn();
			if (QueueCinematicTransitionRequest(CinematicTransitionKind::Recover, false, reason)) {
				return;
			}
			ShowBlackoutFader();
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			CompleteRecoverTransitionNow(reason);
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			HideBlackoutFader();
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

			const auto inv = ref->GetInventory([item](RE::TESBoundObject& obj) {
				return &obj == item;
				}, true);

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

			const auto inv = ref->GetInventory([](RE::TESBoundObject&) {
				return true;
				}, true);

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

		static void EnsureCaptiveStarterLockpicks(std::int32_t targetCount, const char* reason)
		{
			auto* player = Player();
			auto* lockpick = ResolveLockpickItem();
			if (!player || !lockpick || targetCount <= 0) {
				return;
			}

			const auto currentCount = GetReferenceItemCount(player, lockpick);
			if (currentCount >= targetCount) {
				spdlog::info("[TFD][Captive] starter lockpick skipped current={} target={} reason={}",
					currentCount,
					targetCount,
					reason ? reason : "unknown");
				return;
			}

			const auto addCount = targetCount - currentCount;
			player->AddObjectToContainer(lockpick, nullptr, addCount, nullptr);
			spdlog::info("[TFD][Captive] starter lockpick granted add={} finalTarget={} reason={}",
				addCount,
				targetCount,
				reason ? reason : "unknown");
		}

		static bool TransferPlayerInventoryToCaptiveStorage(RE::TESObjectREFR* target, const char* reason)
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

			const auto inv = player->GetInventory([](RE::TESBoundObject&) {
				return true;
				}, true);

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
				spdlog::info("[TFD][Captive] confiscation skipped empty_inventory target={:08X} reason={}",
					target->GetFormID(),
					reason ? reason : "unknown");
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

				player->RemoveItem(
					item,
					requestCount,
					RE::ITEM_REMOVE_REASON::kStoreInContainer,
					nullptr,
					target);

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

				spdlog::warn(
					"[TFD][Captive] confiscation stack mismatch item={:08X} requested={} removed={} added={} beforePlayer={} afterPlayer={} beforeTarget={} afterTarget={} reason={}",
					item->GetFormID(),
					requestCount,
					removedNow,
					addedNow,
					beforePlayer,
					afterPlayer,
					beforeTarget,
					afterTarget,
					reason ? reason : "unknown");
			}

			const auto playerUnitsAfter = GetReferenceTotalInventoryCount(player);

			spdlog::info(
				"[TFD][Captive] confiscation moved_manual target={:08X} stacks={} units={} movedStacks={} partialStacks={} failedStacks={} removedUnits={} addedUnits={} playerUnitsAfter={} reason={}",
				target->GetFormID(),
				totalStacks,
				totalUnits,
				movedStacks,
				partialStacks,
				failedStacks,
				removedUnits,
				addedUnits,
				playerUnitsAfter,
				reason ? reason : "unknown");

			return removedUnits > 0 || addedUnits > 0;
		}

		static void ClearPendingCaptiveConfiscation(const char* reason)
		{
			const bool hadPending = g_captiveConfiscationPending || g_captiveStarterLockpickPending || !g_captivePendingConfiscationReason.empty();
			g_captiveConfiscationPending = false;
			g_captiveStarterLockpickPending = false;
			g_captivePendingConfiscationReason.clear();
			g_captiveConfiscationAttemptCount = 0;
			g_captiveConfiscationNextAttempt = {};
			if (hadPending) {
				spdlog::info("[TFD][Captive] confiscation pending cleared reason={}", reason ? reason : "unknown");
			}
		}

		static void QueuePendingCaptiveConfiscation(const char* reason, bool starterKitWanted)
		{
			g_captiveConfiscationPending = true;
			g_captiveStarterLockpickPending = starterKitWanted;
			g_captivePendingConfiscationReason = reason ? reason : "unknown";
			g_captiveConfiscationAttemptCount = 0;
			g_captiveConfiscationNextAttempt = Now() + std::chrono::milliseconds(kCaptiveConfiscationInitialDelayMs);
			spdlog::info("[TFD][Captive] confiscation pending queued starterKitWanted={} delayMs={} reason={}",
				starterKitWanted ? 1 : 0,
				kCaptiveConfiscationInitialDelayMs,
				g_captivePendingConfiscationReason.c_str());
		}

		static void ProcessPendingCaptiveConfiscation()
		{
			if (!g_captiveConfiscationPending) {
				return;
			}

			if (g_captiveConfiscationApplied) {
				ClearPendingCaptiveConfiscation("already_applied");
				return;
			}

			if (!g_captiveState || g_captivePhase != CaptivePhaseValue::Captive) {
				ClearPendingCaptiveConfiscation("not_in_captive_phase");
				return;
			}

			if (Now() < g_captiveConfiscationNextAttempt) {
				return;
			}

			auto* ui = RE::UI::GetSingleton();
			if (ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
				g_captiveConfiscationNextAttempt = Now() + std::chrono::milliseconds(kCaptiveConfiscationRetryDelayMs);
				return;
			}

			auto* player = Player();
			auto* marker = TFD::Location::GetCachedCaptiveMarker();
			auto* playerCell = player ? player->GetParentCell() : nullptr;
			auto* markerCell = marker ? marker->GetParentCell() : nullptr;
			if (!player || !playerCell || !marker || !markerCell || playerCell != markerCell) {
				g_captiveConfiscationNextAttempt = Now() + std::chrono::milliseconds(kCaptiveConfiscationRetryDelayMs);
				return;
			}

			const char* reason = g_captivePendingConfiscationReason.empty() ? "unknown" : g_captivePendingConfiscationReason.c_str();
			++g_captiveConfiscationAttemptCount;
			spdlog::info("[TFD][Captive] confiscation pending attempt={} cell={:08X} marker={:08X} reason={}",
				g_captiveConfiscationAttemptCount,
				playerCell->GetFormID(),
				marker->GetFormID(),
				reason);

			auto* storage = TFD::Location::ResolveNearestCaptiveStorageTarget(nullptr);
			SyncCaptiveStorageDebugAliases(reason);
			if (!storage) {
				if (g_captiveConfiscationAttemptCount >= kCaptiveConfiscationMaxAttempts) {
					spdlog::warn("[TFD][Captive] confiscation aborted no_container_target attempts={} reason={}",
						g_captiveConfiscationAttemptCount,
						reason);
					ClearPendingCaptiveConfiscation("no_container_target");
				}
				else {
					g_captiveConfiscationNextAttempt = Now() + std::chrono::milliseconds(kCaptiveConfiscationRetryDelayMs);
				}
				return;
			}

			if (!IsContainerStorageTarget(storage)) {
				if (g_captiveConfiscationAttemptCount >= kCaptiveConfiscationMaxAttempts) {
					spdlog::warn("[TFD][Captive] confiscation aborted non_container_target target={:08X} attempts={} reason={}",
						storage->GetFormID(),
						g_captiveConfiscationAttemptCount,
						reason);
					ClearPendingCaptiveConfiscation("non_container_target");
				}
				else {
					g_captiveConfiscationNextAttempt = Now() + std::chrono::milliseconds(kCaptiveConfiscationRetryDelayMs);
				}
				return;
			}

			const bool moved = TransferPlayerInventoryToCaptiveStorage(storage, reason);
			g_captiveConfiscationApplied = true;
			if (moved && g_captiveStarterLockpickPending) {
				EnsureCaptiveStarterLockpicks(3, reason);
			}
			ClearPendingCaptiveConfiscation(moved ? "completed" : "completed_no_items");
		}

		static void CompleteCaptiveTransitionNow(const char* reason)
		{
			ClearNoMarkerFallbackState();
			ResetBleedRuntimeState();
			TFD::Bleedout::ClearBridgeAliases(nullptr, "blackout_teleport");
			g_lastAggressor.reset();
			AdvanceGameHoursSoft(1.0f);
			if (!TeleportPlayerToCachedMarkerNow()) {
				EnterNonCaptiveChoice("teleport_failed");
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(120));
			RecoverPlayerAfterTeleport();
			SetGraceSeconds(5);
			(void)TFD::Flow::Controller::GetSingleton().BeginCaptive(ResolveBleedFlowActorFormID(), TFD::Flow::CaptiveMode::Kidnapped, reason ? reason : "captive_enter");
			SetCaptiveRuntime(true, CaptivePhaseValue::Captive);
			g_prevDialogueOpen = IsDialogueOpen();
			g_prevLockpickOpen = IsLockpickingOpen();
			ResetLockpickWatch();
			ArmEscapeContextFromCurrentState();
			if (g_captiveDoor.HasDoor()) {
				g_captiveDoor.SealToInitial(true);
			}
			ApplyCalmBubble((std::max)(2000.0f, TFD::Settings::GetSweepRadius()));
			QueuePendingCaptiveConfiscation(reason, true);
			SyncPlayerCaptiveAlias(Player(), reason ? reason : "captive_enter");
			spdlog::info("[TFD][Captive] entered captivePhase reason={} confiscationQueued=1 starterKitPending=1",
				reason ? reason : "unknown");
		}

		static void PollTransitionResult()
		{
			// TFDTransition* globals removed from ESP.
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
			const auto snapshot = TFD::Flow::Controller::GetSingleton().GetSnapshot();
			if (snapshot.root == TFD::Flow::RootFlow::InCombat || snapshot.root == TFD::Flow::RootFlow::Bleedout) {
				return true;
			}
			if (snapshot.root == TFD::Flow::RootFlow::Captive &&
				(snapshot.sub == TFD::Flow::SubFlow::EscapeAttempt ||
					snapshot.sub == TFD::Flow::SubFlow::EscapeFailed ||
					snapshot.sub == TFD::Flow::SubFlow::Recapture)) {
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
			return TFD::Flow::Controller::GetSingleton().GetSnapshot().primaryActorFormID;
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
			TFD::RescueRuntime::SetStateValue(value);
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
				TFD::VictoryRuntime::SetStateValue(0);
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
			TFD::VictoryRuntime::SetStateValue(ComputeVictoryState(player, victoryContext, enemies));
			SetGlobalInt(g_hostileStateGlobal, ComputeHostileState(player, enemies));
			SetGlobalInt(g_enemyFactionStateGlobal, ComputeEnemyFactionState(enemies));
			SetGlobalInt(g_enemyRaceStateGlobal, ComputeEnemyRaceState(enemies));
			SetGlobalInt(g_recoveryStateGlobal, ComputeRecoveryState());
			SetGlobalInt(g_leftForDeadStateGlobal, ComputeLeftForDeadState(player));
		}

		static void SetCaptiveRuntimeOnly(bool stateActive, CaptivePhaseValue phase)
		{
			g_captiveState = stateActive;
			g_captivePhase = phase;
			if (!stateActive) {
				SyncPlayerCaptiveAlias(nullptr, "captive_exit");
				ClearCaptiveStorageDebugAliases("captive_exit");
			}
			if (!stateActive || phase != CaptivePhaseValue::Captive) {
				g_captiveConfiscationApplied = false;
				ClearPendingCaptiveConfiscation("captive_state_reset");
			}
		}

		static void SetCaptiveRuntime(bool stateActive, CaptivePhaseValue phase)
		{
			SetCaptiveRuntimeOnly(stateActive, phase);
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

		static bool IsLockpickingOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && ui->IsMenuOpen(RE::LockpickingMenu::MENU_NAME);
		}

		static void ResetLockpickWatch()
		{
			g_lockpickDoorCandidate.reset();
			g_lockpickDoorWasLocked = false;
			g_prevLockpickOpen = false;
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

		static RE::TESObjectREFR* ResolveBoundEscapeDoor()
		{
			if (!g_boundEscapeDoor) return nullptr;
			auto ptr = g_boundEscapeDoor.get();
			return ptr.get();
		}

		static void BindCaptiveDoor(RE::TESObjectREFR* door)
		{
			if (!door) return;
			g_captiveDoor.Bind(door);
			g_boundEscapeDoor = door->GetHandle();
		}

		static RE::BGSLocation* GetLocationFromRef(RE::TESObjectREFR* ref)
		{
			return TFD::Location::GetLocationFromRef(ref);
		}

		static RE::TESObjectREFR* FindNearestDoorNearCaptiveMarker()
		{
			RE::TESObjectREFR* marker = nullptr;
			if (g_captiveMarker) {
				auto ptr = g_captiveMarker.get();
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

		static void ClearEscapeContext()
		{
			g_captiveMarker.reset();
			g_captiveCellFormID = 0;
			g_captiveLocationFormID = 0;
			g_escapeRadiusActive = false;
			g_escapeRadiusSince = {};
			g_boundEscapeDoor.reset();
			g_captiveDoor.Reset();
		}

		static void ArmEscapeContextFromCurrentState()
		{
			auto* player = Player();
			if (!player) return;
			auto* marker = TFD::Location::GetCachedCaptiveMarker();
			if (!marker) {
				TFD::Location::RescanCaptiveMarker();
				marker = TFD::Location::GetCachedCaptiveMarker();
			}
			g_captiveMarker = marker ? marker->GetHandle() : RE::ObjectRefHandle{};
			auto* cell = player->GetParentCell();
			g_captiveCellFormID = cell ? cell->GetFormID() : 0;
			auto* loc = cell ? cell->GetLocation() : nullptr;
			g_captiveLocationFormID = loc ? loc->GetFormID() : 0;
			g_escapeRadiusActive = false;
			g_escapeRadiusSince = {};
			if (!g_captiveDoor.HasDoor()) {
				if (auto* door = FindNearestDoorNearCaptiveMarker()) {
					BindCaptiveDoor(door);
					spdlog::info("[TFD][Captive] Bound nearest captive door {:08X} on captive enter", door->GetFormID());
				}
			}
			spdlog::info("[TFD][Captive] Escape context armed marker={:08X} cell={:08X} loc={:08X}", marker ? marker->GetFormID() : 0, g_captiveCellFormID, g_captiveLocationFormID);
		}

		static bool IsDoorNearCaptiveMarker(RE::TESObjectREFR* door)
		{
			if (!door || !IsDoorRef(door)) return false;
			RE::TESObjectREFR* marker = nullptr;
			if (g_captiveMarker) {
				auto ptr = g_captiveMarker.get();
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

		static RE::Actor* ResolveAggressor();
		static RE::Actor* FindBestAggressor(float radius);

		static RE::TESObjectREFR* ResolveLockpickDoorCandidate(RE::TESObjectREFR* target)
		{
			if (target && IsDoorRef(target) && IsDoorNearCaptiveMarker(target)) return target;
			auto* boundDoor = ResolveBoundEscapeDoor();
			if (boundDoor && IsDoorRef(boundDoor) && IsDoorNearCaptiveMarker(boundDoor)) return boundDoor;
			return nullptr;
		}

		static void EnterEscapeCommit(const char* reason, RE::TESObjectREFR* door)
		{
			if (!g_captiveState || g_captivePhase != CaptivePhaseValue::Captive) return;
			TFD::FactionMask::Clear();
			TFD::AggressionClamp::Clear();
			g_grace.store(false, std::memory_order_release);
			SetCaptiveRuntime(true, CaptivePhaseValue::Escape);
			(void)TFD::Flow::Controller::GetSingleton().ResolveCaptiveOutcome(TFD::Flow::CaptiveOutcome::EscapeStarted, 0u, reason ? reason : "escape_started");
			UpdatePreCombatState();
			ResetLockpickWatch();
			g_prevDialogueOpen = false;
			if (door) BindCaptiveDoor(door); else door = ResolveBoundEscapeDoor();
			auto* player = Player();
			if (player) player->EvaluatePackage(true, false);
			RE::Actor* aggressor = ResolveAggressor();
			if (!aggressor) {
				const float radius = (std::max)(1800.0f, TFD::Settings::GetSweepRadius());
				aggressor = FindBestAggressor(radius);
				if (aggressor) g_lastAggressor = aggressor->GetHandle();
			}
			if (aggressor) {
				aggressor->EvaluatePackage(true, false);
				spdlog::info("[TFD][Captive] Escape aggro nudge actor={:08X}", aggressor->GetFormID());
			}
			else {
				spdlog::info("[TFD][Captive] Escape aggro nudge skipped (no aggressor)");
			}
			spdlog::info("[TFD][Captive] EscapeCommit reason={} door={:08X}", reason ? reason : "unknown", door ? door->GetFormID() : 0);
		}

		static void TryCommitEscapeByRadius()
		{
			if (!g_captiveState || g_captivePhase != CaptivePhaseValue::Captive) {
				g_escapeRadiusActive = false;
				return;
			}
			auto* player = Player();
			if (!player) return;
			RE::TESObjectREFR* marker = nullptr;
			if (g_captiveMarker) {
				auto ptr = g_captiveMarker.get();
				marker = ptr.get();
			}
			if (!marker) marker = TFD::Location::GetCachedCaptiveMarker();
			if (!marker) return;
			const auto pp = player->GetPosition();
			const auto mp = marker->GetPosition();
			const double dx = static_cast<double>(pp.x - mp.x);
			const double dy = static_cast<double>(pp.y - mp.y);
			const double dz = static_cast<double>(pp.z - mp.z);
			const double distSq = dx * dx + dy * dy + dz * dz;
			const bool outside = distSq > (512.0 * 512.0);
			if (!outside) {
				g_escapeRadiusActive = false;
				return;
			}
			if (!g_escapeRadiusActive) {
				g_escapeRadiusActive = true;
				g_escapeRadiusSince = Now();
				return;
			}
			const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(Now() - g_escapeRadiusSince).count();
			if (held >= 2000) {
				EnterEscapeCommit("marker_radius", nullptr);
				g_escapeRadiusActive = false;
			}
		}

		static void TryResolveEscapeByLocation()
		{
			if (!g_captiveState || g_captivePhase != CaptivePhaseValue::Escape) return;
			auto* player = Player();
			if (!player) return;
			auto* loc = GetLocationFromRef(player);
			const auto curLoc = loc ? loc->GetFormID() : 0;
			if (g_captiveLocationFormID != 0 && curLoc != 0 && curLoc != g_captiveLocationFormID) {
				SetCaptiveRuntime(false, CaptivePhaseValue::None);
				(void)TFD::Flow::Controller::GetSingleton().ResolveCaptiveOutcome(TFD::Flow::CaptiveOutcome::EscapeSucceeded, 0u, "escape_resolved_location");
				(void)TFD::Flow::Controller::GetSingleton().CompleteTerminalContext("escape_resolved_location");
				ClearEscapeContext();
				UpdatePreCombatState();
				spdlog::info("[TFD][Captive] EscapeResolved by location old={:08X} new={:08X}", g_captiveLocationFormID, curLoc);
			}
		}

		static bool BreakEscapeOnDefeatThreshold(RE::Actor* player)
		{
			if (!player) return false;
			if (!g_captiveState || g_captivePhase != CaptivePhaseValue::Escape) return false;
			const float hpNow = player->GetActorValue(RE::ActorValue::kHealth);
			const float hpMax = (std::max)(1.0f, player->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float pct = (hpNow / hpMax) * 100.0f;
			const float thresh = TFD::Settings::GetDefeatThresholdPct();
			if (pct > thresh) return false;

			RE::Actor* preferred = nullptr;
			const float reacquireRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius());
			if (auto* resolved = ResolveAggressor()) {
				preferred = resolved;
			}
			else if (auto* fallback = FindBestAggressor(reacquireRadius)) {
				preferred = fallback;
			}

			(void)TFD::Flow::Controller::GetSingleton().ResolveCaptiveOutcome(TFD::Flow::CaptiveOutcome::EscapeFailed, preferred ? preferred->GetFormID() : 0u, "escape_broken_threshold");
			g_escapeBreakPreferredAggressor = preferred ? preferred->GetHandle() : RE::ActorHandle{};
			g_escapeBreakBleedPending = true;
			g_lastAggressor.reset();
			SetCaptiveRuntime(true, CaptivePhaseValue::Captive);
			ClearEscapeContext();
			UpdatePreCombatState();
			spdlog::info(
				"[TFD][Captive] Escape broken by defeat threshold pct={:.1f} thresh={:.1f} -> revert to captive and schedule rebleed preferred={:08X}",
				pct,
				thresh,
				preferred ? preferred->GetFormID() : 0);
			return true;
		}

		static void NormalizeInvalidCaptivePair()
		{
			if (g_captiveState && g_captivePhase == CaptivePhaseValue::None) {
				SetCaptiveRuntime(true, CaptivePhaseValue::Escape);
				(void)TFD::Flow::Controller::GetSingleton().ResolveCaptiveOutcome(TFD::Flow::CaptiveOutcome::EscapeStarted, 0u, "normalize_invalid_captive_pair");
				spdlog::info("[TFD][Captive] Normalized invalid captive pair -> Escape");
			}
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
			for (auto* actor : CollectRegisteredTeammates()) {
				if (!actor || actor->IsDead() || actor->IsDisabled()) {
					continue;
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

				const bool isLockedAlly = [&]() {
					auto it = g_bleedLocks.find(actor->GetFormID());
					return it != g_bleedLocks.end() && it->second.kind == BleedLockKind::Ally;
					}();
				const bool isDown = isLockedAlly || IsActorBleedingOut(actor) || IsActorDownByThreshold(actor, TFD::Settings::GetAllyDownedThresholdPct());
				if (!isDown) {
					actor->EvaluatePackage(false, true);
					actor->EvaluatePackage(true, true);
				}

				spdlog::info("[TFD][Defeat] teammate battle-win settle actor={:08X} down={} locked={}",
					actor->GetFormID(),
					isDown ? 1 : 0,
					isLockedAlly ? 1 : 0);
			}
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
			for (auto* actor : CollectKnownTeammates(radius)) {
				const float threshold = ResolveBleedLockThresholdPct(actor);
				if (ShouldEnterBleedLock(actor, threshold)) {
					EnterBleedLock(actor, ResolveBleedLockKind(actor), threshold, "threshold_scan_follower");
				}
			}

			TFD::ActorScan::Rescan(radius, false);
			const auto count = TFD::ActorScan::GetCount();
			for (int i = 0; i < count; ++i) {
				auto e = TFD::ActorScan::GetEntry(i);
				auto sp = e.actor.get();
				auto* actor = sp.get();
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

		static void SetBleedDialogueOutcome(BleedDialogueOutcome outcome, const char* reason)
		{
			TFD::Bleedout::SetDialogueOutcome(outcome, reason);
		}

		static void ClearBleedDialogueOutcome(const char* reason)
		{
			TFD::Bleedout::ClearDialogueOutcome(reason);
		}

		static RE::TESFaction* ResolveReleaseFollowHelperFaction()
		{
			static RE::TESFaction* cached = nullptr;
			static bool tried = false;
			if (!tried) {
				tried = true;
				cached = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDTruceTeammateFaction");
				if (!cached) {
					spdlog::warn("[TFD][Grace] helper faction not found editorId=TFDTruceTeammateFaction");
				}
			}
			return cached;
		}

		static RE::TESFaction* ResolvePacifyHelperFaction()
		{
			static RE::TESFaction* cached = nullptr;
			static bool tried = false;
			if (!tried) {
				tried = true;
				cached = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDPacifyFaction");
				if (!cached) {
					spdlog::warn("[TFD][Grace] pacify faction not found editorId=TFDPacifyFaction");
				}
			}
			return cached;
		}

		static bool ActorHasActiveDialoguePhaseFaction(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			constexpr const char* kPhaseFactionEditorIds[] = {
				"TFDPreCombatTruceFaction",
				"TFDInCombatTruceFaction",
				"TFDBleedOutFaction",
				"TFDBleedoutFaction",
				"TFDCaptiveFaction",
				"TFDWorkingCaptiveFaction",
				"TFDAfterPleasureFaction",
				"TFDSaviorFaction"
			};
			for (auto* editorID : kPhaseFactionEditorIds) {
				auto* faction = RE::TESForm::LookupByEditorID<RE::TESFaction>(editorID);
				if (faction && actor->IsInFaction(faction)) {
					return true;
				}
			}
			return false;
		}

		static std::vector<RE::Actor*> CollectReleaseFollowGraceActors(RE::Actor* speaker)
		{
			ResolveTruceQuestRegistry();
			std::vector<RE::Actor*> actors{};
			auto addUnique = [&](RE::Actor* actor) {
				if (!actor || actor->IsDead() || actor->IsDisabled() || actor == Player()) {
					return;
				}
				for (auto* existing : actors) {
					if (existing == actor) {
						return;
					}
				}
				actors.push_back(actor);
				};
			addUnique(speaker);
			for (auto* alias : g_truceQuestRegistry.truceAliases) {
				if (!alias) {
					continue;
				}
				addUnique(alias->GetActorReference());
			}
			return actors;
		}

		static void RemoveReleaseFollowGraceFromActor(RE::Actor* actor, const char* reason)
		{
			if (!actor) {
				return;
			}

			auto* helperFaction = ResolveReleaseFollowHelperFaction();
			auto* pacifyFaction = ResolvePacifyHelperFaction();
			if (helperFaction && actor->IsInFaction(helperFaction)) {
				actor->RemoveFromFaction(helperFaction);
			}
			if (pacifyFaction && actor->IsInFaction(pacifyFaction) && !ActorHasActiveDialoguePhaseFaction(actor)) {
				actor->RemoveFromFaction(pacifyFaction);
			}

			g_releaseFollowGraceEntries.erase(actor->GetFormID());
			spdlog::info("[TFD][Grace] removed actor={:08X} reason={}", actor->GetFormID(), reason ? reason : "unknown");
		}

		static void ApplyReleaseFollowGraceToActor(RE::Actor* actor, double durationSeconds, const char* reason)
		{
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				return;
			}

			auto* helperFaction = ResolveReleaseFollowHelperFaction();
			auto* pacifyFaction = ResolvePacifyHelperFaction();
			if (!helperFaction && !pacifyFaction) {
				return;
			}

			if (durationSeconds <= 0.0) {
				durationSeconds = 20.0;
			}

			if (helperFaction && !actor->IsInFaction(helperFaction)) {
				actor->AddToFaction(helperFaction, 0);
			}
			if (pacifyFaction && !actor->IsInFaction(pacifyFaction)) {
				actor->AddToFaction(pacifyFaction, 0);
			}

			ReleaseFollowGraceEntry entry{};
			entry.actor = actor->GetHandle();
			entry.expiresAt = Now() + std::chrono::milliseconds(static_cast<int>((std::max)(0.0, durationSeconds) * 1000.0));
			entry.source = reason ? reason : "unknown";
			g_releaseFollowGraceEntries[actor->GetFormID()] = std::move(entry);

			if (auto* player = Player()) {
				(void)SuppressReleaseFollowTargetingToPlayer(actor, player, reason ? reason : "grace_apply");
			}

			spdlog::info("[TFD][Grace] applied actor={:08X} helperFaction={:08X} pacifyFaction={:08X} duration={:.2f} reason={}",
				actor->GetFormID(),
				helperFaction ? helperFaction->GetFormID() : 0u,
				pacifyFaction ? pacifyFaction->GetFormID() : 0u,
				durationSeconds,
				reason ? reason : "unknown");
		}

		static void ApplyReleaseFollowGraceToSpeakerAndCrowd(RE::Actor* speaker, double durationSeconds, const char* reason)
		{
			for (auto* actor : CollectReleaseFollowGraceActors(speaker)) {
				ApplyReleaseFollowGraceToActor(actor, durationSeconds, reason);
			}
		}

		static void RemoveReleaseFollowGraceFromSpeakerAndCrowd(RE::Actor* speaker, const char* reason)
		{
			for (auto* actor : CollectReleaseFollowGraceActors(speaker)) {
				RemoveReleaseFollowGraceFromActor(actor, reason);
			}
		}

		static bool HasReleaseFollowGrace(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			return g_releaseFollowGraceEntries.find(actor->GetFormID()) != g_releaseFollowGraceEntries.end();
		}

		static bool SuppressReleaseFollowTargetingToPlayer(RE::Actor* actor, RE::Actor* player, const char* reason)
		{
			if (!actor || !player || actor == player) {
				return false;
			}

			bool changed = false;
			auto* target = ResolveCurrentCombatTarget(actor);
			if (target == player) {
				actor->GetActorRuntimeData().currentCombatTarget = RE::ActorHandle{};
				changed = true;
			}

			if (actor->IsInCombat() || target == player) {
				actor->StopCombat();

				if (auto* process = RE::ProcessLists::GetSingleton()) {
					process->StopCombatAndAlarmOnActor(actor, false);
				}

				changed = true;
			}

			if (!changed) {
				return false;
			}

			if (auto* process = RE::ProcessLists::GetSingleton()) {
				process->ClearCachedFactionFightReactions();
			}
			actor->EvaluatePackage(false, true);
			actor->EvaluatePackage(true, true);
			actor->UpdateCombat();
			player->UpdateCombat();

			spdlog::info("[TFD][Grace] suppressed player targeting actor={:08X} player={:08X} inCombat={} reason={}",
				actor->GetFormID(),
				player->GetFormID(),
				actor->IsInCombat() ? 1 : 0,
				reason ? reason : "release_follow");
			return true;
		}

		static void CancelReleaseFollowGraceFromPlayerAggression(RE::Actor* actor, const char* reason)
		{
			if (!actor || !HasReleaseFollowGrace(actor)) {
				return;
			}
			RemoveReleaseFollowGraceFromSpeakerAndCrowd(actor, reason ? reason : "player_attack_cancel");
		}

		static void MaintainReleaseFollowGrace()
		{
			if (g_releaseFollowGraceEntries.empty()) {
				return;
			}

			auto* player = Player();
			std::vector<RE::Actor*> actorsToClear{};
			std::vector<RE::FormID> staleIds{};
			const auto now = Now();
			for (auto& [formID, entry] : g_releaseFollowGraceEntries) {
				auto actorPtr = RE::Actor::LookupByHandle(entry.actor.native_handle());
				auto* actor = actorPtr.get();
				const bool expired = now >= entry.expiresAt;
				const bool invalid = !actor || actor->IsDead() || actor->IsDisabled();
				if (!expired && !invalid) {
					if (player) {
						(void)SuppressReleaseFollowTargetingToPlayer(actor, player, entry.source.c_str());
					}
					continue;
				}
				if (actor) {
					actorsToClear.push_back(actor);
				}
				else {
					staleIds.push_back(formID);
					spdlog::info("[TFD][Grace] removed stale handle actor={:08X} reason={}", formID, expired ? "timer_expired_missing_actor" : "actor_missing");
				}
			}
			for (auto* actor : actorsToClear) {
				RemoveReleaseFollowGraceFromActor(actor, "timer_or_invalid");
			}
			for (auto formID : staleIds) {
				g_releaseFollowGraceEntries.erase(formID);
			}
		}

		static void ClearAllReleaseFollowGrace(const char* reason)
		{
			if (g_releaseFollowGraceEntries.empty()) {
				return;
			}
			std::vector<RE::Actor*> actorsToClear{};
			std::vector<RE::FormID> ids{};
			actorsToClear.reserve(g_releaseFollowGraceEntries.size());
			ids.reserve(g_releaseFollowGraceEntries.size());
			for (auto& [formID, entry] : g_releaseFollowGraceEntries) {
				auto actorPtr = RE::Actor::LookupByHandle(entry.actor.native_handle());
				if (auto* actor = actorPtr.get()) {
					actorsToClear.push_back(actor);
				}
				else {
					ids.push_back(formID);
				}
			}
			for (auto* actor : actorsToClear) {
				RemoveReleaseFollowGraceFromActor(actor, reason ? reason : "clear_all");
			}
			for (auto formID : ids) {
				g_releaseFollowGraceEntries.erase(formID);
			}
		}

		static RE::Actor* ResolveActorFromEventArg(const std::string_view& arg)
		{
			if (arg.empty()) {
				return nullptr;
			}

			std::string parsedArg(arg);
			char* end = nullptr;
			const auto raw = std::strtoul(parsedArg.c_str(), &end, 0);
			if (end == nullptr || end == parsedArg.c_str()) {
				return nullptr;
			}

			return RE::TESForm::LookupByID<RE::Actor>(static_cast<RE::FormID>(raw));
		}

		static RE::FormID ResolveActorFormIDFromEventArg(const std::string_view& arg)
		{
			if (auto* actor = ResolveActorFromEventArg(arg)) {
				return actor->GetFormID();
			}
			return 0;
		}

		static int ResolveSourceFlowFromEventArg(const std::string_view& arg)
		{
			if (arg.empty()) {
				return 0;
			}

			const auto firstSep = arg.find('|');
			if (firstSep == std::string_view::npos || firstSep + 1 >= arg.size()) {
				return 0;
			}

			auto sourceView = arg.substr(firstSep + 1);
			const auto secondSep = sourceView.find('|');
			if (secondSep != std::string_view::npos) {
				sourceView = sourceView.substr(0, secondSep);
			}

			std::string sourceText(sourceView);
			char* end = nullptr;
			const auto raw = std::strtol(sourceText.c_str(), &end, 10);
			if (end == nullptr || end == sourceText.c_str()) {
				return 0;
			}

			return static_cast<int>(raw);
		}

		static bool ResolveBleedPostDialogueSystemEventOutcome(RE::Actor* player, const char* reason);
		static bool ShouldDropBleedSystemEventBecauseFallback(const char* eventName);

		static bool ResolveBleedPostDialogueSystemEventOutcome(RE::Actor* player, const char* reason)
		{
			(void)player;
			return TFD::Bleedout::TryResolvePostDialogueSystemEvent(
				reason,
				TFD::Bleedout::PendingSystemEventHandlers{
					[&](const char* r) { ClearBleedDialogueOutcome(r); },
					[&](const char* r) { ClearBleedSystemEventOutcomeWindow(r); },
					[&](const char* r) { CompleteBleedPayRelease(r); },
					[&]() { ReleaseBleedTruceSession(TFD::Pacify::ReleaseReason::FlowHandoff); },
					[&]() { return ResolveCaptiveMarkerForOutcome(); },
					[&]() { DoBlackoutTeleport(); },
					[&](int seconds) { SetGraceSeconds(seconds); },
					[&](const char* r) { ReleaseBleedNoSpeakerTameSession(r); },
					[&]() {
						g_inBleedState.store(false, std::memory_order_release);
						g_minHp = 0.0f;
						TFD::BleedoutGreet::ResetRuntime("bleed_reset");
						g_bleedLastSeconds = -1;
					},
					[&](const char* r) { EnterNonCaptiveChoice(r); },
					{},
					{},
					{}
				});
		}

		static bool ShouldDropBleedSystemEventBecauseFallback(const char* eventName)
		{
			const auto commit = GetBleedTerminalCommit();
			return TFD::Bleedout::ShouldDropSystemEventBecauseFallback(
				eventName,
				commit == BleedTerminalCommit::Captive || commit == BleedTerminalCommit::NonCaptiveFallback,
				BleedTerminalCommitName(commit));
		}

		static bool ResolveBleedPendingSystemEventFallback(RE::Actor* player, const char* reason)
		{
			return TFD::Bleedout::TryHandlePendingSystemEventFallback(
				TFD::Bleedout::PendingSystemEventContext{
					TFD::PleasureRuntime::GetPhaseName(),
					TFD::PleasureRuntime::IsActive(),
					TFD::PleasureRuntime::IsBlocking()
				},
				reason,
				TFD::Bleedout::PendingSystemEventHandlers{
					[&](const char* r) { ClearBleedDialogueOutcome(r); },
					[&](const char* r) { ClearBleedSystemEventOutcomeWindow(r); },
					[&](const char* r) { CompleteBleedPayRelease(r); },
					[&]() { ReleaseBleedTruceSession(TFD::Pacify::ReleaseReason::FlowHandoff); },
					[&]() { return ResolveCaptiveMarkerForOutcome(); },
					[&]() { DoBlackoutTeleport(); },
					[&](int seconds) { SetGraceSeconds(seconds); },
					[&](const char* r) { ReleaseBleedNoSpeakerTameSession(r); },
					[&]() {
						g_inBleedState.store(false, std::memory_order_release);
						g_minHp = 0.0f;
						TFD::BleedoutGreet::ResetRuntime("bleed_reset");
						g_bleedLastSeconds = -1;
					},
					[&](const char* r) { EnterNonCaptiveChoice(r); },
					[&]() { return g_bleedDialogueRetryCount; },
					[&](int count) { g_bleedDialogueRetryCount = count; },
					[&](const char* r) { return PromoteNextBleedSpeakerFromTruceQueue(player, r, false); }
				});
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

			TFD::ActorScan::Rescan(radius, false);
			RE::Actor* best = nullptr;
			float bestScore = std::numeric_limits<float>::max();
			const auto n = TFD::ActorScan::GetCount();
			for (int i = 0; i < n; ++i) {
				auto e = TFD::ActorScan::GetEntry(i);
				auto sp = e.actor.get();
				auto* a = sp.get();
				if (!a || a->IsDead() || a->IsDisabled()) continue;
				if (!a->Is3DLoaded()) continue;
				if (a->GetFormID() == player->GetFormID()) continue;
				if (a->GetParentCell() != pCell) continue;
				if (!IsCombatSupportedAggressor(a)) continue;
				if (!IsStandingEnemyThresholdActor(a)) continue;
				if (!IsBleedSpaceCompatible(a, player)) continue;
				auto* currentTarget = ResolveCurrentCombatTarget(a);
				const bool hostileToPlayer = e.hostile || a->IsHostileToActor(player);
				const bool targetsPlayer = currentTarget == player;
				const bool targetsFollower = currentTarget && IsActiveFollowerActor(currentTarget);
				const bool inCombat = e.inCombat || a->IsInCombat();
				if (!hostileToPlayer && !targetsPlayer && !targetsFollower && !inCombat) continue;

				float score = e.dist;
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
			TFD::AntiAggro::SweepOnce(sweepRadius, true);
			TFD::AntiAggro::ScheduleWaves(sweepRadius, true, 12, 120);
			TFD::ActorScan::Rescan(sweepRadius, false);
			const auto n = TFD::ActorScan::GetCount();
			std::size_t applied = 0;
			for (int i = 0; i < n; ++i) {
				auto e = TFD::ActorScan::GetEntry(i);
				auto sp = e.actor.get();
				auto* a = sp.get();
				if (!a || a->IsDead() || a->IsDisabled()) continue;
				if (!a->Is3DLoaded()) continue;
				if (a->GetFormID() == player->GetFormID()) continue;
				if (pCell && a->GetParentCell() != pCell) continue;
				if (!e.hostile && !e.inCombat && !a->IsInCombat()) continue;

				if (auto* process = RE::ProcessLists::GetSingleton()) {
					const bool runDetection = process->runDetection;
					process->runDetection = false;
					process->ClearCachedFactionFightReactions();
					process->StopCombatAndAlarmOnActor(a, false);
					process->runDetection = runDetection;
				}

				TFD::AggressionClamp::Apply(a);
				a->StopCombat();
				if (a->IsWeaponDrawn()) {
					a->DrawWeaponMagicHands(false);
				}
				a->EvaluatePackage(true, false);
				++applied;
			}

			spdlog::info("[TFD][Defeat] calm bubble same-cell applied={} radius={:.0f}", applied, sweepRadius);
		}

		static void RecoverPlayerAfterTeleport()
		{
			auto* p = Player();
			if (!p) return;
			ReleasePlayerBleedLock("recover_after_teleport", false);
			p->NotifyAnimationGraph("BleedoutStop");
			p->NotifyAnimationGraph("GetUpStart");
			const float hpMax = (std::max)(1.0f, p->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float threshPct = std::clamp(TFD::Settings::GetDefeatThresholdPct() / 100.0f, 0.05f, 0.95f);
			const float safePct = std::clamp(threshPct + 0.17f, 0.38f, 0.85f);
			const float target = (std::max)(45.0f, hpMax * safePct);
			const float hpNow = p->GetActorValue(RE::ActorValue::kHealth);
			if (hpNow < target) {
				p->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, (target - hpNow));
			}
			const float staminaMax = (std::max)(1.0f, p->GetPermanentActorValue(RE::ActorValue::kStamina));
			const float staminaTarget = (std::max)(30.0f, staminaMax * 0.40f);
			const float staminaNow = p->GetActorValue(RE::ActorValue::kStamina);
			if (staminaNow < staminaTarget) {
				p->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina, (staminaTarget - staminaNow));
			}
			if (p->IsInCombat()) p->StopCombat();
			p->DrawWeaponMagicHands(false);
			SetPlayerBleedImmune(false);
		}

		static void RecoverPlayerForTransition()
		{
			auto* p = Player();
			if (!p) return;
			ReleasePlayerBleedLock("recover_for_transition", false);
			p->NotifyAnimationGraph("BleedoutStop");
			p->NotifyAnimationGraph("GetUpStart");
			auto restoreToPct = [&](RE::ActorValue av, float pct, float minValue) {
				const float maxValue = (std::max)(1.0f, p->GetPermanentActorValue(av));
				const float target = (std::max)(minValue, maxValue * pct);
				const float nowValue = p->GetActorValue(av);
				if (nowValue < target) {
					p->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, av, (target - nowValue));
				}
				};
			const float threshPct = std::clamp(TFD::Settings::GetDefeatThresholdPct() / 100.0f, 0.05f, 0.95f);
			const float safeHealthPct = std::clamp(threshPct + 0.12f, 0.58f, 1.00f);
			restoreToPct(RE::ActorValue::kHealth, safeHealthPct, 45.0f);
			restoreToPct(RE::ActorValue::kStamina, 0.98f, 40.0f);
			restoreToPct(RE::ActorValue::kMagicka, 0.95f, 25.0f);
			if (p->IsInCombat()) p->StopCombat();
			p->DrawWeaponMagicHands(false);
			SetPlayerBleedImmune(false);
		}

		static void TickLeftForDeadCooldown()
		{
			if (!IsLeftForDeadCooldownActive()) return;
			MaintainFollowerHold();
			const auto now = Now();
			if (g_leftForDeadNextPulse.time_since_epoch().count() != 0 && now < g_leftForDeadNextPulse) return;
			g_leftForDeadNextPulse = now + std::chrono::milliseconds(900);
			MaintainTransitionCalmWindow();
		}

		static void EnterNonCaptiveChoice(const char* reason);
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
			handlers.isCaptiveEscapePhase = []() { return g_captiveState && g_captivePhase == CaptivePhaseValue::Escape; };
			handlers.isDialogueOpen = []() { return IsDialogueOpen(); };
			handlers.resolveSpeakerFromRuntime = []() -> RE::Actor* {
				if (g_bleedSpeakerId == 0) {
					return nullptr;
				}
				return RE::TESForm::LookupByID<RE::Actor>(g_bleedSpeakerId);
			};
			handlers.resolveAggressor = []() -> RE::Actor* { return ResolveAggressor(); };
			handlers.findBestAggressor = [](float radius) -> RE::Actor* { return FindBestAggressor(radius); };
			handlers.releaseNoSpeakerTameSession = [](const char* reason) { ReleaseBleedNoSpeakerTameSession(reason); };
			handlers.releaseTruceSession = []() { ReleaseBleedTruceSession(TFD::Pacify::ReleaseReason::Generic); };
			handlers.startTruceSessionForSpeaker = [](RE::Actor* player, RE::Actor* aggressor, const char* reason) {
				return StartBleedTruceSessionForSpeaker(player, aggressor, reason);
			};
			handlers.resetSpeakerKick = []() {
				g_bleedSpeakerKickLast = {};
				g_bleedSpeakerKickCount = 0;
			};
			handlers.resetGreetRuntime = [](const char* reason) { TFD::BleedoutGreet::ResetRuntime(reason); };
			handlers.beginGreet = [](RE::Actor* actor, const char* reason) { TFD::BleedoutGreet::Begin(actor, reason); };
			return TFD::Bleedout::BeginDialogueHotkey((std::max)(2400.0f, TFD::Settings::GetSweepRadius()), 1800.0f, handlers);
		}

		static void EnterEscapeFromLockpick(RE::TESObjectREFR* door)
		{
			EnterEscapeCommit("lockpick", door);
		}

		static bool BeginResolvedNoMarkerFallback(const char* reason)
		{
			if (!TryBeginBleedTerminalCommit(BleedTerminalCommit::NonCaptiveFallback, reason ? reason : "noncaptive_fallback")) {
				return false;
			}
			ClearPendingCinematicFadeIn();
			ClearEscapeContext();
			ResetLockpickWatch();
			g_grace.store(false, std::memory_order_release);
			g_prevDialogueOpen = false;
			g_prevLockpickOpen = false;
			SetCaptiveRuntime(false, CaptivePhaseValue::None);

			auto* player = Player();
			if (player && player->IsWeaponDrawn()) {
				player->DrawWeaponMagicHands(false);
			}

			const auto branch = ResolveNoMarkerFallback(reason);
			if (branch == NoMarkerFallbackBranch::None) {
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
				NoMarkerBranchName(branch),
				reason ? reason : "unknown");

			if (branch == NoMarkerFallbackBranch::RescueCached) {
				if (!BeginRescueTransition(reason ? reason : "rescue_cached")) {
					g_noMarkerFallback.branch = NoMarkerFallbackBranch::LeftForDeadSolo;
					g_noMarkerFallback.destination.reset();
					g_noMarkerFallback.hasFallbackPos = false;
					ResolveLeftForDeadDestination(g_noMarkerFallback);
					BeginRecoverTransition("rescue_cached_fallback_left_for_dead");
				}
				return true;
			}

			BeginRecoverTransition(reason ? reason : NoMarkerBranchName(branch));
			return true;
		}

		static void EnterNonCaptiveChoice(const char* reason)
		{
			(void)TFD::Bleedout::EnterNonCaptiveChoice(
				reason,
				TFD::Bleedout::NonCaptiveChoiceHandlers{
					[&]() {
						const auto activeCommit = GetBleedTerminalCommit();
						return activeCommit != BleedTerminalCommit::None && activeCommit != BleedTerminalCommit::NonCaptiveFallback;
					},
					[&]() {
						return BleedTerminalCommitName(GetBleedTerminalCommit());
					},
					[&](const char* r) { return BeginResolvedNoMarkerFallback(r); },
					[&](const char* r) { TFD::Bleedout::ClearBridgeAliases(nullptr, r); },
					[&]() { ClearPendingCinematicFadeIn(); },
					[&]() { TFD::FactionMask::Clear(); },
					[&]() { ClearEscapeContext(); },
					[&]() { ResetLockpickWatch(); },
					[&](bool active) { g_grace.store(active, std::memory_order_release); },
					[&]() { g_lastAggressor.reset(); },
					[&]() { ResetBleedRuntimeState(); },
					[&](bool v) { g_prevDialogueOpen = v; },
					[&](bool v) { g_prevLockpickOpen = v; },
					[&](bool captive) { SetCaptiveRuntime(captive, captive ? CaptivePhaseValue::Captive : CaptivePhaseValue::None); },
					[&](bool immune) { SetPlayerBleedImmune(immune); },
					[&](const char* r) { QueueNonCaptiveChoiceRequest(r); },
					[&](const char* r) { TFD::Flow::Controller::GetSingleton().ResetRuntime(r ? r : "noncaptive_choice"); }
				});
		}

		static void DoBlackoutTeleport()
		{
			(void)TFD::Bleedout::DoBlackoutTeleport(
				"blackout_teleport",
				TFD::Bleedout::BlackoutHandlers{
					[&]() {
						const auto activeCommit = GetBleedTerminalCommit();
						return activeCommit != BleedTerminalCommit::None && activeCommit != BleedTerminalCommit::Captive;
					},
					[&]() {
						return BleedTerminalCommitName(GetBleedTerminalCommit());
					},
					[&]() { return ResolveCaptiveMarkerForOutcome(); },
					[&](const char* r) { EnterNonCaptiveChoice(r); },
					[&](const char* r) {
						const auto activeCommit = GetBleedTerminalCommit();
						if (activeCommit == BleedTerminalCommit::None) {
							return TryBeginBleedTerminalCommit(BleedTerminalCommit::Captive, r);
						}
						return true;
					},
					[&]() { ResetBleedRuntimeState(); },
					[&](const char* r) { TFD::Bleedout::ClearBridgeAliases(nullptr, r); },
					[&]() { g_lastAggressor.reset(); },
					[&]() {
						RE::DebugNotification("TFDEngine: Blackout -> Captive (1h)");
						(void)TFD::Flow::Controller::GetSingleton().BeginCaptive(ResolveBleedFlowActorFormID(), TFD::Flow::CaptiveMode::Kidnapped, "bleed_blackout_teleport");
					},
					[&]() { ClearPendingCinematicFadeIn(); },
					[&]() { return QueueCinematicTransitionRequest(CinematicTransitionKind::Captive, false, "captive_blackout"); },
					[&]() { ShowBlackoutFader(); },
					[&](const char* r) { CompleteCaptiveTransitionNow(r); },
					[&]() { HideBlackoutFader(); }
				});
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
			SetCaptiveRuntime(true, CaptivePhaseValue::Captive);
			SyncPlayerCaptiveAlias(Player(), reason ? reason : "captive_pleasure_prepare");
		}

		static void CompleteCaptivePleasureHandoff(const char* reason)
		{
			(void)TFD::Bleedout::CompleteCaptivePleasureHandoff(
				reason,
				TFD::Bleedout::CompletionHandlers{
					[&](const char* r) { ClearBleedSystemEventOutcomeWindow(r); },
					[&](BleedTerminalCommit kind, const char* r) { return TryBeginBleedTerminalCommit(kind, r); },
					[&]() { ClearPendingCinematicFadeIn(); },
					[&](const char* r) { TFD::Bleedout::ClearBridgeAliases(nullptr, r); },
					{},
					[&]() { ClearEscapeContext(); },
					[&]() { ResetLockpickWatch(); },
					[&](bool active) { g_grace.store(active, std::memory_order_release); },
					[&]() { g_lastAggressor.reset(); },
					[&](bool preserve) { ResetBleedRuntimeState(preserve); },
					{},
					[&](bool captive) { SetCaptiveRuntime(captive, captive ? CaptivePhaseValue::Captive : CaptivePhaseValue::None); },
					[&](const char* r) { SyncPlayerCaptiveAlias(Player(), r); },
					[&](bool immune) { SetPlayerBleedImmune(immune); },
					[&]() { RecoverPlayerForTransition(); },
					[&]() { return TFD::Settings::GetSweepRadius(); },
					[&](float radius) { ApplyCalmBubble(radius); },
					{},
					{},
					{},
					[&]() { UpdatePreCombatState(); },
					[&]() -> RE::Actor* { return g_bleedSpeakerId != 0 ? RE::TESForm::LookupByID<RE::Actor>(g_bleedSpeakerId) : nullptr; },
					[&](RE::Actor* speaker, bool captive, const char* why) {
						(void)TFD::PleasureRuntime::BeginPleasure(speaker,
							captive ? TFD::PleasureRuntime::SourceContext::Captive : TFD::PleasureRuntime::SourceContext::Bleedout,
							why);
					},
					[&](bool v) { g_prevDialogueOpen = v; },
					[&](bool v) { g_prevLockpickOpen = v; }
				});
		}

		static void CompleteBleedPayRelease(const char* reason)
		{
			(void)TFD::Bleedout::CompletePayRelease(
				reason,
				TFD::Bleedout::CompletionHandlers{
					[&](const char* r) { ClearBleedSystemEventOutcomeWindow(r); },
					[&](BleedTerminalCommit kind, const char* r) { return TryBeginBleedTerminalCommit(kind, r); },
					[&]() { ClearPendingCinematicFadeIn(); },
					[&](const char* r) { TFD::Bleedout::ClearBridgeAliases(nullptr, r); },
					[&]() { TFD::FactionMask::Clear(); },
					[&]() { ClearEscapeContext(); },
					[&]() { ResetLockpickWatch(); },
					[&](bool active) { g_grace.store(active, std::memory_order_release); },
					[&]() { g_lastAggressor.reset(); },
					[&](bool preserve) { if (preserve) ResetBleedRuntimeState(true); else ResetBleedRuntimeState(); },
					{},
					[&](bool captive) { SetCaptiveRuntime(captive, captive ? CaptivePhaseValue::Captive : CaptivePhaseValue::None); },
					{},
					[&](bool immune) { SetPlayerBleedImmune(immune); },
					[&]() { RecoverPlayerForTransition(); },
					[&]() { return TFD::Settings::GetSweepRadius(); },
					[&](float radius) { ApplyCalmBubble(radius); },
					[&](int secs) { BeginLeftForDeadCooldown(secs); },
					[&](int secs) { SetGraceSeconds(secs); },
					{},
					[&]() { UpdatePreCombatState(); },
					{},
					{},
					[&](bool v) { g_prevDialogueOpen = v; },
					[&](bool v) { g_prevLockpickOpen = v; }
				});
		}

		static void CompleteBleedPleasureHandoff(const char* reason)
		{
			const char* why = reason ? reason : "bleed_pleasure_handoff";
			const bool ok = TFD::Bleedout::CompleteBleedPleasureHandoff(
				why,
				TFD::Bleedout::CompletionHandlers{
					[&](const char* r) { ClearBleedSystemEventOutcomeWindow(r); },
					[&](BleedTerminalCommit kind, const char* r) { return TryBeginBleedTerminalCommit(kind, r); },
					[&]() { ClearPendingCinematicFadeIn(); },
					[&](const char* r) { TFD::Bleedout::ClearBridgeAliases(nullptr, r); },
					{},
					[&]() { ClearEscapeContext(); },
					[&]() { ResetLockpickWatch(); },
					[&](bool active) { g_grace.store(active, std::memory_order_release); },
					{},
					{},
					[&](const char* r) { TransitionBleedRuntimeToPleasureCommit(r); g_lastRouterCombatContextActive = false; },
					[&](bool captive) { SetCaptiveRuntime(captive, captive ? CaptivePhaseValue::Captive : CaptivePhaseValue::None); },
					{},
					[&](bool immune) { SetPlayerBleedImmune(immune); },
					[&]() { RecoverPlayerForTransition(); },
					[&]() { return TFD::Settings::GetSweepRadius(); },
					[&](float radius) { ApplyCalmBubble(radius); },
					[&](int secs) { BeginLeftForDeadCooldown(secs); },
					[&](int secs) { SetGraceSeconds(secs); },
					[&]() { RefreshPostDefeatGlobals(); },
					[&]() { UpdatePreCombatState(); },
					[&]() -> RE::Actor* { return g_bleedSpeakerId != 0 ? RE::TESForm::LookupByID<RE::Actor>(g_bleedSpeakerId) : nullptr; },
					[&](RE::Actor* speaker, bool captive, const char* why2) {
						(void)TFD::PleasureRuntime::BeginPleasure(speaker,
							captive ? TFD::PleasureRuntime::SourceContext::Captive : TFD::PleasureRuntime::SourceContext::Bleedout,
							why2);
					},
					[&](bool v) { g_prevDialogueOpen = v; },
					[&](bool v) { g_prevLockpickOpen = v; }
				});
			if (ok) {
				spdlog::info("[TFD][Defeat] bleed pleasure handoff complete reason={} preserveStabilization=1 clearFlow=0 preserveFlowOwner=1 speaker={:08X} session={} captor={:08X}",
					why,
					g_bleedSpeakerId,
					g_bleedTruceSessionId != 0 ? 1 : 0,
					TFD::Bleedout::GetActiveCaptorFormID());
			}
		}

		static void UpdateLockpickEscapeWatch()
		{
			if (!g_captiveState || g_captivePhase != CaptivePhaseValue::Captive) {
				ResetLockpickWatch();
				return;
			}
			const bool lockOpen = IsLockpickingOpen();
			if (lockOpen && !g_prevLockpickOpen) {
				auto* rawTarget = RE::LockpickingMenu::GetTargetReference();
				auto* target = ResolveLockpickDoorCandidate(rawTarget);
				if (target) {
					g_lockpickDoorCandidate = target->GetHandle();
					g_lockpickDoorWasLocked = IsRefLocked(target);
					if (g_lockpickDoorWasLocked) BindCaptiveDoor(target);
					spdlog::info("[TFD][Captive] lockpick opened on door {:08X} wasLocked={} nearMarker=1 rawTarget={:08X}", target->GetFormID(), g_lockpickDoorWasLocked ? 1 : 0, rawTarget ? rawTarget->GetFormID() : 0);
				}
				else {
					g_lockpickDoorCandidate.reset();
					g_lockpickDoorWasLocked = false;
					auto* boundDoor = ResolveBoundEscapeDoor();
					if (rawTarget) {
						spdlog::info("[TFD][Captive] lockpick target {:08X} ignored (door={} nearMarker={} fallbackBoundDoor={:08X})", rawTarget->GetFormID(), IsDoorRef(rawTarget) ? 1 : 0, IsDoorNearCaptiveMarker(rawTarget) ? 1 : 0, boundDoor ? boundDoor->GetFormID() : 0);
					}
					else {
						spdlog::info("[TFD][Captive] lockpick target null (fallbackBoundDoor={:08X})", boundDoor ? boundDoor->GetFormID() : 0);
					}
				}
			}
			if (g_captiveDoor.HasDoor() && g_captiveDoor.UpdateWatcher()) {
				EnterEscapeCommit("door_watch", nullptr);
			}
			if (!lockOpen && g_prevLockpickOpen) {
				RE::TESObjectREFR* door = nullptr;
				if (g_lockpickDoorCandidate) {
					auto ptr = g_lockpickDoorCandidate.get();
					door = ptr.get();
				}
				if (!door) door = ResolveBoundEscapeDoor();
				if (door && IsDoorRef(door) && g_lockpickDoorWasLocked && !IsRefLocked(door)) {
					EnterEscapeFromLockpick(door);
				}
				else {
					ResetLockpickWatch();
				}
			}
			g_prevLockpickOpen = lockOpen;
		}

		static RE::Actor* ResolveEscapeBreakPreferredAggressor(float radius)
		{
			auto* player = Player();
			if (!player) {
				return nullptr;
			}

			if (g_escapeBreakPreferredAggressor) {
				auto sp = RE::Actor::LookupByHandle(g_escapeBreakPreferredAggressor.native_handle());
				auto* actor = sp.get();
				float dist = -1.0f;
				if (IsReasonableCombatAggressor(actor, player, radius, &dist) ||
					IsReasonableBleedoutSpeaker(actor, player, 1400.0f, &dist)) {
					return actor;
				}
				g_escapeBreakPreferredAggressor.reset();
			}

			if (auto* aggressor = ResolveAggressor()) {
				return aggressor;
			}
			return FindBestAggressor(radius);
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
			state.bleedLastCrowdAssign = &g_bleedLastCrowdAssign;
			state.bleedCrowdAssigned = reinterpret_cast<std::vector<std::uint32_t>*>(&g_bleedCrowdAssigned);
			state.bleedRejectedSpeakerIds = reinterpret_cast<std::unordered_set<std::uint32_t>*>(&g_bleedRejectedSpeakerIds);
			state.bleedSpeakerId = &g_bleedSpeakerId;
			state.bleedSpeakerKickLast = &g_bleedSpeakerKickLast;
			state.bleedSpeakerKickCount = &g_bleedSpeakerKickCount;
			state.bleedDialogueRetryCount = &g_bleedDialogueRetryCount;
			state.escapeBreakBleedPending = &g_escapeBreakBleedPending;
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

		static TFD::Bleedout::RuntimeHostHandlers BuildBleedRuntimeHostHandlers()
		{
			TFD::Bleedout::RuntimeHostHandlers handlers{};
			handlers.clearTerminalCommit = [](const char* reason) { ClearBleedTerminalCommit(reason); };
			handlers.clearBridgeAliasesForActor = [](RE::Actor* actor, const char* reason) { TFD::Bleedout::ClearBridgeAliases(actor, reason); };
			handlers.clearNoMarkerFallbackState = []() { ClearNoMarkerFallbackState(); };
			handlers.releaseNoSpeakerTameSession = [](const char* reason) { ReleaseBleedNoSpeakerTameSession(reason); };
			handlers.clearBleedSupportBridgeAliases = [](const char* reason) { ClearBleedSupportBridgeAliases(reason); };
			handlers.releaseTruceSession = []() { ReleaseBleedTruceSession(TFD::Pacify::ReleaseReason::Generic); };
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
			handlers.applyFactionMaskFromAggressor = [](RE::Actor* actor) { return TFD::FactionMask::ApplyFromAggressor(actor); };
			handlers.resolveCaptiveMarkerForOutcome = []() { return ResolveCaptiveMarkerForOutcome(); };
			handlers.canUseCaptiveFallbackHeuristic = [](RE::Actor* player, RE::Actor* aggressor, bool hasCaptiveOutcome, float* outDistance) {
				if (!outDistance) {
					float dummy = 0.0f;
					return CanUseCaptiveFallbackHeuristic(player, aggressor, hasCaptiveOutcome, dummy);
				}
				return CanUseCaptiveFallbackHeuristic(player, aggressor, hasCaptiveOutcome, *outDistance);
			};
			handlers.tryEnsureNoSpeakerTameSession = [](const std::vector<RE::Actor*>& actors, const char* reason) { return TryEnsureBleedNoSpeakerTameSession(actors, reason); };
			handlers.setLastAggressor = [](RE::Actor* actor) { g_lastAggressor = actor ? actor->GetHandle() : RE::ActorHandle{}; };
			handlers.debugNotification = [](const char* msg) { RE::DebugNotification(msg); };
			handlers.clearEnemyTargetsToPlayerForDefeat = [](RE::Actor* player, float radius, const char* reason) { ClearEnemyTargetsToPlayerForDefeat(player, radius, reason); };
			handlers.resolveEscapeBreakPreferredAggressor = [](float radius) -> RE::Actor* { return ResolveEscapeBreakPreferredAggressor(radius); };
			handlers.startTruceSessionForSpeaker = [](RE::Actor* player, RE::Actor* speaker, const char* reason) { return StartBleedTruceSessionForSpeaker(player, speaker, reason); };
			handlers.canUseAggressorForBleedoutGreet = [](RE::Actor* player, RE::Actor* aggressor, float* outDistance) {
				if (!outDistance) {
					float dummy = 0.0f;
					return CanUseAggressorForBleedoutGreet(player, aggressor, dummy);
				}
				return CanUseAggressorForBleedoutGreet(player, aggressor, *outDistance);
			};
			handlers.applyForceGreetOverdrive = [](RE::Actor* player, RE::Actor* speaker, const char* reason, bool restart) { ApplyBleedForceGreetOverdrive(player, speaker, reason, restart); };
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
			PollTransitionResult();
			ProcessPendingCinematicFadeIn();
			ProcessPendingCaptiveConfiscation();
			if (IsTransitionAwaiting() || g_pendingCinematicFadeIn) {
				MaintainTransitionCalmWindow();
				UpdatePreCombatState();
				return;
			}
			RefreshPostDefeatGlobals();
			NormalizeInvalidCaptivePair();
			const bool captiveBleedOverlay = g_escapeBreakBleedPending || g_inBleedState.load(std::memory_order_acquire);
			const bool inCombatDialogueOpen = IsDialogueOpen();
			if (!captiveBleedOverlay && TFD::InCombat::IsActive() && inCombatDialogueOpen) {
				TFD::InCombatGreet::NotifyDialogueOpened();
			}
			if (g_captiveState && g_captivePhase == CaptivePhaseValue::Captive) {
				const bool dialogOpen = IsDialogueOpen();
				if (!dialogOpen && g_prevDialogueOpen) {
					spdlog::info("[TFD][Captive] Dialogue closed -> no implicit action");
				}
				g_prevDialogueOpen = dialogOpen;
				if (!captiveBleedOverlay) {
					UpdateLockpickEscapeWatch();
					TryCommitEscapeByRadius();
				}
			}
			else if (g_captiveState && g_captivePhase == CaptivePhaseValue::Escape) {
				TryResolveEscapeByLocation();
				if (g_captiveState && g_captivePhase == CaptivePhaseValue::Escape && !BreakEscapeOnDefeatThreshold(Player())) {
					return;
				}
			}

			if (g_captiveState && !captiveBleedOverlay) {
				return;
			}

			if (ui && ui->GameIsPaused()) return;
			auto* player = Player();
			if (!player) {
				RefreshPostDefeatGlobals();
				return;
			}
			TFD::ForceGreet::Tick();
			TFD::PleasureRuntime::Tick();
			TFD::Location::UpdateAmbientKidnapAvailability(false);
			MaintainReleaseFollowGrace();
			if (HandlePendingEscapeBreakBleed()) {
				return;
			}
			TickBleedLocks();
			UpdatePreCombatState();
			if (IsLeftForDeadCooldownActive()) {
				TickLeftForDeadCooldown();
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
					else if (TFD::BleedoutGreet::TryAfterPleasureWatchdog(g_prevDialogueOpen, dOpen, g_bleedSpeakerId, nowBleedHold,
						[&](const char* reopenReason) -> bool {
							auto* reopenSpeaker = RE::TESForm::LookupByID<RE::Actor>(g_bleedSpeakerId);
							float reopenDist = 99999.0f;
							if (!(player && reopenSpeaker && CanUseAggressorForBleedoutGreet(player, reopenSpeaker, reopenDist))) {
								return false;
							}
							ApplyBleedForceGreetOverdrive(player, reopenSpeaker, reopenReason, true);
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
					if (TFD::BleedoutGreet::TryTimeoutRearm(g_prevDialogueOpen, HasBleedTerminalCommit(), TFD::PleasureRuntime::IsBlocking(), g_bleedSpeakerId, nowBleed,
						[&](const char* reopenReason) -> bool {
							auto* timeoutSpeaker = RE::TESForm::LookupByID<RE::Actor>(g_bleedSpeakerId);
							float timeoutDist = 99999.0f;
							if (!(player && timeoutSpeaker && CanUseAggressorForBleedoutGreet(player, timeoutSpeaker, timeoutDist))) {
								return false;
							}
							ApplyBleedForceGreetOverdrive(player, timeoutSpeaker, reopenReason, true);
							g_bleedPauseStarted = nowBleed;
							g_bleedLastSeconds = -1;
							g_bleedSpeakerKickLast = {};
							g_bleedSpeakerKickCount = 0;
							spdlog::info("[TFD][Defeat] bleed forcegreet timeout rearm speaker={:08X} dist={:.1f}",
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
					if (TFD::BleedoutGreet::TryStickyWatchdog(HasBleedTerminalCommit(), IsDialogueOpen(), TFD::PleasureRuntime::IsBlocking(), g_bleedSpeakerId, nowBleed,
						[&](const char* reopenReason) -> bool {
							auto* reopenSpeaker = RE::TESForm::LookupByID<RE::Actor>(g_bleedSpeakerId);
							float reopenDist = 99999.0f;
							if (!(player && reopenSpeaker && CanUseAggressorForBleedoutGreet(player, reopenSpeaker, reopenDist))) {
								return false;
							}
							ApplyBleedForceGreetOverdrive(player, reopenSpeaker, reopenReason, true);
							spdlog::info("[TFD][Defeat] bleed sticky watchdog reopen speaker={:08X} dist={:.1f}",
								reopenSpeaker->GetFormID(),
								reopenDist);
							return true;
						})) {
						return;
					}
				}

				if (g_bleedSpeakerId == 0) {
					const auto nowBleed = Now();
					if (g_bleedNoSpeakerTameLastAttempt.time_since_epoch().count() == 0 || (nowBleed - g_bleedNoSpeakerTameLastAttempt) >= std::chrono::milliseconds(900)) {
						g_bleedNoSpeakerTameLastAttempt = nowBleed;
						const float bleedRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius());
						auto crowd = CollectBleedoutCrowd(bleedRadius, nullptr, false);
						const bool tameHeld = TryEnsureBleedNoSpeakerTameSession(crowd, "bleed_tick_no_speaker");
						if (tameHeld) {
							spdlog::info("[TFD][Defeat] bleed no-speaker tick tameHeld=1 crowdSize={}", crowd.size());
						}
					}
				}
				if (TFD::Bleedout::TryHandlePayReleaseDialogueClosed(
					TFD::BleedoutGreet::HasSeenDialogue(),
					g_prevDialogueOpen,
					TFD::Bleedout::DialogueCloseHandlers{
						[&](const char* r) { ClearBleedDialogueOutcome(r); },
						[&](const char* r) { CompleteBleedPayRelease(r); }
					})) {
					g_prevDialogueOpen = false;
					return;
				}
				if (TFD::BleedoutGreet::HandleDialogueClosedFlow(
					TFD::BleedoutGreet::DialogueClosedContext{
						TFD::BleedoutGreet::HasSeenDialogue(),
						g_prevDialogueOpen,
						HasBleedTerminalCommit(),
						TFD::PleasureRuntime::IsBlocking(),
						TFD::Bleedout::GetDialogueOutcome() == BleedDialogueOutcome::Captive
					},
					player,
					g_bleedSpeakerId,
					TFD::BleedoutGreet::DialogueClosedHandlers{
						[&]() {
							g_prevDialogueOpen = false;
							g_bleedLastSeconds = -1;
							spdlog::info("[TFD][Defeat] bleed dialogue closed -> fallback suppressed activeTerminalCommit={}",
								BleedTerminalCommitName(GetBleedTerminalCommit()));
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
							ReleaseBleedTruceSession(TFD::Pacify::ReleaseReason::FlowHandoff);
							if (ResolveCaptiveMarkerForOutcome()) {
								spdlog::info("[TFD][Defeat] bleedout dialogue closed after captive outcome -> captive marker found");
								DoBlackoutTeleport();
								SetGraceSeconds(4);
							}
 else {
  spdlog::info("[TFD][Defeat] bleedout dialogue closed after captive outcome -> no marker -> resolve fallback");
  ReleaseBleedNoSpeakerTameSession("dialogue_closed_captive_no_marker");
  g_inBleedState.store(false, std::memory_order_release);
  g_minHp = 0.0f;
  TFD::BleedoutGreet::ResetRuntime("bleed_reset");
  g_bleedLastSeconds = -1;
  EnterNonCaptiveChoice("dialogue_closed_captive_no_marker");
}
},
[&](const TFD::BleedoutGreet::StickyReopenProbe& probe) {
	g_prevDialogueOpen = false;
	TFD::BleedoutGreet::ResetRuntime("bleed_reset");
	g_bleedSpeakerKickCount = 0;
	g_bleedSpeakerKickLast = {};
	g_bleedLastSeconds = -1;
	ClearBleedSystemEventOutcomeWindow("dialogue_closed_sticky_reopen");
	auto* reopenSpeaker = probe.speakerFormID != 0 ? RE::TESForm::LookupByID<RE::Actor>(probe.speakerFormID) : nullptr;
	if (player && reopenSpeaker) {
		ApplyBleedForceGreetOverdrive(player, reopenSpeaker, "dialogue_closed_sticky_reopen", true);
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
	g_bleedSpeakerKickCount = 0;
	g_bleedSpeakerKickLast = {};
	g_bleedLastSeconds = -1;
	ClearBleedSystemEventOutcomeWindow("dialogue_closed_sticky_reopen");
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
					if (HasBleedTerminalCommit()) {
						TFD::BleedoutGreet::MarkStickyReopenPending(false, "terminal_commit_active");
						ClearBleedSystemEventOutcomeWindow("terminal_commit_active");
						return;
					}
					if (ResolveBleedPostDialogueSystemEventOutcome(player, "post_dialogue_system_event")) {
						return;
					}
					if (ResolveBleedPendingSystemEventFallback(player, "post_dialogue_system_event_window_expired")) {
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
					(void)IsBleedSystemEventPendingForFallback(&pendingSystemReason);
					if (TFD::Bleedout::HandleBleedTimeout(
						TFD::Bleedout::TimeoutContext{
							HasBleedTerminalCommit(),
							BleedTerminalCommitName(GetBleedTerminalCommit()),
							pendingSystemReason,
							g_bleedPendingCaptiveOutcome
						},
						"bleed_timeout",
						TFD::Bleedout::TimeoutHandlers{
							[&]() { ReleaseBleedTruceSession(TFD::Pacify::ReleaseReason::Generic); },
							[&]() { return ResolveCaptiveMarkerForOutcome(); },
							[&]() { ResetBleedRuntimeState(); },
							[&]() { DoBlackoutTeleport(); },
							[&](int seconds) { SetGraceSeconds(seconds); },
							[&](const char* r) { EnterNonCaptiveChoice(r); }
						})) {
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

		class DefeatedRecruitEventSink final : public RE::BSTEventSink<SKSE::ModCallbackEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* ev, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
			{
				if (!ev) {
					return RE::BSEventNotifyControl::kContinue;
				}
				std::string_view name(ev->eventName);
				if (name.empty() || name != kDefeatedHumanoidRecruitEvent) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto* actor = ResolvePendingDefeatedDialogueTargetInternal();
				if (!actor) {
					spdlog::warn("[TFD][Defeat] defeated humanoid recruit event ignored reason=no_pending_target");
					return RE::BSEventNotifyControl::kContinue;
				}
				const bool ok = RecruitDefeatedHumanoidAsTeammate(actor);
				spdlog::info("[TFD][Defeat] defeated humanoid recruit event actor={:08X} ok={}", actor->GetFormID(), ok ? 1 : 0);
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		DefeatedRecruitEventSink g_defeatedRecruitEventSink{};

		class BleedOutcomeEventSink final : public RE::BSTEventSink<SKSE::ModCallbackEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* ev, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
			{
				if (!ev) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto* rawName = ev->eventName.c_str();
				const std::string_view name = rawName ? std::string_view(rawName) : std::string_view{};
				if (name.empty()) {
					return RE::BSEventNotifyControl::kContinue;
				}

				(void)TFD::PleasureRuntime::HandleModEvent(ev->eventName, ev->strArg, ev->numArg, ev->sender);

				if (name == kAfterPleasureEnterEvent) {
					auto* actor = ResolveActorFromEventArg(ev->strArg.c_str() ? std::string_view(ev->strArg.c_str()) : std::string_view{});
					const int sourceFlow = ResolveSourceFlowFromEventArg(ev->strArg.c_str() ? std::string_view(ev->strArg.c_str()) : std::string_view{});
					const auto snapshot = TFD::Flow::Controller::GetSingleton().GetSnapshot();
					const bool inCombatAfterPleasure =
						snapshot.root == TFD::Flow::RootFlow::InCombat ||
						snapshot.sub == TFD::Flow::SubFlow::InCombatPleasure ||
						snapshot.sub == TFD::Flow::SubFlow::InCombatAfterPleasure;
					if (actor && sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::Bleedout)) {
						if (TFD::Bleedout::HandleAfterPleasureEnter(actor, "after_pleasure_enter")) {
							TFD::BleedoutGreet::BeginAfterPleasure(actor, "after_pleasure_enter");
							spdlog::info("[TFD][Defeat] bleedout after pleasure greet armed source={} actor={:08X}", sourceFlow, actor->GetFormID());
						}
						else {
							spdlog::warn("[TFD][Defeat] bleedout after pleasure flow reject source={} actor={:08X}", sourceFlow, actor->GetFormID());
						}
					}
					else if (actor && inCombatAfterPleasure) {
						(void)TFD::InCombat::HandleAfterPleasureEnter(
							actor,
							"after_pleasure_enter",
							TFD::InCombat::AfterPleasureHandlers{
								[&](std::uint32_t actorFormID, const char* r) { TFD::InCombat::NoteAfterPleasure(actorFormID, r); },
								[&](RE::Actor* greetActor, const char* r) -> bool { return TFD::InCombatGreet::BeginAfterPleasure(greetActor, r); }
							});
						spdlog::info("[TFD][Defeat] incombat after pleasure greet armed source={} actor={:08X}", sourceFlow, actor->GetFormID());
					}
					else if (actor && sourceFlow == static_cast<int>(TFD::PleasureRuntime::SourceContext::Captive)) {
						TFD::ForceGreet::BeginAfterPleasure(actor);
						spdlog::info("[TFD][Defeat] after pleasure greet armed source={} actor={:08X}", sourceFlow, actor->GetFormID());
					}
					return RE::BSEventNotifyControl::kContinue;
				}

				if (name == kInCombatOutcomeReleaseEvent ||
					name == kInCombatOutcomeFollowEvent) {
					auto* actor = ResolveActorFromEventArg(ev->strArg.c_str() ? std::string_view(ev->strArg.c_str()) : std::string_view{});
					TFD::InCombat::GraceEventContext context{};
					context.eventName = rawName;
					context.actor = actor;
					context.durationSec = ev->numArg > 0.0f ? static_cast<double>(ev->numArg) : 20.0;
					(void)TFD::InCombat::HandleReleaseFollowEvent(
						context,
						TFD::InCombat::GraceEventHandlers{
							[&](RE::Actor* graceActor, double seconds, const char* graceReason) {
								ApplyReleaseFollowGraceToSpeakerAndCrowd(graceActor, seconds, graceReason);
							}
						});
					return RE::BSEventNotifyControl::kContinue;
				}

				if (TFD::PreCombatGreet::HandleModCallbackEvent(
					ev,
					TFD::PreCombatGreet::GraceEventHandlers{
						[&](RE::Actor* graceActor, double seconds, const char* graceReason) {
							ApplyReleaseFollowGraceToSpeakerAndCrowd(graceActor, seconds, graceReason);
						},
						[&](RE::Actor* graceActor, const char* graceReason) {
							RemoveReleaseFollowGraceFromSpeakerAndCrowd(graceActor, graceReason);
						}
					})) {
					return RE::BSEventNotifyControl::kContinue;
				}

				if (name == kBleedoutOutcomeReleaseEvent ||
					name == kPleasureOutcomeReleaseEvent) {
					auto* actor = ResolveActorFromEventArg(ev->strArg.c_str() ? std::string_view(ev->strArg.c_str()) : std::string_view{});
					const double durationSec = ev->numArg > 0.0f ? static_cast<double>(ev->numArg) : 20.0;
					if (!actor) {
						spdlog::warn("[TFD][Grace] event={} ignored reason=invalid_actor arg={}", std::string(name), ev->strArg.c_str() ? ev->strArg.c_str() : "");
						return RE::BSEventNotifyControl::kContinue;
					}
					const char* graceReason =
						name == kBleedoutOutcomeReleaseEvent ? "bleedout_release" : "pleasure_release";
					ApplyReleaseFollowGraceToSpeakerAndCrowd(actor, durationSec, graceReason);
					return RE::BSEventNotifyControl::kContinue;
				}

				const TFD::InCombat::OutcomeEventHandlers inCombatOutcomeEventHandlers{
					[&](const char* reason, double seconds) { ArmBleedSystemEventOutcomeWindow(reason, seconds); },
					[&](TFD::InCombat::DialogueOutcome outcome, const char* reason) { TFD::InCombat::SetDialogueOutcome(outcome, reason); },
					[&](const char* reason) { TFD::InCombat::ClearDialogueOutcome(reason); },
					[&](std::uint32_t actorFormID, const char* reason) -> bool {
							auto& flow = TFD::Flow::Controller::GetSingleton();
							return flow.BeginCaptive(actorFormID, TFD::Flow::CaptiveMode::Kidnapped, reason ? reason : "mod_event_captive");
					},
					[&](std::uint32_t actorFormID, const char* reason) -> bool {
							auto& flow = TFD::Flow::Controller::GetSingleton();
							if (!flow.BeginCaptive(actorFormID, TFD::Flow::CaptiveMode::Kidnapped, "mod_event_captive_pleasure_begin")) {
							spdlog::warn("[TFD][Defeat] ignore mod_event_pleasure reason=begin_captive_reject actor={:08X}", actorFormID);
							return false;
						}
						if (!flow.ResolveCaptiveOutcome(TFD::Flow::CaptiveOutcome::Pleasure, actorFormID, reason ? reason : "mod_event_pleasure")) {
							spdlog::warn("[TFD][Defeat] ignore mod_event_pleasure reason=captive_flow_reject actor={:08X}", actorFormID);
							return false;
						}
						return true;
					},
					[&](const char* reason) { PreparePlayerForCaptivePleasureScene(reason); },
					[&](const char* reason) { CompleteCaptivePleasureHandoff(reason); },
					[&](const char* reason) { PreparePlayerForBleedoutPleasureScene(reason); },
					[&](const char* reason) { CompleteBleedPleasureHandoff(reason); }
				};

				if (name == kInCombatOutcomePayEvent) {
					auto actorFormID = ResolveActorFormIDFromEventArg(ev->strArg.c_str() ? std::string_view(ev->strArg.c_str()) : std::string_view{});
					if (!actorFormID) {
						actorFormID = TFD::InCombat::GetPrimaryActorFormID();
					}
					auto* flowActor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
					TFD::InCombat::OutcomeEventContext context{};
					context.eventName = "mod_event_pay";
					context.rawEventName = rawName;
					context.actor = flowActor;
					context.actorFormID = actorFormID;
					context.inCombatState = TFD::InCombat::IsActive();
					(void)TFD::InCombat::HandleOutcomePayEvent(context, inCombatOutcomeEventHandlers);
					return RE::BSEventNotifyControl::kContinue;
				}

				if (name == kInCombatOutcomePleasureEvent) {
					auto actorFormID = ResolveActorFormIDFromEventArg(ev->strArg.c_str() ? std::string_view(ev->strArg.c_str()) : std::string_view{});
					if (!actorFormID) {
						actorFormID = TFD::InCombat::GetPrimaryActorFormID();
					}
					auto* flowActor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
					TFD::InCombat::OutcomeEventContext context{};
					context.eventName = "mod_event_pleasure";
					context.rawEventName = rawName;
					context.actor = flowActor;
					context.actorFormID = actorFormID;
					context.inCombatState = TFD::InCombat::IsActive();
					context.preserveCaptive = g_captiveState && g_captivePhase == CaptivePhaseValue::Captive;
					(void)TFD::InCombat::HandleOutcomePleasureEvent(context, inCombatOutcomeEventHandlers);
					return RE::BSEventNotifyControl::kContinue;
				}

				if (name == kInCombatOutcomeCaptiveEvent) {
					auto actorFormID = ResolveActorFormIDFromEventArg(ev->strArg.c_str() ? std::string_view(ev->strArg.c_str()) : std::string_view{});
					if (!actorFormID) {
						actorFormID = TFD::InCombat::GetPrimaryActorFormID();
					}
					auto* flowActor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
					TFD::InCombat::OutcomeEventContext context{};
					context.eventName = "mod_event_captive";
					context.rawEventName = rawName;
					context.actor = flowActor;
					context.actorFormID = actorFormID;
					context.inCombatState = TFD::InCombat::IsActive();
					(void)TFD::InCombat::HandleOutcomeCaptiveEvent(context, inCombatOutcomeEventHandlers);
					return RE::BSEventNotifyControl::kContinue;
				}

				if (name == kInCombatOutcomeResetEvent) {
					TFD::InCombat::OutcomeEventContext context{};
					context.eventName = "mod_event_reset";
					context.rawEventName = rawName;
					(void)TFD::InCombat::HandleOutcomeResetEvent(context, inCombatOutcomeEventHandlers);
					return RE::BSEventNotifyControl::kContinue;
				}

				const TFD::Bleedout::OutcomeEventHandlers bleedOutcomeEventHandlers{
					[&](const char* eventName) { return ShouldDropBleedSystemEventBecauseFallback(eventName); },
					[&](const char* reason, double seconds) { ArmBleedSystemEventOutcomeWindow(reason, seconds); },
					[&](TFD::Bleedout::DialogueOutcome outcome, const char* reason) { SetBleedDialogueOutcome(outcome, reason); },
					[&](const char* reason) { ClearBleedDialogueOutcome(reason); },
					[&](const char* reason) { ClearBleedSystemEventOutcomeWindow(reason); },
					[&](std::uint32_t actorFormID, const char* reason) -> bool {
							auto& flow = TFD::Flow::Controller::GetSingleton();
							if (!flow.BeginCaptive(actorFormID, TFD::Flow::CaptiveMode::Kidnapped, "mod_event_captive_pleasure_begin")) {
							spdlog::warn("[TFD][Defeat] ignore mod_event_pleasure reason=begin_captive_reject actor={:08X}", actorFormID);
							return false;
						}
						if (!flow.ResolveCaptiveOutcome(TFD::Flow::CaptiveOutcome::Pleasure, actorFormID, reason ? reason : "mod_event_pleasure")) {
							spdlog::warn("[TFD][Defeat] ignore mod_event_pleasure reason=captive_flow_reject actor={:08X}", actorFormID);
							return false;
						}
						return true;
					},
					[&](const char* reason) { PreparePlayerForCaptivePleasureScene(reason); },
					[&](const char* reason) { CompleteCaptivePleasureHandoff(reason); },
					[&](const char* reason) { PreparePlayerForBleedoutPleasureScene(reason); },
					[&](const char* reason) { CompleteBleedPleasureHandoff(reason); }
				};

				if (name == kBleedoutOutcomePayEvent) {
					auto* flowActor = RE::TESForm::LookupByID<RE::Actor>(ResolveBleedFlowActorFormID());
					TFD::Bleedout::OutcomeEventContext context{};
					context.eventName = "mod_event_pay";
					context.rawEventName = rawName;
					context.actor = flowActor;
					context.actorFormID = ResolveBleedFlowActorFormID();
					context.inBleedState = g_inBleedState.load(std::memory_order_acquire);
					(void)TFD::Bleedout::HandleOutcomePayEvent(context, bleedOutcomeEventHandlers);
					return RE::BSEventNotifyControl::kContinue;
				}

				if (name == kBleedoutOutcomePleasureEvent) {
					const auto actorFormID = ResolveBleedFlowActorFormID();
					auto* flowActor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
					TFD::Bleedout::OutcomeEventContext context{};
					context.eventName = "mod_event_pleasure";
					context.rawEventName = rawName;
					context.actor = flowActor;
					context.actorFormID = actorFormID;
					context.inBleedState = g_inBleedState.load(std::memory_order_acquire);
					context.preserveCaptive = g_captiveState && g_captivePhase == CaptivePhaseValue::Captive;
					(void)TFD::Bleedout::HandleOutcomePleasureEvent(context, bleedOutcomeEventHandlers);
					return RE::BSEventNotifyControl::kContinue;
				}
				if (name == kBleedoutOutcomeCaptiveEvent) {
					const auto actorFormID = ResolveBleedFlowActorFormID();
					auto* flowActor = RE::TESForm::LookupByID<RE::Actor>(actorFormID);
					TFD::Bleedout::OutcomeEventContext context{};
					context.eventName = "mod_event_captive";
					context.rawEventName = rawName;
					context.actor = flowActor;
					context.actorFormID = actorFormID;
					context.inBleedState = g_inBleedState.load(std::memory_order_acquire);
					(void)TFD::Bleedout::HandleOutcomeCaptiveEvent(context, bleedOutcomeEventHandlers);
					return RE::BSEventNotifyControl::kContinue;
				}
				if (name == kBleedoutOutcomeResetEvent) {
					TFD::Bleedout::OutcomeEventContext context{};
					context.eventName = "mod_event_reset";
					context.rawEventName = rawName;
					(void)TFD::Bleedout::HandleOutcomeResetEvent(context, bleedOutcomeEventHandlers);
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

				auto* player = Player();
				if (!player) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* causeRef = ev->cause.get();
				auto* targetRef = ev->target.get();
				if (!causeRef || !targetRef) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* causeActor = causeRef->As<RE::Actor>();
				auto* targetActor = targetRef->As<RE::Actor>();
				if (!causeActor || !targetActor) {
					return RE::BSEventNotifyControl::kContinue;
				}

				if (causeActor->GetFormID() != player->GetFormID()) {
					return RE::BSEventNotifyControl::kContinue;
				}

				CancelReleaseFollowGraceFromPlayerAggression(targetActor, "player_attack_cancel");
				(void)InvalidatePassiveStateForActor(targetActor, "player_hit", false);
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* ev, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
			{
				if (!ev) {
					return RE::BSEventNotifyControl::kContinue;
				}

				const auto* rawName = ev->eventName.c_str();
				const std::string_view name = rawName ? std::string_view(rawName) : std::string_view{};
				if (name != kPassiveBreakCrimeEvent && name != kPassiveBreakPickpocketEvent) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* actor = ResolveActorFromEventArg(ev->strArg.c_str() ? std::string_view(ev->strArg.c_str()) : std::string_view{});
				if (!actor) {
					spdlog::warn("[TFD][PassiveBreak] event={} ignored reason=invalid_actor arg={}",
						std::string(name),
						ev->strArg.c_str() ? ev->strArg.c_str() : "");
					return RE::BSEventNotifyControl::kContinue;
				}

				const char* reason = name == kPassiveBreakCrimeEvent ? "player_crime" : "player_pickpocket";
				(void)InvalidatePassiveStateForActor(actor, reason, true);
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
		TFD::CaptiveRuntime::ResetForLoad();
		SetCaptiveRuntimeOnly(false, CaptivePhaseValue::None);
		g_prevDialogueOpen = false;
		ResetLockpickWatch();
		ClearEscapeContext();
		SetPlayerBleedImmune(false);
		ResetBleedRuntimeState();
		TFD::Flow::Controller::GetSingleton().ResetRuntime("defeat_install");
		TFD::PleasureRuntime::Install();
		g_lastRouterCombatContextActive = false;
		ClearAllBleedLocks("install");
		TFD::Bleedout::ClearBridgeAliases(nullptr, "install");
		TFD::FactionMask::Initialize();
		TFD::Location::Initialize();
		if (auto* scripts = RE::ScriptEventSourceHolder::GetSingleton()) {
			scripts->AddEventSink<RE::TESHitEvent>(&g_passiveBreakEventSink);
		}
		if (auto* src = SKSE::GetModCallbackEventSource()) {
			src->AddEventSink(&g_defeatedRecruitEventSink);
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
		SetCaptiveRuntime(false, CaptivePhaseValue::None);
		g_hasQueuedProgressState = false;
		g_queuedCaptiveState = false;
		g_queuedCaptivePhase = CaptivePhaseValue::None;
		g_queuedBleedOutState = false;
		SetPlayerBleedImmune(false);
		ResetBleedRuntimeState();
		TFD::Flow::Controller::GetSingleton().ResetRuntime("defeat_shutdown");
		g_lastRouterCombatContextActive = false;
		ClearAllBleedLocks("shutdown");
		TFD::Bleedout::ClearBridgeAliases(nullptr, "shutdown");
		g_loadTransition.store(false, std::memory_order_release);
		ResetLockpickWatch();
		ClearEscapeContext();
		ClearLeftForDeadCooldown();
		if (auto* scripts = RE::ScriptEventSourceHolder::GetSingleton()) {
			scripts->RemoveEventSink<RE::TESHitEvent>(&g_passiveBreakEventSink);
		}
		if (auto* src = SKSE::GetModCallbackEventSource()) {
			src->RemoveEventSink(&g_defeatedRecruitEventSink);
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
		ClearLeftForDeadCooldown();
		ClearAllReleaseFollowGrace("reset_grace");
	}

	bool HandlePassiveInvalidationAgainstActor(RE::Actor* actor, const char* reason)
	{
		return InvalidatePassiveStateForActor(actor, reason, false);
	}

	bool GetCaptiveStateForSave()
	{
		return g_captiveState;
	}

	std::uint32_t GetCaptivePhaseForSave()
	{
		return TFD::CaptiveRuntime::GetPhaseRaw(g_captiveState, g_captivePhase);
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
		g_queuedCaptiveState = stateActive;
		CaptivePhaseValue phase = stateActive ? PhaseFromRaw(phaseRaw) : CaptivePhaseValue::None;
		if (stateActive && phase == CaptivePhaseValue::None) phase = CaptivePhaseValue::Escape;
		g_queuedCaptivePhase = phase;
		spdlog::info("[TFD][Defeat] QueueLoadedProgressState state={} phase={} normalized={}", stateActive ? 1 : 0, phaseRaw, static_cast<int>(g_queuedCaptivePhase));
	}

	void QueueDefaultProgressState()
	{
		g_hasQueuedProgressState = true;
		g_queuedCaptiveState = false;
		g_queuedCaptivePhase = CaptivePhaseValue::None;
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
		ClearLeftForDeadCooldown();
		SetCaptiveRuntime(g_queuedCaptiveState, g_queuedCaptivePhase);
		g_prevDialogueOpen = IsDialogueOpen();
		g_prevLockpickOpen = IsLockpickingOpen();
		if (g_queuedCaptiveState && g_queuedCaptivePhase == CaptivePhaseValue::Captive) {
			TFD::Location::RescanCaptiveMarker();
			ArmEscapeContextFromCurrentState();
		}
		else {
			ResetLockpickWatch();
			ClearEscapeContext();
		}
		SetPlayerBleedImmune(false);
		TFD::Bleedout::ClearBridgeAliases(nullptr, "apply_queued_state");
		SetRescueStateValue(0);
		RefreshPostDefeatGlobals();
		UpdatePreCombatState();
		spdlog::info("[TFD][Defeat] ApplyQueuedProgressState state={} phase={} bleed={}", g_queuedCaptiveState ? 1 : 0, static_cast<int>(g_queuedCaptivePhase), g_queuedBleedOutState ? 1 : 0);
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
		SetCaptiveRuntimeOnly(false, CaptivePhaseValue::None);
		g_prevDialogueOpen = false;
		ResetLockpickWatch();
		ClearEscapeContext();
		TFD::FactionMask::Clear();
		TFD::AggressionClamp::Clear();
		ClearLeftForDeadCooldown();
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
			ResetLockpickWatch();
			TFD::PleasureRuntime::ResetForLoad("defeat_set_load_transition");
			spdlog::info("[TFD][Defeat] SetLoadTransition(true)");
		}
		else {
			spdlog::info("[TFD][Defeat] SetLoadTransition(false)");
		}
	}

	static bool SnapshotHasAfterPleasureContext(const TFD::Flow::Snapshot& snapshot)
	{
		switch (snapshot.sub) {
		case TFD::Flow::SubFlow::PreCombatAfterPleasure:
		case TFD::Flow::SubFlow::InCombatAfterPleasure:
		case TFD::Flow::SubFlow::BleedoutAfterPleasure:
		case TFD::Flow::SubFlow::CaptiveAfterPleasure:
		case TFD::Flow::SubFlow::VictoryAfterPleasure:
			return true;
		default:
			break;
		}
		return false;
	}

	static bool SnapshotHasPleasureScene(const TFD::Flow::Snapshot& snapshot)
	{
		switch (snapshot.sub) {
		case TFD::Flow::SubFlow::PreCombatPleasure:
		case TFD::Flow::SubFlow::InCombatPleasure:
		case TFD::Flow::SubFlow::BleedoutPleasure:
		case TFD::Flow::SubFlow::CaptivePleasure:
		case TFD::Flow::SubFlow::VictoryPleasure:
			return true;
		default:
			break;
		}
		return false;
	}

	static TFD::DefeatMonitor::DialogueContextKind ResolveDialogueContextKindInternal()
	{
		const auto snapshot = TFD::Flow::Controller::GetSingleton().GetSnapshot();

		const auto runtimePhase = TFD::PleasureRuntime::GetPhase();
		const bool runtimeAfterPleasure =
			runtimePhase == TFD::PleasureRuntime::Phase::AfterPleasureAwaitQuest ||
			runtimePhase == TFD::PleasureRuntime::Phase::AfterPleasureDialogue ||
			runtimePhase == TFD::PleasureRuntime::Phase::RedoPending ||
			runtimePhase == TFD::PleasureRuntime::Phase::Finalizing;

		if (g_captiveState || snapshot.root == TFD::Flow::RootFlow::Captive) {
			if (snapshot.captiveMode == TFD::Flow::CaptiveMode::JoinedEnemy) {
				return DialogueContextKind::JoinedEnemy;
			}
			if (SnapshotHasAfterPleasureContext(snapshot) || runtimeAfterPleasure) {
				return DialogueContextKind::AfterPleasure;
			}
			return DialogueContextKind::Captive;
		}

		if (SnapshotHasAfterPleasureContext(snapshot) || runtimeAfterPleasure) {
			return DialogueContextKind::AfterPleasure;
		}

		if (g_inBleedState.load(std::memory_order_acquire)) {
			return DialogueContextKind::Bleedout;
		}

		if (snapshot.root == TFD::Flow::RootFlow::PreCombat && snapshot.gate == TFD::Flow::DecisionGate::Truce) {
			return DialogueContextKind::PreCombat;
		}

		if (snapshot.root == TFD::Flow::RootFlow::Bleedout && snapshot.gate == TFD::Flow::DecisionGate::PlayerBleedout) {
			return DialogueContextKind::Bleedout;
		}

		if (snapshot.root == TFD::Flow::RootFlow::InCombat && snapshot.gate == TFD::Flow::DecisionGate::Truce) {
			return DialogueContextKind::InCombat;
		}

		return DialogueContextKind::None;
	}

	static TFD::DefeatMonitor::PassiveHoldKind ResolvePassiveHoldKindInternal()
	{
		const auto snapshot = TFD::Flow::Controller::GetSingleton().GetSnapshot();

		if (snapshot.root == TFD::Flow::RootFlow::Captive && snapshot.captiveMode == TFD::Flow::CaptiveMode::JoinedEnemy) {
			return PassiveHoldKind::JoinedEnemy;
		}

		if (TFD::PleasureRuntime::IsPassiveLockActive() || SnapshotHasPleasureScene(snapshot) || SnapshotHasAfterPleasureContext(snapshot)) {
			return PassiveHoldKind::Pleasure;
		}

		if (g_captiveState) {
			return PassiveHoldKind::Captive;
		}

		if (!g_releaseFollowGraceEntries.empty()) {
			return PassiveHoldKind::Grace;
		}

		if (g_inBleedState.load(std::memory_order_acquire)) {
			return PassiveHoldKind::Dialogue;
		}

		if ((snapshot.root == TFD::Flow::RootFlow::PreCombat || snapshot.root == TFD::Flow::RootFlow::InCombat || snapshot.root == TFD::Flow::RootFlow::Bleedout) &&
			(snapshot.gate == TFD::Flow::DecisionGate::Truce || snapshot.gate == TFD::Flow::DecisionGate::PlayerBleedout) && !snapshot.terminalResolved) {
			return PassiveHoldKind::Dialogue;
		}

		return PassiveHoldKind::None;
	}

	bool IsLeftForDeadRecoveryActive()
	{
		return g_leftForDeadActive;
	}

	static RE::Actor* ResolveCurrentPassivePrimaryActor()
	{
		const auto snapshot = TFD::Flow::Controller::GetSingleton().GetSnapshot();
		if (snapshot.primaryActorFormID != 0) {
			if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(snapshot.primaryActorFormID)) {
				return actor;
			}
		}

		if (auto* speaker = TFD::PleasureRuntime::GetPrimarySpeaker()) {
			return speaker;
		}

		if (g_bleedSpeakerId != 0) {
			if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(g_bleedSpeakerId)) {
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

		if (TFD::Pacify::IsPacified(actor)) {
			return true;
		}

		if (g_releaseFollowGraceEntries.find(actor->GetFormID()) != g_releaseFollowGraceEntries.end()) {
			return true;
		}

		if (ActorHasActiveDialoguePhaseFaction(actor)) {
			return true;
		}

		if (TFD::PleasureRuntime::IsActorTracked(actor)) {
			return true;
		}

		if (auto* primary = ResolveCurrentPassivePrimaryActor()) {
			if (actor == primary) {
				return true;
			}
			if (TFD::FactionMask::SharesAllowedFactionExact(actor, primary)) {
				return true;
			}
		}

		return false;
	}

	static bool InvalidatePassiveStateForActor(RE::Actor* actor, const char* reason, bool severeCrime)
	{
		auto* player = Player();
		if (!actor || !player || actor == player) {
			return false;
		}

		const bool covered = IsActorCoveredByCurrentPassiveContext(actor);
		const auto contextKind = ResolveDialogueContextKindInternal();
		const auto holdKind = ResolvePassiveHoldKindInternal();
		if (!covered && holdKind == PassiveHoldKind::None) {
			return false;
		}

		const auto actorId = actor->GetFormID();
		spdlog::info("[TFD][PassiveBreak] actor={:08X} reason={} ctx={} hold={} covered={} severeCrime={}",
			actorId,
			reason ? reason : "unknown",
			GetDialogueContextName(),
			GetPassiveHoldName(),
			covered ? 1 : 0,
			severeCrime ? 1 : 0);

		bool changed = false;
		ClearAllReleaseFollowGrace(reason ? reason : "passive_break");

		changed |= TFD::Pacify::ReleaseActiveTruceSessionForActor(actor, TFD::Pacify::ReleaseReason::PlayerAggression, false);
		if (auto* primary = ResolveCurrentPassivePrimaryActor(); primary && primary != actor) {
			changed |= TFD::Pacify::ReleaseActiveTruceSessionForActor(primary, TFD::Pacify::ReleaseReason::PlayerAggression, false);
		}

		if (g_inBleedState.load(std::memory_order_acquire) || g_bleedTruceSessionId != 0) {
			ReleaseBleedTruceSession(TFD::Pacify::ReleaseReason::PlayerAggression);
			ClearAllBleedLocks(reason ? reason : "passive_break");
			TFD::Bleedout::ClearBridgeAliases(actor, reason ? reason : "passive_break");
			changed = true;
		}

		if (TFD::PleasureRuntime::IsActive() || TFD::PleasureRuntime::IsBlocking()) {
			TFD::PleasureRuntime::Break(reason ? reason : "passive_break", true, true, true);
			changed = true;
		}

		TFD::AggressionClamp::Clear();
		TFD::FactionMask::Clear();

		auto& flow = TFD::Flow::Controller::GetSingleton();
		switch (contextKind) {
		case DialogueContextKind::JoinedEnemy:
			SetCaptiveRuntime(false, CaptivePhaseValue::None);
			flow.ResetRuntime(reason ? reason : "player_aggression_joined_enemy");
			changed = true;
			break;
		case DialogueContextKind::Captive:
			SetCaptiveRuntime(true, CaptivePhaseValue::Escape);
			(void)flow.ResolveCaptiveOutcome(TFD::Flow::CaptiveOutcome::EscapeStarted, actorId, reason ? reason : "player_aggression_captive");
			changed = true;
			break;
		case DialogueContextKind::AfterPleasure:
			flow.ResetRuntime(reason ? reason : "player_aggression_afterpleasure");
			changed = true;
			break;
		default:
			break;
		}

		if (severeCrime) {
			ClearPendingDefeatedDialogueTargetInternal();
		}

		return changed || covered;
	}

	DialogueContextKind GetDialogueContextKind()
	{
		return ResolveDialogueContextKindInternal();
	}

	const char* GetDialogueContextName()
	{
		switch (ResolveDialogueContextKindInternal()) {
		case DialogueContextKind::PreCombat:
			return "PreCombat";
		case DialogueContextKind::InCombat:
			return "InCombat";
		case DialogueContextKind::Bleedout:
			return "Bleedout";
		case DialogueContextKind::Captive:
			return "Captive";
		case DialogueContextKind::AfterPleasure:
			return "AfterPleasure";
		case DialogueContextKind::JoinedEnemy:
			return "JoinedEnemy";
		default:
			return "None";
		}
	}

	bool IsDialogueContextActive()
	{
		return ResolveDialogueContextKindInternal() != DialogueContextKind::None;
	}

	PassiveHoldKind GetPassiveHoldKind()
	{
		return ResolvePassiveHoldKindInternal();
	}

	const char* GetPassiveHoldName()
	{
		switch (ResolvePassiveHoldKindInternal()) {
		case PassiveHoldKind::Dialogue:
			return "Dialogue";
		case PassiveHoldKind::Grace:
			return "Grace";
		case PassiveHoldKind::Pleasure:
			return "Pleasure";
		case PassiveHoldKind::Captive:
			return "Captive";
		case PassiveHoldKind::JoinedEnemy:
			return "JoinedEnemy";
		default:
			return "None";
		}
	}

	bool IsPassiveHoldActive()
	{
		return ResolvePassiveHoldKindInternal() != PassiveHoldKind::None;
	}

	bool IsPassiveHoldProtectedHandoff()
	{
		const auto holdKind = ResolvePassiveHoldKindInternal();
		return holdKind == PassiveHoldKind::Pleasure ||
			holdKind == PassiveHoldKind::Captive ||
			holdKind == PassiveHoldKind::JoinedEnemy;
	}

	bool IsPleasureLockActive()
	{
		return TFD::PleasureRuntime::IsPassiveLockActive();
	}

	bool IsPreCombatBlocked()
	{
		if (g_leftForDeadActive) {
			return true;
		}

		const auto contextKind = ResolveDialogueContextKindInternal();
		switch (contextKind) {
		case DialogueContextKind::Captive:
		case DialogueContextKind::AfterPleasure:
		case DialogueContextKind::JoinedEnemy:
		case DialogueContextKind::Bleedout:
			return true;
		default:
			break;
		}

		const auto holdKind = ResolvePassiveHoldKindInternal();
		return holdKind == PassiveHoldKind::Pleasure ||
			holdKind == PassiveHoldKind::Captive ||
			holdKind == PassiveHoldKind::JoinedEnemy;
	}

	bool IsCaptivePhase()
	{
		return g_captiveState && g_captivePhase == CaptivePhaseValue::Captive;
	}

	std::uint32_t GetCaptivePhaseRaw()
	{
		return TFD::CaptiveRuntime::GetPhaseRaw(g_captiveState, g_captivePhase);
	}

	const char* GetCaptivePhaseName()
	{
		return TFD::CaptiveRuntime::GetPhaseName(g_captiveState, g_captivePhase);
	}

	bool IsCaptiveFamily()
	{
		return TFD::CaptiveRuntime::IsFamily(g_captiveState, g_captivePhase);
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
		if (IsActiveFollowerActor(actor) || TFD::Pacify::IsCompanion(actor)) {
			return IsActorDownByThreshold(actor, TFD::Settings::GetAllyDownedThresholdPct());
		}
		return IsActorDownByThreshold(actor, TFD::Settings::GetEnemyDownedThresholdPct());
	}

	bool ReviveDownedAlly(RE::Actor* actor, float targetHealthPct)
	{
		if (!actor || actor == Player() || actor->IsDisabled() || actor->IsDead()) {
			return false;
		}

		const bool managedAlly = IsActiveFollowerActor(actor) || TFD::Pacify::IsCompanion(actor) || TFD::Pacify::HasActiveTameSession(actor);
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
		if (TFD::Pacify::IsPacified(actor)) {
			return false;
		}
		if (HasReleaseFollowGrace(actor)) {
			return false;
		}
		return true;
	}

	bool HasReleaseFollowGraceForActor(RE::Actor* actor)
	{
		return HasReleaseFollowGrace(actor);
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

	static bool RecruitDefeatedHumanoidAsTeammateBridgeImpl(RE::Actor* actor)
	{
		if (!actor || !IsDialogueCapableDefeatedEnemyInternal(actor)) {
			return false;
		}
		if (GetDefeatedEnemyRemainingSecondsInternal(actor) <= 0.0) {
			return false;
		}
		if (!SendBridgeModEvent(kHumanoidTeammateAssignEvent, actor)) {
			spdlog::warn("[TFD][Defeat] defeated humanoid recruit failed actor={:08X} reason=bridge_assign_failed", actor->GetFormID());
			return false;
		}
		SuppressDefeatedReentry(actor, kDefeatedReentrySuppressSeconds, "defeated_humanoid_recruit");
		ReleaseBleedLock(actor, "defeated_humanoid_recruit", true);
		RestoreActorHealthToSafePct(actor, TFD::Settings::GetEnemyDownedThresholdPct(), 0.12f, 0.58f, 0.92f, 45.0f, "defeated_humanoid_recruit");
		if (actor->IsInCombat()) {
			actor->StopCombat();
		}
		actor->DrawWeaponMagicHands(false);
		ClearPendingDefeatedDialogueTargetInternal();
		spdlog::info("[TFD][Defeat] defeated humanoid recruit actor={:08X}", actor->GetFormID());
		return true;
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
		return RecruitDefeatedHumanoidAsTeammateBridgeImpl(actor);
	}

	bool RecruitDefeatedCreatureAsTeammate(RE::Actor* actor, double nowSec)
	{
		auto* player = Player();
		if (!player || !actor || !IsCreatureDefeatedEnemyInternal(actor)) {
			return false;
		}
		if (GetDefeatedEnemyRemainingSecondsInternal(actor) <= 0.0) {
			return false;
		}
		// Defeated creature recruit is a direct single-target conversion, not a normal local-splash tame.
		// Bypass pack bait validation here so Shift+H on a knocked creature does not fail on tame-bait checks.
		auto session = TFD::Pacify::BeginTameSession(player, actor, nowSec, false, false);
		if (!session.has_value()) {
			spdlog::warn("[TFD][Defeat] defeated creature recruit failed actor={:08X} reason=begin_tame_failed", actor->GetFormID());
			return false;
		}
		if (!TFD::Pacify::PromoteActiveTameToCompanion(actor, 24.0)) {
			TFD::Pacify::ReleaseActiveTameActor(actor, TFD::Pacify::ReleaseReason::Generic);
			spdlog::warn("[TFD][Defeat] defeated creature recruit failed actor={:08X} reason=promote_failed", actor->GetFormID());
			return false;
		}
		SuppressDefeatedReentry(actor, kDefeatedReentrySuppressSeconds, "defeated_creature_recruit");
		ReleaseBleedLock(actor, "defeated_creature_recruit", true);
		RestoreActorHealthToSafePct(actor, TFD::Settings::GetEnemyDownedThresholdPct(), 0.12f, 0.58f, 0.92f, 45.0f, "defeated_creature_recruit");
		if (actor->IsInCombat()) {
			actor->StopCombat();
		}
		actor->DrawWeaponMagicHands(false);
		spdlog::info("[TFD][Defeat] defeated creature recruit actor={:08X}", actor->GetFormID());
		return true;
	}

}
