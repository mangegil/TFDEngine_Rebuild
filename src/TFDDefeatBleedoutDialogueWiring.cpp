#include "TFDDefeatBleedoutDialogueWiring.h"

#include <mutex>
#include <utility>

#include <spdlog/spdlog.h>

#include "TFDDefeatAggressorResolver.h"
#include "TFDDefeatRuntimeActions.h"

namespace TFD::DefeatBleedoutDialogueWiring
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
		spdlog::info("[TFD][BleedoutDialogue][P28B] provider installed");
	}

	void ShutdownProvider()
	{
		std::scoped_lock lock(g_providerLock);
		g_dependencies = {};
		g_hasProvider = false;
		spdlog::info("[TFD][BleedoutDialogue][P28B] provider cleared");
	}

	bool HasProvider()
	{
		std::scoped_lock lock(g_providerLock);
		return g_hasProvider;
	}

	TFD::Bleedout::SpeakerLogicHandlers BuildSpeakerHandlers(bool preserveAssigned)
	{
		const auto dependencies = ResolveDependencies();
		TFD::Bleedout::SpeakerLogicHandlers handlers{};
		handlers.getPlayer = [dependencies]() -> RE::Actor* {
			return dependencies.getPlayer ? dependencies.getPlayer() : RE::PlayerCharacter::GetSingleton();
		};
		handlers.isStandingEnemyThresholdActor = [dependencies](RE::Actor* actor) -> bool {
			return dependencies.isStandingEnemyThresholdActor ? dependencies.isStandingEnemyThresholdActor(actor) : false;
		};
		handlers.isCaptiveSupportedAggressor = [](RE::Actor* actor) -> bool {
			return TFD::DefeatAggressorResolver::IsCaptiveSupportedAggressor(actor);
		};
		handlers.isBleedCrowdSupportedAggressor = [](RE::Actor* actor) -> bool {
			return TFD::DefeatAggressorResolver::IsBleedCrowdSupportedAggressor(actor);
		};
		handlers.isBleedSpaceCompatible = [dependencies](RE::Actor* actor, RE::Actor* player) -> bool {
			return dependencies.isBleedSpaceCompatible ? dependencies.isBleedSpaceCompatible(actor, player) : false;
		};
		handlers.hasLineOfSightToPlayer = [dependencies](RE::Actor* actor, RE::Actor* player) -> bool {
			return dependencies.hasLineOfSightToPlayer ? dependencies.hasLineOfSightToPlayer(actor, player) : false;
		};
		handlers.isActorCloseAndFront = [dependencies](RE::Actor* actor, RE::Actor* player, float maxDist) -> bool {
			return dependencies.isActorCloseAndFront ? dependencies.isActorCloseAndFront(actor, player, maxDist) : false;
		};
		handlers.resolveCurrentCombatTarget = [dependencies](RE::Actor* actor) -> RE::Actor* {
			return dependencies.resolveCurrentCombatTarget ? dependencies.resolveCurrentCombatTarget(actor) : nullptr;
		};
		handlers.isActiveFollowerActor = [dependencies](RE::Actor* actor) -> bool {
			return dependencies.isActiveFollowerActor ? dependencies.isActiveFollowerActor(actor) : false;
		};
		handlers.resolveLastAggressor = []() -> RE::Actor* {
			return TFD::DefeatAggressorResolver::ResolveLastAggressor();
		};
		handlers.isPreservedAssigned = [preserveAssigned](RE::Actor* actor) {
			if (!preserveAssigned || !actor) {
				return false;
			}
			return TFD::Bleedout::HasBleedCrowdAssignedID(actor->GetFormID());
		};
		return handlers;
	}

	TFD::BleedoutDialogueRuntime::Context BuildRuntimeContext()
	{
		const auto dependencies = ResolveDependencies();
		TFD::BleedoutDialogueRuntime::Context context{};
		context.getPlayer = [dependencies]() -> RE::Actor* {
			return dependencies.getPlayer ? dependencies.getPlayer() : RE::PlayerCharacter::GetSingleton();
		};
		context.buildSpeakerHandlers = [](bool preserveAssigned) { return BuildSpeakerHandlers(preserveAssigned); };
		context.buildRuntimeStateRefs = []() { return TFD::DefeatRuntimeActions::BuildBleedRuntimeHostStateRefs(); };
		context.buildRuntimeHostHandlers = []() { return TFD::Bleedout::RuntimeHost::BuildHandlers(); };
		context.currentSpeakerID = []() { return TFD::Bleedout::GetBleedSpeakerID(); };
		context.ownsCurrentFlow = []() { return TFD::Bleedout::OwnsCurrentFlow(); };
		return context;
	}

	std::vector<RE::Actor*> CollectCrowd(float radius, RE::Actor* preferred, bool preserveAssigned)
	{
		return TFD::Bleedout::CollectCrowd(radius, preferred, preserveAssigned, BuildSpeakerHandlers(preserveAssigned));
	}

	RE::Actor* FindBestSpeaker(float scanRadius, float maxDistance, RE::Actor* preferred)
	{
		return TFD::BleedoutDialogueRuntime::FindBestSpeaker(scanRadius, maxDistance, preferred, BuildRuntimeContext());
	}
}
