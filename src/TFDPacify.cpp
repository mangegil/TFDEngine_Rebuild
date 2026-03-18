#include "TFDPacify.h"

#include "TFDActorScan.h"
#include "TFDSettings.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>
#include <SKSE/SKSE.h>

namespace TFD::Pacify
{
    namespace
    {
        struct TruceState
        {
            bool spent{ false };
            bool betrayed{ false };
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
            case Mode::TruceInCombat:
                return "TFDTruceUnassign";
            default:
                return nullptr;
            }
        }

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

        constexpr double kTameDurationSec = 60.0;
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
        constexpr std::size_t kCrowdAliasCap = 10;
        constexpr float kTruceActiveCombatRadius = 3500.0f;
        constexpr float kTrucePrimaryLinkRadius = 2400.0f;

        RE::Actor* ResolveCurrentCombatTarget(RE::Actor* actor);
        bool IsEnemyToPlayer(RE::Actor* player, RE::Actor* actor);
        bool ForceRehostile(RE::Actor* actor, RE::Actor* player, ReleaseReason reason, bool drawWeapon);
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

        bool IsActorStillValid(RE::Actor* actor);

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

        float GetLocalHostileSplashRadius()
        {
            const float settingsRadius = TFD::Settings::GetSweepRadius();
            return std::clamp(settingsRadius, kLocalHostileSplashRadiusMin, kLocalHostileSplashRadiusMax);
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

        void RefreshSessionEntries(Session& session, double nowSec, double durationSec)
        {
            (void)durationSec;

            session.startTimeSec = nowSec;
            session.endTimeSec = 0.0;
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
                entry.startTimeSec = nowSec;
                entry.endTimeSec = 0.0;
            }
        }

