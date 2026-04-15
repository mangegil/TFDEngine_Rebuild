#pragma once

#include <cstdint>

namespace RE
{
    class Actor;
}

namespace SKSE
{
    struct ModCallbackEvent;
}

namespace TFD::Release
{
    enum class Source : std::uint8_t
    {
        None = 0,
        PreCombat,
        InCombat,
        Bleedout,
        AfterPleasure
    };

    void Install();
    void Reset();
    void Tick();

    bool BeginRelease(RE::Actor* actor, double graceSeconds, Source source, const char* reason = nullptr);
    bool HandleModCallbackEvent(const SKSE::ModCallbackEvent* ev);

    bool IsActive();
    const char* ToString(Source source);
}
