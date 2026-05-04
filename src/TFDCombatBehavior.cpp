#include "TFDCombatBehavior.h"

#include "TFDActor.h"
#include "TFDFlowController.h"
#include "TFDHostilityController.h"
#include "TFDLocation.h"
#include "TFDSettings.h"
#include "TFDTame.h"
#include "TFDTeammateManager.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace TFD::CombatBehavior
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr auto kWorkerSleep = std::chrono::milliseconds(450);
        constexpr double kPairNudgeCooldownSec = 1.65;
        constexpr double kPairStateKeepSec = 20.0;
        constexpr double kStickyThreatSeconds = 3.0;
        constexpr double kSummaryLogCooldownSec = 2.0;
        constexpr double kOwnershipKeepSec = 10.0;
        constexpr float kInteriorScanRadius = 9000.0f;
        constexpr float kExteriorScanRadius = 12000.0f;
        constexpr float kThreatMaxVerticalDelta = 3000.0f;

        constexpr const char* kCombatAllyFactionEditorIDs[] = {
            "TFDCombatAllyFaction",
            "TFDCombatAlly",
            nullptr
        };
        constexpr const char* kCombatThreatFactionEditorIDs[] = {
            "TFDCombatThreatFaction",
            "TFDCombatThreat",
            nullptr
        };

        std::atomic_bool g_installed{ false };
        std::atomic_bool g_running{ false };
        std::atomic_bool g_tickPending{ false };
        std::thread g_worker;

        std::mutex g_stateLock;
        std::unordered_map<std::uint64_t, double> g_nextPairNudgeSec;
        RE::ActorHandle g_stickyThreat{};
        double g_stickyThreatUntilSec{ 0.0 };
        double g_nextSummaryLogSec{ 0.0 };
        std::uint32_t g_lastPrimaryThreatId{ 0 };

        std::unordered_map<std::uint32_t, double> g_diagnosticAllyUntilSec;
        std::unordered_map<std::uint32_t, double> g_diagnosticThreatUntilSec;
        std::unordered_map<std::uint64_t, double> g_diagnosticPairUntilSec;

        RE::TESFaction* g_combatAllyFaction{ nullptr };
        RE::TESFaction* g_combatThreatFaction{ nullptr };
        bool g_combatFactionLookupDone{ false };
        bool g_combatFactionMissingLogged{ false };

        double NowSec()
        {
            static const auto t0 = Clock::now();
            return std::chrono::duration<double>(Clock::now() - t0).count();
        }

        RE::PlayerCharacter* Player()
        {
            return RE::PlayerCharacter::GetSingleton();
        }

        RE::Actor* CurrentCombatTarget(RE::Actor* actor)
        {
            if (!actor) {
                return nullptr;
            }
            auto sp = actor->GetActorRuntimeData().currentCombatTarget.get();
            return sp.get();
        }

        RE::TESFaction* LookupFactionByEditorIDs(const char* const* editorIDs)
        {
            if (!editorIDs) {
                return nullptr;
            }

            for (auto ids = editorIDs; *ids; ++ids) {
                if (auto* faction = RE::TESForm::LookupByEditorID<RE::TESFaction>(*ids)) {
                    return faction;
                }
            }
            return nullptr;
        }

        bool ResolveCombatFactions(RE::TESFaction*& allyFaction, RE::TESFaction*& threatFaction)
        {
            std::scoped_lock lock(g_stateLock);

            if (!g_combatFactionLookupDone) {
                g_combatAllyFaction = LookupFactionByEditorIDs(kCombatAllyFactionEditorIDs);
                g_combatThreatFaction = LookupFactionByEditorIDs(kCombatThreatFactionEditorIDs);
                g_combatFactionLookupDone = true;

                if (g_combatAllyFaction && g_combatThreatFaction) {
                    spdlog::info(
                        "[TFD][CombatBehavior] temporary combat factions resolved ally={:08X} threat={:08X}",
                        g_combatAllyFaction->GetFormID(),
                        g_combatThreatFaction->GetFormID());
                }
            }

            allyFaction = g_combatAllyFaction;
            threatFaction = g_combatThreatFaction;

            if (!allyFaction || !threatFaction) {
                if (!g_combatFactionMissingLogged) {
                    g_combatFactionMissingLogged = true;
                    spdlog::warn(
                        "[TFD][CombatBehavior] temporary combat factions missing; expected EditorIDs TFDCombatAllyFaction and TFDCombatThreatFaction");
                }
                return false;
            }

            return true;
        }

        bool AddFactionIfMissing(RE::Actor* actor, RE::TESFaction* faction)
        {
            if (!actor || !faction) {
                return false;
            }
            if (actor->IsInFaction(faction)) {
                return false;
            }
            actor->AddToFaction(faction, 0);
            return true;
        }

        bool RemoveFactionIfPresent(RE::Actor* actor, RE::TESFaction* faction)
        {
            if (!actor || !faction) {
                return false;
            }
            if (!actor->IsInFaction(faction)) {
                return false;
            }
            actor->RemoveFromFaction(faction);
            return true;
        }

        void ClearFactionFightReactionCacheIfChanged(bool changed)
        {
            if (!changed) {
                return;
            }
            if (auto* process = RE::ProcessLists::GetSingleton()) {
                process->ClearCachedFactionFightReactions();
            }
        }

        float Distance3D(RE::TESObjectREFR* a, RE::TESObjectREFR* b, float* outDz = nullptr)
        {
            if (outDz) {
                *outDz = 0.0f;
            }
            if (!a || !b) {
                return 0.0f;
            }

            const auto pa = a->GetPosition();
            const auto pb = b->GetPosition();
            const float dx = pa.x - pb.x;
            const float dy = pa.y - pb.y;
            const float dz = pa.z - pb.z;
            if (outDz) {
                *outDz = std::fabs(dz);
            }
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }


        bool IsBleedingOutActor(RE::Actor* actor)
        {
            auto* state = actor ? actor->AsActorState() : nullptr;
            return state && state->IsBleedingOut();
        }

        bool IsNormalCombatAssistContext()
        {
            auto* player = Player();
            if (!player || player->IsDead() || player->IsDisabled() || IsBleedingOutActor(player)) {
                return false;
            }

            if (TFD::FlowController::IsDialogueContextActive() ||
                TFD::FlowController::IsPassiveHoldActive() ||
                TFD::FlowController::IsPleasureLockActive()) {
                return false;
            }

            const auto snapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
            if (snapshot.locked || snapshot.terminalResolved) {
                return false;
            }
            if (snapshot.gate != TFD::FlowController::DecisionGate::None ||
                snapshot.sub != TFD::FlowController::SubFlow::None) {
                return false;
            }

            switch (snapshot.root) {
            case TFD::FlowController::RootFlow::None:
            case TFD::FlowController::RootFlow::InCombat:
                break;
            default:
                return false;
            }

            return true;
        }

        bool IsSoftPackageRepairReason(const char* reason)
        {
            if (!reason || !reason[0]) {
                return false;
            }
            const std::string_view r{ reason };
            return r == "loading_menu_closed" ||
                r == "post_load_humanoid_catchup" ||
                r == "converted_package_repair" ||
                r == "register_now_refresh" ||
                r == "follow_maintenance" ||
                r == "teammate_manager_register_now";
        }

        bool IsStandingActor(RE::Actor* actor)
        {
            if (!actor || actor->IsDead() || actor->IsDisabled()) {
                return false;
            }
            if (actor->GetActorValue(RE::ActorValue::kHealth) <= 0.0f) {
                return false;
            }
            return true;
        }

        bool IsPlayerSideActor(RE::Actor* actor, RE::PlayerCharacter* player)
        {
            if (!actor || !player) {
                return false;
            }
            return actor == player ||
                actor->IsPlayerTeammate() ||
                TFD::TeammateManager::IsActiveFollowerActor(actor) ||
                TFD::Tame::IsCompanion(actor);
        }

        bool IsSuppressedNormalCombatActor(RE::Actor* actor)
        {
            return actor && (TFD::HostilityController::IsSuppressed(actor) || TFD::Actor::Ops::HasReleaseFollowGrace(actor));
        }

        bool IsDownedEnemy(RE::Actor* actor)
        {
            if (!actor) {
                return true;
            }
            if (TFD::Actor::Ops::IsDefeatedEnemyKnocked(actor)) {
                return true;
            }
            return TFD::Actor::IsDownByHealthThreshold(actor, TFD::Settings::GetEnemyDownedThresholdPct());
        }

        bool IsDownedTeammate(RE::Actor* actor)
        {
            if (!actor) {
                return true;
            }
            return TFD::Actor::IsDownByHealthThreshold(actor, TFD::Settings::GetAllyDownedThresholdPct());
        }

        std::vector<RE::ActorHandle> CollectActorHandles()
        {
            std::vector<RE::ActorHandle> out;
            out.reserve(256);

            auto* lists = RE::ProcessLists::GetSingleton();
            if (!lists) {
                return out;
            }

            auto addArray = [&](auto& arr) {
                for (auto& handle : arr) {
                    if (handle) {
                        out.push_back(handle);
                    }
                }
            };

            addArray(lists->highActorHandles);
            addArray(lists->middleHighActorHandles);
            addArray(lists->middleLowActorHandles);
            addArray(lists->lowActorHandles);

            return out;
        }

        bool IsSameLocationScope(RE::Actor* actor, RE::PlayerCharacter* player, float maxRadius)
        {
            if (!actor || !player) {
                return false;
            }

            float dz = 0.0f;
            const float dist = Distance3D(actor, player, &dz);
            if (maxRadius > 0.0f && dist > maxRadius) {
                return false;
            }
            if (dz > kThreatMaxVerticalDelta) {
                return false;
            }

            auto* playerCell = player->GetParentCell();
            auto* actorCell = actor->GetParentCell();
            if (playerCell && actorCell && playerCell == actorCell) {
                return true;
            }

            auto* playerLoc = TFD::Location::GetLocationFromRef(player);
            auto* actorLoc = TFD::Location::GetLocationFromRef(actor);
            if (playerLoc && actorLoc && playerLoc == actorLoc) {
                return true;
            }

            auto* playerWorld = player->GetWorldspace();
            auto* actorWorld = actor->GetWorldspace();
            if (playerWorld && actorWorld && playerWorld == actorWorld) {
                return true;
            }

            return false;
        }

        bool IsValidThreat(RE::Actor* enemy, RE::PlayerCharacter* player, float maxRadius)
        {
            if (!enemy || !player || enemy == player) {
                return false;
            }
            if (!IsStandingActor(enemy) || !enemy->Is3DLoaded()) {
                return false;
            }
            if (IsPlayerSideActor(enemy, player)) {
                return false;
            }
            if (IsSuppressedNormalCombatActor(enemy)) {
                return false;
            }
            if (IsDownedEnemy(enemy)) {
                return false;
            }
            if (!IsSameLocationScope(enemy, player, maxRadius)) {
                return false;
            }
            return true;
        }

        bool IsValidTeammate(RE::Actor* actor, RE::PlayerCharacter* player, float maxRadius)
        {
            if (!actor || !player || actor == player) {
                return false;
            }
            if (!IsStandingActor(actor) || !actor->Is3DLoaded()) {
                return false;
            }
            if (!IsPlayerSideActor(actor, player)) {
                return false;
            }
            if (IsSuppressedNormalCombatActor(actor)) {
                return false;
            }
            if (IsDownedTeammate(actor)) {
                return false;
            }
            if (!IsSameLocationScope(actor, player, maxRadius)) {
                return false;
            }
            return true;
        }

        struct ThreatCandidate
        {
            RE::Actor* actor{ nullptr };
            float score{ -1000000.0f };
            const char* reason{ "none" };
        };

        void RemoveStaleTemporaryCombatFactions(
            const std::vector<RE::ActorHandle>& handles,
            const std::unordered_set<RE::FormID>& keepAllies,
            const std::unordered_set<RE::FormID>& keepThreats)
        {
            RE::TESFaction* allyFaction = nullptr;
            RE::TESFaction* threatFaction = nullptr;
            if (!ResolveCombatFactions(allyFaction, threatFaction)) {
                return;
            }

            bool changed = false;
            unsigned removedAlly = 0;
            unsigned removedThreat = 0;

            for (auto& handle : handles) {
                auto sp = handle.get();
                auto* actor = sp.get();
                if (!actor) {
                    continue;
                }

                const auto id = actor->GetFormID();
                const bool keepAlly = keepAllies.find(id) != keepAllies.end();
                const bool keepThreat = keepThreats.find(id) != keepThreats.end();

                if (!keepAlly && RemoveFactionIfPresent(actor, allyFaction)) {
                    changed = true;
                    ++removedAlly;
                }
                if (!keepThreat && RemoveFactionIfPresent(actor, threatFaction)) {
                    changed = true;
                    ++removedThreat;
                }
            }

            ClearFactionFightReactionCacheIfChanged(changed);

            if (changed) {
                spdlog::info(
                    "[TFD][CombatBehavior] temporary faction cleanup removedAlly={} removedThreat={} keepAllies={} keepThreats={}",
                    removedAlly,
                    removedThreat,
                    keepAllies.size(),
                    keepThreats.size());
            }
        }

        bool ApplyTemporaryCombatFactions(
            const std::vector<RE::ActorHandle>& handles,
            const std::vector<RE::Actor*>& teammates,
            const std::vector<ThreatCandidate>& threats,
            std::unordered_set<RE::FormID>& allyIds,
            std::unordered_set<RE::FormID>& threatIds)
        {
            allyIds.clear();
            threatIds.clear();
            allyIds.reserve(teammates.size() * 2 + 1);
            threatIds.reserve(threats.size() * 2 + 1);

            for (auto* teammate : teammates) {
                if (teammate) {
                    allyIds.insert(teammate->GetFormID());
                }
            }
            for (const auto& threat : threats) {
                if (threat.actor) {
                    threatIds.insert(threat.actor->GetFormID());
                }
            }

            RE::TESFaction* allyFaction = nullptr;
            RE::TESFaction* threatFaction = nullptr;
            const bool factionPairReady = ResolveCombatFactions(allyFaction, threatFaction);
            if (!factionPairReady) {
                return false;
            }

            bool changed = false;
            unsigned addedAlly = 0;
            unsigned addedThreat = 0;

            for (auto* teammate : teammates) {
                if (AddFactionIfMissing(teammate, allyFaction)) {
                    changed = true;
                    ++addedAlly;
                }
                if (RemoveFactionIfPresent(teammate, threatFaction)) {
                    changed = true;
                }
            }

            for (const auto& threat : threats) {
                auto* actor = threat.actor;
                if (AddFactionIfMissing(actor, threatFaction)) {
                    changed = true;
                    ++addedThreat;
                }
                if (RemoveFactionIfPresent(actor, allyFaction)) {
                    changed = true;
                }
            }

            RemoveStaleTemporaryCombatFactions(handles, allyIds, threatIds);
            ClearFactionFightReactionCacheIfChanged(changed);

            if (changed) {
                spdlog::info(
                    "[TFD][CombatBehavior] temporary faction apply allies={} threats={} addedAlly={} addedThreat={}",
                    allyIds.size(),
                    threatIds.size(),
                    addedAlly,
                    addedThreat);
            }

            return true;
        }

        bool ScoreThreat(RE::Actor* enemy, RE::PlayerCharacter* player, float maxRadius, ThreatCandidate& out)
        {
            if (!IsValidThreat(enemy, player, maxRadius)) {
                return false;
            }

            auto* target = CurrentCombatTarget(enemy);
            const bool targetPlayer = target == player;
            const bool targetPlayerSide = target && IsPlayerSideActor(target, player);
            const bool hostileToPlayer = enemy->IsHostileToActor(player);
            const bool playerHostileToEnemy = player->IsHostileToActor(enemy);
            const bool enemyInCombat = enemy->IsInCombat();
            const bool playerInCombat = player->IsInCombat();
            const bool weaponDrawn = enemy->IsWeaponDrawn();

            if (!targetPlayer && !targetPlayerSide && !(hostileToPlayer && (enemyInCombat || playerInCombat || weaponDrawn))) {
                return false;
            }

            float dz = 0.0f;
            const float dist = Distance3D(enemy, player, &dz);
            float score = 0.0f;
            const char* reason = "hostile_alert";

            if (targetPlayer) {
                score += 8000.0f;
                reason = "target_player";
            }
            else if (targetPlayerSide) {
                score += 6500.0f;
                reason = "target_playerside";
            }
            if (hostileToPlayer) {
                score += 2200.0f;
            }
            if (playerHostileToEnemy) {
                score += 1200.0f;
            }
            if (enemyInCombat) {
                score += 1200.0f;
            }
            if (playerInCombat) {
                score += 500.0f;
            }
            if (weaponDrawn) {
                score += 350.0f;
            }

            score -= (dist * 0.35f);
            score -= (dz * 0.10f);

            out.actor = enemy;
            out.score = score;
            out.reason = reason;
            return true;
        }

        std::uint64_t PairKey(RE::FormID teammateId, RE::FormID threatId)
        {
            return (static_cast<std::uint64_t>(teammateId) << 32) | static_cast<std::uint64_t>(threatId);
        }

        std::uint32_t ActorId(RE::Actor* actor)
        {
            return actor ? actor->GetFormID() : 0u;
        }

        void PruneDiagnosticStateUnsafe(double now)
        {
            auto pruneActorMap = [now](std::unordered_map<std::uint32_t, double>& map) {
                for (auto it = map.begin(); it != map.end();) {
                    if (it->second < now) {
                        it = map.erase(it);
                    }
                    else {
                        ++it;
                    }
                }
            };

            pruneActorMap(g_diagnosticAllyUntilSec);
            pruneActorMap(g_diagnosticThreatUntilSec);

            for (auto it = g_diagnosticPairUntilSec.begin(); it != g_diagnosticPairUntilSec.end();) {
                if (it->second < now) {
                    it = g_diagnosticPairUntilSec.erase(it);
                }
                else {
                    ++it;
                }
            }
        }

        void ClearDiagnosticState()
        {
            std::scoped_lock lock(g_stateLock);
            g_diagnosticAllyUntilSec.clear();
            g_diagnosticThreatUntilSec.clear();
            g_diagnosticPairUntilSec.clear();
        }

        void TrackDiagnosticThreat(RE::Actor* threat, double now)
        {
            if (!threat) {
                return;
            }

            std::scoped_lock lock(g_stateLock);
            PruneDiagnosticStateUnsafe(now);
            g_diagnosticThreatUntilSec[ActorId(threat)] = now + kOwnershipKeepSec;
        }

        void TrackDiagnosticPair(RE::Actor* teammate, RE::Actor* threat, double now)
        {
            if (!teammate || !threat) {
                return;
            }

            std::scoped_lock lock(g_stateLock);
            PruneDiagnosticStateUnsafe(now);
            g_diagnosticAllyUntilSec[ActorId(teammate)] = now + kOwnershipKeepSec;
            g_diagnosticThreatUntilSec[ActorId(threat)] = now + kOwnershipKeepSec;
            g_diagnosticPairUntilSec[PairKey(ActorId(teammate), ActorId(threat))] = now + kOwnershipKeepSec;
        }

        void PrunePairCooldowns(double now)
        {
            if (g_nextPairNudgeSec.size() <= 96) {
                return;
            }

            for (auto it = g_nextPairNudgeSec.begin(); it != g_nextPairNudgeSec.end();) {
                if (it->second + kPairStateKeepSec < now) {
                    it = g_nextPairNudgeSec.erase(it);
                }
                else {
                    ++it;
                }
            }
        }

        bool CanNudgePair(RE::Actor* teammate, RE::Actor* threat, double now)
        {
            if (!teammate || !threat) {
                return false;
            }

            std::scoped_lock lock(g_stateLock);
            PrunePairCooldowns(now);

            const auto key = PairKey(teammate->GetFormID(), threat->GetFormID());
            auto it = g_nextPairNudgeSec.find(key);
            if (it != g_nextPairNudgeSec.end() && now < it->second) {
                return false;
            }

            g_nextPairNudgeSec[key] = now + kPairNudgeCooldownSec;
            return true;
        }

        RE::Actor* ResolveStickyThreat(RE::PlayerCharacter* player, float maxRadius, const std::vector<ThreatCandidate>& threats)
        {
            if (!player) {
                return nullptr;
            }

            const double now = NowSec();
            {
                std::scoped_lock lock(g_stateLock);
                if (now <= g_stickyThreatUntilSec) {
                    auto sp = g_stickyThreat.get();
                    auto* sticky = sp.get();
                    if (sticky && std::any_of(threats.begin(), threats.end(), [sticky](const ThreatCandidate& t) { return t.actor == sticky; })) {
                        return sticky;
                    }
                }
            }

            if (threats.empty()) {
                std::scoped_lock lock(g_stateLock);
                g_stickyThreat = RE::ActorHandle{};
                g_stickyThreatUntilSec = 0.0;
                g_lastPrimaryThreatId = 0;
                return nullptr;
            }

            const auto best = std::max_element(threats.begin(), threats.end(), [](const ThreatCandidate& a, const ThreatCandidate& b) {
                return a.score < b.score;
            });
            if (best == threats.end() || !best->actor) {
                return nullptr;
            }

            {
                std::scoped_lock lock(g_stateLock);
                g_stickyThreat = best->actor->GetHandle();
                g_stickyThreatUntilSec = now + kStickyThreatSeconds;
                g_lastPrimaryThreatId = best->actor->GetFormID();
            }

            return best->actor;
        }

        RE::Actor* ChooseThreatForTeammate(
            RE::Actor* teammate,
            RE::PlayerCharacter* player,
            const std::vector<ThreatCandidate>& threats,
            std::unordered_map<RE::FormID, unsigned>& assignedCounts)
        {
            if (!teammate || !player || threats.empty()) {
                return nullptr;
            }

            auto* current = CurrentCombatTarget(teammate);
            if (current) {
                auto it = std::find_if(threats.begin(), threats.end(), [current](const ThreatCandidate& threat) {
                    return threat.actor == current;
                });
                if (it != threats.end()) {
                    return current;
                }
            }

            RE::Actor* bestThreat = nullptr;
            float bestScore = -10000000.0f;

            for (const auto& threat : threats) {
                if (!threat.actor) {
                    continue;
                }

                const auto id = threat.actor->GetFormID();
                const auto assignedIt = assignedCounts.find(id);
                const unsigned assigned = assignedIt != assignedCounts.end() ? assignedIt->second : 0;
                const float distancePenalty = Distance3D(teammate, threat.actor) * 0.22f;
                const float assignmentPenalty = static_cast<float>(assigned) * 900.0f;
                const float candidateScore = threat.score - distancePenalty - assignmentPenalty;

                if (!bestThreat || candidateScore > bestScore) {
                    bestThreat = threat.actor;
                    bestScore = candidateScore;
                }
            }

            return bestThreat;
        }

        void PrimeThreatAgainstPlayer(RE::Actor* threat, RE::PlayerCharacter* player)
        {
            if (!threat || !player) {
                return;
            }

            const double now = NowSec();
            TrackDiagnosticThreat(threat, now);

            auto* target = CurrentCombatTarget(threat);
            auto* playerTargetBefore = CurrentCombatTarget(player);
            const bool targetIsPlayerSide = target && IsPlayerSideActor(target, player);
            const bool targetIsInvalidPlayerSide = targetIsPlayerSide &&
                target != player &&
                (!IsStandingActor(target) || IsDownedTeammate(target));

            if (!target || targetIsInvalidPlayerSide || (target != player && !targetIsPlayerSide)) {
                threat->GetActorRuntimeData().currentCombatTarget = player->GetHandle();
            }

            if (!player->IsInCombat() || CurrentCombatTarget(player) == nullptr) {
                player->GetActorRuntimeData().currentCombatTarget = threat->GetHandle();
            }

            auto* threatTargetAfterSet = CurrentCombatTarget(threat);
            auto* playerTargetAfterSet = CurrentCombatTarget(player);

            threat->SetBeenAttacked(true);
            player->SetBeenAttacked(true);
            (void)threat->RequestDetectionLevel(player, RE::DETECTION_PRIORITY::kCritical);
            (void)player->RequestDetectionLevel(threat, RE::DETECTION_PRIORITY::kCritical);

            threat->UpdateCombat();
            auto* threatTargetAfterThreatUpdate = CurrentCombatTarget(threat);
            auto* playerTargetAfterThreatUpdate = CurrentCombatTarget(player);

            player->UpdateCombat();
            auto* threatTargetFinal = CurrentCombatTarget(threat);
            auto* playerTargetFinal = CurrentCombatTarget(player);

            spdlog::info(
                "[TFD][CombatDiag] prime_threat threat={:08X} before={:08X} afterSet={:08X} afterThreatUpdate={:08X} final={:08X} playerBefore={:08X} playerAfterSet={:08X} playerAfterThreatUpdate={:08X} playerFinal={:08X} threatInCombat={} playerInCombat={} targetWasPlayerSide={} targetWasInvalidPlayerSide={}",
                threat->GetFormID(),
                target ? target->GetFormID() : 0u,
                threatTargetAfterSet ? threatTargetAfterSet->GetFormID() : 0u,
                threatTargetAfterThreatUpdate ? threatTargetAfterThreatUpdate->GetFormID() : 0u,
                threatTargetFinal ? threatTargetFinal->GetFormID() : 0u,
                playerTargetBefore ? playerTargetBefore->GetFormID() : 0u,
                playerTargetAfterSet ? playerTargetAfterSet->GetFormID() : 0u,
                playerTargetAfterThreatUpdate ? playerTargetAfterThreatUpdate->GetFormID() : 0u,
                playerTargetFinal ? playerTargetFinal->GetFormID() : 0u,
                threat->IsInCombat() ? 1 : 0,
                player->IsInCombat() ? 1 : 0,
                targetIsPlayerSide ? 1 : 0,
                targetIsInvalidPlayerSide ? 1 : 0);
        }

        bool NudgeTeammateToThreat(RE::Actor* teammate, RE::Actor* threat, RE::PlayerCharacter* player, const char* reason, double now)
        {
            if (!IsValidTeammate(teammate, player, kExteriorScanRadius) || !IsValidThreat(threat, player, kExteriorScanRadius)) {
                return false;
            }
            if (teammate == threat) {
                return false;
            }

            auto* current = CurrentCombatTarget(teammate);
            if (current == threat && teammate->IsInCombat()) {
                return false;
            }
            if (current && current != threat && IsValidThreat(current, player, kExteriorScanRadius)) {
                return false;
            }
            if (!CanNudgePair(teammate, threat, now)) {
                return false;
            }

            if (!teammate->IsAIEnabled()) {
                teammate->EnableAI(true);
            }

            TrackDiagnosticPair(teammate, threat, now);

            const auto* teammateTargetBefore = CurrentCombatTarget(teammate);
            const auto* threatTargetBefore = CurrentCombatTarget(threat);
            const auto* playerTargetBefore = CurrentCombatTarget(player);
            const bool teammateInCombatBefore = teammate->IsInCombat();
            const bool threatInCombatBefore = threat->IsInCombat();
            const bool playerInCombatBefore = player->IsInCombat();
            const bool allyHostileBefore = teammate->IsHostileToActor(threat);
            const bool threatHostileBefore = threat->IsHostileToActor(teammate);

            teammate->GetActorRuntimeData().currentCombatTarget = threat->GetHandle();
            const auto* teammateTargetAfterSet = CurrentCombatTarget(teammate);

            spdlog::info(
                "[TFD][CombatDiag] nudge_begin teammate={:08X} threat={:08X} reason={} before={:08X} afterSet={:08X} threatBefore={:08X} playerBefore={:08X} teammateCombatBefore={} threatCombatBefore={} playerCombatBefore={} allyHostileBefore={} threatHostileBefore={} weaponDrawn={}",
                teammate->GetFormID(),
                threat->GetFormID(),
                reason ? reason : "threat",
                teammateTargetBefore ? teammateTargetBefore->GetFormID() : 0u,
                teammateTargetAfterSet ? teammateTargetAfterSet->GetFormID() : 0u,
                threatTargetBefore ? threatTargetBefore->GetFormID() : 0u,
                playerTargetBefore ? playerTargetBefore->GetFormID() : 0u,
                teammateInCombatBefore ? 1 : 0,
                threatInCombatBefore ? 1 : 0,
                playerInCombatBefore ? 1 : 0,
                allyHostileBefore ? 1 : 0,
                threatHostileBefore ? 1 : 0,
                teammate->IsWeaponDrawn() ? 1 : 0);

            teammate->SetBeenAttacked(true);
            threat->SetBeenAttacked(true);
            player->SetBeenAttacked(true);

            (void)teammate->RequestDetectionLevel(threat, RE::DETECTION_PRIORITY::kCritical);
            (void)threat->RequestDetectionLevel(teammate, RE::DETECTION_PRIORITY::kCritical);
            (void)teammate->RequestDetectionLevel(player, RE::DETECTION_PRIORITY::kCritical);
            (void)player->RequestDetectionLevel(threat, RE::DETECTION_PRIORITY::kCritical);

            teammate->UpdateCombat();
            const auto* teammateTargetAfterTeammateUpdate = CurrentCombatTarget(teammate);
            const bool teammateInCombatAfterTeammateUpdate = teammate->IsInCombat();

            threat->UpdateCombat();
            const auto* teammateTargetAfterThreatUpdate = CurrentCombatTarget(teammate);
            const auto* threatTargetAfterThreatUpdate = CurrentCombatTarget(threat);
            const bool threatInCombatAfterThreatUpdate = threat->IsInCombat();

            player->UpdateCombat();

            const auto* after = CurrentCombatTarget(teammate);
            const auto* threatTarget = CurrentCombatTarget(threat);
            const auto* playerTarget = CurrentCombatTarget(player);
            const bool allyHostileAfter = teammate->IsHostileToActor(threat);
            const bool threatHostileAfter = threat->IsHostileToActor(teammate);

            spdlog::info(
                "[TFD][CombatDiag] nudge_phases teammate={:08X} threat={:08X} reason={} before={:08X} afterSet={:08X} afterTeammateUpdate={:08X} afterThreatUpdate={:08X} final={:08X} threatBefore={:08X} threatAfterThreatUpdate={:08X} threatFinal={:08X} playerBefore={:08X} playerFinal={:08X} teammateCombatBefore={} teammateCombatAfterTeammateUpdate={} teammateCombatFinal={} threatCombatBefore={} threatCombatAfterThreatUpdate={} threatCombatFinal={} playerCombatBefore={} playerCombatFinal={} allyHostileBefore={} allyHostileAfter={} threatHostileBefore={} threatHostileAfter={}",
                teammate->GetFormID(),
                threat->GetFormID(),
                reason ? reason : "threat",
                teammateTargetBefore ? teammateTargetBefore->GetFormID() : 0u,
                teammateTargetAfterSet ? teammateTargetAfterSet->GetFormID() : 0u,
                teammateTargetAfterTeammateUpdate ? teammateTargetAfterTeammateUpdate->GetFormID() : 0u,
                teammateTargetAfterThreatUpdate ? teammateTargetAfterThreatUpdate->GetFormID() : 0u,
                after ? after->GetFormID() : 0u,
                threatTargetBefore ? threatTargetBefore->GetFormID() : 0u,
                threatTargetAfterThreatUpdate ? threatTargetAfterThreatUpdate->GetFormID() : 0u,
                threatTarget ? threatTarget->GetFormID() : 0u,
                playerTargetBefore ? playerTargetBefore->GetFormID() : 0u,
                playerTarget ? playerTarget->GetFormID() : 0u,
                teammateInCombatBefore ? 1 : 0,
                teammateInCombatAfterTeammateUpdate ? 1 : 0,
                teammate->IsInCombat() ? 1 : 0,
                threatInCombatBefore ? 1 : 0,
                threatInCombatAfterThreatUpdate ? 1 : 0,
                threat->IsInCombat() ? 1 : 0,
                playerInCombatBefore ? 1 : 0,
                player->IsInCombat() ? 1 : 0,
                allyHostileBefore ? 1 : 0,
                allyHostileAfter ? 1 : 0,
                threatHostileBefore ? 1 : 0,
                threatHostileAfter ? 1 : 0);

            spdlog::info(
                "[TFD][CombatBehavior] engage teammate={:08X} threat={:08X} reason={} teammateInCombat={} targetAfter={:08X} threatTarget={:08X} allyHostile={} threatHostile={} allyHostileBefore={} threatHostileBefore={}",
                teammate->GetFormID(),
                threat->GetFormID(),
                reason ? reason : "threat",
                teammate->IsInCombat() ? 1 : 0,
                after ? after->GetFormID() : 0u,
                threatTarget ? threatTarget->GetFormID() : 0u,
                allyHostileAfter ? 1 : 0,
                threatHostileAfter ? 1 : 0,
                allyHostileBefore ? 1 : 0,
                threatHostileBefore ? 1 : 0);

            return true;
        }

        void TickGameThread()
        {
            g_tickPending.store(false, std::memory_order_release);

            auto* player = Player();
            if (!player || player->IsDead() || player->IsDisabled()) {
                ClearDiagnosticState();
                return;
            }

            const bool exterior = player->GetWorldspace() != nullptr;
            const float scanRadius = exterior ? kExteriorScanRadius : kInteriorScanRadius;
            const double now = NowSec();

            auto handles = CollectActorHandles();

            if (IsBleedingOutActor(player) || !IsNormalCombatAssistContext()) {
                ClearDiagnosticState();
                RemoveStaleTemporaryCombatFactions(handles, {}, {});
                return;
            }

            std::unordered_set<RE::FormID> seen;
            seen.reserve(handles.size() * 2);

            std::vector<ThreatCandidate> threats;
            std::vector<RE::Actor*> teammates;
            threats.reserve(64);
            teammates.reserve(16);

            for (auto& handle : handles) {
                auto sp = handle.get();
                auto* actor = sp.get();
                if (!actor || actor == player) {
                    continue;
                }

                const auto id = actor->GetFormID();
                if (id == 0 || !seen.insert(id).second) {
                    continue;
                }

                if (IsValidTeammate(actor, player, scanRadius)) {
                    teammates.push_back(actor);
                    continue;
                }

                ThreatCandidate threat{};
                if (ScoreThreat(actor, player, scanRadius, threat)) {
                    threats.push_back(threat);
                }
            }

            if (threats.empty() || teammates.empty()) {
                ClearDiagnosticState();
                RemoveStaleTemporaryCombatFactions(handles, {}, {});
                return;
            }

            std::sort(threats.begin(), threats.end(), [](const ThreatCandidate& a, const ThreatCandidate& b) {
                return a.score > b.score;
            });

            std::unordered_set<RE::FormID> allyIds;
            std::unordered_set<RE::FormID> threatIds;
            const bool factionPairReady = ApplyTemporaryCombatFactions(handles, teammates, threats, allyIds, threatIds);

            auto* primaryThreat = ResolveStickyThreat(player, scanRadius, threats);
            if (!primaryThreat) {
                RemoveStaleTemporaryCombatFactions(handles, allyIds, threatIds);
                return;
            }

            const auto reasonIt = std::find_if(threats.begin(), threats.end(), [primaryThreat](const ThreatCandidate& t) {
                return t.actor == primaryThreat;
            });
            const char* reason = reasonIt != threats.end() ? reasonIt->reason : "threat";

            PrimeThreatAgainstPlayer(primaryThreat, player);

            unsigned nudged = 0;
            std::unordered_map<RE::FormID, unsigned> assignedCounts;
            assignedCounts.reserve(threats.size() * 2 + 1);

            for (auto* teammate : teammates) {
                auto* assignedThreat = ChooseThreatForTeammate(teammate, player, threats, assignedCounts);
                if (!assignedThreat) {
                    continue;
                }

                const auto threatId = assignedThreat->GetFormID();
                if (NudgeTeammateToThreat(teammate, assignedThreat, player, reason, now)) {
                    ++nudged;
                }
                ++assignedCounts[threatId];
            }

            bool logSummary = false;
            {
                std::scoped_lock lock(g_stateLock);
                if (now >= g_nextSummaryLogSec || g_lastPrimaryThreatId != primaryThreat->GetFormID() || nudged > 0) {
                    g_nextSummaryLogSec = now + kSummaryLogCooldownSec;
                    g_lastPrimaryThreatId = primaryThreat->GetFormID();
                    logSummary = true;
                }
            }

            if (logSummary) {
                spdlog::info(
                    "[TFD][CombatBehavior] summary threat={:08X} reason={} threats={} teammates={} nudged={} factionPair={} playerInCombat={} playerTarget={:08X}",
                    primaryThreat->GetFormID(),
                    reason ? reason : "threat",
                    threats.size(),
                    teammates.size(),
                    nudged,
                    factionPairReady ? 1 : 0,
                    player->IsInCombat() ? 1 : 0,
                    CurrentCombatTarget(player) ? CurrentCombatTarget(player)->GetFormID() : 0u);
            }
        }

        void QueueTick()
        {
            if (g_tickPending.exchange(true, std::memory_order_acq_rel)) {
                return;
            }

            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([]() {
                    try {
                        TickGameThread();
                    }
                    catch (const std::exception& e) {
                        g_tickPending.store(false, std::memory_order_release);
                        spdlog::error("[TFD][CombatBehavior] tick exception: {}", e.what());
                    }
                    catch (...) {
                        g_tickPending.store(false, std::memory_order_release);
                        spdlog::error("[TFD][CombatBehavior] tick exception: unknown");
                    }
                });
            }
            else {
                g_tickPending.store(false, std::memory_order_release);
            }
        }

        void WorkerLoop()
        {
            while (g_running.load(std::memory_order_acquire)) {
                QueueTick();
                std::this_thread::sleep_for(kWorkerSleep);
            }
        }
    }

    void Install()
    {
        if (g_installed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        if (auto* msg = SKSE::GetMessagingInterface()) {
            msg->RegisterListener([](SKSE::MessagingInterface::Message* message) {
                if (!message) {
                    return;
                }
                if (message->type == SKSE::MessagingInterface::kPreLoadGame ||
                    message->type == SKSE::MessagingInterface::kPostLoadGame) {
                    ResetForLoad();
                }
            });
        }

        g_running.store(true, std::memory_order_release);
        g_tickPending.store(false, std::memory_order_release);
        g_worker = std::thread([]() { WorkerLoop(); });
        // The game process owns plugin lifetime; avoid a joinable std::thread destructor during shutdown.
        g_worker.detach();

        spdlog::info("[TFD][CombatBehavior] installed R73 ownership guard native scanner with temporary combat faction pair");
    }

    void Shutdown()
    {
        if (!g_installed.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        g_running.store(false, std::memory_order_release);
        if (g_worker.joinable()) {
            g_worker.join();
        }
        g_tickPending.store(false, std::memory_order_release);
        ResetForLoad();
        spdlog::info("[TFD][CombatBehavior] shutdown");
    }

    void ResetForLoad()
    {
        std::scoped_lock lock(g_stateLock);
        g_nextPairNudgeSec.clear();
        g_stickyThreat = RE::ActorHandle{};
        g_stickyThreatUntilSec = 0.0;
        g_nextSummaryLogSec = 0.0;
        g_lastPrimaryThreatId = 0;
        g_diagnosticAllyUntilSec.clear();
        g_diagnosticThreatUntilSec.clear();
        g_diagnosticPairUntilSec.clear();
        spdlog::info("[TFD][CombatBehavior] reset runtime state R73 ownership guard");
    }
    bool IsNormalCombatAssistActive()
    {
        if (!IsNormalCombatAssistContext()) {
            return false;
        }

        const double now = NowSec();
        std::scoped_lock lock(g_stateLock);
        PruneDiagnosticStateUnsafe(now);
        return !g_diagnosticAllyUntilSec.empty() && !g_diagnosticThreatUntilSec.empty();
    }

    bool IsActiveCombatAlly(RE::Actor* actor)
    {
        if (!actor || !IsNormalCombatAssistContext()) {
            return false;
        }

        const double now = NowSec();
        std::scoped_lock lock(g_stateLock);
        PruneDiagnosticStateUnsafe(now);
        const auto it = g_diagnosticAllyUntilSec.find(ActorId(actor));
        return it != g_diagnosticAllyUntilSec.end() && it->second >= now;
    }

    bool IsActiveCombatThreat(RE::Actor* actor)
    {
        if (!actor || !IsNormalCombatAssistContext()) {
            return false;
        }

        const double now = NowSec();
        std::scoped_lock lock(g_stateLock);
        PruneDiagnosticStateUnsafe(now);
        const auto it = g_diagnosticThreatUntilSec.find(ActorId(actor));
        return it != g_diagnosticThreatUntilSec.end() && it->second >= now;
    }

    bool IsActiveCombatPair(RE::Actor* actor, RE::Actor* target)
    {
        if (!actor || !target || !IsNormalCombatAssistContext()) {
            return false;
        }

        const double now = NowSec();
        std::scoped_lock lock(g_stateLock);
        PruneDiagnosticStateUnsafe(now);
        const auto it = g_diagnosticPairUntilSec.find(PairKey(ActorId(actor), ActorId(target)));
        if (it != g_diagnosticPairUntilSec.end() && it->second >= now) {
            return true;
        }

        const auto allyIt = g_diagnosticAllyUntilSec.find(ActorId(actor));
        const auto threatIt = g_diagnosticThreatUntilSec.find(ActorId(target));
        return allyIt != g_diagnosticAllyUntilSec.end() && allyIt->second >= now &&
            threatIt != g_diagnosticThreatUntilSec.end() && threatIt->second >= now;
    }

    bool ShouldPreserveCombatTarget(RE::Actor* actor, RE::Actor* target)
    {
        auto* player = Player();
        if (!actor || !target || !player || actor == target || actor == player || target == player) {
            return false;
        }
        if (!IsNormalCombatAssistContext()) {
            return false;
        }
        if (!IsPlayerSideActor(actor, player) || IsPlayerSideActor(target, player)) {
            return false;
        }
        if (!IsStandingActor(actor) || !IsStandingActor(target) || IsBleedingOutActor(actor) || IsBleedingOutActor(target)) {
            return false;
        }
        if (IsSuppressedNormalCombatActor(actor) || IsSuppressedNormalCombatActor(target)) {
            return false;
        }
        if (IsDownedTeammate(actor) || IsDownedEnemy(target)) {
            return false;
        }

        const double now = NowSec();
        std::scoped_lock lock(g_stateLock);
        PruneDiagnosticStateUnsafe(now);

        const auto actorId = ActorId(actor);
        const auto targetId = ActorId(target);
        const auto pairIt = g_diagnosticPairUntilSec.find(PairKey(actorId, targetId));
        if (pairIt != g_diagnosticPairUntilSec.end() && pairIt->second >= now) {
            return true;
        }

        const auto allyIt = g_diagnosticAllyUntilSec.find(actorId);
        const auto threatIt = g_diagnosticThreatUntilSec.find(targetId);
        return allyIt != g_diagnosticAllyUntilSec.end() && allyIt->second >= now &&
            threatIt != g_diagnosticThreatUntilSec.end() && threatIt->second >= now;
    }

    bool ShouldSuppressTeammatePackageRepair(RE::Actor* actor, const char* reason)
    {
        auto* player = Player();
        if (!actor || !player || !IsSoftPackageRepairReason(reason)) {
            return false;
        }
        if (!IsNormalCombatAssistContext()) {
            return false;
        }
        if (!IsPlayerSideActor(actor, player) || !IsStandingActor(actor) || IsBleedingOutActor(actor)) {
            return false;
        }

        const double now = NowSec();
        std::scoped_lock lock(g_stateLock);
        PruneDiagnosticStateUnsafe(now);
        const auto allyIt = g_diagnosticAllyUntilSec.find(ActorId(actor));
        if (allyIt == g_diagnosticAllyUntilSec.end() || allyIt->second < now) {
            return false;
        }

        auto* target = CurrentCombatTarget(actor);
        if (!target) {
            return !g_diagnosticThreatUntilSec.empty();
        }
        const auto pairIt = g_diagnosticPairUntilSec.find(PairKey(ActorId(actor), ActorId(target)));
        if (pairIt != g_diagnosticPairUntilSec.end() && pairIt->second >= now) {
            return true;
        }
        const auto threatIt = g_diagnosticThreatUntilSec.find(ActorId(target));
        return threatIt != g_diagnosticThreatUntilSec.end() && threatIt->second >= now;
    }

    bool IsDiagnosticActor(RE::Actor* actor)
    {
        if (!actor) {
            return false;
        }

        const double now = NowSec();
        std::scoped_lock lock(g_stateLock);
        const auto id = ActorId(actor);
        const auto allyIt = g_diagnosticAllyUntilSec.find(id);
        if (allyIt != g_diagnosticAllyUntilSec.end() && allyIt->second >= now) {
            return true;
        }
        const auto threatIt = g_diagnosticThreatUntilSec.find(id);
        return threatIt != g_diagnosticThreatUntilSec.end() && threatIt->second >= now;
    }

    bool IsDiagnosticPair(RE::Actor* actor, RE::Actor* target)
    {
        if (!actor || !target) {
            return false;
        }

        const double now = NowSec();
        std::scoped_lock lock(g_stateLock);
        const auto pairIt = g_diagnosticPairUntilSec.find(PairKey(ActorId(actor), ActorId(target)));
        return pairIt != g_diagnosticPairUntilSec.end() && pairIt->second >= now;
    }

    const char* DiagnosticRole(RE::Actor* actor)
    {
        if (!actor) {
            return "none";
        }

        const double now = NowSec();
        std::scoped_lock lock(g_stateLock);
        const auto id = ActorId(actor);
        const auto allyIt = g_diagnosticAllyUntilSec.find(id);
        const bool ally = allyIt != g_diagnosticAllyUntilSec.end() && allyIt->second >= now;
        const auto threatIt = g_diagnosticThreatUntilSec.find(id);
        const bool threat = threatIt != g_diagnosticThreatUntilSec.end() && threatIt->second >= now;

        if (ally && threat) {
            return "ally_threat";
        }
        if (ally) {
            return "ally";
        }
        if (threat) {
            return "threat";
        }
        return "none";
    }

}
