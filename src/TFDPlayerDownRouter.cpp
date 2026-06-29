#include "TFDPlayerDownRouter.h"

#include <algorithm>

#include <spdlog/spdlog.h>

namespace TFD::PlayerDownRouter
{
	namespace
	{
		PendingOverkillRoute g_pendingOverkillRoute{};

		static std::chrono::steady_clock::time_point Now()
		{
			return std::chrono::steady_clock::now();
		}
	}

	bool HasPendingOverkillRoute()
	{
		return g_pendingOverkillRoute.pending;
	}

	PendingOverkillRoute GetPendingOverkillRoute()
	{
		return g_pendingOverkillRoute;
	}

	void QueueOverkillRoute(RE::Actor* attacker, float thresholdPct, float hpBefore, float originalDamage, float clampedDamage, float blockedDamage, float safeFloorHp, const char* reason)
	{
		g_pendingOverkillRoute.pending = true;
		g_pendingOverkillRoute.attacker = attacker ? attacker->GetHandle() : RE::ActorHandle{};
		g_pendingOverkillRoute.queuedAt = Now();
		g_pendingOverkillRoute.thresholdPct = thresholdPct;
		g_pendingOverkillRoute.hpBefore = hpBefore;
		g_pendingOverkillRoute.originalDamage = originalDamage;
		g_pendingOverkillRoute.clampedDamage = clampedDamage;
		g_pendingOverkillRoute.blockedDamage = blockedDamage;
		g_pendingOverkillRoute.safeFloorHp = safeFloorHp;

		spdlog::warn(
			"[TFD][PlayerDownRouter][R21] overkill route queued attacker={:08X} hpBefore={:.2f} damageIn={:.2f} damageOut={:.2f} blocked={:.2f} safeFloor={:.2f} threshold={:.1f} reason={}",
			attacker ? attacker->GetFormID() : 0u,
			hpBefore,
			originalDamage,
			clampedDamage,
			blockedDamage,
			safeFloorHp,
			thresholdPct,
			reason && reason[0] ? reason : "unknown");
	}

	void AccumulatePendingOverkillDamage(float originalDamage, float clampedDamage, float blockedDamage)
	{
		if (!g_pendingOverkillRoute.pending) {
			return;
		}
		g_pendingOverkillRoute.originalDamage += originalDamage;
		g_pendingOverkillRoute.clampedDamage = clampedDamage;
		g_pendingOverkillRoute.blockedDamage += blockedDamage;
	}

	void ClearPendingOverkillRoute(const char* reason)
	{
		if (g_pendingOverkillRoute.pending) {
			spdlog::info(
				"[TFD][PlayerDownRouter][R21] overkill route cleared attacker={:08X} blocked={:.2f} clamped={:.2f} reason={}",
				[&]() -> std::uint32_t {
					if (!g_pendingOverkillRoute.attacker) {
						return 0u;
					}
					auto sp = RE::Actor::LookupByHandle(g_pendingOverkillRoute.attacker.native_handle());
					auto* actor = sp.get();
					return actor ? actor->GetFormID() : 0u;
				}(),
				g_pendingOverkillRoute.blockedDamage,
				g_pendingOverkillRoute.clampedDamage,
				reason && reason[0] ? reason : "unknown");
		}
		g_pendingOverkillRoute = {};
	}

	ThresholdClassification ClassifyThresholdOutcome(const ThresholdScan& scan)
	{
		ThresholdClassification classification{};
		classification.rememberedAggressor = scan.initialAggressor;
		classification.observeStandingFollowers = !scan.standingFollowers.empty();
		classification.observeUnresolvedBattle =
			scan.unresolvedBattle &&
			scan.hostileCoalitionStanding &&
			classification.observeStandingFollowers;
		return classification;
	}

	bool HasThresholdRescueFallbackCandidate(const ThresholdScan& scan, bool hasCachedDestination)
	{
		if (!scan.standingFollowers.empty()) {
			return true;
		}
		return hasCachedDestination;
	}

