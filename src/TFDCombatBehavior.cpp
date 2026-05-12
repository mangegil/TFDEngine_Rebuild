#include "TFDCombatBehavior.h"

#include "TFDActor.h"
#include "TFDBleedout.h"
#include "TFDDefeatMonitor.h"
#include "TFDFlowController.h"
#include "TFDHostilityController.h"
#include "TFDPleasureRuntime.h"
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
#include <mutex>
#include <limits>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace TFD::CombatBehavior
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        enum class RuntimeRole : std::uint8_t
        {
            None = 0,
            BleedoutAlly,
            BleedoutThreat
        };

        struct PendingTarget
        {
            RE::ActorHandle target{};
            RuntimeRole role{ RuntimeRole::None };
            Clock::time_point until{};
        };

        std::atomic_bool g_installed{ false };
        std::atomic_bool g_running{ false };
        std::atomic_bool g_bleedoutRetargetActive{ false };
        std::atomic_flag g_tickPending = ATOMIC_FLAG_INIT;
        std::thread g_worker{};
        std::mutex g_runtimeLock{};
        std::unordered_map<std::uint32_t, PendingTarget> g_pendingTargets{};
        std::unordered_map<std::uint32_t, RuntimeRole> g_roles{};
        std::unordered_map<std::uint32_t, Clock::time_point> g_lastCommitDispatch{};
        std::unordered_map<std::uint32_t, RE::ActorHandle> g_settleCandidates{};
        std::unordered_map<std::uint32_t, Clock::time_point> g_lastSettleDispatch{};

        constexpr auto kWorkerInterval = std::chrono::milliseconds(300);
        constexpr auto kPendingTargetTtl = std::chrono::milliseconds(2200);
        constexpr auto kCommitDispatchInterval = std::chrono::milliseconds(650);
        constexpr auto kSettleDispatchInterval = std::chrono::milliseconds(1600);
        constexpr float kMinRetargetRadius = 2400.0f;
        constexpr float kRetargetRadiusPadding = 600.0f;
        constexpr float kLocalThreatRadius = 3600.0f;
        constexpr float kTargetEngageRadius = 5200.0f;

        RE::Actor* Player()
        {
            return RE::PlayerCharacter::GetSingleton();
        }

        std::uint32_t ActorId(RE::Actor* actor)
        {
            return actor ? actor->GetFormID() : 0u;
        }

        bool IsBleedingOut(RE::Actor* actor)
        {
            auto* state = actor ? actor->AsActorState() : nullptr;
            return state && state->IsBleedingOut();
        }

        bool IsUsableActor(RE::Actor* actor)
        {
            return actor && !actor->IsDead() && !actor->IsDisabled() && actor->Is3DLoaded();
        }

        float ActorHealth(RE::Actor* actor)
        {
            return actor ? actor->GetActorValue(RE::ActorValue::kHealth) : 0.0f;
        }

        float ActorHealthPct(RE::Actor* actor)
        {
            if (!actor) {
                return 0.0f;
            }
            const float maxHealth = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
            return (ActorHealth(actor) / maxHealth) * 100.0f;
        }

        bool IsAboveHealthThreshold(RE::Actor* actor, float thresholdPct)
        {
            if (!actor) {
                return false;
            }
            const float safeThreshold = std::clamp(thresholdPct, 1.0f, 95.0f);
            return ActorHealth(actor) > 0.0f && ActorHealthPct(actor) > safeThreshold;
        }

        bool IsStandingActor(RE::Actor* actor)
        {
            if (!IsUsableActor(actor)) {
                return false;
            }
            if (IsBleedingOut(actor)) {
                return false;
            }
            if (TFD::DefeatMonitor::IsThresholdDownedActor(actor)) {
                return false;
            }
            return actor->GetActorValue(RE::ActorValue::kHealth) > 0.0f;
        }

        bool IsAssistWindowOpen()
        {
            if (TFD::PleasureRuntime::IsActive() || TFD::PleasureRuntime::IsBlocking()) {
                return false;
            }
            return TFD::Bleedout::DefeatGlue::IsPlayerBleedHoldTargetBlocked();
        }

        bool SameLoadedSpace(RE::Actor* lhs, RE::Actor* rhs)
        {
            if (!lhs || !rhs) {
                return false;
            }
            auto* lhsCell = lhs->GetParentCell();
            auto* rhsCell = rhs->GetParentCell();
            if (lhsCell && rhsCell && lhsCell != rhsCell) {
                return false;
            }
            auto* lhsWorld = lhs->GetWorldspace();
            auto* rhsWorld = rhs->GetWorldspace();
            if (lhsWorld && rhsWorld && lhsWorld != rhsWorld) {
                return false;
            }
            return true;
        }

        float DistSq(RE::TESObjectREFR* lhs, RE::TESObjectREFR* rhs)
        {
            if (!lhs || !rhs) {
                return 0.0f;
            }
            const auto a = lhs->GetPosition();
            const auto b = rhs->GetPosition();
            const float dx = a.x - b.x;
            const float dy = a.y - b.y;
            const float dz = a.z - b.z;
            return dx * dx + dy * dy + dz * dz;
        }

        bool IsNear(RE::TESObjectREFR* lhs, RE::TESObjectREFR* rhs, float radius)
        {
            if (!lhs || !rhs || radius <= 0.0f) {
                return false;
            }
            return DistSq(lhs, rhs) <= radius * radius;
        }

        RE::Actor* ResolveCurrentTarget(RE::Actor* actor)
        {
            return TFD::Actor::GetCurrentTarget(actor);
        }

        bool IsPlayerSideActor(RE::Actor* actor, RE::Actor* player)
        {
            if (!actor || actor == player) {
                return false;
            }
            if (TFD::Bleedout::DefeatGlue::IsObserverAlly(actor)) {
                return true;
            }
            return actor->IsPlayerTeammate() ||
                TFD::TeammateManager::IsActiveFollowerActor(actor) ||
                TFD::TeammateManager::IsPlayerSideTeammateActor(actor) ||
                TFD::Tame::IsCompanion(actor);
        }

        bool IsValidAlly(RE::Actor* actor, RE::Actor* player)
        {
            return IsStandingActor(actor) &&
                IsAboveHealthThreshold(actor, TFD::Settings::GetAllyDownedThresholdPct()) &&
                SameLoadedSpace(actor, player) &&
                IsPlayerSideActor(actor, player) &&
                !TFD::HostilityController::IsActorTemporarilySuppressed(actor);
        }

        bool IsValidThreat(RE::Actor* actor, RE::Actor* player)
        {
            if (!IsStandingActor(actor) ||
                !IsAboveHealthThreshold(actor, TFD::Settings::GetEnemyDownedThresholdPct()) ||
                !SameLoadedSpace(actor, player)) {
                return false;
            }
            if (actor == player || IsPlayerSideActor(actor, player)) {
                return false;
            }
            if (TFD::HostilityController::IsActorTemporarilySuppressed(actor)) {
                return false;
            }

            auto* currentTarget = ResolveCurrentTarget(actor);
            const bool targetsPlayerSide = currentTarget == player || IsPlayerSideActor(currentTarget, player);
            const bool combatPosture = actor->IsInCombat() || actor->IsWeaponDrawn();
            const bool strongPosture = actor->IsInCombat() && actor->IsWeaponDrawn();

            if (targetsPlayerSide) {
                // CB10: a stale far target is not enough to keep the battle alive.
                // Keep distant archers/mages only if they still have strong combat posture.
                // Otherwise require the target/player to be within a real local engagement range.
                if (IsNear(actor, currentTarget, kTargetEngageRadius) || IsNear(actor, player, kTargetEngageRadius) || strongPosture) {
                    return true;
                }
                return false;
            }

            // CB10: generic posture without a player-side target is local only.
            // This prevents far dungeon actors with stale IsInCombat from keeping
            // teammates in search/assist mode after the actual encounter is resolved.
            return combatPosture && IsNear(actor, player, kLocalThreatRadius);
        }

        bool ContainsActor(const std::vector<RE::Actor*>& actors, RE::Actor* actor)
        {
            const auto id = ActorId(actor);
            if (id == 0) {
                return false;
            }
            return std::any_of(actors.begin(), actors.end(), [id](RE::Actor* candidate) {
                return ActorId(candidate) == id;
            });
        }

        void AddUnique(std::vector<RE::Actor*>& actors, RE::Actor* actor)
        {
            if (!actor || ContainsActor(actors, actor)) {
                return;
            }
            actors.push_back(actor);
        }

        void MergeUnique(std::vector<RE::Actor*>& out, const std::vector<RE::Actor*>& in, RE::Actor* player, bool allies)
        {
            for (auto* actor : in) {
                if (allies) {
                    if (IsValidAlly(actor, player)) {
                        AddUnique(out, actor);
                    }
                }
                else if (IsValidThreat(actor, player)) {
                    AddUnique(out, actor);
                }
            }
        }

        struct RuntimeActors
        {
            std::vector<RE::Actor*> allies{};
            std::vector<RE::Actor*> threats{};
        };

        RuntimeActors CollectRuntimeActors(RE::Actor* player)
        {
            RuntimeActors result{};
            if (!player) {
                return result;
            }

            const float radius = (std::max)(kMinRetargetRadius, TFD::Settings::GetSweepRadius() + kRetargetRadiusPadding);
            auto snapshot = TFD::Actor::BuildSnapshot(player, TFD::Actor::ScanOptions{ radius, false });
            result.allies.reserve(snapshot.actors.size());
            result.threats.reserve(snapshot.actors.size());

            for (const auto& info : snapshot.actors) {
                auto* actor = info.get();
                if (!actor) {
                    continue;
                }
                if (IsValidAlly(actor, player)) {
                    AddUnique(result.allies, actor);
                }
                else if (IsValidThreat(actor, player)) {
                    AddUnique(result.threats, actor);
                }
            }

            MergeUnique(result.allies, TFD::Bleedout::DefeatGlue::CollectStandingFollowersFromSnapshot(), player, true);
            MergeUnique(result.threats, TFD::Bleedout::DefeatGlue::CollectStandingEnemiesFromSnapshot(), player, false);
            return result;
        }

        RE::Actor* PickHighestHealthTarget(
            RE::Actor* source,
            const std::vector<RE::Actor*>& candidates,
            RE::Actor* preferredA,
            RE::Actor* preferredB,
            RE::Actor* player,
            bool pickAlly)
        {
            RE::Actor* best = nullptr;
            float bestScore = -std::numeric_limits<float>::max();
            for (auto* candidate : candidates) {
                if (!source || !candidate || source == candidate) {
                    continue;
                }

                const bool valid = pickAlly ? IsValidAlly(candidate, player) : IsValidThreat(candidate, player);
                if (!valid) {
                    continue;
                }

                // CB08: primary priority is the largest remaining HP above threshold.
                // Distance/current engagement only breaks ties, so low-HP or threshold-downed
                // actors are no longer attractive combat targets while the player is down.
                float score = ActorHealth(candidate) * 1000.0f;
                score += ActorHealthPct(candidate) * 8.0f;
                if (candidate == preferredA) {
                    score += 350.0f;
                }
                if (candidate == preferredB) {
                    score += 250.0f;
                }
                if (candidate->IsInCombat()) {
                    score += 75.0f;
                }
                if (candidate->IsWeaponDrawn()) {
                    score += 35.0f;
                }
                const float distSq = DistSq(source, candidate);
                score -= (std::min)(distSq / 15000.0f, 220.0f);

                if (!best || score > bestScore) {
                    best = candidate;
                    bestScore = score;
                }
            }
            return best;
        }

        RE::Actor* PickAllyForThreat(RE::Actor* threat, const std::vector<RE::Actor*>& allies, RE::Actor* player)
        {
            auto* resolved = TFD::Bleedout::DefeatGlue::ResolveBleedRedirectTarget(threat);
            auto* currentTarget = ResolveCurrentTarget(threat);
            return PickHighestHealthTarget(threat, allies, resolved, currentTarget, player, true);
        }

        RE::Actor* PickThreatForAlly(RE::Actor* ally, const std::vector<RE::Actor*>& threats, RE::Actor* player)
        {
            auto* resolved = TFD::Bleedout::DefeatGlue::ResolveBleedFollowerAggroTarget(ally);
            auto* currentTarget = ResolveCurrentTarget(ally);
            return PickHighestHealthTarget(ally, threats, resolved, currentTarget, player, false);
        }


        void SetRuntimeRole(RE::Actor* actor, RuntimeRole role)
        {
            const auto id = ActorId(actor);
            if (id == 0) {
                return;
            }
            std::scoped_lock lk(g_runtimeLock);
            if (role == RuntimeRole::None) {
                g_roles.erase(id);
            }
            else {
                g_roles[id] = role;
            }
        }

        void StorePendingTarget(RE::Actor* actor, RE::Actor* target, RuntimeRole role, Clock::time_point now)
        {
            const auto actorId = ActorId(actor);
            if (actorId == 0 || !target) {
                return;
            }
            std::scoped_lock lk(g_runtimeLock);
            g_pendingTargets[actorId] = PendingTarget{ target->GetHandle(), role, now + kPendingTargetTtl };
            g_roles[actorId] = role;
            if (role == RuntimeRole::BleedoutAlly) {
                g_roles[target->GetFormID()] = RuntimeRole::BleedoutThreat;
            }
            else if (role == RuntimeRole::BleedoutThreat) {
                g_roles[target->GetFormID()] = RuntimeRole::BleedoutAlly;
            }
        }

        void PruneRuntimeState(Clock::time_point now, bool clearAll = false)
        {
            std::scoped_lock lk(g_runtimeLock);
            if (clearAll) {
                g_pendingTargets.clear();
                g_roles.clear();
                g_lastCommitDispatch.clear();
                g_settleCandidates.clear();
                g_lastSettleDispatch.clear();
                return;
            }
            for (auto it = g_pendingTargets.begin(); it != g_pendingTargets.end();) {
                if (it->second.until <= now) {
                    it = g_pendingTargets.erase(it);
                }
                else {
                    ++it;
                }
            }
            if (g_pendingTargets.empty() && !g_bleedoutRetargetActive.load(std::memory_order_acquire)) {
                g_roles.clear();
            }
        }

        RuntimeRole GetRuntimeRole(RE::Actor* actor)
        {
            const auto id = ActorId(actor);
            if (id == 0) {
                return RuntimeRole::None;
            }
            std::scoped_lock lk(g_runtimeLock);
            auto it = g_roles.find(id);
            return it != g_roles.end() ? it->second : RuntimeRole::None;
        }

        RE::Actor* GetPendingTarget(RE::Actor* actor)
        {
            const auto id = ActorId(actor);
            if (id == 0) {
                return nullptr;
            }
            if (!IsAssistWindowOpen()) {
                PruneRuntimeState(Clock::now(), true);
                return nullptr;
            }
            const auto now = Clock::now();
            std::scoped_lock lk(g_runtimeLock);
            auto it = g_pendingTargets.find(id);
            if (it == g_pendingTargets.end()) {
                return nullptr;
            }
            if (it->second.until <= now) {
                g_pendingTargets.erase(it);
                return nullptr;
            }
            auto sp = it->second.target.get();
            return sp.get();
        }

        bool ShouldLightEvaluate(RE::Actor* actor)
        {
            return actor && actor->Is3DLoaded();
        }

        RuntimeRole InverseRole(RuntimeRole role)
        {
            switch (role) {
            case RuntimeRole::BleedoutAlly:
                return RuntimeRole::BleedoutThreat;
            case RuntimeRole::BleedoutThreat:
                return RuntimeRole::BleedoutAlly;
            default:
                return RuntimeRole::None;
            }
        }

        bool ShouldDispatchCommit(RE::Actor* actor, Clock::time_point now, bool force)
        {
            const auto actorId = ActorId(actor);
            if (actorId == 0) {
                return false;
            }

            std::scoped_lock lk(g_runtimeLock);
            auto it = g_lastCommitDispatch.find(actorId);
            if (force || it == g_lastCommitDispatch.end() || now >= it->second + kCommitDispatchInterval) {
                g_lastCommitDispatch[actorId] = now;
                return true;
            }
            return false;
        }

        void QueuePapyrusCommitIfDue(RE::Actor* actor, const char* reason, Clock::time_point now, bool force)
        {
            if (!actor || !reason) {
                return;
            }
            if (!ShouldDispatchCommit(actor, now, force)) {
                return;
            }

            const bool queued = TFD::FlowController::QueueBridgeModEvent(
                "TFDCombatBehaviorCommit",
                actor,
                reason,
                0.0f);

            spdlog::info(
                "[TFD][CombatBehavior][CB10] bridge commit {} actor={:08X} reason={}",
                queued ? "queued" : "failed",
                ActorId(actor),
                reason);
        }

        void RememberSettleCandidate(RE::Actor* actor)
        {
            const auto id = ActorId(actor);
            if (id == 0) {
                return;
            }
            std::scoped_lock lk(g_runtimeLock);
            g_settleCandidates[id] = actor->GetHandle();
        }

        bool ShouldDispatchSettle(RE::Actor* actor, Clock::time_point now, bool force)
        {
            const auto actorId = ActorId(actor);
            if (actorId == 0) {
                return false;
            }

            std::scoped_lock lk(g_runtimeLock);
            auto it = g_lastSettleDispatch.find(actorId);
            if (force || it == g_lastSettleDispatch.end() || now >= it->second + kSettleDispatchInterval) {
                g_lastSettleDispatch[actorId] = now;
                return true;
            }
            return false;
        }

        void QueueSettleActorIfDue(RE::Actor* actor, const char* reason, Clock::time_point now, bool force)
        {
            if (!actor || !reason || !IsPlayerSideActor(actor, Player())) {
                return;
            }
            if (!ShouldDispatchSettle(actor, now, force)) {
                return;
            }

            const bool queued = TFD::FlowController::QueueBridgeModEvent(
                "TFDCombatBehaviorSettle",
                actor,
                reason,
                0.0f);

            spdlog::info(
                "[TFD][CombatBehavior][CB10] settle {} actor={:08X} reason={}",
                queued ? "queued" : "failed",
                ActorId(actor),
                reason);
        }

        void QueueKnownAllySettle(const char* reason, Clock::time_point now, bool force)
        {
            std::vector<RE::ActorHandle> handles{};
            {
                std::scoped_lock lk(g_runtimeLock);
                handles.reserve(g_settleCandidates.size());
                for (const auto& entry : g_settleCandidates) {
                    handles.push_back(entry.second);
                }
            }

            for (auto& handle : handles) {
                auto actorSp = handle.get();
                if (auto* actor = actorSp.get()) {
                    QueueSettleActorIfDue(actor, reason, now, force);
                }
            }
        }

        bool QueueCombatPair(RE::Actor* actor, RE::Actor* target, RuntimeRole role, const char* reason)
        {
            if (!actor || !target || actor == target) {
                return false;
            }
            auto* player = Player();
            if (!player || !SameLoadedSpace(actor, target) || !SameLoadedSpace(actor, player)) {
                return false;
            }

            // CB05: advisory only.
            // Do not write currentCombatTarget, do not RequestDetectionLevel, do not UpdateCombat,
            // do not EvaluatePackage here. R101 crashed inside Skyrim detection/ActorKnowledge while
            // this module was repeatedly forcing target/detection state during player bleedout.
            // The native side now only publishes a short-lived target hint; Papyrus teammate
            // maintenance commits it with Actor.StartCombat(), which is slower but much safer.
            const auto oldPending = ActorId(GetPendingTarget(actor));
            const auto newPending = ActorId(target);
            const bool changed = oldPending != newPending;

            const auto now = Clock::now();
            StorePendingTarget(actor, target, role, now);
            const auto inverse = InverseRole(role);
            if (inverse != RuntimeRole::None) {
                StorePendingTarget(target, actor, inverse, now);
            }

            const char* dispatchReason = reason && reason[0] ? reason : "bleedout_assist_hint";
            if (changed) {
                spdlog::info(
                    "[TFD][CombatBehavior][CB10] pending actor={:08X} old={:08X} new={:08X} role={} reason={}",
                    ActorId(actor),
                    oldPending,
                    newPending,
                    role == RuntimeRole::BleedoutThreat ? "threat" : "ally",
                    dispatchReason);
            }

            // CB09: the previous advisory target hint only became effective when the
            // teammate maintenance loop happened to process it. Queue a direct, safe
            // Papyrus StartCombat commit for both sides. Native still does not mutate
            // currentCombatTarget, detection, UpdateCombat, or packages.
            QueuePapyrusCommitIfDue(actor, dispatchReason, now, changed);
            QueuePapyrusCommitIfDue(target, role == RuntimeRole::BleedoutThreat ? "paired_ally_commit" : "paired_threat_commit", now, changed);
            return changed;
        }

        void MarkCurrentPairs(const RuntimeActors& actors, RE::Actor* player, Clock::time_point now)
        {
            for (auto* threat : actors.threats) {
                auto* target = ResolveCurrentTarget(threat);
                if (IsValidAlly(target, player)) {
                    StorePendingTarget(threat, target, RuntimeRole::BleedoutThreat, now);
                }
            }
            for (auto* ally : actors.allies) {
                auto* target = ResolveCurrentTarget(ally);
                if (IsValidThreat(target, player)) {
                    StorePendingTarget(ally, target, RuntimeRole::BleedoutAlly, now);
                }
            }
        }

        bool TargetNeedsThreatRedirect(RE::Actor* threat, RE::Actor* currentTarget, RE::Actor* player)
        {
            if (!threat || !player) {
                return false;
            }
            if (currentTarget == player) {
                return true;
            }
            if (!currentTarget) {
                return true;
            }
            if (IsBleedingOut(currentTarget)) {
                return true;
            }
            return !IsValidAlly(currentTarget, player);
        }

        bool TargetNeedsAllyRedirect(RE::Actor* ally, RE::Actor* currentTarget, RE::Actor* player)
        {
            if (!ally || !player) {
                return false;
            }
            if (!currentTarget) {
                return true;
            }
            if (currentTarget == player) {
                return true;
            }
            if (IsBleedingOut(currentTarget)) {
                return true;
            }
            return !IsValidThreat(currentTarget, player);
        }

        void TickUI()
        {
            g_tickPending.clear(std::memory_order_release);

            const auto now = Clock::now();
            if (!IsAssistWindowOpen()) {
                if (g_bleedoutRetargetActive.exchange(false, std::memory_order_acq_rel)) {
                    spdlog::info("[TFD][CombatBehavior][CB10] bleedout retarget inactive reason=assist_window_closed");
                    QueueKnownAllySettle("assist_window_closed", now, true);
                }
                PruneRuntimeState(now, true);
                return;
            }

            auto* player = Player();
            if (!player) {
                g_bleedoutRetargetActive.store(false, std::memory_order_release);
                PruneRuntimeState(now, true);
                return;
            }

            auto actors = CollectRuntimeActors(player);
            for (auto* ally : actors.allies) {
                RememberSettleCandidate(ally);
            }

            if (actors.allies.empty() || actors.threats.empty()) {
                if (!actors.allies.empty() && actors.threats.empty()) {
                    for (auto* ally : actors.allies) {
                        QueueSettleActorIfDue(ally, "no_active_bleedout_threat", now, false);
                    }
                }
                if (g_bleedoutRetargetActive.exchange(false, std::memory_order_acq_rel)) {
                    spdlog::info(
                        "[TFD][CombatBehavior][CB10] bleedout retarget inactive allies={} threats={}",
                        static_cast<unsigned int>(actors.allies.size()),
                        static_cast<unsigned int>(actors.threats.size()));
                    if (!actors.allies.empty() && actors.threats.empty()) {
                        QueueKnownAllySettle("no_active_bleedout_threat", now, false);
                    }
                }
                PruneRuntimeState(now, false);
                return;
            }

            if (!g_bleedoutRetargetActive.exchange(true, std::memory_order_acq_rel)) {
                spdlog::info(
                    "[TFD][CombatBehavior][CB10] bleedout retarget active allies={} threats={}",
                    static_cast<unsigned int>(actors.allies.size()),
                    static_cast<unsigned int>(actors.threats.size()));
            }

            MarkCurrentPairs(actors, player, now);

            for (auto* threat : actors.threats) {
                auto* currentTarget = ResolveCurrentTarget(threat);
                if (currentTarget == player) {
                    TFD::Bleedout::DefeatGlue::NoteEnemyTargetingPlayer(threat);
                }
                auto* replacement = PickAllyForThreat(threat, actors.allies, player);
                if (IsValidAlly(replacement, player) && ActorId(replacement) != ActorId(currentTarget)) {
                    const char* reason = TargetNeedsThreatRedirect(threat, currentTarget, player) ?
                        (currentTarget == player ? "enemy_off_bleedout_player" : "enemy_to_standing_ally") :
                        "enemy_to_highest_hp_ally";
                    (void)QueueCombatPair(threat, replacement, RuntimeRole::BleedoutThreat, reason);
                }
            }

            for (auto* ally : actors.allies) {
                auto* currentTarget = ResolveCurrentTarget(ally);
                auto* replacement = PickThreatForAlly(ally, actors.threats, player);
                if (IsValidThreat(replacement, player) && ActorId(replacement) != ActorId(currentTarget)) {
                    const char* reason = TargetNeedsAllyRedirect(ally, currentTarget, player) ?
                        "ally_to_standing_enemy" :
                        "ally_to_highest_hp_enemy";
                    (void)QueueCombatPair(ally, replacement, RuntimeRole::BleedoutAlly, reason);
                }
            }

            for (auto* ally : actors.allies) {
                SetRuntimeRole(ally, RuntimeRole::BleedoutAlly);
            }
            for (auto* threat : actors.threats) {
                SetRuntimeRole(threat, RuntimeRole::BleedoutThreat);
            }
            PruneRuntimeState(now, false);
        }

        void WorkerLoop()
        {
            while (g_running.load(std::memory_order_acquire)) {
                if (!g_tickPending.test_and_set(std::memory_order_acq_rel)) {
                    if (auto* tasks = SKSE::GetTaskInterface()) {
                        tasks->AddTask([]() { TickUI(); });
                    }
                    else {
                        g_tickPending.clear(std::memory_order_release);
                    }
                }
                std::this_thread::sleep_for(kWorkerInterval);
            }
        }

        bool IsSoftPackageRepairReason(std::string_view reason)
        {
            if (reason.empty()) {
                return true;
            }
            return reason == "post_load_humanoid_catchup" ||
                reason == "loading_menu_closed" ||
                reason == "converted_package_repair" ||
                reason == "follow_maintenance" ||
                reason == "follow_maintenance_r90_safe_eval" ||
                reason == "register_now_refresh" ||
                reason == "teammate_manager_register_now" ||
                reason == "combat_behavior_commit";
        }

        RE::Actor* PapyrusGetPendingCombatAssistTarget(RE::StaticFunctionTag*, RE::Actor* actor)
        {
            return GetPendingTarget(actor);
        }

        bool PapyrusHasPendingCombatAssistTarget(RE::StaticFunctionTag*, RE::Actor* actor)
        {
            return GetPendingTarget(actor) != nullptr;
        }
    }

    void Install()
    {
        if (g_installed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        g_running.store(true, std::memory_order_release);
        g_bleedoutRetargetActive.store(false, std::memory_order_release);
        PruneRuntimeState(Clock::now(), true);
        g_worker = std::thread([]() { WorkerLoop(); });

        spdlog::info("[TFD][CombatBehavior] installed CB10 bleedout assist settle advisor");
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
        g_bleedoutRetargetActive.store(false, std::memory_order_release);
        PruneRuntimeState(Clock::now(), true);

        spdlog::info("[TFD][CombatBehavior] shutdown CB10 bleedout assist settle advisor");
    }

    void ResetForLoad()
    {
        g_bleedoutRetargetActive.store(false, std::memory_order_release);
        PruneRuntimeState(Clock::now(), true);
        spdlog::info("[TFD][CombatBehavior] reset runtime state CB10 bleedout assist settle advisor");
    }

    bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm)
    {
        if (!a_vm) {
            return false;
        }

        a_vm->RegisterFunction(
            "GetPendingCombatAssistTarget",
            "TFDCombatBehaviorNative",
            PapyrusGetPendingCombatAssistTarget);

        a_vm->RegisterFunction(
            "HasPendingCombatAssistTarget",
            "TFDCombatBehaviorNative",
            PapyrusHasPendingCombatAssistTarget);

        spdlog::info("[TFD][CombatBehavior] Papyrus natives registered CB10 bleedout assist settle advisor");
        return true;
    }

    bool IsNormalCombatAssistActive()
    {
        return false;
    }

    bool IsActiveCombatAlly(RE::Actor* actor)
    {
        return GetRuntimeRole(actor) == RuntimeRole::BleedoutAlly;
    }

    bool IsActiveCombatThreat(RE::Actor* actor)
    {
        return GetRuntimeRole(actor) == RuntimeRole::BleedoutThreat;
    }

    bool IsActiveCombatPair(RE::Actor* actor, RE::Actor* target)
    {
        if (!actor || !target) {
            return false;
        }
        auto* pending = GetPendingTarget(actor);
        if (pending && ActorId(pending) == ActorId(target)) {
            return true;
        }
        const auto actorRole = GetRuntimeRole(actor);
        const auto targetRole = GetRuntimeRole(target);
        return (actorRole == RuntimeRole::BleedoutAlly && targetRole == RuntimeRole::BleedoutThreat) ||
            (actorRole == RuntimeRole::BleedoutThreat && targetRole == RuntimeRole::BleedoutAlly);
    }

    bool ShouldPreserveCombatTarget(RE::Actor* actor, RE::Actor* target)
    {
        if (!g_bleedoutRetargetActive.load(std::memory_order_acquire)) {
            return false;
        }
        if (!IsAssistWindowOpen()) {
            return false;
        }
        return IsActiveCombatPair(actor, target);
    }

    bool ShouldSuppressTeammatePackageRepair(RE::Actor* actor, const char* reason)
    {
        if (!actor || !g_bleedoutRetargetActive.load(std::memory_order_acquire)) {
            return false;
        }
        if (!IsAssistWindowOpen()) {
            return false;
        }
        if (!IsActiveCombatAlly(actor)) {
            return false;
        }
        return IsSoftPackageRepairReason(reason ? std::string_view(reason) : std::string_view{});
    }

    bool IsDiagnosticActor(RE::Actor* actor)
    {
        return GetRuntimeRole(actor) != RuntimeRole::None;
    }

    bool IsDiagnosticPair(RE::Actor* actor, RE::Actor* target)
    {
        return IsActiveCombatPair(actor, target);
    }

    const char* DiagnosticRole(RE::Actor* actor)
    {
        switch (GetRuntimeRole(actor)) {
        case RuntimeRole::BleedoutAlly:
            return "bleedout_ally";
        case RuntimeRole::BleedoutThreat:
            return "bleedout_threat";
        default:
            return "none";
        }
    }
}
