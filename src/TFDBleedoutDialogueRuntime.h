#pragma once

#include <cstdint>
#include <functional>

#include <RE/Skyrim.h>

#include "TFDBleedout.h"

namespace TFD::BleedoutDialogueRuntime
{
	struct Context
	{
		std::function<RE::Actor*()> getPlayer;
		std::function<TFD::Bleedout::SpeakerLogicHandlers(bool)> buildSpeakerHandlers;
		std::function<TFD::Bleedout::RuntimeHostStateRefs()> buildRuntimeStateRefs;
		std::function<TFD::Bleedout::RuntimeHostHandlers()> buildRuntimeHostHandlers;
		std::function<std::uint32_t()> currentSpeakerID;
		std::function<bool()> ownsCurrentFlow;
	};

	RE::Actor* FindBestSpeaker(float radius, float maxDist, RE::Actor* preferred, const Context& context);
	bool IsReasonableSpeaker(RE::Actor* actor, RE::Actor* player, float maxDist, float* outDistance, const Context& context);
	bool CanUseAggressorForGreet(RE::Actor* player, RE::Actor* aggressor, float& outDistance, const Context& context);
	void ApplyOverdrive(RE::Actor* player, RE::Actor* speaker, const char* reason, bool restartDialogue, const Context& context);
}
