#include "TFDPostDefeatState.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <chrono>

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

#include "TFDActor.h"
#include "TFDLocation.h"
#include "TFDSettings.h"
#include "TFDTeammateManager.h"
#include "TFDTransition.h"

namespace TFD::PostDefeatState
{
    namespace
    {
        RE::TESGlobal* g_defeatStateGlobal = nullptr;
        RE::TESGlobal* g_hostileStateGlobal = nullptr;
        RE::TESGlobal* g_enemyFactionStateGlobal = nullptr;
        RE::TESGlobal* g_enemyRaceStateGlobal = nullptr;
        RE::TESGlobal* g_recoveryStateGlobal = nullptr;
        RE::TESGlobal* g_leftForDeadStateGlobal = nullptr;

        bool g_loggedDefeatStateGlobal = false;
        bool g_loggedHostileStateGlobal = false;
        bool g_loggedEnemyFactionStateGlobal = false;
        bool g_loggedEnemyRaceStateGlobal = false;
        bool g_loggedRecoveryStateGlobal = false;
        bool g_loggedLeftForDeadStateGlobal = false;

        void ResolveGlobal(RE::TESGlobal*& global, bool& logged, const char* editorID)
        {
            if (global) {
                return;
            }
            global = RE::TESForm::LookupByEditorID<RE::TESGlobal>(editorID);
            if (global && !logged) {
                logged = true;
                spdlog::info("[TFD][PostDefeatState][R391B] {} resolved {:08X}", editorID, global->GetFormID());
            }
        }

        void ResolveGlobals()
        {
            ResolveGlobal(g_defeatStateGlobal, g_loggedDefeatStateGlobal, "TFDDefeatState");
            ResolveGlobal(g_hostileStateGlobal, g_loggedHostileStateGlobal, "TFDHostileState");
            ResolveGlobal(g_enemyFactionStateGlobal, g_loggedEnemyFactionStateGlobal, "TFDEnemyFactionState");
            ResolveGlobal(g_enemyRaceStateGlobal, g_loggedEnemyRaceStateGlobal, "TFDEnemyRaceState");
            ResolveGlobal(g_recoveryStateGlobal, g_loggedRecoveryStateGlobal, "TFDRecoveryState");
            ResolveGlobal(g_leftForDeadStateGlobal, g_loggedLeftForDeadStateGlobal, "TFDLeftForDeadState");
        }

        void SetGlobalInt(RE::TESGlobal* global, int value)
        {
            if (global) {
                global->value = static_cast<float>(value);
            }
        }

        bool ComputePlayerBleedOutState(RE::Actor* player)
        {
            if (!player || player->IsDead() || player->IsDisabled()) {
                return false;
            }
            const float hpNow = player->GetActorValue(RE::ActorValue::kHealth);
            const float hpMax = (std::max)(1.0f, player->GetPermanentActorValue(RE::ActorValue::kHealth));
            const float pct = (hpNow / hpMax) * 100.0f;
            return pct <= TFD::Settings::GetDefeatThresholdPct();
        }

        int ComputeDefeatState(RE::Actor* player, bool combatContext, bool forcePlayerBleedout)
        {
            if (!player) {
                return 0;
            }
            if (forcePlayerBleedout) {
                return 2;
            }
            if (!combatContext) {
                return 0;
            }
            return ComputePlayerBleedOutState(player) ? 2 : 1;
        }

        int ComputeHostileState(RE::Actor* player, const std::vector<RE::Actor*>& enemies, bool forcePlayerBleedout)
        {
            if (!player || forcePlayerBleedout || ComputePlayerBleedOutState(player)) {
                return 0;
            }
            const int count = static_cast<int>(enemies.size());
            if (count <= 0) {
                return 0;
            }
            return count == 1 ? 1 : 2;
        }

        bool ActorHasKeywordByEditorID(RE::Actor* actor, const char* editorID)
        {
            if (!actor || !editorID || !*editorID) {
                return false;
            }
            auto* form = RE::TESForm::LookupByEditorID(editorID);
            auto* keyword = form ? form->As<RE::BGSKeyword>() : nullptr;
            return keyword && actor->HasKeyword(keyword);
        }

        std::uint32_t ComputeEnemyRaceKey(RE::Actor* actor)
        {
            if (!actor) {
                return 0;
            }
            if (ActorHasKeywordByEditorID(actor, "ActorTypeNPC")) {
                return 1;
            }
            if (auto* race = actor->GetRace()) {
                return race->GetFormID();
            }
            return 0;
        }

