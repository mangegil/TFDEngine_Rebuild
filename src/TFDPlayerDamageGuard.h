#pragma once

#include <chrono>
#include <cstdint>

#include <RE/Skyrim.h>

namespace TFD::PlayerDamageGuard
{
	struct Config
	{
		float thresholdPct{ 2.0f };
		float protectedHp{ 1.0f };
		float safeFloorHp{ 1.0f };
	};

	struct HealthDamageClampResult
	{
		bool blocked{ false };
		float damageOut{ 0.0f };
		float blockedDamage{ 0.0f };
		float hpBefore{ 0.0f };
		float safeFloorHp{ 0.0f };
	};

	struct TickResult
	{
		bool active{ false };
		bool pendingBleedout{ false };
		bool recoveredDeadState{ false };
		bool observedDamage{ false };
		float virtualHp{ 0.0f };
		float virtualHpPct{ 0.0f };
		float thresholdHp{ 0.0f };
		float actualHpBefore{ 0.0f };
		float actualHpAfter{ 0.0f };
		float protectedHp{ 0.0f };
		float damageDelta{ 0.0f };
		RE::ActorHandle attacker{};
	};

	bool IsActive();
	bool HasPendingBleedout();
	bool IsHardImmunityActive();
	void SetHardImmunity(RE::Actor* player, bool enable, const char* reason);
	HealthDamageClampResult ClampIncomingHealthDamage(RE::Actor* player, RE::Actor* attacker, float damageIn, float safeFloorHp, bool blockAll, const char* reason);
	bool Arm(RE::Actor* player, RE::Actor* attacker, const Config& config, const char* reason);
	TickResult Tick(RE::Actor* player, const Config& config);
	TickResult GetSnapshot(RE::Actor* player);
	RE::Actor* ResolveAttacker();
	void NoteAttacker(RE::Actor* attacker);
	void ReleaseToVirtualHealth(RE::Actor* player, const Config& config, const char* reason);
	void Reset(const char* reason);
}
