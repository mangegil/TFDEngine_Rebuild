#include "TFDDefeatMonitor.h"
#include "TFDActor.h"
#include "TFDVictory.h"

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
#include <utility>

#include <RE/Skyrim.h>
#include <type_traits>
#include <RE/A/ActorValues.h>
#include <RE/L/LockpickingMenu.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include "TFDSettings.h"
#include "TFDLocation.h"
#include "TFDCaptive.h"
#include "TFDDefeatCaptiveTickWiring.h"
#include "TFDCaptiveGreet.h"
#include "TFDRescueGreet.h"
#include "TFDRescue.h"
#include "TFDPostDefeatState.h"
#include "TFDDefeatBridge.h"
#include "TFDDefeatRuntimeProviders.h"
#include "TFDDefeatRuntimeProviderWiring.h"
#include "TFDDefeatLifecycleBootstrapWiring.h"
#include "TFDDefeatBleedoutLifecycleWiring.h"
#include "TFDDefeatFlowLifecycleWiring.h"
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
#include "TFDPlayerDamageGuard.h"
#include "TFDPlayerOverkillDamageHook.h"
#include "TFDPlayerDownRouter.h"
#include "TFDBleedLockState.h"
#include "TFDDialogueLifecycle.h"
#include "TFDDefeatRuntimeActions.h"
#include "TFDPlayerDownRouterWiring.h"
#include "TFDPlayerDownRouterPendingWiring.h"
#include "TFDPlayerDownRouterOutcomeWiring.h"
#include "TFDPlayerThresholdOutcomeWiring.h"
#include "TFDBleedLockRuntime.h"
#include "TFDDefeatBattleObserveState.h"
#include "TFDDefeatBleedoutRuntimeTimer.h"
#include "TFDDefeatBleedLockWiring.h"
#include "TFDDefeatTerminalLockout.h"
#include "TFDDefeatStickyReopenGrace.h"
#include "TFDDefeatFlowRefresh.h"
#include "TFDDefeatAggressorResolver.h"
#include "TFDBleedoutDialogueRuntime.h"
#include "TFDTeammateManager.h"
#include "TFDCaptiveRecaptureRecovery.h"
#include "TFDCaptiveRecaptureRecoveryWiring.h"
#include "EditorIdCache.h"
#include "RE/B/BGSRefAlias.h"
#include "RE/T/TESQuest.h"

namespace TFD::DefeatMonitor
{
	namespace
	{
		static bool ActorHasLineOfSightToPlayer(RE::Actor* actor, RE::Actor* player);
		static float Distance3D(const RE::NiPoint3& a, const RE::NiPoint3& b);
		using CaptivePhaseValue = TFD::Captive::PhaseValue;


		constexpr const char* kBleedoutCancelStickyReopenEvent = "TFDBleedoutCancelStickyReopen";
		constexpr const char* kBleedoutGreetConfirmedEvent = "TFDBleedoutGreetConfirmed";

		static std::uint32_t ResolveBleedFlowActorFormID();
		static void MaintainBleedPrimaryCaptorBinding();
		static bool IsStandingAllyThresholdActor(RE::Actor* actor);
		static TFD::Transition::RuntimeHandlers BuildTransitionRuntimeHandlers();
		static bool IsObserverAlly(RE::Actor* actor);
		static bool HasPlayerBleedLock();
		static void ClampHealth(RE::Actor* actor, float minHp);
		static TFD::PlayerDamageGuard::Config BuildPlayerDamageGuardConfig(RE::Actor* actor, float thresholdPct, float protectedHp, float safeFloorHp);
		static TFD::BleedoutDialogueRuntime::Context BuildBleedoutDialogueRuntimeContext();
		static void ResetBleedRuntimeState(bool preserveCaptive);

		using BleedDialogueOutcome = TFD::Bleedout::DialogueOutcome;
		using BleedTerminalCommit = TFD::Bleedout::TerminalCommit;

		std::atomic_bool g_installed{ false };
		std::atomic_bool g_running{ false };
		std::atomic_bool g_loadTransition{ false };
		std::atomic_flag g_tickPending = ATOMIC_FLAG_INIT;
		std::thread g_worker{};

		// P20: router combat context cache moved to TFDDefeatFlowRefresh.


		std::atomic_bool g_inBleedState{ false };
		float g_minHp{ 0.0f };

		// P11: pre-death shield lifecycle is owned by TFDPlayerOverkillDamageHook.


		bool g_playerBleedImmuneForced = false;
		bool g_playerWasEssential = false;
		bool g_playerWasInvulnerable = false;
		bool g_playerWasNoBleedoutRecovery = false;
		bool g_playerWasBaseInvulnerable = false;

		// P10: killmove guard lifecycle is owned by TFDPlayerOverkillDamageHook.

		static std::chrono::steady_clock::time_point Now()
		{
			return std::chrono::steady_clock::now();
		}

		// P24: bleedout runtime timer/countdown state is owned by TFDDefeatBleedoutRuntimeTimer.

		// P19: sticky reopen grace is owned by TFDDefeatStickyReopenGrace.

		// P18: post-terminal lockout is owned by TFDDefeatTerminalLockout.

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

		static RE::PlayerCharacter* Player();
		static bool IsStandingEnemyThresholdActor(RE::Actor* actor);
		static bool IsBleedSpaceCompatible(RE::Actor* actor, RE::Actor* player);
		static RE::Actor* ResolveCurrentCombatTarget(RE::Actor* actor);
		static bool IsActiveFollowerActor(RE::Actor* actor);
		static bool IsActorCloseAndFront(RE::Actor* actor, RE::Actor* player, float maxDist);

		// P25: aggressor resolver state is owned by TFDDefeatAggressorResolver.

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


		static std::uint32_t ResolveBleedFlowActorFormID()
		{
			const auto speakerId = TFD::Bleedout::GetBleedSpeakerID();
			if (speakerId != 0) {
				return speakerId;
			}
			return TFD::FlowController::Controller::GetSingleton().GetSnapshot().primaryActorFormID;
		}
		bool g_bleedPendingCaptiveOutcome = false;
		bool g_bleedPendingNonCaptiveOutcome = false;

		// P23: bleed battle observe state is owned by TFDDefeatBattleObserveState.


		using BleedLockKind = TFD::BleedLockState::Kind;
		using BleedLockEntry = TFD::BleedLockState::Entry;

		static const char* ReferenceProbeKindName(BleedLockKind kind)
		{
			return kind == BleedLockKind::Player ? "Player" : "Ally";
		}

		static const char* ReferenceProbeLifeStateName(RE::ACTOR_LIFE_STATE state)
		{
			switch (state) {
			case RE::ACTOR_LIFE_STATE::kAlive:
				return "Alive";
			case RE::ACTOR_LIFE_STATE::kDying:
				return "Dying";
			case RE::ACTOR_LIFE_STATE::kDead:
				return "Dead";
			case RE::ACTOR_LIFE_STATE::kUnconcious:
				return "Unconscious";
			case RE::ACTOR_LIFE_STATE::kReanimate:
				return "Reanimate";
			case RE::ACTOR_LIFE_STATE::kRecycle:
				return "Recycle";
			case RE::ACTOR_LIFE_STATE::kRestrained:
				return "Restrained";
			case RE::ACTOR_LIFE_STATE::kEssentialDown:
				return "EssentialDown";
			case RE::ACTOR_LIFE_STATE::kBleedout:
				return "Bleedout";
			default:
				return "Unknown";
			}
		}

		static const char* ReferenceProbeKnockStateName(RE::KNOCK_STATE_ENUM state)
		{
			switch (state) {
			case RE::KNOCK_STATE_ENUM::kNormal:
				return "Normal";
			case RE::KNOCK_STATE_ENUM::kExplode:
				return "Explode";
			case RE::KNOCK_STATE_ENUM::kExplodeLeadIn:
				return "ExplodeLeadIn";
			case RE::KNOCK_STATE_ENUM::kOut:
				return "Out";
			case RE::KNOCK_STATE_ENUM::kOutLeadIn:
				return "OutLeadIn";
			case RE::KNOCK_STATE_ENUM::kQueued:
				return "Queued";
			case RE::KNOCK_STATE_ENUM::kGetUp:
				return "GetUp";
			case RE::KNOCK_STATE_ENUM::kDown:
				return "Down";
			case RE::KNOCK_STATE_ENUM::kWaitForTaskQueue:
				return "WaitForTaskQueue";
			default:
				return "Unknown";
			}
		}

