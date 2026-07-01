#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <RE/Skyrim.h>

namespace TFD::DefeatMonitorBootstrapWiring
{
	struct Dependencies
	{
		std::function<RE::PlayerCharacter*()> getPlayerCharacter;
		std::function<RE::Actor*()> getPlayerActor;
		std::function<bool(RE::Actor*)> isActiveFollowerActor;
		std::function<bool(RE::Actor*)> isStandingEnemyThresholdActor;
		std::function<bool(RE::Actor*)> isStandingAllyThresholdActor;
		std::function<bool(RE::Actor*)> isObserverAlly;
		std::function<bool(RE::Actor*, RE::Actor*)> isBleedSpaceCompatible;
		std::function<bool(RE::Actor*, RE::Actor*)> hasLineOfSightToPlayer;
		std::function<bool(RE::Actor*, RE::Actor*, float)> isActorCloseAndFront;
		std::function<RE::Actor*(RE::Actor*)> resolveCurrentCombatTarget;
		std::function<void()> resetBleedRuntimeState;
		std::function<void(bool)> resetBleedRuntimeStatePreserve;
		std::function<std::vector<RE::Actor*>(float)> collectBleedStandingFollowers;
		std::function<float(RE::Actor*)> getActorHealthPct;
		std::function<bool(RE::Actor*)> isActorBleedingOut;
		std::function<bool()> hasPlayerBleedLock;
		std::function<void()> resetEnemyThresholdScanTimer;
		std::function<std::vector<RE::Actor*>(RE::Actor*, float, const std::vector<RE::Actor*>&)> collectObservedEnemies;
		std::function<bool(RE::Actor*, RE::Actor*)> isActorActivelyTargetingPlayerSideForRouter;
		std::function<bool(RE::Actor*)> isPlayerBleedLockActive;
		std::function<std::uint32_t()> resolveBleedFlowActorFormID;
		std::function<void(const char*)> transitionBleedRuntimeToPleasureCommit;
		std::function<RE::Actor*()> resolveObservedDownedFollower;
	};

	Dependencies BuildDefaultDependencies();
	void InstallEarlyProviders(Dependencies dependencies);
	void InstallCoreRuntimeProviders(Dependencies dependencies);
	void InstallPlayerDownAndOverkillProviders(Dependencies dependencies);
	void InstallLifecycleAndRuntimeProviders(Dependencies dependencies);
	void InstallAllProviders(Dependencies dependencies);
	void ShutdownPlayerDownAndOverkillProviders();
	void ShutdownLifecycleProviders();
}