        void ApplyPacify(RE::Actor* actor, Entry& entry, double nowSec)
        {
            if (!IsActorStillValid(actor)) {
                return;
            }

            const bool preserveThreatMemory = IsTruceMode(entry.mode);

            if ((nowSec - entry.lastPacifyApplySec) >= kPacifyApplyIntervalSec) {
                if (auto* process = RE::ProcessLists::GetSingleton()) {
                    const bool runDetection = process->runDetection;
                    process->runDetection = false;

                    // Truce should hold combat temporarily without erasing the actor's
                    // alarm/hostility context. Repeated StopCombatAndAlarmOnActor()
                    // was causing hostile-but-blind behavior after dialogue closed.
                    if (!preserveThreatMemory) {
                        process->ClearCachedFactionFightReactions();
                        process->StopCombatAndAlarmOnActor(actor, false);
                    }

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

        bool ForceRehostile(RE::Actor* actor, RE::Actor* player, ReleaseReason reason, bool drawWeapon)
        {
            if (!IsActorStillValid(actor) || !IsActorStillValid(player)) {
                return false;
            }

            if (g_entries.find(actor->GetFormID()) != g_entries.end()) {
                return false;
            }

            if (auto* process = RE::ProcessLists::GetSingleton()) {
                process->ClearCachedFactionFightReactions();
            }

            actor->SetBeenAttacked(true);
            player->SetBeenAttacked(true);

            actor->RequestDetectionLevel(player, RE::DETECTION_PRIORITY::kCritical);
            player->RequestDetectionLevel(actor, RE::DETECTION_PRIORITY::kCritical);

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

        void QueueRehostileRetry(RE::Actor* actor, RE::Actor* player, RE::FormID sessionId, ReleaseReason reason, double nowSec, bool drawWeapon)
        {
            if (!actor || !player) {
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
            bool isPrimaryTarget)
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
            entry.lastPacifyApplySec = 0.0;
            entry.lastPackageEvalSec = 0.0;
            entry.allowDialogue = allowDialogue;
            entry.isPrimaryTarget = isPrimaryTarget;

            g_rehostileRequests.erase(entry.actorId);
            g_entries[entry.actorId] = entry;
            return true;
        }

        ReleaseReason ComputeInvalidReason(Session& session, double nowSec)
        {
            auto* player = ResolveActor(session.playerId);
            auto* primaryTarget = ResolveActor(session.primaryTargetId);

            if (!IsActorStillValid(player) || !IsActorStillValid(primaryTarget)) {
                return ReleaseReason::InvalidActor;
            }

            if ((nowSec - session.startTimeSec) >= kArmedGraceSec && IsPlayerArmedForPacify(player)) {
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

            const bool canStartle =
                session.primaryMode == Mode::Tame &&
                (nowSec - session.startTimeSec) >= kTameStartleGraceSec;

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
            float cellBubbleRadius)
        {
            (void)nowSec;
            if (!IsActorStillValid(player) || !IsActorStillValid(primaryTarget)) {
                return std::nullopt;
            }

            nowSec = PacifyNowSec();
            const double effectiveDurationSec = 0.0;
            (void)durationSec;

            if (IsTruceMode(mode)) {
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
                spdlog::info(
                    "TFDPacify: reject session mode={} target={:08X} reason=player_armed",
                    ToString(mode),
                    primaryTarget->GetFormID());
                return std::nullopt;
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

            const RE::FormID sessionId = g_nextSessionId++;
            const RE::FormID playerId = player->GetFormID();
            const RE::FormID targetId = primaryTarget->GetFormID();

            Session session;
            session.sessionId = sessionId;
            session.playerId = playerId;
            session.primaryTargetId = targetId;
            session.primaryMode = mode;
            session.pendingReleaseReason = ReleaseReason::Generic;
            session.startTimeSec = nowSec;
            session.endTimeSec = 0.0;
            session.invalidSinceSec = 0.0;
            session.armedSinceSec = 0.0;
            session.tooFarSinceSec = 0.0;
            session.tameStartleSinceSec = 0.0;
            session.lastPlayerSampleSec = 0.0;
            session.lastPlayerPos = {};
            session.hasPlayerSample = false;
            session.dialogueRequested = allowDialogue;
            session.dialogueOpened = false;
            session.finished = false;

            if (!AddOrRefreshEntry(
                primaryTarget,
                mode,
                sessionId,
                targetId,
                nowSec,
                0.0,
                allowDialogue,
                true)) {
                return std::nullopt;
            }

            std::size_t cellBubbleCount = 0;
            std::size_t localSplashCount = 0;
            std::size_t truceClusterCount = 0;
            std::vector<RE::FormID> curatedTruceIds;
            if (mode == Mode::TruceInCombat) {
                const float scanRadius = (std::max)(GetCellBubbleRadius(cellBubbleRadius), 6000.0f);
                curatedTruceIds = BuildTruceInCombatMemberIds(player, primaryTarget, scanRadius, cellBubbleCount, truceClusterCount);
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
                        0.0,
                        allowDialogue,
                        isPrimary);
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
                        0.0,
                        allowDialogue,
                        isPrimary)) {
                        continue;
                    }

                    if (!existedInSession) {
                        ++cellBubbleCount;
                    }
                }
            }

            if (mode == Mode::Tame) {
                const float splashRadius = GetLocalHostileSplashRadius();
                const float scanRadius = splashRadius + 256.0f;
                TFD::ActorScan::Rescan(scanRadius, false);
                const auto count = TFD::ActorScan::GetCount();
                for (int i = 0; i < count; ++i) {
                    auto scanEntry = TFD::ActorScan::GetEntry(i);
                    auto* actor = TFD::ActorScan::GetActor(i);
                    if (!IsEligibleLocalSplashActor(actor, player, primaryTarget, scanEntry, splashRadius)) {
                        continue;
                    }

                    const RE::FormID actorId = actor->GetFormID();
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
                        0.0,
                        allowDialogue,
                        false)) {
                        continue;
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
                "TFDPacify: begin session id={} mode={} target={:08X} cellBubble={} localSplash={} packSize={} allowDialogue={}",
                sessionId,
                ToString(mode),
                targetId,
                static_cast<unsigned int>(cellBubbleCount),
                static_cast<unsigned int>(localSplashCount + truceClusterCount),
                static_cast<unsigned int>(packSize),
                allowDialogue ? 1 : 0);

            const auto eventIds = IsTruceMode(mode) ? applyIds : SelectCrowdEventTargets(applyIds, player, targetId);
            const auto sent = SendModEventToActors(GetAssignEventName(mode), eventIds);
            spdlog::info(
                "TFDPacify: assign events event={} session={} sent={} primary={:08X}",
                GetAssignEventName(mode) ? GetAssignEventName(mode) : "<none>",
                sessionId,
                static_cast<unsigned int>(sent),
                targetId);
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
        bool allowDialogue)
    {
        return BeginSessionCommon(
            player,
            primaryTarget,
            Mode::Tame,
            nowSec,
            0.0,
            allowDialogue,
            false,
            0.0f);
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
            12000.0f);
    }

    std::optional<RE::FormID> BeginTruceInCombatSession(
        RE::Actor* player,
        RE::Actor* primaryTarget,
        double nowSec,
        bool allowDialogue)
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
            cellBubbleRadius);
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
            0.0,
            false,
            true,
            radius);
    }

    bool IsPacified(RE::Actor* actor)
    {
        if (!actor) {
            return false;
        }

        return g_entries.find(actor->GetFormID()) != g_entries.end();
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
        if (it == g_entries.end()) {
            return false;
        }

        const Entry& entry = it->second;
        return entry.allowDialogue;
    }

    bool CanStartTruce(RE::Actor* actor)
    {
        if (!actor) {
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

        auto sessionIt = g_sessions.find(sessionId);
        if (sessionIt != g_sessions.end()) {
            primaryTargetId = sessionIt->second.primaryTargetId;
            primaryMode = sessionIt->second.primaryMode;
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
                    player &&
                    (reason == ReleaseReason::PlayerArmed ||
                        reason == ReleaseReason::TameBroken);

                const bool shouldRehostileTruce =
                    IsTruceMode(releasedEntry.mode) &&
                    player &&
                    (reason == ReleaseReason::DialogueClosed ||
                        reason == ReleaseReason::PlayerArmed);

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

            g_entries.erase(it);
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

        const auto eventIds = IsTruceMode(primaryMode) ? actorIds : SelectCrowdEventTargets(actorIds, player, primaryTargetId);
        const auto sent = SendModEventToActors(GetUnassignEventName(primaryMode), eventIds);
        spdlog::info(
            "TFDPacify: unassign events event={} session={} sent={} primary={:08X}",
            GetUnassignEventName(primaryMode) ? GetUnassignEventName(primaryMode) : "<none>",
            sessionId,
            static_cast<unsigned int>(sent),
            primaryTargetId);

        spdlog::info(
            "TFDPacify: release session id={} reason={} mode={} target={:08X} packSize={}",
            sessionId,
            ToString(reason),
            ToString(primaryMode),
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
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!IsEnemyToPlayer(player, actor)) {
                return false;
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
        SendModEvent("TFDTruceClearAll", nullptr);

        g_entries.clear();
        g_sessions.clear();
        g_rehostileRequests.clear();
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
        default:
            return "Unknown";
        }
    }
}
