#pragma once

#include "RE/Skyrim.h"

#include "TFDInteractionRouter.h"

namespace TFD::PreCombatGreet
{
	void Install();
	void Shutdown();

	void SetSuspended(bool suspended);
	bool IsSuspended();

	bool BeginForActor(RE::Actor* actor, TFD::InteractionRouter::Action* outAction = nullptr);
	void CancelAll();

	void OnPreLoadGame();
	void OnPostLoadGame();

	RE::Actor* GetRecentActor(double maxAgeSec);
}
