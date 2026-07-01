#include "TFDDefeatBleedHealthGuard.h"

#include <algorithm>
#include <cstdint>

#include <spdlog/spdlog.h>

#include "TFDDefeatBleedLockWiring.h"
#include "TFDDefeatBleedRuntimeState.h"
#include "TFDPlayerBleedImmunityGuard.h"

namespace TFD::DefeatBleedHealthGuard
{
	namespace
	{
		using BleedLockKind = TFD::BleedLockState::Kind;

		const char* ReferenceProbeKindName(BleedLockKind kind)
		{
			return kind == BleedLockKind::Player ? "Player" : "Ally";
		}

		const char* ReferenceProbeLifeStateName(RE::ACTOR_LIFE_STATE state)
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

		const char* ReferenceProbeKnockStateName(RE::KNOCK_STATE_ENUM state)
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
	}

	void ClampHealth(RE::Actor* actor, float minHp)
	{
		if (!actor) {
			return;
		}
		const float hp = actor->GetActorValue(RE::ActorValue::kHealth);
		if (hp < minHp) {
			actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, minHp - hp);
		}
	}

	void ClampHealthCeiling(RE::Actor* actor, float maxHp)
	{
		if (!actor) {
			return;
		}
		const float hp = actor->GetActorValue(RE::ActorValue::kHealth);
		if (hp > maxHp + 0.001f) {
			actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, -(hp - maxHp));
		}
	}

	float ResolveActorHealthForPct(RE::Actor* actor, float pct)
	{
		if (!actor) {
			return 0.0f;
		}
		const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
		return hpMax * std::clamp(pct / 100.0f, 0.0f, 1.0f);
	}

	float ResolvePlayerBleedRuntimeSafeHealth(RE::Actor* actor, float thresholdPct)
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

	TFD::PlayerDamageGuard::Config BuildPlayerDamageGuardConfig(RE::Actor* actor, float thresholdPct, float protectedHp, float safeFloorHp)
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

	void EnforcePlayerBleedInvulnerability(RE::Actor* actor, BleedLockEntry& entry)
	{
		if (!actor) {
			return;
		}

		if (actor->IsDead(false) || actor->GetActorRuntimeData().boolFlags.all(RE::Actor::BOOL_FLAGS::kIsInKillMove)) {
			TFD::DefeatBleedLockWiring::ForcePlayerBleedAlive(actor, entry, actor->IsDead(false) ? "dead_state" : "killmove_state");
		}

		TFD::PlayerBleedImmunityGuard::SetPlayerActive(true);

		entry.minHp = (std::max)(entry.minHp, (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth)) * 0.05f);
		const auto guardTick = TFD::PlayerDamageGuard::Tick(
			actor,
			BuildPlayerDamageGuardConfig(actor, entry.thresholdPct, entry.protectedHealth, entry.minHp));
		if (guardTick.active) {
			entry.protectedHealth = (std::max)(entry.protectedHealth, guardTick.protectedHp);
			entry.lastHealthSample = (std::max)(entry.lastHealthSample, guardTick.actualHpAfter);
		}
		entry.protectedHealth = (std::max)(entry.protectedHealth, ResolvePlayerBleedRuntimeSafeHealth(actor, entry.thresholdPct));
		TFD::DefeatBleedRuntimeState::MaxMinHp(entry.protectedHealth);

		const float hardFloorHp = (std::max)(1.0f, entry.minHp);
		const float protectedHp = (std::max)(entry.protectedHealth, hardFloorHp);
		ClampHealth(actor, protectedHp);

		float hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
		if (hpNow + 0.001f < protectedHp) {
			const float delta = protectedHp - hpNow;
			actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, delta);
			const auto now = std::chrono::steady_clock::now();
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

	void LogReferenceBleedState(RE::Actor* actor, const BleedLockEntry& entry, const char* point, const char* note)
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

	void MaybeLogReferenceBleedSamples(RE::Actor* actor, BleedLockEntry& entry, std::chrono::steady_clock::time_point now)
	{
		if (!actor || entry.referenceProbeStartedAt.time_since_epoch().count() == 0) {
			return;
		}

		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.referenceProbeStartedAt);
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
}
