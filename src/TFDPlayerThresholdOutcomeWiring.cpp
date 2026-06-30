#include "TFDPlayerThresholdOutcomeWiring.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <utility>

#include <spdlog/spdlog.h>

#include "TFDActor.h"
#include "TFDBleedLockRuntime.h"
#include "TFDBleedLockState.h"
#include "TFDBleedout.h"
#include "TFDBleedoutGreet.h"
#include "TFDDefeatAggressorResolver.h"
#include "TFDDefeatBleedLockWiring.h"
#include "TFDFlowController.h"
#include "TFDPlayerDownRouterOutcomeWiring.h"
#include "TFDSettings.h"
#include "TFDTame.h"
#include "TFDTeammateManager.h"

namespace TFD::PlayerThresholdOutcomeWiring
{
	namespace
	{
		std::mutex g_providerLock;
		Dependencies g_dependencies{};
		bool g_hasProvider = false;
		std::chrono::steady_clock::time_point g_thresholdScanImmediateDispatchLast{};

		static std::chrono::steady_clock::time_point Now()
		{
			return std::chrono::steady_clock::now();
		}

		Dependencies ResolveDependencies()
		{
			std::scoped_lock lock(g_providerLock);
			return g_dependencies;
		}

		bool IsObserverAlly(const Dependencies& dependencies, RE::Actor* actor)
		{
			return dependencies.isObserverAlly ? dependencies.isObserverAlly(actor) : false;
		}

		bool IsInBleedState(const Dependencies& dependencies)
		{
			return dependencies.isInBleedState ? dependencies.isInBleedState() : false;
		}

		void ReleaseStaleEscapeBreakBleedRuntime(const Dependencies& dependencies, const char* reason)
		{
			if (dependencies.releaseStaleEscapeBreakBleedRuntime) {
				dependencies.releaseStaleEscapeBreakBleedRuntime(reason);
				return;
			}

			TFD::BleedoutGreet::ResetRuntime(reason ? reason : "r270_stale_escape_break_rebleed");
			TFD::Bleedout::ClearDialogueOutcome(reason ? reason : "r270_stale_escape_break_rebleed");
		}

		std::vector<RE::Actor*> CollectStandingFollowers(const Dependencies& dependencies, float radius)
		{
			return dependencies.collectStandingFollowers ? dependencies.collectStandingFollowers(radius) : std::vector<RE::Actor*>{};
		}

		bool ContainsActorByFormID(const std::vector<RE::Actor*>& actors, RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			const auto formID = actor->GetFormID();
			for (auto* candidate : actors) {
				if (candidate && candidate->GetFormID() == formID) {
					return true;
				}
			}
			return false;
		}

		bool IsConcreteStandingPlayerSideAlly(RE::Actor* actor, RE::Actor* player)
		{
			if (!actor || !player || actor == player || actor->IsDead() || actor->IsDisabled()) {
				return false;
			}
			if (TFD::Actor::IsDownByHealthThreshold(actor, TFD::Settings::GetAllyDownedThresholdPct())) {
				return false;
			}
			return
				actor->IsPlayerTeammate() ||
				TFD::TeammateManager::IsActiveFollowerActor(actor) ||
				TFD::TeammateManager::IsPlayerSideTeammateActor(actor) ||
				TFD::Tame::IsCompanion(actor);
		}

		std::vector<RE::Actor*> CollectConcreteStandingPlayerSideAlliesFromSnapshot(
			const TFD::Actor::Snapshot& snapshot,
			RE::Actor* player)
		{
			std::vector<RE::Actor*> out;
			for (auto* actor : TFD::Actor::ResolveStandingPlayerSideActors(snapshot, false)) {
				if (!IsConcreteStandingPlayerSideAlly(actor, player)) {
					continue;
				}
				if (ContainsActorByFormID(out, actor)) {
					continue;
				}
				out.push_back(actor);
			}
			return out;
		}
	}

	void InstallProvider(Dependencies dependencies)
	{
		std::scoped_lock lock(g_providerLock);
		g_dependencies = std::move(dependencies);
		g_hasProvider = true;
		g_thresholdScanImmediateDispatchLast = {};
		spdlog::info("[TFD][PlayerThresholdOutcome][P26] provider installed");
	}

	void ShutdownProvider()
	{
		std::scoped_lock lock(g_providerLock);
		g_dependencies = Dependencies{};
		g_hasProvider = false;
		g_thresholdScanImmediateDispatchLast = {};
		spdlog::info("[TFD][PlayerThresholdOutcome][P26] provider cleared");
	}

	bool HasProvider()
	{
		std::scoped_lock lock(g_providerLock);
		return g_hasProvider;
	}

