#include "TFDRecruit.h"

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    struct NamedFaction
    {
        const char* editorID{ nullptr };
        RE::TESFaction* faction{ nullptr };
        bool hostileSource{ false };
        bool playerSideState{ false };
    };

    constexpr double kRecruitObserveThrottleSec = 3.00;
    constexpr double kRecruitCommitThrottleSec = 10.00;

    constexpr const char* kHostileSourceFactionEditorIDs[] = {
        "BanditFaction",
        "ForswornFaction",
        "NecromancerFaction",
        "WarlockFaction",
        "WitchFaction",
        "VampireFaction",
        "DLC1VampireFaction",
        "SilverHandFaction",
        "ThalmorFaction",
        "AlikrFaction",
        "BloodHorkerFaction",
        "MS06BanditFaction",
        "WEPlayerEnemyFaction",
        "dunMistwatchBanditFaction",
        "dunCragslaneFaction",
        "dunTrevasBanditFaction",
        "dunFellglowWarlockFaction",
        "dunBrokenOarFaction",
        "dunWhiteRiverFaction",
        "dunValtheimFaction",
        "dunBannermistFaction",
        "dunHaltedStreamFaction"
    };

    constexpr const char* kPlayerSideStateFactionEditorIDs[] = {
        "TFDTeammateFaction",
        "TFDTruceTeammateFaction",
        "TFDPacifyFaction",
        "TFDPreCombatTruceFaction",
        "TFDInCombatTruceFaction",
        "TFDBleedOutFaction",
        "TFDBleedoutFaction",
        "TFDCaptiveFaction",
        "TFDWorkingCaptiveFaction",
        "TFDAfterPleasureFaction",
        "TFDSaviorFaction",
        "CurrentFollowerFaction",
        "PlayerFollowerFaction",
        "PotentialFollowerFaction"
    };

    std::once_flag g_resolveOnce;
    std::vector<NamedFaction> g_factions;
    std::mutex g_logLock;
    std::unordered_map<RE::FormID, double> g_nextObserveLogSec;
    std::unordered_map<RE::FormID, double> g_nextCommitLogSec;
    std::unordered_set<RE::FormID> g_committedCleanActors;
    std::unordered_set<RE::FormID> g_quarantineAttemptedActors;

    double NowSec()
    {
        static const auto t0 = Clock::now();
        return std::chrono::duration<double>(Clock::now() - t0).count();
    }

    RE::PlayerCharacter* Player()
    {
        return RE::PlayerCharacter::GetSingleton();
    }

    RE::Actor* ResolveCurrentCombatTarget(RE::Actor* actor)
    {
        if (!actor) {
            return nullptr;
        }
        auto sp = actor->GetActorRuntimeData().currentCombatTarget.get();
        return sp.get();
    }

    void AddFactionByEditorID(const char* editorID, bool hostileSource, bool playerSideState)
    {
        if (!editorID || !editorID[0]) {
            return;
        }

        auto* faction = RE::TESForm::LookupByEditorID<RE::TESFaction>(editorID);
        if (!faction) {
            spdlog::warn("[TFD][Recruit] faction unresolved editorID={} group={}",
                editorID,
                hostileSource ? "hostile" : "state");
            return;
        }

        const auto it = std::find_if(g_factions.begin(), g_factions.end(), [faction](const NamedFaction& entry) {
            return entry.faction == faction;
            });
        if (it != g_factions.end()) {
            it->hostileSource = it->hostileSource || hostileSource;
            it->playerSideState = it->playerSideState || playerSideState;
            return;
        }

        g_factions.push_back({ editorID, faction, hostileSource, playerSideState });
    }

    void ResolveFactions()
    {
        std::call_once(g_resolveOnce, []() {
            g_factions.reserve(std::size(kHostileSourceFactionEditorIDs) + std::size(kPlayerSideStateFactionEditorIDs));
            for (const char* editorID : kHostileSourceFactionEditorIDs) {
                AddFactionByEditorID(editorID, true, false);
            }
            for (const char* editorID : kPlayerSideStateFactionEditorIDs) {
                AddFactionByEditorID(editorID, false, true);
            }
            spdlog::info("[TFD][Recruit] registry resolved factions={}", static_cast<unsigned>(g_factions.size()));
            });
    }

    std::int32_t GetExactFactionRank(RE::Actor* actor, RE::TESFaction* faction)
    {
        if (!actor || !faction) {
            return -2;
        }
        return actor->GetFactionRank(faction, false);
    }

    bool HasFactionAnyRank(RE::Actor* actor, RE::TESFaction* faction)
    {
        return GetExactFactionRank(actor, faction) > -2;
    }

    bool HasActiveHostileFactionRank(std::int32_t rank)
    {
        // R8 showed BanditFaction rank changes from 0 to -1 after RemoveFromFaction().
        // Treat rank -1 as a removed/tombstone state for hostile-source factions so
        // quarantine does not keep removing the same faction forever.
        return rank >= 0;
    }

    bool HasQuarantineBeenAttempted(RE::FormID actorId)
    {
        std::scoped_lock lk(g_logLock);
        return g_quarantineAttemptedActors.find(actorId) != g_quarantineAttemptedActors.end();
    }

    void MarkQuarantineAttempted(RE::FormID actorId)
    {
        std::scoped_lock lk(g_logLock);
        g_quarantineAttemptedActors.insert(actorId);
    }

    struct FactionSummary
    {
        unsigned hostileMatches{ 0 };
        unsigned stateMatches{ 0 };
        bool tfdTeammate{ false };
        bool truceTeammate{ false };
        bool currentFollower{ false };
        bool playerFollower{ false };
        bool potentialFollower{ false };
    };

    FactionSummary BuildFactionSummary(RE::Actor* actor)
    {
        ResolveFactions();

        FactionSummary summary{};
        if (!actor) {
            return summary;
        }

        for (const auto& entry : g_factions) {
            if (!entry.faction) {
                continue;
            }

            const auto rank = GetExactFactionRank(actor, entry.faction);
            if (rank <= -2) {
                continue;
            }

            const bool activeHostile = entry.hostileSource && HasActiveHostileFactionRank(rank);
            const bool playerSideState = entry.playerSideState;

            if (activeHostile) {
                ++summary.hostileMatches;
            }
            if (playerSideState) {
                ++summary.stateMatches;
            }

            if (!playerSideState) {
                continue;
            }

            const std::string_view id = entry.editorID ? entry.editorID : "";
            if (id == "TFDTeammateFaction") {
                summary.tfdTeammate = true;
            }
            else if (id == "TFDTruceTeammateFaction") {
                summary.truceTeammate = true;
            }
            else if (id == "CurrentFollowerFaction") {
                summary.currentFollower = true;
            }
            else if (id == "PlayerFollowerFaction") {
                summary.playerFollower = true;
            }
            else if (id == "PotentialFollowerFaction") {
                summary.potentialFollower = true;
            }
        }

        return summary;
    }

    bool IsRecruitLikeFromSummary(RE::Actor* actor, const FactionSummary& summary)
    {
        return actor &&
            (actor->IsPlayerTeammate() ||
                summary.tfdTeammate ||
                summary.truceTeammate ||
                summary.currentFollower ||
                summary.playerFollower);
    }

    bool ShouldLogObserve(RE::Actor* actor, bool force, bool throttle, bool rawHostile, const FactionSummary& summary)
    {
        if (!actor) {
            return false;
        }
        if (force || !throttle) {
            return true;
        }

        const bool interesting = rawHostile || summary.hostileMatches > 0 || actor->IsPlayerTeammate() || summary.tfdTeammate || summary.truceTeammate;
        if (!interesting) {
            return false;
        }

        const auto actorId = actor->GetFormID();
        const double now = NowSec();

        std::scoped_lock lk(g_logLock);
        auto it = g_nextObserveLogSec.find(actorId);
        if (it != g_nextObserveLogSec.end() && now < it->second) {
            return false;
        }

        g_nextObserveLogSec[actorId] = now + kRecruitObserveThrottleSec;
        return true;
    }

    bool ShouldLogCommit(RE::Actor* actor, bool force, const TFD::Recruit::CommitResult& result)
    {
        if (!actor) {
            return false;
        }
        if (force || result.removedHostileFactions > 0 || result.rawHostileBefore != result.rawHostileAfter) {
            return true;
        }
        if (!result.rawHostileBefore && result.hostileFactionMatchesBefore == 0) {
            return false;
        }

        const auto actorId = actor->GetFormID();
        const double now = NowSec();

        std::scoped_lock lk(g_logLock);
        auto it = g_nextCommitLogSec.find(actorId);
        if (it != g_nextCommitLogSec.end() && now < it->second) {
            return false;
        }

        g_nextCommitLogSec[actorId] = now + kRecruitCommitThrottleSec;
        return true;
    }

    void LogFactionDetails(RE::Actor* actor, bool hostileOnly)
    {
        ResolveFactions();
        if (!actor) {
            return;
        }

        for (const auto& entry : g_factions) {
            if (!entry.faction) {
                continue;
            }
            if (hostileOnly && !entry.hostileSource) {
                continue;
            }
            const auto rank = GetExactFactionRank(actor, entry.faction);
            if (rank <= -2) {
                continue;
            }
            if (hostileOnly && entry.hostileSource && !HasActiveHostileFactionRank(rank)) {
                continue;
            }

            const char* group = entry.hostileSource ?
                (HasActiveHostileFactionRank(rank) ? "hostile" : "hostile_removed") :
                "state";

            spdlog::info(
                "[TFD][Recruit] faction actor={:08X} group={} editorID={} form={:08X} rank={}",
                actor->GetFormID(),
                group,
                entry.editorID ? entry.editorID : "unknown",
                entry.faction->GetFormID(),
                rank);
        }
    }

    void EnsureTeammateFaction(RE::Actor* actor)
    {
        if (!actor) {
            return;
        }

        ResolveFactions();
        for (const auto& entry : g_factions) {
            if (!entry.faction) {
                continue;
            }
            const std::string_view id = entry.editorID ? entry.editorID : "";
            if (id != "TFDTeammateFaction") {
                continue;
            }
            if (!HasFactionAnyRank(actor, entry.faction)) {
                actor->AddToFaction(entry.faction, 0);
                spdlog::info(
                    "[TFD][Recruit] ensure teammate faction actor={:08X} faction={:08X}",
                    actor->GetFormID(),
                    entry.faction->GetFormID());
            }
            return;
        }
    }

    unsigned RemoveHostileSourceFactions(RE::Actor* actor)
    {
        ResolveFactions();
        if (!actor) {
            return 0;
        }

        unsigned removed = 0;
        for (const auto& entry : g_factions) {
            if (!entry.faction || !entry.hostileSource) {
                continue;
            }

            const auto rank = GetExactFactionRank(actor, entry.faction);
            if (!HasActiveHostileFactionRank(rank)) {
                continue;
            }

            actor->RemoveFromFaction(entry.faction);
            ++removed;

            spdlog::info(
                "[TFD][Recruit] quarantine remove actor={:08X} faction={:08X} editorID={} rankBefore={}",
                actor->GetFormID(),
                entry.faction->GetFormID(),
                entry.editorID ? entry.editorID : "unknown",
                rank);
        }

        return removed;
    }

    bool ClearRecruitCombatState(RE::Actor* actor, RE::PlayerCharacter* player)
    {
        if (!actor || !player) {
            return false;
        }

        bool changed = false;
        auto* actorTarget = ResolveCurrentCombatTarget(actor);
        auto* playerTarget = ResolveCurrentCombatTarget(player);
        const bool actorTargetWasPlayer = actorTarget == player;
        const bool playerTargetWasActor = playerTarget == actor;

        if (actorTargetWasPlayer || actorTarget) {
            actor->GetActorRuntimeData().currentCombatTarget = RE::ActorHandle{};
            changed = true;
        }
        if (playerTargetWasActor) {
            player->GetActorRuntimeData().currentCombatTarget = RE::ActorHandle{};
            changed = true;
        }

        if (auto* process = RE::ProcessLists::GetSingleton()) {
            process->ClearCachedFactionFightReactions();
            process->StopCombatAndAlarmOnActor(actor, false);
            if (playerTargetWasActor) {
                process->StopCombatAndAlarmOnActor(player, false);
            }
            changed = true;
        }

        if (actor->IsInCombat()) {
            actor->StopCombat();
            changed = true;
        }
        actor->StopAlarmOnActor();

        if (playerTargetWasActor || actorTargetWasPlayer) {
            player->StopCombat();
            changed = true;
        }

        actor->UpdateCombat();
        if (playerTargetWasActor) {
            player->UpdateCombat();
        }

        return changed;
    }

    void EvaluateRecruitPackage(RE::Actor* actor)
    {
        if (!actor) {
            return;
        }
        if (actor->IsWeaponDrawn()) {
            actor->DrawWeaponMagicHands(false);
        }
        actor->EvaluatePackage(false, true);
        actor->EvaluatePackage(true, true);
    }
}

