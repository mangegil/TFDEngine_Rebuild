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
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <tuple>
#include <array>

#include <RE/Skyrim.h>
#include <type_traits>
#include <RE/A/ActorValues.h>
#include <RE/L/LockpickingMenu.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include "TFDSettings.h"
#include "TFDLocation.h"
#include "TFDCaptiveDoorController.h"
#include "TFDAntiAggro.h"
#include "TFDFactionMask.h"
#include "TFDActorScan.h"
#include "TFDAggressionClamp.h"
#include "TFDPreCombatGreet.h"
#include "TFDPacify.h"
#include "EditorIdCache.h"
#include "RE/B/BGSRefAlias.h"
#include "RE/T/TESQuest.h"

namespace TFD::DefeatMonitor
{
	namespace
	{
		static void ClearBleedoutBridgeAliases(RE::TESForm* sender, const char* reason);
		static void AssignBleedoutBridgeActor(RE::Actor* actor);
		static float Distance3D(const RE::NiPoint3& a, const RE::NiPoint3& b);
		enum class CaptivePhaseValue : int
		{
			None = 0,
			Captive = 1,
			Escape = 2,
			ReleasedWork = 3,
			Scene = 4
		};

		std::atomic_bool g_installed{ false };
		std::atomic_bool g_running{ false };
		std::atomic_bool g_loadTransition{ false };
		std::atomic_flag g_tickPending = ATOMIC_FLAG_INIT;
		std::thread g_worker{};

		static RE::TESGlobal* g_captiveStateGlobal = nullptr;
		static RE::TESGlobal* g_captivePhaseGlobal = nullptr;
		static RE::TESGlobal* g_preCombatStateGlobal = nullptr;
		static RE::TESGlobal* g_transitionPendingGlobal = nullptr;
		static RE::TESGlobal* g_transitionBusyGlobal = nullptr;
		static RE::TESGlobal* g_transitionReasonGlobal = nullptr;
		static RE::TESGlobal* g_transitionResultGlobal = nullptr;
		static bool g_loggedCaptiveStateGlobal = false;
		static bool g_loggedCaptivePhaseGlobal = false;
		static bool g_loggedPreCombatStateGlobal = false;
		static bool g_loggedTransitionPendingGlobal = false;
		static bool g_loggedTransitionBusyGlobal = false;
		static bool g_loggedTransitionReasonGlobal = false;
		static bool g_loggedTransitionResultGlobal = false;

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

		std::atomic_bool g_grace{ false };
		std::chrono::steady_clock::time_point g_graceUntil{};

		bool g_leftForDeadActive = false;
		std::chrono::steady_clock::time_point g_leftForDeadUntil{};
		std::chrono::steady_clock::time_point g_leftForDeadNextPulse{};
		bool g_leftForDeadNeedsAggroKick = false;


		RE::ActorHandle g_lastAggressor{};
		RE::ActorHandle g_lastEnemyTargetingPlayer{};
		RE::FormID g_lastEnemyTargetingPlayerFormID = 0;
		std::chrono::steady_clock::time_point g_lastEnemyTargetingPlayerSeen{};
		bool g_bleedSawDialogue = false;
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

		bool g_captiveState = false;
		CaptivePhaseValue g_captivePhase = CaptivePhaseValue::None;
		bool g_prevDialogueOpen = false;
		bool g_prevLockpickOpen = false;
		bool g_captiveConfiscationApplied = false;

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

		struct CaptiveQuestRegistryCache
		{
			RE::TESQuest* quest{ nullptr };
			RE::BGSRefAlias* playerCaptiveAlias{ nullptr };
			std::array<RE::BGSRefAlias*, 3> bossCaptorAliases{};
			std::array<RE::BGSRefAlias*, 3> bossContainerAliases{};
			std::array<RE::BGSRefAlias*, 3> containerAliases{};
			RE::BGSRefAlias* lootTargetAlias{ nullptr };
			bool resolved{ false };
		};

		CaptiveQuestRegistryCache g_captiveQuestRegistry{};
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
		TFD::CaptiveDoorController g_captiveDoor{};
		RE::ObjectRefHandle g_captiveMarker{};
		RE::FormID g_captiveCellFormID = 0;
		RE::FormID g_captiveLocationFormID = 0;
		bool g_escapeRadiusActive = false;
		std::chrono::steady_clock::time_point g_escapeRadiusSince{};
		RE::ObjectRefHandle g_boundEscapeDoor{};

		static constexpr double kCaptiveEscapeDoorRadius = 512.0;
		static constexpr std::size_t kBleedBridgeMaxActors = 10;

		bool g_hasQueuedProgressState = false;
		bool g_queuedCaptiveState = false;
		CaptivePhaseValue g_queuedCaptivePhase = CaptivePhaseValue::None;

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
		static void SnapshotBleedFollowerDownState(float radius);
		static std::vector<RE::Actor*> CollectBleedStandingFollowers(float radius);
		static std::vector<RE::Actor*> CollectBleedStandingEnemies(float radius);
		static bool IsStandingObserverActor(RE::Actor* actor);
		static bool IsObserverAlly(RE::Actor* actor);
		static bool IsObserverEnemy(RE::Actor* actor, RE::Actor* player, const std::vector<RE::Actor*>& allies, bool hostileHint, bool inCombatHint);
		static bool IsValidBleedBattleEnemyRosterActor(RE::Actor* actor, RE::Actor* player);
		static bool HasStandingHumanoidFollowers(const std::vector<RE::Actor*>& followers);
		static bool BuildBleedBattleObserveSnapshot(RE::Actor* player, float radius, RE::Actor* preferredEnemy);
		static std::vector<RE::Actor*> CollectBleedStandingFollowersFromSnapshot();
		static std::vector<RE::Actor*> CollectBleedStandingEnemiesFromSnapshot();
		static void RedirectBleedObserverAggro(RE::Actor* player, const std::vector<RE::Actor*>& followers, const std::vector<RE::Actor*>& enemies)
		{
			(void)player;
			(void)followers;
			(void)enemies;
			return;
		}

		static void MaintainBleedObserverFollowerAggro(RE::Actor* player, const std::vector<RE::Actor*>& followers, const std::vector<RE::Actor*>& enemies)
		{
			(void)player;
			(void)followers;
			(void)enemies;
			return;
		}

		static bool StartBleedBattleObservePending(RE::Actor* player);
		static void TickBleedBattleObservePending();
		static bool StartBleedBattleObserve(RE::Actor* player, RE::Actor* preferredEnemy);
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
		static void ClearPendingCinematicFadeIn();
		static void ClearEscapeContext();
		static void QueueNonCaptiveChoiceRequest(const char* reason);
		static void ResolveLeftForDeadDestination(NoMarkerFallbackState& state);
		static void ClampHealth(RE::Actor* actor, float minHp);

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

		static void ClearBleedoutBridgeAliases(RE::TESForm* sender, const char* reason)
		{
			const bool queued = SendBridgeModEvent("TFDBleedoutClearAll", sender);
			spdlog::info("[TFD][BleedBridge] ClearAll reason={} queued={}", reason ? reason : "unknown", queued);
		}

		static void AssignBleedoutBridgeActor(RE::Actor* actor)
		{
			if (!actor) {
				return;
			}

			const bool queued = SendBridgeModEvent("TFDBleedoutAssign", actor);
			spdlog::info("[TFD][BleedBridge] Assign actor={:08X} queued={}", actor->GetFormID(), queued);
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
		static void AssignBleedSupportBridgeActors(const std::vector<RE::Actor*>& actors, RE::Actor* speaker, const char* reason);
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
			const bool truceQueued = SendBridgeModEvent("TFDTruceClearAll", nullptr);
			const bool inCombatQueued = SendBridgeModEvent("TFDInCombatClearAll", nullptr);
			spdlog::info("[TFD][BleedBridge] ClearSupport reason={} truceQueued={} inCombatQueued={}",
				reason ? reason : "unknown",
				truceQueued ? 1 : 0,
				inCombatQueued ? 1 : 0);
		}

