#pragma once

#include <functional>
#include <vector>

#include <RE/Skyrim.h>

#include "TFDBleedout.h"
#include "TFDBleedoutDialogueRuntime.h"

namespace TFD::DefeatBleedoutDialogueWiring
{
	struct Dependencies
	{
		std::function<RE::Actor*()> getPlayer;
		std::function<bool(RE::Actor*)> isStandingEnemyThresholdActor;
		std::function<bool(RE::Actor*, RE::Actor*)> isBleedSpaceCompatible;
		std::function<bool(RE::Actor*, RE::Actor*)> hasLineOfSightToPlayer;
		std::function<bool(RE::Actor*, RE::Actor*, float)> isActorCloseAndFront;
		std::function<RE::Actor*(RE::Actor*)> resolveCurrentCombatTarget;
		std::function<bool(RE::Actor*)> isActiveFollowerActor;
	};

	void InstallProvider(Dependencies dependencies);
	void ShutdownProvider();
	bool HasProvider();

	TFD::Bleedout::SpeakerLogicHandlers BuildSpeakerHandlers(bool preserveAssigned = false);
	TFD::BleedoutDialogueRuntime::Context BuildRuntimeContext();
	std::vector<RE::Actor*> CollectCrowd(float radius, RE::Actor* preferred, bool preserveAssigned = false);
	RE::Actor* FindBestSpeaker(float scanRadius, float maxDistance, RE::Actor* preferred);
}
