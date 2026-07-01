#include <RE/Skyrim.h>
#include <RE/A/ActorValues.h>

#include "TFDDefeatActorQueries.h"

#include <algorithm>
#include <cmath>

#include "TFDActor.h"
#include "TFDBleedLockRuntime.h"
#include "TFDBleedLockState.h"
#include "TFDBleedout.h"
#include "TFDDefeatBattleObserveState.h"
#include "TFDDefeatBleedLockWiring.h"
#include "TFDFlowController.h"
#include "TFDSettings.h"
#include "TFDTame.h"
#include "TFDTeammateManager.h"

namespace TFD::DefeatActorQueries
{
	RE::PlayerCharacter* Player()
	{
		return RE::PlayerCharacter::GetSingleton();
	}

	std::uint32_t ResolveBleedFlowActorFormID()
	{
		const auto speakerId = TFD::Bleedout::GetBleedSpeakerID();
		if (speakerId != 0) {
			return speakerId;
		}
		return TFD::FlowController::Controller::GetSingleton().GetSnapshot().primaryActorFormID;
	}

	bool ActorHasLineOfSightToPlayer(RE::Actor* actor, RE::Actor* player)
	{
		if (!actor || !player) {
			return false;
		}
		bool hasLOSData = false;
		return actor->HasLineOfSight(player, hasLOSData);
	}

	bool IsBleedSpaceCompatible(RE::Actor* actor, RE::Actor* player)
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

	bool IsActorCloseAndFront(RE::Actor* actor, RE::Actor* player, float maxDist)
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

	bool IsObserverAlly(RE::Actor* actor)
	{
		return TFD::Bleedout::DefeatGlue::IsObserverAlly(actor);
	}

	bool IsTrackedDefeatedEnemy(RE::Actor* actor)
	{
		return TFD::DefeatBattleObserveState::IsTrackedEnemy(actor);
	}

	bool IsActorBleedingOut(RE::Actor* actor)
	{
		if (!actor) {
			return false;
		}
		if (auto* state = actor->AsActorState()) {
			return state->IsBleedingOut();
		}
		return false;
	}

	float GetActorHealthPct(RE::Actor* actor)
	{
		if (!actor) {
			return 0.0f;
		}
		const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
		const float hpNow = (std::max)(0.0f, actor->GetActorValue(RE::ActorValue::kHealth));
		return (hpNow / hpMax) * 100.0f;
	}

	bool HasActiveBleedLock(RE::Actor* actor)
	{
		if (!actor) {
			return false;
		}
		auto& locks = TFD::BleedLockState::Entries();
		return locks.find(actor->GetFormID()) != locks.end();
	}

	bool IsPlayerBleedLockActive(RE::Actor* player)
	{
		if (!player) {
			return false;
		}
		auto& locks = TFD::BleedLockState::Entries();
		auto it = locks.find(player->GetFormID());
		return it != locks.end() && it->second.kind == TFD::BleedLockState::Kind::Player;
	}

	bool IsActorDownByThreshold(RE::Actor* actor, float thresholdPct)
	{
		if (!actor || actor->IsDisabled() || actor->IsDead() || IsActorBleedingOut(actor)) {
			return true;
		}
		if (HasActiveBleedLock(actor) || TFD::Actor::Ops::IsDefeatedEnemyKnocked(actor)) {
			return true;
		}
		return GetActorHealthPct(actor) <= std::clamp(thresholdPct, 2.0f, 95.0f);
	}

	bool IsStandingAllyThresholdActor(RE::Actor* actor)
	{
		if (!actor || actor == Player()) {
			return false;
		}
		return !IsActorDownByThreshold(actor, TFD::Settings::GetAllyDownedThresholdPct());
	}

	bool IsStandingEnemyThresholdActor(RE::Actor* actor)
	{
		if (!actor || actor == Player()) {
			return false;
		}
		return !IsActorDownByThreshold(actor, TFD::Settings::GetEnemyDownedThresholdPct());
	}

	bool IsActiveFollowerActor(RE::Actor* actor)
	{
		return TFD::TeammateManager::IsActiveFollowerActor(actor);
	}

	RE::Actor* ResolveCurrentCombatTarget(RE::Actor* actor)
	{
		return TFD::Actor::GetCurrentTarget(actor);
	}

	bool IsPlayerSideActorForRouter(RE::Actor* actor, RE::Actor* player)
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

	bool IsActorActivelyTargetingPlayerSideForRouter(RE::Actor* actor, RE::Actor* player)
	{
		if (!actor || !player) {
			return false;
		}
		auto* currentTarget = ResolveCurrentCombatTarget(actor);
		return IsPlayerSideActorForRouter(currentTarget, player);
	}

	bool IsValidBleedBattleEnemyRosterActor(RE::Actor* actor, RE::Actor* player)
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

	std::vector<RE::Actor*> CollectLiveStandingObservedEnemies(RE::Actor* player, float radius, RE::Actor* preferredEnemy, const std::vector<RE::Actor*>& allies)
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

	FollowerResolution ResolveFollowerCandidates(float radius)
	{
		auto external = TFD::TeammateManager::ResolveFollowerCandidates(radius);
		FollowerResolution result{};
		result.standing = external.standing;
		result.downed = external.downed;
		return result;
	}

	std::vector<RE::Actor*> CollectBleedStandingFollowers(float radius)
	{
		return TFD::TeammateManager::CollectStandingFollowers(radius);
	}

	bool HasPlayerBleedLock()
	{
		return TFD::BleedLockRuntime::HasPlayer(TFD::DefeatBleedLockWiring::BuildContext());
	}
}
