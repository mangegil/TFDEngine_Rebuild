#include "TFDPacify.h"

#include "TFDActorScan.h"
#include "TFDSettings.h"

#include <algorithm>

#include <optional>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

namespace TFD::Pacify
{
    namespace
    {
        struct TruceState
        {
            bool spent{ false };
            bool betrayed{ false };
        };

        std::unordered_map<RE::FormID, Entry> g_entries;
        std::unordered_map<RE::FormID, Session> g_sessions;
        std::unordered_map<RE::FormID, TruceState> g_truceState;
        RE::FormID g_nextSessionId = 1;

        constexpr double kTameDurationSec = 10.0;
        constexpr double kTruceHiddenFailsafeSec = 120.0;

        constexpr double kPacifyApplyIntervalSec = 0.25;
        constexpr double kPackageEvalIntervalSec = 1.0;

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

            return scanEntry.hostile || scanEntry.inCombat || actor->IsInCombat();
        }

        bool IsPlayerArmedForTruce(RE::Actor* player)
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

        void MarkTruceSpent(RE::Actor* actor)
        {
            if (!actor) {
                return;
            }

            auto& state = g_truceState[actor->GetFormID()];
            state.spent = true;
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
            case ReleaseReason::PlayerAggression:
            case ReleaseReason::PlayerArmed:
            case ReleaseReason::DialogueClosed:
                return true;
            default:
                return false;
            }
        }

        ReleaseReason ComputeInvalidReason(Session& session, double nowSec)
        {
            auto* player = ResolveActor(session.playerId);
            auto* primaryTarget = ResolveActor(session.primaryTargetId);

            if (!IsActorStillValid(player) || !IsActorStillValid(primaryTarget)) {
                return ReleaseReason::InvalidActor;
            }

            if (!IsTruceMode(session.primaryMode)) {
                session.invalidSinceSec = 0.0;
                return ReleaseReason::Generic;
            }

            if (IsPlayerArmedForTruce(player)) {
                return ReleaseReason::PlayerArmed;
            }

            session.invalidSinceSec = 0.0;
            return ReleaseReason::Generic;
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
            if (!IsActorStillValid(player) || !IsActorStillValid(primaryTarget)) {
                return std::nullopt;
            }

            if (IsTruceMode(mode)) {
                if (!CanStartTruce(primaryTarget)) {
                    spdlog::info(
                        "TFDPacify: reject session mode={} target={:08X} reason=truce_spent",
                        ToString(mode),
                        primaryTarget->GetFormID());
                    return std::nullopt;
                }

                if (IsPlayerArmedForTruce(player)) {
                    spdlog::info(
                        "TFDPacify: reject session mode={} target={:08X} reason=player_armed",
                        ToString(mode),
                        primaryTarget->GetFormID());
                    return std::nullopt;
                }
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
            session.endTimeSec = nowSec + durationSec;
            session.invalidSinceSec = 0.0;
            session.dialogueRequested = allowDialogue;
            session.dialogueOpened = false;
            session.finished = false;

            if (!AddOrRefreshEntry(
                    primaryTarget,
                    mode,
                    sessionId,
                    targetId,
                    nowSec,
                    nowSec + durationSec,
                    allowDialogue,
                    true)) {
                return std::nullopt;
            }

            std::size_t cellBubbleCount = 0;
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
                            nowSec + durationSec,
                            isPrimary ? allowDialogue : false,
                            isPrimary)) {
                        continue;
                    }

                    ++cellBubbleCount;
                }
            }

            g_sessions[sessionId] = session;

            std::vector<RE::FormID> applyIds;
            applyIds.reserve(g_entries.size());
            for (const auto& [actorId, entry] : g_entries) {
                if (entry.sessionId == sessionId) {
                    applyIds.push_back(actorId);
                }
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

            spdlog::info(
                "TFDPacify: begin session id={} mode={} target={:08X} cellBubble={}",
                sessionId,
                ToString(mode),
                targetId,
                static_cast<unsigned int>(cellBubbleCount));

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
        std::vector<std::pair<RE::FormID, ReleaseReason>> sessionsToRelease;
        sessionsToRelease.reserve(g_sessions.size());

        for (auto& [sessionId, session] : g_sessions) {
            if (session.finished) {
                sessionsToRelease.emplace_back(sessionId, ReleaseReason::Generic);
                continue;
            }

            if (nowSec >= session.endTimeSec) {
                sessionsToRelease.emplace_back(sessionId, ReleaseReason::HardFailsafeExpired);
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

            if (nowSec >= entry.endTimeSec) {
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
        double nowSec)
    {
        return BeginSessionCommon(
            player,
            primaryTarget,
            Mode::Tame,
            nowSec,
            kTameDurationSec,
            false,
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
            kTruceHiddenFailsafeSec,
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
            kTruceHiddenFailsafeSec,
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
            (std::max)(1.0, durationSec),
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
        return entry.isPrimaryTarget && entry.allowDialogue;
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

        for (RE::FormID actorId : actorIds) {
            auto it = g_entries.find(actorId);
            if (it == g_entries.end()) {
                continue;
            }

            if (auto* actor = ResolveActor(actorId)) {
                RemovePacify(actor, it->second);
            }

            g_entries.erase(it);
        }

        if (sessionIt != g_sessions.end()) {
            sessionIt->second.finished = true;
            g_sessions.erase(sessionIt);
        }

        if (IsTruceMode(primaryMode)) {
            if (auto* actor = ResolveActor(primaryTargetId)) {
                if (DoesReasonCountAsBetrayal(reason)) {
                    MarkTruceBetrayed(actor);
                }
            }
        }

        spdlog::info(
            "TFDPacify: release session id={} reason={} mode={} target={:08X}",
            sessionId,
            ToString(reason),
            ToString(primaryMode),
            primaryTargetId);
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