	ThresholdScan ScanOutcome(RE::Actor* player)
	{
		const auto dependencies = ResolveDependencies();
		ThresholdScan scan{};
		scan.scanRadius = (std::max)(12000.0f, TFD::Settings::GetSweepRadius());
		scan.initialAggressor = TFD::DefeatAggressorResolver::ResolveAggressor();
		if (!scan.initialAggressor) {
			scan.initialAggressor = TFD::DefeatAggressorResolver::FindBestAggressor(scan.scanRadius);
		}
		scan.coalitionSnapshot = TFD::Actor::BuildSnapshot(scan.scanRadius, false);
		scan.unresolvedBattle = !TFD::Actor::IsConflictResolved(scan.coalitionSnapshot);
		scan.playerSideStanding = TFD::Actor::HasStandingTeammateOnPlayerSide(scan.coalitionSnapshot);
		scan.hostileCoalitionStanding = TFD::Actor::HasStandingHostileCoalition(scan.coalitionSnapshot);
		scan.standingFollowers = CollectStandingFollowers(dependencies, scan.scanRadius);

		// R10: TeammateManager aliases can be stale/empty at the exact player overkill
		// hook, especially with vanilla followers. The combat snapshot already knows
		// whether a concrete non-player player-side actor is standing. Merge that
		// snapshot ally into the follower guard so enemy Bleedout ForceGreet cannot
		// preempt an active teammate-vs-enemy fight.
		auto snapshotAllies = CollectConcreteStandingPlayerSideAlliesFromSnapshot(scan.coalitionSnapshot, player);
		std::uint32_t merged = 0;
		for (auto* ally : snapshotAllies) {
			if (ContainsActorByFormID(scan.standingFollowers, ally)) {
				continue;
			}
			scan.standingFollowers.push_back(ally);
			++merged;
		}
		if (merged > 0) {
			spdlog::info(
				"[TFD][Defeat][R10] merged concrete player-side allies into bleedout follower guard merged={} total={} playerSideStanding={} hostileStanding={}",
				merged,
				static_cast<unsigned int>(scan.standingFollowers.size()),
				scan.playerSideStanding ? 1 : 0,
				scan.hostileCoalitionStanding ? 1 : 0);
		}
		return scan;
	}

	ThresholdClassification ClassifyOutcome(const ThresholdScan& scan)
	{
		const auto dependencies = ResolveDependencies();
		auto classification = TFD::PlayerDownRouter::ClassifyThresholdOutcome(scan);
		classification.rememberedAggressor =
			(scan.initialAggressor && !IsObserverAlly(dependencies, scan.initialAggressor)) ? scan.initialAggressor : nullptr;
		return classification;
	}

	bool TryBeginNoThreatRescueFallback(RE::Actor* player, const ThresholdScan& scan, float thresholdPct, const char* reason)
	{
		return TFD::PlayerDownRouter::TryBeginNoThreatRescueFallback(
			player,
			scan,
			thresholdPct,
			reason,
			TFD::PlayerDownRouterOutcomeWiring::BuildNoThreatHandlers());
	}

	bool TryBeginNoThreatRescueFallback(RE::Actor* player, float thresholdPct, const char* reason)
	{
		return TryBeginNoThreatRescueFallback(player, ScanOutcome(player), thresholdPct, reason);
	}

	bool DispatchOutcome(RE::Actor* player, const ThresholdScan& scan, const ThresholdClassification& classification)
	{
		return TFD::PlayerDownRouter::DispatchThresholdOutcome(
			player,
			scan,
			classification,
			TFD::PlayerDownRouterOutcomeWiring::BuildDispatchHandlers());
	}

