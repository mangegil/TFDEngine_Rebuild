#pragma once

#include <RE/Skyrim.h>

namespace TFD::FactionMask
{
	// Single allowlist: factions yang boleh dicopy dari aggressor -> player
	static constexpr const char* kAllowListEditorId = "TFDAllowedAggressorFactions";

	void Initialize();                       // call once after DataLoaded
	bool IsActive();

	// Copy factions from aggressor to player using allowlist
	bool ApplyFromAggressor(RE::Actor* aggressor);

	// Restore original player ranks / remove added factions
	void Clear();

	void DumpToLog();
}