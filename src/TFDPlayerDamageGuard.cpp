#include "TFDPlayerDamageGuard.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <spdlog/spdlog.h>

namespace TFD::PlayerDamageGuard
{
	namespace
	{
		struct State
		{
			bool active{ false };
			bool pendingBleedout{ false };
			bool recoveredDeadState{ false };
			RE::ActorHandle attacker{};
			std::chrono::steady_clock::time_point armedAt{};
			std::chrono::steady_clock::time_point lastLog{};
			float maxHp{ 1.0f };
			float thresholdHp{ 1.0f };
			float virtualHp{ 1.0f };
			float lastActualHp{ 1.0f };
			float protectedHp{ 1.0f };
			float safeFloorHp{ 1.0f };
			float totalAbsorbed{ 0.0f };
			std::string reason{};
		};

		State g_state{};

		struct HardImmunityState
		{
			bool active{ false };
			bool wasEssential{ false };
			bool wasProtected{ false };
			bool wasNoBleedoutRecovery{ false };
			bool wasCanSpeakEssentialDown{ false };
			bool wasBaseInvulnerable{ false };
		};

		HardImmunityState g_hardImmunity{};

		static std::chrono::steady_clock::time_point Now()
		{
			return std::chrono::steady_clock::now();
		}

		static float ResolveMaxHp(RE::Actor* player)
		{
			if (!player) {
				return 1.0f;
			}
			return (std::max)(1.0f, player->GetPermanentActorValue(RE::ActorValue::kHealth));
		}

		static float ResolveCurrentHp(RE::Actor* player)
		{
			if (!player) {
				return 0.0f;
			}
			return (std::max)(0.0f, player->GetActorValue(RE::ActorValue::kHealth));
		}

		static float ClampHp(float value, float low, float high)
		{
			return std::clamp(value, low, (std::max)(low, high));
		}

		static void RestoreToAtLeast(RE::Actor* player, float hp)
		{
			if (!player) {
				return;
			}
			const float now = player->GetActorValue(RE::ActorValue::kHealth);
			if (now < hp - 0.001f) {
				player->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, hp - now);
			}
		}

