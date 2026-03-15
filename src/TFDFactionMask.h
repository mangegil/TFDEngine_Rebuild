#pragma once

#include <RE/Skyrim.h>

namespace TFD::FactionMask
{
	inline constexpr const char* kAllowListEditorId = "TFDHostileFactionAllowList";

	void Initialize();
	bool ApplyFromAggressor(RE::Actor* aggressor);
	void Clear();
	bool IsActive();
}