		static void AssignBleedSupportBridgeActors(const std::vector<RE::Actor*>& actors, RE::Actor* speaker, const char* reason)
		{
			ClearBleedSupportBridgeAliases(reason);

			std::vector<RE::FormID> supportIds;
			supportIds.reserve(actors.size());
			const RE::FormID speakerId = speaker ? speaker->GetFormID() : 0;

			for (auto* actor : actors) {
				if (!actor) {
					continue;
				}
				const auto actorId = actor->GetFormID();
				if (actorId == 0 || actorId == speakerId) {
					continue;
				}
				if (std::find(supportIds.begin(), supportIds.end(), actorId) != supportIds.end()) {
					continue;
				}
				SendBridgeModEvent("TFDTruceAssign", actor);
				SendBridgeModEvent("TFDInCombatAssign", actor);
				supportIds.push_back(actorId);
			}

			g_bleedCrowdAssigned = std::move(supportIds);
			g_bleedLastCrowdAssign = Now();

			spdlog::info("[TFD][BleedBridge] AssignSupport reason={} supportSize={} speaker={:08X}",
				reason ? reason : "unknown",
				static_cast<unsigned int>(g_bleedCrowdAssigned.size()),
				speakerId);
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

		static std::vector<RE::Actor*> CollectBleedoutCrowd(float radius, RE::Actor* preferred, bool preserveAssigned = false)
		{
			std::vector<std::pair<float, RE::Actor*>> scored;

			auto* player = Player();
			if (!player) {
				return {};
			}


			std::unordered_set<RE::FormID> preservedIds;
			if (preserveAssigned) {
				preservedIds.insert(g_bleedCrowdAssigned.begin(), g_bleedCrowdAssigned.end());
			}

			const float scanRadius = (std::max)(radius, 2000.0f);
			TFD::ActorScan::Rescan(scanRadius, false);
			const auto n = TFD::ActorScan::GetCount();
			for (int i = 0; i < n; ++i) {
				auto e = TFD::ActorScan::GetEntry(i);
				auto sp = e.actor.get();
				auto* actor = sp.get();
				if (!actor || actor->IsDead() || actor->IsDisabled()) continue;
				if (!actor->Is3DLoaded()) continue;
				if (actor->GetFormID() == player->GetFormID()) continue;
				if (!IsBleedSpaceCompatible(actor, player)) continue;
				if (!IsBleedCrowdSupportedAggressor(actor)) continue;
				if (e.dist > scanRadius) continue;

				const bool targetingPlayer = e.hostile || e.inCombat || actor->IsInCombat() || actor->IsHostileToActor(player);
				const bool preserved = preserveAssigned && preservedIds.find(actor->GetFormID()) != preservedIds.end();
				if (!targetingPlayer && !preserved && actor != preferred) continue;

				float score = e.dist;
				if (actor == preferred) score -= 1000.0f;
				if (targetingPlayer) score -= 140.0f;
				if (e.hostile) score -= 80.0f;
				if (e.inCombat || actor->IsInCombat()) score -= 60.0f;
				if (preserved) score -= 90.0f;
				if (IsActorCloseAndFront(actor, player, 320.0f)) score -= 120.0f;
				scored.emplace_back(score, actor);
			}

			std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
				if (a.first != b.first) {
					return a.first < b.first;
				}
				if (!a.second || !b.second) {
					return a.second != nullptr;
				}
				return a.second->GetFormID() < b.second->GetFormID();
				});

			std::vector<RE::Actor*> result;
			result.reserve((std::min)(scored.size(), kBleedBridgeMaxActors));
			for (const auto& [score, actor] : scored) {
				(void)score;
				if (!actor) {
					continue;
				}
				const auto id = actor->GetFormID();
				bool seen = false;
				for (auto* existing : result) {
					if (existing && existing->GetFormID() == id) {
						seen = true;
						break;
					}
				}
				if (seen) {
					continue;
				}
				result.push_back(actor);
				if (result.size() >= kBleedBridgeMaxActors) {
					break;
				}
			}

			if (preferred) {
				const auto preferredId = preferred->GetFormID();
				auto it = std::find_if(result.begin(), result.end(), [preferredId](RE::Actor* actor) {
					return actor && actor->GetFormID() == preferredId;
					});
				if (it == result.end()) {
					if (result.size() >= kBleedBridgeMaxActors) {
						result.pop_back();
					}
					result.insert(result.begin(), preferred);
				}
				else if (it != result.begin()) {
					std::rotate(result.begin(), it, it + 1);
				}
			}

			return result;
		}

		static void AssignBleedoutBridgeCrowd(const std::vector<RE::Actor*>& actors)
		{
			std::size_t sent = 0;
			for (auto* actor : actors) {
				if (!actor) {
					continue;
				}
				const bool queued = SendBridgeModEvent("TFDBleedoutAssign", actor);
				if (queued) {
					++sent;
				}
				spdlog::info("[TFD][BleedBridge] Assign actor={:08X} queued={}", actor->GetFormID(), queued);
			}

			spdlog::info("[TFD][BleedBridge] Assign crowd sent={} size={} primary={:08X}",
				sent,
				actors.size(),
				!actors.empty() && actors.front() ? actors.front()->GetFormID() : 0u);
		}

		static void RefreshBleedoutBridgeCrowd(float radius, RE::Actor* preferred, bool forceClear, std::vector<RE::Actor*>* explicitCrowd = nullptr)
		{
			std::vector<RE::Actor*> crowd = explicitCrowd ? *explicitCrowd : CollectBleedoutCrowd(radius, preferred, !forceClear);
			std::vector<RE::FormID> next;
			next.reserve(crowd.size());
			for (auto* actor : crowd) {
				if (actor) {
					next.push_back(actor->GetFormID());
				}
			}

			const bool changed = forceClear || next != g_bleedCrowdAssigned;
			if (changed) {
				ClearBleedoutBridgeAliases(preferred, forceClear ? "bleed_crowd_force_refresh" : "bleed_crowd_refresh");
				if (!crowd.empty()) {
					AssignBleedoutBridgeCrowd(crowd);
				}
				g_bleedCrowdAssigned = std::move(next);
			}
			g_bleedLastCrowdAssign = Now();
		}

		static void ReleasePlayerBleedLock(const char* reason, bool playGetUp);

