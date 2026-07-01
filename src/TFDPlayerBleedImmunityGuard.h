#pragma once

#include <RE/Skyrim.h>

namespace TFD::PlayerBleedImmunityGuard
{
	bool IsActive();
	void SetActive(RE::Actor* player, bool active, const char* reason = nullptr);
	void SetPlayerActive(bool active, const char* reason = nullptr);
	void Reset(RE::Actor* player = nullptr, const char* reason = nullptr);
}
