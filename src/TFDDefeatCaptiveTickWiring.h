#pragma once

#include <functional>

#include <RE/Skyrim.h>

#include "TFDCaptive.h"

namespace TFD::DefeatCaptiveTickWiring
{
	struct Dependencies
	{
		std::function<void()> updatePreCombatState;
		std::function<bool()> isDialogueOpen;
		std::function<bool()> wasDialogueOpen;
		std::function<void(bool)> setDialogueOpenObserved;
		std::function<void()> clearLastAggressor;
		std::function<void(RE::Actor*)> setLastAggressor;
		std::function<RE::Actor*()> resolveAggressor;
		std::function<RE::Actor*(float)> findBestAggressor;
		std::function<void(bool)> setGraceActive;
	};

	void InstallProvider(Dependencies dependencies);
	void ShutdownProvider();
	bool HasProvider();

	TFD::Captive::EscapeTickHandlers BuildEscapeTickHandlers();
	TFD::Captive::RuntimeTickHandlers BuildRuntimeTickHandlers();
	bool TickRuntime(RE::Actor* player, bool captiveBleedOverlay);
}
