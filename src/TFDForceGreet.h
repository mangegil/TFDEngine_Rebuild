#pragma once
#include <cstdint>

namespace RE
{
	class Actor;
}

namespace TFD::ForceGreet
{
	enum class Mode : std::int32_t
	{
		None = 0,
		Bleedout = 1,
		CaptiveMarker = 2
	};

	void Install();

	void BeginBleedout(RE::Actor* speaker);
	void BeginCaptiveMarker(RE::Actor* speaker);

	void Tick();

	void Cancel();
	bool IsActive();
	bool DidSucceed();
	Mode GetMode();
}