		static void ResetBleedRuntimeState()
		{
			ReleasePlayerBleedLock("reset_bleed_runtime", false);
			ReleaseBleedTruceSession(TFD::Pacify::ReleaseReason::Generic);
			ReleaseBleedNoSpeakerTameSession("reset_bleed_runtime");
			ClearBleedSupportBridgeAliases("reset_bleed_runtime");
			g_inBleedState.store(false, std::memory_order_release);
			g_minHp = 0.0f;
			g_bleedSawDialogue = false;
			g_bleedPendingCaptiveOutcome = false;
			g_bleedPendingNonCaptiveOutcome = false;
			g_bleedPaused = false;
			g_bleedPauseStarted = {};
			g_bleedLastCalmPulse = {};
			g_bleedLastCrowdAssign = {};
			g_bleedCrowdAssigned.clear();
			g_bleedStart = Now();
			g_bleedLastSeconds = -1;
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
			ClearLastEnemyTargetingPlayerInternal();
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

		static void BlackoutAndAdvanceHours(float hours, int holdMs, const char* reason)
		{
			ShowBlackoutFader();
			std::this_thread::sleep_for(std::chrono::milliseconds(350));
			AdvanceGameHoursSoft(hours);
			if (holdMs > 0) {
				std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
			}
			HideBlackoutFader();
			spdlog::info("[TFD][Defeat] blackout advance reason={} hours={:.2f}", reason ? reason : "unknown", hours);
		}

		static CaptivePhaseValue PhaseFromRaw(std::uint32_t raw)
		{
			switch (raw) {
			case 1: return CaptivePhaseValue::Captive;
			case 2: return CaptivePhaseValue::Escape;
			case 3: return CaptivePhaseValue::ReleasedWork;
			case 4: return CaptivePhaseValue::Scene;
			default: return CaptivePhaseValue::None;
			}
		}


		static RE::Actor* ResolveAggressor();
		static RE::Actor* FindBestAggressor(float radius);
		static RE::Actor* FindBestBleedoutSpeaker(float radius, float maxDist, RE::Actor* preferred = nullptr);
		static bool IsReasonableBleedoutSpeaker(RE::Actor* actor, RE::Actor* player, float maxDist, float* outDistance = nullptr);
		static RE::Actor* ResolveRecentPreCombatAggressor(float radius);
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
		static bool ProcessCaptiveConfiscation(const char* reason);
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
						g_teammateRegistry.teammateAliases[slot - 1] = refAlias;
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
						g_defeatedEnemyRegistry.enemyAliases[slot - 1] = refAlias;
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
							g_captiveQuestRegistry.bossCaptorAliases[slot - 1] = refAlias;
						}
					}
					catch (...) {}
					continue;
				}
				if (aliasName.rfind("BossContainer", 0) == 0 && aliasName.size() >= 14) {
					try {
						int slot = std::stoi(aliasName.substr(13));
						if (slot >= 1 && slot <= static_cast<int>(g_captiveQuestRegistry.bossContainerAliases.size())) {
							g_captiveQuestRegistry.bossContainerAliases[slot - 1] = refAlias;
						}
					}
					catch (...) {}
					continue;
				}
				if (aliasName.rfind("Container", 0) == 0 && aliasName.size() >= 10) {
					try {
						int slot = std::stoi(aliasName.substr(9));
						if (slot >= 1 && slot <= static_cast<int>(g_captiveQuestRegistry.containerAliases.size())) {
							g_captiveQuestRegistry.containerAliases[slot - 1] = refAlias;
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

		static void SnapshotBleedFollowerDownState(float radius)
		{
			(void)radius;
			auto* player = Player();
			if (!player) {
				return;
			}
			for (auto* actor : CollectKnownTeammates(radius)) {
				if (!actor || actor == player) {
					continue;
				}
				if (IsActorDownByThreshold(actor, TFD::Settings::GetAllyDownedThresholdPct())) {
					g_bleedFollowerDownIds.insert(actor->GetFormID());
				}
			}
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

		static std::vector<RE::Actor*> CollectBleedStandingEnemies(float radius)
		{
			std::vector<RE::Actor*> out;
			auto* player = Player();
			if (!player) {
				return out;
			}
			auto* pCell = player->GetParentCell();
			if (!pCell) {
				return out;
			}
			TFD::ActorScan::Rescan(radius, false);
			const auto count = TFD::ActorScan::GetCount();
			for (int i = 0; i < count; ++i) {
				auto e = TFD::ActorScan::GetEntry(i);
				auto sp = e.actor.get();
				auto* actor = sp.get();
				if (!IsStandingEnemyThresholdActor(actor)) {
					continue;
				}
				if (!actor->Is3DLoaded()) {
					continue;
				}
				if (actor->GetParentCell() != pCell) {
					continue;
				}
				if (IsActiveFollowerActor(actor)) {
					continue;
				}
				if (!e.hostile && !e.inCombat && !actor->IsInCombat()) {
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

		static bool HasStandingHumanoidFollowers(const std::vector<RE::Actor*>& followers)
		{
			for (auto* actor : followers) {
				if (!IsStandingAllyThresholdActor(actor)) {
					continue;
				}
				if (ActorHasKeywordByEditorID(actor, "ActorTypeNPC")) {
					return true;
				}
			}
			return false;
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

		static bool BuildBleedBattleObserveSnapshot(RE::Actor* player, float radius, RE::Actor* preferredEnemy)
		{
			if (!player) {
				return false;
			}

			BleedBattleObserverState next{};

			auto allies = CollectBleedStandingFollowers(radius);
			for (auto* ally : allies) {
				if (ally && IsStandingAllyThresholdActor(ally)) {
					next.allyIds.insert(ally->GetFormID());
				}
			}
			if (next.allyIds.empty()) {
				return false;
			}

			for (auto id : g_bleedBattleObserver.enemyIds) {
				auto* enemy = RE::TESForm::LookupByID<RE::Actor>(id);
				if (IsValidBleedBattleEnemyRosterActor(enemy, player)) {
					next.enemyIds.insert(id);
				}
			}

			auto enemies = CollectCurrentObservedEnemies(player, radius, preferredEnemy, allies);
			for (auto* enemy : enemies) {
				if (enemy && IsValidBleedBattleEnemyRosterActor(enemy, player)) {
					next.enemyIds.insert(enemy->GetFormID());
				}
			}

			if (preferredEnemy && IsValidBleedBattleEnemyRosterActor(preferredEnemy, player)) {
				next.enemyIds.insert(preferredEnemy->GetFormID());
			}

			next.hadValidObservedEnemy = !next.enemyIds.empty() || g_bleedBattleObserver.hadValidObservedEnemy;
			g_bleedBattleObserver = std::move(next);
			return g_bleedBattleObserver.hadValidObservedEnemy && !CollectBleedStandingEnemiesFromSnapshot().empty();
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

		static void ForceObservedCombatCommit(RE::Actor* actor, RE::Actor* target, bool drawWeapon)
		{
			if (!actor || !target) {
				return;
			}

			ObservedCombatCommitScope commitScope{};
			auto* player = Player();
			actor->GetActorRuntimeData().currentCombatTarget = target->GetHandle();
			if (!actor->IsAIEnabled()) {
				actor->EnableAI(true);
			}
			if (drawWeapon && !actor->IsWeaponDrawn()) {
				actor->DrawWeaponMagicHands(true);
			}
			actor->SetBeenAttacked(true);
			target->SetBeenAttacked(true);
			actor->RequestDetectionLevel(target, RE::DETECTION_PRIORITY::kCritical);
			target->RequestDetectionLevel(actor, RE::DETECTION_PRIORITY::kCritical);
			if (player) {
				player->SetBeenAttacked(true);
				actor->RequestDetectionLevel(player, RE::DETECTION_PRIORITY::kCritical);
				player->RequestDetectionLevel(actor, RE::DETECTION_PRIORITY::kCritical);
				player->RequestDetectionLevel(target, RE::DETECTION_PRIORITY::kCritical);
				target->RequestDetectionLevel(player, RE::DETECTION_PRIORITY::kCritical);
			}
			if (auto* process = RE::ProcessLists::GetSingleton()) {
				process->ClearCachedFactionFightReactions();
			}
			actor->EvaluatePackage(false, true);
			actor->EvaluatePackage(true, true);
			target->EvaluatePackage(false, true);
			target->EvaluatePackage(true, true);
			actor->UpdateCombat();
			target->UpdateCombat();
		}

		static bool StartBleedBattleObservePending(RE::Actor* player)
		{
			if (!player) {
				return false;
			}

			spdlog::info("[TFD][Defeat] bleed observe pending sentinel step=begin");
			ClearBleedoutBridgeAliases(nullptr, "start_bleed_observe_pending");
			spdlog::info("[TFD][Defeat] bleed observe pending sentinel step=after_clear_bridge");
			ClearNoMarkerFallbackState();
			spdlog::info("[TFD][Defeat] bleed observe pending sentinel step=after_clear_nomarker");
			ReleaseBleedTruceSession(TFD::Pacify::ReleaseReason::Generic);
			spdlog::info("[TFD][Defeat] bleed observe pending sentinel step=after_release_truce");
			ReleaseBleedNoSpeakerTameSession("start_bleed_observe_pending");
			spdlog::info("[TFD][Defeat] bleed observe pending sentinel step=after_release_tame");
			ClearBleedSupportBridgeAliases("start_bleed_observe_pending");
			spdlog::info("[TFD][Defeat] bleed observe pending sentinel step=after_clear_support");

			g_inBleedState.store(true, std::memory_order_release);
			g_bleedSawDialogue = false;
			g_bleedPendingCaptiveOutcome = false;
			g_bleedPendingNonCaptiveOutcome = false;
			g_bleedStart = Now();
			g_bleedLastSeconds = -1;
			g_bleedPaused = false;
			g_bleedPauseStarted = {};
			g_bleedLastCalmPulse = {};
			g_bleedLastCrowdAssign = {};
			g_bleedCrowdAssigned.clear();
			g_bleedBattleObservePending = true;
			g_bleedBattleObservePendingUntil = Now() + std::chrono::milliseconds(1800);
			g_bleedBattleObservePendingLastRedirect = {};
			g_bleedBattleObservePendingEmptyEnemyTicks = 0;
			g_bleedBattleObserveActive = false;
			g_bleedBattleObserveSince = {};
			g_bleedBattleObserveLastRedirect = {};
			g_bleedBattleObserveActiveEmptyEnemyTicks = 0;
			g_bleedBattlePreferredEnemy = {};
			g_bleedBattleObserver = {};

			const float maxHp = player->GetPermanentActorValue(RE::ActorValue::kHealth);
			g_minHp = (std::max)(1.0f, maxHp * 0.02f);
			SetPlayerBleedImmune(true);
			ClampHealth(player, g_minHp);
			player->NotifyAnimationGraph("BleedoutStart");

			const float immediateRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 600.0f);
			auto immediateFollowers = CollectBleedStandingFollowers(immediateRadius);
			const float immediateEnemyScanRadius = ComputeBleedBattleEnemyScanRadius(player, immediateFollowers, immediateRadius);
			auto* immediatePreferredEnemy = ResolveAggressor();
			if (!immediatePreferredEnemy) {
				immediatePreferredEnemy = ResolveLastEnemyTargetingPlayerInternal(immediateEnemyScanRadius, 15.0);
			}
			if (!immediatePreferredEnemy) {
				immediatePreferredEnemy = FindBestAggressor(immediateEnemyScanRadius);
			}
			if (immediatePreferredEnemy && IsObserverAlly(immediatePreferredEnemy)) {
				immediatePreferredEnemy = nullptr;
			}
			auto immediateEnemies = CollectCurrentObservedEnemies(player, immediateRadius, immediatePreferredEnemy, immediateFollowers);
			UpdateBleedBattleObserverRoster(player, immediateFollowers, immediateEnemies, immediatePreferredEnemy);
			auto immediateRosterEnemies = CollectBleedStandingEnemiesFromSnapshot();
			auto immediateResolvedEnemies = immediateRosterEnemies.empty() ? immediateEnemies : immediateRosterEnemies;
			spdlog::info("[TFD][Defeat] bleed observe pending sentinel step=after_snapshot followers={} enemies={} rosterEnemies={} preferred={:08X}",
				immediateFollowers.size(),
				immediateEnemies.size(),
				immediateResolvedEnemies.size(),
				immediatePreferredEnemy ? immediatePreferredEnemy->GetFormID() : 0u);
			if (!immediateFollowers.empty() && !immediateResolvedEnemies.empty()) {
				spdlog::info("[TFD][Defeat] bleed observe pending sentinel step=vanilla_ai_passthrough");
				g_bleedBattleObservePendingLastRedirect = Now();
			}

			const int bleedSeconds = TFD::Settings::GetBleedWindowSeconds();
			char msg[96]{};
			std::snprintf(msg, sizeof(msg), "TFDEngine: Bleeding... allies fighting (%ds)", bleedSeconds);
			RE::DebugNotification(msg);
			spdlog::info("[TFD][Defeat] bleed battle observe pending started");
			return true;
		}

		static void TickBleedBattleObservePending()
		{
			auto* player = Player();
			if (!player) {
				g_bleedBattleObservePending = false;
				return;
			}

			if (g_minHp > 0.0f) {
				SetPlayerBleedImmune(true);
				ClampHealth(player, g_minHp);
			}

			const float radius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 600.0f);
			auto followers = CollectBleedStandingFollowers(radius);
			if (followers.empty()) {
				g_bleedBattleObservePending = false;
				EnterObservedLeftForDead("battle_observe_pending_no_followers");
				return;
			}

			const float enemyScanRadius = ComputeBleedBattleEnemyScanRadius(player, followers, radius);
			auto* preferredEnemy = ResolveAggressor();
			if (!preferredEnemy) {
				preferredEnemy = ResolveLastEnemyTargetingPlayerInternal(enemyScanRadius, 15.0);
			}
			if (!preferredEnemy) {
				preferredEnemy = FindBestAggressor(enemyScanRadius);
			}
			if (preferredEnemy && IsObserverAlly(preferredEnemy)) {
				preferredEnemy = nullptr;
			}
			auto enemies = CollectCurrentObservedEnemies(player, radius, preferredEnemy, followers);
			UpdateBleedBattleObserverRoster(player, followers, enemies, preferredEnemy);
			auto rosterEnemies = CollectBleedStandingEnemiesFromSnapshot();
			spdlog::info("[TFD][Defeat] bleed observe pending scan followers={} enemies={} rosterEnemies={} allyThresh={:.1f} enemyThresh={:.1f} preferred={:08X}",
				followers.size(),
				enemies.size(),
				rosterEnemies.size(),
				TFD::Settings::GetAllyDownedThresholdPct(),
				TFD::Settings::GetEnemyDownedThresholdPct(),
				preferredEnemy ? preferredEnemy->GetFormID() : 0u);

			const auto now = Now();

			if (!rosterEnemies.empty()) {
				g_bleedBattleObservePendingEmptyEnemyTicks = 0;
				g_bleedBattleObservePendingUntil = now + std::chrono::milliseconds(750);
				return;
			}

			if (now < g_bleedBattleObservePendingUntil) {
				return;
			}

			if (rosterEnemies.empty()) {
				++g_bleedBattleObservePendingEmptyEnemyTicks;
				if (g_bleedBattleObservePendingEmptyEnemyTicks < 8) {
					g_bleedBattleObservePendingUntil = now + std::chrono::milliseconds(500);
					return;
				}

				g_bleedBattleObservePending = false;
				if (g_bleedBattleObserver.hadValidObservedEnemy && !followers.empty()) {
					EnterObservedBattleWin();
				}
				else {
					EnterObservedLeftForDead("battle_observe_no_survivor");
				}
				return;
			}

			g_bleedBattleObservePendingEmptyEnemyTicks = 0;
			g_bleedBattleObservePendingUntil = now + std::chrono::milliseconds(750);
		}

		static bool StartBleedBattleObserve(RE::Actor* player, RE::Actor* preferredEnemy)
		{
			if (!player) {
				return false;
			}

			const float radius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 600.0f);
			if (!BuildBleedBattleObserveSnapshot(player, radius, preferredEnemy)) {
				spdlog::info("[TFD][Defeat] bleed battle observe rejected: no valid enemy snapshot preferred={:08X}", preferredEnemy ? preferredEnemy->GetFormID() : 0u);
				return false;
			}

			ClearBleedoutBridgeAliases(preferredEnemy, "start_bleed_observe");
			ClearNoMarkerFallbackState();
			ReleaseBleedTruceSession(TFD::Pacify::ReleaseReason::Generic);
			ReleaseBleedNoSpeakerTameSession("start_bleed_observe");
			ClearBleedSupportBridgeAliases("start_bleed_observe");

			g_inBleedState.store(true, std::memory_order_release);
			g_bleedSawDialogue = false;
			g_bleedPendingCaptiveOutcome = false;
			g_bleedPendingNonCaptiveOutcome = false;
			g_bleedStart = Now();
			g_bleedLastSeconds = -1;
			g_bleedPaused = false;
			g_bleedPauseStarted = {};
			g_bleedLastCalmPulse = {};
			g_bleedLastCrowdAssign = {};
			g_bleedCrowdAssigned.clear();
			g_bleedBattleObservePending = false;
			g_bleedBattleObservePendingEmptyEnemyTicks = 0;
			g_bleedBattleObserveActive = true;
			g_bleedBattleObserveSince = Now();
			g_bleedBattleObserveLastRedirect = {};
			g_bleedBattleObserveActiveEmptyEnemyTicks = 0;
			g_bleedBattlePreferredEnemy = preferredEnemy ? preferredEnemy->GetHandle() : RE::ActorHandle{};

			const float maxHp = player->GetPermanentActorValue(RE::ActorValue::kHealth);
			g_minHp = (std::max)(1.0f, maxHp * 0.02f);
			SetPlayerBleedImmune(true);
			ClampHealth(player, g_minHp);
			player->NotifyAnimationGraph("BleedoutStart");

			const int bleedSeconds = TFD::Settings::GetBleedWindowSeconds();
			char msg[96]{};
			std::snprintf(msg, sizeof(msg), "TFDEngine: Bleeding... allies fighting (%ds)", bleedSeconds);
			RE::DebugNotification(msg);
			spdlog::info("[TFD][Defeat] bleed battle observe started enemyCount={} allyCount={} preferred={:08X}",
				g_bleedBattleObserver.enemyIds.size(),
				g_bleedBattleObserver.allyIds.size(),
				preferredEnemy ? preferredEnemy->GetFormID() : 0u);
			return true;
		}

		static void EnterObservedBattleWin()
		{
			ClearBleedoutBridgeAliases(nullptr, "battle_observe_win");
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

			ClearBleedoutBridgeAliases(nullptr, reason ? reason : "battle_observe_loss");
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
			auto* player = Player();
			if (!player) {
				return;
			}
			SetPlayerBleedImmune(true);
			if (g_minHp > 0.0f) {
				ClampHealth(player, g_minHp);
			}

			auto followers = CollectBleedStandingFollowersFromSnapshot();
			auto enemies = CollectBleedStandingEnemiesFromSnapshot();
			spdlog::info("[TFD][Defeat] bleed observe active scan followers={} enemies={} allyThresh={:.1f} enemyThresh={:.1f}",
				followers.size(),
				enemies.size(),
				TFD::Settings::GetAllyDownedThresholdPct(),
				TFD::Settings::GetEnemyDownedThresholdPct());

			const auto now = Now();
			(void)now;

			if (followers.empty()) {
				EnterObservedLeftForDead("battle_observe_loss");
				return;
			}

			if (enemies.empty()) {
				++g_bleedBattleObserveActiveEmptyEnemyTicks;
				if (g_bleedBattleObserveActiveEmptyEnemyTicks < 8) {
					return;
				}

				if (g_bleedBattleObserver.hadValidObservedEnemy && !followers.empty()) {
					EnterObservedBattleWin();
				}
				else {
					EnterObservedLeftForDead("battle_observe_no_survivor");
				}
				return;
			}

			g_bleedBattleObserveActiveEmptyEnemyTicks = 0;
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
			UpdatePreCombatState();
			spdlog::info("[TFD][Transition] left-for-dead complete branch={} reason={}",
				NoMarkerBranchName(g_noMarkerFallback.branch),
				reason ? reason : "unknown");
		}

		static void ResolveGlobals()
		{
			if (!g_captiveStateGlobal) {
				g_captiveStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDCaptiveState");
				if (g_captiveStateGlobal && !g_loggedCaptiveStateGlobal) {
					g_loggedCaptiveStateGlobal = true;
					spdlog::info("[TFD][Defeat] TFDCaptiveState resolved {:08X}", g_captiveStateGlobal->GetFormID());
				}
			}
			if (!g_captivePhaseGlobal) {
				g_captivePhaseGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDCaptivePhase");
				if (g_captivePhaseGlobal && !g_loggedCaptivePhaseGlobal) {
					g_loggedCaptivePhaseGlobal = true;
					spdlog::info("[TFD][Defeat] TFDCaptivePhase resolved {:08X}", g_captivePhaseGlobal->GetFormID());
				}
			}
			if (!g_preCombatStateGlobal) {
				g_preCombatStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDPreCombatState");
				if (g_preCombatStateGlobal && !g_loggedPreCombatStateGlobal) {
					g_loggedPreCombatStateGlobal = true;
					spdlog::info("[TFD][Defeat] TFDPreCombatState resolved {:08X}", g_preCombatStateGlobal->GetFormID());
				}
			}
			if (!g_transitionPendingGlobal) {
				g_transitionPendingGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDTransitionPending");
				if (g_transitionPendingGlobal && !g_loggedTransitionPendingGlobal) {
					g_loggedTransitionPendingGlobal = true;
					spdlog::info("[TFD][Transition] TFDTransitionPending resolved {:08X}", g_transitionPendingGlobal->GetFormID());
				}
			}
			if (!g_transitionBusyGlobal) {
				g_transitionBusyGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDTransitionBusy");
				if (g_transitionBusyGlobal && !g_loggedTransitionBusyGlobal) {
					g_loggedTransitionBusyGlobal = true;
					spdlog::info("[TFD][Transition] TFDTransitionBusy resolved {:08X}", g_transitionBusyGlobal->GetFormID());
				}
			}
			if (!g_transitionReasonGlobal) {
				g_transitionReasonGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDTransitionReason");
				if (g_transitionReasonGlobal && !g_loggedTransitionReasonGlobal) {
					g_loggedTransitionReasonGlobal = true;
					spdlog::info("[TFD][Transition] TFDTransitionReason resolved {:08X}", g_transitionReasonGlobal->GetFormID());
				}
			}
			if (!g_transitionResultGlobal) {
				g_transitionResultGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDTransitionResult");
				if (g_transitionResultGlobal && !g_loggedTransitionResultGlobal) {
					g_loggedTransitionResultGlobal = true;
					spdlog::info("[TFD][Transition] TFDTransitionResult resolved {:08X}", g_transitionResultGlobal->GetFormID());
				}
			}
		}

		static int CinematicReasonCode(CinematicTransitionKind kind, bool fadeIn)
		{
			switch (kind) {
			case CinematicTransitionKind::Captive: return fadeIn ? 21 : 11;
			case CinematicTransitionKind::Rescue:  return fadeIn ? 22 : 12;
			case CinematicTransitionKind::Recover: return fadeIn ? 23 : 13;
			default: return 0;
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
			ResolveGlobals();
			if (!g_transitionPendingGlobal || !g_transitionReasonGlobal || !g_transitionResultGlobal) {
				spdlog::warn("[TFD][Transition] cinematic request unavailable kind={} phase={} reason={}",
					CinematicKindName(kind), fadeIn ? "fadein" : "fadeout", reason ? reason : "unknown");
				return false;
			}
			if (g_transitionBusyGlobal && g_transitionBusyGlobal->value >= 0.5f) {
				spdlog::info("[TFD][Transition] cinematic request skipped (busy) kind={} phase={} reason={}",
					CinematicKindName(kind), fadeIn ? "fadein" : "fadeout", reason ? reason : "unknown");
				return false;
			}
			if (g_transitionPendingGlobal->value >= 0.5f) {
				spdlog::info("[TFD][Transition] cinematic request skipped (pending={}) kind={} phase={} reason={}",
					g_transitionPendingGlobal->value, CinematicKindName(kind), fadeIn ? "fadein" : "fadeout", reason ? reason : "unknown");
				return false;
			}
			const int code = CinematicReasonCode(kind, fadeIn);
			if (code == 0) {
				return false;
			}
			g_transitionReasonGlobal->value = static_cast<float>(code);
			g_transitionResultGlobal->value = 0.0f;
			g_transitionPendingGlobal->value = 2.0f;
			spdlog::info("[TFD][Transition] queued cinematic kind={} phase={} code={} reason={}",
				CinematicKindName(kind), fadeIn ? "fadein" : "fadeout", code, reason ? reason : "unknown");
			return true;
		}

		static void ClearPendingCinematicFadeIn()
		{
			g_pendingCinematicFadeIn = false;
			g_pendingCinematicFadeInKind = CinematicTransitionKind::None;
			g_pendingCinematicFadeInNotBefore = {};
			g_pendingCinematicFadeInSawLoadingMenu = false;
		}

		static void SchedulePendingCinematicFadeIn(CinematicTransitionKind kind, const char* reason, int settleMs = 300)
		{
			if (kind == CinematicTransitionKind::None) {
				ClearPendingCinematicFadeIn();
				return;
			}
			g_pendingCinematicFadeIn = true;
			g_pendingCinematicFadeInKind = kind;
			g_pendingCinematicFadeInNotBefore = Now() + std::chrono::milliseconds((std::max)(0, settleMs));
			g_pendingCinematicFadeInSawLoadingMenu = false;
			spdlog::info("[TFD][Transition] scheduled fadein kind={} settleMs={} reason={}",
				CinematicKindName(kind), settleMs, reason ? reason : "unknown");
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

		static int ResolveNonCaptiveChoiceReason(const char* reason)
		{
			if (!reason || !reason[0]) {
				return 4;
			}
			if (std::strcmp(reason, "bleed_timeout") == 0) {
				return 2;
			}
			if (std::strcmp(reason, "dialogue_closed_no_marker") == 0) {
				return 3;
			}
			if (std::strcmp(reason, "no_valid_npc") == 0 ||
				std::strcmp(reason, "unsupported_aggressor_nonhumanoid") == 0 ||
				std::strcmp(reason, "unsupported_aggressor_allowlist") == 0) {
				return 1;
			}
			return 4;
		}

		static void QueueNonCaptiveChoiceRequest(const char* reason)
		{
			ResolveGlobals();
			if (!g_transitionPendingGlobal) {
				spdlog::warn("[TFD][Transition] non-captive choice skipped (globals missing) reason={}", reason ? reason : "unknown");
				return;
			}
			if (g_transitionBusyGlobal && g_transitionBusyGlobal->value >= 0.5f) {
				spdlog::info("[TFD][Transition] non-captive choice skipped (busy) reason={}", reason ? reason : "unknown");
				return;
			}
			if (g_transitionPendingGlobal->value >= 0.5f) {
				spdlog::info("[TFD][Transition] non-captive choice skipped (already pending={}) reason={}", g_transitionPendingGlobal->value, reason ? reason : "unknown");
				return;
			}
			const int mappedReason = ResolveNonCaptiveChoiceReason(reason);
			if (g_transitionReasonGlobal) {
				g_transitionReasonGlobal->value = static_cast<float>(mappedReason);
			}
			if (g_transitionResultGlobal) {
				g_transitionResultGlobal->value = 0.0f;
			}
			g_transitionPendingGlobal->value = 1.0f;
			spdlog::info("[TFD][Transition] queued non-captive choice reason={} source={}", mappedReason, reason ? reason : "unknown");
		}

		static int ConsumeTransitionResult()
		{
			ResolveGlobals();
			if (!g_transitionResultGlobal) {
				return 0;
			}
			const int result = static_cast<int>(std::lround(g_transitionResultGlobal->value));
			if (result != 0) {
				g_transitionResultGlobal->value = 0.0f;
			}
			return result;
		}

		static bool IsTransitionAwaiting()
		{
			ResolveGlobals();
			const bool pending = g_transitionPendingGlobal && g_transitionPendingGlobal->value >= 0.5f;
			const bool busy = g_transitionBusyGlobal && g_transitionBusyGlobal->value >= 0.5f;
			return pending || busy;
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
			if (player->IsInCombat()) {
				player->StopCombat();
			}
			player->DrawWeaponMagicHands(false);
			const float radius = (std::max)(3200.0f, TFD::Settings::GetSweepRadius() + 1200.0f);
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
			UpdatePreCombatState();
			spdlog::info("[TFD][Transition] recover complete branch={} reason={}", NoMarkerBranchName(g_noMarkerFallback.branch), reason ? reason : "unknown");
		}

		static void BeginRecoverTransition(const char* reason)
		{
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
			auto* player = Player();
			if (!player || !target || target == player) {
				spdlog::info("[TFD][Captive] confiscation skipped player={:08X} target={:08X} reason={}",
					player ? player->GetFormID() : 0u,
					target ? target->GetFormID() : 0u,
					reason ? reason : "unknown");
				return false;
			}

			auto* changes = player->GetInventoryChanges();
			if (!changes) {
				spdlog::info("[TFD][Captive] confiscation skipped no_inventory_changes target={:08X} reason={}",
					target->GetFormID(),
					reason ? reason : "unknown");
				return false;
			}

			const auto inv = player->GetInventory([](RE::TESBoundObject&) {
				return true;
				}, true);

			std::int32_t totalStacks = 0;
			std::int32_t totalUnits = 0;
			for (const auto& [item, invData] : inv) {
				const auto& [count, entry] = invData;
				if (!item || count <= 0 || !entry) {
					continue;
				}

				++totalStacks;
				totalUnits += count;
			}

			if (totalUnits <= 0) {
				spdlog::info("[TFD][Captive] confiscation skipped empty_inventory target={:08X} reason={}",
					target->GetFormID(),
					reason ? reason : "unknown");
				return false;
			}

			changes->RemoveAllItems(
				player,
				target,
				false,
				false,
				false);

			spdlog::info(
				"[TFD][Captive] confiscation moved_all target={:08X} stacks={} units={} keepOwnership=0 arg6=0 reason={}",
				target->GetFormID(),
				totalStacks,
				totalUnits,
				reason ? reason : "unknown");
			return true;
		}

		static bool ProcessCaptiveConfiscation(const char* reason)
		{
			if (g_captiveConfiscationApplied) {
				spdlog::info("[TFD][Captive] confiscation already applied reason={}", reason ? reason : "unknown");
				SyncCaptiveStorageDebugAliases(reason ? reason : "confiscation_already_applied");
				return false;
			}

			auto* storage = TFD::Location::ResolveNearestCaptiveStorageTarget(nullptr);
			SyncCaptiveStorageDebugAliases(reason ? reason : "confiscation_scan");
			if (!storage) {
				g_captiveConfiscationApplied = true;
				spdlog::info("[TFD][Captive] no storage target found; skipping confiscation reason={}", reason ? reason : "unknown");
				return false;
			}

			TransferPlayerInventoryToCaptiveStorage(storage, reason);
			g_captiveConfiscationApplied = true;
			return true;
		}

		static void CompleteCaptiveTransitionNow(const char* reason)
		{
			ClearNoMarkerFallbackState();
			ResetBleedRuntimeState();
			ClearBleedoutBridgeAliases(nullptr, "blackout_teleport");
			g_lastAggressor.reset();
			AdvanceGameHoursSoft(1.0f);
			if (!TeleportPlayerToCachedMarkerNow()) {
				EnterNonCaptiveChoice("teleport_failed");
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(120));
			RecoverPlayerAfterTeleport();
			SetGraceSeconds(5);
			SetCaptiveRuntime(true, CaptivePhaseValue::Captive);
			g_prevDialogueOpen = IsDialogueOpen();
			g_prevLockpickOpen = IsLockpickingOpen();
			ResetLockpickWatch();
			ArmEscapeContextFromCurrentState();
			if (g_captiveDoor.HasDoor()) {
				g_captiveDoor.SealToInitial(true);
			}
			ApplyCalmBubble((std::max)(2000.0f, TFD::Settings::GetSweepRadius()));
			const bool starterKitAllowed = ProcessCaptiveConfiscation(reason);
			SyncPlayerCaptiveAlias(Player(), reason ? reason : "captive_enter");
			if (starterKitAllowed) {
				EnsureCaptiveStarterLockpicks(3, reason ? reason : "captive_enter");
			}
			spdlog::info("[TFD][Captive] entered captivePhase reason={} starterKitAllowed={}",
				reason ? reason : "unknown",
				starterKitAllowed ? 1 : 0);
		}

		static void PollTransitionResult()
		{
			const int result = ConsumeTransitionResult();
			if (result == 0) {
				return;
			}
			if (result == 1) {
				spdlog::info("[TFD][Transition] result=completed");
				return;
			}
			if (result == 2) {
				spdlog::info("[TFD][Transition] result=cancelled");
				return;
			}
			if (result == 3) {
				spdlog::info("[TFD][Transition] result=recover_chosen");
				BeginRecoverTransition("recover_chosen");
				return;
			}
			if (result == 4) {
				spdlog::info("[TFD][Transition] result=rescue_chosen");
				if (!BeginRescueTransition("rescue_chosen")) {
					BeginRecoverTransition("rescue_fallback_recover");
				}
				return;
			}
			if (result == 101) {
				spdlog::info("[TFD][Transition] result=captive_blackout_ready");
				CompleteCaptiveTransitionNow("captived_blackout_ready");
				SchedulePendingCinematicFadeIn(CinematicTransitionKind::Captive, "captived_fadein", 350);
				return;
			}
			if (result == 102) {
				spdlog::info("[TFD][Transition] result=rescue_blackout_ready");
				if (!CompleteRescueTransitionNow("rescue_blackout_ready")) {
					g_noMarkerFallback.branch = NoMarkerFallbackBranch::LeftForDeadSolo;
					g_noMarkerFallback.destination.reset();
					g_noMarkerFallback.hasFallbackPos = false;
					ResolveLeftForDeadDestination(g_noMarkerFallback);
					CompleteRecoverTransitionNow("rescue_fallback_left_for_dead");
					SchedulePendingCinematicFadeIn(CinematicTransitionKind::Recover, "rescue_fallback_left_for_dead_fadein", 300);
				}
				else {
					SchedulePendingCinematicFadeIn(CinematicTransitionKind::Rescue, "rescue_fadein", 350);
				}
				return;
			}
			if (result == 103) {
				spdlog::info("[TFD][Transition] result=recover_blackout_ready");
				CompleteRecoverTransitionNow("recover_blackout_ready");
				SchedulePendingCinematicFadeIn(CinematicTransitionKind::Recover, "recover_fadein", 300);
				return;
			}
			if (result == 201 || result == 202 || result == 203) {
				spdlog::info("[TFD][Transition] result=fadein_complete code={}", result);
				return;
			}
			spdlog::info("[TFD][Transition] result={} (unknown)", result);
		}

		static void SyncCaptiveGlobals(bool stateActive, CaptivePhaseValue phase)
		{
			ResolveGlobals();
			if (g_captiveStateGlobal) {
				g_captiveStateGlobal->value = stateActive ? 1.0f : 0.0f;
			}
			if (g_captivePhaseGlobal) {
				g_captivePhaseGlobal->value = static_cast<float>(static_cast<int>(phase));
			}
		}

		static void SyncPreCombatGlobal(bool active)
		{
			ResolveGlobals();
			if (g_preCombatStateGlobal) {
				g_preCombatStateGlobal->value = active ? 1.0f : 0.0f;
			}
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
			}
		}

		static void SetCaptiveRuntime(bool stateActive, CaptivePhaseValue phase)
		{
			SetCaptiveRuntimeOnly(stateActive, phase);
			SyncCaptiveGlobals(stateActive, phase);
		}

		static void UpdatePreCombatState()
		{
			auto* player = Player();
			bool preCombat = false;
			if (player) {
				preCombat = true;
				if (g_loadTransition.load(std::memory_order_acquire)) preCombat = false;
				if (g_inBleedState.load(std::memory_order_acquire)) preCombat = false;
				if (g_captiveState) preCombat = false;
				if (player->IsInCombat()) preCombat = false;
				if (g_leftForDeadActive) preCombat = false;
				auto* st = player->AsActorState();
				if (st && st->IsBleedingOut()) preCombat = false;
			}
			SyncPreCombatGlobal(preCombat);
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

		static RE::TESObjectCELL* GetParentCell(RE::TESObjectREFR* ref)
		{
			return ref ? ref->GetParentCell() : nullptr;
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

			g_lastAggressor.reset();
			SetCaptiveRuntime(false, CaptivePhaseValue::None);
			ClearEscapeContext();
			UpdatePreCombatState();
			spdlog::info("[TFD][Captive] Escape broken by defeat threshold pct={:.1f} thresh={:.1f} -> clear stale aggressor", pct, thresh);
			return true;
		}

		static void NormalizeInvalidCaptivePair()
		{
			if (g_captiveState && g_captivePhase == CaptivePhaseValue::None) {
				SetCaptiveRuntime(true, CaptivePhaseValue::Escape);
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
			auto* player = Player();
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

		static RE::Actor* ResolveRecentPreCombatAggressor(float radius)
		{
			auto* player = Player();
			if (!player) {
				return nullptr;
			}

			auto* actor = TFD::PreCombatGreet::GetRecentActor(12.0);
			if (!actor) {
				return nullptr;
			}

			float dist = -1.0f;
			if (!IsReasonableCombatAggressor(actor, player, radius, &dist)) {
				return nullptr;
			}

			spdlog::info("[TFD][Defeat] using recent precombat actor {:08X} as defeat aggressor dist={:.1f}", actor->GetFormID(), dist);
			return actor;
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

			if (auto* recent = ResolveRecentPreCombatAggressor(maxAggressorDist)) {
				g_lastAggressor = recent->GetHandle();
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
			if (outDistance) {
				*outDistance = 99999.0f;
			}
			if (!actor || !player) {
				return false;
			}
			if (!IsStandingEnemyThresholdActor(actor) || !actor->Is3DLoaded()) {
				return false;
			}
			if (actor->GetFormID() == player->GetFormID()) {
				return false;
			}
			if (!IsCaptiveSupportedAggressor(actor)) {
				return false;
			}
			if (!IsBleedSpaceCompatible(actor, player)) {
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
			if (dist > maxDist) {
				return false;
			}
			return true;
		}

		static RE::Actor* FindBestBleedoutSpeaker(float radius, float maxDist, RE::Actor* preferred)
		{
			auto* player = Player();
			if (!player) return nullptr;
			float preferredDist = 99999.0f;
			const bool preferredValid = IsReasonableBleedoutSpeaker(preferred, player, maxDist, &preferredDist);

			TFD::ActorScan::Rescan(radius, true);
			RE::Actor* best = nullptr;
			float bestScore = 1.0e30f;
			const auto n = TFD::ActorScan::GetCount();
			for (int i = 0; i < n; ++i) {
				auto e = TFD::ActorScan::GetEntry(i);
				auto sp = e.actor.get();
				auto* a = sp.get();
				if (!IsStandingEnemyThresholdActor(a)) continue;
				if (!a->Is3DLoaded()) continue;
				if (a->GetFormID() == player->GetFormID()) continue;
				if (!IsBleedSpaceCompatible(a, player)) continue;
				if (!e.inCombat && !e.hostile) continue;
				if (!IsCaptiveSupportedAggressor(a)) continue;
				if (e.dist > maxDist) continue;

				float score = e.dist;
				if (e.hostile) score -= 120.0f;
				if (e.inCombat) score -= 80.0f;
				if (IsActorCloseAndFront(a, player, 320.0f)) score -= 160.0f;
				if (a == preferred && preferredValid) score -= 40.0f;
				if (score < bestScore) {
					bestScore = score;
					best = a;
				}
			}

			if (best) {
				if (!preferredValid) {
					spdlog::info("[TFD][Defeat] local bleedout speaker {:08X} selected (no valid preferred)", best->GetFormID());
					return best;
				}

				float bestDist = 99999.0f;
				IsReasonableBleedoutSpeaker(best, player, maxDist, &bestDist);
				if (best != preferred && bestDist + 128.0f < preferredDist) {
					spdlog::info(
						"[TFD][Defeat] replacing stale/far speaker {:08X} dist={:.1f} with local {:08X} dist={:.1f}",
						preferred ? preferred->GetFormID() : 0,
						preferredDist,
						best->GetFormID(),
						bestDist);
					return best;
				}
			}

			if (preferredValid) {
				return preferred;
			}
			return best;
		}


		static bool CanUseAggressorForBleedoutGreet(RE::Actor* player, RE::Actor* aggressor, float& outDistance)
		{
			outDistance = 99999.0f;
			if (!player || !aggressor) {
				return false;
			}
			if (!IsStandingEnemyThresholdActor(aggressor) || !aggressor->Is3DLoaded()) {
				return false;
			}
			if (!IsBleedSpaceCompatible(aggressor, player)) {
				return false;
			}

			const auto pa = player->GetPosition();
			const auto pb = aggressor->GetPosition();
			const float dx = pb.x - pa.x;
			const float dy = pb.y - pa.y;
			const float dz = pb.z - pa.z;
			outDistance = std::sqrt(dx * dx + dy * dy + dz * dz);

			if (outDistance > 1400.0f) {
				return false;
			}

			return true;
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

		static void BeginLeftForDeadBlackout(int, const char*)
		{
			// Disabled on C++ side. Native wait / blackout will be handled by CK/Papyrus.
		}

		static bool TickLeftForDeadBlackout()
		{
			return false;
		}

		static void EnterNonCaptiveChoice(const char* reason);
		static bool BeginResolvedNoMarkerFallback(const char* reason);

		static void StartBleedWindow(RE::Actor* player, RE::Actor* aggressor)
		{
			if (!player) {
				return;
			}

			ClearBleedoutBridgeAliases(aggressor, "start_bleed_window");
			ClearNoMarkerFallbackState();
			ReleaseBleedNoSpeakerTameSession("start_bleed_window");
			ClearBleedSupportBridgeAliases("start_bleed_window");

			g_inBleedState.store(true, std::memory_order_release);
			g_bleedSawDialogue = false;
			g_bleedPendingCaptiveOutcome = false;
			g_bleedPendingNonCaptiveOutcome = false;
			g_bleedStart = Now();
			g_bleedLastSeconds = -1;
			g_bleedPaused = false;
			g_bleedPauseStarted = {};
			g_bleedLastCalmPulse = {};
			g_bleedLastCrowdAssign = {};
			g_bleedCrowdAssigned.clear();
			g_bleedBattleObservePending = false;
			g_bleedBattleObservePendingUntil = {};
			g_bleedBattleObservePendingLastRedirect = {};
			g_bleedBattleObservePendingEmptyEnemyTicks = 0;
			g_bleedBattleObserveActive = false;
			g_bleedBattleObserveSince = {};
			g_bleedBattleObserveLastRedirect = {};
			g_bleedBattleObserveActiveEmptyEnemyTicks = 0;
			g_bleedBattlePreferredEnemy.reset();

			const float maxHp = player->GetPermanentActorValue(RE::ActorValue::kHealth);
			const float minHp = (std::max)(1.0f, maxHp * 0.02f);
			g_minHp = minHp;
			SetPlayerBleedImmune(true);
			ClampHealth(player, g_minHp);
			player->NotifyAnimationGraph("BleedoutStart");

			const float radius = (std::max)(12000.0f, TFD::Settings::GetSweepRadius());
			const float maxSpeakerDist = 900.0f;
			aggressor = FindBestBleedoutSpeaker(radius, maxSpeakerDist, aggressor);
			if (aggressor) {
				float chosenDist = 99999.0f;
				IsReasonableBleedoutSpeaker(aggressor, player, maxSpeakerDist, &chosenDist);
				spdlog::info("[TFD][Defeat] bleed speaker locked {:08X} dist={:.1f}", aggressor->GetFormID(), chosenDist);
			}
			else {
				spdlog::info("[TFD][Defeat] no local bleed speaker within {:.0f} -> hold without greet", maxSpeakerDist);
			}
			auto initialCrowd = CollectBleedoutCrowd(radius, aggressor, false);
			if (aggressor) {
				ClearBleedoutBridgeAliases(aggressor, "start_bleed_window_primary");
				AssignBleedoutBridgeActor(aggressor);
			}
			else {
				ClearBleedoutBridgeAliases(nullptr, "start_bleed_window_no_speaker");
				ClearBleedSupportBridgeAliases("start_bleed_window_no_speaker");
			}

			if (aggressor) {
				g_lastAggressor = aggressor->GetHandle();
				g_bleedSpeakerId = aggressor->GetFormID();
				if (const auto sessionId = TFD::Pacify::BeginTruceInCombatSession(player, aggressor, 0.0, true); sessionId.has_value()) {
					g_bleedTruceSessionId = *sessionId;
					spdlog::info("[TFD][Defeat] bleed truce session started id={} speaker={:08X}", g_bleedTruceSessionId, g_bleedSpeakerId);
				}
				else {
					spdlog::warn("[TFD][Defeat] bleed truce session failed speaker={:08X}", g_bleedSpeakerId);
				}

				if (!IsCaptiveSupportedAggressor(aggressor)) {
					g_bleedPendingCaptiveOutcome = false;
					g_bleedPendingNonCaptiveOutcome = true;
					spdlog::info("[TFD][Defeat] aggressor {:08X} not captive-supported (non-humanoid) -> keep bleed hold pending=noncaptive", aggressor->GetFormID());
				}
				else {
					const bool allowlistSupported = TFD::FactionMask::ApplyFromAggressor(aggressor);
					const bool hasCaptiveOutcome = ResolveCaptiveMarkerForOutcome();
					float fallbackDistance = 99999.0f;
					const bool fallbackSupported = !allowlistSupported &&
						CanUseCaptiveFallbackHeuristic(player, aggressor, hasCaptiveOutcome, fallbackDistance);

					if (!allowlistSupported && fallbackSupported) {
						spdlog::info(
							"[TFD][Defeat] captive fallback accepted via marker+speaker heuristic actor={:08X} dist={:.1f}",
							aggressor->GetFormID(),
							fallbackDistance);
					}

					if (!allowlistSupported && !fallbackSupported) {
						g_bleedPendingCaptiveOutcome = false;
						g_bleedPendingNonCaptiveOutcome = true;
						spdlog::info(
							"[TFD][Defeat] aggressor {:08X} has no captive-supported allowlist faction and fallback rejected (marker={} dist={:.1f}) -> keep bleed hold pending=noncaptive",
							aggressor->GetFormID(),
							hasCaptiveOutcome ? 1 : 0,
							fallbackDistance);
					}
					else {
						g_bleedPendingCaptiveOutcome = hasCaptiveOutcome;
						g_bleedPendingNonCaptiveOutcome = !hasCaptiveOutcome;

						float greetDistance = 99999.0f;
						const bool greetableNow = CanUseAggressorForBleedoutGreet(player, aggressor, greetDistance);
						if (!greetableNow) {
							spdlog::info("[TFD][Defeat] aggressor {:08X} not greetable now dist={:.1f} -> keep bleed hold pending={}",
								aggressor->GetFormID(), greetDistance, hasCaptiveOutcome ? "captive" : "noncaptive");
						}

						if (greetableNow) {
						}
					}
				}
			}
			else {
				const bool hasCaptiveOutcome = ResolveCaptiveMarkerForOutcome();
				g_bleedPendingCaptiveOutcome = hasCaptiveOutcome;
				g_bleedPendingNonCaptiveOutcome = !hasCaptiveOutcome;
				const bool tameHeld = TryEnsureBleedNoSpeakerTameSession(initialCrowd, "start_bleed_window_no_speaker");
				spdlog::info("[TFD][Defeat] no speaker -> keep bleed hold pending={} tameHeld={}", hasCaptiveOutcome ? "captive" : "noncaptive", tameHeld ? 1 : 0);
			}

			const int bleedSeconds = TFD::Settings::GetBleedWindowSeconds();
			char msg[96]{};
			std::snprintf(msg, sizeof(msg), "TFDEngine: Bleeding... (%ds)", bleedSeconds);
			RE::DebugNotification(msg);
			spdlog::info("[TFD][Defeat] bleed window started ({}s)", bleedSeconds);
		}

		static void EnterEscapeFromLockpick(RE::TESObjectREFR* door)
		{
			EnterEscapeCommit("lockpick", door);
		}

		static bool BeginResolvedNoMarkerFallback(const char* reason)
		{
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

			ClearBleedoutBridgeAliases(nullptr, reason ? reason : "noncaptive");
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
			if (BeginResolvedNoMarkerFallback(reason)) {
				return;
			}

			ClearBleedoutBridgeAliases(nullptr, reason ? reason : "noncaptive");
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
			QueueNonCaptiveChoiceRequest(reason);
			spdlog::warn("[TFD][Transition] fallback to legacy non-captive choice reason={}", reason ? reason : "unknown");
		}

		static void DoBlackoutTeleport()
		{
			ResetBleedRuntimeState();
			ClearBleedoutBridgeAliases(nullptr, "blackout_teleport");
			g_lastAggressor.reset();
			RE::DebugNotification("TFDEngine: Blackout -> Captive (1h)");
			if (!ResolveCaptiveMarkerForOutcome()) {
				EnterNonCaptiveChoice("marker_not_found");
				return;
			}
			ClearPendingCinematicFadeIn();
			if (QueueCinematicTransitionRequest(CinematicTransitionKind::Captive, false, "captive_blackout")) {
				return;
			}
			ShowBlackoutFader();
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			CompleteCaptiveTransitionNow("captive_blackout_fallback");
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			HideBlackoutFader();
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
			if (IsTransitionAwaiting() || g_pendingCinematicFadeIn) {
				MaintainTransitionCalmWindow();
				UpdatePreCombatState();
				return;
			}
			NormalizeInvalidCaptivePair();
			if (g_captiveState && g_captivePhase == CaptivePhaseValue::Captive) {
				const bool dialogOpen = IsDialogueOpen();
				if (!dialogOpen && g_prevDialogueOpen) {
					ApplyCalmBubble((std::max)(1800.0f, TFD::Settings::GetSweepRadius()));
					spdlog::info("[TFD][Captive] Dialogue closed -> calm burst");
				}
				g_prevDialogueOpen = dialogOpen;
				UpdateLockpickEscapeWatch();
				TryCommitEscapeByRadius();
			}
			else if (g_captiveState && g_captivePhase == CaptivePhaseValue::Escape) {
				TryResolveEscapeByLocation();
				if (g_captiveState && g_captivePhase == CaptivePhaseValue::Escape && !BreakEscapeOnDefeatThreshold(Player())) {
					return;
				}
			}

			if (g_captiveState) {
				return;
			}

			if (ui && ui->GameIsPaused()) return;
			auto* player = Player();
			if (!player) {
				SyncPreCombatGlobal(false);
				return;
			}
			TickBleedLocks();
			UpdatePreCombatState();
			if (IsLeftForDeadCooldownActive()) {
				TickLeftForDeadCooldown();
				return;
			}
			if (IsGraceActive()) return;
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
				const bool dOpen = IsDialogueOpen();
				const int bleedSeconds = TFD::Settings::GetBleedWindowSeconds();
				if (dOpen) {
					g_bleedSawDialogue = true;
					if (!g_bleedPaused) {
						g_bleedPaused = true;
						g_bleedPauseStarted = Now();
						spdlog::info("[TFD][Defeat] bleed countdown paused by dialogue");
					}
					g_prevDialogueOpen = true;
					return;
				}
				if (g_bleedPaused) {
					g_bleedStart += (Now() - g_bleedPauseStarted);
					g_bleedPaused = false;
					g_bleedPauseStarted = {};
					g_bleedLastSeconds = -1;
					spdlog::info("[TFD][Defeat] bleed countdown resumed after dialogue");
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
				if (g_bleedSawDialogue && g_prevDialogueOpen) {
					g_prevDialogueOpen = false;
					ReleaseBleedTruceSession(TFD::Pacify::ReleaseReason::DialogueClosed);
					if (ResolveCaptiveMarkerForOutcome()) {
						spdlog::info("[TFD][Defeat] bleedout dialogue closed -> captive marker found");
						DoBlackoutTeleport();
						SetGraceSeconds(4);
					}
					else {
						spdlog::info("[TFD][Defeat] bleedout dialogue closed -> no marker -> resolve fallback");
						ReleaseBleedNoSpeakerTameSession("dialogue_closed_no_marker");
						g_inBleedState.store(false, std::memory_order_release);
						g_minHp = 0.0f;
						g_bleedSawDialogue = false;
						g_bleedLastSeconds = -1;
						EnterNonCaptiveChoice("dialogue_closed_no_marker");
					}
					return;
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
					ReleaseBleedTruceSession(TFD::Pacify::ReleaseReason::Generic);
					const bool pendingCaptive = g_bleedPendingCaptiveOutcome;
					if (pendingCaptive && ResolveCaptiveMarkerForOutcome()) {
						ResetBleedRuntimeState();
						spdlog::info("[TFD][Defeat] bleed timeout -> captive blackout");
						DoBlackoutTeleport();
						SetGraceSeconds(4);
					}
					else {
						spdlog::info("[TFD][Defeat] bleed timeout -> resolve no-marker fallback");
						EnterNonCaptiveChoice("bleed_timeout");
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
				aggressor = FindBestBleedoutSpeaker(scanRadius, 900.0f, aggressor);
				if (aggressor && !IsObserverAlly(aggressor)) {
					g_lastAggressor = aggressor->GetHandle();
				}
				if (!aggressor) {
					spdlog::info("[TFD][Defeat] no dialogue-capable aggressor and no standing follower -> direct non-captive resolve");
					EnterNonCaptiveChoice("no_valid_npc");
					SetGraceSeconds(1);
					return;
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
		SetCaptiveRuntimeOnly(false, CaptivePhaseValue::None);
		g_prevDialogueOpen = false;
		ResetLockpickWatch();
		ClearEscapeContext();
		SetPlayerBleedImmune(false);
		ResetBleedRuntimeState();
		ClearAllBleedLocks("install");
		ClearBleedoutBridgeAliases(nullptr, "install");
		TFD::FactionMask::Initialize();
		TFD::Location::Initialize();
		if (auto* src = SKSE::GetModCallbackEventSource()) {
			src->AddEventSink(&g_defeatedRecruitEventSink);
		}
		ClearPendingDefeatedDialogueTargetInternal();
		g_worker = std::thread([]() { WorkerLoop(); });
		TFD::DefeatMonitor::ApplyQueuedProgressState();
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
		SetPlayerBleedImmune(false);
		ResetBleedRuntimeState();
		ClearAllBleedLocks("shutdown");
		ClearBleedoutBridgeAliases(nullptr, "shutdown");
		g_loadTransition.store(false, std::memory_order_release);
		ResetLockpickWatch();
		ClearEscapeContext();
		ClearLeftForDeadCooldown();
		if (auto* src = SKSE::GetModCallbackEventSource()) {
			src->RemoveEventSink(&g_defeatedRecruitEventSink);
		}
		ClearPendingDefeatedDialogueTargetInternal();
		spdlog::info("[TFD][Defeat] monitor shutdown");
	}

	void ResetGrace()
	{
		g_grace.store(false, std::memory_order_release);
		ClearLeftForDeadCooldown();
	}

	bool GetCaptiveStateForSave()
	{
		return g_captiveState;
	}

	std::uint32_t GetCaptivePhaseForSave()
	{
		if (g_captiveState && g_captivePhase == CaptivePhaseValue::None) return 2u;
		return static_cast<std::uint32_t>(static_cast<int>(g_captivePhase));
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
		ClearBleedoutBridgeAliases(nullptr, "apply_queued_state");
		UpdatePreCombatState();
		spdlog::info("[TFD][Defeat] ApplyQueuedProgressState state={} phase={}", g_queuedCaptiveState ? 1 : 0, static_cast<int>(g_queuedCaptivePhase));
	}

	void ResetForLoad()
	{
		g_grace.store(false, std::memory_order_release);
		SetPlayerBleedImmune(false);
		ResetBleedRuntimeState();
		ClearAllBleedLocks("reset_for_load");
		ClearBleedoutBridgeAliases(nullptr, "reset_for_load");
		g_lastAggressor = RE::ActorHandle{};
		ClearLastEnemyTargetingPlayerInternal();
		SetCaptiveRuntimeOnly(false, CaptivePhaseValue::None);
		g_prevDialogueOpen = false;
		ResetLockpickWatch();
		ClearEscapeContext();
		TFD::FactionMask::Clear();
		TFD::AggressionClamp::Clear();
		ClearLeftForDeadCooldown();
		spdlog::info("[TFD][Defeat] ResetForLoad -> runtime only");
	}

	void SetLoadTransition(bool active)
	{
		g_loadTransition.store(active, std::memory_order_release);
		if (active) {
			SetPlayerBleedImmune(false);
			ClearAllBleedLocks("set_load_transition");
			ClearBleedoutBridgeAliases(nullptr, "set_load_transition");
			ResetLockpickWatch();
			spdlog::info("[TFD][Defeat] SetLoadTransition(true)");
		}
		else {
			spdlog::info("[TFD][Defeat] SetLoadTransition(false)");
		}
	}

	bool IsLeftForDeadRecoveryActive()
	{
		return g_leftForDeadActive;
	}

	bool IsCaptivePhase()
	{
		return g_captiveState && g_captivePhase == CaptivePhaseValue::Captive;
	}

	std::uint32_t GetCaptivePhaseRaw()
	{
		if (g_captiveState && g_captivePhase == CaptivePhaseValue::None) {
			return 2u;  // legacy/normalized escape fallback
		}
		return static_cast<std::uint32_t>(static_cast<int>(g_captivePhase));
	}

	const char* GetCaptivePhaseName()
	{
		switch (g_captivePhase) {
		case CaptivePhaseValue::None:
			return g_captiveState ? "Escape" : "None";
		case CaptivePhaseValue::Captive:
			return "Captive";
		case CaptivePhaseValue::Escape:
			return "Escape";
		default:
			return "Unknown";
		}
	}

	bool IsCaptiveFamily()
	{
		return g_captiveState || g_captivePhase != CaptivePhaseValue::None;
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
