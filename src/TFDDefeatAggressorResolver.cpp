#include "TFDDefeatAggressorResolver.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <utility>

#include <spdlog/spdlog.h>

#include "TFDActor.h"
#include "TFDPreCombatGreet.h"
#include "TFDSettings.h"
#include "TFDTame.h"

namespace TFD::DefeatAggressorResolver
{
	namespace
	{
		std::mutex g_providerLock;
		Dependencies g_dependencies{};
		bool g_hasProvider = false;

		RE::ActorHandle g_lastAggressor{};
		RE::ActorHandle g_lastEnemyTargetingPlayer{};
		RE::FormID g_lastEnemyTargetingPlayerFormID = 0;
		std::chrono::steady_clock::time_point g_lastEnemyTargetingPlayerSeen{};

		static std::chrono::steady_clock::time_point Now()
		{
			return std::chrono::steady_clock::now();
		}

		Dependencies ResolveDependencies()
		{
			std::scoped_lock lock(g_providerLock);
			return g_dependencies;
		}

		RE::PlayerCharacter* Player()
		{
			const auto dependencies = ResolveDependencies();
			if (dependencies.getPlayer) {
				return dependencies.getPlayer();
			}
			return RE::PlayerCharacter::GetSingleton();
		}

		RE::BGSKeyword* LookupKeyword(const char* editorID)
		{
			if (!editorID || !editorID[0]) {
				return nullptr;
			}
			return RE::TESForm::LookupByEditorID<RE::BGSKeyword>(editorID);
		}

		bool ActorHasKeywordByEditorID(RE::Actor* actor, const char* editorID)
		{
			if (!actor) {
				return false;
			}
			auto* kw = LookupKeyword(editorID);
			return kw && actor->HasKeyword(kw);
		}

		bool IsActiveFollower(RE::Actor* actor)
		{
			const auto dependencies = ResolveDependencies();
			return dependencies.isActiveFollowerActor ? dependencies.isActiveFollowerActor(actor) : false;
		}

		bool IsStandingEnemyThreshold(RE::Actor* actor)
		{
			const auto dependencies = ResolveDependencies();
			return dependencies.isStandingEnemyThresholdActor ? dependencies.isStandingEnemyThresholdActor(actor) : false;
		}

		bool IsObserverAlly(RE::Actor* actor)
		{
			const auto dependencies = ResolveDependencies();
			return dependencies.isObserverAlly ? dependencies.isObserverAlly(actor) : false;
		}
	}

	void InstallProvider(Dependencies dependencies)
	{
		std::scoped_lock lock(g_providerLock);
		g_dependencies = std::move(dependencies);
		g_hasProvider = true;
		spdlog::info("[TFD][AggressorResolver][P25] provider installed");
	}

	void ShutdownProvider()
	{
		std::scoped_lock lock(g_providerLock);
		g_dependencies = Dependencies{};
		g_hasProvider = false;
		spdlog::info("[TFD][AggressorResolver][P25] provider cleared");
	}

	bool HasProvider()
	{
		std::scoped_lock lock(g_providerLock);
		return g_hasProvider;
	}

	bool IsCaptiveSupportedAggressor(RE::Actor* actor)
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

	bool IsCombatSupportedAggressor(RE::Actor* actor)
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
		if (IsActiveFollower(actor) || TFD::Tame::IsCompanion(actor)) {
			return false;
		}
		return true;
	}

	bool IsReasonableCombatAggressor(RE::Actor* actor, RE::Actor* player, float maxDist, float* outDistance)
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
		if (!IsStandingEnemyThreshold(actor) || !actor->Is3DLoaded()) {
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
		if (currentTarget && IsActiveFollower(currentTarget)) {
			return true;
		}
		return false;
	}

	bool IsBleedCrowdSupportedAggressor(RE::Actor* actor)
	{
		if (!actor) {
			return false;
		}
		if (IsActiveFollower(actor)) {
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

	RE::Actor* FindBestAggressor(float radius)
	{
		return TFD::Actor::FindBestAggressor(radius, Player());
	}

	RE::Actor* ResolveAggressor()
	{
		const float radius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 400.0f);
		return TFD::Actor::ResolveAggressor(radius, Player());
	}

	void RememberAggressor(RE::Actor* actor)
	{
		if (actor && !IsObserverAlly(actor)) {
			g_lastAggressor = actor->GetHandle();
		}
	}

	void ClearLastAggressor()
	{
		g_lastAggressor = RE::ActorHandle{};
	}

	void SetLastAggressor(RE::Actor* actor)
	{
		g_lastAggressor = actor ? actor->GetHandle() : RE::ActorHandle{};
	}

	RE::Actor* ResolveLastAggressor()
	{
		if (!g_lastAggressor) {
			return nullptr;
		}
		auto sp = RE::Actor::LookupByHandle(g_lastAggressor.native_handle());
		return sp.get();
	}

	bool IsLastAggressor(RE::Actor* actor)
	{
		return actor && ResolveLastAggressor() == actor;
	}

	void NoteEnemyTargetingPlayer(RE::Actor* actor)
	{
		if (!actor || !IsCombatSupportedAggressor(actor)) {
			return;
		}
		g_lastEnemyTargetingPlayer = actor->GetHandle();
		g_lastEnemyTargetingPlayerFormID = actor->GetFormID();
		g_lastEnemyTargetingPlayerSeen = Now();
	}

	void ClearLastEnemyTargetingPlayer()
	{
		g_lastEnemyTargetingPlayer = {};
		g_lastEnemyTargetingPlayerFormID = 0;
		g_lastEnemyTargetingPlayerSeen = {};
	}

	bool HasRecentEnemyTargetingPlayer(double maxAgeSec)
	{
		if (g_lastEnemyTargetingPlayerFormID == 0 ||
			g_lastEnemyTargetingPlayerSeen == std::chrono::steady_clock::time_point{}) {
			return false;
		}

		const auto age = std::chrono::duration<double>(Now() - g_lastEnemyTargetingPlayerSeen).count();
		return age >= 0.0 && age <= maxAgeSec;
	}

	RE::Actor* ResolveLastEnemyTargetingPlayer(float radius, double maxAgeSec)
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
			NoteEnemyTargetingPlayer(recent);
			return recent;
		}
		return nullptr;
	}

	RE::Actor* ResolveCachedPlayerOverkillAttacker()
	{
		auto* player = Player();
		auto valid = [player](RE::Actor* actor) -> bool {
			return actor && actor != player && !actor->IsDead() && !actor->IsDisabled() && !IsObserverAlly(actor);
		};

		if (auto* actor = ResolveLastAggressor(); valid(actor)) {
			return actor;
		}

		if (g_lastEnemyTargetingPlayerFormID != 0) {
			if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(g_lastEnemyTargetingPlayerFormID); valid(actor)) {
				return actor;
			}
		}

		return nullptr;
	}

	void ResetForLoad()
	{
		ClearLastAggressor();
		ClearLastEnemyTargetingPlayer();
		TFD::Actor::Ops::ClearAggressorFactionContext();
	}
}