	bool TryBeginNoThreatRescueFallback(RE::Actor* player, const ThresholdScan& scan, float thresholdPct, const char* reason, const NoThreatFallbackHandlers& handlers)
	{
		if (!player) {
			return false;
		}
		const char* why = reason && reason[0] ? reason : "threshold_no_threat_rescue";
		if (handlers.hasTerminalCommit && handlers.hasTerminalCommit()) {
			spdlog::info("[TFD][PlayerDownRouter][R21] no-threat fallback skipped reason=terminal_commit source={}", why);
			return false;
		}
		const bool hasCachedDestination = handlers.hasCachedRescueDestination && handlers.hasCachedRescueDestination();
		if (!HasThresholdRescueFallbackCandidate(scan, hasCachedDestination)) {
			spdlog::info(
				"[TFD][PlayerDownRouter][R21] no-threat fallback skipped reason=no_candidate source={} playerSide={} followers={} cachedDest={}",
				why,
				scan.playerSideStanding ? 1 : 0,
				static_cast<unsigned int>(scan.standingFollowers.size()),
				hasCachedDestination ? 1 : 0);
			return false;
		}

		const bool enteredLock = handlers.hasPlayerBleedLock ? !handlers.hasPlayerBleedLock() : false;
		if (enteredLock && handlers.enterPlayerBleedLock) {
			handlers.enterPlayerBleedLock(player, thresholdPct, why);
		}
		if (handlers.setPlayerBleedImmune) {
			handlers.setPlayerBleedImmune(true);
		}
		if (handlers.preparePlayer) {
			handlers.preparePlayer(player);
		}

		const bool committed = handlers.executeNoMarkerFallback && handlers.executeNoMarkerFallback(why);
		if (committed) {
			spdlog::warn(
				"[TFD][PlayerDownRouter][R21] no-threat fallback committed source={} playerSide={} followers={} cachedDest={}",
				why,
				scan.playerSideStanding ? 1 : 0,
				static_cast<unsigned int>(scan.standingFollowers.size()),
				hasCachedDestination ? 1 : 0);
			return true;
		}

		if (enteredLock && handlers.releasePlayerBleedLock) {
			handlers.releasePlayerBleedLock("threshold_no_threat_rescue_failed", false);
		}
		if (handlers.setPlayerBleedImmune) {
			handlers.setPlayerBleedImmune(false);
		}
		spdlog::info("[TFD][PlayerDownRouter][R21] no-threat fallback failed source={}", why);
		return false;
	}

	bool DispatchThresholdOutcome(RE::Actor* player, const ThresholdScan& scan, const ThresholdClassification& classification, const DispatchHandlers& handlers)
	{
		if (!player) {
			return false;
		}
		if (handlers.rememberAggressor) {
			handlers.rememberAggressor(classification.rememberedAggressor);
		}

		if (!classification.observeStandingFollowers && handlers.findBleedoutSpeaker) {
			auto* immediateSpeaker = handlers.findBleedoutSpeaker(scan.scanRadius, 12000.0f, classification.rememberedAggressor);
			if (immediateSpeaker) {
				if (handlers.rememberAggressor) {
					handlers.rememberAggressor(immediateSpeaker);
				}
				spdlog::info("[TFD][PlayerDownRouter][R21] player threshold -> immediate bleedout forcegreet speaker={:08X}", immediateSpeaker->GetFormID());
				if (handlers.startBleedWindow) {
					handlers.startBleedWindow(player, immediateSpeaker);
				}
				if (handlers.setGraceSeconds) {
					handlers.setGraceSeconds(1);
				}
				return true;
			}
		}

		if (classification.observeUnresolvedBattle) {
			spdlog::info(
				"[TFD][PlayerDownRouter][R21] delay outcome unresolved battle coalitions={} playerSideStanding={} standingFollowers={}",
				scan.coalitionSnapshot.activeCoalitionCount,
				scan.playerSideStanding ? 1 : 0,
				static_cast<unsigned int>(scan.standingFollowers.size()));
			if (handlers.startBattleObservePending && handlers.startBattleObservePending(player)) {
				if (handlers.setGraceSeconds) {
					handlers.setGraceSeconds(1);
				}
				return true;
			}
		}

		if (classification.observeStandingFollowers) {
			spdlog::info(
				"[TFD][PlayerDownRouter][R21] delay outcome standing follower confirmed followers={} playerSideStanding={}",
				static_cast<unsigned int>(scan.standingFollowers.size()),
				scan.playerSideStanding ? 1 : 0);
			if (handlers.startBattleObservePending && handlers.startBattleObservePending(player)) {
				if (handlers.setGraceSeconds) {
					handlers.setGraceSeconds(1);
				}
				return true;
			}
		}

		RE::Actor* speaker = nullptr;
		if (handlers.findBleedoutSpeaker) {
			speaker = handlers.findBleedoutSpeaker(scan.scanRadius, 12000.0f, classification.rememberedAggressor);
		}
		if (handlers.rememberAggressor) {
			handlers.rememberAggressor(speaker);
		}
		if (!speaker) {
			spdlog::info("[TFD][PlayerDownRouter][R21] no dialogue-capable aggressor and no standing follower -> bleed countdown without speaker");
		}
		if (handlers.startBleedWindow) {
			handlers.startBleedWindow(player, speaker);
		}
		if (handlers.setGraceSeconds) {
			handlers.setGraceSeconds(1);
		}
		return true;
	}

