#pragma once

#include "RE/Skyrim.h"

#include <functional>

#include "TFDInteractionRouter.h"

namespace TFD::PreCombatGreet
{

	struct GraceEventContext
	{
		const char* eventName{ nullptr };
		RE::Actor* actor{ nullptr };
		double durationSec{ 20.0 };
	};

	struct GraceEventHandlers
	{
		std::function<void(RE::Actor*, double, const char*)> applyGrace;
		std::function<void(RE::Actor*, const char*)> removeGrace;
	};
	void Install();
	void Shutdown();

	void SetSuspended(bool suspended);
	bool IsSuspended();

	bool BeginForActor(RE::Actor* actor, TFD::InteractionRouter::Action* outAction = nullptr);
	void CancelAll();

	void OnPreLoadGame();
	void OnPostLoadGame();
	void OnLoadingScreenClosed();
	void OnCaptiveHandoffArrived();

	bool HandleReleaseFollowEvent(const GraceEventContext& context, const GraceEventHandlers& handlers);
	bool HandleReleaseEndEvent(RE::Actor* actor, const GraceEventHandlers& handlers);
	bool HandleGraceModEvent(const char* rawEventName, RE::Actor* actor, double durationSec, const GraceEventHandlers& handlers);

	RE::Actor* GetRecentActor(double maxAgeSec = 0.0);
}
