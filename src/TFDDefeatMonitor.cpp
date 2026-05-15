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
#include <limits>

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
#include "TFDPostDefeatState.h"
#include "TFDDefeatBridge.h"
#include "TFDDefeatRuntimeProviders.h"
#include "TFDDefeatLifecycleBootstrap.h"
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
		constexpr const char* kBleedoutGreetConfirmedEvent = "TFDBleedoutGreetConfirmed";

		static std::uint32_t ResolveBleedFlowActorFormID();
		static TFD::Bleedout::RuntimeHostStateRefs BuildBleedRuntimeHostStateRefs();
		static TFD::Bleedout::DialogueHotkeyHandlers BuildBleedDialogueHotkeyHandlers();
		static TFD::Bleedout::RuntimeHostHandlers BuildLocalBleedRuntimeHostHandlers();
		static TFD::Bleedout::RuntimeHostHandlers BuildBleedRuntimeHostHandlers();
		static void ApplyBleedDialogueOverdrive(RE::Actor* player, RE::Actor* speaker, const char* reason, bool restartDialogue);
		static bool PromoteNextBleedSpeakerFromTruceQueue(RE::Actor* player, const char* reason, bool rejectCurrent);
		static void MaintainBleedPrimaryCaptorBinding();
		static bool IsStandingAllyThresholdActor(RE::Actor* actor);
		static TFD::Transition::RuntimeHandlers BuildTransitionRuntimeHandlers();

		using BleedDialogueOutcome = TFD::Bleedout::DialogueOutcome;
		using BleedTerminalCommit = TFD::Bleedout::TerminalCommit;

		std::atomic_bool g_installed{ false };
		std::atomic_bool g_running{ false };
		std::atomic_bool g_loadTransition{ false };
		std::atomic_flag g_tickPending = ATOMIC_FLAG_INIT;
		std::thread g_worker{};

		static bool g_lastRouterCombatContextActive = false;


		std::atomic_bool g_inBleedState{ false };
		float g_minHp{ 0.0f };
		bool g_playerBleedImmuneForced = false;
		bool g_playerWasEssential = false;
		bool g_playerWasInvulnerable = false;
		bool g_playerWasNoBleedoutRecovery = false;
		bool g_playerWasBaseInvulnerable = false;

		static std::chrono::steady_clock::time_point Now()
		{
			return std::chrono::steady_clock::now();
		}

		std::chrono::steady_clock::time_point g_bleedStart{};
		int g_bleedLastSeconds = -1;
		bool g_bleedPaused = false;
		std::chrono::steady_clock::time_point g_bleedPauseStarted{};
		std::chrono::steady_clock::time_point g_bleedLastCalmPulse{};

		bool g_bleedStickyReopenGraceActive = false;
		std::chrono::steady_clock::time_point g_bleedStickyReopenGraceUntil{};
		std::uint32_t g_bleedStickyReopenGraceSpeakerID = 0;
		float g_bleedStickyReopenGraceDistance = 99999.0f;

		std::atomic_bool g_grace{ false };
		std::chrono::steady_clock::time_point g_graceUntil{};

		bool g_pendingCaptiveRecaptureRecovery = false;
		std::chrono::steady_clock::time_point g_pendingCaptiveRecaptureRecoveryUntil{};
		std::chrono::steady_clock::time_point g_pendingCaptiveRecaptureRecoveryLastPulse{};
		std::string g_pendingCaptiveRecaptureRecoveryReason{};

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

		static RE::PlayerCharacter* Player();
		static bool IsCaptiveSupportedAggressor(RE::Actor* actor);
		static bool IsStandingEnemyThresholdActor(RE::Actor* actor);
		static bool IsBleedSpaceCompatible(RE::Actor* actor, RE::Actor* player);
		static RE::Actor* ResolveCurrentCombatTarget(RE::Actor* actor);
		static bool IsActiveFollowerActor(RE::Actor* actor);
		static bool IsActorCloseAndFront(RE::Actor* actor, RE::Actor* player, float maxDist);

		RE::ActorHandle g_lastAggressor{};

		static RE::Actor* FindBestBleedoutSpeaker(float radius, float maxDist, RE::Actor* preferred)
		{
			auto* player = Player();
			if (!player) {
				return nullptr;
			}

			const float scanRadius = (std::max)(12000.0f, (std::max)(radius, maxDist));
			auto isCandidate = [&](RE::Actor* actor, float* outDistance = nullptr) -> bool {
				if (outDistance) {
					*outDistance = -1.0f;
				}
				if (!actor || actor == player || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
					return false;
				}
				if (!IsCaptiveSupportedAggressor(actor)) {
					return false;
				}
				if (!IsStandingEnemyThresholdActor(actor)) {
					return false;
				}
				if (!IsBleedSpaceCompatible(actor, player)) {
					return false;
				}

				const auto actorPos = actor->GetPosition();
				const auto playerPos = player->GetPosition();
				const float dx = actorPos.x - playerPos.x;
				const float dy = actorPos.y - playerPos.y;
				const float dz = actorPos.z - playerPos.z;
				const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
				if (outDistance) {
					*outDistance = dist;
				}
				if (dist > scanRadius) {
					return false;
				}

				auto* currentTarget = ResolveCurrentCombatTarget(actor);
				if (currentTarget == player) {
					return true;
				}
				if (currentTarget && IsActiveFollowerActor(currentTarget)) {
					return true;
				}
				if (actor->IsHostileToActor(player) || actor->IsInCombat()) {
					return true;
				}
				if (preferred && actor == preferred) {
					return true;
				}
				if (g_lastAggressor) {
					auto lastSp = RE::Actor::LookupByHandle(g_lastAggressor.native_handle());
					if (lastSp.get() == actor) {
						return true;
					}
				}
				return false;
				};

			float preferredDist = -1.0f;
			if (preferred && isCandidate(preferred, &preferredDist)) {
				spdlog::info("[TFD][Defeat][R100A] bleed speaker preferred actor={:08X} dist={:.1f}",
					preferred->GetFormID(), preferredDist);
				return preferred;
			}

			auto snapshot = TFD::Actor::BuildSnapshot(scanRadius, false);
			if (preferred) {
				if (const auto* preferredInfo = TFD::Actor::FindActorInfo(snapshot, preferred); preferredInfo && preferredInfo->coalitionID >= 0) {
					if (auto* coalitionSpeaker = TFD::Actor::ResolveSpeakerCandidate(snapshot, preferredInfo->coalitionID)) {
						float coalitionDist = -1.0f;
						if (isCandidate(coalitionSpeaker, &coalitionDist)) {
							spdlog::info("[TFD][Defeat][R100A] bleed speaker preferred coalition actor={:08X} dist={:.1f}",
								coalitionSpeaker->GetFormID(), coalitionDist);
							return coalitionSpeaker;
						}
					}
				}
			}

			if (snapshot.winningCoalitionCandidateID >= 0) {
				if (auto* coalitionSpeaker = TFD::Actor::ResolveSpeakerCandidate(snapshot, snapshot.winningCoalitionCandidateID)) {
					float coalitionDist = -1.0f;
					if (isCandidate(coalitionSpeaker, &coalitionDist)) {
						spdlog::info("[TFD][Defeat][R100A] bleed speaker winning coalition actor={:08X} dist={:.1f}",
							coalitionSpeaker->GetFormID(), coalitionDist);
						return coalitionSpeaker;
					}
				}
			}

			RE::Actor* best = nullptr;
			float bestScore = std::numeric_limits<float>::max();
			RE::Actor* last = nullptr;
			if (g_lastAggressor) {
				auto lastSp = RE::Actor::LookupByHandle(g_lastAggressor.native_handle());
				last = lastSp.get();
			}
			for (const auto& info : snapshot.actors) {
				auto* actor = info.get();
				float dist = -1.0f;
				if (!isCandidate(actor, &dist)) {
					continue;
				}

				auto* currentTarget = ResolveCurrentCombatTarget(actor);
				const bool targetsPlayer = currentTarget == player;
				const bool targetsFollower = currentTarget && IsActiveFollowerActor(currentTarget);
				const bool hostile = info.hostileToPlayer || actor->IsHostileToActor(player);
				const bool inCombat = info.inCombat || actor->IsInCombat();
				const bool front = IsActorCloseAndFront(actor, player, 448.0f);
				const bool los = ActorHasLineOfSightToPlayer(actor, player);

				float score = dist;
				if (targetsPlayer) score -= 1200.0f;
				if (targetsFollower) score -= 850.0f;
				if (hostile) score -= 360.0f;
				if (inCombat) score -= 250.0f;
				if (front) score -= 160.0f;
				if (los) score -= 80.0f;
				if (actor == preferred) score -= 600.0f;
				if (last && last == actor) score -= 300.0f;

				if (score < bestScore) {
					bestScore = score;
					best = actor;
				}
			}

			spdlog::info("[TFD][Defeat][R100A] bleed speaker scan radius={:.1f} best={:08X} score={:.1f}",
				scanRadius, best ? best->GetFormID() : 0u, best ? bestScore : 0.0f);
			return best;
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
		int g_bleedBattleObservePendingEmptyAllyTicks = 0;
		bool g_bleedBattleObserveActive = false;
		std::chrono::steady_clock::time_point g_bleedBattleObserveSince{};
		std::chrono::steady_clock::time_point g_bleedBattleObserveLastRedirect{};
		int g_bleedBattleObserveActiveEmptyEnemyTicks = 0;
		int g_bleedBattleObserveActiveEmptyAllyTicks = 0;
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

		static constexpr double kDefeatedEnemyKnockSeconds = 5.0;
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
			if (ActorHasKeywordByEditorID(actor, "ActorTypeCreature") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeAnimal") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeDragon") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeDaedra") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeGhost") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeUndead")) {
				return false;
			}
			return ActorHasKeywordByEditorID(actor, "ActorTypeNPC");
		}

		static RE::Actor* ResolveAggressor();
		static RE::Actor* ResolveLastEnemyTargetingPlayerInternal(float radius = 0.0f, double maxAgeSec = 15.0);
		static void NoteEnemyTargetingPlayerInternal(RE::Actor* actor);
		static void ClearLastEnemyTargetingPlayerInternal();
		static RE::Actor* FindBestAggressor(float radius);
		static RE::Actor* ResolvePendingDefeatedDialogueTargetInternal();
		static void SetPendingDefeatedDialogueTargetInternal(RE::Actor* actor);
		static void ClearPendingDefeatedDialogueTargetInternal();

		static void SetPendingDefeatedDialogueTargetInternal(RE::Actor* actor)
		{
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				ClearPendingDefeatedDialogueTargetInternal();
				return;
			}

			g_pendingDefeatedDialogueTarget = actor->CreateRefHandle();
			g_pendingDefeatedDialogueExpiry = std::chrono::steady_clock::now() +
				std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(kPendingDefeatedDialogueTargetSeconds));
			spdlog::info("[TFD][Defeat] pending defeated dialogue target set actor={:08X} seconds={:.1f}",
				actor->GetFormID(),
				kPendingDefeatedDialogueTargetSeconds);
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

		static bool TryGetDefeatedEnemyStateHook(RE::Actor* actor, std::uint8_t* lockKindValue, bool* defeatedManaged, std::chrono::steady_clock::time_point* deadline)
		{
			if (!actor) {
				return false;
			}

			auto it = g_bleedLocks.find(actor->GetFormID());
			if (it == g_bleedLocks.end()) {
				return false;
			}

			const auto& entry = it->second;
			if (lockKindValue) {
				*lockKindValue = static_cast<std::uint8_t>(entry.kind);
			}
			if (defeatedManaged) {
				*defeatedManaged = entry.defeatedManaged;
			}
			if (deadline) {
				*deadline = entry.defeatedDeadline;
			}

			return true;
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
		static bool IsObserverAlly(RE::Actor* actor);
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
		static void ClampHealth(RE::Actor* actor, float minHp);
		static bool ComputePlayerBleedOutState(RE::Actor* player);

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

		static void RefreshPostDefeatGlobals();
		static void SetRescueStateValue(int value);
		static void RecoverVictoryTeammates();

		static void SetRescueStateValue(int value)
		{
			TFD::Rescue::SetStateValue(value);
		}

		static bool IsActorCloseAndFront(RE::Actor* actor, RE::Actor* player, float maxDist);
		static bool IsBleedSpaceCompatible(RE::Actor* actor, RE::Actor* player);

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

		static void QueueCaptiveRecaptureBridgeCleanup(const char* reason)
		{
			const char* why = reason ? reason : "captive_recapture_cleanup";
			(void)TFD::FlowController::QueueBridgeModEvent("TFDBleedoutClearAll", nullptr, why, 1.0f);
			(void)TFD::FlowController::QueueBridgeModEvent("TFDTruceClearAll", nullptr, why, 1.0f);
			(void)TFD::FlowController::QueueBridgeModEvent("TFDTruceHardClearAll", nullptr, why, 1.0f);
			(void)TFD::FlowController::QueueBridgeModEvent("TFDSystemEventClearAfterPleasure", nullptr, why, 1.0f);
		}

		static void ArmCaptiveRecaptureRecoveryPulse(const char* reason)
		{
			const char* why = reason ? reason : "captive_recapture_post_teleport";
			g_pendingCaptiveRecaptureRecovery = true;
			g_pendingCaptiveRecaptureRecoveryUntil = Now() + std::chrono::milliseconds(4500);
			g_pendingCaptiveRecaptureRecoveryLastPulse = {};
			g_pendingCaptiveRecaptureRecoveryReason = why;
			QueueCaptiveRecaptureBridgeCleanup(why);
			ForceRecoverPlayerAfterCaptiveRecapture(why);
		}

		static void ReleasePlayerBleedLock(const char* reason, bool playGetUp);

		static void ResetBleedRuntimeState(bool preserveCaptive = false)
		{
			TFD::Bleedout::RuntimeResetHandlers handlers{};
			handlers.releasePlayerBleedLock = [&](const char* reason) { ReleasePlayerBleedLock(reason, false); };
			handlers.releaseBleedTruceSession = [&]() { TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::Generic); };
			handlers.releaseNoSpeakerTameSession = [&](const char* reason) { TFD::Tame::ReleaseBleedNoSpeakerTameSession(reason); };
			handlers.clearBridgeAliases = [&](const char* reason) { TFD::DefeatBridge::ClearBleedSupportAliases(reason); };
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
				g_bleedBattleObservePendingEmptyAllyTicks = 0;
				g_bleedBattleObserveActive = false;
				g_bleedBattleObserveSince = {};
				g_bleedBattleObserveLastRedirect = {};
				g_bleedBattleObserveActiveEmptyEnemyTicks = 0;
				g_bleedBattleObserveActiveEmptyAllyTicks = 0;
				};
			handlers.clearEscapeBreakState = [&]() { TFD::Captive::ClearEscapeBreakRebleed(); };
			handlers.clearLastEnemyTargetingPlayer = [&]() { ClearLastEnemyTargetingPlayerInternal(); };
			handlers.clearOutcomeWindow = [&](const char* reason) { TFD::Bleedout::ClearSystemEventOutcomeWindow(reason); };
			handlers.resetPleasureRuntime = [&](const char* reason) { TFD::PleasureRuntime::ResetRuntime(reason); };

			TFD::Bleedout::ResetRuntimeState(preserveCaptive, "reset_bleed_runtime", handlers);
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
					[&](const char* why) { TFD::DefeatBridge::ClearBleedSupportAliases(why); },
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
						g_bleedBattleObservePendingEmptyAllyTicks = 0;
						g_bleedBattleObserveActive = false;
						g_bleedBattleObserveSince = {};
						g_bleedBattleObserveLastRedirect = {};
						g_bleedBattleObserveActiveEmptyEnemyTicks = 0;
						g_bleedBattleObserveActiveEmptyAllyTicks = 0;
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
		static void ClearBleedStickyReopenGrace(const char* reason);
		static void ArmBleedStickyReopenGrace(std::uint32_t speakerID, float distance, std::chrono::steady_clock::time_point now, const char* reason);
		static bool TickBleedStickyReopenGrace(RE::Actor* player, std::chrono::steady_clock::time_point now);
		static void UpdatePreCombatState();
		static bool IsActorCloseAndFront(RE::Actor* actor, RE::Actor* player, float maxDist);
		static bool CanUseAggressorForBleedoutGreet(RE::Actor* player, RE::Actor* aggressor, float& outDistance);
		static void PreparePlayerForBleedoutPleasureScene(const char* reason);
		static void PreparePlayerForCaptivePleasureScene(const char* reason);
		static void StartBleedWindow(RE::Actor* player, RE::Actor* aggressor);
		static bool HandlePendingEscapeBreakBleed();
		static void MaintainBleedSpeakerKick();
		static void ClearBleedDialogueOutcome(const char* reason);
		static bool IsDialogueOpen();
		static void ApplyCalmBubble(float radius);
		static void ClearCaptiveOrchestrationResidue(bool clearPendingFadeIn = true);
		static RE::Actor* ResolveBleedRuntimeSpeaker();
		static void BeginBleedPleasureRuntime(RE::Actor* speaker, bool captive, const char* reason);
		static void ExitBleedSystemEventRuntime();
		static TFD::Bleedout::PendingSystemEventHandlers BuildBaseBleedPendingSystemEventHandlers();
		static TFD::Bleedout::DialogueCloseHandlers BuildBleedDialogueCloseHandlers();
		static TFD::Bleedout::TimeoutHandlers BuildBleedTimeoutHandlers();
		static TFD::Bleedout::CompletionHandlers BuildCaptivePleasureCompletionHandlers();
		static TFD::Bleedout::CompletionHandlers BuildBleedPleasureCompletionHandlers();
		static TFD::Bleedout::BlackoutHandlers BuildBleedBlackoutHandlers();

		static void StartBleedWindow(RE::Actor* player, RE::Actor* aggressor)
		{
			ClearBleedStickyReopenGrace("start_bleed_window");
			TFD::Bleedout::RuntimeHost::StartWindow(player, aggressor);
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

		static bool IsObserverAlly(RE::Actor* actor)
		{
			return TFD::Bleedout::DefeatGlue::IsObserverAlly(actor);
		}

		static bool IsValidBleedBattleEnemyRosterActor(RE::Actor* actor, RE::Actor* player)
		{
			if (!player || !actor || actor == player) {
				return false;
			}
			if (actor->IsPlayerTeammate() ||
				TFD::TeammateManager::IsActiveFollowerActor(actor) ||
				TFD::TeammateManager::IsPlayerSideTeammateActor(actor) ||
				TFD::Tame::IsCompanion(actor)) {
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
			return TFD::Actor::FindBestAggressor(radius, Player());
		}

		static RE::Actor* ResolveAggressor()
		{
			const float radius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 400.0f);
			return TFD::Actor::ResolveAggressor(radius, Player());
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
			if (!actor || !player || !IsCaptiveSupportedAggressor(actor) || !IsBleedSpaceCompatible(actor, player)) {
				return false;
			}
			float dist = -1.0f;
			if (!IsReasonableCombatAggressor(actor, player, (std::max)(12000.0f, maxDist), &dist)) {
				return false;
			}
			if (outDistance) {
				*outDistance = dist;
			}
			return true;
		}

		static bool CanUseAggressorForBleedoutGreet(RE::Actor* player, RE::Actor* aggressor, float& outDistance)
		{
			outDistance = -1.0f;
			return IsReasonableBleedoutSpeaker(aggressor, player, 12000.0f, &outDistance);
		}

		static void PreparePlayerForBleedoutPleasureScene(const char* reason)
		{
			TFD::Bleedout::DefeatGlue::PreparePlayerForBleedoutPleasureScene(reason);
		}

		static void PreparePlayerForCaptivePleasureScene(const char* reason)
		{
			TFD::Bleedout::DefeatGlue::PreparePlayerForCaptivePleasureScene(reason);
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

		static TFD::Bleedout::CompletionHandlers BuildCaptivePleasureCompletionHandlers()
		{
			return TFD::Bleedout::Builders::BuildCaptivePleasureCompletionHandlers();
		}

		static TFD::Bleedout::CompletionHandlers BuildBleedPleasureCompletionHandlers()
		{
			return TFD::Bleedout::Builders::BuildBleedPleasureCompletionHandlers();
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
			return TFD::Actor::GetCurrentTarget(actor);
		}

		static bool IsPlayerSideActorForRouter(RE::Actor* actor, RE::Actor* player)
		{
			if (!actor || !player) {
				return false;
			}
			if (actor == player) {
				return true;
			}
			return IsActiveFollowerActor(actor) ||
				TFD::TeammateManager::IsPlayerSideTeammateActor(actor) ||
				TFD::Tame::IsCompanion(actor);
		}

		static bool IsActorActivelyTargetingPlayerSideForRouter(RE::Actor* actor, RE::Actor* player)
		{
			if (!actor || !player) {
				return false;
			}
			auto* currentTarget = ResolveCurrentCombatTarget(actor);
			return IsPlayerSideActorForRouter(currentTarget, player);
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

		struct FollowerResolution
		{
			RE::Actor* standing{ nullptr };
			RE::Actor* downed{ nullptr };
		};

		struct PlayerThresholdOutcomeScan
		{
			float scanRadius{ 0.0f };
			RE::Actor* initialAggressor{ nullptr };
			TFD::Actor::Snapshot coalitionSnapshot{};
			std::vector<RE::Actor*> standingFollowers{};
			bool unresolvedBattle{ false };
			bool playerSideStanding{ false };
			bool hostileCoalitionStanding{ false };
		};

		struct PlayerThresholdOutcomeClassification
		{
			RE::Actor* rememberedAggressor{ nullptr };
			bool observeUnresolvedBattle{ false };
			bool observeStandingFollowers{ false };
		};

		static void RememberAggressorForOutcome(RE::Actor* actor)
		{
			if (actor && !IsObserverAlly(actor)) {
				g_lastAggressor = actor->GetHandle();
			}
		}

		static PlayerThresholdOutcomeScan ScanPlayerThresholdOutcome(RE::Actor* player)
		{
			(void)player;
			PlayerThresholdOutcomeScan scan{};
			scan.scanRadius = (std::max)(12000.0f, TFD::Settings::GetSweepRadius());
			scan.initialAggressor = ResolveAggressor();
			if (!scan.initialAggressor) {
				scan.initialAggressor = FindBestAggressor(scan.scanRadius);
			}
			scan.coalitionSnapshot = TFD::Actor::BuildSnapshot(scan.scanRadius, false);
			scan.unresolvedBattle = !TFD::Actor::IsConflictResolved(scan.coalitionSnapshot);
			scan.playerSideStanding = TFD::Actor::HasStandingTeammateOnPlayerSide(scan.coalitionSnapshot);
			scan.hostileCoalitionStanding = TFD::Actor::HasStandingHostileCoalition(scan.coalitionSnapshot);
			scan.standingFollowers = CollectBleedStandingFollowers(scan.scanRadius);
			return scan;
		}

		static PlayerThresholdOutcomeClassification ClassifyPlayerThresholdOutcome(const PlayerThresholdOutcomeScan& scan)
		{
			PlayerThresholdOutcomeClassification classification{};
			classification.rememberedAggressor =
				(scan.initialAggressor && !IsObserverAlly(scan.initialAggressor)) ? scan.initialAggressor : nullptr;
			classification.observeUnresolvedBattle = scan.unresolvedBattle && scan.hostileCoalitionStanding;
			classification.observeStandingFollowers = !scan.standingFollowers.empty();
			return classification;
		}

		static bool DispatchPlayerThresholdOutcome(
			RE::Actor* player,
			const PlayerThresholdOutcomeScan& scan,
			const PlayerThresholdOutcomeClassification& classification)
		{
			RememberAggressorForOutcome(classification.rememberedAggressor);

			if (!classification.observeStandingFollowers) {
				auto* immediateSpeaker = FindBestBleedoutSpeaker(scan.scanRadius, 12000.0f, classification.rememberedAggressor);
				if (immediateSpeaker) {
					RememberAggressorForOutcome(immediateSpeaker);
					spdlog::info("[TFD][Defeat][R100A] player threshold -> immediate bleedout forcegreet speaker={:08X}",
						immediateSpeaker->GetFormID());
					StartBleedWindow(player, immediateSpeaker);
					SetGraceSeconds(1);
					return true;
				}
			}

			if (classification.observeUnresolvedBattle) {
				spdlog::info("[TFD][Defeat] delay outcome unresolved battle coalitions={} playerSideStanding={}",
					scan.coalitionSnapshot.activeCoalitionCount,
					scan.playerSideStanding ? 1 : 0);
				if (StartBleedBattleObservePending(player)) {
					SetGraceSeconds(1);
					return true;
				}
			}

			if (classification.observeStandingFollowers) {
				if (StartBleedBattleObservePending(player)) {
					SetGraceSeconds(1);
					return true;
				}
			}

			auto* speaker = FindBestBleedoutSpeaker(scan.scanRadius, 12000.0f, classification.rememberedAggressor);
			RememberAggressorForOutcome(speaker);
			if (!speaker) {
				spdlog::info("[TFD][Defeat] no dialogue-capable aggressor and no standing follower -> bleed countdown without speaker");
			}
			StartBleedWindow(player, speaker);
			SetGraceSeconds(1);
			return true;
		}

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

		static std::vector<RE::Actor*> FilterTemporarilySuppressedEnemiesForDialogueGlobals(
			const std::vector<RE::Actor*>& enemies,
			bool* onlySuppressedEnemies,
			std::size_t* suppressedCount)
		{
			std::vector<RE::Actor*> filtered;
			filtered.reserve(enemies.size());

			std::size_t localSuppressedCount = 0;
			for (auto* enemy : enemies) {
				if (!enemy) {
					continue;
				}
				if (TFD::HostilityController::IsActorTemporarilySuppressed(enemy)) {
					++localSuppressedCount;
					continue;
				}
				filtered.push_back(enemy);
			}

			if (onlySuppressedEnemies) {
				*onlySuppressedEnemies = !enemies.empty() && filtered.empty() && localSuppressedCount > 0;
			}
			if (suppressedCount) {
				*suppressedCount = localSuppressedCount;
			}
			return filtered;
		}

		static void RefreshPostDefeatGlobals()
		{
			auto* player = Player();
			const float radius = (std::max)(2200.0f, TFD::Settings::GetSweepRadius() + 200.0f);
			auto followers = CollectBleedStandingFollowers(radius);
			auto observedEnemies = CollectLiveStandingObservedEnemies(player, radius, nullptr, followers);
			bool onlySuppressedDialogueEnemies = false;
			std::size_t suppressedEnemyCount = 0;
			auto enemies = FilterTemporarilySuppressedEnemiesForDialogueGlobals(
				observedEnemies,
				&onlySuppressedDialogueEnemies,
				&suppressedEnemyCount);
			const bool defeatContext = IsDefeatCombatContextActive(player, enemies);
			const bool victoryContext = IsVictoryCombatContextActive(player, enemies);
			const bool routerCombatContext = IsRouterCombatContextActive(player, enemies);
			const bool pleasurePassiveLock = TFD::PleasureRuntime::IsPassiveLockActive();
			const bool battleObserveHold = g_bleedBattleObservePending || g_bleedBattleObserveActive;
			auto refreshResult = TFD::PostDefeatState::Refresh(TFD::PostDefeatState::RefreshInput{
				.player = player,
				.enemies = std::move(enemies),
				.defeatContext = defeatContext,
				.victoryContext = victoryContext,
				.routerCombatContext = routerCombatContext,
				.onlySuppressedDialogueEnemies = onlySuppressedDialogueEnemies,
				.suppressedEnemyCount = suppressedEnemyCount,
				.pleasurePassiveLock = pleasurePassiveLock,
				.battleObserveHold = battleObserveHold
				});
			g_lastRouterCombatContextActive = refreshResult.routerCombatContextActive;
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

		static TFD::Captive::RuntimeTickHandlers BuildCaptiveRuntimeTickHandlers()
		{
			TFD::Captive::RuntimeTickHandlers handlers{};
			handlers.isDialogueOpen = []() -> bool { return IsDialogueOpen(); };
			handlers.getPrevDialogueOpen = []() -> bool { return g_prevDialogueOpen; };
			handlers.setPrevDialogueOpen = [](bool open) { g_prevDialogueOpen = open; };
			handlers.escape = BuildCaptiveEscapeTickHandlers();
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
			auto pctToUnit = [](float v) { return v > 1.0f ? (v / 100.0f) : v; };
			const float thresh = std::clamp(pctToUnit(thresholdPct), 0.05f, 0.95f);
			const float bonus = std::clamp(pctToUnit(bonusPct), 0.0f, 0.95f);
			const float minSafe = std::clamp(pctToUnit(minSafePct), 0.05f, 1.0f);
			const float maxSafe = std::clamp(pctToUnit(maxSafePct), minSafe, 1.0f);
			const float safePct = std::clamp(thresh + bonus, minSafe, maxSafe);
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

		static void TickPendingCaptiveRecaptureRecoveryPulse()
		{
			if (!g_pendingCaptiveRecaptureRecovery) {
				return;
			}
			const auto now = Now();
			if (now > g_pendingCaptiveRecaptureRecoveryUntil) {
				g_pendingCaptiveRecaptureRecovery = false;
				g_pendingCaptiveRecaptureRecoveryReason.clear();
				g_pendingCaptiveRecaptureRecoveryLastPulse = {};
				return;
			}
			if (g_pendingCaptiveRecaptureRecoveryLastPulse.time_since_epoch().count() != 0 &&
				(now - g_pendingCaptiveRecaptureRecoveryLastPulse) < std::chrono::milliseconds(350)) {
				return;
			}
			g_pendingCaptiveRecaptureRecoveryLastPulse = now;

			auto* player = Player();
			const char* why = g_pendingCaptiveRecaptureRecoveryReason.empty() ? "captive_recapture_recover_pulse" : g_pendingCaptiveRecaptureRecoveryReason.c_str();
			ForceRecoverPlayerAfterCaptiveRecapture(why);
			QueueCaptiveRecaptureBridgeCleanup(why);

			if (player && !IsActorBleedingOut(player) && GetActorHealthPct(player) > std::clamp(TFD::Settings::GetDefeatThresholdPct() + 2.0f, 2.0f, 99.0f)) {
				g_pendingCaptiveRecaptureRecovery = false;
				g_pendingCaptiveRecaptureRecoveryReason.clear();
				g_pendingCaptiveRecaptureRecoveryLastPulse = {};
				spdlog::info("[TFD][Defeat] captive recapture recovery pulse completed hpPct={:.1f}", GetActorHealthPct(player));
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

		static bool HasImmediatePlayerSideBleedThreat(RE::Actor* player, const TFD::Actor::Snapshot& snapshot, float radius)
		{
			if (!player) {
				return false;
			}
			if (player->IsInCombat()) {
				return true;
			}
			if (ResolveAggressor()) {
				return true;
			}
			if (FindBestAggressor(radius)) {
				return true;
			}
			return TFD::Actor::HasStandingHostileCoalition(snapshot);
		}

		static bool ShouldEnterBleedLock(RE::Actor* actor, float thresholdPct, bool hasRelevantThreat)
		{
			if (!actor || actor->IsDisabled() || actor->IsDead()) {
				return false;
			}
			if (GetActorHealthPct(actor) > std::clamp(thresholdPct, 2.0f, 95.0f)) {
				return false;
			}
			if (actor == Player() || IsActiveFollowerActor(actor)) {
				return hasRelevantThreat;
			}
			return hasRelevantThreat || TFD::Actor::Ops::IsDefeatedEnemyCandidate(actor);
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

			const float radius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 600.0f);
			auto snapshot = TFD::Actor::BuildSnapshot(radius, false);
			const bool playerSideThreat = HasImmediatePlayerSideBleedThreat(player, snapshot, radius);

			const float playerThreshold = ResolveBleedLockThresholdPct(player);
			if (ShouldEnterBleedLock(player, playerThreshold, playerSideThreat)) {
				EnterBleedLock(player, BleedLockKind::Player, playerThreshold, "threshold_scan_player");
			}
			else if (GetActorHealthPct(player) <= std::clamp(playerThreshold, 2.0f, 95.0f) && !playerSideThreat) {
				spdlog::info("[TFD][Defeat] skip bleed lock player reason=no_immediate_threat hpPct={:.1f} threshold={:.1f}",
					GetActorHealthPct(player),
					std::clamp(playerThreshold, 2.0f, 95.0f));
			}

			for (auto* actor : TFD::TeammateManager::CollectKnownTeammates(radius)) {
				const float threshold = ResolveBleedLockThresholdPct(actor);
				const bool followerThreat = playerSideThreat || (actor && actor->IsInCombat());
				if (ShouldEnterBleedLock(actor, threshold, followerThreat)) {
					EnterBleedLock(actor, ResolveBleedLockKind(actor), threshold, "threshold_scan_follower");
				}
			}

			for (const auto& info : snapshot.actors) {
				auto* actor = info.get();
				if (!actor || actor == player || actor->IsDisabled() || actor->IsDead()) {
					continue;
				}

				// R54: player-side teammates/followers must never fall through the generic
				// defeated-enemy path. If a converted teammate is downed, keep it as Ally
				// bleed lock so manual recovery dialogue can win over Victory activation.
				if (TFD::TeammateManager::IsPlayerSideTeammateActor(actor)) {
					const float threshold = TFD::Settings::GetAllyDownedThresholdPct();
					const bool followerThreat = playerSideThreat || (actor && actor->IsInCombat());
					if (ShouldEnterBleedLock(actor, threshold, followerThreat)) {
						EnterBleedLock(actor, BleedLockKind::Ally, threshold, "threshold_scan_follower_loose");
					}
					continue;
				}

				if (IsActiveFollowerActor(actor)) {
					continue;
				}
				const float threshold = ResolveBleedLockThresholdPct(actor);
				const bool enemyThreat = TFD::Actor::Ops::IsDefeatedEnemyCandidate(actor);
				if (ShouldEnterBleedLock(actor, threshold, enemyThreat)) {
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
					const bool recoveryDialogueHold = TFD::TeammateManager::IsDownedTeammateRecoveryDialogueHoldActor(actor);
					const float hpPctBeforeClamp = GetActorHealthPct(actor);
					if (!recoveryDialogueHold && hpPctBeforeClamp > (entry.thresholdPct + 8.0f)) {
						releases.emplace_back(formID, actor, "ally_recovered", true);
						continue;
					}

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
					if (!recoveryDialogueHold) {
						actor->EvaluatePackage(false, true);
						actor->EvaluatePackage(true, true);
					}

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
					if (entry.defeatedManaged && entry.defeatedDeadline.time_since_epoch().count() != 0 && now >= entry.defeatedDeadline) {
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
				const bool allyRecoveryDialogueHold =
					entry.kind == BleedLockKind::Ally && TFD::TeammateManager::IsDownedTeammateRecoveryDialogueHoldActor(actor);
				const auto pulseInterval = entry.kind == BleedLockKind::Ally ? std::chrono::milliseconds(250) : std::chrono::milliseconds(250);
				const bool pulseDue = entry.lastPulse.time_since_epoch().count() == 0 || (now - entry.lastPulse) >= pulseInterval;
				const bool shouldPulse = [&]() {
					if (suppressBleedPulse) {
						return false;
					}
					if (entry.kind == BleedLockKind::Ally) {
						return !allyRecoveryDialogueHold && pulseDue;
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

		static void ClearBleedStickyReopenGrace(const char* reason)
		{
			if (!g_bleedStickyReopenGraceActive) {
				return;
			}
			spdlog::info("[TFD][Defeat][R113] bleed sticky reopen grace cleared reason={} speaker={:08X}",
				reason ? reason : "unknown",
				g_bleedStickyReopenGraceSpeakerID);
			g_bleedStickyReopenGraceActive = false;
			g_bleedStickyReopenGraceUntil = {};
			g_bleedStickyReopenGraceSpeakerID = 0;
			g_bleedStickyReopenGraceDistance = 99999.0f;
		}

		static void ArmBleedStickyReopenGrace(std::uint32_t speakerID, float distance, std::chrono::steady_clock::time_point now, const char* reason)
		{
			if (speakerID == 0) {
				ClearBleedStickyReopenGrace("arm_no_speaker");
				return;
			}
			const bool captiveEscapeBleedout = TFD::Captive::IsEscapeBleedoutActive() ||
				TFD::Captive::HasEscapeBreakRebleedPending() ||
				TFD::Captive::IsRecaptureCommitActive();
			const auto delayMs = captiveEscapeBleedout ? 2200 : 650;

			g_bleedStickyReopenGraceActive = true;
			g_bleedStickyReopenGraceUntil = now + std::chrono::milliseconds(delayMs);
			g_bleedStickyReopenGraceSpeakerID = speakerID;
			g_bleedStickyReopenGraceDistance = distance;
			TFD::BleedoutGreet::MarkStickyReopenPending(false, "dialogue_closed_sticky_reopen_grace");
			spdlog::info("[TFD][Defeat][R113] bleed sticky reopen delayed speaker={:08X} dist={:.1f} delayMs={} captiveEscapeBleedout={} reason={}",
				speakerID,
				distance,
				delayMs,
				captiveEscapeBleedout ? 1 : 0,
				reason ? reason : "unknown");
		}

		static bool TickBleedStickyReopenGrace(RE::Actor* player, std::chrono::steady_clock::time_point now)
		{
			if (!g_bleedStickyReopenGraceActive) {
				return false;
			}

			const auto outcome = TFD::Bleedout::GetDialogueOutcome();
			if (TFD::Bleedout::HasTerminalCommit() || outcome != BleedDialogueOutcome::None || TFD::PleasureRuntime::IsBlocking() || TFD::PleasureRuntime::IsActive()) {
				ClearBleedStickyReopenGrace("terminal_or_runtime_claimed");
				return true;
			}

			if (IsDialogueOpen()) {
				ClearBleedStickyReopenGrace("dialogue_reopened_elsewhere");
				return false;
			}

			if (now < g_bleedStickyReopenGraceUntil) {
				g_bleedPaused = true;
				g_bleedPauseStarted = now;
				g_bleedLastSeconds = -1;
				return true;
			}

			const auto speakerID = g_bleedStickyReopenGraceSpeakerID;
			ClearBleedStickyReopenGrace("grace_elapsed");
			if (speakerID == 0 || !player) {
				return false;
			}

			auto* reopenSpeaker = RE::TESForm::LookupByID<RE::Actor>(speakerID);
			float reopenDist = 99999.0f;
			if (!(reopenSpeaker && CanUseAggressorForBleedoutGreet(player, reopenSpeaker, reopenDist))) {
				spdlog::info("[TFD][Defeat][R113] bleed sticky reopen delayed skip speaker={:08X} valid=0", speakerID);
				return false;
			}

			ResetBleedSpeakerKickState();
			g_bleedLastSeconds = -1;
			TFD::Bleedout::ClearSystemEventOutcomeWindow("dialogue_closed_sticky_reopen_grace_elapsed");
			ApplyBleedDialogueOverdrive(player, reopenSpeaker, "dialogue_closed_sticky_reopen_grace_elapsed", true);
			g_bleedPaused = true;
			g_bleedPauseStarted = now;
			spdlog::info("[TFD][Defeat][R113] bleed sticky reopen grace elapsed -> reopen speaker={:08X} dist={:.1f}",
				speakerID,
				reopenDist);
			return true;
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
			if (TFD::Transition::IsAwaiting() || TFD::Transition::HasPendingFadeIn()) {
				TFD::Transition::MaintainCalmWindow(BuildTransitionRuntimeHandlers());
				UpdatePreCombatState();
				return;
			}
			RefreshPostDefeatGlobals();
			const bool inCombatDialogueOpen = IsDialogueOpen();
			if (!inCombatDialogueOpen &&
				!TFD::InteractionRouter::DialogueOpen::IsActive() &&
				TFD::Bleedout::TickQueuedAfterPleasureCrowdContinuation()) {
				return;
			}
			TickPendingCaptiveRecaptureRecoveryPulse();
			const bool captiveBleedOverlay = TFD::Captive::HasEscapeBreakRebleedPending() || g_inBleedState.load(std::memory_order_acquire);
			if (!captiveBleedOverlay && TFD::InCombat::IsActive() && inCombatDialogueOpen) {
				TFD::InCombatGreet::NotifyDialogueOpened();
			}
			if (!captiveBleedOverlay && TFD::Rescue::IsActive() && inCombatDialogueOpen) {
				TFD::RescueGreet::NotifyDialogueOpened();
			}

			// R127: Dialogue Menu pauses the game. Bleedout AfterPleasure must observe
			// the open state before the pause gate, or the close transition is missed
			// and the native forcegreet only flashes.
			const auto bleedAfterPleasurePauseSnapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
			if (!captiveBleedOverlay &&
				bleedAfterPleasurePauseSnapshot.sub == TFD::FlowController::SubFlow::BleedoutAfterPleasure &&
				inCombatDialogueOpen) {
				if (!g_prevDialogueOpen) {
					auto* afterSpeaker = TFD::PleasureRuntime::GetPrimarySpeaker();
					spdlog::info(
						"[TFD][Defeat][R127] bleed after pleasure dialogue observed before pause gate speaker={:08X}",
						afterSpeaker ? afterSpeaker->GetFormID() : 0u);
				}
				TFD::BleedoutGreet::NotifyDialogueOpened();
				g_prevDialogueOpen = true;
			}

			if (!TFD::Captive::TickRuntime(Player(), captiveBleedOverlay, BuildCaptiveRuntimeTickHandlers())) {
				return;
			}

			// R128: Bleedout AfterPleasure has to own the full open->close edge
			// before the pause gate. R127 only observed the open edge before
			// GameIsPaused(), then the close/no-commit edge could be missed until
			// the player manually activated the speaker. Service the dialogue-open
			// request here and reopen with the Pleasure/flow primary speaker.
			const auto bleedAfterPleasurePrePauseSnapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
			if (!captiveBleedOverlay &&
				bleedAfterPleasurePrePauseSnapshot.sub == TFD::FlowController::SubFlow::BleedoutAfterPleasure) {
				TFD::InteractionRouter::DialogueOpen::Tick();
				const bool dOpen = IsDialogueOpen();
				const auto nowBleedAfter = Now();

				RE::Actor* afterSpeaker = TFD::PleasureRuntime::GetPrimarySpeaker();
				std::uint32_t afterSpeakerID = afterSpeaker ? afterSpeaker->GetFormID() : bleedAfterPleasurePrePauseSnapshot.primaryActorFormID;
				if (!afterSpeaker && afterSpeakerID != 0) {
					afterSpeaker = RE::TESForm::LookupByID<RE::Actor>(afterSpeakerID);
				}
				if (!afterSpeaker) {
					afterSpeaker = CurrentBleedSpeaker();
					afterSpeakerID = afterSpeaker ? afterSpeaker->GetFormID() : CurrentBleedSpeakerID();
				}

				if (dOpen) {
					TFD::BleedoutGreet::NotifyDialogueOpened();
					g_prevDialogueOpen = true;
					return;
				}

				if (TFD::BleedoutGreet::TryAfterPleasureWatchdog(g_prevDialogueOpen, dOpen, afterSpeakerID, nowBleedAfter,
					[&](const char* reopenReason) -> bool {
						RE::Actor* reopenSpeaker = afterSpeaker;
						if (!reopenSpeaker && afterSpeakerID != 0) {
							reopenSpeaker = RE::TESForm::LookupByID<RE::Actor>(afterSpeakerID);
						}
						if (!reopenSpeaker || reopenSpeaker->IsDead() || reopenSpeaker->IsDisabled()) {
							return false;
						}
						const bool reopened = TFD::BleedoutGreet::BeginAfterPleasure(reopenSpeaker, reopenReason);
						spdlog::info("[TFD][Defeat][R128] bleed after pleasure pre-pause close watchdog native reopen speaker={:08X} ok={}",
							reopenSpeaker->GetFormID(),
							reopened ? 1 : 0);
						return reopened;
					})) {
					g_prevDialogueOpen = false;
					return;
				}

				g_prevDialogueOpen = false;
				return;
			}

			if (ui && ui->GameIsPaused()) return;
			auto* player = Player();
			if (!player) {
				RefreshPostDefeatGlobals();
				return;
			}
			TFD::FlowController::TickRuntime();
			TFD::Location::UpdateAmbientKidnapAvailability(false);
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
				if (TickBleedStickyReopenGrace(player, Now())) {
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
				// R130: no destructive Bleedout forcegreet timeout/rearm.
				// Long-distance or stuck speakers should be handled by approach assist / MoveTo,
				// not by cancelling and restarting the dialogue lifecycle.
				if (g_bleedPaused) {
					g_bleedStart += (Now() - g_bleedPauseStarted);
					g_bleedPaused = false;
					g_bleedPauseStarted = {};
					g_bleedLastSeconds = -1;
					spdlog::info("[TFD][Defeat] bleed countdown resumed after dialogue");
				}

				{
					const auto nowBleed = Now();
					if (TFD::BleedoutGreet::TryInitialHandoffWatchdog(TFD::Bleedout::HasTerminalCommit(), IsDialogueOpen(), TFD::PleasureRuntime::IsBlocking(), CurrentBleedSpeakerID(), nowBleed,
						[&](const char* reopenReason) -> bool {
							auto* reopenSpeaker = CurrentBleedSpeaker();
							float reopenDist = 99999.0f;
							if (!(player && reopenSpeaker && CanUseAggressorForBleedoutGreet(player, reopenSpeaker, reopenDist))) {
								return false;
							}
							ApplyBleedDialogueOverdrive(player, reopenSpeaker, reopenReason, true);
							g_bleedPaused = true;
							g_bleedPauseStarted = nowBleed;
							g_bleedLastSeconds = -1;
							spdlog::info("[TFD][Defeat][CB06B] bleed initial handoff retry speaker={:08X} dist={:.1f}",
								reopenSpeaker->GetFormID(),
								reopenDist);
							return true;
						})) {
						return;
					}
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
							if (TFD::Captive::IsEscapeBleedoutActive() || TFD::Captive::IsRecaptureCommitActive() || TFD::Captive::IsRecaptureRecentlyCommitted()) {
								spdlog::info("[TFD][Defeat] bleedout dialogue closed after captive outcome -> captive recapture FSM");
								if (TFD::Captive::CommitRecapture(CurrentBleedSpeaker(), "dialogue_closed_captive_commit")) {
									ResetBleedRuntimeState();
								}
								g_bleedLastSeconds = -1;
								return;
							}
							if (TFD::Transition::ResolveCaptiveMarkerForOutcome(BuildTransitionRuntimeHandlers())) {
								spdlog::info("[TFD][Defeat] bleedout dialogue closed after captive outcome -> captive marker found");
								const bool teleported = TFD::Bleedout::DoBlackoutTeleport("blackout_teleport", BuildBleedBlackoutHandlers());
								if (teleported) {
									ArmCaptiveRecaptureRecoveryPulse("dialogue_closed_captive_blackout");
								}
								SetGraceSeconds(4);
							}
 else {
  spdlog::info("[TFD][Defeat] bleedout dialogue closed after captive outcome -> no marker -> resolve fallback");
  TFD::Tame::ReleaseBleedNoSpeakerTameSession("dialogue_closed_captive_no_marker");
  g_inBleedState.store(false, std::memory_order_release);
  g_minHp = 0.0f;
  TFD::BleedoutGreet::ResetRuntime("bleed_reset");
  g_bleedLastSeconds = -1;
  (void)TFD::Bleedout::EnterNonCaptiveChoice("dialogue_closed_captive_no_marker", TFD::Bleedout::Builders::BuildNonCaptiveChoiceHandlers());
}
},
[&](const TFD::BleedoutGreet::StickyReopenProbe& probe) {
	g_prevDialogueOpen = false;
	g_bleedLastSeconds = -1;
	const auto nowReopenGrace = Now();
	g_bleedPaused = true;
	g_bleedPauseStarted = nowReopenGrace;
	ArmBleedStickyReopenGrace(probe.speakerFormID, probe.distance, nowReopenGrace, "dialogue_closed_sticky_reopen");
	spdlog::info("[TFD][Defeat][R113] bleedout dialogue closed without committed outcome -> sticky reopen delayed speaker={:08X} dist={:.1f}",
		probe.speakerFormID,
		probe.distance);
},
[&](const TFD::BleedoutGreet::StickyReopenProbe& probe) {
	g_prevDialogueOpen = false;
	ClearBleedStickyReopenGrace("sticky_reopen_unavailable");
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
					const bool suppressCaptiveRecaptureNotice = TFD::Captive::IsEscapeBleedoutActive() || TFD::Captive::IsRecaptureCommitActive() || TFD::Captive::IsRecaptureRecentlyCommitted();
					if (remain > 0 && !suppressCaptiveRecaptureNotice) {
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
				auto thresholdScan = ScanPlayerThresholdOutcome(player);
				const bool immediateThreat = player->IsInCombat() || thresholdScan.initialAggressor != nullptr || thresholdScan.hostileCoalitionStanding;
				if (!immediateThreat) {
					spdlog::info("[TFD][Defeat] skip player threshold outcome reason=no_immediate_threat hpPct={:.1f} threshold={:.1f}", pct, thresh);
					return;
				}
				EnterBleedLock(player, BleedLockKind::Player, thresh, "player_threshold");
				ClearEnemyTargetsToPlayerForDefeat(player, thresholdScan.scanRadius, "player_threshold");
				auto thresholdClassification = ClassifyPlayerThresholdOutcome(thresholdScan);
				if (DispatchPlayerThresholdOutcome(player, thresholdScan, thresholdClassification)) {
					return;
				}
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

				if (std::strcmp(rawName, kBleedoutGreetConfirmedEvent) == 0) {
					auto* speaker = ev->sender ? ev->sender->As<RE::Actor>() : nullptr;
					TFD::BleedoutGreet::NotifyFlowGreetConfirmed(speaker, ev->strArg.c_str());
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

	static TFD::DefeatRuntimeProviders::InstallHooks BuildDefeatRuntimeProviderHooks()
	{
		TFD::DefeatRuntimeProviders::InstallHooks hooks{};
		hooks.teammateHasAllyBleedLock = [](RE::Actor* actor) {
			auto it = g_bleedLocks.find(actor ? actor->GetFormID() : 0u);
			return actor && it != g_bleedLocks.end() && it->second.kind == BleedLockKind::Ally;
			};
		hooks.teammateIsBleedingOutActor = [](RE::Actor* actor) { return IsActorBleedingOut(actor); };
		hooks.teammateIsDialogueCapableDefeatedEnemy = [](RE::Actor* actor) { return TFD::Actor::Ops::IsDialogueCapableDefeatedEnemy(actor); };
		hooks.teammateGetDefeatedEnemyRemainingSeconds = [](RE::Actor* actor) { return TFD::Actor::Ops::GetDefeatedEnemyRemainingSeconds(actor); };
		hooks.teammateSuppressDefeatedReentry = [](RE::Actor* actor, double seconds, const char* reason) { TFD::Actor::Ops::SuppressDefeatedEnemyReentry(actor, seconds, reason); };
		hooks.teammateReleaseBleedLock = [](RE::Actor* actor, const char* reason, bool playGetUp) { ReleaseBleedLock(actor, reason, playGetUp); };
		hooks.teammateRestoreActorHealthToSafePct = [](RE::Actor* actor, float thresholdPct, float bonusPct, float minSafePct, float maxSafePct, float minAbsHp, const char* reason) { RestoreActorHealthToSafePct(actor, thresholdPct, bonusPct, minSafePct, maxSafePct, minAbsHp, reason); };
		hooks.teammateResolvePendingDefeatedDialogueTarget = []() -> RE::Actor* { return ResolvePendingDefeatedDialogueTargetInternal(); };
		hooks.teammateClearPendingDefeatedDialogueTarget = []() { ClearPendingDefeatedDialogueTargetInternal(); };
		hooks.teammateSetPendingDefeatedDialogueTarget = [](RE::Actor* actor) { SetPendingDefeatedDialogueTargetInternal(actor); };
		hooks.teammateReviveDownedAlly = [](RE::Actor* actor, float targetHealthPct) { return TFD::TeammateManager::ReviveDownedAlly(actor, targetHealthPct); };
		hooks.hostilityStartBleedTruceSessionForSpeaker = [](RE::Actor* player, RE::Actor* speaker, const char* reason) {
			return TFD::Bleedout::StartTruceSessionForSpeaker(player, speaker, reason, TFD::Bleedout::RuntimeHost::BuildStateRefs(), TFD::Bleedout::RuntimeHost::BuildHandlers());
			};
		hooks.hostilityReleaseBleedTruceSession = [](TFD::Tame::ReleaseReason reason) { TFD::Bleedout::ReleaseTruceSession(reason); };
		hooks.tameIsCreatureDefeatedEnemy = [](RE::Actor* actor) { return TFD::Actor::Ops::IsCreatureDefeatedEnemy(actor); };
		hooks.tameGetDefeatedEnemyRemainingSeconds = [](RE::Actor* actor) { return TFD::Actor::Ops::GetDefeatedEnemyRemainingSeconds(actor); };
		hooks.tameSuppressDefeatedReentry = [](RE::Actor* actor, double seconds, const char* reason) { TFD::Actor::Ops::SuppressDefeatedEnemyReentry(actor, seconds, reason); };
		hooks.tameReleaseBleedLock = [](RE::Actor* actor, const char* reason, bool playGetUp) { ReleaseBleedLock(actor, reason, playGetUp); };
		hooks.tameRestoreActorHealthToSafePct = [](RE::Actor* actor, float thresholdPct, float bonusPct, float minSafePct, float maxSafePct, float minAbsHp, const char* reason) { RestoreActorHealthToSafePct(actor, thresholdPct, bonusPct, minSafePct, maxSafePct, minAbsHp, reason); };
		hooks.tameReleaseBleedNoSpeakerTameSession = [](const char* reason) { TFD::Bleedout::ReleaseNoSpeakerTameSession(reason); };
		hooks.tameTryEnsureBleedNoSpeakerTameSession = [](const std::vector<RE::Actor*>& actors, const char* reason) {
			auto* player = Player();
			if (!player) {
				return false;
			}
			return TFD::Bleedout::TryEnsureNoSpeakerTameSession(actors, player, reason, TFD::Bleedout::RuntimeHost::BuildHandlers());
			};
		return hooks;
	}


	static TFD::Bleedout::DefeatLifecycleProviders BuildBleedoutDefeatLifecycleProviders()
	{
		return TFD::Bleedout::DefeatLifecycleProviders{
		TFD::Bleedout::Builders::PendingSystemEventProvider{
			[](const char* r) { ClearBleedDialogueOutcome(r); },
			[](const char* r) { TFD::Bleedout::ClearSystemEventOutcomeWindow(r); },
			[](const char* r) { auto completion = TFD::Bleedout::Builders::BuildPayReleaseCompletionHandlers(); (void)TFD::Bleedout::CompletePayRelease(r, completion); },
			[]() { TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::FlowHandoff); },
			[]() -> bool { return TFD::Transition::ResolveCaptiveMarkerForOutcome(TFD::Transition::DefeatGlue::BuildTransitionRuntimeHandlers()); },
			[]() {
				const bool teleported = TFD::Bleedout::DoBlackoutTeleport("blackout_teleport", TFD::Bleedout::Builders::BuildBlackoutHandlers());
				if (teleported) {
					ArmCaptiveRecaptureRecoveryPulse("pending_system_event_blackout");
				}
			},
			[](int seconds) { SetGraceSeconds(seconds); },
			[](const char* r) { TFD::Tame::ReleaseBleedNoSpeakerTameSession(r); },
			[]() { ExitBleedSystemEventRuntime(); },
			[](const char* r) { (void)TFD::Bleedout::EnterNonCaptiveChoice(r, TFD::Bleedout::Builders::BuildNonCaptiveChoiceHandlers()); }
		},
		TFD::Bleedout::Builders::DialogueCloseProvider{
			[](const char* r) { ClearBleedDialogueOutcome(r); },
			[](const char* r) { auto completion = TFD::Bleedout::Builders::BuildPayReleaseCompletionHandlers(); (void)TFD::Bleedout::CompletePayRelease(r, completion); }
		},
		TFD::Bleedout::Builders::TimeoutProvider{
			[]() { TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::Generic); },
			[]() -> bool { return TFD::Transition::ResolveCaptiveMarkerForOutcome(TFD::Transition::DefeatGlue::BuildTransitionRuntimeHandlers()); },
			[]() { ResetBleedRuntimeState(); },
			[]() {
				const bool teleported = TFD::Bleedout::DoBlackoutTeleport("blackout_teleport", TFD::Bleedout::Builders::BuildBlackoutHandlers());
				if (teleported) {
					ArmCaptiveRecaptureRecoveryPulse("timeout_blackout");
				}
			},
			[](int seconds) { SetGraceSeconds(seconds); },
			[](const char* r) { (void)TFD::Bleedout::EnterNonCaptiveChoice(r, TFD::Bleedout::Builders::BuildNonCaptiveChoiceHandlers()); }
		},
		TFD::Bleedout::Builders::BaseCompletionProvider{
			[](const char* r) { TFD::Bleedout::ClearSystemEventOutcomeWindow(r); },
			[](BleedTerminalCommit kind, const char* r) { return TFD::Bleedout::TryBeginTerminalCommit(kind, r); },
			[]() { TFD::Transition::ClearPendingFadeIn(); },
			[](const char* r) { TFD::Bleedout::ClearBridgeAliases(nullptr, r); },
			[]() { TFD::Captive::ClearEscapeContext(); },
			[]() { TFD::Captive::ResetLockpickWatch(); },
			[](bool active) { g_grace.store(active, std::memory_order_release); },
			[](bool captive) { TFD::Captive::SetRuntimeState(captive, captive ? CaptivePhaseValue::Captive : CaptivePhaseValue::None); },
			[](bool immune) { SetPlayerBleedImmune(immune); },
			[]() { TFD::Transition::RecoverPlayerForTransition(TFD::Transition::DefeatGlue::BuildTransitionRuntimeHandlers()); },
			[]() { return TFD::Settings::GetSweepRadius(); },
			[](float radius) { ApplyCalmBubble(radius); },
			[]() { UpdatePreCombatState(); },
			[]() -> RE::Actor* { return ResolveBleedRuntimeSpeaker(); },
			[](RE::Actor* speaker, bool captive, const char* why) { BeginBleedPleasureRuntime(speaker, captive, why); },
			[](bool v) { g_prevDialogueOpen = v; },
			[](bool v) { TFD::Captive::SetPrevLockpickOpen(v); }
		},
		TFD::Bleedout::Builders::CaptivePleasureCompletionExtras{
			[]() { g_lastAggressor.reset(); },
			[](bool preserve) { ResetBleedRuntimeState(preserve); },
			[](const char* r) { TFD::Captive::SyncPlayerAlias(Player(), r); }
		},
		TFD::Bleedout::Builders::PayReleaseCompletionExtras{
			[]() { TFD::Actor::Ops::ClearAggressorFactionContext(); },
			[]() { g_lastAggressor.reset(); },
			[](bool preserve) { if (preserve) { ResetBleedRuntimeState(true); } else { ResetBleedRuntimeState(); } },
			[](int secs) { TFD::Transition::BeginLeftForDeadCooldown(secs); },
			[](int secs) { SetGraceSeconds(secs); }
		},
		TFD::Bleedout::Builders::BleedPleasureCompletionExtras{
			[](const char* r) { TransitionBleedRuntimeToPleasureCommit(r); g_lastRouterCombatContextActive = false; },
			[](int secs) { TFD::Transition::BeginLeftForDeadCooldown(secs); },
			[](int secs) { SetGraceSeconds(secs); },
			[]() { RefreshPostDefeatGlobals(); }
		},
		TFD::Bleedout::RuntimeHost::Provider{
			&g_inBleedState,
			&g_minHp,
			&g_bleedStart,
			&g_bleedLastSeconds,
			&g_bleedPaused,
			&g_bleedPauseStarted,
			&g_bleedLastCalmPulse,
			[]() { return TFD::Captive::HasEscapeBreakRebleedPending(); },
			[](bool pending) { TFD::Captive::SetEscapeBreakRebleedPending(pending); },
			&g_bleedPendingCaptiveOutcome,
			&g_bleedPendingNonCaptiveOutcome,
			&g_bleedBattleObservePending,
			&g_bleedBattleObservePendingUntil,
			&g_bleedBattleObservePendingLastRedirect,
			&g_bleedBattleObservePendingEmptyEnemyTicks,
			&g_bleedBattleObservePendingEmptyAllyTicks,
			&g_bleedBattleObserveActive,
			&g_bleedBattleObserveSince,
			&g_bleedBattleObserveLastRedirect,
			&g_bleedBattleObserveActiveEmptyEnemyTicks,
			&g_bleedBattleObserveActiveEmptyAllyTicks,
			[]() { return BuildLocalBleedRuntimeHostHandlers(); },
			[]() { return BuildBleedDialogueHotkeyHandlers(); },
			[]() -> RE::Actor* { return Player(); },
			[]() { return (std::max)(2400.0f, TFD::Settings::GetSweepRadius()); },
			[]() { return 1800.0f; }
		},
		TFD::Bleedout::DefeatGlue::Provider{
			&g_grace,
			&g_graceUntil,
			&g_prevDialogueOpen,
			[]() { TFD::Transition::ClearPendingFadeIn(); },
			[]() { TFD::Captive::ClearEscapeContext(); },
			[]() { TFD::Captive::ResetLockpickWatch(); },
			[](bool open) { TFD::Captive::SetPrevLockpickOpen(open); },
			[]() { TFD::Captive::SetRuntimeState(false, CaptivePhaseValue::None); },
			[]() -> RE::Actor* { return Player(); },
			[](const char* reason, bool playGetUp) { ReleasePlayerBleedLock(reason, playGetUp); },
			[]() { return CurrentBleedSpeakerID(); },
			[]() -> RE::Actor* { return CurrentBleedSpeaker(); },
			[]() { return BuildBleedoutSpeakerHandlers(); },
			[]() { return IsDialogueOpen(); },
			[]() -> RE::Actor* { return ResolveAggressor(); },
			[](float radius) -> RE::Actor* { return FindBestAggressor(radius); },
			[](const char* reason) { TFD::Tame::ReleaseBleedNoSpeakerTameSession(reason); },
			[]() { TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::Generic); },
			[](RE::Actor* player, RE::Actor* speaker, const char* reason) { return TFD::HostilityController::StartBleedTruceSessionForSpeaker(player, speaker, reason); },
			[]() { ResetBleedSpeakerKickState(); },
			[](const char* reason) { TFD::BleedoutGreet::ResetRuntime(reason); },
			[](RE::Actor* actor, const char* reason) { (void)TFD::BleedoutGreet::Begin(actor, reason); },
			[](const char* reason) { TFD::DefeatBridge::ClearBleedSupportAliases(reason); },
			[]() { g_bleedBattlePreferredEnemy.reset(); g_bleedBattleObserver = {}; },
			[]() { return g_observedCombatCommitDepth > 0; },
			[](RE::Actor* actor) { NoteEnemyTargetingPlayerInternal(actor); },
			[](bool immune) { SetPlayerBleedImmune(immune); },
			[](RE::Actor* actor, float minHp) { ClampHealth(actor, minHp); },
			[](float radius) { return CollectBleedStandingFollowers(radius); },
			[](RE::Actor* player, const std::vector<RE::Actor*>& allies, float baseRadius) { return TFD::Bleedout::DefeatGlue::ComputeObservedEnemyScanRadius(player, allies, baseRadius); },
			[](float radius, double maxAgeSec) -> RE::Actor* { return ResolveLastEnemyTargetingPlayerInternal(radius, maxAgeSec); },
			[](RE::Actor* actor) { return IsObserverAlly(actor); },
			[](RE::Actor* player, float radius, RE::Actor* preferred, const std::vector<RE::Actor*>& allies) { return TFD::Bleedout::DefeatGlue::CollectCurrentObservedEnemies(player, radius, preferred, allies); },
			[](RE::Actor* player, const std::vector<RE::Actor*>& followers, const std::vector<RE::Actor*>& enemies, RE::Actor* preferred) { TFD::Bleedout::DefeatGlue::UpdateObservedBattleRoster(player, followers, enemies, preferred); },
			[]() { return CollectBleedStandingFollowersFromSnapshot(); },
			[]() { return CollectBleedStandingEnemiesFromSnapshot(); },
			[]() { return TFD::Bleedout::DefeatGlue::HadValidObservedEnemy(); },
			[]() { return ResolveBleedFlowActorFormID(); },
			[](const TFD::FlowController::ObservedDefeatInput& input, const char* reason) {
				return TFD::FlowController::ApplyObservedDefeatResolution(input, reason ? reason : "battle_observe_resolution");
			},
			[]() { EnterObservedBattleWin(); },
			[](const char* reason) { EnterObservedLeftForDead(reason); },
			[](float radius, float maxDist, RE::Actor* preferred) { return FindBestBleedoutSpeaker(radius, maxDist, preferred); },
			[](RE::Actor* actor, RE::Actor* player, float maxDist, float* outDistance) { return IsReasonableBleedoutSpeaker(actor, player, maxDist, outDistance); },
			[](float radius, RE::Actor* preferred, bool preserveAssigned) { return CollectBleedoutCrowd(radius, preferred, preserveAssigned); },
			[](RE::Actor* actor) { return IsCaptiveSupportedAggressor(actor); },
			[](RE::Actor* actor) { return TFD::Actor::Ops::ApplyAggressorFactionContext(actor); },
			[]() { return TFD::Transition::ResolveCaptiveMarkerForOutcome(BuildTransitionRuntimeHandlers()); },
			[](RE::Actor* player, RE::Actor* aggressor, bool hasCaptiveOutcome, float* outDistance) { const float maxDist = hasCaptiveOutcome ? 1200.0f : 900.0f; return player && aggressor && IsCaptiveSupportedAggressor(aggressor) && IsReasonableCombatAggressor(aggressor, player, maxDist, outDistance); },
			[](const std::vector<RE::Actor*>& actors, const char* reason) { auto* player = Player(); return player && TFD::Bleedout::TryEnsureNoSpeakerTameSession(actors, player, reason, TFD::Bleedout::RuntimeHost::BuildHandlers()); },
			[](RE::Actor* actor) { g_lastAggressor = actor ? actor->GetHandle() : RE::ActorHandle{}; },
			[](RE::Actor* actor) { return IsBleedCrowdSupportedAggressor(actor); },
			[](RE::Actor* actor, RE::Actor* player) { return IsBleedSpaceCompatible(actor, player); },
			[](const char* msg) { if (msg && msg[0]) RE::DebugNotification(msg); },
			[](RE::Actor* player, float radius, const char* reason) { ClearEnemyTargetsToPlayerForDefeat(player, radius, reason); },
			[](float radius) -> RE::Actor* { return TFD::Captive::ResolveEscapeBreakPreferredAggressor(radius, [](float fallbackRadius) { return FindBestAggressor(fallbackRadius); }); },
			[](RE::Actor* player, RE::Actor* aggressor, float* outDistance) { if (!player || !aggressor) { if (outDistance) *outDistance = -1.0f; return false; } float dist = -1.0f; const bool ok = CanUseAggressorForBleedoutGreet(player, aggressor, dist); if (outDistance) *outDistance = dist; return ok; },
			[](RE::Actor* player, RE::Actor* speaker, const char* reason, bool restartDialogue) { ApplyBleedDialogueOverdrive(player, speaker, reason, restartDialogue); },
			[](RE::Actor* actor) { return IsStandingAllyThresholdActor(actor); },
			[](RE::Actor* actor) { return IsStandingEnemyThresholdActor(actor); }
		}
		};
	}

	static TFD::FlowController::DefeatLifecycleProviders BuildFlowControllerDefeatLifecycleProviders()
	{
		return TFD::FlowController::DefeatLifecycleProviders{
		TFD::FlowController::PassiveRuntimeProviders{
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
		},
		TFD::FlowController::OutcomeRuntimeProviders{
			[](RE::Actor* actor, double seconds, const char* reason) { TFD::Actor::Ops::ApplyReleaseFollowGraceToSpeakerAndCrowd(actor, seconds, reason); },
			[](RE::Actor* actor, const char* reason) { TFD::Actor::Ops::RemoveReleaseFollowGraceFromSpeakerAndCrowd(actor, reason); },
			[]() { return ResolveBleedFlowActorFormID(); },
			[]() { return g_inBleedState.load(std::memory_order_acquire); },
			[](const char* reason) { PreparePlayerForCaptivePleasureScene(reason); },
			[](const char* reason) { auto completion = BuildCaptivePleasureCompletionHandlers(); (void)TFD::Bleedout::CompleteCaptivePleasureHandoff(reason, completion); },
			[](const char* reason) { PreparePlayerForBleedoutPleasureScene(reason); },
			[](const char* reason) { auto completion = BuildBleedPleasureCompletionHandlers(); (void)TFD::Bleedout::CompleteBleedPleasureHandoff(reason ? reason : "bleed_pleasure_handoff", completion); }
		},
		TFD::FlowController::BattleObserverRuntimeProviders{
			[](const char* reason) { TFD::Bleedout::ClearBridgeAliases(nullptr, reason); },
			[]() { ClearCaptiveOrchestrationResidue(); },
			[]() { TFD::Actor::Ops::ClearAggressorFactionContext(); },
			[]() { RecoverVictoryTeammates(); },
			[]() { ResetBleedRuntimeState(); },
			[]() { ClearPendingDefeatedDialogueTargetInternal(); },
			[](bool immune) { SetPlayerBleedImmune(immune); },
			[](const char* reason) { TFD::DefeatBridge::QueueNonCaptiveChoiceRequest(reason); },
			[]() -> RE::Actor* { const float followerRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 400.0f); auto followers = ResolveFollowerCandidates(followerRadius); return followers.downed; },
			[](RE::Actor* follower) { TFD::Transition::ArmObservedLeftForDeadFallback(follower, BuildTransitionRuntimeHandlers()); },
			[](const char* reason) { TFD::Transition::BeginRecoverTransition(reason ? reason : "battle_observe_loss", BuildTransitionRuntimeHandlers()); },
			[]() -> const char* { return TFD::Transition::GetCurrentFallbackBranchName(); }
		}
		};
	}

	static TFD::DefeatLifecycleBootstrap::InstallHooks BuildDefeatLifecycleBootstrapHooks()
	{
		TFD::DefeatLifecycleBootstrap::InstallHooks hooks{};
		hooks.buildBleedoutProviders = []() { return BuildBleedoutDefeatLifecycleProviders(); };
		hooks.buildFlowControllerProviders = []() { return BuildFlowControllerDefeatLifecycleProviders(); };
		return hooks;
	}

	// Defeat lifecycle / install / shutdown / queued progress / load state
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
		TFD::DefeatLifecycleBootstrap::Install(BuildDefeatLifecycleBootstrapHooks());

		TFD::DefeatRuntimeProviders::Install(BuildDefeatRuntimeProviderHooks());
		TFD::PleasureRuntime::Install();
		g_lastRouterCombatContextActive = false;
		ClearAllBleedLocks("install");
		TFD::Bleedout::ClearBridgeAliases(nullptr, "install");
		TFD::Actor::Ops::Initialize();
		TFD::Actor::Ops::InstallDefeatedEnemyQueryHooks(TFD::Actor::Ops::DefeatedEnemyQueryHooks{
			&IsTrackedDefeatedEnemyHook,
			&IsLastAggressorHook
			});
		TFD::Actor::Ops::InstallDefeatedEnemyStateHooks(TFD::Actor::Ops::DefeatedEnemyStateHooks{
			&TryGetDefeatedEnemyStateHook
			});
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
		TFD::FlowController::ResetDefeatLifecycleProviders();
		TFD::TeammateManager::ResetRuntimeProviders();
		TFD::Actor::Ops::InstallDefeatedEnemyStateHooks(TFD::Actor::Ops::DefeatedEnemyStateHooks{});
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
		TFD::Bleedout::ResetDefeatLifecycleProviders();
		TFD::Transition::DefeatGlue::Reset();
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
		TFD::Captive::ApplyQueuedDefeatProgressState(TFD::Captive::ApplyQueuedDefeatProgressHandlers{
			[]() -> RE::Actor* { return Player(); },
			[]() { return IsDialogueOpen(); },
			[](bool open) { g_prevDialogueOpen = open; }
			});
		SetPlayerBleedImmune(false);
		TFD::Bleedout::ClearBridgeAliases(nullptr, "apply_queued_state");
		SetRescueStateValue(0);
		RefreshPostDefeatGlobals();
		UpdatePreCombatState();
		spdlog::info("[TFD][Defeat] ApplyQueuedProgressState state={} phase={} bleed={}", TFD::Captive::GetQueuedStateFlag() ? 1 : 0, static_cast<int>(TFD::Captive::GetQueuedPhase()), g_queuedBleedOutState ? 1 : 0);
	}


	void ForceRecoverPlayerAfterCaptiveRecapture(const char* reason)
	{
		const char* why = reason ? reason : "captive_recapture_recover_player";
		auto* player = Player();
		if (!player) {
			ResetBleedRuntimeState(true);
			TFD::Bleedout::ForceStopBleedRuntimeForCaptiveRecapture(why);
			RefreshPostDefeatGlobals();
			spdlog::warn("[TFD][Defeat] captive recapture player recover skipped: player missing reason={}", why);
			return;
		}

		ReleasePlayerBleedLock(why, true);
		RestoreActorHealthToSafePct(
			player,
			TFD::Settings::GetDefeatThresholdPct(),
			10.0f,
			90.0f,
			95.0f,
			25.0f,
			why);
		ResetBleedRuntimeState(true);
		TFD::Bleedout::ForceStopBleedRuntimeForCaptiveRecapture(why);
		SetPlayerBleedImmune(false);

		if (!player->IsDead() && !player->IsDisabled()) {
			player->NotifyAnimationGraph("BleedoutStop");
			player->NotifyAnimationGraph("GetUpStart");
			player->EvaluatePackage(false, true);
			player->EvaluatePackage(true, true);
		}

		RefreshPostDefeatGlobals();
		spdlog::info("[TFD][Defeat] captive recapture player recovered hpPct={:.1f} reason={}",
			GetActorHealthPct(player),
			why);
	}

	void SuppressDefeatedEnemyAutoDeathForActor(RE::Actor* actor, double seconds, const char* reason)
	{
		const char* why = reason ? reason : "suppress_defeated_enemy_auto_death";
		if (!actor || actor->IsDisabled() || actor->IsDead()) {
			spdlog::warn("[TFD][Defeat] defeated enemy auto-death suppress skipped actor={:08X} reason={} invalid=1",
				actor ? actor->GetFormID() : 0u,
				why);
			return;
		}

		const double safeSeconds = (std::max)(2.0, seconds);
		TFD::Actor::Ops::SuppressDefeatedEnemyReentry(actor, safeSeconds, why);

		bool releasedActiveLock = false;
		auto it = g_bleedLocks.find(actor->GetFormID());
		if (it != g_bleedLocks.end()) {
			const auto kind = it->second.kind;
			const bool defeatedManaged = it->second.defeatedManaged;
			if (kind == BleedLockKind::Other || defeatedManaged) {
				ReleaseBleedLock(actor, why, true);
				releasedActiveLock = true;
			}
		}

		actor->NotifyAnimationGraph("BleedoutStop");
		actor->NotifyAnimationGraph("GetUpStart");
		actor->EvaluatePackage(false, true);
		actor->EvaluatePackage(true, true);

		spdlog::info("[TFD][Defeat] defeated enemy auto-death suppressed actor={:08X} seconds={:.1f} releasedActiveLock={} reason={}",
			actor->GetFormID(),
			safeSeconds,
			releasedActiveLock ? 1 : 0,
			why);
	}


	void ResetForLoad()
	{
		g_grace.store(false, std::memory_order_release);
		g_pendingCaptiveRecaptureRecovery = false;
		g_pendingCaptiveRecaptureRecoveryReason.clear();
		g_pendingCaptiveRecaptureRecoveryUntil = {};
		g_pendingCaptiveRecaptureRecoveryLastPulse = {};
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

	bool IsThresholdDownedActor(RE::Actor* actor)
	{
		if (!actor) {
			return true;
		}

		float thresholdPct = TFD::Settings::GetEnemyDownedThresholdPct();
		if (actor == Player()) {
			thresholdPct = TFD::Settings::GetDefeatThresholdPct();
		}
		else if (IsActiveFollowerActor(actor)) {
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

	bool IsActorCoveredByCurrentPassiveContext(RE::Actor*)
	{
		return false;
	}

}