namespace TFD::Recruit
{
    const char* ToString(SourceFlow sourceFlow)
    {
        switch (sourceFlow) {
        case SourceFlow::PreCombat:
            return "PreCombat";
        case SourceFlow::InCombat:
            return "InCombat";
        case SourceFlow::Bleedout:
            return "Bleedout";
        case SourceFlow::Captive:
            return "Captive";
        case SourceFlow::Victory:
            return "Victory";
        case SourceFlow::Pleasure:
            return "Pleasure";
        case SourceFlow::Defeated:
            return "Defeated";
        case SourceFlow::Teammate:
            return "Teammate";
        case SourceFlow::Dialogue:
            return "Dialogue";
        case SourceFlow::Unknown:
        default:
            return "Unknown";
        }
    }

    bool IsRecruitLike(RE::Actor* actor)
    {
        if (!actor || actor->IsDisabled() || actor->IsDead()) {
            return false;
        }

        const auto summary = BuildFactionSummary(actor);
        return IsRecruitLikeFromSummary(actor, summary);
    }

    bool HasKnownHostileSourceFaction(RE::Actor* actor)
    {
        return actor && BuildFactionSummary(actor).hostileMatches > 0;
    }

    bool IsRawHostileToPlayer(RE::Actor* actor, RE::PlayerCharacter* player)
    {
        if (!actor) {
            return false;
        }
        if (!player) {
            player = Player();
        }
        if (!player || actor == player) {
            return false;
        }
        return actor->IsHostileToActor(player);
    }

