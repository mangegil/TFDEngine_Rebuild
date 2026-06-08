#include "TFDLeftForDead.h"

#include "TFDSettings.h"

#include <algorithm>
#include <cmath>

#include <spdlog/spdlog.h>

namespace TFD::LeftForDead
{
	namespace
	{
		static RE::Actor* ResolvePlayer(const TFD::Transition::RuntimeHandlers& handlers)
		{
			return handlers.getPlayer ? handlers.getPlayer() : nullptr;
		}

		static void ApplyWakeState(RE::Actor* actor, bool followerStyle)
		{
			if (!actor) {
				return;
			}
			actor->NotifyAnimationGraph("BleedoutStop");
			actor->NotifyAnimationGraph("GetUpStart");

			const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float threshPct = std::clamp(TFD::Settings::GetDefeatThresholdPct() / 100.0f, 0.05f, 0.95f);
			const float safePct = std::clamp(threshPct + (followerStyle ? 0.14f : 0.12f), followerStyle ? 0.34f : 0.32f, 0.85f);
			const float targetHp = (std::max)(followerStyle ? 32.0f : 45.0f, hpMax * safePct);
			const float hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
			if (hpNow < targetHp) {
				actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, targetHp - hpNow);
			}

			const float staminaMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kStamina));
			const float staminaTarget = (std::max)(20.0f, staminaMax * (followerStyle ? 0.28f : 0.35f));
			const float staminaNow = actor->GetActorValue(RE::ActorValue::kStamina);
			if (staminaNow < staminaTarget) {
				actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina, staminaTarget - staminaNow);
			}

			if (actor->IsInCombat()) {
				actor->StopCombat();
			}
			// R244A: no forced weapon stance; Skyrim handles sheath/draw naturally. Disabled: actor->DrawWeaponMagicHands(false);
		}

		static void MoveFollowerNearPlayer(const TFD::Transition::RuntimeHandlers& runtimeHandlers,
			const Handlers& handlers,
			RE::Actor* follower)
		{
			auto* player = ResolvePlayer(runtimeHandlers);
			if (!player || !follower) {
				return;
			}

			const bool interior = player->GetParentCell() ? player->GetParentCell()->IsInteriorCell() : false;
			const float offset = interior ? 128.0f : 220.0f;
			const float yaw = player->GetAngleZ();
			RE::NiPoint3 pos = player->GetPosition();
			pos.x += std::cos(yaw) * offset;
			pos.y -= std::sin(yaw) * offset;

			if (!follower->IsDead()) {
				follower->MoveTo(player);
			}
			follower->SetPosition(pos, true);
			if (handlers.applyFacing) {
				handlers.applyFacing(follower, yaw);
			}
			if (!follower->IsDead()) {
				ApplyWakeState(follower, true);
				if (handlers.setFollowerHold) {
					handlers.setFollowerHold(follower);
				}
			}
		}
	}

	void ExecuteWake(const State& state,
		const char* reason,
		const TFD::Transition::RuntimeHandlers& runtimeHandlers,
		const Handlers& handlers)
	{
		auto* player = ResolvePlayer(runtimeHandlers);
		if (!player) {
			return;
		}

		if (state.destination) {
			auto refSp = state.destination.get();
			if (auto* dest = refSp.get()) {
				player->MoveTo(dest);
			}
		} else if (state.hasFallbackPos) {
			player->SetPosition(state.fallbackPos, true);
		}

		if (handlers.applyFacing) {
			handlers.applyFacing(player, state.angleZ);
		}
		ApplyWakeState(player, false);

		if (state.branch == TFD::Transition::FallbackBranch::LeftForDeadWithFollower && state.follower) {
			auto followerSp = RE::Actor::LookupByHandle(state.follower.native_handle());
			if (auto* follower = followerSp.get()) {
				MoveFollowerNearPlayer(runtimeHandlers, handlers, follower);
			}
		}

		if (handlers.maintainCalmWindow) {
			handlers.maintainCalmWindow();
		}
		if (handlers.setAggroKickNeeded) {
			handlers.setAggroKickNeeded(false);
		}
		if (handlers.beginCooldown) {
			handlers.beginCooldown(5);
		}
		if (runtimeHandlers.setGraceSeconds) {
			runtimeHandlers.setGraceSeconds(5);
		}
		if (runtimeHandlers.setRescueStateValue) {
			runtimeHandlers.setRescueStateValue(0);
		}
		if (runtimeHandlers.refreshPostDefeatGlobals) {
			runtimeHandlers.refreshPostDefeatGlobals();
		}
		if (runtimeHandlers.updatePreCombatState) {
			runtimeHandlers.updatePreCombatState();
		}

		spdlog::info("[TFD][LeftForDead] complete branch={} reason={}",
			TFD::Transition::GetBranchName(state.branch),
			reason ? reason : "unknown");
	}
}
