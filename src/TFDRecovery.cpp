#include "TFDRecovery.h"

#include <spdlog/spdlog.h>

namespace TFD::Recovery
{
	bool IsFollowerInvalidForRecovery(RE::Actor* follower)
	{
		return !follower || follower->IsDead() || (follower->AsActorState() && follower->AsActorState()->IsBleedingOut());
	}

	namespace
	{
		static void ExecuteFollowerBranch(const State& state,
			const char* reason,
			const Handlers& handlers,
			const std::function<void(const char*)>& executeLeftForDead,
			const std::function<const char*(TFD::Transition::FallbackBranch)>& getBranchName)
		{
			auto followerSp = RE::Actor::LookupByHandle(state.follower.native_handle());
			auto* follower = followerSp.get();
			if (IsFollowerInvalidForRecovery(follower)) {
				if (executeLeftForDead) {
					executeLeftForDead(reason ? reason : "recovery_follower_degraded_lfd");
				}
				return;
			}

			if (handlers.recoverPlayerForTransition) {
				handlers.recoverPlayerForTransition();
			}
			if (handlers.setFollowerHold) {
				handlers.setFollowerHold(follower);
			}
			if (handlers.finalizePostDefeatRecoveryWindow) {
				handlers.finalizePostDefeatRecoveryWindow(3, 0);
			}
			spdlog::info("[TFD][Recovery] complete branch={} reason={} follower={:08X}",
				getBranchName ? getBranchName(state.branch) : "unknown",
				reason ? reason : "unknown",
				follower->GetFormID());
		}

		static void ExecutePotionBranch(const State& state,
			const char* reason,
			const Handlers& handlers,
			const std::function<const char*(TFD::Transition::FallbackBranch)>& getBranchName)
		{
			if (handlers.recoverPlayerForTransition) {
				handlers.recoverPlayerForTransition();
			}
			if (handlers.finalizePostDefeatRecoveryWindow) {
				handlers.finalizePostDefeatRecoveryWindow(3, 0);
			}
			spdlog::info("[TFD][Recovery] complete branch={} reason={} potion={:08X}",
				getBranchName ? getBranchName(state.branch) : "unknown",
				reason ? reason : "unknown",
				state.potionFormId);
		}

		static void ExecuteGenericBranch(const State& state,
			const char* reason,
			const Handlers& handlers,
			const std::function<const char*(TFD::Transition::FallbackBranch)>& getBranchName)
		{
			if (handlers.recoverPlayerForTransition) {
				handlers.recoverPlayerForTransition();
			}
			if (handlers.finalizePostDefeatRecoveryWindow) {
				handlers.finalizePostDefeatRecoveryWindow(3, 0);
			}
			spdlog::info("[TFD][Recovery] complete branch={} reason={}",
				getBranchName ? getBranchName(state.branch) : "unknown",
				reason ? reason : "unknown");
		}
	}

	void ExecuteResolvedBranch(const State& state,
		const char* reason,
		const Handlers& handlers,
		const std::function<void(const char*)>& executeLeftForDead,
		const std::function<const char*(TFD::Transition::FallbackBranch)>& getBranchName)
	{
		if (handlers.clearPlayerSavior) {
			handlers.clearPlayerSavior(nullptr, "recover_transition");
		}

		switch (state.branch) {
		case TFD::Transition::FallbackBranch::RecoveryFollower:
			ExecuteFollowerBranch(state, reason, handlers, executeLeftForDead, getBranchName);
			return;
		case TFD::Transition::FallbackBranch::RecoveryPotion:
			ExecutePotionBranch(state, reason, handlers, getBranchName);
			return;
		case TFD::Transition::FallbackBranch::LeftForDeadSolo:
		case TFD::Transition::FallbackBranch::LeftForDeadWithFollower:
			if (executeLeftForDead) {
				executeLeftForDead(reason);
			}
			return;
		default:
			ExecuteGenericBranch(state, reason, handlers, getBranchName);
			return;
		}
	}
}