    void ObserveRecruitState(RE::Actor* actor, RE::PlayerCharacter* player, const ObserveOptions& options)
    {
        if (!actor || actor->IsDisabled() || actor->IsDead()) {
            return;
        }
        if (!player) {
            player = Player();
        }
        if (!player || actor == player) {
            return;
        }

        const auto summary = BuildFactionSummary(actor);
        const bool rawHostile = actor->IsHostileToActor(player);
        const bool recruitLike = IsRecruitLikeFromSummary(actor, summary);
        const bool staleRawHostility = recruitLike && rawHostile;

        if (!ShouldLogObserve(actor, options.detailed, options.throttle, rawHostile, summary)) {
            return;
        }

        spdlog::info(
            "[TFD][Recruit] observe actor={:08X} name='{}' source={} reason={} rawHostile={} recruitLike={} playerTeammate={} tfdTeammate={} truceTeammate={} currentFollower={} playerFollower={} potentialFollower={} hostileFactions={} stateFactions={}",
            actor->GetFormID(),
            actor->GetName() ? actor->GetName() : "",
            ToString(options.sourceFlow),
            options.reason ? options.reason : "unknown",
            rawHostile ? 1 : 0,
            recruitLike ? 1 : 0,
            actor->IsPlayerTeammate() ? 1 : 0,
            summary.tfdTeammate ? 1 : 0,
            summary.truceTeammate ? 1 : 0,
            summary.currentFollower ? 1 : 0,
            summary.playerFollower ? 1 : 0,
            summary.potentialFollower ? 1 : 0,
            summary.hostileMatches,
            summary.stateMatches);

        if (staleRawHostility) {
            spdlog::warn(
                "[TFD][Recruit] stale raw hostility actor={:08X} hostileFactions={} stateFactions={} source={} reason={}",
                actor->GetFormID(),
                summary.hostileMatches,
                summary.stateMatches,
                ToString(options.sourceFlow),
                options.reason ? options.reason : "unknown");
        }

        LogFactionDetails(actor, !options.detailed);
    }

