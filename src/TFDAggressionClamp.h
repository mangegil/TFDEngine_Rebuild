#pragma once
#include "RE/Skyrim.h"

namespace TFD::AggressionClamp
{
    // Apply a temporary "pacify" to an actor.
    // Safe to call repeatedly (stateful + throttled).
    void Apply(RE::Actor* a);

    // Restore original aggression for all cached actors.
    void Clear();
}