#pragma once

#include "TFDTransition.h"

namespace TFD::LeftForDead
{
	struct State
	{
		TFD::Transition::FallbackBranch branch{ TFD::Transition::FallbackBranch::None };
		RE::ActorHandle follower{};
		RE::ObjectRefHandle destination{};
		RE::NiPoint3 fallbackPos{};
		bool hasFallbackPos{ false };
		float angleZ{ 0.0f };
	};

	struct Handlers
	{
		std::function<void(RE::Actor*, float)> applyFacing;
		std::function<void(RE::Actor*)> setFollowerHold;
		std::function<void()> maintainCalmWindow;
		std::function<void(int)> beginCooldown;
		std::function<void(bool)> setAggroKickNeeded;
	};

	void ExecuteWake(const State& state,
		const char* reason,
		const TFD::Transition::RuntimeHandlers& runtimeHandlers,
		const Handlers& handlers);
}
