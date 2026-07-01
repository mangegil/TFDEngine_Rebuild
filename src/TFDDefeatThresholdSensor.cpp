#include "TFDDefeatThresholdSensor.h"

#include <algorithm>
#include <chrono>

#include <RE/A/ActorValues.h>
#include <spdlog/spdlog.h>

#include "TFDActor.h"
#include "TFDBleedLockState.h"
#include "TFDDefeatTerminalLockout.h"
#include "TFDPlayerThresholdOutcomeWiring.h"
#include "TFDSettings.h"
#include "TFDTeammateManager.h"
#include "TFDVictory.h"

namespace TFD::DefeatThresholdSensor
{
	namespace
	{
		std::chrono::steady_clock::time_point g_enemyThresholdScanLast{};

		static std::chrono::steady_clock::time_point Now()
		{
			return std::chrono::steady_clock::now();
		}

		static RE::PlayerCharacter* Player()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

		static bool IsActiveFollowerActor(RE::Actor* actor)
		{
			return TFD::TeammateManager::IsActiveFollowerActor(actor);
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
			auto& entries = TFD::BleedLockState::Entries();
			return entries.find(actor->GetFormID()) != entries.end();
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
	}

	void ResetEnemyTimer()
	{
		g_enemyThresholdScanLast = {};
	}

	void TickEnemy(const char* reason)
	{
		const auto now = Now();
		if (g_enemyThresholdScanLast.time_since_epoch().count() != 0 &&
			(now - g_enemyThresholdScanLast) < std::chrono::milliseconds(250)) {
			return;
		}
		g_enemyThresholdScanLast = now;
		ScanEnemyThresholdCandidatesOnly(reason && reason[0] ? reason : "enemy_threshold_sensor");
	}

	bool HandlePlayer(RE::Actor* player, const char* reason)
	{
		if (!player) {
			return false;
		}

		const float hpNow = player->GetActorValue(RE::ActorValue::kHealth);
		const float hpMax = (std::max)(1.0f, player->GetPermanentActorValue(RE::ActorValue::kHealth));
		const float pct = (hpNow / hpMax) * 100.0f;
		const float threshold = TFD::Settings::GetDefeatThresholdPct();
		if (pct > threshold) {
			return false;
		}

		const char* routeReason = reason && reason[0] ? reason : "player_threshold";
		if (TFD::DefeatTerminalLockout::IsActive(routeReason, pct, threshold)) {
			return true;
		}
		return TFD::PlayerThresholdOutcomeWiring::HandlePlayerThreshold(player, pct, threshold, routeReason);
	}

	bool IsDownedActor(RE::Actor* actor)
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

	bool IsCombatTargetValid(RE::Actor* actor)
	{
		if (!actor) {
			return false;
		}
		if (actor->IsDisabled() || actor->IsDead()) {
			return false;
		}
		return !IsDownedActor(actor);
	}
}
