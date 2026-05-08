#include "TFDRecruit.h"

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
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

    struct RecruitPendingEntry
    {
        double untilSec{ 0.0 };
        TFD::Recruit::SourceFlow sourceFlow{ TFD::Recruit::SourceFlow::Unknown };
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
        "TFDExpiredTeammate",
        "TFDPacifyFaction",
        "TFDPlayerFaction",
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
    std::unordered_set<RE::FormID> g_runtimeProfileAppliedActors;
    std::unordered_set<RE::FormID> g_recruitSettleAttemptedActors;
    std::unordered_map<RE::FormID, RecruitPendingEntry> g_recruitCommitPending;

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

    bool HasRuntimeProfileBeenApplied(RE::FormID actorId)
    {
        std::scoped_lock lk(g_logLock);
        return g_runtimeProfileAppliedActors.find(actorId) != g_runtimeProfileAppliedActors.end();
    }

    void MarkRuntimeProfileApplied(RE::FormID actorId)
    {
        std::scoped_lock lk(g_logLock);
        g_runtimeProfileAppliedActors.insert(actorId);
    }

    bool HasRecruitSettleBeenAttempted(RE::FormID actorId)
    {
        std::scoped_lock lk(g_logLock);
        return g_recruitSettleAttemptedActors.find(actorId) != g_recruitSettleAttemptedActors.end();
    }

    void MarkRecruitSettleAttempted(RE::FormID actorId)
    {
        std::scoped_lock lk(g_logLock);
        g_recruitSettleAttemptedActors.insert(actorId);
    }

    bool IsPendingExpired(const RecruitPendingEntry& entry, double nowSec)
    {
        return entry.untilSec <= 0.0 || nowSec >= entry.untilSec;
    }

    bool NearlyEqual(float lhs, float rhs)
    {
        return std::fabs(lhs - rhs) <= 0.01f;
    }

    struct FactionSummary
    {
        unsigned hostileMatches{ 0 };
        unsigned stateMatches{ 0 };
        bool tfdTeammate{ false };
        bool expiredTeammate{ false };
        bool tfdPacify{ false };
        bool tfdPlayer{ false };
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
            else if (id == "TFDExpiredTeammate") {
                summary.expiredTeammate = true;
            }
            else if (id == "TFDPacifyFaction") {
                summary.tfdPacify = true;
            }
            else if (id == "TFDPlayerFaction") {
                summary.tfdPlayer = true;
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
                summary.expiredTeammate ||
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

        const bool interesting = rawHostile || summary.hostileMatches > 0 || actor->IsPlayerTeammate() || summary.tfdTeammate || summary.expiredTeammate;
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

    RE::TESFaction* FindFactionByEditorID(std::string_view editorID)
    {
        ResolveFactions();

        for (const auto& entry : g_factions) {
            if (!entry.faction || !entry.editorID) {
                continue;
            }
            if (editorID == entry.editorID) {
                return entry.faction;
            }
        }

        return nullptr;
    }

    bool EnsureFactionActive(RE::Actor* actor, std::string_view editorID, const char* ownerLabel)
    {
        if (!actor) {
            return false;
        }

        auto* faction = FindFactionByEditorID(editorID);
        if (!faction) {
            spdlog::warn(
                "[TFD][Recruit] ensure faction failed actor={:08X} editorID={} owner={} reason=unresolved",
                actor->GetFormID(),
                editorID,
                ownerLabel ? ownerLabel : "unknown");
            return false;
        }

        const auto rankBefore = GetExactFactionRank(actor, faction);
        if (rankBefore >= 0) {
            return false;
        }

        actor->AddToFaction(faction, 0);
        const auto rankAfter = GetExactFactionRank(actor, faction);

        spdlog::info(
            "[TFD][Recruit] ensure faction actor={:08X} editorID={} form={:08X} owner={} rankBefore={} rankAfter={}",
            actor->GetFormID(),
            editorID,
            faction->GetFormID(),
            ownerLabel ? ownerLabel : "unknown",
            rankBefore,
            rankAfter);

        return rankAfter >= 0;
    }

    struct AllianceEnsureResult
    {
        unsigned actorFactions{ 0 };
        bool playerFaction{ false };
    };

    AllianceEnsureResult EnsureRecruitAlliance(RE::Actor* actor, RE::PlayerCharacter* player)
    {
        AllianceEnsureResult result{};

        if (actor) {
            if (EnsureFactionActive(actor, "TFDTeammateFaction", "actor")) {
                ++result.actorFactions;
            }
            if (EnsureFactionActive(actor, "TFDPacifyFaction", "actor")) {
                ++result.actorFactions;
            }
            // R67: converted enemies must be true player-side allies, not only
            // dialogue-safe pacified actors. Sharing TFDPlayerFaction with the
            // player lets normal Assistance=FriendsAndAllies combat alarm treat
            // attacks on the player as calls for help, without force-starting combat.
            if (EnsureFactionActive(actor, "TFDPlayerFaction", "actor_player_side")) {
                ++result.actorFactions;
            }
            // R80: TFD-converted teammates must get the same follower anchor
            // factions as vanilla-style followers during native commit, not only
            // after the Papyrus alias repair arrives. This makes combat assist
            // state exist before the first threat scan/AI package tick.
            if (EnsureFactionActive(actor, "CurrentFollowerFaction", "actor_follower_anchor")) {
                ++result.actorFactions;
            }
            if (EnsureFactionActive(actor, "PlayerFollowerFaction", "actor_follower_anchor")) {
                ++result.actorFactions;
            }
        }

        if (player) {
            result.playerFaction = EnsureFactionActive(player, "TFDPlayerFaction", "player");
        }

        return result;
    }

    float GetActorValue(RE::Actor* actor, RE::ActorValue actorValue)
    {
        if (!actor) {
            return 0.0f;
        }

        auto* owner = actor->AsActorValueOwner();
        if (!owner) {
            return 0.0f;
        }

        return owner->GetActorValue(actorValue);
    }

    void SetActorValue(RE::Actor* actor, RE::ActorValue actorValue, float value)
    {
        if (!actor) {
            return;
        }

        auto* owner = actor->AsActorValueOwner();
        if (!owner) {
            return;
        }

        owner->SetActorValue(actorValue, value);
    }

    struct RuntimeProfileResult
    {
        bool changed{ false };
        float aggressionBefore{ 0.0f };
        float confidenceBefore{ 0.0f };
        float assistanceBefore{ 0.0f };
        float moralityBefore{ 0.0f };
        float aggressionAfter{ 0.0f };
        float confidenceAfter{ 0.0f };
        float assistanceAfter{ 0.0f };
        float moralityAfter{ 0.0f };
    };

    RuntimeProfileResult ApplyRecruitRuntimeProfile(RE::Actor* actor, bool detailedLog)
    {
        RuntimeProfileResult result{};
        if (!actor) {
            return result;
        }

        constexpr float kAggressionAggressive = 1.0f;
        constexpr float kConfidenceBrave = 3.0f;
        constexpr float kAssistanceFriendsAndAllies = 2.0f;
        constexpr float kMoralityAnyCrime = 0.0f;

        result.aggressionBefore = GetActorValue(actor, RE::ActorValue::kAggression);
        result.confidenceBefore = GetActorValue(actor, RE::ActorValue::kConfidence);
        result.assistanceBefore = GetActorValue(actor, RE::ActorValue::kAssistance);
        result.moralityBefore = GetActorValue(actor, RE::ActorValue::kMorality);

        const bool alreadyApplied = HasRuntimeProfileBeenApplied(actor->GetFormID());
        const bool needsAggression = !NearlyEqual(result.aggressionBefore, kAggressionAggressive);
        const bool needsConfidence = !NearlyEqual(result.confidenceBefore, kConfidenceBrave);
        const bool needsAssistance = !NearlyEqual(result.assistanceBefore, kAssistanceFriendsAndAllies);
        const bool needsMorality = !NearlyEqual(result.moralityBefore, kMoralityAnyCrime);

        result.changed = needsAggression || needsConfidence || needsAssistance || needsMorality;

        if (result.changed) {
            SetActorValue(actor, RE::ActorValue::kAggression, kAggressionAggressive);
            SetActorValue(actor, RE::ActorValue::kConfidence, kConfidenceBrave);
            SetActorValue(actor, RE::ActorValue::kAssistance, kAssistanceFriendsAndAllies);
            SetActorValue(actor, RE::ActorValue::kMorality, kMoralityAnyCrime);
        }

        result.aggressionAfter = GetActorValue(actor, RE::ActorValue::kAggression);
        result.confidenceAfter = GetActorValue(actor, RE::ActorValue::kConfidence);
        result.assistanceAfter = GetActorValue(actor, RE::ActorValue::kAssistance);
        result.moralityAfter = GetActorValue(actor, RE::ActorValue::kMorality);

        MarkRuntimeProfileApplied(actor->GetFormID());

        if (result.changed || detailedLog || !alreadyApplied) {
            spdlog::info(
                "[TFD][Recruit] runtime profile actor={:08X} changed={} aggression={:.1f}->{:.1f} confidence={:.1f}->{:.1f} assistance={:.1f}->{:.1f} morality={:.1f}->{:.1f}",
                actor->GetFormID(),
                result.changed ? 1 : 0,
                result.aggressionBefore,
                result.aggressionAfter,
                result.confidenceBefore,
                result.confidenceAfter,
                result.assistanceBefore,
                result.assistanceAfter,
                result.moralityBefore,
                result.moralityAfter);
        }

        return result;
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
        const bool actorRawHostileToPlayer = actor->IsHostileToActor(player);
        const bool clearPlayerSideAlarm = playerTargetWasActor || actorTargetWasPlayer || actorRawHostileToPlayer;

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
            if (clearPlayerSideAlarm) {
                process->StopCombatAndAlarmOnActor(player, false);
            }
            changed = true;
        }

        if (actor->IsInCombat()) {
            actor->StopCombat();
            changed = true;
        }
        actor->StopAlarmOnActor();
        actor->SetBeenAttacked(false);
        changed = true;

        if (clearPlayerSideAlarm) {
            player->StopCombat();
            player->StopAlarmOnActor();
            player->SetBeenAttacked(false);
            changed = true;
        }

        actor->UpdateCombat();
        if (clearPlayerSideAlarm || actor->IsHostileToActor(player)) {
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
            "[TFD][Recruit] observe actor={:08X} name='{}' source={} reason={} rawHostile={} recruitLike={} playerTeammate={} tfdTeammate={} expiredTeammate={} tfdPacify={} tfdPlayer={} currentFollower={} playerFollower={} potentialFollower={} hostileFactions={} stateFactions={}",
            actor->GetFormID(),
            actor->GetName() ? actor->GetName() : "",
            ToString(options.sourceFlow),
            options.reason ? options.reason : "unknown",
            rawHostile ? 1 : 0,
            recruitLike ? 1 : 0,
            actor->IsPlayerTeammate() ? 1 : 0,
            summary.tfdTeammate ? 1 : 0,
            summary.expiredTeammate ? 1 : 0,
            summary.tfdPacify ? 1 : 0,
            summary.tfdPlayer ? 1 : 0,
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

    void MarkRecruitCommitPending(RE::Actor* actor, double durationSec, SourceFlow sourceFlow, const char* reason)
    {
        if (!actor || actor->IsDead() || actor->IsDisabled()) {
            return;
        }

        const auto actorId = actor->GetFormID();
        if (actorId == 0) {
            return;
        }

        const double safeDuration = std::clamp(durationSec, 0.50, 10.00);
        const double now = NowSec();
        const double until = now + safeDuration;
        bool refreshed = false;
        double oldUntil = 0.0;
        double newUntil = until;

        {
            std::scoped_lock lk(g_logLock);
            auto& entry = g_recruitCommitPending[actorId];
            refreshed = entry.untilSec > now;
            oldUntil = entry.untilSec;
            entry.untilSec = std::max(entry.untilSec, until);
            entry.sourceFlow = sourceFlow;
            newUntil = entry.untilSec;
        }

        spdlog::info(
            "[TFD][Recruit] pending {} actor={:08X} source={} reason={} duration={:.2f}s oldUntil={:.2f} newUntil={:.2f}",
            refreshed ? "refresh" : "begin",
            actorId,
            ToString(sourceFlow),
            reason ? reason : "unknown",
            safeDuration,
            oldUntil,
            newUntil);
    }

    void MarkRecruitCommitPendingGroup(const std::vector<RE::Actor*>& actors, double durationSec, SourceFlow sourceFlow, const char* reason)
    {
        std::unordered_set<RE::FormID> seen;
        unsigned count = 0;

        for (auto* actor : actors) {
            if (!actor || actor->IsDead() || actor->IsDisabled()) {
                continue;
            }
            const auto actorId = actor->GetFormID();
            if (actorId == 0 || !seen.insert(actorId).second) {
                continue;
            }
            MarkRecruitCommitPending(actor, durationSec, sourceFlow, reason);
            ++count;
        }

        if (count > 0) {
            spdlog::info(
                "[TFD][Recruit] pending group source={} reason={} count={} duration={:.2f}s",
                ToString(sourceFlow),
                reason ? reason : "unknown",
                count,
                std::clamp(durationSec, 0.50, 10.00));
        }
    }

    std::vector<RE::Actor*> CollectRecruitCommitPendingActors(SourceFlow sourceFlow, bool includeUnknownSource)
    {
        std::vector<RE::FormID> actorIds;
        const double now = NowSec();

        {
            std::scoped_lock lk(g_logLock);

            for (auto it = g_recruitCommitPending.begin(); it != g_recruitCommitPending.end();) {
                if (IsPendingExpired(it->second, now)) {
                    spdlog::info(
                        "[TFD][Recruit] pending expired actor={:08X} source={} reason=collect",
                        it->first,
                        ToString(it->second.sourceFlow));
                    it = g_recruitCommitPending.erase(it);
                    continue;
                }

                const bool sourceMatches =
                    it->second.sourceFlow == sourceFlow ||
                    (includeUnknownSource && it->second.sourceFlow == SourceFlow::Unknown);
                if (sourceMatches) {
                    actorIds.push_back(it->first);
                }

                ++it;
            }
        }

        std::vector<RE::Actor*> actors;
        actors.reserve(actorIds.size());
        std::unordered_set<RE::FormID> seen;
        seen.reserve(actorIds.size());

        for (const auto actorId : actorIds) {
            if (actorId == 0 || !seen.insert(actorId).second) {
                continue;
            }

            auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorId);
            if (!actor || actor->IsDead() || actor->IsDisabled()) {
                continue;
            }

            actors.push_back(actor);
        }

        if (!actors.empty()) {
            spdlog::info(
                "[TFD][Recruit] pending collect source={} count={} includeUnknown={}",
                ToString(sourceFlow),
                static_cast<unsigned>(actors.size()),
                includeUnknownSource ? 1 : 0);
        }

        return actors;
    }

    bool IsRecruitCommitPending(RE::Actor* actor)
    {
        if (!actor || actor->IsDead() || actor->IsDisabled()) {
            return false;
        }

        const auto actorId = actor->GetFormID();
        if (actorId == 0) {
            return false;
        }

        const double now = NowSec();

        std::scoped_lock lk(g_logLock);
        auto it = g_recruitCommitPending.find(actorId);
        if (it == g_recruitCommitPending.end()) {
            return false;
        }

        if (IsPendingExpired(it->second, now)) {
            spdlog::info(
                "[TFD][Recruit] pending expired actor={:08X} source={}",
                actorId,
                ToString(it->second.sourceFlow));
            g_recruitCommitPending.erase(it);
            return false;
        }

        return true;
    }

    void ClearRecruitCommitPending(RE::Actor* actor, const char* reason)
    {
        if (!actor) {
            return;
        }

        const auto actorId = actor->GetFormID();
        if (actorId == 0) {
            return;
        }

        bool removed = false;
        {
            std::scoped_lock lk(g_logLock);
            removed = g_recruitCommitPending.erase(actorId) > 0;
        }

        if (removed) {
            spdlog::info(
                "[TFD][Recruit] pending clear actor={:08X} reason={}",
                actorId,
                reason ? reason : "unknown");
        }
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
        result.pendingCommitBefore = IsRecruitCommitPending(actor);
        result.rawHostileBefore = actor->IsHostileToActor(player);
        result.hostileFactionMatchesBefore = beforeSummary.hostileMatches;

        // CommitRecruit must only convert actors that are already in a player-side
        // recruit state or were captured by the native pending recruit participant
        // guard. This prevents accidental pacification of unrelated enemies merely
        // because they have a known hostile faction.
        if (!result.recruitLikeBefore && !result.pendingCommitBefore) {
            result.skipped = true;
            if (ShouldLogCommit(actor, options.detailedLog, result)) {
                spdlog::info(
                    "[TFD][Recruit] commit skipped actor={:08X} source={} reason={} rawBefore={} hostileFactions={} pending={} reasonDetail=not_recruit_like",
                    actor->GetFormID(),
                    ToString(options.sourceFlow),
                    options.reason ? options.reason : "unknown",
                    result.rawHostileBefore ? 1 : 0,
                    result.hostileFactionMatchesBefore,
                    result.pendingCommitBefore ? 1 : 0);
            }
            return result;
        }

        const auto actorId = actor->GetFormID();
        const bool quarantineAlreadyAttempted = HasQuarantineBeenAttempted(actorId);
        const bool settleAlreadyAttempted = HasRecruitSettleBeenAttempted(actorId);

        if (options.ensurePacifyAlliance) {
            const auto alliance = EnsureRecruitAlliance(actor, player);
            result.ensuredStateFactions = alliance.actorFactions;
            result.playerFactionEnsured = alliance.playerFaction;
        }

        if (options.applyRuntimeProfile) {
            const auto profile = ApplyRecruitRuntimeProfile(actor, options.detailedLog);
            result.runtimeProfileApplied = profile.changed;
        }

        if (options.quarantineHostileFactions && result.hostileFactionMatchesBefore > 0) {
            result.removedHostileFactions = RemoveHostileSourceFactions(actor);
        }

        if (options.quarantineHostileFactions) {
            MarkQuarantineAttempted(actorId);
        }

        const bool staleRawHostilityNeedsSettle =
            result.rawHostileBefore &&
            result.hostileFactionMatchesBefore == 0 &&
            beforeSummary.stateMatches > 0 &&
            (beforeSummary.tfdTeammate || beforeSummary.expiredTeammate || beforeSummary.tfdPacify);

        const bool nothingNewToSettle =
            quarantineAlreadyAttempted &&
            settleAlreadyAttempted &&
            result.hostileFactionMatchesBefore == 0 &&
            result.removedHostileFactions == 0 &&
            result.ensuredStateFactions == 0 &&
            !result.playerFactionEnsured &&
            !result.runtimeProfileApplied &&
            !staleRawHostilityNeedsSettle;

        if (staleRawHostilityNeedsSettle) {
            spdlog::info(
                "[TFD][Recruit] stale raw settle actor={:08X} source={} reason={} stateFactions={} detail=no_active_hostile_faction",
                actor->GetFormID(),
                ToString(options.sourceFlow),
                options.reason ? options.reason : "unknown",
                beforeSummary.stateMatches);
        }

        // R8B stops repeated quarantine. R9 still allows one post-profile settle pass,
        // because pacify alliance and runtime actor values can affect raw hostility only
        // after combat/cache/package refresh.
        if (nothingNewToSettle) {
            result.skipped = true;
            result.hostileFactionMatchesAfter = result.hostileFactionMatchesBefore;
            result.rawHostileAfter = result.rawHostileBefore;

            if (ShouldLogCommit(actor, options.detailedLog, result)) {
                spdlog::info(
                    "[TFD][Recruit] commit skipped actor={:08X} source={} reason={} rawBefore={} hostileFactions={} reasonDetail=already_settled_no_active_hostile",
                    actor->GetFormID(),
                    ToString(options.sourceFlow),
                    options.reason ? options.reason : "unknown",
                    result.rawHostileBefore ? 1 : 0,
                    result.hostileFactionMatchesBefore);
            }

            if (result.rawHostileAfter || options.detailedLog) {
                ObserveRecruitState(actor, player, {
                    options.sourceFlow,
                    options.reason ? options.reason : "already_settled_no_active_hostile",
                    options.detailedLog || result.rawHostileAfter,
                    options.throttleObserve
                    });
            }

            return result;
        }

        if (options.clearCombat) {
            result.combatCleared = ClearRecruitCombatState(actor, player);
        }
        if (options.evaluatePackage) {
            EvaluateRecruitPackage(actor);
        }

        MarkRecruitSettleAttempted(actorId);

        const auto afterSummary = BuildFactionSummary(actor);
        result.hostileFactionMatchesAfter = afterSummary.hostileMatches;
        result.rawHostileAfter = actor->IsHostileToActor(player);

        if (!result.rawHostileAfter && result.hostileFactionMatchesAfter == 0) {
            {
                std::scoped_lock lk(g_logLock);
                g_committedCleanActors.insert(actor->GetFormID());
            }
            ClearRecruitCommitPending(actor, options.reason ? options.reason : "commit_clean");
        }

        if (ShouldLogCommit(actor, options.detailedLog, result)) {
            spdlog::info(
                "[TFD][Recruit] commit actor={:08X} name='{}' source={} reason={} rawBefore={} rawAfter={} recruitLikeBefore={} pendingCommit={} hostileBefore={} hostileAfter={} removed={} ensuredState={} playerFaction={} runtimeProfile={} combatCleared={} eval={} clean={}",
                actor->GetFormID(),
                actor->GetName() ? actor->GetName() : "",
                ToString(options.sourceFlow),
                options.reason ? options.reason : "unknown",
                result.rawHostileBefore ? 1 : 0,
                result.rawHostileAfter ? 1 : 0,
                result.recruitLikeBefore ? 1 : 0,
                result.pendingCommitBefore ? 1 : 0,
                result.hostileFactionMatchesBefore,
                result.hostileFactionMatchesAfter,
                result.removedHostileFactions,
                result.ensuredStateFactions,
                result.playerFactionEnsured ? 1 : 0,
                result.runtimeProfileApplied ? 1 : 0,
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
