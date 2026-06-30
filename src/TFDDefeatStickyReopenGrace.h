#pragma once

#include <chrono>
#include <cstdint>

#include <RE/Skyrim.h>

namespace TFD::DefeatStickyReopenGrace
{
	bool IsActive();
	std::uint32_t SpeakerID();
	void Clear(const char* reason = nullptr);
	void CancelAfterTerminalOutcome(const char* eventName, const char* reason = nullptr);
	bool Tick(RE::Actor* player, std::chrono::steady_clock::time_point now);
}
