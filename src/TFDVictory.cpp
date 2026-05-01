#include "TFDVictory.h"

#include "TFDActor.h"

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace TFD::Victory
{
    namespace
    {
        RE::TESGlobal* g_stateGlobal = nullptr;
        bool g_logged = false;
        std::chrono::steady_clock::time_point g_observedCombatContextUntil{};
        static constexpr int kObservedCombatContextLingerMs = 2500;
        static constexpr float kDefeatedDialogueScanRadius = 512.0f;

        void ResolveGlobal()
        {
            if (!g_stateGlobal) {
                g_stateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDVictoryState");
                if (g_stateGlobal && !g_logged) {
                    g_logged = true;
                    spdlog::info("[TFD][Victory] TFDVictoryState resolved {:08X}", g_stateGlobal->GetFormID());
                }
            }
        }

        inline std::chrono::steady_clock::time_point Now()
        {
            return std::chrono::steady_clock::now();
        }

        bool IsUsableDefeatedDialogueActor(RE::Actor* actor, RE::Actor* player)
        {
            if (!actor || !player) {
                return false;
            }
            if (actor == player) {
                return false;
            }
            if (actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
                return false;
            }
            if (actor->GetParentCell() != player->GetParentCell()) {
                return false;
            }
            if (!TFD::Actor::Ops::IsDialogueCapableDefeatedEnemy(actor)) {
                return false;
            }
            if (TFD::Actor::Ops::GetDefeatedEnemyRemainingSeconds(actor) <= 0.0) {
                return false;
            }
            return true;
        }
    }

    void SetStateValue(int value)
    {
        ResolveGlobal();
        if (!g_stateGlobal) {
            return;
        }

        const float desired = static_cast<float>(value);
        if (g_stateGlobal->value != desired) {
            const int previous = static_cast<int>(std::lround(g_stateGlobal->value));
            g_stateGlobal->value = desired;
            spdlog::info("[TFD][Victory] state set previous={} value={}", previous, value);
        }
    }

    int GetStateValue()
    {
        ResolveGlobal();
        return g_stateGlobal ? static_cast<int>(std::lround(g_stateGlobal->value)) : 0;
    }

    bool IsActive()
    {
        return GetStateValue() != 0;
    }

    void ResetObservedContext()
    {
        g_observedCombatContextUntil = {};
    }

    RE::Actor* FindDialogueCapableDefeatedEnemy(float radius)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return nullptr;
        }

        const float scanRadius = (std::max)(radius, kDefeatedDialogueScanRadius);
        const auto snapshot = TFD::Actor::BuildSnapshot(scanRadius, false);

        RE::Actor* best = nullptr;
        float bestDist = scanRadius + 1.0f;

        for (const auto& info : snapshot.actors) {
            auto* actor = info.get();
            if (!IsUsableDefeatedDialogueActor(actor, player)) {
                continue;
            }
            if (info.dist > scanRadius) {
                continue;
            }
            if (info.dist < bestDist) {
                bestDist = info.dist;
                best = actor;
            }
        }

        return best;
    }

    bool HasDialogueCapableDefeatedEnemy(float radius)
    {
        return FindDialogueCapableDefeatedEnemy(radius) != nullptr;
    }

    void MarkDefeatedDialogueAvailable(RE::Actor* actor)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!IsUsableDefeatedDialogueActor(actor, player)) {
            return;
        }
        SetStateValue(3);
    }

    int ComputeObservedState(const ObservedContext& context)
    {
        if (!context.hasPlayer) {
            ResetObservedContext();
            return 0;
        }

        if (context.playerDown) {
            ResetObservedContext();
            return 0;
        }

        if (HasDialogueCapableDefeatedEnemy(kDefeatedDialogueScanRadius)) {
            return 3;
        }

        if (context.combatContext) {
            g_observedCombatContextUntil = Now() + std::chrono::milliseconds(kObservedCombatContextLingerMs);
        }

        if (context.hasEnemies) {
            return 1;
        }

        if (g_observedCombatContextUntil != std::chrono::steady_clock::time_point{} && Now() < g_observedCombatContextUntil) {
            return 2;
        }

        return 0;
    }

    void RefreshObservedState(const ObservedContext& context)
    {
        SetStateValue(ComputeObservedState(context));
    }
}
