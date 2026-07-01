#pragma once

#include <RE/Skyrim.h>

namespace TFD::DefeatThresholdSensor
{
	void ResetEnemyTimer();
	void TickEnemy(const char* reason);
	bool HandlePlayer(RE::Actor* player, const char* reason);
	bool IsDownedActor(RE::Actor* actor);
	bool IsCombatTargetValid(RE::Actor* actor);
}
