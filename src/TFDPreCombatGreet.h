#pragma once

#include "RE/Skyrim.h"

namespace TFD::PreCombatGreet
{
	void Install();
	void Shutdown();

	void SetSuspended(bool suspended);
	bool IsSuspended();

	bool BeginForActor(RE::Actor* actor);
	void CancelAll();

	void OnPreLoadGame();
	void OnPostLoadGame();

	RE::Actor* GetRecentActor(double maxAgeSec);
}