		static void LogReferenceBleedState(
			RE::Actor* actor,
			const BleedLockEntry& entry,
			const char* point,
			const char* note)
		{
			if (!actor) {
				spdlog::info(
					"[TFD][Defeat][R398A][ReferenceProbe] point={} actor=00000000 kind={} actorMissing=1 pulseCount={} probeOnly=1 mutation=0 note={}",
					point ? point : "unknown",
					ReferenceProbeKindName(entry.kind),
					entry.referenceProbePulseCount,
					note ? note : "-");
				return;
			}

			const auto* state = actor->AsActorState();
			const auto lifeState = state ? state->GetLifeState() : RE::ACTOR_LIFE_STATE::kAlive;
			const auto knockState = state ? state->GetKnockState() : RE::KNOCK_STATE_ENUM::kNormal;
			const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
			const auto& flags = actor->GetActorRuntimeData().boolFlags;

			spdlog::info(
				"[TFD][Defeat][R398A][ReferenceProbe] point={} actor={:08X} kind={} hp={:.2f} hpPct={:.1f} threshold={:.1f} life={}({}) knock={}({}) bleedingOut={} unconscious={} inBleedoutAnimation={} noBleedoutRecovery={} canSpeakEssentialDown={} essential={} protected={} loaded3D={} inCombat={} pulseCount={} probeOnly=1 mutation=0 note={}",
				point ? point : "unknown",
				actor->GetFormID(),
				ReferenceProbeKindName(entry.kind),
				hpNow,
				(hpNow / hpMax) * 100.0f,
				entry.thresholdPct,
				state ? ReferenceProbeLifeStateName(lifeState) : "NoActorState",
				state ? static_cast<std::int32_t>(lifeState) : -1,
				state ? ReferenceProbeKnockStateName(knockState) : "NoActorState",
				state ? static_cast<std::int32_t>(knockState) : -1,
				state && state->IsBleedingOut() ? 1 : 0,
				state && state->IsUnconscious() ? 1 : 0,
				flags.all(RE::Actor::BOOL_FLAGS::kInBleedoutAnimation) ? 1 : 0,
				flags.all(RE::Actor::BOOL_FLAGS::kNoBleedoutRecovery) ? 1 : 0,
				flags.all(RE::Actor::BOOL_FLAGS::kCanSpeakToEssentialDown) ? 1 : 0,
				flags.all(RE::Actor::BOOL_FLAGS::kEssential) ? 1 : 0,
				flags.all(RE::Actor::BOOL_FLAGS::kProtected) ? 1 : 0,
				actor->Is3DLoaded() ? 1 : 0,
				actor->IsInCombat() ? 1 : 0,
				entry.referenceProbePulseCount,
				note ? note : "-");
		}

		static void MaybeLogReferenceBleedSamples(
			RE::Actor* actor,
			BleedLockEntry& entry,
			std::chrono::steady_clock::time_point now)
		{
			if (!actor || entry.referenceProbeStartedAt.time_since_epoch().count() == 0) {
				return;
			}

			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				now - entry.referenceProbeStartedAt);
			const auto emit = [&](std::uint8_t bit, std::chrono::milliseconds delay, const char* point) {
				if ((entry.referenceProbeSampleMask & bit) == 0 && elapsed >= delay) {
					entry.referenceProbeSampleMask = static_cast<std::uint8_t>(entry.referenceProbeSampleMask | bit);
					LogReferenceBleedState(actor, entry, point, "scheduled_reference_sample");
				}
			};

			emit(0x01, std::chrono::milliseconds(250), "reference_250ms");
			emit(0x02, std::chrono::milliseconds(1000), "reference_1000ms");
			emit(0x04, std::chrono::milliseconds(3000), "reference_3000ms");
			emit(0x08, std::chrono::milliseconds(7500), "reference_7500ms");
		}

		auto& g_bleedLocks = TFD::BleedLockState::Entries();
		std::chrono::steady_clock::time_point g_enemyThresholdScanLast{};


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