	bool TickPendingOverkillRoute(RE::Actor* player, const PendingRouteHandlers& handlers)
	{
		if (!g_pendingOverkillRoute.pending) {
			return false;
		}
		if (!player || player->IsDisabled()) {
			ClearPendingOverkillRoute("invalid_player");
			return false;
		}

		const auto pending = g_pendingOverkillRoute;
		const auto now = Now();
		const auto queuedAgeMs = pending.queuedAt.time_since_epoch().count() != 0 ?
			std::chrono::duration_cast<std::chrono::milliseconds>(now - pending.queuedAt).count() : 0LL;
		const float thresholdPct = std::clamp(pending.thresholdPct, 2.0f, 95.0f);

		if (handlers.setPlayerBleedImmune) {
			handlers.setPlayerBleedImmune(true);
		}
		if (handlers.preparePlayer) {
			handlers.preparePlayer(player);
		}
		const float safeFloorHp = handlers.resolveSafeFloorHealth ?
			(std::max)(pending.safeFloorHp, handlers.resolveSafeFloorHealth(player, thresholdPct)) : pending.safeFloorHp;
		if (handlers.clampHealth) {
			handlers.clampHealth(player, safeFloorHp);
		}

		auto attackerSp = RE::Actor::LookupByHandle(pending.attacker.native_handle());
		auto* attacker = attackerSp.get();
		if (attacker && handlers.isObserverAlly && !handlers.isObserverAlly(attacker) && handlers.rememberAggressor) {
			handlers.rememberAggressor(attacker);
		}

		if (handlers.hasPlayerBleedOwner && handlers.hasPlayerBleedOwner()) {
			spdlog::info(
				"[TFD][PlayerDownRouter][R21] overkill route consumed by existing bleed owner attacker={:08X} ageMs={} hpPct={:.1f}",
				attacker ? attacker->GetFormID() : 0u,
				static_cast<long long>(queuedAgeMs),
				handlers.getHealthPct ? handlers.getHealthPct(player) : -1.0f);
			ClearPendingOverkillRoute("existing_bleed_owner");
			return true;
		}

		if (handlers.enterPlayerBleedLock) {
			handlers.enterPlayerBleedLock(player, thresholdPct, "r21_overkill_damage_route");
		}

		const float hpPct = handlers.getHealthPct ? handlers.getHealthPct(player) : -1.0f;
		if (handlers.isBleedDecisionActive && handlers.isBleedDecisionActive()) {
			spdlog::info(
				"[TFD][PlayerDownRouter][R21] overkill route already active attacker={:08X} ageMs={} hpPct={:.1f} threshold={:.1f}",
				attacker ? attacker->GetFormID() : 0u,
				static_cast<long long>(queuedAgeMs),
				hpPct,
				thresholdPct);
			ClearPendingOverkillRoute("bleed_decision_active");
			return true;
		}

		ThresholdScan scan{};
		if (handlers.scanThresholdOutcome) {
			scan = handlers.scanThresholdOutcome(player);
		}
		if (!scan.initialAggressor && attacker && (!handlers.isObserverAlly || !handlers.isObserverAlly(attacker))) {
			scan.initialAggressor = attacker;
		}
		const bool immediateThreat =
			player->IsInCombat() ||
			scan.initialAggressor != nullptr ||
			scan.hostileCoalitionStanding ||
			(attacker && (!handlers.isObserverAlly || !handlers.isObserverAlly(attacker)));

		bool dispatched = false;
		if (immediateThreat) {
			auto classification = ClassifyThresholdOutcome(scan);
			dispatched = DispatchThresholdOutcome(player, scan, classification, handlers.dispatch);
		}

		spdlog::warn(
			"[TFD][PlayerDownRouter][R21] overkill route dispatched attacker={:08X} dispatched={} immediateThreat={} ageMs={} hpPct={:.1f} threshold={:.1f} blocked={:.2f} clamped={:.2f}",
			attacker ? attacker->GetFormID() : 0u,
			dispatched ? 1 : 0,
			immediateThreat ? 1 : 0,
			static_cast<long long>(queuedAgeMs),
			hpPct,
			thresholdPct,
			pending.blockedDamage,
			pending.clampedDamage);

		ClearPendingOverkillRoute("dispatched");
		return dispatched;
	}
}
