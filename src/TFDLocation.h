#pragma once
#include <cstdint>

namespace RE
{
	class TESObjectREFR;
	class Actor;
}

namespace TFD::Location
{
	void Initialize();

	// Debug rescan: preferInterior ikut kondisi player
	bool RescanCaptiveMarker();

	// Flow kidnap/defeat: kasih aggressor biar bisa fallback ke loc aggressor + boss anchor
	bool RescanCaptiveMarkerWithAggressor(RE::Actor* aggressor, bool preferInterior);

	RE::TESObjectREFR* GetCachedCaptiveMarker();
	std::uint32_t GetCachedCaptiveMarkerFormID();

	void DumpContextToLog();

	// Teleport debug: kalau cache kosong baru rescan (prefer interior)
	bool TeleportToCaptiveMarker();
}