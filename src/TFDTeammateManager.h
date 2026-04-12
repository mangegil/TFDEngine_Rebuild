#pragma once

#include <cstddef>

namespace RE
{
    class Actor;
}

namespace TFD::TeammateManager
{
    void Install();
    void Shutdown();
    void SyncNow();

    std::size_t RestoreNow();

    bool IsCreatureCompanion(RE::Actor* actor);
    double GetRemainingCreatureCompanionHours(RE::Actor* actor);
    bool ReleaseCreatureCompanion(RE::Actor* actor);
}
