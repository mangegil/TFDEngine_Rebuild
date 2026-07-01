#include "TFDDefeatTransitionWiring.h"

#include <mutex>
#include <utility>

#include <spdlog/spdlog.h>

#include "TFDBleedLockRuntime.h"
#include "TFDBleedout.h"
#include "TFDDefeatAggressorResolver.h"
#include "TFDDefeatBleedLockWiring.h"
#include "TFDDefeatFlowRefresh.h"
#include "TFDDefeatStickyReopenGrace.h"
#include "TFDRescue.h"
#include "TFDTeammateManager.h"

namespace TFD::DefeatTransitionWiring
{
	namespace
	{
		std::mutex g_providerLock;
		Dependencies g_dependencies{};
		bool g_hasProvider = false;

		Dependencies ResolveDependencies()
		{
			std::scoped_lock lock(g_providerLock);
			return g_dependencies;
		}
	}

	void InstallProvider(Dependencies dependencies)
	{
		std::scoped_lock lock(g_providerLock);
		g_dependencies = std::move(dependencies);
		g_hasProvider = true;
		spdlog::info("[TFD][TransitionWiring][P28A] provider installed");
	}

	void ShutdownProvider()
	{
		std::scoped_lock lock(g_providerLock);
		g_dependencies = Dependencies{};
		g_hasProvider = false;
		spdlog::info("[TFD][TransitionWiring][P28A] provider cleared");
	}

	bool HasProvider()
	{
		std::scoped_lock lock(g_providerLock);
		return g_hasProvider;
	}

	void StartBleedWindow(RE::Actor* player, RE::Actor* aggressor)
	{
		TFD::DefeatStickyReopenGrace::Clear("start_bleed_window");
		TFD::Bleedout::RuntimeHost::StartWindow(player, aggressor);
	}

	TFD::Transition::RuntimeHandlers BuildTransitionRuntimeHandlers()
	{
		const auto dependencies = ResolveDependencies();
		TFD::Transition::RuntimeHandlers handlers{};
		handlers.getPlayer = [dependencies]() -> RE::Actor* {
			return dependencies.getPlayer ? dependencies.getPlayer() : RE::PlayerCharacter::GetSingleton();
		};
		handlers.resolveAggressor = [dependencies]() -> RE::Actor* {
			return dependencies.resolveAggressor ? dependencies.resolveAggressor() : TFD::DefeatAggressorResolver::ResolveAggressor();
		};
		handlers.findBestAggressor = [dependencies](float radius) -> RE::Actor* {
			return dependencies.findBestAggressor ? dependencies.findBestAggressor(radius) : TFD::DefeatAggressorResolver::FindBestAggressor(radius);
		};
		handlers.isCombatSupportedAggressor = [dependencies](RE::Actor* actor) -> bool {
			return dependencies.isCombatSupportedAggressor ? dependencies.isCombatSupportedAggressor(actor) : TFD::DefeatAggressorResolver::IsCombatSupportedAggressor(actor);
		};
		handlers.isActiveFollowerActor = [dependencies](RE::Actor* actor) -> bool {
			return dependencies.isActiveFollowerActor ? dependencies.isActiveFollowerActor(actor) : TFD::TeammateManager::IsActiveFollowerActor(actor);
		};
		handlers.isStandingAllyThresholdActor = [dependencies](RE::Actor* actor) -> bool {
			return dependencies.isStandingAllyThresholdActor ? dependencies.isStandingAllyThresholdActor(actor) : false;
		};
		handlers.collectRegisteredTeammates = [dependencies]() -> std::vector<RE::Actor*> {
			return dependencies.collectRegisteredTeammates ? dependencies.collectRegisteredTeammates() : TFD::TeammateManager::CollectRegisteredTeammates();
		};
		handlers.collectBleedoutCrowd = [dependencies](float radius, RE::Actor* preferred, bool preserveAssigned) -> std::vector<RE::Actor*> {
			return dependencies.collectBleedoutCrowd ? dependencies.collectBleedoutCrowd(radius, preferred, preserveAssigned) : std::vector<RE::Actor*>{};
		};
		handlers.getBleedCrowdAssigned = []() { return TFD::Bleedout::GetBleedCrowdAssignedIDs(); };
		handlers.releasePlayerBleedLock = [](const char* reason, bool playGetUp) {
			TFD::BleedLockRuntime::ReleasePlayer(TFD::DefeatBleedLockWiring::BuildContext(), reason, playGetUp);
		};
		handlers.setGraceSeconds = [dependencies](int seconds) {
			if (dependencies.setGraceSeconds) {
				dependencies.setGraceSeconds(seconds);
			}
		};
		handlers.setRescueStateValue = [](int value) { TFD::Rescue::SetStateValue(value); };
		handlers.refreshPostDefeatGlobals = []() { TFD::DefeatFlowRefresh::RefreshPostDefeatGlobals(); };
		handlers.updatePreCombatState = []() { TFD::DefeatFlowRefresh::UpdatePreCombatState(); };
		return handlers;
	}

	TFD::DefeatRuntimeActions::NoMarkerFallbackContext BuildNoMarkerFallbackContext()
	{
		const auto dependencies = ResolveDependencies();
		TFD::DefeatRuntimeActions::NoMarkerFallbackContext context{};
		context.buildTransitionRuntimeHandlers = []() { return BuildTransitionRuntimeHandlers(); };
		context.clearCaptiveOrchestrationResidue = []() { TFD::Bleedout::DefeatGlue::ClearCaptiveOrchestrationResidue(true); };
		context.getPlayer = [dependencies]() -> RE::Actor* {
			return dependencies.getPlayer ? dependencies.getPlayer() : RE::PlayerCharacter::GetSingleton();
		};
		context.clearBridgeAliases = [](const char* reason) { TFD::Bleedout::ClearBridgeAliases(nullptr, reason); };
		context.setPlayerBleedImmune = [dependencies](bool immune) {
			if (dependencies.setPlayerBleedImmune) {
				dependencies.setPlayerBleedImmune(immune);
			}
		};
		context.resetBleedRuntimeState = [dependencies]() {
			if (dependencies.resetBleedRuntimeState) {
				dependencies.resetBleedRuntimeState();
			}
		};
		context.clearLastAggressor = []() { TFD::DefeatAggressorResolver::ClearLastAggressor(); };
		context.updatePreCombatState = []() { TFD::DefeatFlowRefresh::UpdatePreCombatState(); };
		return context;
	}
}