    void ObserveRecruitState(RE::Actor* actor, const ObserveOptions& options)
    {
        ObserveRecruitState(actor, Player(), options);
    }

    CommitResult CommitRecruit(RE::Actor* actor, RE::PlayerCharacter* player, const CommitOptions& options)
    {
        CommitResult result{};

        if (!actor || actor->IsDisabled() || actor->IsDead()) {
            result.skipped = true;
            return result;
        }
        if (!player) {
            player = Player();
        }
        if (!player || actor == player) {
            result.skipped = true;
            return result;
        }

        result.attempted = true;

        const auto beforeSummary = BuildFactionSummary(actor);
        result.recruitLikeBefore = IsRecruitLikeFromSummary(actor, beforeSummary);
        result.rawHostileBefore = actor->IsHostileToActor(player);
        result.hostileFactionMatchesBefore = beforeSummary.hostileMatches;

        // CommitRecruit must only convert actors that are already in a player-side
        // recruit state. This prevents accidental pacification of unrelated enemies
        // merely because they have a known hostile faction.
        if (!result.recruitLikeBefore) {
            result.skipped = true;
            if (ShouldLogCommit(actor, options.detailedLog, result)) {
                spdlog::info(
                    "[TFD][Recruit] commit skipped actor={:08X} source={} reason={} rawBefore={} hostileFactions={} reasonDetail=not_recruit_like",
                    actor->GetFormID(),
                    ToString(options.sourceFlow),
                    options.reason ? options.reason : "unknown",
                    result.rawHostileBefore ? 1 : 0,
                    result.hostileFactionMatchesBefore);
            }
            return result;
        }

        const auto actorId = actor->GetFormID();
        const bool quarantineAlreadyAttempted = HasQuarantineBeenAttempted(actorId);

        // If R8 already removed the active hostile faction, GetFactionRank may still
        // expose a rank -1 removed/inherited trace. Do not keep clearing combat and
        // evaluating packages on every teammate refresh when there is no active hostile
        // source left to remove.
        if (quarantineAlreadyAttempted && result.hostileFactionMatchesBefore == 0) {
            result.skipped = true;
            result.hostileFactionMatchesAfter = result.hostileFactionMatchesBefore;
            result.rawHostileAfter = result.rawHostileBefore;

            if (ShouldLogCommit(actor, options.detailedLog, result)) {
                spdlog::info(
                    "[TFD][Recruit] commit skipped actor={:08X} source={} reason={} rawBefore={} hostileFactions={} reasonDetail=already_quarantined_no_active_hostile",
                    actor->GetFormID(),
                    ToString(options.sourceFlow),
                    options.reason ? options.reason : "unknown",
                    result.rawHostileBefore ? 1 : 0,
                    result.hostileFactionMatchesBefore);
            }

            if (result.rawHostileAfter || options.detailedLog) {
                ObserveRecruitState(actor, player, {
                    options.sourceFlow,
                    options.reason ? options.reason : "already_quarantined_no_active_hostile",
                    options.detailedLog || result.rawHostileAfter,
                    options.throttleObserve
                    });
            }

            return result;
        }

        EnsureTeammateFaction(actor);

        if (options.quarantineHostileFactions && result.hostileFactionMatchesBefore > 0) {
            result.removedHostileFactions = RemoveHostileSourceFactions(actor);
        }

        if (options.quarantineHostileFactions) {
            MarkQuarantineAttempted(actorId);
        }

        if (options.clearCombat) {
            result.combatCleared = ClearRecruitCombatState(actor, player);
        }
        if (options.evaluatePackage) {
            EvaluateRecruitPackage(actor);
        }

        const auto afterSummary = BuildFactionSummary(actor);
        result.hostileFactionMatchesAfter = afterSummary.hostileMatches;
        result.rawHostileAfter = actor->IsHostileToActor(player);

        if (!result.rawHostileAfter && result.hostileFactionMatchesAfter == 0) {
            std::scoped_lock lk(g_logLock);
            g_committedCleanActors.insert(actor->GetFormID());
        }

        if (ShouldLogCommit(actor, options.detailedLog, result)) {
            spdlog::info(
                "[TFD][Recruit] commit actor={:08X} name='{}' source={} reason={} rawBefore={} rawAfter={} recruitLikeBefore={} hostileBefore={} hostileAfter={} removed={} combatCleared={} eval={} clean={}",
                actor->GetFormID(),
                actor->GetName() ? actor->GetName() : "",
                ToString(options.sourceFlow),
                options.reason ? options.reason : "unknown",
                result.rawHostileBefore ? 1 : 0,
                result.rawHostileAfter ? 1 : 0,
                result.recruitLikeBefore ? 1 : 0,
                result.hostileFactionMatchesBefore,
                result.hostileFactionMatchesAfter,
                result.removedHostileFactions,
                result.combatCleared ? 1 : 0,
                options.evaluatePackage ? 1 : 0,
                (!result.rawHostileAfter && result.hostileFactionMatchesAfter == 0) ? 1 : 0);
        }

        if (result.rawHostileAfter || result.hostileFactionMatchesAfter > 0 || options.detailedLog) {
            ObserveRecruitState(actor, player, {
                options.sourceFlow,
                options.reason ? options.reason : "commit_recruit",
                options.detailedLog || result.rawHostileAfter,
                options.throttleObserve
                });
        }

        return result;
    }

    CommitResult CommitRecruit(RE::Actor* actor, const CommitOptions& options)
    {
        return CommitRecruit(actor, Player(), options);
    }

    unsigned CommitRecruitGroup(const std::vector<RE::Actor*>& actors, RE::PlayerCharacter* player, const CommitOptions& options)
    {
        unsigned converted = 0;
        for (auto* actor : actors) {
            const auto result = CommitRecruit(actor, player, options);
            if (result.removedHostileFactions > 0 || result.rawHostileBefore != result.rawHostileAfter) {
                ++converted;
            }
        }
        return converted;
    }
}