        int ComputeEnemyRaceState(const std::vector<RE::Actor*>& enemies)
        {
            if (enemies.empty()) {
                return 0;
            }
            if (enemies.size() == 1) {
                return 1;
            }
            std::uint32_t firstKey = 0;
            for (auto* enemy : enemies) {
                const auto key = ComputeEnemyRaceKey(enemy);
                if (key == 0) {
                    continue;
                }
                if (firstKey == 0) {
                    firstKey = key;
                    continue;
                }
                if (key != firstKey) {
                    return 2;
                }
            }
            return 1;
        }

        int ComputeEnemyFactionState(const std::vector<RE::Actor*>& enemies)
        {
            if (enemies.empty()) {
                return 0;
            }
            if (enemies.size() == 1) {
                return 1;
            }
            for (std::size_t i = 0; i < enemies.size(); ++i) {
                auto* lhs = enemies[i];
                if (!lhs) {
                    continue;
                }
                for (std::size_t j = i + 1; j < enemies.size(); ++j) {
                    auto* rhs = enemies[j];
                    if (!rhs) {
                        continue;
                    }
                    if (lhs->IsHostileToActor(rhs) || rhs->IsHostileToActor(lhs)) {
                        return 2;
                    }
                }
            }
            return 1;
        }

        int ComputeRecoveryState()
        {
            return TFD::Transition::HasRecoveryPotionAvailable() ? 1 : 0;
        }

        int ComputeLeftForDeadState(RE::Actor* player)
        {
            (void)player;
            const float followerRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 400.0f);
            auto followers = TFD::TeammateManager::ResolveFollowerCandidates(followerRadius);
            const bool hasLivingFollower = (followers.standing != nullptr);
            const bool hasRescueMarker = (TFD::Location::ResolveMostRecentCachedRescueDestination(true) != nullptr) ||
                (TFD::Location::ResolveMostRecentCachedRescueDestination(false) != nullptr);
            const bool hasRecoveryFactor = TFD::Transition::HasRecoveryPotionAvailable();
            return ((hasLivingFollower && hasRescueMarker) || hasRecoveryFactor) ? 0 : 1;
        }
    }

    RefreshResult Refresh(const RefreshInput& input)
    {
        ResolveGlobals();

        if (input.forcePlayerBleedout) {
            static std::chrono::steady_clock::time_point s_lastOwnerOverrideLog{};
            const auto now = std::chrono::steady_clock::now();
            if (s_lastOwnerOverrideLog.time_since_epoch().count() == 0 ||
                (now - s_lastOwnerOverrideLog) >= std::chrono::milliseconds(1200)) {
                s_lastOwnerOverrideLog = now;
                spdlog::info(
                    "[TFD][PostDefeatState][R20] bleedout owner override root={} gate={} lock={} runtime={} hpThresholdIgnored=1",
                    input.ownerRootName ? input.ownerRootName : "None",
                    input.ownerGateName ? input.ownerGateName : "None",
                    input.playerBleedLockActive ? 1 : 0,
                    input.playerBleedRuntimeActive ? 1 : 0);
            }
        }

        RefreshResult result{};
        result.routerCombatContextActive = input.routerCombatContext;

        if (input.pleasurePassiveLock || input.onlySuppressedDialogueEnemies) {
            result.routerCombatContextActive = false;
        }

        const bool playerDownNow = input.forcePlayerBleedout || ComputePlayerBleedOutState(input.player);
        const bool defeatContext = input.forcePlayerBleedout || input.defeatContext || (input.battleObserveHold && input.player && playerDownNow);

        SetGlobalInt(g_defeatStateGlobal, ComputeDefeatState(input.player, defeatContext, input.forcePlayerBleedout));
        SetGlobalInt(g_hostileStateGlobal, ComputeHostileState(input.player, input.enemies, input.forcePlayerBleedout));
        SetGlobalInt(g_enemyFactionStateGlobal, ComputeEnemyFactionState(input.enemies));
        SetGlobalInt(g_enemyRaceStateGlobal, ComputeEnemyRaceState(input.enemies));
        SetGlobalInt(g_recoveryStateGlobal, ComputeRecoveryState());
        SetGlobalInt(g_leftForDeadStateGlobal, ComputeLeftForDeadState(input.player));


        return result;
    }
}
