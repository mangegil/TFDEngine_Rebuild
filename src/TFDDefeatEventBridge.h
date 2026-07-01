#pragma once

#include <functional>

namespace RE
{
	class Actor;
}

namespace TFD::DefeatEventBridge
{
	struct Dependencies
	{
		std::function<RE::Actor*()> getPlayerActor;
		std::function<bool(RE::Actor*)> isObserverAlly;
	};

	void Install(Dependencies dependencies);
	void Shutdown();
}
