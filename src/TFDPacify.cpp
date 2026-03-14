#include "TFDPacify.h"

#include <optional>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

namespace TFD::Pacify
{
    namespace
    {
        std::unordered_map<RE::FormID, Entry> g_entries;
        std::unordered_map<RE::FormID, Session> g_sessions;
        RE::FormID g_nextSessionId = 1;

        constexpr double kTameDurationSec = 4.0;
        constexpr double kTrucePreCombatDurationSec = 8.0;
        constexpr double kTruceInCombatDurationSec = 6.0;

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

            return true;
        }

        bool IsEntryExpired(const Entry& entry, double nowSec)
        {
            return nowSec >= entry.endTimeSec;
        }

        void ApplyPacify(RE::Actor* actor, Entry& entry)
        {
            if (!IsActorStillValid(actor)) {
                return;
            }

            actor->StopCombat();
            (void)entry;
        }

        void RemovePacify(RE::Actor* actor, Entry& entry)
        {
            (void)actor;
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
            bool allowDialogue)
        {
            if (!IsActorStillValid(player) || !IsActorStillValid(primaryTarget)) {
                return std::nullopt;
            }

            const RE::FormID sessionId = g_nextSessionId++;
            const RE::FormID playerId = player->GetFormID();
            const RE::FormID targetId = primaryTarget->GetFormID();

            Session session;
            session.sessionId = sessionId;
            session.playerId = playerId;
            session.primaryTargetId = targetId;
            session.primaryMode = mode;
            session.startTimeSec = nowSec;
            session.endTimeSec = nowSec + durationSec;
            session.dialogueRequested = false;
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

            g_sessions[sessionId] = session;

            auto it = g_entries.find(targetId);
            if (it != g_entries.end()) {
                ApplyPacify(primaryTarget, it->second);
            }

            spdlog::info(
                "TFDPacify: begin session id={} mode={} target={:08X}",
                sessionId,
                ToString(mode),
                targetId);

            return sessionId;
        }
    }

    void Reset()
    {
        ReleaseAll();
        g_nextSessionId = 1;
    }

    void Update(double nowSec)
    {
        std::vector<RE::FormID> sessionsToRelease;
        sessionsToRelease.reserve(g_sessions.size());

        for (auto& [sessionId, session] : g_sessions) {
            if (session.finished || nowSec >= session.endTimeSec) {
                sessionsToRelease.push_back(sessionId);
                continue;
            }

            auto* primaryTarget = ResolveActor(session.primaryTargetId);
            if (!IsActorStillValid(primaryTarget)) {
                sessionsToRelease.push_back(sessionId);
                continue;
            }
        }

        for (RE::FormID sessionId : sessionsToRelease) {
            ReleaseSession(sessionId);
        }

        std::vector<RE::FormID> entriesToErase;
        entriesToErase.reserve(g_entries.size());

        for (auto& [actorId, entry] : g_entries) {
            auto* actor = ResolveActor(actorId);
            if (!IsActorStillValid(actor)) {
                entriesToErase.push_back(actorId);
                continue;
            }

            if (IsEntryExpired(entry, nowSec)) {
                entriesToErase.push_back(actorId);
                continue;
            }

            auto sessionIt = g_sessions.find(entry.sessionId);
            if (sessionIt == g_sessions.end() || sessionIt->second.finished) {
                entriesToErase.push_back(actorId);
                continue;
            }

            ApplyPacify(actor, entry);
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
            false);
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
            kTrucePreCombatDurationSec,
            true);
    }

    std::optional<RE::FormID> BeginTruceInCombatSession(
        RE::Actor* player,
        RE::Actor* primaryTarget,
        double nowSec)
    {
        return BeginSessionCommon(
            player,
            primaryTarget,
            Mode::TruceInCombat,
            nowSec,
            kTruceInCombatDurationSec,
            true);
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

    void ReleaseSession(RE::FormID sessionId)
    {
        if (sessionId == 0) {
            return;
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

        auto sessionIt = g_sessions.find(sessionId);
        if (sessionIt != g_sessions.end()) {
            sessionIt->second.finished = true;
            g_sessions.erase(sessionIt);
        }

        spdlog::info("TFDPacify: release session id={}", sessionId);
    }

    void ReleaseAll()
    {
        std::vector<RE::FormID> sessionIds;
        sessionIds.reserve(g_sessions.size());

        for (const auto& [sessionId, _] : g_sessions) {
            sessionIds.push_back(sessionId);
        }

        for (RE::FormID sessionId : sessionIds) {
            ReleaseSession(sessionId);
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
}