#include "TFDDefeatBleedLockWiring.h"

#include "TFDDefeatTerminalLockout.h"

#include <algorithm>
#include <mutex>
#include <utility>

#include "TFDActor.h"
#include "TFDPlayerOverkillDamageHook.h"
#include "TFDSettings.h"
#include "TFDTeammateManager.h"

#include <spdlog/spdlog.h>

namespace TFD::DefeatBleedLockWiring
{
	using BleedLockKind = TFD::BleedLockRuntime::Kind;

	namespace
	{
		std::mutex g_providerLock;
		Dependencies g_dependencies{};
		bool g_hasProvider = false;

		Dependencies ResolveDependencies()
		{
			std::scoped_lock lock(g_providerLock);
			return g_dependencies;
		}

		RE::Actor* ResolvePlayer(const Dependencies& dependencies)
		{
			return dependencies.getPlayer ? dependencies.getPlayer() : RE::PlayerCharacter::GetSingleton();
		}

		float GetActorHealthPct(const Dependencies& dependencies, RE::Actor* actor)
		{
			if (dependencies.getActorHealthPct) {
				return dependencies.getActorHealthPct(actor);
			}
			if (!actor) {
				return 100.0f;
			}
			const float maxHp = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
			return std::clamp((actor->GetActorValue(RE::ActorValue::kHealth) / maxHp) * 100.0f, 0.0f, 100.0f);
		}

		bool IsActiveFollowerActor(const Dependencies& dependencies, RE::Actor* actor)
		{
			return dependencies.isActiveFollowerActor && dependencies.isActiveFollowerActor(actor);
		}

		float ResolveBleedLockThresholdPct(const Dependencies& dependencies, RE::Actor* actor)
		{
			if (!actor) {
				return 0.0f;
			}
			if (actor == ResolvePlayer(dependencies)) {
				return TFD::Settings::GetDefeatThresholdPct();
			}
			if (IsActiveFollowerActor(dependencies, actor)) {
				return TFD::Settings::GetAllyDownedThresholdPct();
			}
			return TFD::Settings::GetEnemyDownedThresholdPct();
		}

		bool HasImmediatePlayerSideBleedThreat(const Dependencies& dependencies, RE::Actor* player, const TFD::Actor::Snapshot& snapshot, float radius)
		{
			if (!player) {
				return false;
			}
			if (player->IsInCombat()) {
				return true;
			}
			if (dependencies.resolveAggressor && dependencies.resolveAggressor()) {
				return true;
			}
			if (dependencies.findBestAggressor && dependencies.findBestAggressor(radius)) {
				return true;
			}
			return TFD::Actor::HasStandingHostileCoalition(snapshot);
		}

		bool ShouldEnterPlayerOrAllyBleedLock(const Dependencies& dependencies, RE::Actor* actor, float thresholdPct, bool hasRelevantThreat)
		{
			if (!actor || actor->IsDisabled() || actor->IsDead()) {
				return false;
			}
			if (GetActorHealthPct(dependencies, actor) > std::clamp(thresholdPct, 2.0f, 95.0f)) {
				return false;
			}
			if (actor == ResolvePlayer(dependencies) ||
				IsActiveFollowerActor(dependencies, actor) ||
				TFD::TeammateManager::IsPlayerSideTeammateActor(actor)) {
				return hasRelevantThreat;
			}
			return false;
		}
	}

	void ApplyBleedRegenOverride(RE::Actor* actor, Entry& entry)
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

	void RestoreBleedRegenOverride(RE::Actor* actor, const Entry& entry)
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

	void ForcePlayerBleedAlive(RE::Actor* actor, Entry& entry, const char* reason)
	{
		if (!actor) {
			return;
		}

		const Dependencies dependencies = ResolveDependencies();
		if (dependencies.setPlayerBleedImmune) {
			dependencies.setPlayerBleedImmune(true);
		}

		auto& boolFlags = actor->GetActorRuntimeData().boolFlags;
		const bool wasDead = actor->IsDead(false);
		const bool wasKillMove = boolFlags.all(RE::Actor::BOOL_FLAGS::kIsInKillMove);

		if (wasDead) {
			actor->Resurrect(false, true);
		}

		// P16: bleed-alive reassert is owned by DefeatBleedLockWiring.
		boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);

