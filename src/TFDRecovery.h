#pragma once

#include "TFDTransition.h"

namespace TFD::Recovery
{
	struct State
	{
		TFD::Transition::FallbackBranch branch{ TFD::Transition::FallbackBranch::None };
		RE::ActorHandle follower{};
		RE::FormID potionFormId{ 0 };
	};

	struct Handlers
	{
		std::function<void(RE::TESForm*, const char*)> clearPlayerSavior;
		std::function<void()> recoverPlayerForTransition;
		std::function<void(RE::Actor*)> setFollowerHold;
		std::function<void(int, int)> finalizePostDefeatRecoveryWindow;
	};

	bool IsFollowerInvalidForRecovery(RE::Actor* follower);
	void ExecuteResolvedBranch(const State& state,
		const char* reason,
		const Handlers& handlers,
		const std::function<void(const char*)>& executeLeftForDead,
		const std::function<const char*(TFD::Transition::FallbackBranch)>& getBranchName);
}
