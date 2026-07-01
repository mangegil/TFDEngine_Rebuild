#include "TFDDefeatMonitorBootstrapWiring.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include <spdlog/spdlog.h>

#include "TFDActor.h"
#include "TFDBleedout.h"
#include "TFDBleedoutGreet.h"
#include "TFDDefeatAggressorResolver.h"
#include "TFDDefeatBleedoutDialogueWiring.h"
#include "TFDDefeatBleedRuntimeState.h"
#include "TFDDefeatReleaseGrace.h"
#include "TFDDefeatBattleObserveState.h"
#include "TFDDefeatBleedHealthGuard.h"
#include "TFDDefeatBleedLockWiring.h"
#include "TFDDefeatCaptiveTickWiring.h"
#include "TFDDefeatFlowRefresh.h"
#include "TFDDialogueLifecycle.h"
#include "TFDBleedLockRuntime.h"
#include "TFDBleedLockState.h"
#include "TFDPlayerDamageGuard.h"
#include "TFDPlayerDownRouter.h"
#include "TFDPlayerDownRouterPendingWiring.h"
#include "TFDPlayerDownRouterOutcomeWiring.h"
#include "TFDPlayerDownRouterWiring.h"
#include "TFDLocation.h"
#include "TFDDefeatRuntimeActions.h"
#include "TFDDefeatRuntimeProviderWiring.h"
#include "TFDDefeatLifecycleBootstrapWiring.h"
#include "TFDDefeatBleedoutLifecycleWiring.h"
#include "TFDDefeatFlowLifecycleWiring.h"
#include "TFDFlowController.h"
#include "TFDPlayerOverkillDamageHook.h"
#include "TFDCaptiveRecaptureRecoveryWiring.h"
#include "TFDSettings.h"
#include "TFDDefeatTransitionWiring.h"
#include "TFDPlayerThresholdOutcomeWiring.h"
#include "TFDPlayerBleedImmunityGuard.h"
#include "TFDTeammateManager.h"
#include "TFDTransition.h"
#include "TFDDefeatThresholdSensor.h"
#include "TFDDefeatBleedRuntimeResetWiring.h"
#include "TFDDefeatActorQueries.h"

namespace TFD::DefeatMonitorBootstrapWiring
{
	namespace
	{
		RE::Actor* PlayerActor(const Dependencies& dependencies)
		{
			if (dependencies.getPlayerActor) {
				return dependencies.getPlayerActor();
			}
			if (dependencies.getPlayerCharacter) {
				return dependencies.getPlayerCharacter();
			}
			return RE::PlayerCharacter::GetSingleton();
		}
	}