			// R19: hard player damage immunity is owned by TFDPlayerDamageGuard.
			// DefeatMonitor may request the guard during threshold/bleed detection,
			// but it no longer owns or restores player essential/protected flags.
			TFD::PlayerDamageGuard::SetHardImmunity(
				player,
				enable,
				enable ? "defeat_monitor_request_enable" : "defeat_monitor_request_release");
		}

		// P10: killmove guard lifecycle moved to TFDPlayerOverkillDamageHook.



		static bool ActorHasLineOfSightToPlayer(RE::Actor* actor, RE::Actor* player)
		{
			if (!actor || !player) {
				return false;
			}
			bool hasLOSData = false;
			return actor->HasLineOfSight(player, hasLOSData);
		}


		static bool IsTrackedDefeatedEnemyHook(RE::Actor* actor)
		{
			return TFD::DefeatBattleObserveState::IsTrackedEnemy(actor);
		}


		static std::vector<RE::Actor*> CollectBleedStandingFollowers(float radius);
		static bool IsObserverAlly(RE::Actor* actor);
		static bool IsValidBleedBattleEnemyRosterActor(RE::Actor* actor, RE::Actor* player);
		static bool IsActorBleedingOut(RE::Actor* actor);
		static float GetActorHealthPct(RE::Actor* actor);
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

		static void SetRescueStateValue(int value);

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
			handlers.isCaptiveSupportedAggressor = [](RE::Actor* actor) { return TFD::DefeatAggressorResolver::IsCaptiveSupportedAggressor(actor); };
			handlers.isBleedCrowdSupportedAggressor = [](RE::Actor* actor) { return TFD::DefeatAggressorResolver::IsBleedCrowdSupportedAggressor(actor); };
			handlers.isBleedSpaceCompatible = [](RE::Actor* actor, RE::Actor* player) { return IsBleedSpaceCompatible(actor, player); };
			handlers.hasLineOfSightToPlayer = [](RE::Actor* actor, RE::Actor* player) { return ActorHasLineOfSightToPlayer(actor, player); };
			handlers.isActorCloseAndFront = [](RE::Actor* actor, RE::Actor* player, float maxDist) { return IsActorCloseAndFront(actor, player, maxDist); };
			handlers.resolveCurrentCombatTarget = [](RE::Actor* actor) -> RE::Actor* { return ResolveCurrentCombatTarget(actor); };
			handlers.isActiveFollowerActor = [](RE::Actor* actor) { return IsActiveFollowerActor(actor); };
			handlers.resolveLastAggressor = []() -> RE::Actor* { return TFD::DefeatAggressorResolver::ResolveLastAggressor(); };
			handlers.isPreservedAssigned = [preserveAssigned](RE::Actor* actor) {
				if (!preserveAssigned || !actor) {
					return false;
				}
				return TFD::Bleedout::HasBleedCrowdAssignedID(actor->GetFormID());
				};
			return handlers;
		}


		static TFD::BleedoutDialogueRuntime::Context BuildBleedoutDialogueRuntimeContext()
		{
			TFD::BleedoutDialogueRuntime::Context context{};
			context.getPlayer = []() -> RE::Actor* { return Player(); };
			context.buildSpeakerHandlers = [](bool preserveAssigned) { return BuildBleedoutSpeakerHandlers(preserveAssigned); };
			context.buildRuntimeStateRefs = []() { return TFD::DefeatRuntimeActions::BuildBleedRuntimeHostStateRefs(); };
			context.buildRuntimeHostHandlers = []() { return TFD::Bleedout::RuntimeHost::BuildHandlers(); };
			context.currentSpeakerID = []() { return TFD::Bleedout::GetBleedSpeakerID(); };
			context.ownsCurrentFlow = []() { return TFD::Bleedout::OwnsCurrentFlow(); };
			return context;
		}

		static std::vector<RE::Actor*> CollectBleedoutCrowd(float radius, RE::Actor* preferred, bool preserveAssigned = false)
		{
			return TFD::Bleedout::CollectCrowd(radius, preferred, preserveAssigned, BuildBleedoutSpeakerHandlers(preserveAssigned));
		}


		static void ResetBleedRuntimeState(bool preserveCaptive = false)
		{
			TFD::Bleedout::RuntimeResetHandlers handlers{};
			handlers.releasePlayerBleedLock = [&](const char* reason) { TFD::BleedLockRuntime::ReleasePlayer(TFD::DefeatBleedLockWiring::BuildContext(), reason, false); };
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
				TFD::DefeatBattleObserveState::ResetTracking();
				};
			handlers.resetDialogueRuntimeState = [&]() {
				g_bleedPendingCaptiveOutcome = false;
				g_bleedPendingNonCaptiveOutcome = false;
				TFD::DefeatBleedoutRuntimeTimer::ResetDialogueRuntimeClock();
				TFD::Bleedout::ResetBleedSpeakerKick();
				TFD::Bleedout::SetBleedDialogueRetryCount(0);
				TFD::Bleedout::BleedLastCrowdAssignRef() = {};
				TFD::Bleedout::ClearBleedCrowdAssigned();
				TFD::Bleedout::ClearBleedRejectedSpeakerIds();
				};
			handlers.resetBattleObserveState = [&]() {
				TFD::DefeatBattleObserveState::ResetFlags();
				};
			handlers.clearEscapeBreakState = [&]() { TFD::Captive::ClearEscapeBreakRebleed(); };
			handlers.clearLastEnemyTargetingPlayer = [&]() { TFD::DefeatAggressorResolver::ClearLastEnemyTargetingPlayer(); };
			handlers.clearOutcomeWindow = [&](const char* reason) { TFD::Bleedout::ClearSystemEventOutcomeWindow(reason); };
			handlers.resetPleasureRuntime = [&](const char* reason) { TFD::PleasureRuntime::ResetRuntime(reason); };

			TFD::Bleedout::ResetRuntimeState(preserveCaptive, "reset_bleed_runtime", handlers);
			TFD::PlayerOverkillDamageHook::SetKillmoveGuard(false, "reset_bleed_runtime");
		}

		static void TransitionBleedRuntimeToPleasureCommit(const char* reason)
		{
			TFD::Bleedout::TransitionRuntimeToPleasureCommit(
				reason,
				static_cast<std::uint32_t>(TFD::Bleedout::GetBleedSpeakerID()),
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
						TFD::DefeatBleedoutRuntimeTimer::ResetDialogueRuntimeClock();
						TFD::Bleedout::ResetBleedSpeakerKick();
						TFD::Bleedout::SetBleedDialogueRetryCount(0);
						TFD::Bleedout::BleedLastCrowdAssignRef() = {};
						TFD::Bleedout::ClearBleedCrowdAssigned();
						TFD::Bleedout::ClearBleedRejectedSpeakerIds();
					},
					[&]() {
						TFD::DefeatBattleObserveState::ResetFlags();
						TFD::DefeatBattleObserveState::ResetTracking();
					},
					[&]() { TFD::Captive::ClearEscapeBreakRebleed(); },
					[&]() { TFD::DefeatAggressorResolver::ClearLastEnemyTargetingPlayer(); },
					[&](const char* why) { TFD::Bleedout::ClearSystemEventOutcomeWindow(why); }
				});
		}

		static CaptivePhaseValue PhaseFromRaw(std::uint32_t raw)
		{
			return TFD::Captive::PhaseFromRaw(raw);
		}

		static void SetGraceSeconds(int seconds);
		static bool IsGraceActive();
		static bool IsActorCloseAndFront(RE::Actor* actor, RE::Actor* player, float maxDist);
		static void StartBleedWindow(RE::Actor* player, RE::Actor* aggressor);

		static void StartBleedWindow(RE::Actor* player, RE::Actor* aggressor)
		{
			TFD::DefeatStickyReopenGrace::Clear("start_bleed_window");
			TFD::Bleedout::RuntimeHost::StartWindow(player, aggressor);
		}

		static TFD::Transition::RuntimeHandlers BuildTransitionRuntimeHandlers()
		{
			TFD::Transition::RuntimeHandlers handlers{};
			handlers.getPlayer = []() -> RE::Actor* { return Player(); };
			handlers.resolveAggressor = []() -> RE::Actor* { return TFD::DefeatAggressorResolver::ResolveAggressor(); };
			handlers.findBestAggressor = [](float radius) -> RE::Actor* { return TFD::DefeatAggressorResolver::FindBestAggressor(radius); };
			handlers.isCombatSupportedAggressor = [](RE::Actor* actor) { return TFD::DefeatAggressorResolver::IsCombatSupportedAggressor(actor); };
			handlers.isActiveFollowerActor = [](RE::Actor* actor) { return TFD::TeammateManager::IsActiveFollowerActor(actor); };
			handlers.isStandingAllyThresholdActor = [](RE::Actor* actor) { return IsStandingAllyThresholdActor(actor); };
			handlers.collectRegisteredTeammates = []() { return TFD::TeammateManager::CollectRegisteredTeammates(); };
			handlers.collectBleedoutCrowd = [](float radius, RE::Actor* preferred, bool preserveAssigned) {
				return CollectBleedoutCrowd(radius, preferred, preserveAssigned);
			};
			handlers.getBleedCrowdAssigned = []() { return TFD::Bleedout::GetBleedCrowdAssignedIDs(); };
			handlers.releasePlayerBleedLock = [](const char* reason, bool playGetUp) { TFD::BleedLockRuntime::ReleasePlayer(TFD::DefeatBleedLockWiring::BuildContext(), reason, playGetUp); };
			handlers.setGraceSeconds = [](int seconds) { SetGraceSeconds(seconds); };
			handlers.setRescueStateValue = [](int value) { TFD::Rescue::SetStateValue(value); };
			handlers.refreshPostDefeatGlobals = []() { TFD::DefeatFlowRefresh::RefreshPostDefeatGlobals(); };
			handlers.updatePreCombatState = []() { TFD::DefeatFlowRefresh::UpdatePreCombatState(); };
			return handlers;
		}

		static TFD::DefeatRuntimeActions::NoMarkerFallbackContext BuildNoMarkerFallbackContext()
		{
			TFD::DefeatRuntimeActions::NoMarkerFallbackContext context{};
			context.buildTransitionRuntimeHandlers = []() { return BuildTransitionRuntimeHandlers(); };
			context.clearCaptiveOrchestrationResidue = []() { TFD::Bleedout::DefeatGlue::ClearCaptiveOrchestrationResidue(true); };
			context.getPlayer = []() -> RE::Actor* { return Player(); };
			context.clearBridgeAliases = [](const char* reason) { TFD::Bleedout::ClearBridgeAliases(nullptr, reason); };
			context.setPlayerBleedImmune = [](bool immune) { SetPlayerBleedImmune(immune); };
			context.resetBleedRuntimeState = []() { ResetBleedRuntimeState(); };
			context.clearLastAggressor = []() { TFD::DefeatAggressorResolver::ClearLastAggressor(); };
			context.updatePreCombatState = []() { TFD::DefeatFlowRefresh::UpdatePreCombatState(); };
			return context;
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
			if (HasActiveBleedLock(actor) || TFD::Actor::Ops::IsDefeatedEnemyKnocked(actor)) {
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

		static void MaintainBleedPrimaryCaptorBinding()
		{
			TFD::Bleedout::MaintainPrimaryCaptorBinding(TFD::DialogueLifecycle::IsDialogueOpen(), TFD::DefeatRuntimeActions::BuildBleedRuntimeHostStateRefs());
		}

		// Shared combat-target and player-side actor helpers

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

		// R17: DefeatMonitor is a HP/threshold sensor only.
		// Combat target mutation is owned by TFDCombatBehavior.
		// Keep these legacy hooks as no-op providers because FlowController/
		// Bleedout runtime still pass the function pointer through existing
		// provider structs.
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

		// P20: PostDefeat/PreCombat refresh ownership moved to TFDDefeatFlowRefresh.



		// P21: Captive runtime tick handler construction moved to TFDDefeatCaptiveTickWiring.

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

		static bool HasPlayerBleedLock()
		{
			return TFD::BleedLockRuntime::HasPlayer(TFD::DefeatBleedLockWiring::BuildContext());
		}

		static float ResolvePlayerBleedRuntimeSafeHealth(RE::Actor* actor, float thresholdPct)
		{
			if (!actor) {
				return 1.0f;
			}
			const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
			// Keep enough real HP under the hard-invuln flags to absorb oversized hits.
			// TFD state, not low raw HP, owns player defeat once the bleed runtime starts.
			const float safePct = std::clamp((std::max)(70.0f, thresholdPct + 45.0f), 35.0f, 95.0f);
			return (std::max)(1.0f, hpMax * (safePct / 100.0f));
		}


		static TFD::PlayerDamageGuard::Config BuildPlayerDamageGuardConfig(RE::Actor* actor, float thresholdPct, float protectedHp, float safeFloorHp)
		{
			TFD::PlayerDamageGuard::Config config{};
			config.thresholdPct = std::clamp(thresholdPct, 2.0f, 95.0f);
			config.protectedHp = (std::max)(1.0f, protectedHp);
			config.safeFloorHp = (std::max)(1.0f, safeFloorHp);
			if (actor) {
				const float maxHp = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
				config.protectedHp = std::clamp(config.protectedHp, 1.0f, maxHp);
				config.safeFloorHp = std::clamp(config.safeFloorHp, 1.0f, config.protectedHp);
			}
			return config;
		}



		static void EnforcePlayerBleedInvulnerability(RE::Actor* actor, BleedLockEntry& entry)
		{
			if (!actor) {
				return;
			}

			if (actor->IsDead(false) || actor->GetActorRuntimeData().boolFlags.all(RE::Actor::BOOL_FLAGS::kIsInKillMove)) {
				TFD::DefeatBleedLockWiring::ForcePlayerBleedAlive(actor, entry, actor->IsDead(false) ? "dead_state" : "killmove_state");
			}

			SetPlayerBleedImmune(true);

			entry.minHp = (std::max)(entry.minHp, (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth)) * 0.05f);
			const auto guardTick = TFD::PlayerDamageGuard::Tick(
				actor,
				BuildPlayerDamageGuardConfig(actor, entry.thresholdPct, entry.protectedHealth, entry.minHp));
			if (guardTick.active) {
				entry.protectedHealth = (std::max)(entry.protectedHealth, guardTick.protectedHp);
				entry.lastHealthSample = (std::max)(entry.lastHealthSample, guardTick.actualHpAfter);
			}
			entry.protectedHealth = (std::max)(entry.protectedHealth, ResolvePlayerBleedRuntimeSafeHealth(actor, entry.thresholdPct));
			g_minHp = (std::max)(g_minHp, entry.protectedHealth);

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


		static void ScanEnemyThresholdCandidatesOnly(const char* reason)
		{
			auto* player = Player();
			if (!player) {
				return;
			}

			const float radius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 600.0f);
			auto snapshot = TFD::Actor::BuildSnapshot(radius, false);
			const float threshold = std::clamp(TFD::Settings::GetEnemyDownedThresholdPct(), 2.0f, 95.0f);

			for (const auto& info : snapshot.actors) {
				auto* actor = info.get();
				if (!actor || actor == player || actor->IsDisabled() || actor->IsDead()) {
					continue;
				}
				if (TFD::TeammateManager::IsPlayerSideTeammateActor(actor) || IsActiveFollowerActor(actor)) {
					continue;
				}
				if (GetActorHealthPct(actor) > threshold) {
					continue;
				}
				// Sensor ownership stops at the HP crossing. Victory performs the
				// hostile/candidate validation and deduplicates repeated notices.
				(void)TFD::Victory::NotifyEnemyBelowThreshold(
					actor,
					threshold,
					reason && reason[0] ? reason : "enemy_threshold_sensor");
			}
		}

		static void TickEnemyThresholdSensor(const char* reason)
		{
			const auto now = Now();
			if (g_enemyThresholdScanLast.time_since_epoch().count() != 0 &&
				(now - g_enemyThresholdScanLast) < std::chrono::milliseconds(250)) {
				return;
			}
			g_enemyThresholdScanLast = now;
			ScanEnemyThresholdCandidatesOnly(reason && reason[0] ? reason : "enemy_threshold_sensor");
		}


		// P14: BleedLockRuntime context construction moved to TFDDefeatBleedLockWiring.


		// Temporary split module: bleed dialogue/runtime orchestration

		// P19: sticky reopen grace state moved to TFDDefeatStickyReopenGrace.

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
				TFD::DefeatFlowRefresh::UpdatePreCombatState();
				return;
			}
			TFD::DefeatFlowRefresh::RefreshPostDefeatGlobals();

			// R393A: DefeatMonitor is the Enemy HP sensor only. Victory owns
			// registry, passive state, countdown, and auto-death.
			TickEnemyThresholdSensor("pre_owner_gate");

			const bool inCombatDialogueOpen = TFD::DialogueLifecycle::IsDialogueOpen();
			if (!inCombatDialogueOpen &&
				!TFD::InteractionRouter::DialogueOpen::IsActive() &&
				TFD::Bleedout::TickQueuedAfterPleasureCrowdContinuation()) {
				return;
			}
			TFD::CaptiveRecaptureRecoveryWiring::TickPulse();
			const bool captiveBleedOverlay = TFD::Captive::HasEscapeBreakRebleedPending() || g_inBleedState.load(std::memory_order_acquire);
			const auto dialogueSnapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
			auto dialogueTick = TFD::DialogueLifecycle::TickOpenEdges(TFD::DialogueLifecycle::TickInput{
				captiveBleedOverlay,
				TFD::InCombat::IsActive(),
				TFD::Rescue::IsActive(),
				dialogueSnapshot.sub == TFD::FlowController::SubFlow::BleedoutAfterPleasure,
				TFD::PleasureRuntime::IsBlocking()
			});
			if (dialogueTick.handledBleedAfterPleasurePause) {
				return;
			}

			if (!TFD::DefeatCaptiveTickWiring::TickRuntime(Player(), captiveBleedOverlay)) {
				return;
			}

			if (ui && ui->GameIsPaused()) return;
			auto* player = Player();
			if (!player) {
				TFD::DefeatFlowRefresh::RefreshPostDefeatGlobals();
				return;
			}
			TFD::FlowController::TickRuntime();
			TFD::Location::UpdateAmbientKidnapAvailability(false);
			if (TFD::Bleedout::DefeatGlue::HandlePendingEscapeBreak()) {
				return;
			}

			// R215A: Pay/Release and Left-For-Dead terminal outcomes arm the
			// post-defeat recovery cooldown before the next threshold scan.  The
			// old order scanned TickBleedLocks() first, so a still-low player HP
			// could immediately create a new Bleedout window before recovery had
			// a chance to own the state.
			if (TFD::Transition::IsLeftForDeadCooldownActive(BuildTransitionRuntimeHandlers())) {
				TFD::Transition::TickLeftForDeadCooldown(BuildTransitionRuntimeHandlers());
				return;
			}

			TFD::BleedLockRuntime::Tick(TFD::DefeatBleedLockWiring::BuildContext());
			TFD::DefeatFlowRefresh::UpdatePreCombatState();
			if (IsGraceActive()) return;
			if (!captiveBleedOverlay && !g_inBleedState.load(std::memory_order_acquire) && TFD::InCombat::IsActive()) {
				// R470A: DefeatMonitor must not own InCombat dialogue lifecycle.
				// InCombatGreet / FlowController own sticky reopen, close, complete,
				// cancel, and AfterPleasure handoff.  DefeatMonitor only observed the
				// DialogueMenu open edge above for compatibility with existing state.
			}


			if (g_inBleedState.load(std::memory_order_acquire)) {
				if (TFD::DefeatBattleObserveState::Pending()) {
					TFD::Bleedout::RuntimeHost::TickBattleObservePending();
					return;
				}
				if (TFD::DefeatBattleObserveState::Active()) {
					TFD::Bleedout::RuntimeHost::TickBattleObserve();
					return;
				}
				if (TFD::DefeatStickyReopenGrace::Tick(player, Now())) {
					return;
				}
				{
					const float runtimeSafeHp = ResolvePlayerBleedRuntimeSafeHealth(player, TFD::Settings::GetDefeatThresholdPct());
					g_minHp = (std::max)(g_minHp, runtimeSafeHp);
					SetPlayerBleedImmune(true);
					ClampHealth(player, g_minHp);
				}
				MaintainBleedPrimaryCaptorBinding();
				TFD::Bleedout::DefeatGlue::MaintainSpeakerKick();
				const bool dOpen = TFD::DialogueLifecycle::IsDialogueOpen();
				const int bleedSeconds = TFD::Settings::GetBleedWindowSeconds();
				const bool pleasureCommitted = TFD::Bleedout::GetDialogueOutcome() == BleedDialogueOutcome::Pleasure;
				const bool ostimBridgeBlocking = TFD::PleasureRuntime::IsBlocking();
				auto holdDecision = TFD::BleedoutGreet::EvaluateHold(dOpen, pleasureCommitted, ostimBridgeBlocking);
				if (holdDecision.hold) {
					const auto nowBleedHold = Now();
					if (dOpen) {
						TFD::DialogueLifecycle::ObserveBleedoutDialogueOpened("bleed_hold");
					}
					(void)nowBleedHold;
					TFD::DefeatBleedoutRuntimeTimer::Pause(TFD::BleedoutGreet::GetHoldReasonName(holdDecision.reason));
					TFD::DialogueLifecycle::SetDialogueOpenObserved(dOpen);
					return;
				}
				// R130: no destructive Bleedout forcegreet timeout/rearm.
				// Long-distance or stuck speakers should be handled by approach assist / MoveTo,
				// not by cancelling and restarting the dialogue lifecycle.
				TFD::DefeatBleedoutRuntimeTimer::ResumeIfPaused();

				// R470A: no DefeatMonitor-owned Bleedout forcegreet reopen.
				// Initial handoff retry and speaker primer belong to TFDBleedout/TFDBleedoutGreet.
				// This keeps DefeatMonitor as HP/death guard instead of a dialogue owner.

				if (TFD::Bleedout::GetBleedSpeakerID() == 0) {
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
				// R470A: DefeatMonitor is no longer a Bleedout dialogue decision owner.
				// It may observe the close edge to clear local monitor bookkeeping, but it
				// must not resolve Pay/Release, captive fallback, post-dialogue system
				// events, or sticky reopen.  Those are route-owner responsibilities
				// (Bleedout/PleasureRuntime/FlowController/Papyrus fragments).
				const bool bleedDialogueSeen = TFD::BleedoutGreet::HasSeenDialogue();
				const bool bleedTerminalCommit = TFD::Bleedout::HasTerminalCommit();
				const bool bleedPleasureBlocking = TFD::PleasureRuntime::IsBlocking();
				const bool bleedCaptiveOutcome = TFD::Bleedout::GetDialogueOutcome() == BleedDialogueOutcome::Captive;
				if (bleedDialogueSeen && TFD::DialogueLifecycle::WasDialogueOpen() && !dOpen) {
					TFD::DialogueLifecycle::SetDialogueOpenObserved(false);
					TFD::DefeatBleedoutRuntimeTimer::SetCountdownDirty();
					TFD::DefeatStickyReopenGrace::Clear("r470a_monitor_only_dialogue_closed");
					TFD::BleedoutGreet::MarkStickyReopenPending(false, "r470a_monitor_only_dialogue_closed");
					spdlog::info(
						"[TFD][Defeat][R470A] bleed dialogue closed observed monitor-only terminal={} pleasureBlocking={} captiveOutcome={} speaker={:08X}",
						bleedTerminalCommit ? 1 : 0,
						bleedPleasureBlocking ? 1 : 0,
						bleedCaptiveOutcome ? 1 : 0,
						TFD::Bleedout::GetBleedSpeakerID());

					if (bleedTerminalCommit || bleedPleasureBlocking || bleedCaptiveOutcome) {
						return;
					}
				}

				if (TFD::Bleedout::IsAwaitingSystemEventOutcome() && !TFD::DialogueLifecycle::WasDialogueOpen()) {
					const char* pendingSystemReason = nullptr;
					(void)TFD::Bleedout::IsSystemEventPendingForFallback(&pendingSystemReason);
					if (pendingSystemReason && pendingSystemReason[0]) {
						spdlog::info(
							"[TFD][Defeat][R470A] pending bleed system event observed monitor-only reason={} terminal={} speaker={:08X}",
							pendingSystemReason,
							TFD::Bleedout::HasTerminalCommit() ? 1 : 0,
							TFD::Bleedout::GetBleedSpeakerID());
						return;
					}
				}

				const bool suppressCaptiveRecaptureNotice = TFD::Captive::IsEscapeBleedoutActive() || TFD::Captive::IsRecaptureCommitActive() || TFD::Captive::IsRecaptureRecentlyCommitted();
				if (TFD::DefeatBleedoutRuntimeTimer::TickCountdown(TFD::DefeatBleedoutRuntimeTimer::CountdownTickInput{
					bleedSeconds,
					suppressCaptiveRecaptureNotice,
					[]() -> bool {
						const char* pendingSystemReason = nullptr;
						(void)TFD::Bleedout::IsSystemEventPendingForFallback(&pendingSystemReason);
						TFD::Bleedout::TimeoutContext timeoutContext{
							TFD::Bleedout::HasTerminalCommit(),
							TFD::Bleedout::GetTerminalCommitName(TFD::Bleedout::GetTerminalCommit()),
							pendingSystemReason,
							g_bleedPendingCaptiveOutcome
						};
						auto timeoutHandlers = TFD::Bleedout::Builders::BuildTimeoutHandlers();
						return TFD::Bleedout::HandleBleedTimeout(timeoutContext, "bleed_timeout", timeoutHandlers);
					}
				})) {
					return;
				}
				return;
			}
			const float hpNow = player->GetActorValue(RE::ActorValue::kHealth);
			const float hpMax = (std::max)(1.0f, player->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float pct = (hpNow / hpMax) * 100.0f;
			const float thresh = TFD::Settings::GetDefeatThresholdPct();
			if (pct <= thresh) {
				if (TFD::DefeatTerminalLockout::IsActive("player_threshold", pct, thresh)) {
					return;
				}
				if (TFD::PlayerThresholdOutcomeWiring::HandlePlayerThreshold(player, pct, thresh, "player_threshold")) {
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

				if (std::strcmp(rawName, kBleedoutCancelStickyReopenEvent) == 0) {
					auto* speaker = ev->sender ? ev->sender->As<RE::Actor>() : nullptr;
					const auto speakerID = speaker ? speaker->GetFormID() : 0u;
					const bool wasGraceActive = TFD::DefeatStickyReopenGrace::IsActive();
					const auto graceSpeakerID = TFD::DefeatStickyReopenGrace::SpeakerID();
					const char* why = ev->strArg.empty() ? "r441a_reject_cycle_handoff" : ev->strArg.c_str();

					TFD::DefeatStickyReopenGrace::Clear(why);
					TFD::Bleedout::ClearSystemEventOutcomeWindow(why);
					TFD::BleedoutGreet::ClearFlowGreetConfirmed(why);
					TFD::BleedoutGreet::MarkStickyReopenPending(false, why);
					TFD::BleedoutGreet::ResetRuntime(why);
					TFD::DialogueLifecycle::SetDialogueOpenObserved(false);
					TFD::DefeatBleedoutRuntimeTimer::SetCountdownDirty();
					spdlog::info(
						"[TFD][Defeat][R441A] bleed sticky reopen cancelled by reject cycle sender={:08X} graceSpeaker={:08X} wasActive={} reason={}",
						speakerID,
						graceSpeakerID,
						wasGraceActive ? 1 : 0,
						why);
					return RE::BSEventNotifyControl::kContinue;
				}

				if (TFD::FlowController::HandleOutcomeModEvent(rawName, ev->strArg.c_str(), ev->numArg, ev->sender)) {
					if (TFD::DefeatTerminalLockout::IsBleedoutTerminalOutcomeEvent(rawName)) {
						TFD::DefeatStickyReopenGrace::CancelAfterTerminalOutcome(rawName, "mod_event_terminal_outcome");
						TFD::DefeatTerminalLockout::Arm(rawName, TFD::DefeatTerminalLockout::ResolveSeconds(rawName, ev->numArg), "mod_event_terminal_outcome");
					}
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
				if (targetActor && targetActor == Player() &&
					causeActor && causeActor != targetActor &&
					TFD::DefeatAggressorResolver::IsCombatSupportedAggressor(causeActor) &&
					!IsObserverAlly(causeActor)) {
					TFD::DefeatAggressorResolver::NoteEnemyTargetingPlayer(causeActor);
					TFD::PlayerOverkillDamageHook::SetKillmoveGuard(true, "tes_hit_event_player_target", std::chrono::milliseconds(2500));
					(void)TFD::PlayerOverkillDamageHook::TryQueueKillmoveBlockedBleedout(targetActor, causeActor, "tes_hit_event_player_target");
				}
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

	// P27D: FlowController defeat lifecycle provider construction moved to TFDDefeatFlowLifecycleWiring.


	// Defeat lifecycle / install / shutdown / queued progress / load state
	void Install()
	{
		if (g_installed.exchange(true, std::memory_order_acq_rel)) return;
		g_running.store(true, std::memory_order_release);
		g_loadTransition.store(false, std::memory_order_release);
		TFD::Captive::ResetForLoad();
		TFD::Bleedout::DefeatGlue::ClearCaptiveOrchestrationResidue(false);
		SetPlayerBleedImmune(false);
		TFD::PlayerDamageGuard::Reset("defeat_monitor_reset");

		TFD::DefeatAggressorResolver::Dependencies aggressorResolverDependencies{};
		aggressorResolverDependencies.getPlayer = []() -> RE::PlayerCharacter* { return Player(); };
		aggressorResolverDependencies.isActiveFollowerActor = [](RE::Actor* actor) -> bool { return IsActiveFollowerActor(actor); };
		aggressorResolverDependencies.isStandingEnemyThresholdActor = [](RE::Actor* actor) -> bool { return IsStandingEnemyThresholdActor(actor); };
		aggressorResolverDependencies.isObserverAlly = [](RE::Actor* actor) -> bool { return IsObserverAlly(actor); };
		TFD::DefeatAggressorResolver::InstallProvider(std::move(aggressorResolverDependencies));

		TFD::PlayerThresholdOutcomeWiring::Dependencies playerThresholdOutcomeDependencies{};
		playerThresholdOutcomeDependencies.collectStandingFollowers = [](float radius) { return CollectBleedStandingFollowers(radius); };
		playerThresholdOutcomeDependencies.isObserverAlly = [](RE::Actor* actor) -> bool { return IsObserverAlly(actor); };
		playerThresholdOutcomeDependencies.isInBleedState = []() -> bool { return g_inBleedState.load(std::memory_order_acquire); };
		playerThresholdOutcomeDependencies.releaseStaleEscapeBreakBleedRuntime = [](const char* reason) {
			g_inBleedState.store(false, std::memory_order_release);
			g_minHp = 0.0f;
			TFD::BleedoutGreet::ResetRuntime(reason ? reason : "r270_stale_escape_break_rebleed");
			TFD::Bleedout::ClearDialogueOutcome(reason ? reason : "r270_stale_escape_break_rebleed");
		};
		TFD::PlayerThresholdOutcomeWiring::InstallProvider(std::move(playerThresholdOutcomeDependencies));

		TFD::DefeatBleedLockWiring::Dependencies bleedLockDependencies{};
		bleedLockDependencies.getPlayer = []() -> RE::Actor* { return Player(); };
		bleedLockDependencies.resolveAggressor = []() -> RE::Actor* { return TFD::DefeatAggressorResolver::ResolveAggressor(); };
		bleedLockDependencies.setPlayerBleedImmune = [](bool enable) { SetPlayerBleedImmune(enable); };
		bleedLockDependencies.getMinHp = []() -> float { return g_minHp; };
		bleedLockDependencies.setMinHp = [](float value) { g_minHp = value; };
		bleedLockDependencies.maxMinHp = [](float value) { g_minHp = (std::max)(g_minHp, value); };
		bleedLockDependencies.resetPreDeathShield = []() { TFD::PlayerOverkillDamageHook::ClearPreDeathShield("bleed_lock_reset", false); };
		bleedLockDependencies.isInBleedState = []() -> bool { return g_inBleedState.load(std::memory_order_acquire); };
		bleedLockDependencies.resolvePlayerBleedRuntimeSafeHealth = [](RE::Actor* actor, float thresholdPct) -> float {
			return ResolvePlayerBleedRuntimeSafeHealth(actor, thresholdPct);
		};
		bleedLockDependencies.buildPlayerDamageGuardConfig = [](RE::Actor* actor, float thresholdPct, float protectedHp, float safeFloorHp) {
			return BuildPlayerDamageGuardConfig(actor, thresholdPct, protectedHp, safeFloorHp);
		};
		bleedLockDependencies.clampHealth = [](RE::Actor* actor, float minHp) { ClampHealth(actor, minHp); };
		bleedLockDependencies.clampHealthCeiling = [](RE::Actor* actor, float maxHp) { ClampHealthCeiling(actor, maxHp); };
		bleedLockDependencies.resolveActorHealthForPct = [](RE::Actor* actor, float pct) -> float { return ResolveActorHealthForPct(actor, pct); };
		bleedLockDependencies.getActorHealthPct = [](RE::Actor* actor) -> float { return GetActorHealthPct(actor); };
		bleedLockDependencies.isActorBleedingOut = [](RE::Actor* actor) -> bool { return IsActorBleedingOut(actor); };
		bleedLockDependencies.logReferenceBleedState = [](RE::Actor* actor, const BleedLockEntry& entry, const char* point, const char* note) {
			LogReferenceBleedState(actor, entry, point, note);
		};
		bleedLockDependencies.maybeLogReferenceBleedSamples = [](RE::Actor* actor, BleedLockEntry& entry, std::chrono::steady_clock::time_point now) {
			MaybeLogReferenceBleedSamples(actor, entry, now);
		};
		bleedLockDependencies.enforcePlayerBleedInvulnerability = [](RE::Actor* actor, BleedLockEntry& entry) { EnforcePlayerBleedInvulnerability(actor, entry); };
		bleedLockDependencies.tickPlayerKillmoveSuppression = []() { TFD::PlayerOverkillDamageHook::TickKillmoveSuppression(); };
		bleedLockDependencies.tickPlayerOverkillBlockPending = []() { TFD::PlayerDownRouterPendingWiring::TickPendingOverkillRoute(); };
		bleedLockDependencies.tickPlayerPreDeathShield = []() { TFD::PlayerOverkillDamageHook::TickPreDeathShield(); };
		bleedLockDependencies.scanBleedLockCandidates = []() { TFD::DefeatBleedLockWiring::ScanCandidates(); };
		bleedLockDependencies.findBestAggressor = [](float radius) -> RE::Actor* { return TFD::DefeatAggressorResolver::FindBestAggressor(radius); };
		bleedLockDependencies.isActiveFollowerActor = [](RE::Actor* actor) -> bool { return IsActiveFollowerActor(actor); };
		bleedLockDependencies.hasPlayerBleedLock = []() -> bool { return HasPlayerBleedLock(); };
		bleedLockDependencies.dispatchThresholdScanImmediateBleedout = [](RE::Actor* player, float hpPct, float thresholdPct, const char* reason) -> bool {
			return TFD::PlayerThresholdOutcomeWiring::DispatchImmediateBleedout(player, hpPct, thresholdPct, reason);
		};
		bleedLockDependencies.scanPlayerThresholdOutcome = [](RE::Actor* player) -> TFD::PlayerDownRouter::ThresholdScan { return TFD::PlayerThresholdOutcomeWiring::ScanOutcome(player); };
		bleedLockDependencies.tryBeginThresholdNoThreatRescueFallback = [](RE::Actor* player, const TFD::PlayerDownRouter::ThresholdScan& scan, float thresholdPct, const char* reason) -> bool {
			return TFD::PlayerThresholdOutcomeWiring::TryBeginNoThreatRescueFallback(player, scan, thresholdPct, reason);
		};
		bleedLockDependencies.isDownedTeammateRecoveryDialogueHoldActor = [](RE::Actor* actor) -> bool {
			return TFD::TeammateManager::IsDownedTeammateRecoveryDialogueHoldActor(actor);
		};
		bleedLockDependencies.isPlayerBattleObserveActive = []() -> bool {
			return TFD::Bleedout::BleedBattleObservePendingRef() || TFD::Bleedout::BleedBattleObserveActiveRef();
		};
		bleedLockDependencies.resetPlayerDamageGuard = [](const char* reason) { TFD::PlayerDamageGuard::Reset(reason ? reason : "bleed_lock_runtime_clear"); };
		bleedLockDependencies.clearPendingOverkillRoute = [](const char* reason) { TFD::PlayerDownRouter::ClearPendingOverkillRoute(reason ? reason : "bleed_lock_runtime_clear"); };
		bleedLockDependencies.clearPreDeathShield = []() { TFD::PlayerOverkillDamageHook::ClearPreDeathShield("bleed_lock_clear", true); };
		bleedLockDependencies.resetEnemyThresholdScanTimer = []() { g_enemyThresholdScanLast = {}; };
		TFD::DefeatBleedLockWiring::InstallProvider(std::move(bleedLockDependencies));

		ResetBleedRuntimeState();
		TFD::DefeatFlowRefresh::Dependencies flowRefreshDependencies{};
		flowRefreshDependencies.getPlayer = []() -> RE::Actor* { return Player(); };
		flowRefreshDependencies.getSweepRadius = []() -> float { return TFD::Settings::GetSweepRadius(); };
		flowRefreshDependencies.collectStandingFollowers = [](float radius) { return CollectBleedStandingFollowers(radius); };
		flowRefreshDependencies.collectObservedEnemies = [](RE::Actor* player, float radius, const std::vector<RE::Actor*>& followers) {
			return CollectLiveStandingObservedEnemies(player, radius, nullptr, followers);
			};
		flowRefreshDependencies.resolveCurrentCombatTarget = [](RE::Actor* actor) -> RE::Actor* { return ResolveCurrentCombatTarget(actor); };
		flowRefreshDependencies.isActorActivelyTargetingPlayerSideForRouter = [](RE::Actor* actor, RE::Actor* player) -> bool {
			return IsActorActivelyTargetingPlayerSideForRouter(actor, player);
			};
		flowRefreshDependencies.isPlayerBleedRuntimeActive = []() -> bool { return g_inBleedState.load(std::memory_order_acquire); };
		flowRefreshDependencies.isBattleObserveHold = []() -> bool { return TFD::DefeatBattleObserveState::Pending() || TFD::DefeatBattleObserveState::Active(); };
		flowRefreshDependencies.isPlayerBleedLockActive = [](RE::Actor* player) -> bool {
			if (!player) {
				return false;
			}
			auto it = g_bleedLocks.find(player->GetFormID());
			return it != g_bleedLocks.end() && it->second.kind == BleedLockKind::Player;
			};
		TFD::DefeatFlowRefresh::InstallProvider(std::move(flowRefreshDependencies));

		TFD::DefeatCaptiveTickWiring::Dependencies captiveTickDependencies{};
		captiveTickDependencies.updatePreCombatState = []() { TFD::DefeatFlowRefresh::UpdatePreCombatState(); };
		captiveTickDependencies.isDialogueOpen = []() -> bool { return TFD::DialogueLifecycle::IsDialogueOpen(); };
		captiveTickDependencies.wasDialogueOpen = []() -> bool { return TFD::DialogueLifecycle::WasDialogueOpen(); };
		captiveTickDependencies.setDialogueOpenObserved = [](bool open) { TFD::DialogueLifecycle::SetDialogueOpenObserved(open); };
		captiveTickDependencies.clearLastAggressor = []() { TFD::DefeatAggressorResolver::ClearLastAggressor(); };
		captiveTickDependencies.setLastAggressor = [](RE::Actor* actor) { TFD::DefeatAggressorResolver::SetLastAggressor(actor); };
		captiveTickDependencies.resolveAggressor = []() -> RE::Actor* { return TFD::DefeatAggressorResolver::ResolveAggressor(); };
		captiveTickDependencies.findBestAggressor = [](float radius) -> RE::Actor* { return TFD::DefeatAggressorResolver::FindBestAggressor(radius); };
		captiveTickDependencies.setGraceActive = [](bool active) { g_grace.store(active, std::memory_order_release); };
		TFD::DefeatCaptiveTickWiring::InstallProvider(std::move(captiveTickDependencies));

		TFD::CaptiveRecaptureRecoveryWiring::Dependencies captiveRecaptureRecoveryDependencies{};
		captiveRecaptureRecoveryDependencies.getPlayer = []() -> RE::Actor* { return Player(); };
		captiveRecaptureRecoveryDependencies.releasePlayerBleedLock = [](const char* reason, bool playGetUp) {
			TFD::BleedLockRuntime::ReleasePlayer(TFD::DefeatBleedLockWiring::BuildContext(), reason, playGetUp);
			};
		captiveRecaptureRecoveryDependencies.restoreActorHealthToSafePct = [](RE::Actor* actor, float thresholdPct, float bonusPct, float minSafePct, float maxSafePct, float minAbsHp, const char* reason) {
			TFD::DefeatBleedLockWiring::RestoreActorHealthToSafePct(actor, thresholdPct, bonusPct, minSafePct, maxSafePct, minAbsHp, reason);
			};
		captiveRecaptureRecoveryDependencies.resetBleedRuntimeState = [](bool preserveCaptive) { ResetBleedRuntimeState(preserveCaptive); };
		captiveRecaptureRecoveryDependencies.forceStopBleedRuntimeForCaptiveRecapture = [](const char* reason) { TFD::Bleedout::ForceStopBleedRuntimeForCaptiveRecapture(reason); };
		captiveRecaptureRecoveryDependencies.setPlayerBleedImmune = [](bool immune) { SetPlayerBleedImmune(immune); };
		captiveRecaptureRecoveryDependencies.playPlayerGetUp = [](RE::Actor* actor) {
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				return;
			}
			actor->NotifyAnimationGraph("BleedoutStop");
			actor->NotifyAnimationGraph("GetUpStart");
			};
		captiveRecaptureRecoveryDependencies.refreshPostDefeatGlobals = []() { TFD::DefeatFlowRefresh::RefreshPostDefeatGlobals(); };
		captiveRecaptureRecoveryDependencies.getActorHealthPct = [](RE::Actor* actor) -> float { return GetActorHealthPct(actor); };
		captiveRecaptureRecoveryDependencies.getDefeatThresholdPct = []() -> float { return TFD::Settings::GetDefeatThresholdPct(); };
		captiveRecaptureRecoveryDependencies.isActorBleedingOut = [](RE::Actor* actor) -> bool { return IsActorBleedingOut(actor); };
		TFD::CaptiveRecaptureRecoveryWiring::InstallProvider(captiveRecaptureRecoveryDependencies);

		TFD::PlayerDownRouterOutcomeWiring::Dependencies outcomeRouteDependencies{};
		outcomeRouteDependencies.rememberAggressor = [](RE::Actor* actor) { TFD::DefeatAggressorResolver::RememberAggressor(actor); };
		outcomeRouteDependencies.findBleedoutSpeaker = [](float scanRadius, float maxDistance, RE::Actor* preferred) -> RE::Actor* {
			return TFD::BleedoutDialogueRuntime::FindBestSpeaker(scanRadius, maxDistance, preferred, BuildBleedoutDialogueRuntimeContext());
		};
		outcomeRouteDependencies.startBleedWindow = [](RE::Actor* player, RE::Actor* speaker) { StartBleedWindow(player, speaker); };
		outcomeRouteDependencies.startBattleObservePending = [](RE::Actor* player) -> bool { return TFD::Bleedout::RuntimeHost::StartBattleObservePending(player); };
		outcomeRouteDependencies.setGraceSeconds = [](int seconds) { SetGraceSeconds(seconds); };
		outcomeRouteDependencies.hasTerminalCommit = []() -> bool { return TFD::Bleedout::HasTerminalCommit(); };
		outcomeRouteDependencies.hasCachedRescueDestination = []() -> bool { return TFD::Location::ResolveMostRecentCachedRescueDestination(true) != nullptr; };
		outcomeRouteDependencies.hasPlayerBleedLock = []() -> bool { return HasPlayerBleedLock(); };
		outcomeRouteDependencies.enterPlayerBleedLock = [](RE::Actor* player, float thresholdPct, const char* reason) {
			TFD::BleedLockRuntime::Enter(TFD::DefeatBleedLockWiring::BuildContext(), player, BleedLockKind::Player, thresholdPct, reason);
		};
		outcomeRouteDependencies.releasePlayerBleedLock = [](const char* reason, bool playGetUp) {
			TFD::BleedLockRuntime::ReleasePlayer(TFD::DefeatBleedLockWiring::BuildContext(), reason, playGetUp);
		};
		outcomeRouteDependencies.setPlayerBleedImmune = [](bool immune) { SetPlayerBleedImmune(immune); };
		outcomeRouteDependencies.preparePlayer = [](RE::Actor* player) {
			if (player) {
				player->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
			}
		};
		outcomeRouteDependencies.executeNoMarkerFallback = [](const char* reason) -> bool {
			return TFD::DefeatRuntimeActions::ExecuteResolvedNoMarkerFallback(reason ? reason : "threshold_no_threat_rescue", BuildNoMarkerFallbackContext());
		};
		TFD::PlayerDownRouterOutcomeWiring::InstallProvider(std::move(outcomeRouteDependencies));

		TFD::PlayerDownRouterPendingWiring::Dependencies pendingRouteDependencies{};
		pendingRouteDependencies.getPlayer = []() -> RE::Actor* { return Player(); };
		pendingRouteDependencies.setPlayerBleedImmune = [](bool immune) { SetPlayerBleedImmune(immune); };
		pendingRouteDependencies.preparePlayer = [](RE::Actor* player) {
			if (!player) {
				return;
			}
			player->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
			if (player->IsDead(false)) {
				player->Resurrect(false, true);
			}
		};
		pendingRouteDependencies.clampHealth = [](RE::Actor* player, float minHealth) { ClampHealth(player, minHealth); };
		pendingRouteDependencies.resolveSafeFloorHealth = [](RE::Actor* player, float thresholdPct) -> float {
			return TFD::PlayerOverkillDamageHook::ResolveSafeFloorHealth(player, thresholdPct);
		};
		pendingRouteDependencies.hasPlayerBleedOwner = []() -> bool { return HasPlayerBleedLock() || g_inBleedState.load(std::memory_order_acquire); };
		pendingRouteDependencies.enterPlayerBleedLock = [](RE::Actor* player, float thresholdPct, const char* reason) {
			TFD::BleedLockRuntime::Enter(TFD::DefeatBleedLockWiring::BuildContext(), player, BleedLockKind::Player, thresholdPct, reason);
		};
		pendingRouteDependencies.isBleedDecisionActive = []() -> bool { return TFD::FlowController::Controller::GetSingleton().IsBleedDecisionActive(); };
		pendingRouteDependencies.scanThresholdOutcome = [](RE::Actor* player) -> TFD::PlayerDownRouter::ThresholdScan { return TFD::PlayerThresholdOutcomeWiring::ScanOutcome(player); };
		pendingRouteDependencies.isObserverAlly = [](RE::Actor* actor) -> bool { return IsObserverAlly(actor); };
		pendingRouteDependencies.rememberAggressor = [](RE::Actor* actor) { TFD::DefeatAggressorResolver::RememberAggressor(actor); };
		pendingRouteDependencies.getHealthPct = [](RE::Actor* actor) -> float { return GetActorHealthPct(actor); };
		pendingRouteDependencies.buildDispatchContext = []() -> TFD::PlayerDownRouterWiring::DispatchContext { return TFD::PlayerDownRouterOutcomeWiring::BuildDispatchContext(); };
		TFD::PlayerDownRouterPendingWiring::InstallProvider(std::move(pendingRouteDependencies));
		// R451A/R460A: true overkill block plus lifecycle-gated player killmove suppression.
		// Clamp player Health damage in CheckClampDamageModifier before it can drop
		// below the safe floor, and keep paired killmoves from entering death state without
		// re-arming the guard from non-combat Health writes after terminal cleanup.
		TFD::PlayerOverkillDamageHook::Context playerOverkillHookContext{};
		playerOverkillHookContext.getPlayer = []() -> RE::Actor* { return Player(); };
		playerOverkillHookContext.resolveCachedAttacker = []() -> RE::Actor* { return TFD::DefeatAggressorResolver::ResolveCachedPlayerOverkillAttacker(); };
		playerOverkillHookContext.hasPendingOverkillRoute = []() -> bool { return TFD::PlayerDownRouter::HasPendingOverkillRoute(); };
		playerOverkillHookContext.hasPlayerBleedLock = []() -> bool { return HasPlayerBleedLock(); };
		playerOverkillHookContext.isInBleedState = []() -> bool { return g_inBleedState.load(std::memory_order_acquire); };
		playerOverkillHookContext.hasRecentEnemyTargetingPlayer = [](double maxAgeSec) -> bool { return TFD::DefeatAggressorResolver::HasRecentEnemyTargetingPlayer(maxAgeSec); };
		playerOverkillHookContext.isObserverAlly = [](RE::Actor* actor) -> bool { return IsObserverAlly(actor); };
		playerOverkillHookContext.setPlayerBleedImmune = [](bool enable) { SetPlayerBleedImmune(enable); };
		playerOverkillHookContext.clampHealth = [](RE::Actor* actor, float minHp) { ClampHealth(actor, minHp); };
		playerOverkillHookContext.noteEnemyTargetingPlayer = [](RE::Actor* actor) { TFD::DefeatAggressorResolver::NoteEnemyTargetingPlayer(actor); };
		playerOverkillHookContext.isPreDeathShieldActive = []() -> bool { return TFD::PlayerOverkillDamageHook::IsPreDeathShieldActive(); };
		playerOverkillHookContext.resolveBleedRuntimeSafeHealth = [](RE::Actor* actor, float thresholdPct) -> float {
			return ResolvePlayerBleedRuntimeSafeHealth(actor, thresholdPct);
			};
		playerOverkillHookContext.rememberAggressor = [](RE::Actor* actor) { TFD::DefeatAggressorResolver::RememberAggressor(actor); };
		playerOverkillHookContext.getDefeatThresholdPct = []() -> float { return TFD::Settings::GetDefeatThresholdPct(); };
		playerOverkillHookContext.getActorHealthPct = [](RE::Actor* actor) -> float { return GetActorHealthPct(actor); };
		playerOverkillHookContext.resolveAggressor = []() -> RE::Actor* { return TFD::DefeatAggressorResolver::ResolveAggressor(); };
		playerOverkillHookContext.resolveLastEnemyTargetingPlayer = [](float radius, double maxAgeSec) -> RE::Actor* { return TFD::DefeatAggressorResolver::ResolveLastEnemyTargetingPlayer(radius, maxAgeSec); };
		playerOverkillHookContext.enterPlayerBleedLock = [](RE::Actor* player, float thresholdPct, const char* reason) {
			TFD::BleedLockRuntime::Enter(TFD::DefeatBleedLockWiring::BuildContext(), player, BleedLockKind::Player, thresholdPct, reason);
		};
		playerOverkillHookContext.dispatchThresholdScanImmediateBleedout = [](RE::Actor* player, float hpPct, float thresholdPct, const char* reason) -> bool {
			return TFD::PlayerThresholdOutcomeWiring::DispatchImmediateBleedout(player, hpPct, thresholdPct, reason);
		};
		playerOverkillHookContext.tryBeginThresholdNoThreatRescueFallback = [](RE::Actor* player, float thresholdPct, const char* reason) -> bool {
			return TFD::PlayerThresholdOutcomeWiring::TryBeginNoThreatRescueFallback(player, thresholdPct, reason);
		};
		TFD::PlayerOverkillDamageHook::SetContext(std::move(playerOverkillHookContext));
		TFD::PlayerOverkillDamageHook::Install();
		spdlog::info("[TFD][Defeat][R460A] player killmove guard lifecycle gated mode=combat_health_damage_or_hit_event");
		TFD::FlowController::Controller::GetSingleton().ResetRuntime("defeat_install");
		TFD::DefeatBleedoutLifecycleWiring::Dependencies bleedoutLifecycleDependencies{};
		bleedoutLifecycleDependencies.inBleedState = &g_inBleedState;
		bleedoutLifecycleDependencies.minHp = &g_minHp;
		bleedoutLifecycleDependencies.bleedPendingCaptiveOutcome = &g_bleedPendingCaptiveOutcome;
		bleedoutLifecycleDependencies.bleedPendingNonCaptiveOutcome = &g_bleedPendingNonCaptiveOutcome;
		bleedoutLifecycleDependencies.graceActive = &g_grace;
		bleedoutLifecycleDependencies.graceUntil = &g_graceUntil;
		bleedoutLifecycleDependencies.previousDialogueOpen = TFD::DialogueLifecycle::PreviousOpenStorage();
		bleedoutLifecycleDependencies.getPlayer = []() -> RE::Actor* { return Player(); };
		bleedoutLifecycleDependencies.setGraceSeconds = [](int seconds) { SetGraceSeconds(seconds); };
		bleedoutLifecycleDependencies.setPlayerBleedImmune = [](bool immune) { SetPlayerBleedImmune(immune); };
		bleedoutLifecycleDependencies.clampHealth = [](RE::Actor* actor, float minHp) { ClampHealth(actor, minHp); };
		bleedoutLifecycleDependencies.resetBleedRuntimeState = [](bool preserve) { ResetBleedRuntimeState(preserve); };
		bleedoutLifecycleDependencies.transitionBleedRuntimeToPleasureCommit = [](const char* reason) { TransitionBleedRuntimeToPleasureCommit(reason); };
		bleedoutLifecycleDependencies.buildTransitionRuntimeHandlers = []() { return BuildTransitionRuntimeHandlers(); };
		bleedoutLifecycleDependencies.buildBleedoutSpeakerHandlers = []() { return BuildBleedoutSpeakerHandlers(); };
		bleedoutLifecycleDependencies.buildBleedoutDialogueRuntimeContext = []() { return BuildBleedoutDialogueRuntimeContext(); };
		bleedoutLifecycleDependencies.resolveBleedFlowActorFormID = []() { return ResolveBleedFlowActorFormID(); };
		bleedoutLifecycleDependencies.collectBleedStandingFollowers = [](float radius) { return CollectBleedStandingFollowers(radius); };
		bleedoutLifecycleDependencies.collectBleedoutCrowd = [](float radius, RE::Actor* preferred, bool preserveAssigned) { return CollectBleedoutCrowd(radius, preferred, preserveAssigned); };
		bleedoutLifecycleDependencies.isBleedSpaceCompatible = [](RE::Actor* actor, RE::Actor* player) { return IsBleedSpaceCompatible(actor, player); };
		bleedoutLifecycleDependencies.isObserverAlly = [](RE::Actor* actor) { return IsObserverAlly(actor); };
		bleedoutLifecycleDependencies.isStandingAllyThresholdActor = [](RE::Actor* actor) { return IsStandingAllyThresholdActor(actor); };
		bleedoutLifecycleDependencies.isStandingEnemyThresholdActor = [](RE::Actor* actor) { return IsStandingEnemyThresholdActor(actor); };
		TFD::DefeatBleedoutLifecycleWiring::InstallProvider(std::move(bleedoutLifecycleDependencies));
		TFD::DefeatFlowLifecycleWiring::Dependencies flowLifecycleDependencies{};
		flowLifecycleDependencies.inBleedState = []() -> bool { return g_inBleedState.load(std::memory_order_acquire); };
		flowLifecycleDependencies.resolveBleedFlowActorFormID = []() -> std::uint32_t { return ResolveBleedFlowActorFormID(); };
		flowLifecycleDependencies.resetBleedRuntimeState = []() { ResetBleedRuntimeState(); };
		flowLifecycleDependencies.setPlayerBleedImmune = [](bool immune) { SetPlayerBleedImmune(immune); };
		flowLifecycleDependencies.buildNoMarkerFallbackContext = []() { return BuildNoMarkerFallbackContext(); };
		flowLifecycleDependencies.resolveObservedDownedFollower = []() -> RE::Actor* {
			const float followerRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 400.0f);
			auto followers = ResolveFollowerCandidates(followerRadius);
			return followers.downed;
		};
		flowLifecycleDependencies.buildTransitionRuntimeHandlers = []() { return BuildTransitionRuntimeHandlers(); };
		TFD::DefeatFlowLifecycleWiring::InstallProvider(std::move(flowLifecycleDependencies));
		TFD::DefeatLifecycleBootstrapWiring::Dependencies lifecycleBootstrapDependencies{};
		lifecycleBootstrapDependencies.buildBleedoutProviders = []() { return TFD::DefeatBleedoutLifecycleWiring::BuildProviders(); };
		lifecycleBootstrapDependencies.buildFlowControllerProviders = []() { return TFD::DefeatFlowLifecycleWiring::BuildProviders(); };
		TFD::DefeatLifecycleBootstrapWiring::Install(lifecycleBootstrapDependencies);

		TFD::DefeatRuntimeProviderWiring::Install();
		TFD::PleasureRuntime::Install();
		TFD::DefeatFlowRefresh::ResetRouterCombatContext();
		TFD::BleedLockRuntime::ClearAll(TFD::DefeatBleedLockWiring::BuildContext(), "install");
		TFD::Bleedout::ClearBridgeAliases(nullptr, "install");
		TFD::Actor::Ops::Initialize();
		TFD::Actor::Ops::InstallDefeatedEnemyQueryHooks(TFD::Actor::Ops::DefeatedEnemyQueryHooks{
			&IsTrackedDefeatedEnemyHook,
			&TFD::DefeatAggressorResolver::IsLastAggressor
			});
		TFD::Location::Initialize();
		if (auto* scripts = RE::ScriptEventSourceHolder::GetSingleton()) {
			scripts->AddEventSink<RE::TESHitEvent>(&g_passiveBreakEventSink);
		}
		if (auto* src = SKSE::GetModCallbackEventSource()) {
			src->AddEventSink(&g_bleedOutcomeEventSink);
			src->AddEventSink(&g_passiveBreakEventSink);
		}
		g_worker = std::thread([]() { WorkerLoop(); });
		TFD::DefeatMonitor::ApplyQueuedProgressState();
		SetRescueStateValue(0);
		TFD::DefeatFlowRefresh::RefreshPostDefeatGlobals();
		spdlog::info("[TFD][Defeat] monitor installed");
	}

	void Shutdown()
	{
		if (!g_installed.exchange(false, std::memory_order_acq_rel)) return;
		g_running.store(false, std::memory_order_release);
		if (g_worker.joinable()) g_worker.join();
		TFD::CaptiveRecaptureRecoveryWiring::ShutdownProvider();
		TFD::PlayerDownRouterPendingWiring::ShutdownProvider();
		TFD::PlayerDownRouterOutcomeWiring::ShutdownProvider();
		TFD::PlayerThresholdOutcomeWiring::ShutdownProvider();
		TFD::DefeatCaptiveTickWiring::ShutdownProvider();
		TFD::DefeatFlowRefresh::ShutdownProvider();
		TFD::PlayerOverkillDamageHook::ClearContext();
		TFD::Bleedout::DefeatGlue::ClearCaptiveOrchestrationResidue(false);
		g_hasQueuedProgressState = false;
		TFD::Captive::ClearQueuedLoadedState();
		g_queuedBleedOutState = false;
		SetPlayerBleedImmune(false);
		ResetBleedRuntimeState();
		TFD::FlowController::Controller::GetSingleton().ResetRuntime("defeat_shutdown");
		TFD::FlowController::ResetDefeatLifecycleProviders();
		TFD::TeammateManager::ResetRuntimeProviders();
		TFD::Tame::ResetRuntimeProviders();
		TFD::HostilityController::ResetBleedTruceRuntimeProviders();
		TFD::DefeatFlowRefresh::ResetRouterCombatContext();
		TFD::BleedLockRuntime::ClearAll(TFD::DefeatBleedLockWiring::BuildContext(), "shutdown");
		TFD::DefeatBleedLockWiring::ShutdownProvider();
		TFD::DefeatAggressorResolver::ShutdownProvider();
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
		SetRescueStateValue(0);
		TFD::DefeatFlowRefresh::RefreshPostDefeatGlobals();
		TFD::Bleedout::ResetDefeatLifecycleProviders();
		TFD::DefeatBleedoutLifecycleWiring::ShutdownProvider();
		TFD::DefeatFlowLifecycleWiring::ShutdownProvider();
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
			[]() { return TFD::DialogueLifecycle::IsDialogueOpen(); },
			[](bool open) { TFD::DialogueLifecycle::SetDialogueOpenObserved(open); }
			});
		SetPlayerBleedImmune(false);
		TFD::PlayerDamageGuard::Reset("apply_queued_state");
		TFD::Bleedout::ClearBridgeAliases(nullptr, "apply_queued_state");
		SetRescueStateValue(0);
		TFD::DefeatFlowRefresh::RefreshPostDefeatGlobals();
		TFD::DefeatFlowRefresh::UpdatePreCombatState();
		spdlog::info("[TFD][Defeat] ApplyQueuedProgressState state={} phase={} bleed={}", TFD::Captive::GetQueuedStateFlag() ? 1 : 0, static_cast<int>(TFD::Captive::GetQueuedPhase()), g_queuedBleedOutState ? 1 : 0);
	}


	void ResetForLoad()
	{
		g_grace.store(false, std::memory_order_release);
		TFD::CaptiveRecaptureRecoveryWiring::ResetPulse();
		SetPlayerBleedImmune(false);
		ResetBleedRuntimeState();
		TFD::BleedLockRuntime::ClearAll(TFD::DefeatBleedLockWiring::BuildContext(), "reset_for_load");
		TFD::Bleedout::ClearBridgeAliases(nullptr, "reset_for_load");
		TFD::DefeatAggressorResolver::ResetForLoad();
		TFD::Bleedout::DefeatGlue::ClearCaptiveOrchestrationResidue(false);
		TFD::HostilityController::ClearAggressionClamp();
		TFD::Transition::ClearLeftForDeadCooldown(BuildTransitionRuntimeHandlers());
		SetRescueStateValue(0);
		TFD::DefeatFlowRefresh::ResetRouterCombatContext();
		TFD::PleasureRuntime::ResetForLoad("defeat_reset_for_load");
		TFD::DefeatFlowRefresh::RefreshPostDefeatGlobals();
		spdlog::info("[TFD][Defeat] ResetForLoad -> runtime only");
	}

	void SetLoadTransition(bool active)
	{
		g_loadTransition.store(active, std::memory_order_release);
		if (active) {
			SetPlayerBleedImmune(false);
			TFD::BleedLockRuntime::ClearAll(TFD::DefeatBleedLockWiring::BuildContext(), "set_load_transition");
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


}
