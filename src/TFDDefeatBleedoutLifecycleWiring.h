#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <vector>

#include <RE/Skyrim.h>

#include "TFDBleedout.h"
#include "TFDBleedoutDialogueRuntime.h"
#include "TFDTransition.h"

namespace TFD::DefeatBleedoutLifecycleWiring
{
	struct Dependencies
	{
		std::atomic_bool* inBleedState = nullptr;
		float* minHp = nullptr;
		bool* bleedPendingCaptiveOutcome = nullptr;
		bool* bleedPendingNonCaptiveOutcome = nullptr;
		std::atomic_bool* graceActive = nullptr;
		std::chrono::steady_clock::time_point* graceUntil = nullptr;
		bool* previousDialogueOpen = nullptr;

		std::function<RE::Actor*()> getPlayer;
		std::function<void(int)> setGraceSeconds;
		std::function<void(bool)> setPlayerBleedImmune;
		std::function<void(RE::Actor*, float)> clampHealth;
		std::function<void(bool)> resetBleedRuntimeState;
		std::function<void(const char*)> transitionBleedRuntimeToPleasureCommit;
		std::function<TFD::Transition::RuntimeHandlers()> buildTransitionRuntimeHandlers;
		std::function<TFD::Bleedout::SpeakerLogicHandlers()> buildBleedoutSpeakerHandlers;
		std::function<TFD::BleedoutDialogueRuntime::Context()> buildBleedoutDialogueRuntimeContext;
		std::function<std::uint32_t()> resolveBleedFlowActorFormID;
		std::function<std::vector<RE::Actor*>(float)> collectBleedStandingFollowers;
		std::function<std::vector<RE::Actor*>(float, RE::Actor*, bool)> collectBleedoutCrowd;
		std::function<bool(RE::Actor*, RE::Actor*)> isBleedSpaceCompatible;
		std::function<bool(RE::Actor*)> isObserverAlly;
		std::function<bool(RE::Actor*)> isStandingAllyThresholdActor;
		std::function<bool(RE::Actor*)> isStandingEnemyThresholdActor;
	};

	void InstallProvider(Dependencies dependencies);
	void ShutdownProvider();
	bool HasProvider();
	TFD::Bleedout::DefeatLifecycleProviders BuildProviders();
}
