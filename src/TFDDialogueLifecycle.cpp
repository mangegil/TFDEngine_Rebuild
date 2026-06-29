#include "TFDDialogueLifecycle.h"

#include <spdlog/spdlog.h>

#include "TFDBleedoutGreet.h"
#include "TFDInCombatGreet.h"
#include "TFDInteractionRouter.h"
#include "TFDPleasureRuntime.h"
#include "TFDRescueGreet.h"

namespace TFD::DialogueLifecycle
{
	namespace
	{
		bool g_prevDialogueOpen = false;
	}

	bool IsDialogueOpen()
	{
		auto* ui = RE::UI::GetSingleton();
		return ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
	}

	bool WasDialogueOpen()
	{
		return g_prevDialogueOpen;
	}

	bool* PreviousOpenStorage()
	{
		return &g_prevDialogueOpen;
	}

	void SetDialogueOpenObserved(bool open)
	{
		g_prevDialogueOpen = open;
	}

	void Reset(const char* reason)
	{
		if (g_prevDialogueOpen) {
			spdlog::info("[TFD][DialogueLifecycle][R22] reset prevDialogueOpen reason={}", reason ? reason : "-");
		}
		g_prevDialogueOpen = false;
	}

	void ObserveBleedoutDialogueOpened(const char* reason)
	{
		TFD::BleedoutGreet::NotifyDialogueOpened();
		g_prevDialogueOpen = true;
		(void)reason;
	}

	TickResult TickOpenEdges(const TickInput& input)
	{
		TickResult result{};
		const bool dOpen = IsDialogueOpen();
		result.dialogueOpen = dOpen;

		if (!input.captiveBleedOverlay && input.inCombatActive && dOpen) {
			TFD::InCombatGreet::NotifyDialogueOpened();
		}
		if (!input.captiveBleedOverlay && input.rescueActive && dOpen) {
			TFD::RescueGreet::NotifyDialogueOpened();
		}

		if (!input.captiveBleedOverlay && input.bleedAfterPleasureActive) {
			TFD::InteractionRouter::DialogueOpen::Tick();
			const bool afterTickOpen = IsDialogueOpen();
			result.dialogueOpen = afterTickOpen;
			if (afterTickOpen) {
				if (!g_prevDialogueOpen) {
					auto* afterSpeaker = TFD::PleasureRuntime::GetPrimarySpeaker();
					spdlog::info(
						"[TFD][DialogueLifecycle][R22] bleed-afterpleasure dialogue observed speaker={:08X}",
						afterSpeaker ? afterSpeaker->GetFormID() : 0u);
				}
				TFD::BleedoutGreet::NotifyDialogueOpened();
				g_prevDialogueOpen = true;
				result.handledBleedAfterPleasurePause = true;
			}
			else {
				g_prevDialogueOpen = false;
			}
		}

		return result;
	}
}
