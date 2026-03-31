#include "TFDPacify.h"

#include "TFDActorScan.h"
#include "TFDDefeatMonitor.h"
#include "TFDSettings.h"
#include "TFDTargetClassifier.h"
#include "TFDTameBait.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>
#include <SKSE/SKSE.h>

namespace TFD::Pacify
{
    namespace
    {
        bool ForceRehostile(RE::Actor* actor, RE::Actor* player, ReleaseReason reason, bool drawWeapon);
        struct TruceState
        {
            bool spent{ false };
            bool betrayed{ false };

            TruceState() = default;
        };

        struct RehostileRequest
        {
            RE::FormID actorId{ 0 };
            RE::FormID playerId{ 0 };
            RE::FormID sessionId{ 0 };
            ReleaseReason reason{ ReleaseReason::Generic };
            double nextAttemptSec{ 0.0 };
            double expireSec{ 0.0 };
            std::uint8_t attemptsRemaining{ 0 };
            bool drawWeapon{ true };
        };

        using Clock = std::chrono::steady_clock;

        std::unordered_map<RE::FormID, Entry> g_entries;
        std::unordered_map<RE::FormID, Session> g_sessions;
        std::unordered_map<RE::FormID, TruceState> g_truceState;
        std::unordered_map<RE::FormID, RehostileRequest> g_rehostileRequests;
        RE::FormID g_nextSessionId = 1;

        const char* GetAssignEventName(Mode mode)
        {
            switch (mode) {
            case Mode::Tame:
                return "TFDTameAssign";
            case Mode::TrucePreCombat:
                return "TFDPreCombatAssign";
            case Mode::TruceInCombat:
                return "TFDTruceAssign";
            default:
                return nullptr;
            }
        }

        const char* GetUnassignEventName(Mode mode)
        {
            switch (mode) {
            case Mode::Tame:
                return "TFDTameUnassign";
            case Mode::TrucePreCombat:
                return "TFDPreCombatClear";
            case Mode::TruceInCombat:
                return "TFDTruceUnassign";
            default:
                return nullptr;
            }
        }

        const char* GetSupplementalAssignEventName(Mode mode)
        {
            switch (mode) {
            case Mode::TruceInCombat:
                return "TFDInCombatAssign";
            default:
                return nullptr;
            }
        }

        const char* GetSupplementalUnassignEventName(Mode mode)
        {
            switch (mode) {
            case Mode::TruceInCombat:
                return "TFDInCombatClear";
            default:
                return nullptr;
            }
        }

        constexpr const char* kCreatureTeammateAssignEvent = "TFDCreatureTeammateAssign";
        constexpr const char* kCreatureTeammateUnassignEvent = "TFDCreatureTeammateUnassign";
        constexpr double kCompanionInitialHours = 3.0;
        constexpr double kCompanionExtendHours = 3.0;
        constexpr double kCompanionMaxHours = 9.0;
        constexpr double kCompanionFeedHours = 3.0;
        constexpr std::int32_t kCalmFeedCost = 1;
        constexpr std::int32_t kCompanionFeedCost = 2;

        void SendModEvent(const char* eventName, RE::Actor* sender)
        {
            if (!eventName) {
                return;
            }

            auto* src = SKSE::GetModCallbackEventSource();
            if (!src) {
                return;
            }

            SKSE::ModCallbackEvent e(eventName, "", 0.0f, sender);
            src->SendEvent(&e);
        }


        double PacifyNowSec()
        {
            static const auto t0 = Clock::now();
            return std::chrono::duration<double>(Clock::now() - t0).count();
        }

        double CurrentGameDays()
        {
            auto* calendar = RE::Calendar::GetSingleton();
            return calendar ? static_cast<double>(calendar->rawDaysPassed) : 0.0;
        }

        constexpr double kTameDurationSec = 60.0;
        constexpr double kMaxTameTotalSec = 180.0;
        constexpr double kTruceHiddenFailsafeSec = 120.0;

        constexpr double kPacifyApplyIntervalSec = 0.25;
        constexpr double kPackageEvalIntervalSec = 1.0;

        constexpr double kRehostileRetryDelaySec = 0.20;
        constexpr double kRehostileRetryExtendSec = 0.35;
        constexpr double kRehostileRetryLifetimeSec = 1.60;
        constexpr std::uint8_t kRehostileRetryCount = 4;

        constexpr double kArmedGraceSec = 1.25;
        constexpr double kArmedDebounceSec = 0.50;
        constexpr double kTooFarDebounceSec = 1.25;
        constexpr double kTameStartleDebounceSec = 0.35;

        constexpr float kTameStartleDistance = 180.0f;
        constexpr float kTameStartleRushSpeedPerSec = 260.0f;
        constexpr float kTruceNonDialogueMaxDistance = 2200.0f;
        constexpr double kTameStartleGraceSec = 2.5;
        constexpr double kTameStartleWindowSec = 5.0;
        constexpr std::size_t kCrowdAliasCap = 10;
        constexpr float kTruceActiveCombatRadius = 3500.0f;
        constexpr float kTrucePrimaryLinkRadius = 2400.0f;

        bool IsSessionSpaceCompatible(RE::Actor* actor, RE::Actor* player, RE::Actor* primaryTarget);
        RE::Actor* ResolveCurrentCombatTarget(RE::Actor* actor);
        bool IsEnemyToPlayer(RE::Actor* player, RE::Actor* actor);
        bool IsActorStillValid(RE::Actor* actor);
        void PulseGlobalDetection(const char* reason);
        bool IsActorBoundToDifferentActiveTameSession(RE::FormID actorId, RE::FormID targetSessionId);

        bool IsFiniteDurationMode(Mode mode)
        {
            return mode == Mode::Tame;
        }

        double ResolveSessionDurationSec(Mode mode, double requestedDurationSec)
        {
            if (mode == Mode::Tame) {
                return requestedDurationSec > 0.0 ? requestedDurationSec : kTameDurationSec;
            }
            return 0.0;
        }

        double ClampTameEndTime(double nowSec, double endTimeSec)
        {
            return (std::min)(endTimeSec, nowSec + kMaxTameTotalSec);
        }

        RE::Actor* ResolveCurrentCombatTarget(RE::Actor* actor);
        bool IsEnemyToPlayer(RE::Actor* player, RE::Actor* actor);
        bool IsActorBoundToDifferentActiveTameSession(RE::FormID actorId, RE::FormID targetSessionId);
        bool ForceRehostile(RE::Actor* actor, RE::Actor* player, ReleaseReason reason, bool drawWeapon)
        {
            if (!IsActorStillValid(actor) || !IsActorStillValid(player)) {
                return false;
            }

            if (TFD::DefeatMonitor::IsDefeatedEnemyKnocked(actor)) {
                spdlog::info(
                    "TFDPacify: rehostile skipped actor={:08X} player={:08X} reason={} defeated_knock=1",
                    actor->GetFormID(),
                    player->GetFormID(),
                    ToString(reason));
                return false;
            }

            if (g_entries.find(actor->GetFormID()) != g_entries.end()) {
                return false;
            }

            if (auto* process = RE::ProcessLists::GetSingleton()) {
                process->ClearCachedFactionFightReactions();
            }

            if (reason == ReleaseReason::DialogueClosed) {
                PulseGlobalDetection(ToString(reason));
            }

            actor->SetBeenAttacked(true);
            player->SetBeenAttacked(true);

            (void)actor->RequestDetectionLevel(player, RE::DETECTION_PRIORITY::kCritical);
            (void)player->RequestDetectionLevel(actor, RE::DETECTION_PRIORITY::kCritical);

            if (drawWeapon && !actor->IsWeaponDrawn()) {
                actor->DrawWeaponMagicHands(true);
            }

            actor->EvaluatePackage(false, true);
            actor->EvaluatePackage(true, true);
            actor->UpdateCombat();
            player->UpdateCombat();

            const auto* combatTarget = ResolveCurrentCombatTarget(actor);
            const bool inCombat = actor->IsInCombat();
            const bool targetingPlayer = combatTarget && combatTarget->GetFormID() == player->GetFormID();
            const bool hostile = IsEnemyToPlayer(player, actor);

            spdlog::info(
                "TFDPacify: rehostile actor={:08X} player={:08X} reason={} hostile={} inCombat={} targetingPlayer={}",
                actor->GetFormID(),
                player->GetFormID(),
                ToString(reason),
                hostile ? 1 : 0,
                inCombat ? 1 : 0,
                targetingPlayer ? 1 : 0);

            return inCombat || targetingPlayer;
        }
        void QueueRehostileRetry(RE::Actor* actor, RE::Actor* player, RE::FormID sessionId, ReleaseReason reason, double nowSec, bool drawWeapon);
        void ProcessRehostileRetries(double nowSec);

        RE::Actor* ResolveActor(RE::FormID actorId)
        {
            if (actorId == 0) {
                return nullptr;
            }

            return RE::TESForm::LookupByID<RE::Actor>(actorId);
        }

        std::size_t SendModEventToActors(const char* eventName, const std::vector<RE::FormID>& actorIds)
        {
            if (!eventName) {
                return 0;
            }

            std::size_t sent = 0;
            for (RE::FormID actorId : actorIds) {
                if (auto* actor = ResolveActor(actorId)) {
                    SendModEvent(eventName, actor);
                    ++sent;
                }
            }
            return sent;
        }

        bool IsDialogueCapableTruceEventActor(RE::Actor* actor)
        {
            if (!actor) {
                return false;
            }

            return TFD::TargetClassifier::IsNegotiable(actor);
        }

        std::vector<RE::FormID> SelectTruceEventTargets(
            const std::vector<RE::FormID>& actorIds,
            RE::FormID primaryTargetId)
        {
            if (actorIds.empty()) {
                return {};
            }

            std::vector<RE::FormID> result;
            result.reserve(actorIds.size());

            auto addUnique = [&](RE::FormID actorId) {
                if (actorId == 0) {
                    return;
                }
                if (std::find(result.begin(), result.end(), actorId) == result.end()) {
                    result.push_back(actorId);
                }
                };

            addUnique(primaryTargetId);

            for (auto actorId : actorIds) {
                if (actorId == primaryTargetId) {
                    continue;
                }

                auto* actor = ResolveActor(actorId);
                if (!IsDialogueCapableTruceEventActor(actor)) {
                    continue;
                }

                addUnique(actorId);
                if (result.size() >= kCrowdAliasCap) {
                    break;
                }
            }

            return result;
        }

        std::vector<RE::FormID> SelectCrowdEventTargets(
            const std::vector<RE::FormID>& actorIds,
            RE::Actor* player,
            RE::FormID primaryTargetId)
        {
            if (actorIds.empty()) {
                return {};
            }

            struct Candidate
            {
                RE::FormID actorId{ 0 };
                bool isPrimary{ false };
                bool targetingPlayer{ false };
                float distanceToPlayer{ 999999.0f };
            };

            std::vector<Candidate> candidates;
            candidates.reserve(actorIds.size());
            for (auto actorId : actorIds) {
                auto* actor = ResolveActor(actorId);
                if (!actor) {
                    continue;
                }

                Candidate c;
                c.actorId = actorId;
                c.isPrimary = actorId == primaryTargetId;
                c.targetingPlayer = player && IsEnemyToPlayer(player, actor);
                if (player) {
                    c.distanceToPlayer = actor->GetPosition().GetDistance(player->GetPosition());
                }
                candidates.push_back(c);
            }

            std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
                if (a.isPrimary != b.isPrimary) {
                    return a.isPrimary > b.isPrimary;
                }
                if (a.targetingPlayer != b.targetingPlayer) {
                    return a.targetingPlayer > b.targetingPlayer;
                }
                if (a.distanceToPlayer != b.distanceToPlayer) {
                    return a.distanceToPlayer < b.distanceToPlayer;
                }
                return a.actorId < b.actorId;
                });