	Dependencies BuildDefaultDependencies()
	{
		Dependencies dependencies{};
		dependencies.getPlayerCharacter = []() -> RE::PlayerCharacter* { return TFD::DefeatActorQueries::Player(); };
		dependencies.getPlayerActor = []() -> RE::Actor* { return TFD::DefeatActorQueries::Player(); };
		dependencies.isActiveFollowerActor = [](RE::Actor* actor) -> bool { return TFD::DefeatActorQueries::IsActiveFollowerActor(actor); };
		dependencies.isStandingEnemyThresholdActor = [](RE::Actor* actor) -> bool { return TFD::DefeatActorQueries::IsStandingEnemyThresholdActor(actor); };
		dependencies.isStandingAllyThresholdActor = [](RE::Actor* actor) -> bool { return TFD::DefeatActorQueries::IsStandingAllyThresholdActor(actor); };
		dependencies.isObserverAlly = [](RE::Actor* actor) -> bool { return TFD::DefeatActorQueries::IsObserverAlly(actor); };
		dependencies.isBleedSpaceCompatible = [](RE::Actor* actor, RE::Actor* player) -> bool { return TFD::DefeatActorQueries::IsBleedSpaceCompatible(actor, player); };
		dependencies.hasLineOfSightToPlayer = [](RE::Actor* actor, RE::Actor* player) -> bool { return TFD::DefeatActorQueries::ActorHasLineOfSightToPlayer(actor, player); };
		dependencies.isActorCloseAndFront = [](RE::Actor* actor, RE::Actor* player, float maxDist) -> bool { return TFD::DefeatActorQueries::IsActorCloseAndFront(actor, player, maxDist); };
		dependencies.resolveCurrentCombatTarget = [](RE::Actor* actor) -> RE::Actor* { return TFD::DefeatActorQueries::ResolveCurrentCombatTarget(actor); };
		dependencies.resetBleedRuntimeState = []() { TFD::DefeatBleedRuntimeResetWiring::Reset(); };
		dependencies.resetBleedRuntimeStatePreserve = [](bool preserveCaptive) { TFD::DefeatBleedRuntimeResetWiring::Reset(preserveCaptive); };
		dependencies.collectBleedStandingFollowers = [](float radius) { return TFD::DefeatActorQueries::CollectBleedStandingFollowers(radius); };
		dependencies.getActorHealthPct = [](RE::Actor* actor) -> float { return TFD::DefeatActorQueries::GetActorHealthPct(actor); };
		dependencies.isActorBleedingOut = [](RE::Actor* actor) -> bool { return TFD::DefeatActorQueries::IsActorBleedingOut(actor); };
		dependencies.hasPlayerBleedLock = []() -> bool { return TFD::DefeatActorQueries::HasPlayerBleedLock(); };
		dependencies.resetEnemyThresholdScanTimer = []() { TFD::DefeatThresholdSensor::ResetEnemyTimer(); };
		dependencies.collectObservedEnemies = [](RE::Actor* player, float radius, const std::vector<RE::Actor*>& followers) {
			return TFD::DefeatActorQueries::CollectLiveStandingObservedEnemies(player, radius, nullptr, followers);
		};
		dependencies.isActorActivelyTargetingPlayerSideForRouter = [](RE::Actor* actor, RE::Actor* player) -> bool {
			return TFD::DefeatActorQueries::IsActorActivelyTargetingPlayerSideForRouter(actor, player);
		};
		dependencies.isPlayerBleedLockActive = [](RE::Actor* player) -> bool {
			return TFD::DefeatActorQueries::IsPlayerBleedLockActive(player);
		};
		dependencies.resolveBleedFlowActorFormID = []() -> std::uint32_t { return TFD::DefeatActorQueries::ResolveBleedFlowActorFormID(); };
		dependencies.transitionBleedRuntimeToPleasureCommit = [](const char* reason) { TFD::DefeatBleedRuntimeResetWiring::TransitionToPleasureCommit(reason); };
		dependencies.resolveObservedDownedFollower = []() -> RE::Actor* {
			const float followerRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 400.0f);
			auto followers = TFD::DefeatActorQueries::ResolveFollowerCandidates(followerRadius);
			return followers.downed;
		};
		return dependencies;
	}

	void InstallEarlyProviders(Dependencies dependencies)
	{
		TFD::DefeatAggressorResolver::Dependencies aggressorResolverDependencies{};
		aggressorResolverDependencies.getPlayer = [dependencies]() -> RE::PlayerCharacter* {
			return dependencies.getPlayerCharacter ? dependencies.getPlayerCharacter() : RE::PlayerCharacter::GetSingleton();
		};
		aggressorResolverDependencies.isActiveFollowerActor = [dependencies](RE::Actor* actor) -> bool {
			return dependencies.isActiveFollowerActor ? dependencies.isActiveFollowerActor(actor) : false;
		};
		aggressorResolverDependencies.isStandingEnemyThresholdActor = [dependencies](RE::Actor* actor) -> bool {
			return dependencies.isStandingEnemyThresholdActor ? dependencies.isStandingEnemyThresholdActor(actor) : false;
		};
		aggressorResolverDependencies.isObserverAlly = [dependencies](RE::Actor* actor) -> bool {
			return dependencies.isObserverAlly ? dependencies.isObserverAlly(actor) : false;
		};
		TFD::DefeatAggressorResolver::InstallProvider(std::move(aggressorResolverDependencies));

		TFD::DefeatTransitionWiring::Dependencies transitionDependencies{};
		transitionDependencies.getPlayer = [dependencies]() -> RE::Actor* { return PlayerActor(dependencies); };
		transitionDependencies.resolveAggressor = []() -> RE::Actor* { return TFD::DefeatAggressorResolver::ResolveAggressor(); };
		transitionDependencies.findBestAggressor = [](float radius) -> RE::Actor* { return TFD::DefeatAggressorResolver::FindBestAggressor(radius); };
		transitionDependencies.isCombatSupportedAggressor = [](RE::Actor* actor) -> bool { return TFD::DefeatAggressorResolver::IsCombatSupportedAggressor(actor); };
		transitionDependencies.isActiveFollowerActor = [](RE::Actor* actor) -> bool { return TFD::TeammateManager::IsActiveFollowerActor(actor); };
		transitionDependencies.isStandingAllyThresholdActor = [dependencies](RE::Actor* actor) -> bool {
			return dependencies.isStandingAllyThresholdActor ? dependencies.isStandingAllyThresholdActor(actor) : false;
		};
		transitionDependencies.collectRegisteredTeammates = []() { return TFD::TeammateManager::CollectRegisteredTeammates(); };
		transitionDependencies.collectBleedoutCrowd = [](float radius, RE::Actor* preferred, bool preserveAssigned) {
			return TFD::DefeatBleedoutDialogueWiring::CollectCrowd(radius, preferred, preserveAssigned);
		};
		transitionDependencies.setGraceSeconds = [](int seconds) { TFD::DefeatReleaseGrace::SetSeconds(seconds); };
		transitionDependencies.setPlayerBleedImmune = [](bool immune) { TFD::PlayerBleedImmunityGuard::SetPlayerActive(immune); };
		transitionDependencies.resetBleedRuntimeState = [dependencies]() {
		};
		TFD::DefeatTransitionWiring::InstallProvider(std::move(transitionDependencies));

		TFD::DefeatReleaseGrace::Dependencies releaseGraceDependencies{};
		releaseGraceDependencies.clearLeftForDeadCooldown = []() {
			TFD::Transition::ClearLeftForDeadCooldown(TFD::DefeatTransitionWiring::BuildTransitionRuntimeHandlers());
		};
		releaseGraceDependencies.clearAllReleaseFollowGrace = [](const char* reason) {
			TFD::Actor::Ops::ClearAllReleaseFollowGrace(reason ? reason : "reset_grace");
		};
		TFD::DefeatReleaseGrace::InstallProvider(std::move(releaseGraceDependencies));

		TFD::DefeatBleedoutDialogueWiring::Dependencies bleedoutDialogueDependencies{};
		bleedoutDialogueDependencies.getPlayer = [dependencies]() -> RE::Actor* { return PlayerActor(dependencies); };
		bleedoutDialogueDependencies.isStandingEnemyThresholdActor = [dependencies](RE::Actor* actor) -> bool {
			return dependencies.isStandingEnemyThresholdActor ? dependencies.isStandingEnemyThresholdActor(actor) : false;
		};
		bleedoutDialogueDependencies.isBleedSpaceCompatible = [dependencies](RE::Actor* actor, RE::Actor* player) -> bool {
			return dependencies.isBleedSpaceCompatible ? dependencies.isBleedSpaceCompatible(actor, player) : false;
		};
		bleedoutDialogueDependencies.hasLineOfSightToPlayer = [dependencies](RE::Actor* actor, RE::Actor* player) -> bool {
			return dependencies.hasLineOfSightToPlayer ? dependencies.hasLineOfSightToPlayer(actor, player) : false;
		};
		bleedoutDialogueDependencies.isActorCloseAndFront = [dependencies](RE::Actor* actor, RE::Actor* player, float maxDist) -> bool {
			return dependencies.isActorCloseAndFront ? dependencies.isActorCloseAndFront(actor, player, maxDist) : false;
		};
		bleedoutDialogueDependencies.resolveCurrentCombatTarget = [dependencies](RE::Actor* actor) -> RE::Actor* {
			return dependencies.resolveCurrentCombatTarget ? dependencies.resolveCurrentCombatTarget(actor) : nullptr;
		};
		bleedoutDialogueDependencies.isActiveFollowerActor = [dependencies](RE::Actor* actor) -> bool {
			return dependencies.isActiveFollowerActor ? dependencies.isActiveFollowerActor(actor) : false;
		};
		TFD::DefeatBleedoutDialogueWiring::InstallProvider(std::move(bleedoutDialogueDependencies));

		TFD::PlayerThresholdOutcomeWiring::Dependencies playerThresholdOutcomeDependencies{};
		playerThresholdOutcomeDependencies.collectStandingFollowers = [dependencies](float radius) {
			return dependencies.collectBleedStandingFollowers ? dependencies.collectBleedStandingFollowers(radius) : std::vector<RE::Actor*>{};
		};
		playerThresholdOutcomeDependencies.isObserverAlly = [dependencies](RE::Actor* actor) -> bool {
			return dependencies.isObserverAlly ? dependencies.isObserverAlly(actor) : false;
		};
		playerThresholdOutcomeDependencies.isInBleedState = []() -> bool { return TFD::DefeatBleedRuntimeState::IsInBleedState(); };
		playerThresholdOutcomeDependencies.releaseStaleEscapeBreakBleedRuntime = [](const char* reason) {
			TFD::DefeatBleedRuntimeState::SetInBleedState(false);
			TFD::BleedoutGreet::ResetRuntime(reason ? reason : "r270_stale_escape_break_rebleed");
			TFD::Bleedout::ClearDialogueOutcome(reason ? reason : "r270_stale_escape_break_rebleed");
		};
		TFD::PlayerThresholdOutcomeWiring::InstallProvider(std::move(playerThresholdOutcomeDependencies));

		spdlog::info("[TFD][Defeat][P29C] early bootstrap providers installed");
	}

	void InstallCoreRuntimeProviders(Dependencies dependencies)
	{
		TFD::DefeatBleedLockWiring::Dependencies bleedLockDependencies{};
		bleedLockDependencies.getPlayer = [dependencies]() -> RE::Actor* { return PlayerActor(dependencies); };
		bleedLockDependencies.resolveAggressor = []() -> RE::Actor* { return TFD::DefeatAggressorResolver::ResolveAggressor(); };
		bleedLockDependencies.setPlayerBleedImmune = [](bool enable) { TFD::PlayerBleedImmunityGuard::SetPlayerActive(enable); };
		bleedLockDependencies.getMinHp = []() -> float { return TFD::DefeatBleedRuntimeState::MinHp(); };
		bleedLockDependencies.setMinHp = [](float value) { TFD::DefeatBleedRuntimeState::MinHp() = value; };
		bleedLockDependencies.maxMinHp = [](float value) { TFD::DefeatBleedRuntimeState::MinHp() = (std::max)(TFD::DefeatBleedRuntimeState::MinHp(), value); };
		bleedLockDependencies.resetPreDeathShield = []() { TFD::PlayerOverkillDamageHook::ClearPreDeathShield("bleed_lock_reset", false); };
		bleedLockDependencies.isInBleedState = []() -> bool { return TFD::DefeatBleedRuntimeState::IsInBleedState(); };
		bleedLockDependencies.resolvePlayerBleedRuntimeSafeHealth = [](RE::Actor* actor, float thresholdPct) -> float {
			return TFD::DefeatBleedHealthGuard::ResolvePlayerBleedRuntimeSafeHealth(actor, thresholdPct);
			};
		bleedLockDependencies.buildPlayerDamageGuardConfig = [](RE::Actor* actor, float thresholdPct, float protectedHp, float safeFloorHp) {
			return TFD::DefeatBleedHealthGuard::BuildPlayerDamageGuardConfig(actor, thresholdPct, protectedHp, safeFloorHp);
			};
		bleedLockDependencies.clampHealth = [](RE::Actor* actor, float minHp) { TFD::DefeatBleedHealthGuard::ClampHealth(actor, minHp); };
		bleedLockDependencies.clampHealthCeiling = [](RE::Actor* actor, float maxHp) { TFD::DefeatBleedHealthGuard::ClampHealthCeiling(actor, maxHp); };
		bleedLockDependencies.resolveActorHealthForPct = [](RE::Actor* actor, float pct) -> float { return TFD::DefeatBleedHealthGuard::ResolveActorHealthForPct(actor, pct); };
		bleedLockDependencies.getActorHealthPct = [dependencies](RE::Actor* actor) -> float {
			return dependencies.getActorHealthPct ? dependencies.getActorHealthPct(actor) : 100.0f;
			};
		bleedLockDependencies.isActorBleedingOut = [dependencies](RE::Actor* actor) -> bool {
			return dependencies.isActorBleedingOut ? dependencies.isActorBleedingOut(actor) : false;
			};
		bleedLockDependencies.logReferenceBleedState = [](RE::Actor* actor, const TFD::DefeatBleedLockWiring::Entry& entry, const char* point, const char* note) {
			TFD::DefeatBleedHealthGuard::LogReferenceBleedState(actor, entry, point, note);
			};
		bleedLockDependencies.maybeLogReferenceBleedSamples = [](RE::Actor* actor, TFD::DefeatBleedLockWiring::Entry& entry, std::chrono::steady_clock::time_point now) {
			TFD::DefeatBleedHealthGuard::MaybeLogReferenceBleedSamples(actor, entry, now);
			};
		bleedLockDependencies.enforcePlayerBleedInvulnerability = [](RE::Actor* actor, TFD::DefeatBleedLockWiring::Entry& entry) { TFD::DefeatBleedHealthGuard::EnforcePlayerBleedInvulnerability(actor, entry); };
		bleedLockDependencies.tickPlayerKillmoveSuppression = []() { TFD::PlayerOverkillDamageHook::TickKillmoveSuppression(); };
		bleedLockDependencies.tickPlayerOverkillBlockPending = []() { TFD::PlayerDownRouterPendingWiring::TickPendingOverkillRoute(); };
		bleedLockDependencies.tickPlayerPreDeathShield = []() { TFD::PlayerOverkillDamageHook::TickPreDeathShield(); };
		bleedLockDependencies.scanBleedLockCandidates = []() { TFD::DefeatBleedLockWiring::ScanCandidates(); };
		bleedLockDependencies.findBestAggressor = [](float radius) -> RE::Actor* { return TFD::DefeatAggressorResolver::FindBestAggressor(radius); };
		bleedLockDependencies.isActiveFollowerActor = [dependencies](RE::Actor* actor) -> bool {
			return dependencies.isActiveFollowerActor ? dependencies.isActiveFollowerActor(actor) : false;
			};
		bleedLockDependencies.hasPlayerBleedLock = [dependencies]() -> bool {
			return dependencies.hasPlayerBleedLock ? dependencies.hasPlayerBleedLock() : false;
			};
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
		bleedLockDependencies.resetEnemyThresholdScanTimer = [dependencies]() {
			if (dependencies.resetEnemyThresholdScanTimer) {
				dependencies.resetEnemyThresholdScanTimer();
			}
			};
		TFD::DefeatBleedLockWiring::InstallProvider(std::move(bleedLockDependencies));

		if (dependencies.resetBleedRuntimeStatePreserve) {
			dependencies.resetBleedRuntimeStatePreserve(false);
		}
		else if (dependencies.resetBleedRuntimeState) {
			dependencies.resetBleedRuntimeState();
		}

		TFD::DefeatFlowRefresh::Dependencies flowRefreshDependencies{};
		flowRefreshDependencies.getPlayer = [dependencies]() -> RE::Actor* { return PlayerActor(dependencies); };
		flowRefreshDependencies.getSweepRadius = []() -> float { return TFD::Settings::GetSweepRadius(); };
		flowRefreshDependencies.collectStandingFollowers = [dependencies](float radius) {
			return dependencies.collectBleedStandingFollowers ? dependencies.collectBleedStandingFollowers(radius) : std::vector<RE::Actor*>{};
			};
		flowRefreshDependencies.collectObservedEnemies = [dependencies](RE::Actor* player, float radius, const std::vector<RE::Actor*>& followers) {
			return dependencies.collectObservedEnemies ? dependencies.collectObservedEnemies(player, radius, followers) : std::vector<RE::Actor*>{};
			};
		flowRefreshDependencies.resolveCurrentCombatTarget = [dependencies](RE::Actor* actor) -> RE::Actor* {
			return dependencies.resolveCurrentCombatTarget ? dependencies.resolveCurrentCombatTarget(actor) : nullptr;
			};
		flowRefreshDependencies.isActorActivelyTargetingPlayerSideForRouter = [dependencies](RE::Actor* actor, RE::Actor* player) -> bool {
			return dependencies.isActorActivelyTargetingPlayerSideForRouter ? dependencies.isActorActivelyTargetingPlayerSideForRouter(actor, player) : false;
			};
		flowRefreshDependencies.isPlayerBleedRuntimeActive = []() -> bool { return TFD::DefeatBleedRuntimeState::IsInBleedState(); };
		flowRefreshDependencies.isBattleObserveHold = []() -> bool { return TFD::DefeatBattleObserveState::Pending() || TFD::DefeatBattleObserveState::Active(); };
		flowRefreshDependencies.isPlayerBleedLockActive = [dependencies](RE::Actor* player) -> bool {
			return dependencies.isPlayerBleedLockActive ? dependencies.isPlayerBleedLockActive(player) : false;
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
		captiveTickDependencies.setGraceActive = [](bool active) { TFD::DefeatReleaseGrace::SetActive(active); };
		TFD::DefeatCaptiveTickWiring::InstallProvider(std::move(captiveTickDependencies));

		TFD::CaptiveRecaptureRecoveryWiring::Dependencies captiveRecaptureRecoveryDependencies{};
		captiveRecaptureRecoveryDependencies.getPlayer = [dependencies]() -> RE::Actor* { return PlayerActor(dependencies); };
		captiveRecaptureRecoveryDependencies.releasePlayerBleedLock = [](const char* reason, bool playGetUp) {
			TFD::BleedLockRuntime::ReleasePlayer(TFD::DefeatBleedLockWiring::BuildContext(), reason, playGetUp);
			};
		captiveRecaptureRecoveryDependencies.restoreActorHealthToSafePct = [](RE::Actor* actor, float thresholdPct, float bonusPct, float minSafePct, float maxSafePct, float minAbsHp, const char* reason) {
			TFD::DefeatBleedLockWiring::RestoreActorHealthToSafePct(actor, thresholdPct, bonusPct, minSafePct, maxSafePct, minAbsHp, reason);
			};
		captiveRecaptureRecoveryDependencies.resetBleedRuntimeState = [dependencies](bool preserveCaptive) {
			if (dependencies.resetBleedRuntimeStatePreserve) {
				dependencies.resetBleedRuntimeStatePreserve(preserveCaptive);
			}
			else if (dependencies.resetBleedRuntimeState) {
				dependencies.resetBleedRuntimeState();
			}
			};
		captiveRecaptureRecoveryDependencies.forceStopBleedRuntimeForCaptiveRecapture = [](const char* reason) { TFD::Bleedout::ForceStopBleedRuntimeForCaptiveRecapture(reason); };
		captiveRecaptureRecoveryDependencies.setPlayerBleedImmune = [](bool immune) { TFD::PlayerBleedImmunityGuard::SetPlayerActive(immune); };
		captiveRecaptureRecoveryDependencies.playPlayerGetUp = [](RE::Actor* actor) {
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				return;
			}
			actor->NotifyAnimationGraph("BleedoutStop");
			actor->NotifyAnimationGraph("GetUpStart");
			};
		captiveRecaptureRecoveryDependencies.refreshPostDefeatGlobals = []() { TFD::DefeatFlowRefresh::RefreshPostDefeatGlobals(); };
		captiveRecaptureRecoveryDependencies.getActorHealthPct = [dependencies](RE::Actor* actor) -> float {
			return dependencies.getActorHealthPct ? dependencies.getActorHealthPct(actor) : 100.0f;
			};
		captiveRecaptureRecoveryDependencies.getDefeatThresholdPct = []() -> float { return TFD::Settings::GetDefeatThresholdPct(); };
		captiveRecaptureRecoveryDependencies.isActorBleedingOut = [dependencies](RE::Actor* actor) -> bool {
			return dependencies.isActorBleedingOut ? dependencies.isActorBleedingOut(actor) : false;
			};
		TFD::CaptiveRecaptureRecoveryWiring::InstallProvider(captiveRecaptureRecoveryDependencies);

		spdlog::info("[TFD][Defeat][P29D] core runtime bootstrap providers installed");
	}

	void InstallPlayerDownAndOverkillProviders(Dependencies dependencies)
	{
		TFD::PlayerDownRouterOutcomeWiring::Dependencies outcomeRouteDependencies{};
		outcomeRouteDependencies.rememberAggressor = [](RE::Actor* actor) { TFD::DefeatAggressorResolver::RememberAggressor(actor); };
		outcomeRouteDependencies.findBleedoutSpeaker = [](float scanRadius, float maxDistance, RE::Actor* preferred) -> RE::Actor* {
			return TFD::DefeatBleedoutDialogueWiring::FindBestSpeaker(scanRadius, maxDistance, preferred);
		};
		outcomeRouteDependencies.startBleedWindow = [](RE::Actor* player, RE::Actor* speaker) { TFD::DefeatTransitionWiring::StartBleedWindow(player, speaker); };
		outcomeRouteDependencies.startBattleObservePending = [](RE::Actor* player) -> bool { return TFD::Bleedout::RuntimeHost::StartBattleObservePending(player); };
		outcomeRouteDependencies.setGraceSeconds = [](int seconds) { TFD::DefeatReleaseGrace::SetSeconds(seconds); };
		outcomeRouteDependencies.hasTerminalCommit = []() -> bool { return TFD::Bleedout::HasTerminalCommit(); };
		outcomeRouteDependencies.hasCachedRescueDestination = []() -> bool { return TFD::Location::ResolveMostRecentCachedRescueDestination(true) != nullptr; };
		outcomeRouteDependencies.hasPlayerBleedLock = [dependencies]() -> bool { return dependencies.hasPlayerBleedLock ? dependencies.hasPlayerBleedLock() : false; };
		outcomeRouteDependencies.enterPlayerBleedLock = [](RE::Actor* player, float thresholdPct, const char* reason) {
			TFD::BleedLockRuntime::Enter(TFD::DefeatBleedLockWiring::BuildContext(), player, TFD::BleedLockState::Kind::Player, thresholdPct, reason);
		};
		outcomeRouteDependencies.releasePlayerBleedLock = [](const char* reason, bool playGetUp) {
			TFD::BleedLockRuntime::ReleasePlayer(TFD::DefeatBleedLockWiring::BuildContext(), reason, playGetUp);
		};
		outcomeRouteDependencies.setPlayerBleedImmune = [](bool immune) { TFD::PlayerBleedImmunityGuard::SetPlayerActive(immune); };
		outcomeRouteDependencies.preparePlayer = [](RE::Actor* player) {
			if (player) {
				player->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
			}
		};
		outcomeRouteDependencies.executeNoMarkerFallback = [](const char* reason) -> bool {
			return TFD::DefeatRuntimeActions::ExecuteResolvedNoMarkerFallback(reason ? reason : "threshold_no_threat_rescue", TFD::DefeatTransitionWiring::BuildNoMarkerFallbackContext());
		};
		TFD::PlayerDownRouterOutcomeWiring::InstallProvider(std::move(outcomeRouteDependencies));

		TFD::PlayerDownRouterPendingWiring::Dependencies pendingRouteDependencies{};
		pendingRouteDependencies.getPlayer = [dependencies]() -> RE::Actor* { return PlayerActor(dependencies); };
		pendingRouteDependencies.setPlayerBleedImmune = [](bool immune) { TFD::PlayerBleedImmunityGuard::SetPlayerActive(immune); };
		pendingRouteDependencies.preparePlayer = [](RE::Actor* player) {
			if (!player) {
				return;
			}
			player->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
			if (player->IsDead(false)) {
				player->Resurrect(false, true);
			}
		};
		pendingRouteDependencies.clampHealth = [](RE::Actor* player, float minHealth) { TFD::DefeatBleedHealthGuard::ClampHealth(player, minHealth); };
		pendingRouteDependencies.resolveSafeFloorHealth = [](RE::Actor* player, float thresholdPct) -> float {
			return TFD::PlayerOverkillDamageHook::ResolveSafeFloorHealth(player, thresholdPct);
		};
		pendingRouteDependencies.hasPlayerBleedOwner = [dependencies]() -> bool {
			return (dependencies.hasPlayerBleedLock ? dependencies.hasPlayerBleedLock() : false) || TFD::DefeatBleedRuntimeState::IsInBleedState();
		};
		pendingRouteDependencies.enterPlayerBleedLock = [](RE::Actor* player, float thresholdPct, const char* reason) {
			TFD::BleedLockRuntime::Enter(TFD::DefeatBleedLockWiring::BuildContext(), player, TFD::BleedLockState::Kind::Player, thresholdPct, reason);
		};
		pendingRouteDependencies.isBleedDecisionActive = []() -> bool { return TFD::FlowController::Controller::GetSingleton().IsBleedDecisionActive(); };
		pendingRouteDependencies.scanThresholdOutcome = [](RE::Actor* player) -> TFD::PlayerDownRouter::ThresholdScan { return TFD::PlayerThresholdOutcomeWiring::ScanOutcome(player); };
		pendingRouteDependencies.isObserverAlly = [dependencies](RE::Actor* actor) -> bool { return dependencies.isObserverAlly ? dependencies.isObserverAlly(actor) : false; };
		pendingRouteDependencies.rememberAggressor = [](RE::Actor* actor) { TFD::DefeatAggressorResolver::RememberAggressor(actor); };
		pendingRouteDependencies.getHealthPct = [dependencies](RE::Actor* actor) -> float { return dependencies.getActorHealthPct ? dependencies.getActorHealthPct(actor) : 100.0f; };
		pendingRouteDependencies.buildDispatchContext = []() -> TFD::PlayerDownRouterWiring::DispatchContext { return TFD::PlayerDownRouterOutcomeWiring::BuildDispatchContext(); };
		TFD::PlayerDownRouterPendingWiring::InstallProvider(std::move(pendingRouteDependencies));

		TFD::PlayerOverkillDamageHook::Context playerOverkillHookContext{};
		playerOverkillHookContext.getPlayer = [dependencies]() -> RE::Actor* { return PlayerActor(dependencies); };
		playerOverkillHookContext.resolveCachedAttacker = []() -> RE::Actor* { return TFD::DefeatAggressorResolver::ResolveCachedPlayerOverkillAttacker(); };
		playerOverkillHookContext.hasPendingOverkillRoute = []() -> bool { return TFD::PlayerDownRouter::HasPendingOverkillRoute(); };
		playerOverkillHookContext.hasPlayerBleedLock = [dependencies]() -> bool { return dependencies.hasPlayerBleedLock ? dependencies.hasPlayerBleedLock() : false; };
		playerOverkillHookContext.isInBleedState = []() -> bool { return TFD::DefeatBleedRuntimeState::IsInBleedState(); };
		playerOverkillHookContext.hasRecentEnemyTargetingPlayer = [](double maxAgeSec) -> bool { return TFD::DefeatAggressorResolver::HasRecentEnemyTargetingPlayer(maxAgeSec); };
		playerOverkillHookContext.isObserverAlly = [dependencies](RE::Actor* actor) -> bool { return dependencies.isObserverAlly ? dependencies.isObserverAlly(actor) : false; };
		playerOverkillHookContext.setPlayerBleedImmune = [](bool enable) { TFD::PlayerBleedImmunityGuard::SetPlayerActive(enable); };
		playerOverkillHookContext.clampHealth = [](RE::Actor* actor, float minHp) { TFD::DefeatBleedHealthGuard::ClampHealth(actor, minHp); };
		playerOverkillHookContext.noteEnemyTargetingPlayer = [](RE::Actor* actor) { TFD::DefeatAggressorResolver::NoteEnemyTargetingPlayer(actor); };
		playerOverkillHookContext.isPreDeathShieldActive = []() -> bool { return TFD::PlayerOverkillDamageHook::IsPreDeathShieldActive(); };
		playerOverkillHookContext.resolveBleedRuntimeSafeHealth = [](RE::Actor* actor, float thresholdPct) -> float {
			return TFD::DefeatBleedHealthGuard::ResolvePlayerBleedRuntimeSafeHealth(actor, thresholdPct);
		};
		playerOverkillHookContext.rememberAggressor = [](RE::Actor* actor) { TFD::DefeatAggressorResolver::RememberAggressor(actor); };
		playerOverkillHookContext.getDefeatThresholdPct = []() -> float { return TFD::Settings::GetDefeatThresholdPct(); };
		playerOverkillHookContext.getActorHealthPct = [dependencies](RE::Actor* actor) -> float { return dependencies.getActorHealthPct ? dependencies.getActorHealthPct(actor) : 100.0f; };
		playerOverkillHookContext.resolveAggressor = []() -> RE::Actor* { return TFD::DefeatAggressorResolver::ResolveAggressor(); };
		playerOverkillHookContext.resolveLastEnemyTargetingPlayer = [](float radius, double maxAgeSec) -> RE::Actor* { return TFD::DefeatAggressorResolver::ResolveLastEnemyTargetingPlayer(radius, maxAgeSec); };
		playerOverkillHookContext.enterPlayerBleedLock = [](RE::Actor* player, float thresholdPct, const char* reason) {
			TFD::BleedLockRuntime::Enter(TFD::DefeatBleedLockWiring::BuildContext(), player, TFD::BleedLockState::Kind::Player, thresholdPct, reason);
		};
		playerOverkillHookContext.dispatchThresholdScanImmediateBleedout = [](RE::Actor* player, float hpPct, float thresholdPct, const char* reason) -> bool {
			return TFD::PlayerThresholdOutcomeWiring::DispatchImmediateBleedout(player, hpPct, thresholdPct, reason);
		};
		playerOverkillHookContext.tryBeginThresholdNoThreatRescueFallback = [](RE::Actor* player, float thresholdPct, const char* reason) -> bool {
			return TFD::PlayerThresholdOutcomeWiring::TryBeginNoThreatRescueFallback(player, thresholdPct, reason);
		};
		TFD::PlayerOverkillDamageHook::SetContext(std::move(playerOverkillHookContext));
		TFD::PlayerOverkillDamageHook::Install();
		spdlog::info("[TFD][Defeat][P29E] player down router and overkill providers installed");
	}

	void ShutdownPlayerDownAndOverkillProviders()
	{
		TFD::PlayerDownRouterPendingWiring::ShutdownProvider();
		TFD::PlayerDownRouterOutcomeWiring::ShutdownProvider();
		TFD::PlayerOverkillDamageHook::ClearContext();
		spdlog::info("[TFD][Defeat][P29E] player down router and overkill providers cleared");
	}

	void InstallLifecycleAndRuntimeProviders(Dependencies dependencies)
	{
		TFD::DefeatBleedoutLifecycleWiring::Dependencies bleedoutLifecycleDependencies{};
		bleedoutLifecycleDependencies.inBleedState = &TFD::DefeatBleedRuntimeState::InBleedState();
		bleedoutLifecycleDependencies.minHp = &TFD::DefeatBleedRuntimeState::MinHp();
		bleedoutLifecycleDependencies.bleedPendingCaptiveOutcome = &TFD::DefeatBleedRuntimeState::PendingCaptiveOutcome();
		bleedoutLifecycleDependencies.bleedPendingNonCaptiveOutcome = &TFD::DefeatBleedRuntimeState::PendingNonCaptiveOutcome();
		bleedoutLifecycleDependencies.graceActive = &TFD::DefeatReleaseGrace::GraceActive();
		bleedoutLifecycleDependencies.graceUntil = &TFD::DefeatReleaseGrace::GraceUntil();
		bleedoutLifecycleDependencies.previousDialogueOpen = TFD::DialogueLifecycle::PreviousOpenStorage();
		bleedoutLifecycleDependencies.getPlayer = [dependencies]() -> RE::Actor* { return PlayerActor(dependencies); };
		bleedoutLifecycleDependencies.setGraceSeconds = [](int seconds) { TFD::DefeatReleaseGrace::SetSeconds(seconds); };
		bleedoutLifecycleDependencies.setPlayerBleedImmune = [](bool immune) { TFD::PlayerBleedImmunityGuard::SetPlayerActive(immune); };
		bleedoutLifecycleDependencies.clampHealth = [](RE::Actor* actor, float minHp) { TFD::DefeatBleedHealthGuard::ClampHealth(actor, minHp); };
		bleedoutLifecycleDependencies.resetBleedRuntimeState = [dependencies](bool preserve) {
			if (dependencies.resetBleedRuntimeStatePreserve) {
				dependencies.resetBleedRuntimeStatePreserve(preserve);
			}
			else if (dependencies.resetBleedRuntimeState) {
				dependencies.resetBleedRuntimeState();
			}
		};
		bleedoutLifecycleDependencies.transitionBleedRuntimeToPleasureCommit = [dependencies](const char* reason) {
			if (dependencies.transitionBleedRuntimeToPleasureCommit) {
				dependencies.transitionBleedRuntimeToPleasureCommit(reason);
			}
		};
		bleedoutLifecycleDependencies.buildTransitionRuntimeHandlers = []() { return TFD::DefeatTransitionWiring::BuildTransitionRuntimeHandlers(); };
		bleedoutLifecycleDependencies.buildBleedoutSpeakerHandlers = []() { return TFD::DefeatBleedoutDialogueWiring::BuildSpeakerHandlers(); };
		bleedoutLifecycleDependencies.buildBleedoutDialogueRuntimeContext = []() { return TFD::DefeatBleedoutDialogueWiring::BuildRuntimeContext(); };
		bleedoutLifecycleDependencies.resolveBleedFlowActorFormID = [dependencies]() -> std::uint32_t {
			return dependencies.resolveBleedFlowActorFormID ? dependencies.resolveBleedFlowActorFormID() : 0u;
		};
		bleedoutLifecycleDependencies.collectBleedStandingFollowers = [dependencies](float radius) {
			return dependencies.collectBleedStandingFollowers ? dependencies.collectBleedStandingFollowers(radius) : std::vector<RE::Actor*>{};
		};
		bleedoutLifecycleDependencies.collectBleedoutCrowd = [](float radius, RE::Actor* preferred, bool preserveAssigned) {
			return TFD::DefeatBleedoutDialogueWiring::CollectCrowd(radius, preferred, preserveAssigned);
		};
		bleedoutLifecycleDependencies.isBleedSpaceCompatible = [dependencies](RE::Actor* actor, RE::Actor* player) -> bool {
			return dependencies.isBleedSpaceCompatible ? dependencies.isBleedSpaceCompatible(actor, player) : false;
		};
		bleedoutLifecycleDependencies.isObserverAlly = [dependencies](RE::Actor* actor) -> bool {
			return dependencies.isObserverAlly ? dependencies.isObserverAlly(actor) : false;
		};
		bleedoutLifecycleDependencies.isStandingAllyThresholdActor = [dependencies](RE::Actor* actor) -> bool {
			return dependencies.isStandingAllyThresholdActor ? dependencies.isStandingAllyThresholdActor(actor) : false;
		};
		bleedoutLifecycleDependencies.isStandingEnemyThresholdActor = [dependencies](RE::Actor* actor) -> bool {
			return dependencies.isStandingEnemyThresholdActor ? dependencies.isStandingEnemyThresholdActor(actor) : false;
		};
		TFD::DefeatBleedoutLifecycleWiring::InstallProvider(std::move(bleedoutLifecycleDependencies));

		TFD::DefeatFlowLifecycleWiring::Dependencies flowLifecycleDependencies{};
		flowLifecycleDependencies.inBleedState = []() -> bool { return TFD::DefeatBleedRuntimeState::IsInBleedState(); };
		flowLifecycleDependencies.resolveBleedFlowActorFormID = [dependencies]() -> std::uint32_t {
			return dependencies.resolveBleedFlowActorFormID ? dependencies.resolveBleedFlowActorFormID() : 0u;
		};
		flowLifecycleDependencies.resetBleedRuntimeState = [dependencies]() {
			if (dependencies.resetBleedRuntimeState) {
				dependencies.resetBleedRuntimeState();
			}
		};
		flowLifecycleDependencies.setPlayerBleedImmune = [](bool immune) { TFD::PlayerBleedImmunityGuard::SetPlayerActive(immune); };
		flowLifecycleDependencies.buildNoMarkerFallbackContext = []() { return TFD::DefeatTransitionWiring::BuildNoMarkerFallbackContext(); };
		flowLifecycleDependencies.resolveObservedDownedFollower = [dependencies]() -> RE::Actor* {
			return dependencies.resolveObservedDownedFollower ? dependencies.resolveObservedDownedFollower() : nullptr;
		};
		flowLifecycleDependencies.buildTransitionRuntimeHandlers = []() { return TFD::DefeatTransitionWiring::BuildTransitionRuntimeHandlers(); };
		TFD::DefeatFlowLifecycleWiring::InstallProvider(std::move(flowLifecycleDependencies));

		TFD::DefeatLifecycleBootstrapWiring::Dependencies lifecycleBootstrapDependencies{};
		lifecycleBootstrapDependencies.buildBleedoutProviders = []() { return TFD::DefeatBleedoutLifecycleWiring::BuildProviders(); };
		lifecycleBootstrapDependencies.buildFlowControllerProviders = []() { return TFD::DefeatFlowLifecycleWiring::BuildProviders(); };
		TFD::DefeatLifecycleBootstrapWiring::Install(lifecycleBootstrapDependencies);

		TFD::DefeatRuntimeProviderWiring::Install();
		spdlog::info("[TFD][DefeatBootstrap][P29F] lifecycle/runtime providers installed");
	}

	void InstallAllProviders(Dependencies dependencies)
	{
		InstallEarlyProviders(dependencies);
		InstallCoreRuntimeProviders(dependencies);
		InstallPlayerDownAndOverkillProviders(dependencies);
		InstallLifecycleAndRuntimeProviders(std::move(dependencies));
		spdlog::info("[TFD][DefeatBootstrap][P31C] all provider groups installed");
	}

	void ShutdownLifecycleProviders()
	{
		TFD::DefeatBleedoutLifecycleWiring::ShutdownProvider();
		TFD::DefeatFlowLifecycleWiring::ShutdownProvider();
		spdlog::info("[TFD][DefeatBootstrap][P29F] lifecycle providers cleared");
	}

}
