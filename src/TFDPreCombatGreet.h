#pragma once

#include "RE/Skyrim.h"

#include <functional>

#include "TFDInteractionRouter.h"

namespace SKSE
{
	struct ModCallbackEvent;
}

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

	bool HandleGraceModEvent(const GraceEventContext& context, const GraceEventHandlers& handlers);
	bool HandleGraceModEventRaw(const char* eventName, const char* eventArg, double durationSec, const GraceEventHandlers& handlers);
bool HandleModEventRaw(const char* eventName, const char* eventArg, double durationSec, const GraceEventHandlers& handlers);
	bool HandleModCallbackEvent(const SKSE::ModCallbackEvent* ev, const GraceEventHandlers& handlers);

	RE::Actor* GetRecentActor(double maxAgeSec = 0.0);
	RE::Actor* ResolveRecentAggressor(float radius, double maxAgeSec = 12.0);
	RE::Actor* ResolveRecentAggressorAndCache(float radius, RE::ActorHandle& cacheHandle, double maxAgeSec = 12.0);
}
