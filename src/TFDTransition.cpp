#include "TFDTransition.h"
#include "TFDBleedout.h"
#include "TFDCaptive.h"
#include "TFDDefeatBridge.h"
#include "TFDActor.h"
#include "TFDFlowController.h"
#include "TFDLeftForDead.h"
#include "TFDRecovery.h"
#include "TFDRescue.h"

#include "EditorIdCache.h"
#include "TFDHostilityController.h"

#include "TFDLocation.h"
#include "TFDPleasureRuntime.h"
#include "TFDSettings.h"
#include "TFDTeammateManager.h"

#include <RE/L/LockpickingMenu.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <unordered_set>

namespace TFD::Transition
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		struct FallbackState
		{
			FallbackBranch branch{ FallbackBranch::None };
			RE::ActorHandle follower{};
			RE::ObjectRefHandle destination{};
			RE::NiPoint3 fallbackPos{};
			bool hasFallbackPos{ false };
			float angleZ{ 0.0f };
			RE::FormID potionFormId{ 0 };
		};

		struct FollowerResolution
		{
			RE::Actor* standing{ nullptr };
			RE::Actor* downed{ nullptr };
		};

		struct NoMarkerFallbackCandidates
		{
			RE::Actor* savior{ nullptr };
			RE::Actor* standingFollower{ nullptr };
			RE::Actor* downedFollower{ nullptr };
			RE::TESObjectREFR* cachedRescueDestination{ nullptr };
			RE::AlchemyItem* recoveryPotion{ nullptr };
		};

		bool g_pendingFadeIn = false;
		Kind g_pendingFadeInKind = Kind::None;
		Clock::time_point g_pendingFadeInNotBefore{};
		bool g_pendingFadeInSawLoadingMenu = false;

		FallbackState g_fallback{};
		RE::ActorHandle g_allyHoldFollower{};
		bool g_allyHoldActive = false;
		std::vector<RE::FormID> g_lockedFallbackCrowdIds{};
		bool g_leftForDeadActive = false;
		Clock::time_point g_leftForDeadUntil{};
		Clock::time_point g_leftForDeadNextPulse{};
		Clock::time_point g_leftForDeadPleasureDeferLast{};
		bool g_leftForDeadNeedsAggroKick = false;

		static RE::Actor* ResolvePlayer(const RuntimeHandlers& handlers)
		{
			if (handlers.getPlayer) {
				if (auto* player = handlers.getPlayer()) {
					return player;
				}
			}
			return RE::PlayerCharacter::GetSingleton();
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

		static bool IsStandingAllyThresholdActor(const RuntimeHandlers& handlers, RE::Actor* actor)
		{
			return handlers.isStandingAllyThresholdActor ? handlers.isStandingAllyThresholdActor(actor) : false;
		}

		static bool IsCombatSupportedAggressor(const RuntimeHandlers& handlers, RE::Actor* actor)
		{
			return handlers.isCombatSupportedAggressor ? handlers.isCombatSupportedAggressor(actor) : false;
		}

		static bool IsActiveFollowerActor(const RuntimeHandlers& handlers, RE::Actor* actor)
		{
			return handlers.isActiveFollowerActor ? handlers.isActiveFollowerActor(actor) : false;
		}

		static std::vector<RE::Actor*> CollectFallbackTeammates(const RuntimeHandlers& handlers, float radius)
		{
			std::vector<RE::Actor*> out;
			std::unordered_set<RE::FormID> seen;

			auto append = [&](RE::Actor* actor) {
				if (!actor || actor->IsDisabled()) {
					return;
				}
				const auto id = actor->GetFormID();
				if (!id || !seen.insert(id).second) {
					return;
				}
				out.push_back(actor);
			};

			if (handlers.collectRegisteredTeammates) {
				for (auto* actor : handlers.collectRegisteredTeammates()) {
					append(actor);
				}
			}

			for (auto* actor : TFD::TeammateManager::CollectKnownTeammates(radius)) {
				append(actor);
			}

			return out;
		}



		static void ApplyFallbackFacing(RE::Actor* actor, float angleZ)
		{
			if (!actor) {
				return;
			}
			actor->data.angle.z = angleZ;
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

		static RE::NiPoint3 ComputeCrowdCenterPoint(const RuntimeHandlers& handlers)
		{
			RE::NiPoint3 center{};
			auto* player = ResolvePlayer(handlers);
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

		static void LockCurrentBleedCrowdSnapshot(const RuntimeHandlers& handlers, RE::Actor* preferredSpeaker)
		{
			g_lockedFallbackCrowdIds.clear();
			if (handlers.getBleedCrowdAssigned) {
				g_lockedFallbackCrowdIds = handlers.getBleedCrowdAssigned();
			}
			if (g_lockedFallbackCrowdIds.empty() && handlers.collectBleedoutCrowd) {
				const float radius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius());
				auto crowd = handlers.collectBleedoutCrowd(radius, preferredSpeaker, true);
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

		static FollowerResolution ResolveFollowerCandidates(const RuntimeHandlers& handlers, float radius)
		{
			FollowerResolution result{};
			auto* player = ResolvePlayer(handlers);
			if (!player) {
				return result;
			}

			float bestStandingDist = std::numeric_limits<float>::max();
			float bestDownedDist = std::numeric_limits<float>::max();

			for (auto* actor : CollectFallbackTeammates(handlers, radius)) {
				if (!actor || actor == player) {
					continue;
				}
				const float dist = Distance3D(actor->GetPosition(), player->GetPosition());
				if (radius > 0.0f && dist > (std::max)(radius, 5000.0f)) {
					continue;
				}
				const bool standing = IsStandingAllyThresholdActor(handlers, actor);
				if (standing) {
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

		static RE::Actor* ResolveBestHumanoidSavior(const RuntimeHandlers& handlers, float radius)
		{
			auto* player = ResolvePlayer(handlers);
			if (!player) {
				return nullptr;
			}

			RE::Actor* best = nullptr;
			float bestDist = std::numeric_limits<float>::max();
			for (auto* actor : CollectFallbackTeammates(handlers, radius)) {
				if (!actor || actor == player || !IsStandingAllyThresholdActor(handlers, actor)) {
					continue;
				}
				if (!actor->HasKeywordString("ActorTypeNPC")) {
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

		static RE::AlchemyItem* ResolveRecoveryPotionCandidate()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
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

		static RE::TESObjectREFR* ResolveCachedRescueDestinationForFallback(const RuntimeHandlers& handlers)
		{
			auto* player = ResolvePlayer(handlers);
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

		static void ComputeLocalLeftForDeadFallback(const RuntimeHandlers& handlers, RE::NiPoint3& outPos, float& outAngleZ)
		{
			auto* player = ResolvePlayer(handlers);
			if (!player) {
				outPos = {};
				outAngleZ = 0.0f;
				return;
			}
			const auto playerPos = player->GetPosition();
			auto crowdCenter = ComputeCrowdCenterPoint(handlers);
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

		static void ResolveLeftForDeadDestination(const RuntimeHandlers& handlers, FallbackState& state)
		{
			state.destination.reset();
			state.hasFallbackPos = false;
			state.angleZ = 0.0f;

			auto* player = ResolvePlayer(handlers);
			auto* playerCell = player ? player->GetParentCell() : nullptr;
			if (!player || !playerCell) {
				return;
			}

			const bool exterior = playerCell->IsExteriorCell();
			const auto playerPos = player->GetPosition();
			const auto crowdCenter = ComputeCrowdCenterPoint(handlers);
			auto* playerLoc = TFD::Location::GetLocationFromRef(player);

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
				auto* refLoc = TFD::Location::GetLocationFromRef(ref);
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
				spdlog::info("[TFD][Transition] left-for-dead anchor selected ref={:08X} branch={} score={:.1f}",
					bestRef->GetFormID(),
					GetBranchName(state.branch),
					bestScore);
				return;
			}

			ComputeLocalLeftForDeadFallback(handlers, state.fallbackPos, state.angleZ);
			state.hasFallbackPos = true;
			spdlog::info("[TFD][Transition] left-for-dead anchor fallback=local branch={} pos=({:.1f},{:.1f},{:.1f})",
				GetBranchName(state.branch), state.fallbackPos.x, state.fallbackPos.y, state.fallbackPos.z);
		}

		static TFD::LeftForDead::State BuildLeftForDeadStateFromFallback()
		{
			TFD::LeftForDead::State state{};
			state.branch = g_fallback.branch;
			state.follower = g_fallback.follower;
			state.destination = g_fallback.destination;
			state.fallbackPos = g_fallback.fallbackPos;
			state.hasFallbackPos = g_fallback.hasFallbackPos;
			state.angleZ = g_fallback.angleZ;
			return state;
		}

		static TFD::LeftForDead::Handlers BuildLeftForDeadHandlers(const RuntimeHandlers& handlers)
		{
			auto handlersCopy = handlers;
			return {
				.applyFacing = [](RE::Actor* actor, float angleZ) { ApplyFallbackFacing(actor, angleZ); },
				.setFollowerHold = [](RE::Actor* actor) { SetFollowerHold(actor); },
				.maintainCalmWindow = [handlersCopy]() { MaintainCalmWindow(handlersCopy); },
				.beginCooldown = [](int seconds) { BeginLeftForDeadCooldown(seconds); },
				.setAggroKickNeeded = [](bool value) { g_leftForDeadNeedsAggroKick = value; }
			};
		}

		static void ExecuteLeftForDeadWake(const char* reason, const RuntimeHandlers& handlers)
		{
			TFD::LeftForDead::ExecuteWake(BuildLeftForDeadStateFromFallback(), reason, handlers, BuildLeftForDeadHandlers(handlers));
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

		static void QueuePostRecoveryAggroKick(const char* reason, const RuntimeHandlers& handlers)
		{
			auto* player = ResolvePlayer(handlers);
			if (!player) {
				return;
			}
			auto* pCell = player->GetParentCell();
			if (!pCell) {
				return;
			}

			const float radius = (std::max)(2200.0f, TFD::Settings::GetSweepRadius() + 400.0f);
			RE::Actor* primary = handlers.resolveAggressor ? handlers.resolveAggressor() : nullptr;
			if (!primary && handlers.findBestAggressor) {
				primary = handlers.findBestAggressor(radius);
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
				if (!IsCombatSupportedAggressor(handlers, actor)) {
					return;
				}

				const auto [_, inserted] = queuedIds.insert(actor->GetFormID());
				if (!inserted) {
					return;
				}

				const bool kickedNow = TFD::HostilityController::ForceDetectionAndCombatRefresh(
					actor,
					player,
					TFD::HostilityController::ReleaseReason::DialogueClosed,
					drawWeapon);
				TFD::HostilityController::QueueDetectionAndCombatRefresh(
					actor,
					player,
					TFD::HostilityController::ReleaseReason::DialogueClosed,
					drawWeapon);
				++queued;

				spdlog::info(
					"[TFD][Transition] post-recovery aggro kick actor={:08X} drawWeapon={} immediate={} reason={}"
					, actor->GetFormID(), drawWeapon ? 1 : 0, kickedNow ? 1 : 0, reason ? reason : "unknown");
				};

			if (primary) {
				queueOne(primary, true);
			}

			auto snapshot = TFD::Actor::BuildSnapshot(radius, false);
			for (const auto& info : snapshot.actors) {
				auto* actor = info.get();
				if (!actor || actor == primary) {
					continue;
				}
				if (info.dist > radius) {
					continue;
				}
				if (!info.hostileToPlayer && !info.inCombat && !actor->IsInCombat() && !actor->IsHostileToActor(player)) {
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
		}

		static void FinishLeftForDeadRecovery(const RuntimeHandlers& handlers)
		{
			RE::Actor* followerToRestore = nullptr;
			if (g_allyHoldFollower) {
				auto sp = RE::Actor::LookupByHandle(g_allyHoldFollower.native_handle());
				followerToRestore = sp.get();
			}
			else if (g_fallback.follower) {
				auto sp = RE::Actor::LookupByHandle(g_fallback.follower.native_handle());
				followerToRestore = sp.get();
			}
			TFD::HostilityController::ClearAggressionClamp();
			TFD::Actor::Ops::ClearAggressorFactionContext();
			if (g_leftForDeadNeedsAggroKick) {
				QueuePostRecoveryAggroKick("left_for_dead_recovery_finished", handlers);
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
			g_fallback = {};
			spdlog::info("[TFD][Transition] recovery finished");
		}
	}

	const char* GetKindName(Kind kind)
	{
		switch (kind) {
		case Kind::Captive:
			return "captive";
		case Kind::Rescue:
			return "rescue";
		case Kind::Recover:
			return "recover";
		default:
			return "none";
		}
	}

	const char* GetBranchName(FallbackBranch branch)
	{
		switch (branch) {
		case FallbackBranch::RecoveryFollower:
			return "recovery_follower";
		case FallbackBranch::RecoveryPotion:
			return "recovery_potion";
		case FallbackBranch::RescueCached:
			return "rescue_cached";
		case FallbackBranch::LeftForDeadSolo:
			return "left_for_dead_solo";
		case FallbackBranch::LeftForDeadWithFollower:
			return "left_for_dead_with_follower";
		default:
			return "none";
		}
	}

	FallbackBranch GetCurrentFallbackBranch()
	{
		return g_fallback.branch;
	}

	const char* GetCurrentFallbackBranchName()
	{
		return GetBranchName(g_fallback.branch);
	}

	bool QueueRequest(Kind kind, bool fadeIn, const char* reason)
	{
		spdlog::info("[TFD][Transition] cinematic request skipped (globals removed) kind={} phase={} reason={}",
			GetKindName(kind),
			fadeIn ? "fadein" : "fadeout",
			reason ? reason : "unknown");
		return false;
	}

	void ClearPendingFadeIn()
	{
		g_pendingFadeIn = false;
		g_pendingFadeInKind = Kind::None;
		g_pendingFadeInNotBefore = {};
		g_pendingFadeInSawLoadingMenu = false;
	}

	void ArmPendingFadeIn(Kind kind, std::chrono::steady_clock::time_point notBefore, bool sawLoadingMenu)
	{
		g_pendingFadeIn = kind != Kind::None;
		g_pendingFadeInKind = kind;
		g_pendingFadeInNotBefore = notBefore;
		g_pendingFadeInSawLoadingMenu = sawLoadingMenu;
		spdlog::info("[TFD][Transition] arm pending fadein kind={} sawLoading={} notBeforeSet={}",
			GetKindName(kind),
			sawLoadingMenu ? 1 : 0,
			g_pendingFadeIn ? 1 : 0);
	}

	bool HasPendingFadeIn()
	{
		return g_pendingFadeIn && g_pendingFadeInKind != Kind::None;
	}

	void ProcessPendingFadeIn()
	{
		if (!HasPendingFadeIn()) {
			return;
		}

		auto* ui = RE::UI::GetSingleton();
		const bool loadingOpen = ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
		if (loadingOpen) {
			g_pendingFadeInSawLoadingMenu = true;
			return;
		}

		if (Clock::now() < g_pendingFadeInNotBefore) {
			return;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->GetParentCell() : nullptr;
		if (!player || !cell) {
			return;
		}

		const auto kind = g_pendingFadeInKind;
		const bool sawLoading = g_pendingFadeInSawLoadingMenu;
		ClearPendingFadeIn();

		spdlog::info("[TFD][Transition] fadein ready kind={} cell={:08X} path={}",
			GetKindName(kind),
			cell->GetFormID(),
			sawLoading ? "after_loading_close" : "after_settle");

		if (!QueueRequest(kind, true, sawLoading ? "fadein_after_loading_close" : "fadein_after_settle")) {
			HideBlackoutFader();
		}
	}

	bool IsAwaiting()
	{
		return false;
	}

	void PollResult()
	{
		// Foundation only.
		// Legacy TFDTransition* globals have already been removed from the ESP side,
		// so there is no external transition result channel to poll yet.
	}

	bool BeginImmediate(Kind kind, const char* reason, const std::function<bool(const char*)>& completeNow)
	{
		ClearPendingFadeIn();
		if (QueueRequest(kind, false, reason)) {
			return true;
		}
		ShowBlackoutFader();
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		const bool ok = completeNow ? completeNow(reason) : false;
		std::this_thread::sleep_for(std::chrono::milliseconds(400));
		HideBlackoutFader();
		return ok;
	}

	void BeginImmediateVoid(Kind kind, const char* reason, const std::function<void(const char*)>& completeNow)
	{
		ClearPendingFadeIn();
		if (QueueRequest(kind, false, reason)) {
			return;
		}
		ShowBlackoutFader();
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		if (completeNow) {
			completeNow(reason);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(400));
		HideBlackoutFader();
	}

	void ShowBlackoutFader()
	{
		auto* queue = RE::UIMessageQueue::GetSingleton();
		auto* strings = RE::InterfaceStrings::GetSingleton();
		if (!queue || !strings) {
			return;
		}
		queue->AddMessage(strings->faderMenu, RE::UI_MESSAGE_TYPE::kShow, nullptr);
		queue->ProcessCommands();
	}

	void HideBlackoutFader()
	{
		auto* queue = RE::UIMessageQueue::GetSingleton();
		auto* strings = RE::InterfaceStrings::GetSingleton();
		if (!queue || !strings) {
			return;
		}
		queue->AddMessage(strings->faderMenu, RE::UI_MESSAGE_TYPE::kHide, nullptr);
		queue->ProcessCommands();
	}

	bool HasRecoveryPotionAvailable()
	{
		return ResolveRecoveryPotionCandidate() != nullptr;
	}

	bool IsRecoveryActive()
	{
		return g_leftForDeadActive;
	}

	void ClearNoMarkerFallbackState()
	{
		g_fallback = {};
		g_allyHoldFollower.reset();
		g_allyHoldActive = false;
		g_lockedFallbackCrowdIds.clear();
		TFD::DefeatBridge::ClearPlayerSavior(nullptr, "clear_no_marker_fallback");
	}

	void AbortCalmWindowForCombat(RE::Actor* actor, const char* reason)
	{
		const char* why = reason ? reason : "combat_owner_abort_calm_window";
		const bool hadLeftForDead = g_leftForDeadActive ||
			g_leftForDeadUntil.time_since_epoch().count() != 0 ||
			g_leftForDeadNextPulse.time_since_epoch().count() != 0;
		const bool hadFallback = g_fallback.branch != FallbackBranch::None ||
			g_fallback.follower ||
			g_fallback.destination ||
			g_fallback.hasFallbackPos ||
			g_fallback.potionFormId != 0;
		const bool hadFollowerHold = g_allyHoldActive || static_cast<bool>(g_allyHoldFollower);
		const std::size_t lockedCrowdBefore = g_lockedFallbackCrowdIds.size();

		g_leftForDeadActive = false;
		g_leftForDeadUntil = {};
		g_leftForDeadNextPulse = {};
		g_leftForDeadPleasureDeferLast = {};
		g_leftForDeadNeedsAggroKick = false;
		g_fallback = {};
		g_allyHoldFollower.reset();
		g_allyHoldActive = false;
		g_lockedFallbackCrowdIds.clear();

		TFD::HostilityController::ClearAggressionClamp();

		if (auto* process = RE::ProcessLists::GetSingleton()) {
			process->runDetection = true;
			process->ClearCachedFactionFightReactions();
		}

		if (actor && !actor->IsDead() && !actor->IsDisabled()) {
			actor->AllowPCDialogue(true);
		}

		if (hadLeftForDead || hadFallback || hadFollowerHold || lockedCrowdBefore > 0 || actor) {
			spdlog::info(
				"[TFD][Transition][R220A] calm window aborted for combat actor={:08X} leftForDead={} fallback={} followerHold={} lockedCrowd={} reason={}",
				actor ? actor->GetFormID() : 0u,
				hadLeftForDead ? 1 : 0,
				hadFallback ? 1 : 0,
				hadFollowerHold ? 1 : 0,
				static_cast<unsigned int>(lockedCrowdBefore),
				why);
		}
	}

	void BeginLeftForDeadCooldown(int seconds)
	{
		if (seconds <= 0) {
			g_leftForDeadActive = false;
			g_leftForDeadUntil = {};
			g_leftForDeadNextPulse = {};
			g_leftForDeadPleasureDeferLast = {};
			return;
		}
		const auto now = Clock::now();
		g_leftForDeadActive = true;
		g_leftForDeadUntil = now + std::chrono::seconds(seconds);
		g_leftForDeadNextPulse = now;
		g_leftForDeadPleasureDeferLast = {};
	}

	bool IsLeftForDeadCooldownActive(const RuntimeHandlers& handlers)
	{
		if (!g_leftForDeadActive) {
			return false;
		}

		const auto flow = TFD::FlowController::Controller::GetSingleton().GetSnapshot();

		if (flow.root == TFD::FlowController::RootFlow::InCombat ||
			flow.sub == TFD::FlowController::SubFlow::InCombatEscapeBreak) {
			auto* combatActor = flow.primaryActorFormID ? RE::TESForm::LookupByID<RE::Actor>(flow.primaryActorFormID) : nullptr;
			AbortCalmWindowForCombat(combatActor, "left_for_dead_cooldown_incombat_guard");
			return false;
		}

		const auto now = Clock::now();
		if (now >= g_leftForDeadUntil) {
			if (TFD::PleasureRuntime::IsPassiveLockActive()) {
				g_leftForDeadUntil = now + std::chrono::seconds(1);
				if (g_leftForDeadPleasureDeferLast.time_since_epoch().count() == 0 ||
					(now - g_leftForDeadPleasureDeferLast) >= std::chrono::seconds(2)) {
					g_leftForDeadPleasureDeferLast = now;
					spdlog::info("[TFD][Transition] recovery deferred by pleasure lock phase={} active={} blocking={}",
						TFD::PleasureRuntime::GetPhaseName(),
						TFD::PleasureRuntime::IsActive() ? 1 : 0,
						TFD::PleasureRuntime::IsBlocking() ? 1 : 0);
				}
				return true;
			}
			FinishLeftForDeadRecovery(handlers);
			return false;
		}
		return true;
	}

	void TickLeftForDeadCooldown(const RuntimeHandlers& handlers)
	{
		if (!IsLeftForDeadCooldownActive(handlers)) {
			return;
		}
		MaintainFollowerHold();
		const auto now = Clock::now();
		if (g_leftForDeadNextPulse.time_since_epoch().count() != 0 && now < g_leftForDeadNextPulse) {
			return;
		}
		g_leftForDeadNextPulse = now + std::chrono::milliseconds(900);
		MaintainCalmWindow(handlers);
	}

	void ClearLeftForDeadCooldown(const RuntimeHandlers& handlers)
	{
		if (g_leftForDeadActive) {
			FinishLeftForDeadRecovery(handlers);
			return;
		}
		g_leftForDeadActive = false;
		g_leftForDeadUntil = {};
		g_leftForDeadNextPulse = {};
		g_leftForDeadNeedsAggroKick = false;
		ClearNoMarkerFallbackState();
		if (handlers.setRescueStateValue) {
			handlers.setRescueStateValue(0);
		}
		if (handlers.refreshPostDefeatGlobals) {
			handlers.refreshPostDefeatGlobals();
		}
	}

	void MaintainCalmWindow(const RuntimeHandlers& handlers)
	{
		auto* player = ResolvePlayer(handlers);
		if (!player) {
			return;
		}

		const auto flow = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
		if (TFD::PleasureRuntime::IsActive() &&
			TFD::PleasureRuntime::GetSourceContext() == TFD::PleasureRuntime::SourceContext::Bleedout) {
			const auto phase = TFD::PleasureRuntime::GetPhase();
			if (phase == TFD::PleasureRuntime::Phase::PleasureStartPending ||
				phase == TFD::PleasureRuntime::Phase::PleasureActive ||
				phase == TFD::PleasureRuntime::Phase::PleasureFailedDialogue) {
				// R454A: Do not skip the calm window during Bleedout-source
				// Pleasure. OStim may end/abort if hostile actors leak back into
				// combat. Also do not call hostile-intrusion abort here; if the
				// scene ends without climax, PleasureFailed forcegreet owns the next
				// state until the player explicitly chooses Fight.
				const float radius = (std::max)(3200.0f, TFD::Settings::GetSweepRadius() + 1200.0f);
				if (player->IsInCombat()) {
					player->StopCombat();
				}
				player->DrawWeaponMagicHands(false);
				TFD::HostilityController::StopCombatSweep(radius, false);
				TFD::HostilityController::ScheduleStopCombatWaves(radius, false, 3, 75);
				spdlog::info(
					"[TFD][Transition][R454A] calm window maintained during bleedout-source pleasure source={} phase={} root={} sub={} primary={:08X} radius={:.1f}",
					TFD::PleasureRuntime::GetSourceContextName(),
					TFD::PleasureRuntime::GetPhaseName(),
					TFD::FlowController::Controller::ToString(flow.root),
					TFD::FlowController::Controller::ToString(flow.sub),
					flow.primaryActorFormID,
					radius);
				return;
			}
		}
		if (flow.sub == TFD::FlowController::SubFlow::PleasureFailedDialogue) {
			spdlog::info(
				"[TFD][Transition][R247A1] calm window skipped during pleasure failed dialogue root={} sub={} primary={:08X}",
				TFD::FlowController::Controller::ToString(flow.root),
				TFD::FlowController::Controller::ToString(flow.sub),
				flow.primaryActorFormID);
			return;
		}
		if (flow.root == TFD::FlowController::RootFlow::InCombat ||
			flow.sub == TFD::FlowController::SubFlow::InCombatEscapeBreak) {
			auto* combatActor = flow.primaryActorFormID ? RE::TESForm::LookupByID<RE::Actor>(flow.primaryActorFormID) : nullptr;
			AbortCalmWindowForCombat(combatActor, "maintain_calm_window_incombat_guard");
			spdlog::info(
				"[TFD][Transition][R220A] calm window skipped during combat root={} sub={} primary={:08X}",
				TFD::FlowController::Controller::ToString(flow.root),
				TFD::FlowController::Controller::ToString(flow.sub),
				flow.primaryActorFormID);
			return;
		}

		const float radius = (std::max)(3200.0f, TFD::Settings::GetSweepRadius() + 1200.0f);
		if (handlers.tryAbortPleasureDueToHostileIntrusion && handlers.tryAbortPleasureDueToHostileIntrusion(radius)) {
			return;
		}
		if (player->IsInCombat()) {
			player->StopCombat();
		}
		player->DrawWeaponMagicHands(false);
		TFD::HostilityController::StopCombatSweep(radius, false);

		std::size_t applied = 0;
		if (!g_lockedFallbackCrowdIds.empty()) {
			for (auto id : g_lockedFallbackCrowdIds) {
				auto* actorRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				auto* actor = actorRef ? actorRef->As<RE::Actor>() : nullptr;
				if (!actor || actor->IsDead() || actor->IsDisabled() || IsActiveFollowerActor(handlers, actor)) {
					continue;
				}
				TFD::HostilityController::ApplyAggressionClamp(actor);
				actor->StopCombat();
				actor->EvaluatePackage(false, true);
				actor->EvaluatePackage(true, true);
				++applied;
			}
		}
		else {
			auto snapshot = TFD::Actor::BuildSnapshot(radius, false);
			for (const auto& info : snapshot.actors) {
				auto* actor = info.get();
				if (!actor || actor->IsDead() || actor->IsDisabled() || IsActiveFollowerActor(handlers, actor)) {
					continue;
				}
				TFD::HostilityController::ApplyAggressionClamp(actor);
				actor->StopCombat();
				actor->EvaluatePackage(false, true);
				actor->EvaluatePackage(true, true);
				++applied;
			}
		}

		MaintainFollowerHold();
		spdlog::info("[TFD][Transition] calm window maintained applied={} branch={} lockedCrowd={}",
			applied,
			GetBranchName(g_fallback.branch),
			g_lockedFallbackCrowdIds.size());
	}

	void RecoverPlayerForTransition(const RuntimeHandlers& handlers)
	{
		auto* player = ResolvePlayer(handlers);
		if (!player) {
			return;
		}

		if (handlers.releasePlayerBleedLock) {
			handlers.releasePlayerBleedLock("recover_for_transition", false);
		}

		player->NotifyAnimationGraph("BleedoutStop");
		player->NotifyAnimationGraph("GetUpStart");

		auto restoreToPct = [&](RE::ActorValue av, float pct, float minValue) {
			const float maxValue = (std::max)(1.0f, player->GetPermanentActorValue(av));
			const float target = (std::max)(minValue, maxValue * pct);
			const float current = player->GetActorValue(av);
			if (current < target) {
				player->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, av, target - current);
			}
			};

		const float threshPct = std::clamp(TFD::Settings::GetDefeatThresholdPct() / 100.0f, 0.05f, 0.95f);
		const float safeHealthPct = std::clamp(threshPct + 0.12f, 0.58f, 1.00f);
		restoreToPct(RE::ActorValue::kHealth, safeHealthPct, 45.0f);
		restoreToPct(RE::ActorValue::kStamina, 0.98f, 40.0f);
		restoreToPct(RE::ActorValue::kMagicka, 0.95f, 25.0f);

		if (player->IsInCombat()) {
			player->StopCombat();
		}
		player->DrawWeaponMagicHands(false);
	}

	void RecoverPlayerAfterTeleport(const RuntimeHandlers& handlers)
	{
		auto* player = ResolvePlayer(handlers);
		if (!player) {
			return;
		}

		if (handlers.releasePlayerBleedLock) {
			handlers.releasePlayerBleedLock("recover_after_teleport", false);
		}

		player->NotifyAnimationGraph("BleedoutStop");
		player->NotifyAnimationGraph("GetUpStart");

		const float hpMax = (std::max)(1.0f, player->GetPermanentActorValue(RE::ActorValue::kHealth));
		const float threshPct = std::clamp(TFD::Settings::GetDefeatThresholdPct() / 100.0f, 0.05f, 0.95f);
		const float safePct = std::clamp(threshPct + 0.17f, 0.38f, 0.85f);
		const float healthTarget = (std::max)(45.0f, hpMax * safePct);
		const float hpNow = player->GetActorValue(RE::ActorValue::kHealth);
		if (hpNow < healthTarget) {
			player->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, healthTarget - hpNow);
		}

		const float staminaMax = (std::max)(1.0f, player->GetPermanentActorValue(RE::ActorValue::kStamina));
		const float staminaTarget = (std::max)(30.0f, staminaMax * 0.40f);
		const float staminaNow = player->GetActorValue(RE::ActorValue::kStamina);
		if (staminaNow < staminaTarget) {
			player->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina, staminaTarget - staminaNow);
		}

		if (player->IsInCombat()) {
			player->StopCombat();
		}
		player->DrawWeaponMagicHands(false);
	}

	FallbackBranch ResolveNoMarkerFallback(const char* reason, const RuntimeHandlers& handlers)
	{
		(void)reason;
		g_fallback = {};

		RE::Actor* preferredSpeaker = handlers.resolveAggressor ? handlers.resolveAggressor() : nullptr;
		LockCurrentBleedCrowdSnapshot(handlers, preferredSpeaker);

		const float followerRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 400.0f);
		NoMarkerFallbackCandidates candidates{};
		candidates.savior = ResolveBestHumanoidSavior(handlers, followerRadius);
		auto followers = ResolveFollowerCandidates(handlers, followerRadius);
		candidates.standingFollower = followers.standing;
		candidates.downedFollower = followers.downed;
		candidates.cachedRescueDestination = ResolveCachedRescueDestinationForFallback(handlers);
		candidates.recoveryPotion = ResolveRecoveryPotionCandidate();

		const TFD::FlowController::NonCaptiveFallbackInput input{
			.hasSavior = candidates.savior != nullptr,
			.hasStandingFollower = candidates.standingFollower != nullptr,
			.hasDownedFollower = candidates.downedFollower != nullptr,
			.hasRecoveryPotion = candidates.recoveryPotion != nullptr,
			.hasCachedRescueDestination = candidates.cachedRescueDestination != nullptr
		};
		const auto resolution = TFD::FlowController::EvaluateNonCaptiveFallback(input);

		switch (resolution) {
		case TFD::FlowController::NonCaptiveFallbackResolution::RescueCached:
			g_fallback.branch = FallbackBranch::RescueCached;
			g_fallback.follower = candidates.savior ? candidates.savior->GetHandle() : RE::ActorHandle{};
			g_fallback.destination = candidates.cachedRescueDestination ? candidates.cachedRescueDestination->GetHandle() : RE::ObjectRefHandle{};
			break;
		case TFD::FlowController::NonCaptiveFallbackResolution::RecoveryFollower: {
			g_fallback.branch = FallbackBranch::RecoveryFollower;
			RE::Actor* follower = candidates.savior ? candidates.savior : candidates.standingFollower;
			g_fallback.follower = follower ? follower->GetHandle() : RE::ActorHandle{};
			break;
		}
		case TFD::FlowController::NonCaptiveFallbackResolution::RecoveryPotion:
			g_fallback.branch = FallbackBranch::RecoveryPotion;
			g_fallback.potionFormId = candidates.recoveryPotion ? candidates.recoveryPotion->GetFormID() : 0u;
			break;
		case TFD::FlowController::NonCaptiveFallbackResolution::LeftForDeadWithFollower:
			g_fallback.branch = FallbackBranch::LeftForDeadWithFollower;
			g_fallback.follower = candidates.downedFollower ? candidates.downedFollower->GetHandle() : RE::ActorHandle{};
			ResolveLeftForDeadDestination(handlers, g_fallback);
			break;
		case TFD::FlowController::NonCaptiveFallbackResolution::LeftForDeadSolo:
			g_fallback.branch = FallbackBranch::LeftForDeadSolo;
			ResolveLeftForDeadDestination(handlers, g_fallback);
			break;
		case TFD::FlowController::NonCaptiveFallbackResolution::None:
		default:
			g_fallback.branch = FallbackBranch::None;
			break;
		}

		RE::FormID followerId = 0;
		if (g_fallback.follower) {
			auto followerSp = RE::Actor::LookupByHandle(g_fallback.follower.native_handle());
			if (auto* follower = followerSp.get()) {
				followerId = follower->GetFormID();
			}
		}
		RE::FormID destId = 0;
		if (g_fallback.destination) {
			auto destSp = g_fallback.destination.get();
			if (auto* dest = destSp.get()) {
				destId = dest->GetFormID();
			}
		}
		spdlog::info("[TFD][Transition] no-marker fallback resolved branch={} policy={} follower={:08X} potion={:08X} dest={:08X} crowdLocked={} savior={} standingFollower={} downedFollower={} rescueDest={} recoveryPotion={}",
			GetBranchName(g_fallback.branch),
			TFD::FlowController::ToString(resolution),
			followerId,
			g_fallback.potionFormId,
			destId,
			g_lockedFallbackCrowdIds.size(),
			candidates.savior ? 1 : 0,
			candidates.standingFollower ? 1 : 0,
			candidates.downedFollower ? 1 : 0,
			candidates.cachedRescueDestination ? 1 : 0,
			candidates.recoveryPotion ? 1 : 0);

		return g_fallback.branch;
	}


	void ArmObservedLeftForDeadFallback(RE::Actor* follower, const RuntimeHandlers& handlers)
	{
		g_fallback = {};
		g_fallback.branch = follower ? FallbackBranch::LeftForDeadWithFollower : FallbackBranch::LeftForDeadSolo;
		g_fallback.follower = follower ? follower->GetHandle() : RE::ActorHandle{};
		ResolveLeftForDeadDestination(handlers, g_fallback);
	}

	void ForceLeftForDeadSolo(const RuntimeHandlers& handlers)
	{
		g_fallback.branch = FallbackBranch::LeftForDeadSolo;
		g_fallback.destination.reset();
		g_fallback.hasFallbackPos = false;
		g_fallback.follower.reset();
		ResolveLeftForDeadDestination(handlers, g_fallback);
	}

	bool ResolveCaptiveMarkerForOutcome(const RuntimeHandlers& handlers)
	{
		auto* aggressor = handlers.resolveAggressor ? handlers.resolveAggressor() : nullptr;
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

	bool TeleportPlayerToCachedMarkerNow(const RuntimeHandlers& handlers)
	{
		auto* player = ResolvePlayer(handlers);
		auto* marker = TFD::Location::GetCachedCaptiveMarker();
		if (!player || !marker) {
			return false;
		}
		player->MoveTo(marker);
		spdlog::info("[TFD][Transition] direct MoveTo cached captive marker {:08X}", marker->GetFormID());
		return true;
	}

	bool CompleteCaptiveTransitionNow(const char* reason, const RuntimeHandlers& handlers, const CaptiveHandlers& captiveHandlers)
	{
		ClearNoMarkerFallbackState();
		if (captiveHandlers.resetBleedRuntimeState) {
			captiveHandlers.resetBleedRuntimeState();
		}
		if (captiveHandlers.clearBridgeAliases) {
			captiveHandlers.clearBridgeAliases("blackout_teleport");
		}
		if (captiveHandlers.clearLastAggressor) {
			captiveHandlers.clearLastAggressor();
		}
		AdvanceGameHoursSoft(1.0f);
		if (!TeleportPlayerToCachedMarkerNow(handlers)) {
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(120));
		RecoverPlayerAfterTeleport(handlers);
		if (handlers.setGraceSeconds) {
			handlers.setGraceSeconds(5);
		}
		const char* captiveReason = reason ? reason : "captive_enter";
		auto* playerAfterTeleport = ResolvePlayer(handlers);

		if (captiveHandlers.beginCaptiveFlow) {
			captiveHandlers.beginCaptiveFlow(captiveReason);
		}

		const bool usedRuntimeHandler = static_cast<bool>(captiveHandlers.setCaptiveRuntimeCaptive);
		if (captiveHandlers.setCaptiveRuntimeCaptive) {
			captiveHandlers.setCaptiveRuntimeCaptive();
		}
		else {
			// Captive transition can be started outside the old bleedout provider path
			// (for example PreCombat -> Captive). In that route the provider table can
			// be empty, so arm the captive runtime directly instead of only moving the
			// player and flipping FlowController globals.
			TFD::Captive::SetRuntimeState(true, TFD::Captive::PhaseValue::Captive);
		}

		if (captiveHandlers.setPrevDialogueOpen && captiveHandlers.isDialogueOpen) {
			captiveHandlers.setPrevDialogueOpen(captiveHandlers.isDialogueOpen());
		}

		const bool usedCaptureHandler = static_cast<bool>(captiveHandlers.captureCurrentLockpickMenuState);
		if (captiveHandlers.captureCurrentLockpickMenuState) {
			captiveHandlers.captureCurrentLockpickMenuState();
		}
		else {
			TFD::Captive::CaptureCurrentLockpickMenuState();
		}

		const bool usedResetLockpickHandler = static_cast<bool>(captiveHandlers.resetLockpickWatch);
		if (captiveHandlers.resetLockpickWatch) {
			captiveHandlers.resetLockpickWatch();
		}
		else {
			TFD::Captive::ResetLockpickWatch();
		}

		const bool usedEscapeContextHandler = static_cast<bool>(captiveHandlers.armEscapeContextFromCurrentState);
		if (captiveHandlers.armEscapeContextFromCurrentState) {
			captiveHandlers.armEscapeContextFromCurrentState();
		}
		else {
			TFD::Captive::ArmEscapeContextFromCurrentState(playerAfterTeleport);
		}

		const bool usedDoorHandler = static_cast<bool>(captiveHandlers.sealCaptiveDoorIfPresent);
		if (captiveHandlers.sealCaptiveDoorIfPresent) {
			captiveHandlers.sealCaptiveDoorIfPresent();
		}
		else {
			TFD::Captive::SealDoorIfPresent();
		}

		if (captiveHandlers.applyCalmBubble) {
			captiveHandlers.applyCalmBubble((std::max)(2000.0f, TFD::Settings::GetSweepRadius()));
		}

		const bool usedConfiscationHandler = static_cast<bool>(captiveHandlers.queuePendingCaptiveConfiscation);
		if (captiveHandlers.queuePendingCaptiveConfiscation) {
			captiveHandlers.queuePendingCaptiveConfiscation(captiveReason, true);
		}
		else {
			TFD::Captive::QueuePendingConfiscation(captiveReason, true);
		}

		const bool usedAliasHandler = static_cast<bool>(captiveHandlers.syncPlayerCaptiveAlias);
		if (captiveHandlers.syncPlayerCaptiveAlias) {
			captiveHandlers.syncPlayerCaptiveAlias(playerAfterTeleport, captiveReason);
		}
		else {
			TFD::Captive::SyncPlayerAlias(playerAfterTeleport, captiveReason);
		}

		TFD::HostilityController::TickCaptiveSuppression();

		spdlog::info(
			"[TFD][Transition] captive transition complete reason={} runtimeArmed=1 player={:08X} marker={:08X} handler(runtime={} capture={} lockpick={} escapeCtx={} door={} confiscation={} alias={}) starterKitPending=1",
			captiveReason,
			playerAfterTeleport ? playerAfterTeleport->GetFormID() : 0u,
			TFD::Location::GetCachedCaptiveMarker() ? TFD::Location::GetCachedCaptiveMarker()->GetFormID() : 0u,
			usedRuntimeHandler ? 1 : 0,
			usedCaptureHandler ? 1 : 0,
			usedResetLockpickHandler ? 1 : 0,
			usedEscapeContextHandler ? 1 : 0,
			usedDoorHandler ? 1 : 0,
			usedConfiscationHandler ? 1 : 0,
			usedAliasHandler ? 1 : 0);

		return true;
	}

	void FinalizePostDefeatRecoveryWindow(const RuntimeHandlers& handlers, int graceSeconds, int rescueStateValue)
	{
		MaintainCalmWindow(handlers);
		g_leftForDeadNeedsAggroKick = false;
		BeginLeftForDeadCooldown(graceSeconds);
		if (handlers.setGraceSeconds) {
			handlers.setGraceSeconds(graceSeconds);
		}
		if (handlers.setRescueStateValue) {
			handlers.setRescueStateValue(rescueStateValue);
		}
		if (handlers.refreshPostDefeatGlobals) {
			handlers.refreshPostDefeatGlobals();
		}
		if (handlers.updatePreCombatState) {
			handlers.updatePreCombatState();
		}
	}

	static TFD::Rescue::State BuildRescueStateFromFallback()
	{
		TFD::Rescue::State state{};
		state.branch = g_fallback.branch;
		state.follower = g_fallback.follower;
		state.destination = g_fallback.destination;
		return state;
	}

	static TFD::Rescue::Handlers BuildRescueHandlers(const RuntimeHandlers& handlers)
	{
		auto handlersCopy = handlers;
		return {
			.getPlayer = [handlersCopy]() { return ResolvePlayer(handlersCopy); },
			.recoverPlayerForTransition = [handlersCopy]() { RecoverPlayerForTransition(handlersCopy); },
			.finalizePostDefeatRecoveryWindow = [handlersCopy](int graceSeconds, int rescueStateValue) {
				FinalizePostDefeatRecoveryWindow(handlersCopy, graceSeconds, rescueStateValue);
			},
			.assignPlayerSavior = [](RE::Actor* follower) {
				if (!follower) {
					return;
				}
				TFD::DefeatBridge::AssignPlayerSavior(follower);
				follower->EvaluatePackage(false, true);
				follower->EvaluatePackage(true, true);
			}
		};
	}

	static TFD::Recovery::State BuildRecoveryStateFromFallback()
	{
		TFD::Recovery::State state{};
		state.branch = g_fallback.branch;
		state.follower = g_fallback.follower;
		state.potionFormId = g_fallback.potionFormId;
		return state;
	}

	static TFD::Recovery::Handlers BuildRecoveryHandlers(const RuntimeHandlers& handlers)
	{
		auto handlersCopy = handlers;
		return {
			.clearPlayerSavior = [](RE::TESForm* sender, const char* why) { TFD::DefeatBridge::ClearPlayerSavior(sender, why); },
			.recoverPlayerForTransition = [handlersCopy]() { RecoverPlayerForTransition(handlersCopy); },
			.setFollowerHold = [](RE::Actor* follower) { SetFollowerHold(follower); },
			.finalizePostDefeatRecoveryWindow = [handlersCopy](int graceSeconds, int rescueStateValue) {
				FinalizePostDefeatRecoveryWindow(handlersCopy, graceSeconds, rescueStateValue);
			}
		};
	}

	void ExecuteResolvedRecoverBranch(const char* reason, const RuntimeHandlers& handlers)
	{
		TFD::Recovery::ExecuteResolvedBranch(
			BuildRecoveryStateFromFallback(),
			reason,
			BuildRecoveryHandlers(handlers),
			[&](const char* why) {
				auto followerSp = RE::Actor::LookupByHandle(g_fallback.follower.native_handle());
				auto* follower = followerSp.get();
				g_fallback.branch = follower ? FallbackBranch::LeftForDeadWithFollower : FallbackBranch::LeftForDeadSolo;
				ResolveLeftForDeadDestination(handlers, g_fallback);
				ExecuteLeftForDeadWake(why ? why : "recovery_follower_degraded_lfd", handlers);
			},
			[](FallbackBranch branch) { return GetBranchName(branch); });
	}

	bool BeginRescueTransition(const char* reason, const RuntimeHandlers& handlers)
	{
		if (handlers.setRescueStateValue) {
			handlers.setRescueStateValue(0);
		}
		return BeginImmediate(
			Kind::Rescue,
			reason,
			[&](const char* why) {
				return TFD::Rescue::ExecuteResolvedBranch(
					BuildRescueStateFromFallback(),
					why,
					BuildRescueHandlers(handlers),
					[](FallbackBranch branch) { return GetBranchName(branch); });
			});
	}

	void BeginRecoverTransition(const char* reason, const RuntimeHandlers& handlers)
	{
		if (handlers.setRescueStateValue) {
			handlers.setRescueStateValue(0);
		}
		BeginImmediateVoid(
			Kind::Recover,
			reason,
			[&](const char* why) {
				ExecuteResolvedRecoverBranch(why, handlers);
			});
	}
}
// Consolidated from former TFDDefeatTransitionGlue staging module
namespace TFD::Transition::DefeatGlue
{
	namespace
	{
		TFD::Bleedout::Builders::NonCaptiveChoiceProvider g_nonCaptiveChoice{};
		TFD::Bleedout::Builders::BlackoutProvider g_blackout{};
		TFD::Bleedout::Builders::TransitionRuntimeProvider g_transitionRuntime{};
		TFD::Bleedout::Builders::TransitionCaptiveProvider g_transitionCaptive{};
		RuntimeProviders g_runtime{};
	}

	void InstallProviders(
		TFD::Bleedout::Builders::NonCaptiveChoiceProvider nonCaptiveChoice,
		TFD::Bleedout::Builders::BlackoutProvider blackout,
		TFD::Bleedout::Builders::TransitionRuntimeProvider transitionRuntime,
		TFD::Bleedout::Builders::TransitionCaptiveProvider transitionCaptive,
		RuntimeProviders runtime)
	{
		g_nonCaptiveChoice = std::move(nonCaptiveChoice);
		g_blackout = std::move(blackout);
		g_transitionRuntime = std::move(transitionRuntime);
		g_transitionCaptive = std::move(transitionCaptive);
		g_runtime = std::move(runtime);

		auto nonCaptiveForward = g_nonCaptiveChoice;
		nonCaptiveForward.beginResolvedNoMarkerFallback = [](const char* reason) {
			return BeginResolvedNoMarkerFallback(reason);
			};
		TFD::Bleedout::Builders::InstallNonCaptiveChoiceProvider(std::move(nonCaptiveForward));
		TFD::Bleedout::Builders::InstallBlackoutProvider(g_blackout);
		TFD::Bleedout::Builders::InstallTransitionRuntimeProvider(g_transitionRuntime);
		TFD::Bleedout::Builders::InstallTransitionCaptiveProvider(g_transitionCaptive);
	}

	void Reset()
	{
		g_nonCaptiveChoice = {};
		g_blackout = {};
		g_transitionRuntime = {};
		g_transitionCaptive = {};
		g_runtime = {};
	}

	TFD::Transition::RuntimeHandlers BuildTransitionRuntimeHandlers()
	{
		return TFD::Bleedout::Builders::BuildTransitionRuntimeHandlers();
	}

	TFD::Transition::CaptiveHandlers BuildTransitionCaptiveHandlers()
	{
		return TFD::Bleedout::Builders::BuildTransitionCaptiveHandlers();
	}

	bool BeginResolvedNoMarkerFallback(const char* reason)
	{
		return TFD::FlowController::ExecuteResolvedNoMarkerFallback(reason, {
			.tryBeginTerminalCommit = g_runtime.tryBeginTerminalCommit,
			.clearCaptiveOrchestrationResidue = g_runtime.clearCaptiveOrchestrationResidue,
			.getPlayer = g_runtime.getPlayer,
			.clearBridgeAliases = g_runtime.clearBridgeAliases,
			.setPlayerBleedImmune = g_runtime.setPlayerBleedImmune,
			.resetBleedRuntimeState = g_runtime.resetBleedRuntimeState,
			.clearLastAggressor = g_runtime.clearLastAggressor,
			.updatePreCombatState = g_runtime.updatePreCombatState,
			.resolveNoMarkerFallback = [](const char* why) {
				return TFD::Transition::ResolveNoMarkerFallback(why, BuildTransitionRuntimeHandlers());
			},
			.getBranchName = [](TFD::Transition::FallbackBranch branch) {
				return TFD::Transition::GetBranchName(branch);
			},
			.beginRescueTransition = [](const char* why) {
				return TFD::Transition::BeginRescueTransition(why, BuildTransitionRuntimeHandlers());
			},
			.forceLeftForDeadSolo = []() {
				TFD::Transition::ForceLeftForDeadSolo(BuildTransitionRuntimeHandlers());
			},
			.beginRecoverTransition = [](const char* why) {
				TFD::Transition::BeginRecoverTransition(why, BuildTransitionRuntimeHandlers());
			}
			});
	}
}

