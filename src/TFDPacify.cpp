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

        using Clock = std::chrono::steady_clock;

        std::unordered_map<RE::FormID, Entry> g_entries;
        std::unordered_map<RE::FormID, Session> g_sessions;
        std::unordered_map<RE::FormID, TruceState> g_truceState;
        RE::FormID g_nextSessionId = 1;

        RE::Actor* ResolveActor(RE::FormID actorId);

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

        std::vector<RE::FormID> CollectSessionActorIds(RE::FormID sessionId, RE::FormID primaryTargetId)
        {
            std::vector<RE::FormID> actorIds;
            actorIds.reserve(g_entries.size());

            for (const auto& [actorId, entry] : g_entries) {
                if (entry.sessionId == sessionId) {
                    actorIds.push_back(actorId);
                }
            }

            std::sort(actorIds.begin(), actorIds.end(), [primaryTargetId](RE::FormID a, RE::FormID b) {
                if (a == primaryTargetId) {
                    return true;
                }
                if (b == primaryTargetId) {
                    return false;
                }
                return a < b;
            });

            return actorIds;
        }

        void DispatchAssignEventsForSession(Mode mode, RE::FormID sessionId, RE::FormID primaryTargetId)
        {
            const char* eventName = GetAssignEventName(mode);
            if (!eventName) {
                return;
            }

            const auto actorIds = CollectSessionActorIds(sessionId, primaryTargetId);
            if (actorIds.empty()) {
                return;
            }

            if (mode != Mode::Tame) {
                if (auto* primaryActor = ResolveActor(primaryTargetId)) {
                    SendModEvent(eventName, primaryActor);
                }
                return;
            }

            for (RE::FormID actorId : actorIds) {
                if (auto* actor = ResolveActor(actorId)) {
                    SendModEvent(eventName, actor);
                }
            }
        }

        void DispatchUnassignEventsForSession(Mode mode, const std::vector<RE::FormID>& actorIds, RE::FormID primaryTargetId)
        {
            const char* eventName = GetUnassignEventName(mode);
            if (!eventName) {
                return;
            }

            if (mode != Mode::Tame) {
                if (auto* primaryActor = ResolveActor(primaryTargetId)) {
                    SendModEvent(eventName, primaryActor);
                }
                return;
            }

            for (RE::FormID actorId : actorIds) {
                if (auto* actor = ResolveActor(actorId)) {
                    SendModEvent(eventName, actor);
                }
            }
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

        constexpr double kArmedGraceSec = 1.25;
        constexpr double kArmedDebounceSec = 0.50;
        constexpr double kTooFarDebounceSec = 1.25;
        constexpr double kTameStartleDebounceSec = 0.35;

        constexpr float kTameStartleDistance = 180.0f;
        constexpr float kTameStartleRushSpeedPerSec = 260.0f;
        constexpr float kTruceNonDialogueMaxDistance = 2200.0f;

        RE::Actor* ResolveActor(RE::FormID actorId)
        {
            if (actorId == 0) {
                return nullptr;
            }

            return RE::TESForm::LookupByID<RE::Actor>(actorId);
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

        RE::Actor* ResolveCurrentCombatTarget(RE::Actor* actor);
        bool IsEnemyToPlayer(RE::Actor* player, RE::Actor* actor);

        float GetLocalHostileSplashRadius()
        {
            const float settingsRadius = TFD::Settings::GetSweepRadius();
            return std::clamp(settingsRadius, kLocalHostileSplashRadiusMin, kLocalHostileSplashRadiusMax);
        }

        bool AreMutuallyNonHostile(RE::Actor* a, RE::Actor* b)
        {
            if (!a || !b) {
                return false;
            }

            return !a->IsHostileToActor(b) && !b->IsHostileToActor(a);
        }

        bool SharesCurrentCombatTarget(RE::Actor* a, RE::Actor* b)
        {
            auto* aTarget = ResolveCurrentCombatTarget(a);
            auto* bTarget = ResolveCurrentCombatTarget(b);
            if (!aTarget || !bTarget) {
                return false;
            }

            return aTarget->GetFormID() == bTarget->GetFormID();
        }

        bool IsEligibleLocalSplashActor(
            RE::Actor* actor,
            RE::Actor* player,
            RE::Actor* primaryTarget,
            Mode mode,
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
            if (!pCell || actor->GetParentCell() != pCell || primaryTarget->GetParentCell() != pCell) {
                return false;
            }

            if (!IsEnemyToPlayer(player, actor)) {
                return false;
            }

            const float distToPrimary = actor->GetPosition().GetDistance(primaryTarget->GetPosition());
            if (distToPrimary > radius) {
                return false;
            }

            if (mode != Mode::Tame) {
                return true;
            }

            if (!AreMutuallyNonHostile(actor, primaryTarget)) {
                return false;
            }

            if (SharesCurrentCombatTarget(actor, primaryTarget)) {
                return true;
            }

            auto* primaryCombatTarget = ResolveCurrentCombatTarget(primaryTarget);
            if (primaryCombatTarget && primaryCombatTarget->GetFormID() == player->GetFormID()) {
                return true;
            }

            auto* actorCombatTarget = ResolveCurrentCombatTarget(actor);
            if (actorCombatTarget && actorCombatTarget->GetFormID() == player->GetFormID()) {
                return true;
            }

            return false;
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

            if ((nowSec - entry.lastPacifyApplySec) >= kPacifyApplyIntervalSec) {
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

            const bool startled =
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
            if (applyCellBubble) {
                const float scanRadius = GetCellBubbleRadius(cellBubbleRadius);
                TFD::ActorScan::Rescan(scanRadius, false);
                const auto count = TFD::ActorScan::GetCount();
                for (int i = 0; i < count; ++i) {
                    auto scanEntry = TFD::ActorScan::GetEntry(i);
                    auto* actor = TFD::ActorScan::GetActor(i);
                    if (!IsEligibleCellBubbleActor(actor, player, primaryTarget, scanEntry)) {
                        continue;
                    }

                    const bool isPrimary = actor->GetFormID() == targetId;
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

                    ++cellBubbleCount;
                }
            }
            else if (mode == Mode::Tame || mode == Mode::TruceInCombat) {
                const float splashRadius = GetLocalHostileSplashRadius();
                const float scanRadius = splashRadius + 256.0f;
                TFD::ActorScan::Rescan(scanRadius, false);
                const auto count = TFD::ActorScan::GetCount();
                for (int i = 0; i < count; ++i) {
                    auto scanEntry = TFD::ActorScan::GetEntry(i);
                    auto* actor = TFD::ActorScan::GetActor(i);
                    if (!IsEligibleLocalSplashActor(actor, player, primaryTarget, mode, scanEntry, splashRadius)) {
                        continue;
                    }

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

                    ++localSplashCount;
                }
            }

            g_sessions[sessionId] = session;

            auto applyIds = CollectSessionActorIds(sessionId, targetId);

            for (RE::FormID actorId : applyIds) {
                auto it = g_entries.find(actorId);
                if (it == g_entries.end()) {
                    continue;
                }
                if (auto* actor = ResolveActor(actorId)) {
                    ApplyPacify(actor, it->second, nowSec);
                }
            }

            spdlog::info(
                "TFDPacify: begin session id={} mode={} target={:08X} cellBubble={} localSplash={} packSize={} allowDialogue={}",
                sessionId,
                ToString(mode),
                targetId,
                static_cast<unsigned int>(cellBubbleCount),
                static_cast<unsigned int>(localSplashCount),
                static_cast<unsigned int>(applyIds.size()),
                allowDialogue ? 1 : 0);

            DispatchAssignEventsForSession(mode, sessionId, targetId);
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

        auto actorIds = CollectSessionActorIds(sessionId, primaryTargetId);

        RE::Actor* player = nullptr;
        if (sessionIt != g_sessions.end()) {
            player = ResolveActor(sessionIt->second.playerId);
        }

        for (RE::FormID actorId : actorIds) {
            auto it = g_entries.find(actorId);
            if (it == g_entries.end()) {
                continue;
            }

            Entry releasedEntry = it->second;
            if (auto* actor = ResolveActor(actorId)) {
                RemovePacify(actor, releasedEntry);

                const bool shouldRehostile =
                    releasedEntry.mode == Mode::Tame &&
                    player &&
                    (reason == ReleaseReason::PlayerArmed ||
                        reason == ReleaseReason::TameBroken);

                if (shouldRehostile) {
                    actor->EvaluatePackage(false, true);
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

        DispatchUnassignEventsForSession(primaryMode, actorIds, primaryTargetId);

        spdlog::info(
            "TFDPacify: release session id={} reason={} mode={} target={:08X} packSize={}",
            sessionId,
            ToString(reason),
            ToString(primaryMode),
            primaryTargetId,
            static_cast<unsigned int>(actorIds.size()));
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
