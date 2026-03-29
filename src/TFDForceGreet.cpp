#include <RE/Skyrim.h>
#include "TFDForceGreet.h"

#include <spdlog/spdlog.h>

namespace TFD::ForceGreet
{
	void Install()
	{
		spdlog::info("[TFD][ForceGreet] Install disabled (ESP-owned forcegreet)");
	}

	void BeginBleedout(RE::Actor* speaker)
	{
		spdlog::info(
			"[TFD][ForceGreet] BeginBleedout ignored speaker={:08X}",
			speaker ? speaker->GetFormID() : 0);
	}

	void BeginCaptiveMarker(RE::Actor* speaker)
	{
		spdlog::info(
			"[TFD][ForceGreet] BeginCaptiveMarker ignored speaker={:08X}",
			speaker ? speaker->GetFormID() : 0);
	}

	void BeginInCombatTruce(RE::Actor* speaker)
	{
		spdlog::info(
			"[TFD][ForceGreet] BeginInCombatTruce ignored speaker={:08X}",
			speaker ? speaker->GetFormID() : 0);
	}

	void BeginPreCombatTruce(RE::Actor* speaker)
	{
		spdlog::info(
			"[TFD][ForceGreet] BeginPreCombatTruce ignored speaker={:08X}",
			speaker ? speaker->GetFormID() : 0);
	}

	void Tick()
	{
	}

	void Cancel()
	{
	}

	bool IsActive()
	{
		return false;
	}

	bool DidSucceed()
	{
		return false;
	}

	Mode GetMode()
	{
		return Mode::None;
	}
}
