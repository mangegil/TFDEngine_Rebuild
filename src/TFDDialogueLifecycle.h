#pragma once

#include <RE/Skyrim.h>

namespace TFD::DialogueLifecycle
{
	bool IsDialogueOpen();
	bool WasDialogueOpen();
	bool* PreviousOpenStorage();
	void SetDialogueOpenObserved(bool open);
	void Reset(const char* reason = nullptr);
	void ObserveBleedoutDialogueOpened(const char* reason = nullptr);

	struct TickInput
	{
		bool captiveBleedOverlay{ false };
		bool inCombatActive{ false };
		bool rescueActive{ false };
		bool bleedAfterPleasureActive{ false };
		bool pleasureRuntimeBlocking{ false };
	};

	struct TickResult
	{
		bool dialogueOpen{ false };
		bool handledBleedAfterPleasurePause{ false };
	};

	TickResult TickOpenEdges(const TickInput& input);
}
