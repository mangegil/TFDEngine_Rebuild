#include "TFDBleedLockState.h"

#include <spdlog/spdlog.h>

namespace TFD::BleedLockState
{
	std::unordered_map<RE::FormID, Entry>& Entries()
	{
		static std::unordered_map<RE::FormID, Entry> entries{};
		return entries;
	}

	void Clear(const char* reason)
	{
		auto& entries = Entries();
		if (!entries.empty()) {
			spdlog::info(
				"[TFD][BleedLockState][R21] clear entries={} reason={}",
				static_cast<unsigned int>(entries.size()),
				reason && reason[0] ? reason : "unknown");
		}
		entries.clear();
	}
}