		static void ClampToAtMost(RE::Actor* player, float hp)
		{
			if (!player) {
				return;
			}
			const float now = player->GetActorValue(RE::ActorValue::kHealth);
			if (now > hp + 0.001f) {
				player->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, -(now - hp));
			}
		}

		static TickResult BuildResult(RE::Actor* player, bool observedDamage, float damageDelta, float actualBefore, float actualAfter)
		{
			TickResult result{};
			result.active = g_state.active;
			result.pendingBleedout = g_state.pendingBleedout;
			result.recoveredDeadState = g_state.recoveredDeadState;
			result.observedDamage = observedDamage;
			result.virtualHp = g_state.virtualHp;
			result.virtualHpPct = g_state.maxHp > 0.0f ? (g_state.virtualHp / g_state.maxHp) * 100.0f : 0.0f;
			result.thresholdHp = g_state.thresholdHp;
			result.actualHpBefore = actualBefore;
			result.actualHpAfter = actualAfter;
			result.protectedHp = g_state.protectedHp;
			result.damageDelta = damageDelta;
			result.attacker = g_state.attacker;
			(void)player;
			return result;
		}
	}

	bool IsActive()
	{
		return g_state.active;
	}

	bool HasPendingBleedout()
	{
		return g_state.active && g_state.pendingBleedout;
	}

	bool IsHardImmunityActive()
	{
		return g_hardImmunity.active;
	}

	void SetHardImmunity(RE::Actor* player, bool enable, const char* reason)
	{
		if (!player || player->IsDisabled()) {
			return;
		}

		auto& boolFlags = player->GetActorRuntimeData().boolFlags;
		auto* actorBase = player->GetActorBase();
		auto* baseData = actorBase ? static_cast<RE::TESActorBaseData*>(actorBase) : nullptr;

		if (enable) {
			if (!g_hardImmunity.active) {
				g_hardImmunity.wasEssential = boolFlags.all(RE::Actor::BOOL_FLAGS::kEssential);
				g_hardImmunity.wasProtected = boolFlags.all(RE::Actor::BOOL_FLAGS::kProtected);
				g_hardImmunity.wasNoBleedoutRecovery = boolFlags.all(RE::Actor::BOOL_FLAGS::kNoBleedoutRecovery);
				g_hardImmunity.wasCanSpeakEssentialDown = boolFlags.all(RE::Actor::BOOL_FLAGS::kCanSpeakToEssentialDown);
				g_hardImmunity.wasBaseInvulnerable = baseData && baseData->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kInvulnerable);
				g_hardImmunity.active = true;
				spdlog::info(
					"[TFD][PlayerDamageGuard][R19] hard immunity enabled player={:08X} essentialWas={} protectedWas={} noBleedoutWas={} canSpeakWas={} baseInvulnWas={} reason={}",
					player->GetFormID(),
					g_hardImmunity.wasEssential ? 1 : 0,
					g_hardImmunity.wasProtected ? 1 : 0,
					g_hardImmunity.wasNoBleedoutRecovery ? 1 : 0,
					g_hardImmunity.wasCanSpeakEssentialDown ? 1 : 0,
					g_hardImmunity.wasBaseInvulnerable ? 1 : 0,
					reason && reason[0] ? reason : "unknown");
			}

			boolFlags.set(RE::Actor::BOOL_FLAGS::kEssential);
			boolFlags.set(RE::Actor::BOOL_FLAGS::kProtected);
			boolFlags.set(RE::Actor::BOOL_FLAGS::kCanSpeakToEssentialDown);
			boolFlags.set(RE::Actor::BOOL_FLAGS::kNoBleedoutRecovery);
			boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
			if (baseData) {
				baseData->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kInvulnerable);
			}
			return;
		}

		if (!g_hardImmunity.active) {
			return;
		}

		if (!g_hardImmunity.wasEssential) {
			boolFlags.reset(RE::Actor::BOOL_FLAGS::kEssential);
		}
		if (!g_hardImmunity.wasProtected) {
			boolFlags.reset(RE::Actor::BOOL_FLAGS::kProtected);
		}
		if (!g_hardImmunity.wasNoBleedoutRecovery) {
			boolFlags.reset(RE::Actor::BOOL_FLAGS::kNoBleedoutRecovery);
		}
		if (!g_hardImmunity.wasCanSpeakEssentialDown) {
			boolFlags.reset(RE::Actor::BOOL_FLAGS::kCanSpeakToEssentialDown);
		}
		boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
		if (baseData && !g_hardImmunity.wasBaseInvulnerable) {
			baseData->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kInvulnerable);
		}

		spdlog::info(
			"[TFD][PlayerDamageGuard][R19] hard immunity released player={:08X} reason={}",
			player->GetFormID(),
			reason && reason[0] ? reason : "unknown");
		g_hardImmunity = {};
	}


	HealthDamageClampResult ClampIncomingHealthDamage(RE::Actor* player, RE::Actor* attacker, float damageIn, float safeFloorHp, bool blockAll, const char* reason)
	{
		HealthDamageClampResult result{};
		result.damageOut = damageIn;
		result.safeFloorHp = safeFloorHp;
		if (!player || player->IsDisabled() || damageIn <= 0.0f) {
			return result;
		}

		const float hpBefore = ResolveCurrentHp(player);
		const float maxHp = ResolveMaxHp(player);
		const float floorHp = ClampHp((std::max)(1.0f, safeFloorHp), 1.0f, maxHp);
		const float allowedDamage = blockAll ? 0.0f : (std::max)(0.0f, hpBefore - floorHp);
		const float damageOut = (std::min)(damageIn, allowedDamage);
		const float blockedDamage = damageIn - damageOut;

		result.damageOut = damageOut;
		result.blockedDamage = blockedDamage;
		result.hpBefore = hpBefore;
		result.safeFloorHp = floorHp;
		result.blocked = blockedDamage > 0.001f;

		if (!result.blocked) {
			return result;
		}

		SetHardImmunity(player, true, reason && reason[0] ? reason : "incoming_health_damage_clamp");
		player->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
		RestoreToAtLeast(player, floorHp);

		if (attacker) {
			NoteAttacker(attacker);
		}

		spdlog::warn(
			"[TFD][PlayerDamageGuard][R20] incoming health damage clamped player={:08X} attacker={:08X} hpBefore={:.2f} damageIn={:.2f} damageOut={:.2f} blocked={:.2f} safeFloor={:.2f} blockAll={} reason={}",
			player->GetFormID(),
			attacker ? attacker->GetFormID() : 0u,
			hpBefore,
			damageIn,
			damageOut,
			blockedDamage,
			floorHp,
			blockAll ? 1 : 0,
			reason && reason[0] ? reason : "unknown");

		return result;
	}

	void NoteAttacker(RE::Actor* attacker)
	{
		if (!attacker) {
			return;
		}
		g_state.attacker = attacker->GetHandle();
	}

	RE::Actor* ResolveAttacker()
	{
		if (!g_state.attacker) {
			return nullptr;
		}
		auto sp = RE::Actor::LookupByHandle(g_state.attacker.native_handle());
		return sp.get();
	}

	bool Arm(RE::Actor* player, RE::Actor* attacker, const Config& config, const char* reason)
	{
		if (!player || player->IsDisabled()) {
			return false;
		}

		const float maxHp = ResolveMaxHp(player);
		const float thresholdPct = std::clamp(config.thresholdPct, 2.0f, 95.0f);
		const float thresholdHp = (std::max)(1.0f, maxHp * (thresholdPct / 100.0f));
		const float protectedHp = ClampHp((std::max)(config.protectedHp, thresholdHp + 1.0f), 1.0f, maxHp);
		const float safeFloorHp = ClampHp((std::max)(config.safeFloorHp, thresholdHp + 0.5f), 1.0f, protectedHp);
		const float currentHp = ResolveCurrentHp(player);

		if (g_state.active) {
			if (attacker) {
				g_state.attacker = attacker->GetHandle();
			}
			g_state.protectedHp = (std::max)(g_state.protectedHp, protectedHp);
			g_state.safeFloorHp = (std::max)(g_state.safeFloorHp, safeFloorHp);
			RestoreToAtLeast(player, g_state.protectedHp);
			g_state.lastActualHp = g_state.protectedHp;
			return true;
		}

		g_state = {};
		g_state.active = true;
		g_state.pendingBleedout = currentHp <= thresholdHp + 0.001f;
		g_state.recoveredDeadState = false;
		g_state.attacker = attacker ? attacker->GetHandle() : RE::ActorHandle{};
		g_state.armedAt = Now();
		g_state.lastLog = g_state.armedAt;
		g_state.maxHp = maxHp;
		g_state.thresholdHp = thresholdHp;
		g_state.virtualHp = ClampHp(currentHp, 1.0f, maxHp);
		g_state.lastActualHp = protectedHp;
		g_state.protectedHp = protectedHp;
		g_state.safeFloorHp = safeFloorHp;
		g_state.totalAbsorbed = 0.0f;
		g_state.reason = reason && reason[0] ? reason : "unknown";

		player->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
		RestoreToAtLeast(player, g_state.protectedHp);

		spdlog::warn(
			"[TFD][PlayerDamageGuard][R447A] armed player={:08X} attacker={:08X} virtualHp={:.2f} virtualPct={:.1f} protectedHp={:.2f} safeFloor={:.2f} thresholdHp={:.2f} thresholdPct={:.1f} reason={}",
			player->GetFormID(),
			attacker ? attacker->GetFormID() : 0u,
			g_state.virtualHp,
			(g_state.virtualHp / g_state.maxHp) * 100.0f,
			g_state.protectedHp,
			g_state.safeFloorHp,
			g_state.thresholdHp,
			thresholdPct,
			g_state.reason);

		return true;
	}

	TickResult Tick(RE::Actor* player, const Config& config)
	{
		if (!g_state.active) {
			return {};
		}
		if (!player || player->IsDisabled()) {
			Reset("invalid_player");
			return {};
		}

		const float maxHp = ResolveMaxHp(player);
		const float thresholdPct = std::clamp(config.thresholdPct, 2.0f, 95.0f);
		g_state.maxHp = (std::max)(g_state.maxHp, maxHp);
		g_state.thresholdHp = (std::max)(1.0f, maxHp * (thresholdPct / 100.0f));
		g_state.protectedHp = ClampHp((std::max)(g_state.protectedHp, config.protectedHp), 1.0f, maxHp);
		g_state.safeFloorHp = ClampHp((std::max)(g_state.safeFloorHp, config.safeFloorHp), 1.0f, g_state.protectedHp);

		float actualBefore = ResolveCurrentHp(player);
		float damageDelta = 0.0f;
		bool recoveredDeadState = false;
		if (player->IsDead(false)) {
			recoveredDeadState = true;
			g_state.recoveredDeadState = true;
			player->Resurrect(false, true);
			damageDelta = (std::max)(g_state.lastActualHp, g_state.protectedHp);
			actualBefore = 0.0f;
		}
		else {
			const float referenceHp = (std::max)(g_state.lastActualHp, g_state.protectedHp);
			damageDelta = (std::max)(0.0f, referenceHp - actualBefore);
		}

		const bool observedDamage = damageDelta > 0.001f;
		if (observedDamage) {
			g_state.virtualHp = ClampHp(g_state.virtualHp - damageDelta, 0.0f, g_state.maxHp);
			g_state.totalAbsorbed += damageDelta;
		}

		if (recoveredDeadState || g_state.virtualHp <= g_state.thresholdHp + 0.001f) {
			g_state.pendingBleedout = true;
		}

		player->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
		RestoreToAtLeast(player, g_state.protectedHp);
		g_state.lastActualHp = g_state.protectedHp;
		const float actualAfter = ResolveCurrentHp(player);

		const auto now = Now();
		if (observedDamage || g_state.pendingBleedout || g_state.lastLog.time_since_epoch().count() == 0 ||
			(now - g_state.lastLog) >= std::chrono::milliseconds(1000)) {
			g_state.lastLog = now;
			spdlog::info(
				"[TFD][PlayerDamageGuard][R447A] tick pending={} observedDamage={} recoveredDead={} damageDelta={:.2f} virtualHp={:.2f} virtualPct={:.1f} actualBefore={:.2f} actualAfter={:.2f} protectedHp={:.2f} thresholdHp={:.2f} totalAbsorbed={:.2f}",
				g_state.pendingBleedout ? 1 : 0,
				observedDamage ? 1 : 0,
				recoveredDeadState ? 1 : 0,
				damageDelta,
				g_state.virtualHp,
				(g_state.maxHp > 0.0f ? (g_state.virtualHp / g_state.maxHp) * 100.0f : 0.0f),
				actualBefore,
				actualAfter,
				g_state.protectedHp,
				g_state.thresholdHp,
				g_state.totalAbsorbed);
		}

		return BuildResult(player, observedDamage, damageDelta, actualBefore, actualAfter);
	}

	TickResult GetSnapshot(RE::Actor* player)
	{
		if (!g_state.active) {
			return {};
		}
		const float actual = ResolveCurrentHp(player);
		return BuildResult(player, false, 0.0f, actual, actual);
	}

	void ReleaseToVirtualHealth(RE::Actor* player, const Config& config, const char* reason)
	{
		if (!g_state.active) {
			return;
		}
		const float targetHp = ClampHp((std::max)(g_state.virtualHp, config.safeFloorHp), 1.0f, ResolveMaxHp(player));
		if (player && !player->IsDisabled()) {
			if (player->IsDead(false)) {
				player->Resurrect(false, true);
			}
			RestoreToAtLeast(player, targetHp);
			ClampToAtMost(player, targetHp);
		}
		spdlog::info(
			"[TFD][PlayerDamageGuard][R447A] release_to_virtual targetHp={:.2f} virtualHp={:.2f} protectedHp={:.2f} totalAbsorbed={:.2f} reason={}",
			targetHp,
			g_state.virtualHp,
			g_state.protectedHp,
			g_state.totalAbsorbed,
			reason && reason[0] ? reason : "unknown");
		g_state = {};
	}

	void Reset(const char* reason)
	{
		if (g_state.active) {
			spdlog::info(
				"[TFD][PlayerDamageGuard][R447A] reset virtualHp={:.2f} protectedHp={:.2f} totalAbsorbed={:.2f} pending={} reason={}",
				g_state.virtualHp,
				g_state.protectedHp,
				g_state.totalAbsorbed,
				g_state.pendingBleedout ? 1 : 0,
				reason && reason[0] ? reason : "unknown");
		}
		g_state = {};
	}
}