            std::vector<RE::FormID> result;
            result.reserve((std::min)(candidates.size(), kCrowdAliasCap));
            for (const auto& c : candidates) {
                if (result.size() >= kCrowdAliasCap) {
                    break;
                }
                result.push_back(c.actorId);
            }
            return result;
        }

        std::vector<RE::FormID> BuildTruceInCombatMemberIds(RE::Actor* player, RE::Actor* primaryTarget, float scanRadius, std::size_t& cellBubbleCount, std::size_t& truceClusterCount);

        struct TruceCandidate
        {
            RE::Actor* actor{ nullptr };
            RE::FormID actorId{ 0 };
            bool isPrimary{ false };
            bool targetingPlayer{ false };
            float distanceToPlayer{ 999999.0f };
            float distanceToPrimary{ 999999.0f };
        };

        bool IsEligibleActiveTruceCombatant(
            RE::Actor* actor,
            RE::Actor* player,
            RE::Actor* primaryTarget,
            const TFD::ActorScan::Entry& scanEntry)
        {
            (void)scanEntry;
            if (!IsActorStillValid(actor) || !player || !primaryTarget) {
                return false;
            }

            if (actor->GetFormID() == player->GetFormID()) {
                return false;
            }

            if (!actor->Is3DLoaded()) {
                return false;
            }

            if (actor->GetFormID() == primaryTarget->GetFormID()) {
                return true;
            }

            if (!IsEnemyToPlayer(player, actor)) {
                return false;
            }

            auto* playerCell = player->GetParentCell();
            auto* actorCell = actor->GetParentCell();
            auto* primaryCell = primaryTarget->GetParentCell();
            const bool sameCell = playerCell && actorCell && primaryCell && actorCell == playerCell && primaryCell == playerCell;

            auto* playerWs = player->GetWorldspace();
            auto* actorWs = actor->GetWorldspace();
            auto* primaryWs = primaryTarget->GetWorldspace();
            const bool sameWorldspace = playerWs && actorWs && primaryWs && actorWs == playerWs && primaryWs == playerWs;

            if (!sameCell && !sameWorldspace) {
                return false;
            }

            const float distToPlayer = actor->GetPosition().GetDistance(player->GetPosition());
            const float distToPrimary = actor->GetPosition().GetDistance(primaryTarget->GetPosition());
            if (distToPlayer > kTruceActiveCombatRadius && distToPrimary > kTrucePrimaryLinkRadius) {
                return false;
            }

            auto* combatTarget = ResolveCurrentCombatTarget(actor);
            const bool targetingPlayer = combatTarget && combatTarget->GetFormID() == player->GetFormID();
            return targetingPlayer || distToPlayer <= 1800.0f || distToPrimary <= 1400.0f;
        }

        std::vector<RE::FormID> BuildTruceInCombatMemberIds(
            RE::Actor* player,
            RE::Actor* primaryTarget,
            float scanRadius,
            std::size_t& cellBubbleCount,
            std::size_t& truceClusterCount)
        {
            std::vector<TruceCandidate> candidates;
            TFD::ActorScan::Rescan(scanRadius, false);
            const auto count = TFD::ActorScan::GetCount();
            candidates.reserve(count);

            auto* playerCell = player ? player->GetParentCell() : nullptr;
            auto* primaryCell = primaryTarget ? primaryTarget->GetParentCell() : nullptr;

            for (int i = 0; i < count; ++i) {
                auto scanEntry = TFD::ActorScan::GetEntry(i);
                auto* actor = TFD::ActorScan::GetActor(i);
                if (!IsEligibleActiveTruceCombatant(actor, player, primaryTarget, scanEntry)) {
                    continue;
                }

                TruceCandidate c;
                c.actor = actor;
                c.actorId = actor->GetFormID();
                c.isPrimary = c.actorId == primaryTarget->GetFormID();
                auto* combatTarget = ResolveCurrentCombatTarget(actor);
                c.targetingPlayer = combatTarget && combatTarget->GetFormID() == player->GetFormID();
                c.distanceToPlayer = actor->GetPosition().GetDistance(player->GetPosition());
                c.distanceToPrimary = actor->GetPosition().GetDistance(primaryTarget->GetPosition());
                candidates.push_back(c);
            }

            std::stable_sort(candidates.begin(), candidates.end(), [](const TruceCandidate& a, const TruceCandidate& b) {
                if (a.isPrimary != b.isPrimary) {
                    return a.isPrimary > b.isPrimary;
                }
                if (a.targetingPlayer != b.targetingPlayer) {
                    return a.targetingPlayer > b.targetingPlayer;
                }
                if (a.distanceToPlayer != b.distanceToPlayer) {
                    return a.distanceToPlayer < b.distanceToPlayer;
                }
                if (a.distanceToPrimary != b.distanceToPrimary) {
                    return a.distanceToPrimary < b.distanceToPrimary;
                }
                return a.actorId < b.actorId;
                });

            std::vector<RE::FormID> result;
            result.reserve((std::min)(candidates.size(), kCrowdAliasCap));
            for (const auto& c : candidates) {
                if (result.size() >= kCrowdAliasCap) {
                    break;
                }
                result.push_back(c.actorId);
                if (playerCell && primaryCell && c.actor && c.actor->GetParentCell() == playerCell && primaryCell == playerCell) {
                    ++cellBubbleCount;
                }
                else {
                    ++truceClusterCount;
                }
            }
            return result;
        }

        bool IsActorStillValid(RE::Actor* actor)
        {
            if (!actor) {
                return false;
            }

            if (actor->IsDead()) {
                return false;
            }

            if (actor->IsDisabled()) {
                return false;
            }

            return true;
        }

        bool IsTruceMode(Mode mode)
        {
            return mode == Mode::TrucePreCombat || mode == Mode::TruceInCombat;
        }

        constexpr float kLocalHostileSplashRadiusMin = 1000.0f;
        constexpr float kLocalHostileSplashRadiusMax = 1800.0f;
        constexpr std::size_t kTamePackMaxMembers = 6;

        float GetLocalHostileSplashRadius()
        {
            const float settingsRadius = TFD::Settings::GetSweepRadius();
            return std::clamp(settingsRadius, kLocalHostileSplashRadiusMin, kLocalHostileSplashRadiusMax);
        }

        RE::TESRace* GetActorRace(RE::Actor* actor)
        {
            if (!actor) {
                return nullptr;
            }

            auto* base = actor->GetActorBase();
            return base ? base->GetRace() : nullptr;
        }

        bool IsSameTamePackSpecies(RE::Actor* actor, RE::Actor* primaryTarget)
        {
            auto* actorRace = GetActorRace(actor);
            auto* primaryRace = GetActorRace(primaryTarget);
            if (!actorRace || !primaryRace) {
                return false;
            }

            return actorRace->GetFormID() == primaryRace->GetFormID();
        }

        bool SharesPrimaryCombatAnchor(RE::Actor* actor, RE::Actor* player, RE::Actor* primaryTarget)
        {
            if (!actor || !player || !primaryTarget) {
                return false;
            }

            auto* primaryCombatTarget = ResolveCurrentCombatTarget(primaryTarget);
            if (!primaryCombatTarget) {
                return true;
            }

            auto* actorCombatTarget = ResolveCurrentCombatTarget(actor);
            if (!actorCombatTarget) {
                return true;
            }

            const auto playerId = player->GetFormID();
            const auto primaryAnchorId = primaryCombatTarget->GetFormID();
            const auto actorAnchorId = actorCombatTarget->GetFormID();

            return actorAnchorId == primaryAnchorId || actorAnchorId == playerId;
        }


        bool IsEligibleLocalSplashActor(
            RE::Actor* actor,
            RE::Actor* player,
            RE::Actor* primaryTarget,
            const TFD::ActorScan::Entry& scanEntry,
            float radius)
        {
            if (!IsActorStillValid(actor) || !player || !primaryTarget) {
                return false;
            }

            if (actor->GetFormID() == player->GetFormID()) {
                return false;
            }

            if (actor->GetFormID() == primaryTarget->GetFormID()) {
                return false;
            }

            if (!actor->Is3DLoaded()) {
                return false;
            }

            auto* pCell = player->GetParentCell();
            auto* aCell = actor->GetParentCell();
            auto* tCell = primaryTarget->GetParentCell();
            const bool sameCell = pCell && aCell && tCell && aCell == pCell && tCell == pCell;

            auto* pWs = player->GetWorldspace();
            auto* aWs = actor->GetWorldspace();
            auto* tWs = primaryTarget->GetWorldspace();
            const bool sameWorldspace = pWs && aWs && tWs && aWs == pWs && tWs == pWs;

            if (!sameCell && !sameWorldspace) {
                return false;
            }

            if (!IsEnemyToPlayer(player, actor)) {
                return false;
            }

            const float distToPrimary = actor->GetPosition().GetDistance(primaryTarget->GetPosition());
            const float distToPlayer = actor->GetPosition().GetDistance(player->GetPosition());
            return distToPrimary <= radius || distToPlayer <= radius;
        }

        std::vector<RE::FormID> BuildTamePackMemberIds(
            RE::Actor* player,
            RE::Actor* primaryTarget,
            float splashRadius,
            bool& outRejected)
        {
            outRejected = false;

            std::vector<RE::FormID> result{};
            if (!player || !primaryTarget) {
                return result;
            }

            const auto primaryId = primaryTarget->GetFormID();
            result.push_back(primaryId);

            const float scanRadius = splashRadius + 256.0f;
            TFD::ActorScan::Rescan(scanRadius, false);

            const auto count = TFD::ActorScan::GetCount();
            for (int i = 0; i < count; ++i) {
                auto scanEntry = TFD::ActorScan::GetEntry(i);
                auto* actor = TFD::ActorScan::GetActor(i);
                if (!IsEligibleLocalSplashActor(actor, player, primaryTarget, scanEntry, splashRadius)) {
                    continue;
                }
                if (!IsSameTamePackSpecies(actor, primaryTarget)) {
                    continue;
                }
                if (!SharesPrimaryCombatAnchor(actor, player, primaryTarget)) {
                    continue;
                }

                const auto actorId = actor->GetFormID();
                if (actorId == 0 || actorId == primaryId) {
                    continue;
                }
                if (IsActorBoundToDifferentActiveTameSession(actorId, 0)) {
                    outRejected = true;
                    spdlog::info(
                        "TFDPacify: reject tame pack primary={:08X} actor={:08X} reason=actor_bound_to_other_tame_session",
                        primaryId,
                        actorId);
                    return {};
                }

                result.push_back(actorId);
            }

            std::sort(result.begin(), result.end());
            result.erase(std::unique(result.begin(), result.end()), result.end());

            if (result.size() > kTamePackMaxMembers) {
                outRejected = true;
                spdlog::info(
                    "TFDPacify: reject tame pack primary={:08X} reason=pack_too_large size={} max={}",
                    primaryId,
                    static_cast<unsigned int>(result.size()),
                    static_cast<unsigned int>(kTamePackMaxMembers));
                return {};
            }

            return result;
        }

        RE::Actor* ResolveCurrentCombatTarget(RE::Actor* actor)
        {
            if (!actor) {
                return nullptr;
            }

            auto targetSp = actor->GetActorRuntimeData().currentCombatTarget.get();
            return targetSp.get();
        }

        bool IsEnemyToPlayer(RE::Actor* player, RE::Actor* actor)
        {
            if (!player || !actor) {
                return false;
            }

            if (actor->IsHostileToActor(player)) {
                return true;
            }

            auto* combatTarget = ResolveCurrentCombatTarget(actor);
            if (combatTarget && combatTarget->GetFormID() == player->GetFormID()) {
                return true;
            }

            return false;
        }

        float GetCellBubbleRadius(float requestedRadius)
        {
            const float settingsRadius = TFD::Settings::GetSweepRadius();
            return (std::max)(requestedRadius, (std::max)(settingsRadius, 12000.0f));
        }

        bool IsEligibleCellBubbleActor(
            RE::Actor* actor,
            RE::Actor* player,
            RE::Actor* primaryTarget,
            const TFD::ActorScan::Entry& scanEntry)
        {
            if (!IsActorStillValid(actor) || !player || !primaryTarget) {
                return false;
            }

            if (actor->GetFormID() == player->GetFormID()) {
                return false;
            }

            if (!actor->Is3DLoaded()) {
                return false;
            }

            auto* pCell = player->GetParentCell();
            if (!pCell || actor->GetParentCell() != pCell) {
                return false;
            }

            if (actor->GetFormID() == primaryTarget->GetFormID()) {
                return true;
            }

            return IsEnemyToPlayer(player, actor);
        }

        bool IsEligibleTruceClusterActor(
            RE::Actor* actor,
            RE::Actor* player,
            RE::Actor* primaryTarget,
            const TFD::ActorScan::Entry& scanEntry)
        {
            if (!IsActorStillValid(actor) || !player || !primaryTarget) {
                return false;
            }

            if (actor->GetFormID() == player->GetFormID()) {
                return false;
            }

            if (!actor->Is3DLoaded()) {
                return false;
            }

            if (actor->GetFormID() == primaryTarget->GetFormID()) {
                return true;
            }

            if (!IsEnemyToPlayer(player, actor)) {
                return false;
            }

            auto* playerCell = player->GetParentCell();
            auto* actorCell = actor->GetParentCell();
            auto* primaryCell = primaryTarget->GetParentCell();
            if (playerCell && actorCell && primaryCell && actorCell == playerCell && primaryCell == playerCell) {
                return true;
            }

            auto* playerWs = player->GetWorldspace();
            auto* actorWs = actor->GetWorldspace();
            auto* primaryWs = primaryTarget->GetWorldspace();
            if (playerWs && actorWs && primaryWs && actorWs == playerWs && primaryWs == playerWs) {
                return true;
            }

            return false;
        }

        bool IsPlayerArmedForPacify(RE::Actor* player)
        {
            if (!player) {
                return true;
            }

            if (player->IsWeaponDrawn()) {
                return true;
            }

            auto* state = player->AsActorState();
            if (state && state->GetWeaponState() != RE::WEAPON_STATE::kSheathed) {
                return true;
            }

            return false;
        }

        float ComputeTravelSpeedPerSec(
            const RE::NiPoint3& prev,
            const RE::NiPoint3& next,
            double deltaSec)
        {
            if (deltaSec <= 0.0) {
                return 0.0f;
            }

            return prev.GetDistance(next) / static_cast<float>(deltaSec);
        }

        void MarkTruceBetrayed(RE::Actor* actor)
        {
            if (!actor) {
                return;
            }

            auto& state = g_truceState[actor->GetFormID()];
            state.spent = true;
            state.betrayed = true;
        }

        bool DoesReasonCountAsBetrayal(ReleaseReason reason)
        {
            switch (reason) {
            case ReleaseReason::PlayerArmed:
            case ReleaseReason::TameBroken:
                return true;
            default:
                return false;
            }
        }

        Session* FindActiveSessionForTarget(RE::FormID targetId)
        {
            auto entryIt = g_entries.find(targetId);
            if (entryIt == g_entries.end()) {
                return nullptr;
            }

            auto sessionIt = g_sessions.find(entryIt->second.sessionId);
            if (sessionIt == g_sessions.end()) {
                return nullptr;
            }

            if (sessionIt->second.finished) {
                return nullptr;
            }

            return std::addressof(sessionIt->second);
        }

        Session* FindCompatibleActiveTameSession(RE::Actor* player, RE::Actor* primaryTarget, bool allowDialogue)
        {
            if (!player || !primaryTarget) {
                return nullptr;
            }

            Session* best = nullptr;
            float bestDist = std::numeric_limits<float>::max();
            const float splashRadius = GetLocalHostileSplashRadius() + 256.0f;

            for (auto& [sessionId, session] : g_sessions) {
                (void)sessionId;
                if (session.finished || session.primaryMode != Mode::Tame) {
                    continue;
                }
                if (session.dialogueRequested != allowDialogue) {
                    continue;
                }

                auto* sessionPrimary = ResolveActor(session.primaryTargetId);
                if (!IsActorStillValid(sessionPrimary)) {
                    continue;
                }
                if (!sessionPrimary->Is3DLoaded()) {
                    continue;
                }
                if (!IsSessionSpaceCompatible(sessionPrimary, player, primaryTarget)) {
                    continue;
                }

                const float distToPlayer = sessionPrimary->GetPosition().GetDistance(player->GetPosition());
                const float distToPrimary = sessionPrimary->GetPosition().GetDistance(primaryTarget->GetPosition());
                if (distToPlayer > splashRadius && distToPrimary > splashRadius) {
                    continue;
                }

                const float score = (std::min)(distToPlayer, distToPrimary);
                if (score < bestDist) {
                    bestDist = score;
                    best = std::addressof(session);
                }
            }

            return best;
        }

        bool IsActorBoundToDifferentActiveTameSession(RE::FormID actorId, RE::FormID targetSessionId)
        {
            auto entryIt = g_entries.find(actorId);
            if (entryIt == g_entries.end()) {
                return false;
            }
            if (entryIt->second.sessionId == targetSessionId) {
                return false;
            }

            auto sessionIt = g_sessions.find(entryIt->second.sessionId);
            if (sessionIt == g_sessions.end()) {
                return false;
            }

            return !sessionIt->second.finished && sessionIt->second.primaryMode == Mode::Tame;
        }

        void RefreshSessionEntries(Session& session, double nowSec, double durationSec)
        {
            const double effectiveDurationSec = ResolveSessionDurationSec(session.primaryMode, durationSec);
            const double endTimeSec =
                IsFiniteDurationMode(session.primaryMode) ?
                ClampTameEndTime(nowSec, nowSec + effectiveDurationSec) :
                0.0;

            if (session.primaryMode != Mode::Tame) {
                session.startTimeSec = nowSec;
            }
            else if (session.startTimeSec <= 0.0) {
                session.startTimeSec = nowSec;
            }
            session.lastCalmRefreshSec = (session.primaryMode == Mode::Tame && session.disposition == TameDisposition::Calm) ? nowSec : session.lastCalmRefreshSec;
            session.endTimeSec = (session.disposition == TameDisposition::Companion) ? 0.0 : endTimeSec;
            session.invalidSinceSec = 0.0;
            session.armedSinceSec = 0.0;
            session.tooFarSinceSec = 0.0;
            session.tameStartleSinceSec = 0.0;
            session.lastPlayerSampleSec = 0.0;
            session.lastPlayerPos = {};
            session.hasPlayerSample = false;

            for (auto& [actorId, entry] : g_entries) {
                if (entry.sessionId != session.sessionId) {
                    continue;
                }
                if (session.primaryMode != Mode::Tame || entry.startTimeSec <= 0.0) {
                    entry.startTimeSec = nowSec;
                }
                entry.endTimeSec = (session.disposition == TameDisposition::Companion) ? 0.0 : endTimeSec;
                entry.disposition = session.disposition;
                entry.companionExpireGameDays = session.companionExpireGameDays;
                entry.temporaryTeammateApplied = session.temporaryTeammateApplied;
            }

            if (session.primaryMode == Mode::Tame) {
                spdlog::info(
                    "TFDPacify: tame timer refresh session={} target={:08X} durationSec={:.2f} endTimeSec={:.2f}",
                    session.sessionId,
                    session.primaryTargetId,
                    effectiveDurationSec,
                    session.endTimeSec);
            }
        }

        bool IsSessionSpaceCompatible(RE::Actor* actor, RE::Actor* player, RE::Actor* primaryTarget)
        {
            if (!actor || !player || !primaryTarget) {
                return false;
            }

            auto* actorCell = actor->GetParentCell();
            auto* playerCell = player->GetParentCell();
            auto* primaryCell = primaryTarget->GetParentCell();
            const bool sameCell = actorCell && playerCell && primaryCell && actorCell == playerCell && primaryCell == playerCell;
            if (sameCell) {
                return true;
            }

            auto* actorWs = actor->GetWorldspace();
            auto* playerWs = player->GetWorldspace();
            auto* primaryWs = primaryTarget->GetWorldspace();
            return actorWs && playerWs && primaryWs && actorWs == playerWs && primaryWs == playerWs;
        }

        bool ShouldHandoffTameToTruce(RE::Actor* actor, RE::Actor* player, RE::Actor* primaryTarget)
        {
            if (!IsActorStillValid(actor) || !player || !primaryTarget) {
                return false;
            }

            if (!actor->Is3DLoaded()) {
                return false;
            }

            if (!IsSessionSpaceCompatible(actor, player, primaryTarget)) {
                return false;
            }

            const float distToPlayer = actor->GetPosition().GetDistance(player->GetPosition());
            const float distToPrimary = actor->GetPosition().GetDistance(primaryTarget->GetPosition());
            return distToPlayer <= kTruceActiveCombatRadius || distToPrimary <= kTrucePrimaryLinkRadius;
        }

        std::vector<RE::FormID> CollectTruceTameHandoffIds(
            RE::Actor* player,
            RE::Actor* primaryTarget,
            std::vector<RE::FormID>& sessionsToRelease)
        {
            std::vector<RE::FormID> actorIds;
            if (!player || !primaryTarget) {
                return actorIds;
            }

            actorIds.reserve(g_sessions.size());
            sessionsToRelease.reserve(g_sessions.size());

            for (const auto& [sessionId, session] : g_sessions) {
                if (session.finished || session.primaryMode != Mode::Tame) {
                    continue;
                }

                auto* tamePrimary = ResolveActor(session.primaryTargetId);
                if (!ShouldHandoffTameToTruce(tamePrimary, player, primaryTarget)) {
                    continue;
                }

                if (std::find(actorIds.begin(), actorIds.end(), session.primaryTargetId) == actorIds.end()) {
                    actorIds.push_back(session.primaryTargetId);
                }
                sessionsToRelease.push_back(sessionId);
            }

            return actorIds;
        }

        void ApplyPacify(RE::Actor* actor, Entry& entry, double nowSec)
        {
            if (!IsActorStillValid(actor)) {
                return;
            }

            if (entry.disposition != TameDisposition::Companion && (nowSec - entry.lastPacifyApplySec) >= kPacifyApplyIntervalSec) {
                if (auto* process = RE::ProcessLists::GetSingleton()) {
                    const bool runDetection = process->runDetection;
                    process->runDetection = false;
                    process->ClearCachedFactionFightReactions();
                    process->StopCombatAndAlarmOnActor(actor, false);
                    process->runDetection = runDetection;
                }

                actor->StopCombat();

                if (actor->IsWeaponDrawn()) {
                    actor->DrawWeaponMagicHands(false);
                }

                entry.lastPacifyApplySec = nowSec;
            }

            if ((nowSec - entry.lastPackageEvalSec) >= kPackageEvalIntervalSec) {
                actor->EvaluatePackage(true, false);
                entry.lastPackageEvalSec = nowSec;
            }
        }

        void RemovePacify(RE::Actor* actor, Entry& entry)
        {
            if (!actor) {
                return;
            }

            if (auto* process = RE::ProcessLists::GetSingleton()) {
                process->ClearCachedFactionFightReactions();
            }

            actor->EvaluatePackage(true, false);
            (void)entry;
        }

        void PulseGlobalDetection(const char* reason)
        {
            auto* process = RE::ProcessLists::GetSingleton();
            if (!process) {
                return;
            }

            const bool before = process->runDetection;
            process->runDetection = false;
            process->ClearCachedFactionFightReactions();
            process->runDetection = true;
            process->ClearCachedFactionFightReactions();

            spdlog::info(
                "TFDPacify: detection pulse reason={} before={} after={}",
                reason ? reason : "<null>",
                before ? 1 : 0,
                process->runDetection ? 1 : 0);
        }

        void QueueRehostileRetry(RE::Actor* actor, RE::Actor* player, RE::FormID sessionId, ReleaseReason reason, double nowSec, bool drawWeapon)
        {
            if (!actor || !player) {
                return;
            }
            if (TFD::DefeatMonitor::IsDefeatedEnemyKnocked(actor)) {
                spdlog::info(
                    "TFDPacify: skip queue rehostile actor={:08X} player={:08X} session={} reason={} defeated_knock=1",
                    actor->GetFormID(),
                    player->GetFormID(),
                    sessionId,
                    ToString(reason));
                return;
            }

            RehostileRequest req;
            req.actorId = actor->GetFormID();
            req.playerId = player->GetFormID();
            req.sessionId = sessionId;
            req.reason = reason;
            req.nextAttemptSec = nowSec + kRehostileRetryDelaySec;
            req.expireSec = nowSec + kRehostileRetryLifetimeSec;
            req.attemptsRemaining = kRehostileRetryCount;
            req.drawWeapon = drawWeapon;

            g_rehostileRequests[req.actorId] = req;

            spdlog::info(
                "TFDPacify: queue rehostile actor={:08X} player={:08X} session={} reason={} attempts={}",
                req.actorId,
                req.playerId,
                sessionId,
                ToString(reason),
                static_cast<unsigned int>(req.attemptsRemaining));
        }

        void ProcessRehostileRetries(double nowSec)
        {
            if (g_rehostileRequests.empty()) {
                return;
            }

            std::vector<RE::FormID> toErase;
            toErase.reserve(g_rehostileRequests.size());

            for (auto& [actorId, req] : g_rehostileRequests) {
                if (req.attemptsRemaining == 0 || nowSec >= req.expireSec) {
                    toErase.push_back(actorId);
                    continue;
                }

                if (nowSec < req.nextAttemptSec) {
                    continue;
                }

                auto* actor = ResolveActor(req.actorId);
                auto* player = ResolveActor(req.playerId);
                if (!IsActorStillValid(actor) || !IsActorStillValid(player)) {
                    toErase.push_back(actorId);
                    continue;
                }

                if (TFD::DefeatMonitor::IsDefeatedEnemyKnocked(actor)) {
                    spdlog::info(
                        "TFDPacify: cancel queued rehostile actor={:08X} player={:08X} session={} reason={} defeated_knock=1",
                        req.actorId,
                        req.playerId,
                        req.sessionId,
                        ToString(req.reason));
                    toErase.push_back(actorId);
                    continue;
                }

                if (g_entries.find(actorId) != g_entries.end()) {
                    req.nextAttemptSec = nowSec + kRehostileRetryDelaySec;
                    continue;
                }

                const bool satisfied = ForceRehostile(actor, player, req.reason, req.drawWeapon);
                if (satisfied) {
                    toErase.push_back(actorId);
                    continue;
                }

                if (req.attemptsRemaining > 0) {
                    --req.attemptsRemaining;
                }
                if (req.attemptsRemaining == 0) {
                    toErase.push_back(actorId);
                    continue;
                }

                req.nextAttemptSec = nowSec + kRehostileRetryDelaySec + (kRehostileRetryExtendSec * (kRehostileRetryCount - req.attemptsRemaining));
            }

            for (auto actorId : toErase) {
                g_rehostileRequests.erase(actorId);
            }
        }

        bool AddOrRefreshEntry(
            RE::Actor* actor,
            Mode mode,
            RE::FormID sessionId,
            RE::FormID primaryTargetId,
            double startTimeSec,
            double endTimeSec,
            bool allowDialogue,
            bool isPrimaryTarget,
            TameDisposition disposition = TameDisposition::None,
            double companionExpireGameDays = 0.0,
            bool temporaryTeammateApplied = false)
        {
            if (!IsActorStillValid(actor)) {
                return false;
            }

            Entry entry;
            entry.actorId = actor->GetFormID();
            entry.mode = mode;
            entry.sessionId = sessionId;
            entry.primaryTargetId = primaryTargetId;
            entry.startTimeSec = startTimeSec;
            entry.endTimeSec = endTimeSec;
            entry.companionExpireGameDays = companionExpireGameDays;
            entry.lastPacifyApplySec = 0.0;
            entry.lastPackageEvalSec = 0.0;
            entry.disposition = disposition;
            entry.temporaryTeammateApplied = temporaryTeammateApplied;
            entry.allowDialogue = allowDialogue;
            entry.isPrimaryTarget = isPrimaryTarget;

            g_rehostileRequests.erase(entry.actorId);
            g_entries[entry.actorId] = entry;
            return true;
        }

        void SyncSessionDisposition(Session& session)
        {
            for (auto& [actorId, entry] : g_entries) {
                if (entry.sessionId != session.sessionId) {
                    continue;
                }
                entry.disposition = session.disposition;
                entry.companionExpireGameDays = session.companionExpireGameDays;
                entry.temporaryTeammateApplied = session.temporaryTeammateApplied;
                if (session.disposition == TameDisposition::Companion) {
                    entry.endTimeSec = 0.0;
                }
            }
        }

        ReleaseReason ComputeInvalidReason(Session& session, double nowSec)
        {
            auto* player = ResolveActor(session.playerId);
            auto* primaryTarget = ResolveActor(session.primaryTargetId);

            if (!IsActorStillValid(player) || !IsActorStillValid(primaryTarget)) {
                return ReleaseReason::InvalidActor;
            }

            if (session.primaryMode == Mode::Tame &&
                session.endTimeSec > 0.0 &&
                nowSec >= session.endTimeSec) {
                return ReleaseReason::TameExpired;
            }

            if (session.primaryMode == Mode::Tame && session.disposition == TameDisposition::Companion) {
                session.armedSinceSec = 0.0;
            }
            else if ((nowSec - session.startTimeSec) >= kArmedGraceSec && IsPlayerArmedForPacify(player)) {
                if (session.armedSinceSec <= 0.0) {
                    session.armedSinceSec = nowSec;
                }
                else if ((nowSec - session.armedSinceSec) >= kArmedDebounceSec) {
                    return ReleaseReason::PlayerArmed;
                }
            }
            else {
                session.armedSinceSec = 0.0;
            }

            const auto playerPos = player->GetPosition();
            const auto targetPos = primaryTarget->GetPosition();
            const float distance = playerPos.GetDistance(targetPos);

            float playerSpeedPerSec = 0.0f;
            if (session.hasPlayerSample && nowSec > session.lastPlayerSampleSec) {
                playerSpeedPerSec = ComputeTravelSpeedPerSec(
                    session.lastPlayerPos,
                    playerPos,
                    nowSec - session.lastPlayerSampleSec);
            }

            session.lastPlayerPos = playerPos;
            session.lastPlayerSampleSec = nowSec;
            session.hasPlayerSample = true;
            session.tooFarSinceSec = 0.0;

            if (session.primaryMode == Mode::Tame &&
                session.disposition == TameDisposition::Companion &&
                session.companionExpireGameDays > 0.0 &&
                CurrentGameDays() >= session.companionExpireGameDays) {
                return ReleaseReason::TameExpired;
            }

            const double calmReferenceSec =
                session.lastCalmRefreshSec > 0.0 ? session.lastCalmRefreshSec : session.startTimeSec;
            const double tameElapsedSec = nowSec - calmReferenceSec;
            const bool canStartle =
                session.primaryMode == Mode::Tame &&
                session.disposition != TameDisposition::Companion &&
                tameElapsedSec >= kTameStartleGraceSec &&
                tameElapsedSec <= kTameStartleWindowSec;

            const bool startled =
                canStartle &&
                distance <= kTameStartleDistance &&
                playerSpeedPerSec >= kTameStartleRushSpeedPerSec;

            if (startled) {
                if (session.tameStartleSinceSec <= 0.0) {
                    session.tameStartleSinceSec = nowSec;
                }
                else if ((nowSec - session.tameStartleSinceSec) >= kTameStartleDebounceSec) {
                    return ReleaseReason::TameBroken;
                }
            }
            else {
                session.tameStartleSinceSec = 0.0;
            }

            session.invalidSinceSec = 0.0;
            return ReleaseReason::Generic;
        }


        std::optional<RE::FormID> BeginSessionCommon(
            RE::Actor* player,
            RE::Actor* primaryTarget,
            Mode mode,
            double nowSec,
            double durationSec,
            bool allowDialogue,
            bool applyCellBubble,
            bool allowLocalSplash,
            float cellBubbleRadius,
            bool ignoreSpent = false)
        {
            (void)nowSec;
            if (!IsActorStillValid(player) || !IsActorStillValid(primaryTarget)) {
                return std::nullopt;
            }

            nowSec = PacifyNowSec();
            const double effectiveDurationSec = ResolveSessionDurationSec(mode, durationSec);
            const double endTimeSec =
                IsFiniteDurationMode(mode) ?
                ClampTameEndTime(nowSec, nowSec + effectiveDurationSec) :
                0.0;

            if (IsTruceMode(mode) && !ignoreSpent) {
                auto it = g_truceState.find(primaryTarget->GetFormID());
                if (it != g_truceState.end() && it->second.spent) {
                    spdlog::info(
                        "TFDPacify: reject session mode={} target={:08X} reason=truce_spent",
                        ToString(mode),
                        primaryTarget->GetFormID());
                    return std::nullopt;
                }
            }

            if (IsPlayerArmedForPacify(player)) {
                const bool allowForcedSheath = (mode == Mode::TruceInCombat || mode == Mode::TrucePreCombat);
                if (allowForcedSheath) {
                    player->DrawWeaponMagicHands(false);
                    spdlog::info(
                        "TFDPacify: forced sheath for session mode={} target={:08X} reason=player_armed",
                        ToString(mode),
                        primaryTarget->GetFormID());
                }
                else {
                    spdlog::info(
                        "TFDPacify: reject session mode={} target={:08X} reason=player_armed",
                        ToString(mode),
                        primaryTarget->GetFormID());
                    return std::nullopt;
                }
            }

            std::vector<RE::FormID> truceHandoffIds;
            if (IsTruceMode(mode)) {
                std::vector<RE::FormID> tameSessionsToRelease;
                truceHandoffIds = CollectTruceTameHandoffIds(player, primaryTarget, tameSessionsToRelease);
                if (!tameSessionsToRelease.empty()) {
                    for (auto tameSessionId : tameSessionsToRelease) {
                        ReleaseSession(tameSessionId, ReleaseReason::Generic);
                    }

                    spdlog::info(
                        "TFDPacify: truce handoff released tameSessions={} handoffActors={} newTarget={:08X}",
                        static_cast<unsigned int>(tameSessionsToRelease.size()),
                        static_cast<unsigned int>(truceHandoffIds.size()),
                        primaryTarget->GetFormID());
                }
            }

            if (Session* active = FindActiveSessionForTarget(primaryTarget->GetFormID())) {
                if (active->primaryMode == mode &&
                    active->dialogueRequested == allowDialogue) {
                    RefreshSessionEntries(*active, nowSec, effectiveDurationSec);
                    spdlog::info(
                        "TFDPacify: refresh session id={} mode={} target={:08X} reason=target_already_active",
                        active->sessionId,
                        ToString(mode),
                        primaryTarget->GetFormID());
                    return active->sessionId;
                }

                const auto activeSessionId = active->sessionId;
                const auto activeMode = active->primaryMode;
                const auto activeTarget = active->primaryTargetId;

                spdlog::info(
                    "TFDPacify: replace session oldId={} oldMode={} oldTarget={:08X} newMode={} newTarget={:08X}",
                    activeSessionId,
                    ToString(activeMode),
                    activeTarget,
                    ToString(mode),
                    primaryTarget->GetFormID());

                ReleaseSession(activeSessionId, ReleaseReason::Generic);
            }

            std::vector<RE::FormID> curatedTameIds;
            bool tamePackRejected = false;
            if (mode == Mode::Tame && allowLocalSplash) {
                curatedTameIds = BuildTamePackMemberIds(player, primaryTarget, GetLocalHostileSplashRadius(), tamePackRejected);
                if (tamePackRejected || curatedTameIds.empty()) {
                    return std::nullopt;
                }

                for (auto actorId : curatedTameIds) {
                    auto* actor = ResolveActor(actorId);
                    if (!IsActorStillValid(actor) || !actor->Is3DLoaded()) {
                        spdlog::info(
                            "TFDPacify: reject tame pack primary={:08X} actor={:08X} reason=pack_member_not_ready",
                            primaryTarget->GetFormID(),
                            actorId);
                        return std::nullopt;
                    }
                }
            }

            const RE::FormID playerId = player->GetFormID();
            const RE::FormID targetId = primaryTarget->GetFormID();

            Session session;
            RE::FormID sessionId = 0;

            if (sessionId == 0) {
                sessionId = g_nextSessionId++;
                session.sessionId = sessionId;
                session.pendingReleaseReason = ReleaseReason::Generic;
                session.dialogueOpened = false;
                session.finished = false;
            }

            session.playerId = playerId;
            session.primaryTargetId = targetId;
            session.primaryMode = mode;
            session.pendingReleaseReason = ReleaseReason::Generic;
            session.startTimeSec = nowSec;
            session.lastCalmRefreshSec = (mode == Mode::Tame) ? nowSec : 0.0;
            session.endTimeSec = endTimeSec;
            session.invalidSinceSec = 0.0;
            session.armedSinceSec = 0.0;
            session.tooFarSinceSec = 0.0;
            session.tameStartleSinceSec = 0.0;
            session.lastPlayerSampleSec = 0.0;
            session.lastPlayerPos = {};
            session.hasPlayerSample = false;
            session.disposition = (mode == Mode::Tame) ? TameDisposition::Calm : TameDisposition::None;
            session.temporaryTeammateApplied = false;
            session.dialogueRequested = allowDialogue;
            session.finished = false;

            if (!AddOrRefreshEntry(
                primaryTarget,
                mode,
                sessionId,
                targetId,
                nowSec,
                endTimeSec,
                allowDialogue,
                true,
                mode == Mode::Tame ? TameDisposition::Calm : TameDisposition::None,
                0.0,
                false)) {
                return std::nullopt;
            }

            std::size_t cellBubbleCount = 0;
            std::size_t localSplashCount = 0;
            std::size_t truceClusterCount = 0;
            std::vector<RE::FormID> curatedTruceIds;
            if (mode == Mode::TruceInCombat) {
                const float scanRadius = (std::max)(GetCellBubbleRadius(cellBubbleRadius), 6000.0f);
                curatedTruceIds = BuildTruceInCombatMemberIds(player, primaryTarget, scanRadius, cellBubbleCount, truceClusterCount);
                for (auto actorId : truceHandoffIds) {
                    if (actorId == 0 || actorId == targetId) {
                        continue;
                    }
                    if (std::find(curatedTruceIds.begin(), curatedTruceIds.end(), actorId) == curatedTruceIds.end()) {
                        curatedTruceIds.push_back(actorId);
                    }
                }
                for (auto actorId : curatedTruceIds) {
                    auto* actor = ResolveActor(actorId);
                    if (!actor) {
                        continue;
                    }
                    const bool isPrimary = actorId == targetId;
                    AddOrRefreshEntry(
                        actor,
                        mode,
                        sessionId,
                        targetId,
                        nowSec,
                        endTimeSec,
                        allowDialogue,
                        isPrimary,
                        mode == Mode::Tame ? TameDisposition::Calm : TameDisposition::None,
                        0.0,
                        false);
                }
            }
            else if (applyCellBubble) {
                const float scanRadius = GetCellBubbleRadius(cellBubbleRadius);
                TFD::ActorScan::Rescan(scanRadius, false);
                const auto count = TFD::ActorScan::GetCount();
                for (int i = 0; i < count; ++i) {
                    auto scanEntry = TFD::ActorScan::GetEntry(i);
                    auto* actor = TFD::ActorScan::GetActor(i);
                    if (!IsEligibleCellBubbleActor(actor, player, primaryTarget, scanEntry)) {
                        continue;
                    }

                    const RE::FormID actorId = actor->GetFormID();
                    const bool existedInSession = [&]() {
                        auto it = g_entries.find(actorId);
                        return it != g_entries.end() && it->second.sessionId == sessionId;
                        }();

                    const bool isPrimary = actorId == targetId;
                    if (!AddOrRefreshEntry(
                        actor,
                        mode,
                        sessionId,
                        targetId,
                        nowSec,
                        endTimeSec,
                        allowDialogue,
                        isPrimary)) {
                        continue;
                    }

                    if (!existedInSession) {
                        ++cellBubbleCount;
                    }
                }
            }

            if (IsTruceMode(mode)) {
                for (auto actorId : truceHandoffIds) {
                    if (actorId == 0 || actorId == targetId) {
                        continue;
                    }
                    auto* actor = ResolveActor(actorId);
                    if (!actor) {
                        continue;
                    }
                    const bool alreadyInSession = [&]() {
                        auto it = g_entries.find(actorId);
                        return it != g_entries.end() && it->second.sessionId == sessionId;
                        }();
                    if (alreadyInSession) {
                        continue;
                    }
                    AddOrRefreshEntry(
                        actor,
                        mode,
                        sessionId,
                        targetId,
                        nowSec,
                        endTimeSec,
                        allowDialogue,
                        false,
                        mode == Mode::Tame ? TameDisposition::Calm : TameDisposition::None,
                        0.0,
                        false);
                }
            }

            if (mode == Mode::Tame && allowLocalSplash) {
                for (auto actorId : curatedTameIds) {
                    if (actorId == 0 || actorId == targetId) {
                        continue;
                    }

                    auto* actor = ResolveActor(actorId);
                    if (!IsActorStillValid(actor) || !actor->Is3DLoaded()) {
                        for (auto it = g_entries.begin(); it != g_entries.end();) {
                            if (it->second.sessionId == sessionId) {
                                it = g_entries.erase(it);
                            }
                            else {
                                ++it;
                            }
                        }
                        spdlog::info(
                            "TFDPacify: reject tame pack session={} target={:08X} actor={:08X} reason=pack_member_lost_before_apply",
                            sessionId,
                            targetId,
                            actorId);
                        return std::nullopt;
                    }

                    const bool existedInSession = [&]() {
                        auto it = g_entries.find(actorId);
                        return it != g_entries.end() && it->second.sessionId == sessionId;
                        }();

                    if (!AddOrRefreshEntry(
                        actor,
                        mode,
                        sessionId,
                        targetId,
                        nowSec,
                        endTimeSec,
                        allowDialogue,
                        false)) {
                        for (auto it = g_entries.begin(); it != g_entries.end();) {
                            if (it->second.sessionId == sessionId) {
                                it = g_entries.erase(it);
                            }
                            else {
                                ++it;
                            }
                        }
                        spdlog::info(
                            "TFDPacify: reject tame pack session={} target={:08X} actor={:08X} reason=pack_add_failed",
                            sessionId,
                            targetId,
                            actorId);
                        return std::nullopt;
                    }

                    if (!existedInSession) {
                        ++localSplashCount;
                    }
                }
            }


            g_sessions[sessionId] = session;

            std::vector<RE::FormID> applyIds;
            if (mode == Mode::TruceInCombat && !curatedTruceIds.empty()) {
                applyIds = curatedTruceIds;
            }
            else {
                applyIds.reserve(g_entries.size());
                for (const auto& [actorId, entry] : g_entries) {
                    if (entry.sessionId == sessionId) {
                        applyIds.push_back(actorId);
                    }
                }
                std::sort(applyIds.begin(), applyIds.end());
            }

            for (RE::FormID actorId : applyIds) {
                auto it = g_entries.find(actorId);
                if (it == g_entries.end()) {
                    continue;
                }
                if (auto* actor = ResolveActor(actorId)) {
                    ApplyPacify(actor, it->second, nowSec);
                }
            }

            const std::size_t packSize = applyIds.size();
            spdlog::info(
                "TFDPacify: begin session id={} mode={} target={:08X} cellBubble={} localSplash={} packSize={} allowDialogue={} durationSec={:.2f} endTimeSec={:.2f}",
                sessionId,
                ToString(mode),
                targetId,
                static_cast<unsigned int>(cellBubbleCount),
                static_cast<unsigned int>(localSplashCount + truceClusterCount),
                static_cast<unsigned int>(packSize),
                allowDialogue ? 1 : 0,
                effectiveDurationSec,
                endTimeSec);

            const auto eventIds = IsTruceMode(mode) ? SelectTruceEventTargets(applyIds, targetId) : SelectCrowdEventTargets(applyIds, player, targetId);
            const auto sent = SendModEventToActors(GetAssignEventName(mode), eventIds);
            spdlog::info(
                "TFDPacify: assign events event={} session={} sent={} primary={:08X}",
                GetAssignEventName(mode) ? GetAssignEventName(mode) : "<none>",
                sessionId,
                static_cast<unsigned int>(sent),
                targetId);

            if (const auto* supplementalAssign = GetSupplementalAssignEventName(mode)) {
                const auto supplementalSent = SendModEventToActors(supplementalAssign, eventIds);
                spdlog::info(
                    "TFDPacify: supplemental assign event={} session={} sent={} primary={:08X}",
                    supplementalAssign,
                    sessionId,
                    static_cast<unsigned int>(supplementalSent),
                    targetId);
            }
            return sessionId;
        }
    }

    void Reset()
    {
        ReleaseAll();
        g_nextSessionId = 1;
        g_truceState.clear();
    }

    void Update(double nowSec)
    {
        (void)nowSec;
        nowSec = PacifyNowSec();

        std::vector<std::pair<RE::FormID, ReleaseReason>> sessionsToRelease;
        sessionsToRelease.reserve(g_sessions.size());

        for (auto& [sessionId, session] : g_sessions) {
            if (session.finished) {
                sessionsToRelease.emplace_back(sessionId, ReleaseReason::Generic);
                continue;
            }

            const auto invalidReason = ComputeInvalidReason(session, nowSec);
            if (invalidReason != ReleaseReason::Generic) {
                sessionsToRelease.emplace_back(sessionId, invalidReason);
                continue;
            }
        }

        for (const auto& [sessionId, reason] : sessionsToRelease) {
            ReleaseSession(sessionId, reason);
        }

        std::vector<RE::FormID> entriesToErase;
        entriesToErase.reserve(g_entries.size());

        for (auto& [actorId, entry] : g_entries) {
            auto* actor = ResolveActor(actorId);
            if (!IsActorStillValid(actor)) {
                entriesToErase.push_back(actorId);
                continue;
            }

            auto sessionIt = g_sessions.find(entry.sessionId);
            if (sessionIt == g_sessions.end() || sessionIt->second.finished) {
                entriesToErase.push_back(actorId);
                continue;
            }

            ApplyPacify(actor, entry, nowSec);
        }

        for (RE::FormID actorId : entriesToErase) {
            auto it = g_entries.find(actorId);
            if (it == g_entries.end()) {
                continue;
            }

            if (auto* actor = ResolveActor(actorId)) {
                RemovePacify(actor, it->second);
            }

            g_entries.erase(it);
        }

        ProcessRehostileRetries(nowSec);
    }

    std::optional<RE::FormID> BeginTameSession(
        RE::Actor* player,
        RE::Actor* primaryTarget,
        double nowSec,
        bool allowDialogue,
        bool allowLocalSplash)
    {
        if (!CanStartTame(primaryTarget)) {
            if (primaryTarget) {
                spdlog::info(
                    "TFDPacify: reject begin tame target={:08X} reason=active_tame_requires_feed",
                    primaryTarget->GetFormID());
            }
            return std::nullopt;
        }

        if (!allowLocalSplash) {
            return BeginSessionCommon(
                player,
                primaryTarget,
                Mode::Tame,
                nowSec,
                kTameDurationSec,
                allowDialogue,
                false,
                false,
                0.0f);
        }

        bool tamePackRejected = false;
        auto tamePackIds = BuildTamePackMemberIds(player, primaryTarget, GetLocalHostileSplashRadius(), tamePackRejected);
        if (tamePackRejected || tamePackIds.empty()) {
            return std::nullopt;
        }

        for (auto actorId : tamePackIds) {
            auto* actor = ResolveActor(actorId);
            if (!IsActorStillValid(actor) || !actor->Is3DLoaded()) {
                spdlog::info(
                    "TFDPacify: reject tame pack primary={:08X} actor={:08X} reason=pack_member_not_ready_before_batch",
                    primaryTarget ? primaryTarget->GetFormID() : 0,
                    actorId);
                return std::nullopt;
            }

            if (!CanStartTame(actor)) {
                spdlog::info(
                    "TFDPacify: reject tame pack primary={:08X} actor={:08X} reason=actor_already_has_active_tame",
                    primaryTarget ? primaryTarget->GetFormID() : 0,
                    actorId);
                return std::nullopt;
            }

            if (TFD::TameBait::CollectValidBaits(player, actor).empty()) {
                spdlog::info(
                    "TFDPacify: reject tame pack primary={:08X} actor={:08X} reason=no_valid_bait_for_pack_member",
                    primaryTarget ? primaryTarget->GetFormID() : 0,
                    actorId);
                return std::nullopt;
            }
        }

        std::vector<RE::FormID> createdSessionIds;
        createdSessionIds.reserve(tamePackIds.size());

        std::optional<RE::FormID> primarySessionId;
        for (auto actorId : tamePackIds) {
            auto* actor = ResolveActor(actorId);
            if (!actor) {
                for (auto createdId : createdSessionIds) {
                    ReleaseSession(createdId, ReleaseReason::Generic);
                }
                spdlog::info(
                    "TFDPacify: reject tame pack primary={:08X} actor={:08X} reason=pack_member_lost_during_batch",
                    primaryTarget ? primaryTarget->GetFormID() : 0,
                    actorId);
                return std::nullopt;
            }

            auto sessionId = BeginSessionCommon(
                player,
                actor,
                Mode::Tame,
                nowSec,
                kTameDurationSec,
                allowDialogue,
                false,
                false,
                0.0f);
            if (!sessionId.has_value()) {
                for (auto createdId : createdSessionIds) {
                    ReleaseSession(createdId, ReleaseReason::Generic);
                }
                spdlog::info(
                    "TFDPacify: reject tame pack primary={:08X} actor={:08X} reason=batch_session_begin_failed",
                    primaryTarget ? primaryTarget->GetFormID() : 0,
                    actorId);
                return std::nullopt;
            }

            createdSessionIds.push_back(*sessionId);
            if (actorId == primaryTarget->GetFormID()) {
                primarySessionId = *sessionId;
            }
        }

        spdlog::info(
            "TFDPacify: tame pack batch success primary={:08X} members={} primarySession={} separateSessions=1",
            primaryTarget ? primaryTarget->GetFormID() : 0,
            static_cast<unsigned int>(tamePackIds.size()),
            primarySessionId.value_or(0));

        return primarySessionId;
    }

    std::optional<RE::FormID> BeginTrucePreCombatSession(
        RE::Actor* player,
        RE::Actor* primaryTarget,
        double nowSec)
    {
        return BeginSessionCommon(
            player,
            primaryTarget,
            Mode::TrucePreCombat,
            nowSec,
            0.0,
            true,
            true,
            false,
            12000.0f);
    }

    std::optional<RE::FormID> BeginTruceInCombatSession(
        RE::Actor* player,
        RE::Actor* primaryTarget,
        double nowSec,
        bool allowDialogue,
        bool ignoreSpent)
    {
        const bool applyCellBubble = allowDialogue;
        const float cellBubbleRadius = allowDialogue ? 12000.0f : 0.0f;

        return BeginSessionCommon(
            player,
            primaryTarget,
            Mode::TruceInCombat,
            nowSec,
            0.0,
            allowDialogue,
            applyCellBubble,
            false,
            cellBubbleRadius,
            ignoreSpent);
    }

    std::optional<RE::FormID> BeginCellTruceBurst(
        RE::Actor* player,
        RE::Actor* primaryTarget,
        double nowSec,
        double durationSec,
        float radius)
    {
        return BeginSessionCommon(
            player,
            primaryTarget,
            Mode::TruceInCombat,
            nowSec,
            durationSec,
            false,
            true,
            false,
            radius);
    }

    bool IsPacified(RE::Actor* actor)
    {
        if (!actor) {
            return false;
        }

        auto it = g_entries.find(actor->GetFormID());
        if (it != g_entries.end()) {
            return it->second.disposition != TameDisposition::Companion;
        }

        return TFD::DefeatMonitor::IsDefeatedEnemyKnocked(actor);
    }

    Mode GetMode(RE::Actor* actor)
    {
        if (!actor) {
            return Mode::None;
        }

        auto it = g_entries.find(actor->GetFormID());
        if (it == g_entries.end()) {
            return Mode::None;
        }

        return it->second.mode;
    }

    bool CanOpenDialogue(RE::Actor* actor)
    {
        if (!actor) {
            return false;
        }

        auto it = g_entries.find(actor->GetFormID());
        if (it != g_entries.end()) {
            const Entry& entry = it->second;
            return entry.allowDialogue;
        }

        return TFD::DefeatMonitor::IsDialogueCapableDefeatedEnemy(actor);
    }

    bool CanStartTame(RE::Actor* actor)
    {
        return actor && !HasActiveTameSession(actor);
    }

    bool HasActiveTameSession(RE::Actor* actor)
    {
        if (!actor) {
            return false;
        }

        auto entryIt = g_entries.find(actor->GetFormID());
        if (entryIt == g_entries.end()) {
            return false;
        }

        auto sessionIt = g_sessions.find(entryIt->second.sessionId);
        if (sessionIt == g_sessions.end() || sessionIt->second.finished) {
            return false;
        }

        return sessionIt->second.primaryMode == Mode::Tame;
    }

    std::vector<ActiveTameSnapshot> GetActiveTameSnapshots(double nowSec)
    {
        if (nowSec <= 0.0) {
            nowSec = PacifyNowSec();
        }

        std::vector<ActiveTameSnapshot> result{};
        result.reserve(g_sessions.size());

        for (const auto& [sessionId, session] : g_sessions) {
            if (session.finished || session.primaryMode != Mode::Tame || session.primaryTargetId == 0) {
                continue;
            }

            ActiveTameSnapshot snap{};
            snap.actorId = session.primaryTargetId;
            snap.sessionId = sessionId;
            snap.mode = session.primaryMode;
            snap.disposition = session.disposition;
            snap.remainingTameSec = session.disposition == TameDisposition::Companion || session.endTimeSec <= 0.0 ?
                0.0 :
                (std::max)(0.0, session.endTimeSec - nowSec);

            if (session.disposition == TameDisposition::Companion && session.companionExpireGameDays > 0.0) {
                const double remainingDays = session.companionExpireGameDays - CurrentGameDays();
                snap.remainingCompanionHours = (std::max)(0.0, remainingDays * 24.0);
            }

            if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(snap.actorId)) {
                snap.loaded = true;
                if (const char* name = actor->GetName(); name && name[0]) {
                    snap.actorName = name;
                }
            }

            if (snap.actorName.empty()) {
                char fallback[64];
                std::snprintf(fallback, sizeof(fallback), "Creature 0x%08X", snap.actorId);
                snap.actorName = fallback;
            }

            result.push_back(std::move(snap));
        }

        std::sort(result.begin(), result.end(), [](const ActiveTameSnapshot& a, const ActiveTameSnapshot& b) {
            if (a.disposition != b.disposition) {
                return static_cast<std::uint8_t>(a.disposition) > static_cast<std::uint8_t>(b.disposition);
            }
            if (a.actorName != b.actorName) {
                return a.actorName < b.actorName;
            }
            return a.actorId < b.actorId;
            });

        return result;
    }

    std::vector<FeedOptionSnapshot> GetActiveTameFeedOptions(RE::Actor* actor, FeedAction action)
    {
        std::vector<FeedOptionSnapshot> result{};
        if (!actor || !HasActiveTameSession(actor)) {
            return result;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return result;
        }

        const bool downed = TFD::DefeatMonitor::IsThresholdDownedActor(actor);
        const auto options = TFD::TameBait::CollectValidBaits(player, actor);
        const std::int32_t cost = action == FeedAction::Teammate ? kCompanionFeedCost : kCalmFeedCost;
        result.reserve(options.size());

        for (const auto& opt : options) {
            if (!opt.item || opt.count < cost) {
                continue;
            }

            FeedOptionSnapshot bait{};
            bait.itemId = opt.item->GetFormID();
            bait.itemName = opt.name;
            bait.count = opt.count;
            bait.cost = cost;
            bait.calmExtendSec = opt.extendSec;
            bait.action = action;

            char buffer[224];
            if (action == FeedAction::Teammate) {
                if (downed) {
                    std::snprintf(buffer, sizeof(buffer), "%s x%d (cost %d, revive + heal, +%.0fh)", opt.name.c_str(), opt.count, cost, kCompanionFeedHours);
                }
                else {
                    std::snprintf(buffer, sizeof(buffer), "%s x%d (cost %d, +%.0fh)", opt.name.c_str(), opt.count, cost, kCompanionFeedHours);
                }
            }
            else {
                if (downed) {
                    std::snprintf(buffer, sizeof(buffer), "%s x%d (cost %d, revive + heal, +%.0fs)", opt.name.c_str(), opt.count, cost, opt.extendSec);
                }
                else {
                    std::snprintf(buffer, sizeof(buffer), "%s x%d (cost %d, +%.0fs)", opt.name.c_str(), opt.count, cost, opt.extendSec);
                }
            }
            bait.label = buffer;
            result.push_back(std::move(bait));
        }

        return result;
    }

    bool ApplyActiveTameFeed(RE::Actor* actor, RE::FormID itemId, FeedAction action)
    {
        if (!actor || itemId == 0 || !HasActiveTameSession(actor)) {
            return false;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return false;
        }

        const bool reviveAfterFeed = TFD::DefeatMonitor::IsThresholdDownedActor(actor);
        const auto options = GetActiveTameFeedOptions(actor, action);
        const auto it = std::find_if(options.begin(), options.end(), [&](const FeedOptionSnapshot& opt) {
            return opt.itemId == itemId;
            });
        if (it == options.end()) {
            return false;
        }

        auto* item = RE::TESForm::LookupByID<RE::TESBoundObject>(itemId);
        if (!item) {
            return false;
        }

        bool ok = false;
        if (action == FeedAction::Teammate) {
            if (IsCompanion(actor)) {
                ok = ExtendActiveCompanionHours(actor, kCompanionFeedHours);
            }
            else {
                ok = PromoteActiveTameToCompanion(actor, kCompanionFeedHours);
            }
        }
        else {
            ok = ExtendActiveTameSession(actor, it->calmExtendSec, 0.0);
        }

        if (!ok) {
            return false;
        }

        if (!TFD::TameBait::ConsumeBait(player, item, it->cost)) {
            return false;
        }

        if (reviveAfterFeed) {
            const float reviveHealPct = action == FeedAction::Teammate ? 60.0f : 45.0f;
            if (!TFD::DefeatMonitor::ReviveDownedAlly(actor, reviveHealPct)) {
                spdlog::warn("TFDPacify: feed revive failed actor={:08X} action={}", actor->GetFormID(), action == FeedAction::Teammate ? "teammate" : "calm");
            }
        }

        return true;
    }

    bool ExtendActiveTameSession(RE::Actor* actor, double addSec, double nowSec)
    {
        if (!actor || addSec <= 0.0) {
            return false;
        }

        if (nowSec <= 0.0) {
            nowSec = PacifyNowSec();
        }

        auto entryIt = g_entries.find(actor->GetFormID());
        if (entryIt == g_entries.end()) {
            return false;
        }

        auto sessionIt = g_sessions.find(entryIt->second.sessionId);
        if (sessionIt == g_sessions.end() || sessionIt->second.finished) {
            return false;
        }

        Session& session = sessionIt->second;
        if (session.primaryMode != Mode::Tame || session.disposition == TameDisposition::Companion) {
            return false;
        }

        const double baseEndTime = session.endTimeSec > nowSec ? session.endTimeSec : nowSec;
        session.endTimeSec = ClampTameEndTime(nowSec, baseEndTime + addSec);
        if (session.startTimeSec <= 0.0) {
            session.startTimeSec = nowSec;
        }
        session.lastCalmRefreshSec = nowSec;
        session.invalidSinceSec = 0.0;
        session.armedSinceSec = 0.0;
        session.tooFarSinceSec = 0.0;
        session.tameStartleSinceSec = 0.0;
        session.lastPlayerSampleSec = 0.0;
        session.lastPlayerPos = {};
        session.hasPlayerSample = false;

        for (auto& [actorId, entry] : g_entries) {
            if (entry.sessionId != session.sessionId) {
                continue;
            }
            if (entry.startTimeSec <= 0.0) {
                entry.startTimeSec = nowSec;
            }
            entry.endTimeSec = session.endTimeSec;
            entry.disposition = session.disposition;
            entry.companionExpireGameDays = session.companionExpireGameDays;
            entry.temporaryTeammateApplied = session.temporaryTeammateApplied;
        }

        spdlog::info(
            "TFDPacify: tame timer extend session={} target={:08X} addSec={:.2f} endTimeSec={:.2f} calmRefreshSec={:.2f}",
            session.sessionId,
            session.primaryTargetId,
            addSec,
            session.endTimeSec,
            session.lastCalmRefreshSec);
        return true;
    }

    double GetRemainingTameTime(RE::Actor* actor, double nowSec)
    {
        if (!actor) {
            return 0.0;
        }

        if (nowSec <= 0.0) {
            nowSec = PacifyNowSec();
        }

        auto entryIt = g_entries.find(actor->GetFormID());
        if (entryIt == g_entries.end()) {
            return 0.0;
        }

        auto sessionIt = g_sessions.find(entryIt->second.sessionId);
        if (sessionIt == g_sessions.end() || sessionIt->second.finished) {
            return 0.0;
        }

        const Session& session = sessionIt->second;
        if (session.primaryMode != Mode::Tame || session.disposition == TameDisposition::Companion || session.endTimeSec <= 0.0) {
            return 0.0;
        }

        return (std::max)(0.0, session.endTimeSec - nowSec);
    }

    TameDisposition GetDisposition(RE::Actor* actor)
    {
        if (!actor) {
            return TameDisposition::None;
        }

        auto entryIt = g_entries.find(actor->GetFormID());
        if (entryIt == g_entries.end()) {
            return TameDisposition::None;
        }

        auto sessionIt = g_sessions.find(entryIt->second.sessionId);
        if (sessionIt == g_sessions.end() || sessionIt->second.finished) {
            return entryIt->second.disposition;
        }

        return sessionIt->second.disposition;
    }

    bool IsCompanion(RE::Actor* actor)
    {
        return GetDisposition(actor) == TameDisposition::Companion;
    }

    bool PromoteActiveTameToCompanion(RE::Actor* actor, double addHoursGameTime)
    {
        if (!actor || addHoursGameTime <= 0.0) {
            return false;
        }

        auto entryIt = g_entries.find(actor->GetFormID());
        if (entryIt == g_entries.end()) {
            return false;
        }

        auto sessionIt = g_sessions.find(entryIt->second.sessionId);
        if (sessionIt == g_sessions.end() || sessionIt->second.finished) {
            return false;
        }

        Session& session = sessionIt->second;
        if (session.primaryMode != Mode::Tame || session.primaryTargetId != actor->GetFormID()) {
            return false;
        }

        const double nowGameDays = CurrentGameDays();
        const double addDays = addHoursGameTime / 24.0;
        const double maxDays = nowGameDays + (kCompanionMaxHours / 24.0);
        const bool wasCompanion = session.disposition == TameDisposition::Companion;
        const double baseGameDays = session.companionExpireGameDays > nowGameDays ? session.companionExpireGameDays : nowGameDays;
        const double oldExpireGameDays = session.companionExpireGameDays;
        const double newExpireGameDays = (std::min)(maxDays, baseGameDays + addDays);
        if (wasCompanion && newExpireGameDays <= oldExpireGameDays + 1e-6) {
            spdlog::info(
                "TFDPacify: reject promote companion session={} target={:08X} reason=companion_at_max expireGameDays={:.4f}",
                session.sessionId,
                session.primaryTargetId,
                oldExpireGameDays);
            return false;
        }

        if (!wasCompanion) {
            SendModEvent("TFDTameUnassign", actor);
            SendModEvent(kCreatureTeammateAssignEvent, actor);
        }

        session.disposition = TameDisposition::Companion;
        session.temporaryTeammateApplied = true;
        session.companionExpireGameDays = newExpireGameDays;
        session.endTimeSec = 0.0;
        session.lastCalmRefreshSec = 0.0;
        session.invalidSinceSec = 0.0;
        session.armedSinceSec = 0.0;
        session.tooFarSinceSec = 0.0;
        session.tameStartleSinceSec = 0.0;
        session.lastPlayerSampleSec = 0.0;
        session.lastPlayerPos = {};
        session.hasPlayerSample = false;

        SyncSessionDisposition(session);

        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            if (auto* process = RE::ProcessLists::GetSingleton()) {
                process->ClearCachedFactionFightReactions();
            }

            actor->EvaluatePackage(false, true);
            actor->EvaluatePackage(true, true);
            actor->UpdateCombat();
            player->UpdateCombat();
        }

        spdlog::info(
            "TFDPacify: promote companion session={} target={:08X} addHours={:.2f} expireGameDays={:.4f}",
            session.sessionId,
            session.primaryTargetId,
            addHoursGameTime,
            session.companionExpireGameDays);
        return true;
    }

    bool ExtendActiveCompanionHours(RE::Actor* actor, double addHoursGameTime)
    {
        if (!actor || addHoursGameTime <= 0.0) {
            return false;
        }

        auto entryIt = g_entries.find(actor->GetFormID());
        if (entryIt == g_entries.end()) {
            return false;
        }

        auto sessionIt = g_sessions.find(entryIt->second.sessionId);
        if (sessionIt == g_sessions.end() || sessionIt->second.finished) {
            return false;
        }

        Session& session = sessionIt->second;
        if (session.primaryMode != Mode::Tame || session.disposition != TameDisposition::Companion) {
            return false;
        }

        const double nowGameDays = CurrentGameDays();
        const double addDays = addHoursGameTime / 24.0;
        const double maxDays = nowGameDays + (kCompanionMaxHours / 24.0);
        const double baseGameDays = session.companionExpireGameDays > nowGameDays ? session.companionExpireGameDays : nowGameDays;
        const double oldExpireGameDays = session.companionExpireGameDays;
        const double newExpireGameDays = (std::min)(maxDays, baseGameDays + addDays);
        if (newExpireGameDays <= oldExpireGameDays + 1e-6) {
            spdlog::info(
                "TFDPacify: reject extend companion session={} target={:08X} reason=companion_at_max expireGameDays={:.4f}",
                session.sessionId,
                session.primaryTargetId,
                oldExpireGameDays);
            return false;
        }
        session.companionExpireGameDays = newExpireGameDays;
        session.invalidSinceSec = 0.0;
        session.armedSinceSec = 0.0;
        session.tooFarSinceSec = 0.0;
        session.tameStartleSinceSec = 0.0;
        session.lastPlayerSampleSec = 0.0;
        session.lastPlayerPos = {};
        session.hasPlayerSample = false;
        SyncSessionDisposition(session);

        spdlog::info(
            "TFDPacify: extend companion session={} target={:08X} addHours={:.2f} expireGameDays={:.4f}",
            session.sessionId,
            session.primaryTargetId,
            addHoursGameTime,
            session.companionExpireGameDays);
        return true;
    }

    bool ReleaseActiveTameActor(RE::Actor* actor, ReleaseReason reason)
    {
        if (!actor) {
            return false;
        }

        auto entryIt = g_entries.find(actor->GetFormID());
        if (entryIt == g_entries.end()) {
            return false;
        }

        auto sessionIt = g_sessions.find(entryIt->second.sessionId);
        if (sessionIt == g_sessions.end() || sessionIt->second.finished || sessionIt->second.primaryMode != Mode::Tame) {
            return false;
        }

        ReleaseSession(sessionIt->second.sessionId, reason);
        return true;
    }

    double GetRemainingCompanionHours(RE::Actor* actor)
    {
        if (!actor) {
            return 0.0;
        }

        auto entryIt = g_entries.find(actor->GetFormID());
        if (entryIt == g_entries.end()) {
            return 0.0;
        }

        auto sessionIt = g_sessions.find(entryIt->second.sessionId);
        if (sessionIt == g_sessions.end() || sessionIt->second.finished) {
            return 0.0;
        }

        const Session& session = sessionIt->second;
        if (session.primaryMode != Mode::Tame || session.disposition != TameDisposition::Companion || session.companionExpireGameDays <= 0.0) {
            return 0.0;
        }

        const double remainingDays = session.companionExpireGameDays - CurrentGameDays();
        return (std::max)(0.0, remainingDays * 24.0);
    }

    bool CanStartTruce(RE::Actor* actor)
    {
        if (!actor) {
            return false;
        }

        if (TFD::DefeatMonitor::IsDefeatedEnemyKnocked(actor)) {
            return false;
        }

        auto it = g_truceState.find(actor->GetFormID());
        if (it == g_truceState.end()) {
            return true;
        }

        return !it->second.spent;
    }

    bool HasSpentTruce(RE::Actor* actor)
    {
        if (!actor) {
            return false;
        }

        auto it = g_truceState.find(actor->GetFormID());
        return it != g_truceState.end() && it->second.spent;
    }

    bool WasTruceBetrayed(RE::Actor* actor)
    {
        if (!actor) {
            return false;
        }

        auto it = g_truceState.find(actor->GetFormID());
        return it != g_truceState.end() && it->second.betrayed;
    }

    void ReleaseSession(RE::FormID sessionId, ReleaseReason reason)
    {
        if (sessionId == 0) {
            return;
        }

        RE::FormID primaryTargetId = 0;
        Mode primaryMode = Mode::None;
        TameDisposition primaryDisposition = TameDisposition::None;

        auto sessionIt = g_sessions.find(sessionId);
        if (sessionIt != g_sessions.end()) {
            primaryTargetId = sessionIt->second.primaryTargetId;
            primaryMode = sessionIt->second.primaryMode;
            primaryDisposition = sessionIt->second.disposition;
        }

        std::vector<RE::FormID> actorIds;
        actorIds.reserve(g_entries.size());

        for (const auto& [actorId, entry] : g_entries) {
            if (entry.sessionId == sessionId) {
                actorIds.push_back(actorId);
            }
        }

        RE::Actor* player = nullptr;
        if (sessionIt != g_sessions.end()) {
            player = ResolveActor(sessionIt->second.playerId);
        }

        const double releaseNowSec = PacifyNowSec();

        for (RE::FormID actorId : actorIds) {
            auto it = g_entries.find(actorId);
            if (it == g_entries.end()) {
                continue;
            }

            Entry releasedEntry = it->second;
            if (auto* actor = ResolveActor(actorId)) {
                RemovePacify(actor, releasedEntry);

                const bool shouldRehostileTame =
                    releasedEntry.mode == Mode::Tame &&
                    releasedEntry.disposition != TameDisposition::Companion &&
                    player &&
                    (reason == ReleaseReason::PlayerArmed ||
                        reason == ReleaseReason::TameBroken ||
                        reason == ReleaseReason::TameExpired);

                const bool shouldRehostileTruce =
                    IsTruceMode(releasedEntry.mode) &&
                    player &&
                    (reason == ReleaseReason::DialogueClosed ||
                        reason == ReleaseReason::PlayerArmed);

                const bool immediateInCombatRehostile =
                    releasedEntry.mode == Mode::TruceInCombat &&
                    player &&
                    reason == ReleaseReason::DialogueClosed;

                if (immediateInCombatRehostile) {
                    g_entries.erase(it);
                    it = g_entries.end();
                }

                if (shouldRehostileTame) {
                    actor->EvaluatePackage(false, true);
                }
                else if (shouldRehostileTruce) {
                    const bool drawWeapon = true;
                    const bool satisfied = ForceRehostile(actor, player, reason, drawWeapon);
                    if (!satisfied || !actor->IsInCombat()) {
                        QueueRehostileRetry(actor, player, sessionId, reason, releaseNowSec, drawWeapon);
                    }
                }
            }

            if (it != g_entries.end()) {
                g_entries.erase(it);
            }
        }

        if (sessionIt != g_sessions.end()) {
            sessionIt->second.finished = true;
            g_sessions.erase(sessionIt);
        }

        RE::Actor* primaryActor = ResolveActor(primaryTargetId);
        if (IsTruceMode(primaryMode)) {
            if (primaryActor) {
                if (DoesReasonCountAsBetrayal(reason)) {
                    MarkTruceBetrayed(primaryActor);
                }
            }
        }

        const auto eventIds = IsTruceMode(primaryMode) ? SelectTruceEventTargets(actorIds, primaryTargetId) : SelectCrowdEventTargets(actorIds, player, primaryTargetId);
        const char* unassignEvent = (primaryMode == Mode::Tame && primaryDisposition == TameDisposition::Companion) ?
            kCreatureTeammateUnassignEvent :
            GetUnassignEventName(primaryMode);
        const auto sent = SendModEventToActors(unassignEvent, eventIds);
        spdlog::info(
            "TFDPacify: unassign events event={} session={} sent={} primary={:08X} disposition={}",
            unassignEvent ? unassignEvent : "<none>",
            sessionId,
            static_cast<unsigned int>(sent),
            primaryTargetId,
            ToString(primaryDisposition));

        if (const auto* supplementalUnassign = GetSupplementalUnassignEventName(primaryMode)) {
            const auto supplementalSent = SendModEventToActors(supplementalUnassign, eventIds);
            spdlog::info(
                "TFDPacify: supplemental unassign event={} session={} sent={} primary={:08X}",
                supplementalUnassign,
                sessionId,
                static_cast<unsigned int>(supplementalSent),
                primaryTargetId);
        }

        spdlog::info(
            "TFDPacify: release session id={} reason={} mode={} disposition={} target={:08X} packSize={}",
            sessionId,
            ToString(reason),
            ToString(primaryMode),
            ToString(primaryDisposition),
            primaryTargetId,
            static_cast<unsigned int>(actorIds.size()));
    }

    bool ReleaseActiveTruceSessionForActor(RE::Actor* actor, ReleaseReason reason, bool onlyIfStillHostile)
    {
        if (!actor) {
            return false;
        }

        auto entryIt = g_entries.find(actor->GetFormID());
        if (entryIt == g_entries.end()) {
            return false;
        }

        const auto sessionId = entryIt->second.sessionId;
        auto sessionIt = g_sessions.find(sessionId);
        if (sessionIt == g_sessions.end() || sessionIt->second.finished) {
            return false;
        }

        if (!IsTruceMode(sessionIt->second.primaryMode)) {
            return false;
        }

        if (onlyIfStillHostile) {
            const bool allowDialogueClosedInCombatRelease =
                sessionIt->second.primaryMode == Mode::TruceInCombat &&
                reason == ReleaseReason::DialogueClosed;
            if (!allowDialogueClosedInCombatRelease) {
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!IsEnemyToPlayer(player, actor)) {
                    return false;
                }
            }
        }

        spdlog::info(
            "TFDPacify: release active truce actor={:08X} session={} reason={}",
            actor->GetFormID(),
            sessionId,
            ToString(reason));
        ReleaseSession(sessionId, reason);
        return true;
    }

    void ReleaseAll()
    {
        std::vector<RE::FormID> sessionIds;
        sessionIds.reserve(g_sessions.size());

        for (const auto& [sessionId, _] : g_sessions) {
            sessionIds.push_back(sessionId);
        }

        for (RE::FormID sessionId : sessionIds) {
            ReleaseSession(sessionId, ReleaseReason::Generic);
        }

        SendModEvent("TFDTameClearAll", nullptr);
        SendModEvent("TFDPreCombatClearAll", nullptr);
        SendModEvent("TFDTruceClearAll", nullptr);
        SendModEvent("TFDInCombatClearAll", nullptr);

        g_entries.clear();
        g_sessions.clear();
        g_rehostileRequests.clear();
    }

    bool ForceDetectionAndCombatRefresh(RE::Actor* actor, RE::Actor* player, ReleaseReason reason, bool drawWeapon)
    {
        return ForceRehostile(actor, player, reason, drawWeapon);
    }

    void QueueDetectionAndCombatRefresh(RE::Actor* actor, RE::Actor* player, ReleaseReason reason, bool drawWeapon)
    {
        if (!actor || !player) {
            return;
        }

        if (ForceRehostile(actor, player, reason, drawWeapon)) {
            return;
        }

        QueueRehostileRetry(actor, player, 0, reason, PacifyNowSec(), drawWeapon);
    }

    const char* ToString(Mode mode)
    {
        switch (mode) {
        case Mode::None:
            return "None";
        case Mode::Tame:
            return "Tame";
        case Mode::TrucePreCombat:
            return "TrucePreCombat";
        case Mode::TruceInCombat:
            return "TruceInCombat";
        default:
            return "Unknown";
        }
    }

    const char* ToString(ReleaseReason reason)
    {
        switch (reason) {
        case ReleaseReason::Generic:
            return "Generic";
        case ReleaseReason::HardFailsafeExpired:
            return "HardFailsafeExpired";
        case ReleaseReason::InvalidActor:
            return "InvalidActor";
        case ReleaseReason::PlayerAggression:
            return "PlayerAggression";
        case ReleaseReason::PlayerArmed:
            return "PlayerArmed";
        case ReleaseReason::DialogueClosed:
            return "DialogueClosed";
        case ReleaseReason::TooFar:
            return "TooFar";
        case ReleaseReason::TameBroken:
            return "TameBroken";
        case ReleaseReason::TameExpired:
            return "TameExpired";
        default:
            return "Unknown";
        }
    }

    const char* ToString(TameDisposition disposition)
    {
        switch (disposition) {
        case TameDisposition::None:
            return "None";
        case TameDisposition::Calm:
            return "Calm";
        case TameDisposition::Companion:
            return "Companion";
        default:
            return "Unknown";
        }
    }
}
