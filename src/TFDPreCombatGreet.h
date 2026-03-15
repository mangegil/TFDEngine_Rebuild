#pragma once

#include <RE/Skyrim.h>

namespace TFD::PreCombatGreet
{
	void Install();
	void Shutdown();

	void SetSuspended(bool suspended);
	bool IsSuspended();

	bool BeginForActor(RE::Actor* actor);

	void OnPreLoadGame();
	void OnPostLoadGame();

	void CancelAll();

	RE::Actor* GetRecentActor(double maxAgeSec = 0.0);
}