		const float hardFloorHp = (std::max)(1.0f, entry.minHp);
		if (dependencies.clampHealth) {
			dependencies.clampHealth(actor, hardFloorHp);
		}
		else {
			float hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
			if (hpNow + 0.001f < hardFloorHp) {
				actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, hardFloorHp - hpNow);
			}
		}

		float hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
		if (hpNow + 0.001f < hardFloorHp) {
			actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, hardFloorHp - hpNow);
			hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
		}

		entry.protectedHealth = (std::max)(entry.protectedHealth, hpNow);
		entry.lastHealthSample = hpNow;

		actor->NotifyAnimationGraph("BleedoutStart");

		spdlog::warn(
			"[TFD][BleedLockWiring][P16] player bleed anti-death reassert actor={:08X} reason={} resurrected={} killmove={}",
			actor->GetFormID(),
			reason ? reason : "unknown",
			wasDead ? 1 : 0,
			wasKillMove ? 1 : 0);
	}


	void RestoreActorHealthToSafePct(RE::Actor* actor, float thresholdPct, float bonusPct, float minSafePct, float maxSafePct, float minAbsHp, const char* reason)
	{
		if (!actor || actor->IsDead() || actor->IsDisabled()) {
			return;
		}

		const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
		const float hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
		auto pctToUnit = [](float value) {
			return value > 1.0f ? (value / 100.0f) : value;
			};

		const float threshold = std::clamp(pctToUnit(thresholdPct), 0.05f, 0.95f);
		const float bonus = std::clamp(pctToUnit(bonusPct), 0.0f, 0.95f);
		const float minSafe = std::clamp(pctToUnit(minSafePct), 0.05f, 1.0f);
		const float maxSafe = std::clamp(pctToUnit(maxSafePct), minSafe, 1.0f);
		const float safePct = std::clamp(threshold + bonus, minSafe, maxSafe);
		const float target = (std::max)(minAbsHp, hpMax * safePct);

		if (hpNow + 0.001f < target) {
			actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, target - hpNow);
			spdlog::info(
				"[TFD][BleedLockWiring][P17] recover actor hp actor={:08X} reason={} from={:.2f} to={:.2f} thresholdPct={:.1f}",
				actor->GetFormID(),
				reason ? reason : "unknown",
				hpNow,
				target,
				thresholdPct);
		}
	}


	void InstallProvider(Dependencies dependencies)
	{
		std::scoped_lock lock(g_providerLock);
		g_dependencies = std::move(dependencies);
		g_hasProvider = true;
		spdlog::info("[TFD][BleedLockWiring][P16] provider installed");
	}

	void ShutdownProvider()
	{
		std::scoped_lock lock(g_providerLock);
		g_dependencies = Dependencies{};
		g_hasProvider = false;
		spdlog::info("[TFD][BleedLockWiring][P16] provider cleared");
	}

	bool HasProvider()
	{
		std::scoped_lock lock(g_providerLock);
		return g_hasProvider;
	}

	TFD::BleedLockRuntime::Context BuildContext()
	{
		Dependencies dependencies = ResolveDependencies();

		TFD::BleedLockRuntime::Context context{};
		context.getPlayer = dependencies.getPlayer;
		context.resolveAggressor = dependencies.resolveAggressor;
		context.setPlayerBleedImmune = dependencies.setPlayerBleedImmune;
		context.getMinHp = dependencies.getMinHp;
		context.setMinHp = dependencies.setMinHp;
		context.maxMinHp = dependencies.maxMinHp;
		context.resetPreDeathShield = dependencies.resetPreDeathShield;
		context.isInBleedState = dependencies.isInBleedState;
		context.resolvePlayerBleedRuntimeSafeHealth = dependencies.resolvePlayerBleedRuntimeSafeHealth;
		context.buildPlayerDamageGuardConfig = dependencies.buildPlayerDamageGuardConfig;
		context.clampHealth = dependencies.clampHealth;
		context.clampHealthCeiling = dependencies.clampHealthCeiling;
		context.resolveActorHealthForPct = dependencies.resolveActorHealthForPct;
		context.getActorHealthPct = dependencies.getActorHealthPct;
		context.isActorBleedingOut = dependencies.isActorBleedingOut;
		context.applyBleedRegenOverride = [](RE::Actor* actor, Entry& entry) { ApplyBleedRegenOverride(actor, entry); };
		context.restoreBleedRegenOverride = [](RE::Actor* actor, const Entry& entry) { RestoreBleedRegenOverride(actor, entry); };
		context.logReferenceBleedState = dependencies.logReferenceBleedState;
		context.maybeLogReferenceBleedSamples = dependencies.maybeLogReferenceBleedSamples;
		context.enforcePlayerBleedInvulnerability = dependencies.enforcePlayerBleedInvulnerability;
		context.forcePlayerBleedAlive = [](RE::Actor* actor, Entry& entry, const char* reason) { ForcePlayerBleedAlive(actor, entry, reason); };
		context.tickPlayerKillmoveSuppression = dependencies.tickPlayerKillmoveSuppression;
		context.tickPlayerOverkillBlockPending = dependencies.tickPlayerOverkillBlockPending;
		context.tickPlayerPreDeathShield = dependencies.tickPlayerPreDeathShield;
		context.scanBleedLockCandidates = dependencies.scanBleedLockCandidates;
		context.isDownedTeammateRecoveryDialogueHoldActor = dependencies.isDownedTeammateRecoveryDialogueHoldActor;
		context.isPlayerBattleObserveActive = dependencies.isPlayerBattleObserveActive;
		context.resetPlayerDamageGuard = dependencies.resetPlayerDamageGuard;
		context.clearPendingOverkillRoute = dependencies.clearPendingOverkillRoute;
		context.clearPreDeathShield = dependencies.clearPreDeathShield;
		context.resetEnemyThresholdScanTimer = dependencies.resetEnemyThresholdScanTimer;
		return context;
	}

	void ScanCandidates()
	{
		const Dependencies dependencies = ResolveDependencies();
		auto* player = ResolvePlayer(dependencies);
		if (!player) {
			return;
		}

		const float radius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 600.0f);
		auto snapshot = TFD::Actor::BuildSnapshot(radius, false);
		const bool playerSideThreat = HasImmediatePlayerSideBleedThreat(dependencies, player, snapshot, radius);

		const float playerThreshold = ResolveBleedLockThresholdPct(dependencies, player);
		const float playerHpPct = GetActorHealthPct(dependencies, player);
		const float clampedPlayerThreshold = std::clamp(playerThreshold, 2.0f, 95.0f);
		const bool terminalLockout = playerHpPct <= clampedPlayerThreshold &&
			TFD::DefeatTerminalLockout::IsActive("threshold_scan_player", playerHpPct, clampedPlayerThreshold);

		const float preDeathArmPct = TFD::PlayerOverkillDamageHook::ResolvePreDeathArmPct(playerThreshold);
		if (!terminalLockout && playerSideThreat && playerHpPct <= preDeathArmPct && playerHpPct > clampedPlayerThreshold) {
			TFD::PlayerOverkillDamageHook::SetPreDeathShieldActive(true, player, playerHpPct, playerThreshold, "r448a_predeath_arm_commit");
			if (dependencies.setPlayerBleedImmune) {
				dependencies.setPlayerBleedImmune(true);
			}
			player->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
			TFD::BleedLockRuntime::Enter(BuildContext(), player, BleedLockKind::Player, playerThreshold, "r448a_predeath_arm_commit");
			const bool dispatched = dependencies.dispatchThresholdScanImmediateBleedout ?
				dependencies.dispatchThresholdScanImmediateBleedout(player, playerHpPct, playerThreshold, "r448a_predeath_arm_commit") :
				false;
			spdlog::warn(
				"[TFD][BleedLockWiring][P16] pre-death arm committed to bleedout dispatched={} hpPctBefore={:.1f} hpPctAfter={:.1f} threshold={:.1f} armPct={:.1f} playerSideThreat={}",
				dispatched ? 1 : 0,
				playerHpPct,
				GetActorHealthPct(dependencies, player),
				clampedPlayerThreshold,
				preDeathArmPct,
				playerSideThreat ? 1 : 0);
			return;
		}
		else if (!(dependencies.hasPlayerBleedLock && dependencies.hasPlayerBleedLock()) &&
			!(dependencies.isInBleedState && dependencies.isInBleedState()) &&
			(TFD::PlayerOverkillDamageHook::IsPreDeathShieldActive() &&
				(terminalLockout || !playerSideThreat || playerHpPct > std::clamp(preDeathArmPct + 8.0f, 6.0f, 99.0f)))) {
			TFD::PlayerOverkillDamageHook::SetPreDeathShieldActive(
				false,
				player,
				playerHpPct,
				playerThreshold,
				terminalLockout ? "terminal_lockout" : (!playerSideThreat ? "no_immediate_threat" : "hp_recovered"));
		}

		if (!terminalLockout && ShouldEnterPlayerOrAllyBleedLock(dependencies, player, playerThreshold, playerSideThreat)) {
			TFD::BleedLockRuntime::Enter(BuildContext(), player, BleedLockKind::Player, playerThreshold, "threshold_scan_player");
			if (dependencies.dispatchThresholdScanImmediateBleedout &&
				dependencies.dispatchThresholdScanImmediateBleedout(player, playerHpPct, playerThreshold, "threshold_scan_player_immediate")) {
				return;
			}
		}
		else if (!terminalLockout && playerHpPct <= clampedPlayerThreshold && !playerSideThreat) {
			TFD::PlayerDownRouter::ThresholdScan thresholdScan{};
			if (dependencies.scanPlayerThresholdOutcome) {
				thresholdScan = dependencies.scanPlayerThresholdOutcome(player);
			}
			if (dependencies.tryBeginThresholdNoThreatRescueFallback &&
				dependencies.tryBeginThresholdNoThreatRescueFallback(player, thresholdScan, playerThreshold, "threshold_scan_player_no_threat")) {
				return;
			}
			spdlog::info(
				"[TFD][BleedLockWiring][P16] skip bleed lock player reason=no_immediate_threat hpPct={:.1f} threshold={:.1f}",
				playerHpPct,
				clampedPlayerThreshold);
		}

		for (auto* actor : TFD::TeammateManager::CollectKnownTeammates(radius)) {
			const float threshold = ResolveBleedLockThresholdPct(dependencies, actor);
			const bool followerThreat = playerSideThreat || (actor && actor->IsInCombat());
			if (ShouldEnterPlayerOrAllyBleedLock(dependencies, actor, threshold, followerThreat)) {
				TFD::BleedLockRuntime::Enter(BuildContext(), actor, BleedLockKind::Ally, threshold, "threshold_scan_follower");
			}
		}

		for (const auto& info : snapshot.actors) {
			auto* actor = info.get();
			if (!actor || actor == player || actor->IsDisabled() || actor->IsDead()) {
				continue;
			}
			if (!TFD::TeammateManager::IsPlayerSideTeammateActor(actor) || IsActiveFollowerActor(dependencies, actor)) {
				continue;
			}
			const float threshold = TFD::Settings::GetAllyDownedThresholdPct();
			const bool followerThreat = playerSideThreat || actor->IsInCombat();
			if (ShouldEnterPlayerOrAllyBleedLock(dependencies, actor, threshold, followerThreat)) {
				TFD::BleedLockRuntime::Enter(BuildContext(), actor, BleedLockKind::Ally, threshold, "threshold_scan_follower_loose");
			}
		}
	}
}
