#include "TFDVictory.h"

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

namespace TFD::Victory
{
	namespace
	{
		RE::TESGlobal* g_stateGlobal = nullptr;
		bool g_logged = false;

		void ResolveGlobal()
		{
			if (!g_stateGlobal) {
				g_stateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDVictoryState");
				if (g_stateGlobal && !g_logged) {
					g_logged = true;
					spdlog::info("[TFD][Victory] TFDVictoryState resolved {:08X}", g_stateGlobal->GetFormID());
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
}
