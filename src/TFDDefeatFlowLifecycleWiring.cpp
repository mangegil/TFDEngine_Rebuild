#include "TFDDefeatFlowLifecycleWiring.h"

#include <mutex>
#include <utility>

#include <spdlog/spdlog.h>

#include "TFDActor.h"
#include "TFDBleedLockRuntime.h"
#include "TFDBleedout.h"
#include "TFDDefeatBleedLockWiring.h"
#include "TFDDefeatBridge.h"
#include "TFDDefeatRuntimeActions.h"
#include "TFDHostilityController.h"
#include "TFDSettings.h"
#include "TFDTame.h"
#include "TFDTransition.h"

namespace TFD::DefeatFlowLifecycleWiring
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

		static RE::Actor* ResolveCurrentPassivePrimaryActor()
		{
			return nullptr;
		}

		static bool IsActorCoveredByCurrentPassiveContext(RE::Actor*)
		{
			return false;
		}
	}

	void InstallProvider(Dependencies dependencies)
	{
		std::scoped_lock lock(g_providerLock);
		g_dependencies = std::move(dependencies);
		g_hasProvider = true;
		spdlog::info("[TFD][DefeatFlowLifecycle][P27D] provider installed");
	}

	void ShutdownProvider()
	{
		std::scoped_lock lock(g_providerLock);
		g_dependencies = Dependencies{};
		g_hasProvider = false;
		spdlog::info("[TFD][DefeatFlowLifecycle][P27D] provider cleared");
	}

	bool HasProvider()
	{
		std::scoped_lock lock(g_providerLock);
		return g_hasProvider;
	}

	TFD::FlowController::DefeatLifecycleProviders BuildProviders()
	{
		const auto dependencies = ResolveDependencies();
		return TFD::FlowController::DefeatLifecycleProviders{
		TFD::FlowController::PassiveRuntimeProviders{
			[dependencies]() { return dependencies.inBleedState ? dependencies.inBleedState() : false; },
			[]() { return TFD::Actor::Ops::HasAnyReleaseFollowGrace(); },
			[]() -> RE::Actor* { return ResolveCurrentPassivePrimaryActor(); },
			[](RE::Actor* actor) { return IsActorCoveredByCurrentPassiveContext(actor); },
			[](RE::Actor* actor, const char* reason) { TFD::Actor::Ops::CancelReleaseFollowGraceFromPlayerAggression(actor, reason); },
			[](const char* reason) { TFD::Actor::Ops::ClearAllReleaseFollowGrace(reason); },
			[]() { TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::PlayerAggression); },
			[](const char* reason) { TFD::BleedLockRuntime::ClearAll(TFD::DefeatBleedLockWiring::BuildContext(), reason); },
			[](RE::Actor* actor, const char* reason) { TFD::Bleedout::ClearBridgeAliases(actor, reason); }
		},
		TFD::FlowController::OutcomeRuntimeProviders{
			[](RE::Actor* actor, double seconds, const char* reason) { TFD::Actor::Ops::ApplyReleaseFollowGraceToSpeakerAndCrowd(actor, seconds, reason); },
			[](RE::Actor* actor, const char* reason) { TFD::Actor::Ops::RemoveReleaseFollowGraceFromSpeakerAndCrowd(actor, reason); },
			[dependencies]() { return dependencies.resolveBleedFlowActorFormID ? dependencies.resolveBleedFlowActorFormID() : 0u; },
			[dependencies]() { return dependencies.inBleedState ? dependencies.inBleedState() : false; },
			[](const char* reason) { (void)TFD::DefeatRuntimeActions::CompletePayRelease(reason ? reason : "mod_event_pay_immediate_terminal"); },
			[](const char* reason) { TFD::Bleedout::DefeatGlue::PreparePlayerForCaptivePleasureScene(reason); },
			[](const char* reason) { auto completion = TFD::Bleedout::Builders::BuildCaptivePleasureCompletionHandlers(); (void)TFD::Bleedout::CompleteCaptivePleasureHandoff(reason, completion); },
			[](const char* reason) { TFD::Bleedout::DefeatGlue::PreparePlayerForBleedoutPleasureScene(reason); },
			[](const char* reason) { auto completion = TFD::Bleedout::Builders::BuildBleedPleasureCompletionHandlers(); (void)TFD::Bleedout::CompleteBleedPleasureHandoff(reason ? reason : "bleed_pleasure_handoff", completion); }
		},
		TFD::FlowController::BattleObserverRuntimeProviders{
			[](const char* reason) { TFD::Bleedout::ClearBridgeAliases(nullptr, reason); },
			[]() { TFD::Bleedout::DefeatGlue::ClearCaptiveOrchestrationResidue(true); },
			[]() { TFD::Actor::Ops::ClearAggressorFactionContext(); },
			[dependencies]() { if (dependencies.resetBleedRuntimeState) { dependencies.resetBleedRuntimeState(); } },
			[dependencies](bool immune) { if (dependencies.setPlayerBleedImmune) { dependencies.setPlayerBleedImmune(immune); } },
			[dependencies](const char* reason) {
				if (!dependencies.buildNoMarkerFallbackContext) {
					return false;
				}
				return TFD::DefeatRuntimeActions::ExecuteResolvedNoMarkerFallback(reason ? reason : "battle_observe_win", dependencies.buildNoMarkerFallbackContext());
			},
			[](const char* reason) { TFD::DefeatBridge::QueueNonCaptiveChoiceRequest(reason); },
			[dependencies]() -> RE::Actor* { return dependencies.resolveObservedDownedFollower ? dependencies.resolveObservedDownedFollower() : nullptr; },
			[dependencies](RE::Actor* follower) { if (dependencies.buildTransitionRuntimeHandlers) { TFD::Transition::ArmObservedLeftForDeadFallback(follower, dependencies.buildTransitionRuntimeHandlers()); } },
			[dependencies](const char* reason) { if (dependencies.buildTransitionRuntimeHandlers) { TFD::Transition::BeginRecoverTransition(reason ? reason : "battle_observe_loss", dependencies.buildTransitionRuntimeHandlers()); } },
			[]() -> const char* { return TFD::Transition::GetCurrentFallbackBranchName(); }
		}
		};
	}
}
