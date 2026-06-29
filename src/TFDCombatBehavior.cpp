#include "TFDCombatBehavior.h"

#include "TFDActor.h"
#include "TFDBleedout.h"
#include "TFDDefeatMonitor.h"
#include "TFDFlowController.h"
#include "TFDHostilityController.h"
#include "TFDPleasureRuntime.h"
#include "TFDRecruit.h"
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
#include <string>
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

        struct CommitFlight
        {
            std::uint32_t targetId{ 0 };
            std::string reason{};
            Clock::time_point until{};
        };

        struct SettleFlight
        {
            std::string reason{};
            Clock::time_point until{};
        };

        struct AssistAnchor
        {
            RE::ActorHandle ally{};
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
        std::unordered_map<std::uint32_t, CommitFlight> g_commitFlights{};
        std::unordered_map<std::uint32_t, AssistAnchor> g_assistAnchors{};
        Clock::time_point g_assistObserveHoldUntil{};
        std::uint32_t g_assistObserveHoldThreatId{ 0 };
        std::uint32_t g_assistObserveHoldAllyId{ 0 };
        std::unordered_map<std::uint32_t, RE::ActorHandle> g_settleCandidates{};
        std::unordered_map<std::uint32_t, Clock::time_point> g_lastSettleDispatch{};
        std::unordered_map<std::uint32_t, SettleFlight> g_settleFlights{};

        struct FactionRuntimeCache
        {
            RE::TESFaction* knockOut{ nullptr };
            RE::TESFaction* retarget{ nullptr };
            RE::TESFaction* combatThreat{ nullptr };
            RE::TESFaction* combatAlly{ nullptr };
            RE::TESFaction* pacify{ nullptr };
            bool tried{ false };
        };

        struct BleedoutFactionSuppressEntry
        {
            std::uint32_t actorId{ 0 };
            RE::ActorHandle handle{};
            Clock::time_point until{};
            Clock::time_point lastPackageEval{};

            bool hadKnockOut{ false };
            bool addedKnockOut{ false };
            bool hadRetarget{ false };
            bool addedRetarget{ false };
            bool hadThreat{ false };
            bool addedThreat{ false };
            bool hadAlly{ false };
            bool addedAlly{ false };
            bool hadPacify{ false };
            bool addedPacify{ false };

            bool hasOriginalAggression{ false };
            bool hasOriginalAssistance{ false };
            float originalAggression{ 0.0f };
            float originalAssistance{ 0.0f };
        };

        FactionRuntimeCache g_factionCache{};
        std::unordered_map<std::uint32_t, BleedoutFactionSuppressEntry> g_bleedoutFactionSuppressions{};

        constexpr auto kWorkerInterval = std::chrono::milliseconds(300);
        constexpr auto kPendingTargetTtl = std::chrono::milliseconds(2200);
        constexpr auto kCommitDispatchInterval = std::chrono::milliseconds(650);
        constexpr auto kSettleDispatchInterval = std::chrono::milliseconds(1600);
        constexpr auto kFirewallCommitDispatchInterval = std::chrono::milliseconds(250);
        constexpr auto kCommitFlightTtl = std::chrono::milliseconds(1600);
        constexpr auto kSettleFlightTtl = std::chrono::milliseconds(6000);
        constexpr auto kAssistAnchorTtl = std::chrono::milliseconds(3500);
        constexpr auto kAssistAnchorObserveHoldTtl = std::chrono::milliseconds(15000);
        constexpr auto kLocalAssistObserveHoldTtl = std::chrono::milliseconds(15000);
        constexpr auto kFactionSuppressTtl = std::chrono::milliseconds(4500);
        constexpr auto kFactionSuppressObserveHoldTtl = std::chrono::milliseconds(15000);
        constexpr auto kHardSuppressEvalInterval = std::chrono::milliseconds(1200);
        constexpr float kMinRetargetRadius = 2400.0f;
        constexpr float kRetargetRadiusPadding = 600.0f;
        constexpr float kLocalThreatRadius = 3600.0f;
        constexpr float kTargetEngageRadius = 5200.0f;

        void PruneFactionSuppressionsLocked(Clock::time_point now, bool clearAll, const char* reason);

        RE::Actor* Player()
        {
            return RE::PlayerCharacter::GetSingleton();
        }

        std::uint32_t ActorId(RE::Actor* actor)
        {
            return actor ? actor->GetFormID() : 0u;
        }

        RE::TESFaction* LookupFactionByEditorID(const char* editorId)
        {
            if (!editorId || !editorId[0]) {
                return nullptr;
            }
            return RE::TESForm::LookupByEditorID<RE::TESFaction>(editorId);
        }

        void ResolveBleedoutSuppressFactions()
        {
            if (g_factionCache.tried) {
                return;
            }

            g_factionCache.tried = true;
            g_factionCache.knockOut = LookupFactionByEditorID("TFDKnockOutFaction");
            g_factionCache.retarget = LookupFactionByEditorID("TFDRetargetFaction");
            g_factionCache.combatThreat = LookupFactionByEditorID("TFDCombatThreatFaction");
            g_factionCache.combatAlly = LookupFactionByEditorID("TFDCombatAllyFaction");
            g_factionCache.pacify = LookupFactionByEditorID("TFDPacifyFaction");

            spdlog::info(
                "[TFD][CombatBehavior][R462A] faction suppress resolve knockOut={} retarget={} threat={} ally={} pacify={}",
                g_factionCache.knockOut ? 1 : 0,
                g_factionCache.retarget ? 1 : 0,
                g_factionCache.combatThreat ? 1 : 0,
                g_factionCache.combatAlly ? 1 : 0,
                g_factionCache.pacify ? 1 : 0);
        }

        bool AddFactionTracked(RE::Actor* actor, RE::TESFaction* faction, bool& hadFlag, bool& addedFlag)
        {
            if (!actor || !faction) {
                return false;
            }
            if (!hadFlag && !addedFlag) {
                hadFlag = actor->IsInFaction(faction);
                if (!hadFlag) {
                    actor->AddToFaction(faction, 0);
                    addedFlag = true;
                    return true;
                }
            }
            return false;
        }

        bool RemoveFactionIfAdded(RE::Actor* actor, RE::TESFaction* faction, bool addedFlag)
        {
            if (!actor || !faction || !addedFlag || !actor->IsInFaction(faction)) {
                return false;
            }
            actor->RemoveFromFaction(faction);
            return true;
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

        bool IsVictoryOrRecruitTransitionActor(RE::Actor* actor)
        {
            if (!actor) {
                return false;
            }
            return TFD::Actor::Ops::IsDefeatedEnemyKnocked(actor) ||
                TFD::DefeatMonitor::IsThresholdDownedActor(actor) ||
                TFD::Recruit::IsRecruitCommitPending(actor) ||
                TFD::Recruit::IsRecruitLike(actor);
        }

        bool IsAssistWindowOpen()
        {
            if (TFD::PleasureRuntime::IsActive() || TFD::PleasureRuntime::IsBlocking()) {
                return false;
            }
            return TFD::Bleedout::DefeatGlue::IsPlayerBleedHoldTargetBlocked();
        }

        bool IsBattleObserveInProgress()
        {
            return TFD::Bleedout::BleedBattleObservePendingRef() ||
                TFD::Bleedout::BleedBattleObserveActiveRef();
        }

        bool IsLocalAssistObserveHoldActiveLocked(Clock::time_point now)
        {
            return g_assistObserveHoldUntil.time_since_epoch().count() != 0 &&
                g_assistObserveHoldUntil > now;
        }

        bool IsAssistObserveHoldActiveLocked(Clock::time_point now)
        {
            return IsBattleObserveInProgress() || IsLocalAssistObserveHoldActiveLocked(now);
        }

        bool IsAssistObserveHoldActive(Clock::time_point now)
        {
            if (IsBattleObserveInProgress()) {
                return true;
            }
            std::scoped_lock lk(g_runtimeLock);
            return IsLocalAssistObserveHoldActiveLocked(now);
        }

        void ClearAssistObserveHoldLocked()
        {
            g_assistObserveHoldUntil = {};
            g_assistObserveHoldThreatId = 0;
            g_assistObserveHoldAllyId = 0;
        }

        void ArmAssistObserveHoldLocked(RE::Actor* threat, RE::Actor* ally, Clock::time_point now, const char* reason)
        {
            const auto threatId = ActorId(threat);
            const auto allyId = ActorId(ally);
            if (threatId == 0 || allyId == 0 || threat == ally) {
                return;
            }

            const auto newUntil = now + kLocalAssistObserveHoldTtl;
            const bool wasActive = IsLocalAssistObserveHoldActiveLocked(now);
            const bool changed = !wasActive ||
                g_assistObserveHoldThreatId != threatId ||
                g_assistObserveHoldAllyId != allyId ||
                g_assistObserveHoldUntil < newUntil;

            if (g_assistObserveHoldUntil < newUntil) {
                g_assistObserveHoldUntil = newUntil;
            }
            g_assistObserveHoldThreatId = threatId;
            g_assistObserveHoldAllyId = allyId;

            if (changed) {
                spdlog::info(
                    "[TFD][CombatBehavior][R27] assist observe hold armed threat={:08X} ally={:08X} ttlMs={} reason={}",
                    threatId,
                    allyId,
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(kLocalAssistObserveHoldTtl).count()),
                    reason ? reason : "unknown");
            }
        }

        bool IsNoThreatSettleReason(std::string_view reason)
        {
            return reason == "no_active_bleedout_threat";
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

        bool IsConcreteStandingPlayerSideAllyForBleedout(RE::Actor* actor, RE::Actor* player)
        {
            // R31: mirror DefeatMonitor's concrete player-side ally guard.  During the
            // first player-down ticks, follower alias state and temporary suppression can
            // blink before CombatBehavior sees the same ally that PlayerDownRouter already
            // used to delay the bleedout outcome.  If the actor is a real standing
            // player-side ally in loaded space, keep it usable as a redirect anchor.
            if (!actor || !player || actor == player || actor->IsDead() || actor->IsDisabled()) {
                return false;
            }
            if (!actor->Is3DLoaded()) {
                return false;
            }
            if (TFD::Actor::Ops::IsDefeatedEnemyKnocked(actor) || TFD::Recruit::IsRecruitCommitPending(actor)) {
                return false;
            }
            if (!IsAboveHealthThreshold(actor, TFD::Settings::GetAllyDownedThresholdPct())) {
                return false;
            }
            if (!SameLoadedSpace(actor, player)) {
                return false;
            }
            return actor->IsPlayerTeammate() ||
                TFD::TeammateManager::IsActiveFollowerActor(actor) ||
                TFD::TeammateManager::IsPlayerSideTeammateActor(actor) ||
                TFD::Tame::IsCompanion(actor) ||
                TFD::Bleedout::DefeatGlue::IsObserverAlly(actor);
        }

        bool IsValidAlly(RE::Actor* actor, RE::Actor* player)
        {
            return IsStandingActor(actor) &&
                !TFD::Actor::Ops::IsDefeatedEnemyKnocked(actor) &&
                !TFD::Recruit::IsRecruitCommitPending(actor) &&
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
            if (IsVictoryOrRecruitTransitionActor(actor)) {
                return false;
            }
            if (TFD::HostilityController::IsActorTemporarilySuppressed(actor)) {
                return false;
            }

            // R435A / SmartTargetingNPC rule: do not invent a threat from raw
            // hostility, weapon posture, or proximity.  For the teammate alert / assist
            // layer, an enemy is actionable only when Skyrim's combat group exists and
            // the actor's current target is exactly the player.  If vanilla has no
            // combat target, or the enemy is merely fighting another actor, TFD must not
            // keep teammates in alert/search mode.
            if (!actor->IsInCombat()) {
                return false;
            }
            auto* combatGroup = actor->GetCombatGroup();
            if (!combatGroup) {
                return false;
            }
            auto* currentTarget = ResolveCurrentTarget(actor);
            if (currentTarget != player) {
                return false;
            }

            return true;
        }

        bool IsActiveBleedoutThreatActor(RE::Actor* actor, RE::Actor* player)
        {
            if (!IsStandingActor(actor) ||
                !IsAboveHealthThreshold(actor, TFD::Settings::GetEnemyDownedThresholdPct()) ||
                !SameLoadedSpace(actor, player)) {
                return false;
            }
            if (actor == player || IsPlayerSideActor(actor, player)) {
                return false;
            }
            if (IsVictoryOrRecruitTransitionActor(actor)) {
                return false;
            }
            return true;
        }

        bool IsAnchorAllyUsable(RE::Actor* actor, RE::Actor* player)
        {
            // R24: anchor validation is intentionally slightly less volatile than
            // IsValidAlly().  Temporary suppression can blink during bleedout dialogue
            // setup; that must not make CombatBehavior conclude that no standing ally
            // exists and fall back to terminal hard suppression.
            return IsStandingActor(actor) &&
                !TFD::Actor::Ops::IsDefeatedEnemyKnocked(actor) &&
                !TFD::Recruit::IsRecruitCommitPending(actor) &&
                IsAboveHealthThreshold(actor, TFD::Settings::GetAllyDownedThresholdPct()) &&
                SameLoadedSpace(actor, player) &&
                IsPlayerSideActor(actor, player);
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

        std::uint32_t MergeConcretePlayerSideAllies(
            std::vector<RE::Actor*>& out,
            const std::vector<RE::Actor*>& in,
            RE::Actor* player,
            const char* reason)
        {
            std::uint32_t merged = 0;
            for (auto* actor : in) {
                if (!IsConcreteStandingPlayerSideAllyForBleedout(actor, player) || ContainsActor(out, actor)) {
                    continue;
                }
                AddUnique(out, actor);
                ++merged;
            }

            if (merged > 0) {
                spdlog::info(
                    "[TFD][CombatBehavior][R31] concrete player-side ally merged merged={} total={} reason={}",
                    merged,
                    static_cast<unsigned int>(out.size()),
                    reason ? reason : "unknown");
            }
            return merged;
        }

        bool IsConcreteBleedoutThreatForObserve(RE::Actor* actor, RE::Actor* player, const std::vector<RE::Actor*>& allies)
        {
            // R32: BattleObserve cannot depend only on vanilla combat group visibility.
            // During the first player-down ticks, Flow/PlayerDownRouter can already see
            // an active hostile targeting the player while CombatBehavior's strict
            // IsValidThreat() still returns false because the combat group or temporary
            // suppression state blinked.  For the bleedout observe owner, a loaded
            // standing non-player-side actor directly targeting the player/standing ally
            // is enough evidence to publish a redirect target.
            if (!IsStandingActor(actor) || !player || actor == player) {
                return false;
            }
            if (!IsAboveHealthThreshold(actor, TFD::Settings::GetEnemyDownedThresholdPct()) || !SameLoadedSpace(actor, player)) {
                return false;
            }
            if (IsPlayerSideActor(actor, player) || IsVictoryOrRecruitTransitionActor(actor)) {
                return false;
            }

            auto* currentTarget = ResolveCurrentTarget(actor);
            if (currentTarget == player) {
                return true;
            }

            bool hostileToSide = actor->IsHostileToActor(player) || player->IsHostileToActor(actor);
            for (auto* ally : allies) {
                if (!IsAnchorAllyUsable(ally, player)) {
                    continue;
                }
                if (currentTarget == ally) {
                    return true;
                }
                if (actor->IsHostileToActor(ally) || ally->IsHostileToActor(actor)) {
                    hostileToSide = true;
                }
            }

            if (TFD::HostilityController::IsActorTemporarilySuppressed(actor)) {
                return false;
            }
            if (!(actor->IsInCombat() || actor->IsWeaponDrawn()) || !hostileToSide) {
                return false;
            }
            if (IsNear(actor, player, kTargetEngageRadius)) {
                return true;
            }
            for (auto* ally : allies) {
                if (IsAnchorAllyUsable(ally, player) && IsNear(actor, ally, kTargetEngageRadius)) {
                    return true;
                }
            }
            return false;
        }

        std::uint32_t MergeConcreteBleedoutThreats(
            std::vector<RE::Actor*>& out,
            const std::vector<RE::Actor*>& in,
            RE::Actor* player,
            const std::vector<RE::Actor*>& allies,
            const char* reason)
        {
            std::uint32_t merged = 0;
            for (auto* actor : in) {
                if (!IsConcreteBleedoutThreatForObserve(actor, player, allies) || ContainsActor(out, actor)) {
                    continue;
                }
                AddUnique(out, actor);
                ++merged;
            }

            if (merged > 0) {
                spdlog::info(
                    "[TFD][CombatBehavior][R32] concrete active bleedout threat merged merged={} total={} allies={} reason={}",
                    merged,
                    static_cast<unsigned int>(out.size()),
                    static_cast<unsigned int>(allies.size()),
                    reason ? reason : "unknown");
            }
            return merged;
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

            const float baseRadius = (std::max)(kMinRetargetRadius, TFD::Settings::GetSweepRadius() + kRetargetRadiusPadding);
            const bool observeInProgress = IsBattleObserveInProgress();
            const float radius = observeInProgress ? (std::max)(baseRadius, 12000.0f) : baseRadius;
            auto snapshot = TFD::Actor::BuildSnapshot(player, TFD::Actor::ScanOptions{ radius, false });
            result.allies.reserve(snapshot.actors.size());
            result.threats.reserve(snapshot.actors.size());

            std::vector<RE::Actor*> snapshotActors{};
            snapshotActors.reserve(snapshot.actors.size());
            for (const auto& info : snapshot.actors) {
                auto* actor = info.get();
                if (!actor) {
                    continue;
                }
                snapshotActors.push_back(actor);
                if (IsValidAlly(actor, player)) {
                    AddUnique(result.allies, actor);
                }
                else if (IsValidThreat(actor, player)) {
                    AddUnique(result.threats, actor);
                }
            }

            auto observedFollowers = TFD::Bleedout::DefeatGlue::CollectStandingFollowersFromSnapshot();
            MergeUnique(result.allies, observedFollowers, player, true);
            MergeConcretePlayerSideAllies(result.allies, observedFollowers, player, "observed_follower_roster");
            MergeConcretePlayerSideAllies(
                result.allies,
                TFD::Actor::ResolveStandingPlayerSideActors(snapshot, false),
                player,
                observeInProgress ? "battle_observe_snapshot" : "combat_snapshot");
            MergeConcretePlayerSideAllies(
                result.allies,
                TFD::TeammateManager::CollectKnownTeammates(radius),
                player,
                observeInProgress ? "battle_observe_known_teammates" : "known_teammates");

            auto observedEnemies = TFD::Bleedout::DefeatGlue::CollectStandingEnemiesFromSnapshot();
            MergeUnique(result.threats, observedEnemies, player, false);
            if (observeInProgress || !result.allies.empty()) {
                MergeConcreteBleedoutThreats(
                    result.threats,
                    snapshotActors,
                    player,
                    result.allies,
                    observeInProgress ? "battle_observe_snapshot" : "combat_snapshot");
                MergeConcreteBleedoutThreats(
                    result.threats,
                    observedEnemies,
                    player,
                    result.allies,
                    "observed_enemy_roster");
            }
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

                const bool valid = pickAlly ?
                    (IsValidAlly(candidate, player) || IsAnchorAllyUsable(candidate, player)) :
                    IsValidThreat(candidate, player);
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

        std::vector<RE::Actor*> CollectCombatGroupAlliesForThreat(
            RE::Actor* threat,
            const std::vector<RE::Actor*>& knownAllies,
            RE::Actor* player)
        {
            std::vector<RE::Actor*> out{};
            if (!threat || !player || knownAllies.empty() || !threat->IsInCombat()) {
                return out;
            }

            auto* combatGroup = threat->GetCombatGroup();
            if (!combatGroup) {
                return out;
            }

            RE::BSReadLockGuard lock(combatGroup->lock);
            for (auto& combatTarget : combatGroup->targets) {
                auto targetSP = combatTarget.targetHandle.get();
                auto* candidate = targetSP.get();
                if (!candidate || candidate == player || candidate == threat) {
                    continue;
                }
                if (!ContainsActor(knownAllies, candidate)) {
                    continue;
                }
                if (IsValidAlly(candidate, player) || IsAnchorAllyUsable(candidate, player)) {
                    AddUnique(out, candidate);
                }
            }
            return out;
        }

        RE::Actor* PickCombatGroupAllyForThreat(RE::Actor* threat, const std::vector<RE::Actor*>& allies, RE::Actor* player)
        {
            auto combatGroupAllies = CollectCombatGroupAlliesForThreat(threat, allies, player);
            if (combatGroupAllies.empty()) {
                return nullptr;
            }

            auto* resolved = TFD::Bleedout::DefeatGlue::ResolveBleedRedirectTarget(threat);
            if (resolved && !ContainsActor(combatGroupAllies, resolved)) {
                resolved = nullptr;
            }
            auto* currentTarget = ResolveCurrentTarget(threat);
            return PickHighestHealthTarget(threat, combatGroupAllies, resolved, currentTarget, player, true);
        }

        void PruneAssistAnchorsLocked(Clock::time_point now, bool clearAll)
        {
            if (clearAll) {
                g_assistAnchors.clear();
                return;
            }

            // R26: while BattleObserve owns player-down resolution, a short scan gap
            // must not expire the last known ally anchor.  The observe owner is still
            // deciding win/loss, so CombatBehavior should hold the target bridge instead
            // of falling back to no_combat_group_ally pacify.
            if (IsAssistObserveHoldActiveLocked(now)) {
                return;
            }

            for (auto it = g_assistAnchors.begin(); it != g_assistAnchors.end();) {
                if (it->second.until <= now) {
                    it = g_assistAnchors.erase(it);
                }
                else {
                    ++it;
                }
            }
        }

        void StoreAssistAnchor(RE::Actor* threat, RE::Actor* ally, Clock::time_point now, const char* reason)
        {
            const auto threatId = ActorId(threat);
            const auto allyId = ActorId(ally);
            if (threatId == 0 || allyId == 0 || threat == ally) {
                return;
            }

            bool changed = false;
            bool observeHold = false;
            {
                std::scoped_lock lk(g_runtimeLock);
                ArmAssistObserveHoldLocked(threat, ally, now, reason ? reason : "assist_anchor_set");
                observeHold = IsAssistObserveHoldActiveLocked(now);
                auto& anchor = g_assistAnchors[threatId];
                auto oldSp = anchor.ally.get();
                auto* oldAlly = oldSp.get();
                const auto ttl = observeHold ? kAssistAnchorObserveHoldTtl : kAssistAnchorTtl;
                changed = ActorId(oldAlly) != allyId || anchor.until <= now || anchor.until < now + ttl;
                anchor.ally = ally->GetHandle();
                anchor.until = now + ttl;
            }

            if (changed) {
                spdlog::info(
                    "[TFD][CombatBehavior][R24] assist anchor set threat={:08X} ally={:08X} ttlMs={} reason={}",
                    threatId,
                    allyId,
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(observeHold ? kAssistAnchorObserveHoldTtl : kAssistAnchorTtl).count()),
                    reason ? reason : "unknown");
                if (observeHold) {
                    spdlog::info(
                        "[TFD][CombatBehavior][R27] anchor held by local observe hold threat={:08X} ally={:08X}",
                        threatId,
                        allyId);
                }
            }
        }

        RE::Actor* ResolveAssistAnchorForThreat(RE::Actor* threat, RE::Actor* player, Clock::time_point now)
        {
            const auto threatId = ActorId(threat);
            if (threatId == 0 || !player) {
                return nullptr;
            }

            RE::ActorHandle allyHandle{};
            {
                std::scoped_lock lk(g_runtimeLock);
                auto it = g_assistAnchors.find(threatId);
                if (it == g_assistAnchors.end()) {
                    return nullptr;
                }
                if (it->second.until <= now) {
                    if (IsAssistObserveHoldActiveLocked(now)) {
                        it->second.until = now + kAssistAnchorObserveHoldTtl;
                        spdlog::info(
                            "[TFD][CombatBehavior][R27] anchor ttl extended by observe hold threat={:08X} ttlMs={}",
                            threatId,
                            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(kAssistAnchorObserveHoldTtl).count()));
                    }
                    else {
                        g_assistAnchors.erase(it);
                        return nullptr;
                    }
                }
                allyHandle = it->second.ally;
            }

            auto allySp = allyHandle.get();
            auto* ally = allySp.get();
            if (!IsAnchorAllyUsable(ally, player)) {
                std::scoped_lock lk(g_runtimeLock);
                auto it = g_assistAnchors.find(threatId);
                if (it != g_assistAnchors.end()) {
                    auto existingSp = it->second.ally.get();
                    if (ActorId(existingSp.get()) == ActorId(ally)) {
                        g_assistAnchors.erase(it);
                    }
                }
                return nullptr;
            }

            spdlog::info(
                "[TFD][CombatBehavior][R24] anchor ally used threat={:08X} ally={:08X}",
                threatId,
                ActorId(ally));
            return ally;
        }

        bool HasActiveAssistAnchorForAlly(RE::Actor* ally, Clock::time_point now)
        {
            const auto allyId = ActorId(ally);
            if (allyId == 0) {
                return false;
            }

            std::scoped_lock lk(g_runtimeLock);
            for (auto it = g_assistAnchors.begin(); it != g_assistAnchors.end();) {
                if (it->second.until <= now && !IsAssistObserveHoldActiveLocked(now)) {
                    it = g_assistAnchors.erase(it);
                    continue;
                }
                auto allySp = it->second.ally.get();
                if (ActorId(allySp.get()) == allyId) {
                    return true;
                }
                ++it;
            }
            return false;
        }

        void RefreshAssistAnchors(const RuntimeActors& actors, RE::Actor* player, Clock::time_point now)
        {
            if (!player || actors.allies.empty() || actors.threats.empty()) {
                return;
            }
            for (auto* threat : actors.threats) {
                auto* resolved = TFD::Bleedout::DefeatGlue::ResolveBleedRedirectTarget(threat);
                auto* currentTarget = ResolveCurrentTarget(threat);
                auto* ally = PickHighestHealthTarget(threat, actors.allies, resolved, currentTarget, player, true);
                if (IsAnchorAllyUsable(ally, player)) {
                    StoreAssistAnchor(threat, ally, now, "refresh_current_scan");
                }
            }
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
            PruneFactionSuppressionsLocked(
                now,
                clearAll,
                clearAll ? "assist_window_closed" : "ttl_expired");
            PruneAssistAnchorsLocked(now, clearAll);
            if (clearAll) {
                g_pendingTargets.clear();
                g_roles.clear();
                g_lastCommitDispatch.clear();
                g_commitFlights.clear();
                g_settleCandidates.clear();
                g_lastSettleDispatch.clear();
                g_settleFlights.clear();
                ClearAssistObserveHoldLocked();
                return;
            }
            for (auto it = g_settleFlights.begin(); it != g_settleFlights.end();) {
                if (it->second.until <= now) {
                    it = g_settleFlights.erase(it);
                }
                else {
                    ++it;
                }
            }
            for (auto it = g_commitFlights.begin(); it != g_commitFlights.end();) {
                if (it->second.until <= now) {
                    it = g_commitFlights.erase(it);
                }
                else {
                    ++it;
                }
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

        void ReleaseFactionSuppressionLocked(BleedoutFactionSuppressEntry& entry, const char* reason)
        {
            ResolveBleedoutSuppressFactions();
            auto actorSp = entry.handle.get();
            auto* actor = actorSp.get();
            if (!actor) {
                return;
            }

            bool removedKnockOut = RemoveFactionIfAdded(actor, g_factionCache.knockOut, entry.addedKnockOut);
            bool removedRetarget = RemoveFactionIfAdded(actor, g_factionCache.retarget, entry.addedRetarget);
            bool removedThreat = RemoveFactionIfAdded(actor, g_factionCache.combatThreat, entry.addedThreat);
            bool removedAlly = RemoveFactionIfAdded(actor, g_factionCache.combatAlly, entry.addedAlly);
            bool removedPacify = RemoveFactionIfAdded(actor, g_factionCache.pacify, entry.addedPacify);

            bool restoredAggression = false;
            bool restoredAssistance = false;
            if (auto* owner = actor->AsActorValueOwner()) {
                if (entry.hasOriginalAggression) {
                    owner->SetActorValue(RE::ActorValue::kAggression, entry.originalAggression);
                    restoredAggression = true;
                }
                if (entry.hasOriginalAssistance) {
                    owner->SetActorValue(RE::ActorValue::kAssistance, entry.originalAssistance);
                    restoredAssistance = true;
                }
            }

            spdlog::info(
                "[TFD][CombatBehavior][R462A] faction suppress released actor={:08X} removedKO={} removedRetarget={} removedThreat={} removedAlly={} removedPacify={} restoredAgg={} restoredAssist={} reason={}",
                ActorId(actor),
                removedKnockOut ? 1 : 0,
                removedRetarget ? 1 : 0,
                removedThreat ? 1 : 0,
                removedAlly ? 1 : 0,
                removedPacify ? 1 : 0,
                restoredAggression ? 1 : 0,
                restoredAssistance ? 1 : 0,
                reason ? reason : "unknown");
        }

        void PruneFactionSuppressionsLocked(Clock::time_point now, bool clearAll, const char* reason)
        {
            for (auto it = g_bleedoutFactionSuppressions.begin(); it != g_bleedoutFactionSuppressions.end();) {
                if (!clearAll && it->second.until <= now && IsAssistObserveHoldActiveLocked(now) &&
                    (it->second.addedThreat || it->second.addedAlly || it->second.addedRetarget)) {
                    it->second.until = now + kFactionSuppressObserveHoldTtl;
                    spdlog::info(
                        "[TFD][CombatBehavior][R27] faction suppress ttl extended by observe hold actor={:08X} ttlMs={} reason={}",
                        it->second.actorId,
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(kFactionSuppressObserveHoldTtl).count()),
                        reason ? reason : "ttl_expired");
                    ++it;
                    continue;
                }
                if (clearAll || it->second.until <= now) {
                    ReleaseFactionSuppressionLocked(it->second, reason);
                    it = g_bleedoutFactionSuppressions.erase(it);
                }
                else {
                    ++it;
                }
            }
        }

        bool ApplyFactionSuppressRole(RE::Actor* actor, RuntimeRole role, Clock::time_point now, const char* reason)
        {
            if (!actor) {
                return false;
            }
            const auto id = ActorId(actor);
            if (id == 0) {
                return false;
            }
            ResolveBleedoutSuppressFactions();

            std::scoped_lock lk(g_runtimeLock);
            auto& entry = g_bleedoutFactionSuppressions[id];
            entry.actorId = id;
            entry.handle = actor->GetHandle();
            entry.until = now + (IsAssistObserveHoldActiveLocked(now) ? kFactionSuppressObserveHoldTtl : kFactionSuppressTtl);

            bool changed = false;
            if (role == RuntimeRole::None) {
                changed = AddFactionTracked(actor, g_factionCache.knockOut, entry.hadKnockOut, entry.addedKnockOut) || changed;
            }
            else if (role == RuntimeRole::BleedoutThreat) {
                changed = AddFactionTracked(actor, g_factionCache.retarget, entry.hadRetarget, entry.addedRetarget) || changed;
                changed = AddFactionTracked(actor, g_factionCache.combatThreat, entry.hadThreat, entry.addedThreat) || changed;
            }
            else if (role == RuntimeRole::BleedoutAlly) {
                changed = AddFactionTracked(actor, g_factionCache.combatAlly, entry.hadAlly, entry.addedAlly) || changed;
            }

            if (changed) {
                spdlog::info(
                    "[TFD][CombatBehavior][R462A] faction suppress role actor={:08X} role={} addedKO={} addedRetarget={} addedThreat={} addedAlly={} reason={}",
                    id,
                    role == RuntimeRole::BleedoutThreat ? "threat" : (role == RuntimeRole::BleedoutAlly ? "ally" : "knockout"),
                    entry.addedKnockOut ? 1 : 0,
                    entry.addedRetarget ? 1 : 0,
                    entry.addedThreat ? 1 : 0,
                    entry.addedAlly ? 1 : 0,
                    reason ? reason : "unknown");
            }

            return changed;
        }

        bool HardSuppressActorTargetingBleedoutPlayer(
            RE::Actor* actor,
            Clock::time_point now,
            const char* reason,
            bool addPacify,
            bool evaluatePackage)
        {
            if (!actor || actor == Player()) {
                return false;
            }
            const auto id = ActorId(actor);
            if (id == 0) {
                return false;
            }
            ResolveBleedoutSuppressFactions();

            bool shouldEval = false;
            {
                std::scoped_lock lk(g_runtimeLock);
                auto& entry = g_bleedoutFactionSuppressions[id];
                entry.actorId = id;
                entry.handle = actor->GetHandle();
                entry.until = now + (IsAssistObserveHoldActiveLocked(now) ? kFactionSuppressObserveHoldTtl : kFactionSuppressTtl);

                if (addPacify) {
                    (void)AddFactionTracked(actor, g_factionCache.pacify, entry.hadPacify, entry.addedPacify);
                    if (auto* owner = actor->AsActorValueOwner()) {
                        if (!entry.hasOriginalAggression) {
                            entry.originalAggression = owner->GetActorValue(RE::ActorValue::kAggression);
                            entry.hasOriginalAggression = true;
                        }
                        if (!entry.hasOriginalAssistance) {
                            entry.originalAssistance = owner->GetActorValue(RE::ActorValue::kAssistance);
                            entry.hasOriginalAssistance = true;
                        }
                        owner->SetActorValue(RE::ActorValue::kAggression, 0.0f);
                        owner->SetActorValue(RE::ActorValue::kAssistance, 0.0f);
                    }
                }

                if (evaluatePackage && ShouldLightEvaluate(actor) &&
                    (entry.lastPackageEval.time_since_epoch().count() == 0 || now >= entry.lastPackageEval + kHardSuppressEvalInterval)) {
                    entry.lastPackageEval = now;
                    shouldEval = true;
                }
            }

            const bool beforeCombat = actor->IsInCombat();
            const bool beforeTargetPlayer = ResolveCurrentTarget(actor) == Player();
            actor->StopAlarmOnActor();
            actor->StopCombat();
            if (shouldEval) {
                actor->EvaluatePackage(false, true);
                actor->EvaluatePackage(true, true);
            }

            spdlog::info(
                "[TFD][CombatBehavior][R462A] player bleedout target suppressed actor={:08X} beforeCombat={} beforeTargetPlayer={} addPacify={} eval={} reason={}",
                id,
                beforeCombat ? 1 : 0,
                beforeTargetPlayer ? 1 : 0,
                addPacify ? 1 : 0,
                shouldEval ? 1 : 0,
                reason ? reason : "unknown");
            return true;
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

        bool ShouldQueueCommitFlight(RE::Actor* actor, std::uint32_t targetId, const char* reason, Clock::time_point now)
        {
            const auto actorId = ActorId(actor);
            if (actorId == 0 || !reason || !reason[0]) {
                return false;
            }

            std::scoped_lock lk(g_runtimeLock);
            auto it = g_commitFlights.find(actorId);
            if (it != g_commitFlights.end() &&
                it->second.until > now &&
                it->second.targetId == targetId &&
                it->second.reason == reason) {
                spdlog::info(
                    "[TFD][CombatBehavior][R24] commit skipped inFlight actor={:08X} target={:08X} reason={}",
                    actorId,
                    targetId,
                    reason);
                return false;
            }

            g_commitFlights[actorId] = CommitFlight{ targetId, std::string(reason), now + kCommitFlightTtl };
            return true;
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

            const auto actorId = ActorId(actor);
            if (actorId == 0) {
                return;
            }
            const auto targetId = ActorId(GetPendingTarget(actor));
            const bool bleedoutPlayerFirewall = std::string_view(reason) == "enemy_off_bleedout_player";
            if (bleedoutPlayerFirewall) {
                std::scoped_lock lk(g_runtimeLock);
                auto it = g_lastCommitDispatch.find(actorId);
                if (!force && it != g_lastCommitDispatch.end() && now < it->second + kFirewallCommitDispatchInterval) {
                    return;
                }
                g_lastCommitDispatch[actorId] = now;
            }
            else if (!ShouldDispatchCommit(actor, now, force)) {
                return;
            }

            if (!ShouldQueueCommitFlight(actor, targetId, reason, now)) {
                return;
            }

            const bool queued = TFD::FlowController::QueueBridgeModEvent(
                "TFDCombatBehaviorCommit",
                actor,
                reason,
                0.0f);

            spdlog::info(
                "[TFD][CombatBehavior][CB10] bridge commit {} actor={:08X} target={:08X} reason={}",
                queued ? "queued" : "failed",
                actorId,
                targetId,
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

        bool ShouldQueueSettleFlight(RE::Actor* actor, const char* reason, Clock::time_point now, bool force)
        {
            const auto actorId = ActorId(actor);
            if (actorId == 0 || !reason || !reason[0]) {
                return false;
            }
            if (force) {
                std::scoped_lock lk(g_runtimeLock);
                g_settleFlights[actorId] = SettleFlight{ std::string(reason), now + kSettleFlightTtl };
                return true;
            }

            std::scoped_lock lk(g_runtimeLock);
            auto it = g_settleFlights.find(actorId);
            if (it != g_settleFlights.end() &&
                it->second.until > now &&
                it->second.reason == reason) {
                spdlog::info(
                    "[TFD][CombatBehavior][R25] settle skipped inFlight actor={:08X} reason={}",
                    actorId,
                    reason);
                return false;
            }

            g_settleFlights[actorId] = SettleFlight{ std::string(reason), now + kSettleFlightTtl };
            return true;
        }

        bool ShouldGateNoThreatSettle(RE::Actor* actor, const char* reason, Clock::time_point now, bool force)
        {
            if (force || !IsNoThreatSettleReason(reason ? std::string_view(reason) : std::string_view{})) {
                return false;
            }

            auto* player = Player();
            if (!player || !actor) {
                return false;
            }

            if (IsAssistObserveHoldActive(now)) {
                spdlog::info(
                    "[TFD][CombatBehavior][R27] settle held by local observe hold actor={:08X} reason={}",
                    ActorId(actor),
                    reason ? reason : "unknown");
                return true;
            }

            auto* currentTarget = ResolveCurrentTarget(actor);
            if (IsActiveBleedoutThreatActor(currentTarget, player)) {
                spdlog::info(
                    "[TFD][CombatBehavior][R25] settle gated actor={:08X} target={:08X} reason={} gate=target_still_active",
                    ActorId(actor),
                    ActorId(currentTarget),
                    reason ? reason : "unknown");
                return true;
            }

            auto* pendingTarget = GetPendingTarget(actor);
            if (IsActiveBleedoutThreatActor(pendingTarget, player)) {
                spdlog::info(
                    "[TFD][CombatBehavior][R25] settle gated actor={:08X} target={:08X} reason={} gate=pending_target_still_active",
                    ActorId(actor),
                    ActorId(pendingTarget),
                    reason ? reason : "unknown");
                return true;
            }

            if (HasActiveAssistAnchorForAlly(actor, now)) {
                spdlog::info(
                    "[TFD][CombatBehavior][R25] settle gated actor={:08X} reason={} gate=assist_anchor_active",
                    ActorId(actor),
                    reason ? reason : "unknown");
                return true;
            }

            return false;
        }

        void QueueSettleActorIfDue(RE::Actor* actor, const char* reason, Clock::time_point now, bool force)
        {
            if (!actor || !reason || !IsPlayerSideActor(actor, Player())) {
                return;
            }
            if (ShouldGateNoThreatSettle(actor, reason, now, force)) {
                return;
            }
            if (!ShouldDispatchSettle(actor, now, force)) {
                return;
            }
            if (!ShouldQueueSettleFlight(actor, reason, now, force)) {
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

        bool QueueCombatPair(RE::Actor* actor, RE::Actor* target, RuntimeRole role, const char* reason, bool pairTarget)
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
            if (pairTarget && inverse != RuntimeRole::None) {
                StorePendingTarget(target, actor, inverse, now);
            }
            if (role == RuntimeRole::BleedoutThreat) {
                StoreAssistAnchor(actor, target, now, reason ? reason : "queue_pair_threat");
            }
            else if (role == RuntimeRole::BleedoutAlly) {
                StoreAssistAnchor(target, actor, now, reason ? reason : "queue_pair_ally");
            }

            const char* dispatchReason = reason && reason[0] ? reason : "bleedout_assist_hint";
            if (changed) {
                spdlog::info(
                    "[TFD][CombatBehavior][R431B] pending actor={:08X} old={:08X} new={:08X} role={} reason={} paired={}",
                    ActorId(actor),
                    oldPending,
                    newPending,
                    role == RuntimeRole::BleedoutThreat ? "threat" : "ally",
                    dispatchReason,
                    pairTarget ? 1 : 0);
            }

            // R431B: enemy_off_bleedout_player is one-way. It redirects the hostile observer
            // away from the bleedout player, but it must not force the standing teammate to
            // StartCombat the same actor. That paired commit survived across Victory recruit
            // transition and made old teammates attack newly recruited actors.
            QueuePapyrusCommitIfDue(actor, dispatchReason, now, changed || std::string_view(dispatchReason) == "enemy_off_bleedout_player");
            if (pairTarget) {
                QueuePapyrusCommitIfDue(target, role == RuntimeRole::BleedoutThreat ? "paired_ally_commit" : "paired_threat_commit", now, changed);
            }
            return changed;
        }

        void MarkCurrentPairs(const RuntimeActors& actors, RE::Actor* player, Clock::time_point now)
        {
            for (auto* threat : actors.threats) {
                auto* target = ResolveCurrentTarget(threat);
                if (IsValidAlly(target, player) || IsAnchorAllyUsable(target, player)) {
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

        bool AllyTargetWasInvalidatedByVictoryOrRecruit(RE::Actor* ally, RE::Actor* currentTarget, RE::Actor* player)
        {
            if (!ally || !currentTarget || !player || ally == player || currentTarget == player) {
                return false;
            }
            if (!IsValidAlly(ally, player)) {
                return false;
            }
            if (IsVictoryOrRecruitTransitionActor(currentTarget)) {
                return true;
            }
            if (IsPlayerSideActor(currentTarget, player)) {
                return true;
            }
            return false;
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
            (void)ApplyFactionSuppressRole(player, RuntimeRole::None, now, "player_bleedout_knockout");
            for (auto* ally : actors.allies) {
                RememberSettleCandidate(ally);
                (void)ApplyFactionSuppressRole(ally, RuntimeRole::BleedoutAlly, now, "player_bleedout_standing_ally");
            }
            for (auto* threat : actors.threats) {
                (void)ApplyFactionSuppressRole(threat, RuntimeRole::BleedoutThreat, now, "player_bleedout_standing_threat");
            }

            if (actors.threats.empty()) {
                if (IsAssistObserveHoldActive(now)) {
                    spdlog::info(
                        "[TFD][CombatBehavior][R27] no-threat settle held allies={} threats={} reason=local_observe_hold",
                        static_cast<unsigned int>(actors.allies.size()),
                        static_cast<unsigned int>(actors.threats.size()));
                    PruneRuntimeState(now, false);
                    return;
                }
                if (!actors.allies.empty()) {
                    for (auto* ally : actors.allies) {
                        QueueSettleActorIfDue(ally, "no_active_bleedout_threat", now, false);
                    }
                }
                if (g_bleedoutRetargetActive.exchange(false, std::memory_order_acq_rel)) {
                    spdlog::info(
                        "[TFD][CombatBehavior][CB10] bleedout retarget inactive allies={} threats={}",
                        static_cast<unsigned int>(actors.allies.size()),
                        static_cast<unsigned int>(actors.threats.size()));
                    if (!actors.allies.empty()) {
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
            RefreshAssistAnchors(actors, player, now);

            for (auto* threat : actors.threats) {
                auto* currentTarget = ResolveCurrentTarget(threat);
                if (currentTarget != player) {
                    continue;
                }

                TFD::Bleedout::DefeatGlue::NoteEnemyTargetingPlayer(threat);
                auto* resolved = TFD::Bleedout::DefeatGlue::ResolveBleedRedirectTarget(threat);
                auto* replacement = PickCombatGroupAllyForThreat(threat, actors.allies, player);
                const char* suppressReason = "enemy_off_bleedout_player_redirect";
                bool usedAnchor = false;
                if (!IsValidAlly(replacement, player) && !IsAnchorAllyUsable(replacement, player)) {
                    if (IsAnchorAllyUsable(resolved, player)) {
                        replacement = resolved;
                        suppressReason = "enemy_off_bleedout_player_resolved_redirect";
                    }
                }
                if (!IsValidAlly(replacement, player) && !IsAnchorAllyUsable(replacement, player)) {
                    replacement = PickHighestHealthTarget(threat, actors.allies, resolved, nullptr, player, true);
                    if (IsAnchorAllyUsable(replacement, player)) {
                        suppressReason = "enemy_off_bleedout_player_known_ally_redirect";
                    }
                }
                if (!IsValidAlly(replacement, player) && !IsAnchorAllyUsable(replacement, player)) {
                    replacement = ResolveAssistAnchorForThreat(threat, player, now);
                    usedAnchor = replacement != nullptr;
                    if (usedAnchor) {
                        suppressReason = "enemy_off_bleedout_player_anchor_redirect";
                    }
                }
                if ((IsValidAlly(replacement, player) || IsAnchorAllyUsable(replacement, player)) && ActorId(replacement) != ActorId(currentTarget)) {
                    (void)HardSuppressActorTargetingBleedoutPlayer(
                        threat,
                        now,
                        suppressReason,
                        false,
                        false);
                    (void)QueueCombatPair(threat, replacement, RuntimeRole::BleedoutThreat, "enemy_off_bleedout_player", false);
                }
                else {
                    if (IsBattleObserveInProgress() || IsAssistObserveHoldActive(now)) {
                        (void)HardSuppressActorTargetingBleedoutPlayer(
                            threat,
                            now,
                            "enemy_off_bleedout_player_waiting_for_ally",
                            false,
                            false);
                        spdlog::info(
                            "[TFD][CombatBehavior][R31] no-target fallback soft-held actor={:08X} currentTarget={:08X} allies={} reason=battle_observe_waiting_for_ally",
                            ActorId(threat),
                            ActorId(currentTarget),
                            static_cast<unsigned int>(actors.allies.size()));
                        continue;
                    }

                    (void)HardSuppressActorTargetingBleedoutPlayer(
                        threat,
                        now,
                        "enemy_off_bleedout_player_no_target",
                        true,
                        true);
                    spdlog::info(
                        "[TFD][CombatBehavior][R462A] enemy_off_bleedout_player hard-suppressed actor={:08X} currentTarget={:08X} reason=no_combat_group_ally",
                        ActorId(threat),
                        ActorId(currentTarget));
                }
            }

            for (auto* ally : actors.allies) {
                auto* currentTarget = ResolveCurrentTarget(ally);
                if (AllyTargetWasInvalidatedByVictoryOrRecruit(ally, currentTarget, player)) {
                    QueueSettleActorIfDue(ally, "victory_or_recruit_target_invalidated", now, true);
                    spdlog::info(
                        "[TFD][CombatBehavior][R431B] ally settle queued actor={:08X} oldTarget={:08X} reason=victory_or_recruit_target_invalidated",
                        ActorId(ally),
                        ActorId(currentTarget));
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

        spdlog::info("[TFD][CombatBehavior] installed R32 battle-observe hard proof + R462A suppressor");
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

        spdlog::info("[TFD][CombatBehavior] shutdown R32 battle-observe hard proof + R462A suppressor");
    }

    void ResetForLoad()
    {
        g_bleedoutRetargetActive.store(false, std::memory_order_release);
        PruneRuntimeState(Clock::now(), true);
        g_factionCache = FactionRuntimeCache{};
        spdlog::info("[TFD][CombatBehavior] reset runtime state R32 battle-observe hard proof + R462A suppressor");
    }

    bool PapyrusReleaseBleedoutPleasureFailedFightCrowd(
        RE::StaticFunctionTag*,
        RE::Actor* actor,
        RE::BSFixedString reason)
    {
        auto* player = Player();
        const char* why = reason.c_str() && reason.c_str()[0] ? reason.c_str() : "bleedout_pleasure_failed_fight_crowd";
        if (!actor || !player || actor == player) {
            spdlog::warn(
                "[TFD][CombatBehavior][R435A] bleedout PF fight crowd wake rejected actor={:08X} player={:08X} reason={}",
                ActorId(actor),
                ActorId(player),
                why);
            return false;
        }
        if (actor->IsDead() || actor->IsDisabled()) {
            spdlog::info(
                "[TFD][CombatBehavior][R435A] bleedout PF fight crowd wake skipped invalid actor={:08X} dead={} disabled={} reason={}",
                ActorId(actor),
                actor->IsDead() ? 1 : 0,
                actor->IsDisabled() ? 1 : 0,
                why);
            return false;
        }
        if (TFD::TeammateManager::IsPlayerSideTeammateActor(actor)) {
            spdlog::info(
                "[TFD][CombatBehavior][R435A] bleedout PF fight crowd wake skipped player-side actor={:08X} reason={}",
                ActorId(actor),
                why);
            return false;
        }

        const bool beforeSuppressed = TFD::HostilityController::IsActorTemporarilySuppressed(actor);
        const bool beforeCombat = actor->IsInCombat();
        const bool beforeWeapon = actor->IsWeaponDrawn();
        const bool beforeTargetPlayer = ResolveCurrentTarget(actor) == player;
        const bool brokeOwner = TFD::HostilityController::BreakPassiveOwnershipForFightChoice(actor, player, why);
        TFD::Bleedout::ClearBleedoutDialogueFactionForActor(actor, why);
        TFD::Actor::Ops::RemoveReleaseFollowGraceFromActorOnlyForFightChoice(actor, why);
        TFD::HostilityController::QueueDetectionAndCombatRefresh(
            actor,
            player,
            TFD::HostilityController::ReleaseReason::FightChoice,
            true);

        spdlog::info(
            "[TFD][CombatBehavior][R435A] bleedout PF fight crowd wake actor={:08X} player={:08X} beforeSuppressed={} beforeCombat={} beforeWeapon={} beforeTargetPlayer={} brokeOwner={} afterSuppressed={} afterCombat={} afterWeapon={} afterTargetPlayer={} reason={}",
            ActorId(actor),
            ActorId(player),
            beforeSuppressed ? 1 : 0,
            beforeCombat ? 1 : 0,
            beforeWeapon ? 1 : 0,
            beforeTargetPlayer ? 1 : 0,
            brokeOwner ? 1 : 0,
            TFD::HostilityController::IsActorTemporarilySuppressed(actor) ? 1 : 0,
            actor->IsInCombat() ? 1 : 0,
            actor->IsWeaponDrawn() ? 1 : 0,
            ResolveCurrentTarget(actor) == player ? 1 : 0,
            why);
        return true;
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

        a_vm->RegisterFunction(
            "ReleaseBleedoutPleasureFailedFightCrowd",
            "TFDCombatBehaviorNative",
            PapyrusReleaseBleedoutPleasureFailedFightCrowd);

        spdlog::info("[TFD][CombatBehavior] Papyrus natives registered R32 battle-observe hard proof + R462A suppressor");
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
