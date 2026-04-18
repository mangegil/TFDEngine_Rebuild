#include "TFDVictory.h"

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cmath>

namespace TFD::Victory
{
	namespace
	{
		RE::TESGlobal* g_stateGlobal = nullptr;
		bool g_logged = false;
		std::chrono::steady_clock::time_point g_observedCombatContextUntil{};
		static constexpr int kObservedCombatContextLingerMs = 2500;

		void ResolveGlobal()
		{
			if (!g_stateGlobal) {
				g_stateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDVictoryState");
				if (g_stateGlobal && !g_logged) {
					g_logged = true;
					spdlog::info("[TFD][Victory] TFDVictoryState resolved {:08X}", g_stateGlobal->GetFormID());
				}
			}
		}

		inline std::chrono::steady_clock::time_point Now()
		{
			return std::chrono::steady_clock::now();
		}
	}

	void SetStateValue(int value)
	{
		ResolveGlobal();
		if (g_stateGlobal) {
			g_stateGlobal->value = static_cast<float>(value);
		}
	}

	int GetStateValue()
	{
		ResolveGlobal();
		return g_stateGlobal ? static_cast<int>(std::lround(g_stateGlobal->value)) : 0;
	}

	bool IsActive()
	{
		return GetStateValue() != 0;
	}

	void ResetObservedContext()
	{
		g_observedCombatContextUntil = {};
	}

	int ComputeObservedState(const ObservedContext& context)
	{
		if (!context.hasPlayer) {
			ResetObservedContext();
			return 0;
		}

		if (context.playerDown) {
			ResetObservedContext();
			return 0;
		}

		if (context.combatContext) {
			g_observedCombatContextUntil = Now() + std::chrono::milliseconds(kObservedCombatContextLingerMs);
		}

		if (context.hasEnemies) {
			return 1;
		}

		if (g_observedCombatContextUntil != std::chrono::steady_clock::time_point{} && Now() < g_observedCombatContextUntil) {
			return 2;
		}

		return 0;
	}

	void RefreshObservedState(const ObservedContext& context)
	{
		SetStateValue(ComputeObservedState(context));
	}
}
