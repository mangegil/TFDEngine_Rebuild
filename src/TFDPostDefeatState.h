#pragma once

#include <cstddef>
#include <vector>

namespace RE
{
    class Actor;
}

namespace TFD::PostDefeatState
{
    struct RefreshInput
    {
        RE::Actor* player{ nullptr };
        std::vector<RE::Actor*> enemies{};
        bool defeatContext{ false };
        bool victoryContext{ false };
        bool routerCombatContext{ false };
        bool onlySuppressedDialogueEnemies{ false };
        std::size_t suppressedEnemyCount{ 0 };
        bool pleasurePassiveLock{ false };
        bool battleObserveHold{ false };
    };

    struct RefreshResult
    {
        bool routerCombatContextActive{ false };
    };

    RefreshResult Refresh(const RefreshInput& input);
}
