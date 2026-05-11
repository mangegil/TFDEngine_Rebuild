#include "TFDPostDefeatState.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <chrono>

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

#include "TFDFlowController.h"
#include "TFDActor.h"
#include "TFDLocation.h"
#include "TFDSettings.h"
#include "TFDTeammateManager.h"
#include "TFDTransition.h"
#include "TFDVictory.h"
#include "TFDPleasureRuntime.h"

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

        void ResolveGlobals()
        {
            if (!g_defeatStateGlobal) {
                g_defeatStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDDefeatState");
                if (g_defeatStateGlobal && !g_loggedDefeatStateGlobal) {
                    g_loggedDefeatStateGlobal = true;
                    spdlog::info("[TFD][PostDefeatState] TFDDefeatState resolved {:08X}", g_defeatStateGlobal->GetFormID());
                }
            }
            if (!g_hostileStateGlobal) {
                g_hostileStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDHostileState");
                if (g_hostileStateGlobal && !g_loggedHostileStateGlobal) {
                    g_loggedHostileStateGlobal = true;
                    spdlog::info("[TFD][PostDefeatState] TFDHostileState resolved {:08X}", g_hostileStateGlobal->GetFormID());
                }
            }
            if (!g_enemyFactionStateGlobal) {
                g_enemyFactionStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDEnemyFactionState");
                if (g_enemyFactionStateGlobal && !g_loggedEnemyFactionStateGlobal) {
                    g_loggedEnemyFactionStateGlobal = true;
                    spdlog::info("[TFD][PostDefeatState] TFDEnemyFactionState resolved {:08X}", g_enemyFactionStateGlobal->GetFormID());
                }
            }
            if (!g_enemyRaceStateGlobal) {
                g_enemyRaceStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDEnemyRaceState");
                if (g_enemyRaceStateGlobal && !g_loggedEnemyRaceStateGlobal) {
                    g_loggedEnemyRaceStateGlobal = true;
                    spdlog::info("[TFD][PostDefeatState] TFDEnemyRaceState resolved {:08X}", g_enemyRaceStateGlobal->GetFormID());
                }
            }
            if (!g_recoveryStateGlobal) {
                g_recoveryStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDRecoveryState");
                if (g_recoveryStateGlobal && !g_loggedRecoveryStateGlobal) {
                    g_loggedRecoveryStateGlobal = true;
                    spdlog::info("[TFD][PostDefeatState] TFDRecoveryState resolved {:08X}", g_recoveryStateGlobal->GetFormID());
                }
            }
            if (!g_leftForDeadStateGlobal) {
                g_leftForDeadStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDLeftForDeadState");
                if (g_leftForDeadStateGlobal && !g_loggedLeftForDeadStateGlobal) {
                    g_loggedLeftForDeadStateGlobal = true;
                    spdlog::info("[TFD][PostDefeatState] TFDLeftForDeadState resolved {:08X}", g_leftForDeadStateGlobal->GetFormID());
                }
            }
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
            const float thresh = TFD::Settings::GetDefeatThresholdPct();
            return pct <= thresh;
        }

        int ComputeDefeatState(RE::Actor* player, bool combatContext)
        {
            if (!player || !combatContext) {
                return 0;
            }
            return ComputePlayerBleedOutState(player) ? 2 : 1;
        }

        int ComputeHostileState(RE::Actor* player, const std::vector<RE::Actor*>& enemies)
        {
            if (!player || ComputePlayerBleedOutState(player)) {
                return 0;
            }

            const int count = static_cast<int>(enemies.size());
            if (count <= 0) {
                return 0;
            }
            if (count == 1) {
                return 1;
            }
            return 2;
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
            const bool hasRescueFactor = hasLivingFollower && hasRescueMarker;
            const bool hasRecoveryFactor = TFD::Transition::HasRecoveryPotionAvailable();
            return (hasRescueFactor || hasRecoveryFactor) ? 0 : 1;
        }

        static constexpr int kVictoryStateNeutral = 0;
        static constexpr int kVictoryStateNo = 1;
        static constexpr int kVictoryStateYes = 2;
        static constexpr int kVictoryRecruitGlobalRefreshIntervalMs = 1000;
        static constexpr int kStaleVictoryNeutralConfirmTicks = 2;

        int g_lastObservedVictoryStateForRecruitGlobals = -1;
        std::chrono::steady_clock::time_point g_nextVictoryRecruitGlobalRefresh{};
        std::chrono::steady_clock::time_point g_nextVictoryPreserveLog{};
        int g_staleVictoryNeutralTicks = 0;

        bool IsBleedoutPleasureLockActive()
        {
            if (TFD::PleasureRuntime::GetSourceContext() != TFD::PleasureRuntime::SourceContext::Bleedout) {
                return false;
            }

            // R121: only the actual OStim scene handoff should preserve the old
            // Defeat global.  Once the scene has ended and the flow is waiting
            // for AfterPleasure dialogue, the Bleedout decision is already
            // terminally resolved.  Keeping TFDDefeatState at 2 during
            // AfterPleasure makes the shared AfterPleasure dialogue behave like
            // it is still inside the defeat/bleedout dialogue phase, which can
            // pass the root greet and then immediately close before player
            // choices settle.
            switch (TFD::PleasureRuntime::GetPhase()) {
            case TFD::PleasureRuntime::Phase::PleasureStartPending:
            case TFD::PleasureRuntime::Phase::PleasureActive:
            case TFD::PleasureRuntime::Phase::PleasureEnding:
                return true;
            default:
                return false;
            }
        }

        bool IsDialogueMenuOpen()
        {
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
        }

        void RefreshRecruitGlobalsWhenVictoryReady(int victoryState)
        {
            if (victoryState != kVictoryStateYes) {
                g_lastObservedVictoryStateForRecruitGlobals = victoryState;
                g_nextVictoryRecruitGlobalRefresh = {};
                return;
            }

            const auto now = std::chrono::steady_clock::now();
            const bool enteredVictoryReady = g_lastObservedVictoryStateForRecruitGlobals != kVictoryStateYes;
            const bool refreshDue =
                g_nextVictoryRecruitGlobalRefresh == std::chrono::steady_clock::time_point{} ||
                now >= g_nextVictoryRecruitGlobalRefresh;

            if (enteredVictoryReady || refreshDue) {
                TFD::TeammateManager::RefreshRecruitCapacityGlobals(
                    enteredVictoryReady ? "victory_state_ready_enter" : "victory_state_ready_hold");
                g_nextVictoryRecruitGlobalRefresh = now + std::chrono::milliseconds(kVictoryRecruitGlobalRefreshIntervalMs);
            }

            g_lastObservedVictoryStateForRecruitGlobals = victoryState;
        }

        bool IsVictoryFlowStillBackedByDefeatedActor(const TFD::FlowController::Snapshot& snapshot)
        {
            if (snapshot.root != TFD::FlowController::RootFlow::Victory) {
                return false;
            }
            if (snapshot.primaryActorFormID == 0) {
                return false;
            }

            auto* actor = RE::TESForm::LookupByID<RE::Actor>(snapshot.primaryActorFormID);
            if (!actor) {
                return false;
            }

            return TFD::Actor::Ops::IsDialogueCapableDefeatedEnemy(actor);
        }

        bool PreserveVictoryFlowIfBackedByDefeatedActor(const char* reason)
        {
            auto& flow = TFD::FlowController::Controller::GetSingleton();
            const auto snapshot = flow.GetSnapshot();
            if (!IsVictoryFlowStillBackedByDefeatedActor(snapshot)) {
                return false;
            }

            const int previousVictoryState = TFD::Victory::GetStateValue();
            TFD::Victory::SetStateValue(kVictoryStateYes);
            RefreshRecruitGlobalsWhenVictoryReady(kVictoryStateYes);
            g_staleVictoryNeutralTicks = 0;

            const auto now = std::chrono::steady_clock::now();
            const bool shouldLog = previousVictoryState != kVictoryStateYes ||
                g_nextVictoryPreserveLog == std::chrono::steady_clock::time_point{} ||
                now >= g_nextVictoryPreserveLog;

            if (shouldLog) {
                spdlog::info(
                    "[TFD][PostDefeatState][R93N] preserving Victory flow root primary={:08X} token={} previousState={} reason={}",
                    snapshot.primaryActorFormID,
                    snapshot.token,
                    previousVictoryState,
                    reason ? reason : "victory_flow_backed_by_defeated_actor");
                g_nextVictoryPreserveLog = now + std::chrono::milliseconds(kVictoryRecruitGlobalRefreshIntervalMs);
            }
            return true;
        }

        void ClearStaleVictoryFlowWhenNeutral(int victoryState, const RefreshInput& input)
        {
            (void)input;

            if (victoryState != kVictoryStateNeutral) {
                g_staleVictoryNeutralTicks = 0;
                return;
            }

            auto& flow = TFD::FlowController::Controller::GetSingleton();
            const auto snapshot = flow.GetSnapshot();
            if (snapshot.root != TFD::FlowController::RootFlow::Victory) {
                g_staleVictoryNeutralTicks = 0;
                return;
            }

            if (IsVictoryFlowStillBackedByDefeatedActor(snapshot)) {
                TFD::Victory::SetStateValue(kVictoryStateYes);
                RefreshRecruitGlobalsWhenVictoryReady(kVictoryStateYes);
                g_staleVictoryNeutralTicks = 0;
                spdlog::info(
                    "[TFD][PostDefeatState] preserved Victory flow root despite neutral global primary={:08X} token={} reason=defeated_actor_still_dialogue_capable",
                    snapshot.primaryActorFormID,
                    snapshot.token);
                return;
            }

            if (snapshot.terminalResolved || IsDialogueMenuOpen()) {
                g_staleVictoryNeutralTicks = 0;
                return;
            }

            ++g_staleVictoryNeutralTicks;
            if (g_staleVictoryNeutralTicks < kStaleVictoryNeutralConfirmTicks) {
                return;
            }

            spdlog::info(
                "[TFD][PostDefeatState] clearing stale Victory flow root because VictoryState is neutral primary={:08X} token={} reason=victory_state_neutral",
                snapshot.primaryActorFormID,
                snapshot.token);
            flow.ResetRuntime("victory_state_neutral_stale_root_clear");
            g_staleVictoryNeutralTicks = 0;
        }
    }

    RefreshResult Refresh(const RefreshInput& input)
    {
        ResolveGlobals();

        RefreshResult result{};
        result.routerCombatContextActive = input.routerCombatContext;

        if (input.onlySuppressedDialogueEnemies) {
            TFD::Victory::ResetObservedContext();
            result.routerCombatContextActive = false;
            spdlog::info(
                "[TFD][PostDefeatState] cleared transient hostile globals because only suppressed dialogue-phase enemies remain count={}",
                input.suppressedEnemyCount);
        }

        if (input.pleasurePassiveLock) {
            result.routerCombatContextActive = false;

            if (IsBleedoutPleasureLockActive()) {
                SetGlobalInt(g_defeatStateGlobal, 2);
                TFD::Victory::ResetObservedContext();
                TFD::Victory::SetStateValue(kVictoryStateNo);
                RefreshRecruitGlobalsWhenVictoryReady(kVictoryStateNo);
                g_staleVictoryNeutralTicks = 0;
                SetGlobalInt(g_hostileStateGlobal, 0);
                SetGlobalInt(g_enemyFactionStateGlobal, 0);
                SetGlobalInt(g_enemyRaceStateGlobal, 0);
                SetGlobalInt(g_recoveryStateGlobal, ComputeRecoveryState());
                SetGlobalInt(g_leftForDeadStateGlobal, 0);
                spdlog::info(
                    "[TFD][PostDefeatState][R108] preserving Bleedout state during pleasure lock phase={} source={} defeat=2 victory=No",
                    TFD::PleasureRuntime::GetPhaseName(),
                    TFD::PleasureRuntime::GetSourceContextName());
                return result;
            }

            SetGlobalInt(g_defeatStateGlobal, 0);
            TFD::Victory::ResetObservedContext();
            if (!PreserveVictoryFlowIfBackedByDefeatedActor("pleasure_passive_lock_ignored_for_active_victory")) {
                TFD::Victory::SetStateValue(0);
                RefreshRecruitGlobalsWhenVictoryReady(kVictoryStateNeutral);
                g_staleVictoryNeutralTicks = 0;
            }
            SetGlobalInt(g_hostileStateGlobal, 0);
            SetGlobalInt(g_enemyFactionStateGlobal, 0);
            SetGlobalInt(g_enemyRaceStateGlobal, 0);
            SetGlobalInt(g_recoveryStateGlobal, ComputeRecoveryState());
            SetGlobalInt(g_leftForDeadStateGlobal, 0);
            return result;
        }

        const bool playerDownNow = ComputePlayerBleedOutState(input.player);
        SetGlobalInt(g_defeatStateGlobal, ComputeDefeatState(input.player, input.defeatContext));

        const bool forceVictoryNoForPlayerDefeat = input.player && playerDownNow && input.defeatContext;
        const bool playerCanOwnVictory = input.player && !playerDownNow;
        const bool preservedActiveVictoryFlow = !forceVictoryNoForPlayerDefeat && playerCanOwnVictory &&
            PreserveVictoryFlowIfBackedByDefeatedActor("active_victory_dialogue_actor_before_observed_refresh");

        if (forceVictoryNoForPlayerDefeat) {
            TFD::Victory::ResetObservedContext();
            TFD::Victory::SetStateValue(kVictoryStateNo);
            RefreshRecruitGlobalsWhenVictoryReady(kVictoryStateNo);
            g_staleVictoryNeutralTicks = 0;
        }
        else if (!preservedActiveVictoryFlow) {
            TFD::Victory::RefreshObservedState(TFD::Victory::ObservedContext{
                .hasPlayer = (input.player != nullptr),
                .playerDown = playerDownNow,
                .combatContext = input.victoryContext,
                .hasEnemies = !input.enemies.empty()
                });
        }

        const int victoryState = TFD::Victory::GetStateValue();
        if (!preservedActiveVictoryFlow && !forceVictoryNoForPlayerDefeat) {
            RefreshRecruitGlobalsWhenVictoryReady(victoryState);
        }
        ClearStaleVictoryFlowWhenNeutral(victoryState, input);
        SetGlobalInt(g_hostileStateGlobal, ComputeHostileState(input.player, input.enemies));
        SetGlobalInt(g_enemyFactionStateGlobal, ComputeEnemyFactionState(input.enemies));
        SetGlobalInt(g_enemyRaceStateGlobal, ComputeEnemyRaceState(input.enemies));
        SetGlobalInt(g_recoveryStateGlobal, ComputeRecoveryState());
        SetGlobalInt(g_leftForDeadStateGlobal, ComputeLeftForDeadState(input.player));
        return result;
    }
}