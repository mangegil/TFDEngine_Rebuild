#pragma once

#include <RE/Skyrim.h>

namespace TFD::FactionManager
{
	inline constexpr const char* kAllowListEditorId = "TFDHostileFactionAllowList";

	void Initialize();
	bool ApplyFromAggressor(RE::Actor* aggressor);
	void Clear();
	bool IsActive();
	bool SharesAllowedFactionExact(RE::Actor* lhs, RE::Actor* rhs);
}