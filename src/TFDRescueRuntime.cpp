#include "TFDRescueRuntime.h"

#include <cmath>
#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

namespace TFD::RescueRuntime
{
	namespace
	{
		RE::TESGlobal* g_stateGlobal = nullptr;
		bool g_logged = false;

		void ResolveGlobal()
		{
			if (!g_stateGlobal) {
				g_stateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDRescueState");
				if (g_stateGlobal && !g_logged) {
					g_logged = true;
					spdlog::info("[TFD][Rescue] TFDRescueState resolved {:08X}", g_stateGlobal->GetFormID());
				}
			}
		}
	}

	void SetStateValue(int value)
	{
		ResolveGlobal();
		if (g_stateGlobal) {
			g_stateGlobal->value = static_cast<float>(value);
		}
	}

	int GetStateValue()
	{
		ResolveGlobal();
		return g_stateGlobal ? static_cast<int>(std::lround(g_stateGlobal->value)) : 0;
	}

	bool IsActive()
	{
		return GetStateValue() != 0;
	}
}