	bool DispatchImmediateBleedout(RE::Actor* player, float playerHpPct, float playerThreshold, const char* reason)
	{
		if (!player) {
			return false;
		}

		const auto dependencies = ResolveDependencies();
		const auto now = Now();
		if (g_thresholdScanImmediateDispatchLast.time_since_epoch().count() != 0 &&
			(now - g_thresholdScanImmediateDispatchLast) < std::chrono::milliseconds(900)) {
			spdlog::info(
				"[TFD][Defeat][R270A] threshold scan immediate bleedout skipped reason=recent_dispatch hpPct={:.1f} threshold={:.1f}",
				playerHpPct,
				std::clamp(playerThreshold, 2.0f, 95.0f));
			return false;
		}

		auto& flow = TFD::FlowController::Controller::GetSingleton();
		const bool flowDecisionActive = flow.IsBleedDecisionActive();
		const auto flowSnapshot = flow.GetSnapshot();
		const bool staleEscapeBreakBleedRuntime =
			IsInBleedState(dependencies) &&
			!flowDecisionActive &&
			flowSnapshot.root == TFD::FlowController::RootFlow::InCombat &&
			flowSnapshot.contextRoot == TFD::FlowController::RootFlow::Captive &&
			flowSnapshot.sub == TFD::FlowController::SubFlow::InCombatEscapeBreak;

		if (flowDecisionActive) {
			spdlog::info(
				"[TFD][Defeat][R270A] threshold scan immediate bleedout skipped reason=bleed_decision_active hpPct={:.1f} threshold={:.1f}",
				playerHpPct,
				std::clamp(playerThreshold, 2.0f, 95.0f));
			return false;
		}

		if (IsInBleedState(dependencies) && !staleEscapeBreakBleedRuntime) {
			spdlog::info(
				"[TFD][Defeat][R270A] threshold scan immediate bleedout skipped reason=bleed_runtime_active root={} ctx={} sub={} hpPct={:.1f} threshold={:.1f}",
				TFD::FlowController::Controller::ToString(flowSnapshot.root),
				TFD::FlowController::Controller::ToString(flowSnapshot.contextRoot),
				TFD::FlowController::Controller::ToString(flowSnapshot.sub),
				playerHpPct,
				std::clamp(playerThreshold, 2.0f, 95.0f));
			return false;
		}

		if (staleEscapeBreakBleedRuntime) {
			ReleaseStaleEscapeBreakBleedRuntime(dependencies, "r270_stale_escape_break_rebleed");
			spdlog::info(
				"[TFD][Defeat][R270A] stale escape-break bleed runtime released root={} ctx={} sub={} primary={:08X}",
				TFD::FlowController::Controller::ToString(flowSnapshot.root),
				TFD::FlowController::Controller::ToString(flowSnapshot.contextRoot),
				TFD::FlowController::Controller::ToString(flowSnapshot.sub),
				flowSnapshot.primaryActorFormID);
		}

		auto thresholdScan = ScanOutcome(player);
		const bool immediateThreat = player->IsInCombat() ||
			thresholdScan.initialAggressor != nullptr ||
			thresholdScan.hostileCoalitionStanding;
		if (!immediateThreat) {
			if (TryBeginNoThreatRescueFallback(player, thresholdScan, playerThreshold, reason ? reason : "threshold_scan_no_threat")) {
				g_thresholdScanImmediateDispatchLast = now;
				return true;
			}
			spdlog::info(
				"[TFD][Defeat][R270A] threshold scan immediate bleedout skipped reason=no_immediate_threat hpPct={:.1f} threshold={:.1f}",
				playerHpPct,
				std::clamp(playerThreshold, 2.0f, 95.0f));
			return false;
		}

		auto thresholdClassification = ClassifyOutcome(thresholdScan);
		const bool dispatched = DispatchOutcome(player, thresholdScan, thresholdClassification);
		spdlog::info(
			"[TFD][Defeat][R270A] threshold scan immediate bleedout dispatch dispatched={} staleEscapeBreak={} hpPct={:.1f} threshold={:.1f} root={} ctx={} sub={} speaker={:08X}",
			dispatched ? 1 : 0,
			staleEscapeBreakBleedRuntime ? 1 : 0,
			playerHpPct,
			std::clamp(playerThreshold, 2.0f, 95.0f),
			TFD::FlowController::Controller::ToString(flowSnapshot.root),
			TFD::FlowController::Controller::ToString(flowSnapshot.contextRoot),
			TFD::FlowController::Controller::ToString(flowSnapshot.sub),
			thresholdClassification.rememberedAggressor ? thresholdClassification.rememberedAggressor->GetFormID() : 0u);
		if (dispatched) {
			g_thresholdScanImmediateDispatchLast = now;
		}
		return dispatched;
	}

	bool HandlePlayerThreshold(RE::Actor* player, float playerHpPct, float playerThreshold, const char* reason)
	{
		if (!player) {
			return false;
		}

		auto thresholdScan = ScanOutcome(player);
		const bool immediateThreat = player->IsInCombat() ||
			thresholdScan.initialAggressor != nullptr ||
			thresholdScan.hostileCoalitionStanding;
		if (!immediateThreat) {
			if (TryBeginNoThreatRescueFallback(player, thresholdScan, playerThreshold, "player_threshold_no_threat")) {
				return true;
			}
			spdlog::info(
				"[TFD][Defeat] skip player threshold outcome reason=no_immediate_threat hpPct={:.1f} threshold={:.1f}",
				playerHpPct,
				playerThreshold);
			return true;
		}

		TFD::BleedLockRuntime::Enter(
			TFD::DefeatBleedLockWiring::BuildContext(),
			player,
			TFD::BleedLockState::Kind::Player,
			playerThreshold,
			reason ? reason : "player_threshold");
		auto thresholdClassification = ClassifyOutcome(thresholdScan);
		return DispatchOutcome(player, thresholdScan, thresholdClassification);
	}

	void ClearImmediateDispatchThrottle()
	{
		g_thresholdScanImmediateDispatchLast = {};
	}
}
