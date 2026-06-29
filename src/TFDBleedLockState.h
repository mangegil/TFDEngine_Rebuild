#pragma once

#include <chrono>
#include <cstdint>
#include <unordered_map>

#include <RE/Skyrim.h>

namespace TFD::BleedLockState
{
	enum class Kind : std::uint8_t
	{
		Player = 0,
		Ally = 1
	};

	struct Entry
	{
		RE::ActorHandle handle{};
		Kind kind{ Kind::Player };
		float thresholdPct{ 0.0f };
		float minHp{ 0.0f };
		float protectedHealth{ 0.0f };
		float lastHealthSample{ 0.0f };
		float healAccumulator{ 0.0f };
		float savedHealRate{ 0.0f };
		float savedHealRateMult{ 100.0f };
		float savedCombatHealRateMult{ 1.0f };
		bool regenOverridden{ false };
		std::chrono::steady_clock::time_point lastPulse{};
		std::chrono::steady_clock::time_point lastDamageLog{};
		std::chrono::steady_clock::time_point lastHealGain{};
		std::chrono::steady_clock::time_point referenceProbeStartedAt{};
		std::uint8_t referenceProbeSampleMask{ 0 };
		std::uint32_t referenceProbePulseCount{ 0 };
	};

	std::unordered_map<RE::FormID, Entry>& Entries();
	void Clear(const char* reason);
}
