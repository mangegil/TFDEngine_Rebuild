#include "TFDVictory.h"

#include "TFDActor.h"
#include "TFDSettings.h"
#include "TFDTeammateManager.h"
#include "TFDTame.h"

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
        std::chrono::steady_clock::time_point g_dialogueReadyHoldUntil{};
        RE::FormID g_dialogueReadyHoldActorFormID = 0;

        static constexpr int kStateNeutral = 0;
        static constexpr int kStateNo = 1;
        static constexpr int kStateYes = 2;
        static constexpr int kObservedCombatContextLingerMs = 2500;
        static constexpr float kDefeatedDialogueScanRadius = 512.0f;
        static constexpr float kVictoryObservedScanRadius = 2400.0f;
        static constexpr float kActorLevelDefeatedScanRadius = 4200.0f;

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

        bool HasKeywordByEditorID(RE::Actor* actor, const char* editorID)
        {
            if (!actor || !editorID || !editorID[0]) {
                return false;
            }
            auto* keyword = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(editorID);
            return keyword && actor->HasKeyword(keyword);
        }

        bool IsHumanoidVictoryActor(RE::Actor* actor)
        {
            return HasKeywordByEditorID(actor, "ActorTypeNPC");
        }

        bool IsBasicLivingVictoryActor(RE::Actor* actor, RE::Actor* player)
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
            if (actor->IsPlayerTeammate() || TFD::TeammateManager::IsActiveFollowerActor(actor) || TFD::TeammateManager::IsPlayerSideTeammateActor(actor)) {
                return false;
            }
            if (TFD::Tame::IsCompanion(actor) || TFD::Tame::HasActiveSession(actor)) {
                return false;
            }
            return true;
        }

        bool IsRelevantLivingEnemyActor(RE::Actor* actor, RE::Actor* player)
        {
            if (!IsBasicLivingVictoryActor(actor, player)) {
                return false;
            }
            if (TFD::Actor::Ops::IsDialogueCapableDefeatedEnemy(actor)) {
                return true;
            }
            if (TFD::Actor::Ops::IsDefeatedEnemyCandidate(actor)) {
                return true;
            }
            if (actor->IsHostileToActor(player) || actor->IsInCombat()) {
                return true;
            }

            auto targetSp = actor->GetActorRuntimeData().currentCombatTarget.get();
            auto* currentTarget = targetSp.get();
            if (currentTarget == player) {
                return true;
            }
            if (currentTarget && TFD::TeammateManager::IsActiveFollowerActor(currentTarget)) {
                return true;
            }
            return false;
        }

        bool IsUsableDefeatedDialogueActor(RE::Actor* actor, RE::Actor* player)
        {
            if (!IsBasicLivingVictoryActor(actor, player)) {
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

        bool IsThresholdDefeatedVictoryActor(RE::Actor* actor, RE::Actor* player, bool allowContextFallback)
        {
            if (!IsBasicLivingVictoryActor(actor, player)) {
                return false;
            }
            if (!IsHumanoidVictoryActor(actor)) {
                return false;
            }
            if (!TFD::Actor::IsDownByHealthThreshold(actor, TFD::Settings::GetEnemyDownedThresholdPct())) {
                return false;
            }
            return allowContextFallback || IsRelevantLivingEnemyActor(actor, player);
        }


        bool HasActorLevelDefeatedLivingVictoryActor(float radius)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return false;
            }

            const float scanRadius = (std::max)(radius, kActorLevelDefeatedScanRadius);
            const auto snapshot = TFD::Actor::BuildSnapshot(scanRadius, false);

            for (const auto& info : snapshot.actors) {
                auto* actor = info.get();
                if (!actor || info.dist > scanRadius) {
                    continue;
                }
                if (!IsBasicLivingVictoryActor(actor, player)) {
                    continue;
                }
                if (TFD::Actor::Ops::IsDefeatedEnemyKnocked(actor)) {
                    return true;
                }
            }

            return false;
        }

        bool HasThresholdDefeatedVictoryActor(float radius, bool allowContextFallback)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return false;
            }

            const float scanRadius = (std::max)(radius, kVictoryObservedScanRadius);
            const auto snapshot = TFD::Actor::BuildSnapshot(scanRadius, false);

            for (const auto& info : snapshot.actors) {
                auto* actor = info.get();
                if (!actor || info.dist > scanRadius) {
                    continue;
                }
                if (IsThresholdDefeatedVictoryActor(actor, player, allowContextFallback)) {
                    return true;
                }
            }

            return false;
        }

        bool IsDialogueReadyHoldActive()
        {
            const auto now = Now();
            if (g_dialogueReadyHoldActorFormID == 0 || now >= g_dialogueReadyHoldUntil) {
                g_dialogueReadyHoldActorFormID = 0;
                g_dialogueReadyHoldUntil = {};
                return false;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(g_dialogueReadyHoldActorFormID);
            if (!IsUsableDefeatedDialogueActor(actor, player)) {
                spdlog::info(
                    "[TFD][Victory] dialogue ready hold cleared actor={:08X} reason=actor_not_usable",
                    g_dialogueReadyHoldActorFormID);
                g_dialogueReadyHoldActorFormID = 0;
                g_dialogueReadyHoldUntil = {};
                return false;
            }

            return true;
        }

        bool HasRelevantLivingEnemyActor(float radius)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return false;
            }

            const float scanRadius = (std::max)(radius, kVictoryObservedScanRadius);
            const auto snapshot = TFD::Actor::BuildSnapshot(scanRadius, false);

            for (const auto& info : snapshot.actors) {
                auto* actor = info.get();
                if (!actor || info.dist > scanRadius) {
                    continue;
                }
                if (IsRelevantLivingEnemyActor(actor, player)) {
                    return true;
                }
            }

            return false;
        }
    }

    void SetStateValue(int value)
    {
        ResolveGlobal();
        if (!g_stateGlobal) {
            return;
        }

        if (value < kStateNeutral || value > kStateYes) {
            spdlog::warn("[TFD][Victory] rejected invalid state value={}", value);
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
        return g_stateGlobal ? static_cast<int>(std::lround(g_stateGlobal->value)) : kStateNeutral;
    }

    bool IsActive()
    {
        return GetStateValue() != kStateNeutral;
    }

    void ResetObservedContext()
    {
        g_observedCombatContextUntil = {};
    }

    void ArmDialogueReadyHold(RE::Actor* actor, double seconds, const char* reason)
    {
        if (!actor || seconds <= 0.0) {
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!IsUsableDefeatedDialogueActor(actor, player)) {
            return;
        }

        const auto duration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
        g_dialogueReadyHoldActorFormID = actor->GetFormID();
        g_dialogueReadyHoldUntil = Now() + duration;

        spdlog::info(
            "[TFD][Victory] dialogue ready hold armed actor={:08X} seconds={:.2f} reason={}",
            actor->GetFormID(),
            seconds,
            reason ? reason : "unknown");
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
        SetStateValue(kStateYes);
    }

    int ComputeObservedState(const ObservedContext& context)
    {
        // 0 = Neutral: no living enemy remains relevant to the player.
        // 1 = No: a live/standing enemy is still active, so Victory is not available yet.
        // 2 = Yes: combat has resolved and at least one living humanoid enemy is downed below threshold.
        if (!context.hasPlayer) {
            ResetObservedContext();
            return kStateNeutral;
        }

        if (context.playerDown) {
            ResetObservedContext();
            return kStateNeutral;
        }

        const auto now = Now();
        const int previousState = GetStateValue();
        const bool lingerActive =
            g_observedCombatContextUntil != std::chrono::steady_clock::time_point{} &&
            now < g_observedCombatContextUntil;

        if (IsDialogueReadyHoldActive()) {
            return kStateYes;
        }

        if (context.hasEnemies && context.combatContext) {
            g_observedCombatContextUntil = now + std::chrono::milliseconds(kObservedCombatContextLingerMs);
            return kStateNo;
        }

        // Standing observed enemies without combat proof are precombat/threat data,
        // not a committed combat-resolution context.  Do not advertise VictoryState=1
        // just because an actor can see or warn the player.
        const bool activeVictoryContext = context.combatContext || lingerActive || previousState == kStateYes;

        if (HasDialogueCapableDefeatedEnemy(kDefeatedDialogueScanRadius)) {
            return kStateYes;
        }

        // R93M: Actor-level defeated locks are the authoritative Victory truth.
        // Enemy downed by a teammate after PreCombat may no longer look hostile or
        // have an active combat context, but it is still a living defeated enemy.
        if (HasActorLevelDefeatedLivingVictoryActor(kActorLevelDefeatedScanRadius)) {
            return kStateYes;
        }

        if (HasThresholdDefeatedVictoryActor(kVictoryObservedScanRadius, activeVictoryContext)) {
            return kStateYes;
        }

        if (context.combatContext && HasRelevantLivingEnemyActor(kVictoryObservedScanRadius)) {
            g_observedCombatContextUntil = now + std::chrono::milliseconds(kObservedCombatContextLingerMs);
            return kStateNo;
        }

        ResetObservedContext();
        return kStateNeutral;
    }

    void RefreshObservedState(const ObservedContext& context)
    {
        SetStateValue(ComputeObservedState(context));
    }
}
