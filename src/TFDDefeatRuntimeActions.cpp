#include "TFDDefeatRuntimeActions.h"

#include <algorithm>

#include <spdlog/spdlog.h>

#include "TFDBleedout.h"
#include "TFDHostilityController.h"

namespace TFD::DefeatRuntimeActions
{
	TFD::Bleedout::RuntimeHostStateRefs BuildBleedRuntimeHostStateRefs()
	{
		return TFD::Bleedout::RuntimeHost::BuildStateRefs();
	}

	bool CompletePayRelease(const char* reason)
	{
		auto completion = TFD::Bleedout::Builders::BuildPayReleaseCompletionHandlers();
		return TFD::Bleedout::CompletePayRelease(reason ? reason : "defeat_runtime_pay_release", completion);
	}

	void ApplyTerminalCalmBubble(RE::Actor* player, RE::Actor* primary, float radius, float configuredSweepRadius, const char* reason)
	{
		if (!player || !primary) {
			return;
		}
		const float sweepRadius = (std::max)(radius, (std::max)(configuredSweepRadius, 12000.0f));
		spdlog::info(
			"[TFD][DefeatRuntimeActions][R22] terminal calm bubble delegated radius={:.0f} primary={:08X} reason={}",
			sweepRadius,
			primary->GetFormID(),
			reason ? reason : "-");
		TFD::HostilityController::StopCombatAndAlarmSweep(sweepRadius, true, reason ? reason : "defeat_runtime_terminal_calm");
		TFD::HostilityController::ScheduleStopCombatAndAlarmWaves(sweepRadius, true, 3, 180, reason ? reason : "defeat_runtime_terminal_calm");
	}

	TFD::FlowController::NonCaptiveFallbackExecutionHandlers BuildNoMarkerFallbackExecutionHandlers(const NoMarkerFallbackContext& context)
	{
		TFD::FlowController::NonCaptiveFallbackExecutionHandlers handlers{};
		handlers.tryBeginTerminalCommit = [](TFD::Bleedout::TerminalCommit kind, const char* reason) {
			return TFD::Bleedout::TryBeginTerminalCommit(kind, reason);
		};
		handlers.clearCaptiveOrchestrationResidue = [context]() {
			if (context.clearCaptiveOrchestrationResidue) {
				context.clearCaptiveOrchestrationResidue();
			}
		};
		handlers.getPlayer = [context]() -> RE::Actor* {
			return context.getPlayer ? context.getPlayer() : nullptr;
		};
		handlers.clearBridgeAliases = [context](const char* reason) {
			if (context.clearBridgeAliases) {
				context.clearBridgeAliases(reason);
			}
		};
		handlers.setPlayerBleedImmune = [context](bool immune) {
			if (context.setPlayerBleedImmune) {
				context.setPlayerBleedImmune(immune);
			}
		};
		handlers.resetBleedRuntimeState = [context]() {
			if (context.resetBleedRuntimeState) {
				context.resetBleedRuntimeState();
			}
		};
		handlers.clearLastAggressor = [context]() {
			if (context.clearLastAggressor) {
				context.clearLastAggressor();
			}
		};
		handlers.updatePreCombatState = [context]() {
			if (context.updatePreCombatState) {
				context.updatePreCombatState();
			}
		};
		handlers.resolveNoMarkerFallback = [context](const char* reason) {
			return TFD::Transition::ResolveNoMarkerFallback(
				reason,
				context.buildTransitionRuntimeHandlers ? context.buildTransitionRuntimeHandlers() : TFD::Transition::RuntimeHandlers{});
		};
		handlers.getBranchName = [](TFD::Transition::FallbackBranch branch) {
			return TFD::Transition::GetBranchName(branch);
		};
		handlers.beginRescueTransition = [context](const char* reason) {
			return TFD::Transition::BeginRescueTransition(
				reason,
				context.buildTransitionRuntimeHandlers ? context.buildTransitionRuntimeHandlers() : TFD::Transition::RuntimeHandlers{});
		};
		handlers.forceLeftForDeadSolo = [context]() {
			TFD::Transition::ForceLeftForDeadSolo(
				context.buildTransitionRuntimeHandlers ? context.buildTransitionRuntimeHandlers() : TFD::Transition::RuntimeHandlers{});
		};
		handlers.beginRecoverTransition = [context](const char* reason) {
			TFD::Transition::BeginRecoverTransition(
				reason,
				context.buildTransitionRuntimeHandlers ? context.buildTransitionRuntimeHandlers() : TFD::Transition::RuntimeHandlers{});
		};
		return handlers;
	}

	bool ExecuteResolvedNoMarkerFallback(const char* reason, const NoMarkerFallbackContext& context)
	{
		return TFD::FlowController::ExecuteResolvedNoMarkerFallback(
			reason ? reason : "defeat_runtime_no_marker_fallback",
			BuildNoMarkerFallbackExecutionHandlers(context));
	}
}
