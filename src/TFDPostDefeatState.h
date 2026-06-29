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
        bool routerCombatContext{ false };
        bool onlySuppressedDialogueEnemies{ false };
        std::size_t suppressedEnemyCount{ 0 };
        bool pleasurePassiveLock{ false };
        bool battleObserveHold{ false };
        // R20: R429A/R447A intentionally raise the player's raw HP during
        // TFD-owned bleedout to absorb overkill hits.  Post-defeat globals must
        // therefore trust the flow/guard owner state, not raw HP alone.
        bool forcePlayerBleedout{ false };
        bool playerBleedLockActive{ false };
        bool playerBleedRuntimeActive{ false };
        const char* ownerRootName{ "None" };
        const char* ownerGateName{ "None" };
    };

    struct RefreshResult
    {
        bool routerCombatContextActive{ false };
    };

    RefreshResult Refresh(const RefreshInput& input);
}
