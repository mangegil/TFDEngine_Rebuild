#include "TFDPayModel.h"

#include "TFDActor.h"
#include "TFDHostilityController.h"
#include "TFDLocation.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

namespace TFD::PayModel
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr float kScanRadius = 2200.0f;
        constexpr int kEncounterGoldCap = 3000;
        constexpr const char* kSharedGoldGlobalEditorID = "TFDPayGold";

        struct CacheState
        {
            bool valid{ false };
            PayContext context{ PayContext::None };
            std::uint32_t speakerFormID{ 0 };
            EncounterQuote quote{};
            double builtAtSec{ 0.0 };
        };

        std::mutex gLock;
        CacheState gCachedQuote{};
        Clock::time_point gT0 = Clock::now();
        RE::TESGlobal* gSharedGoldGlobal = nullptr;
        bool gLoggedSharedGoldMissing = false;

        double NowSec()
        {
            return std::chrono::duration<double>(Clock::now() - gT0).count();
        }

        RE::TESGlobal* ResolveSharedGoldGlobal()
        {
            if (gSharedGoldGlobal) {
                return gSharedGoldGlobal;
            }

            gSharedGoldGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>(kSharedGoldGlobalEditorID);
            if (!gSharedGoldGlobal && !gLoggedSharedGoldMissing) {
                gLoggedSharedGoldMissing = true;
                spdlog::warn("[TFD][PayModel] shared gold global '{}' not found", kSharedGoldGlobalEditorID);
            }
            return gSharedGoldGlobal;
        }

        const char* PayContextName(PayContext context)
        {
            switch (context) {
            case PayContext::PreCombat:
                return "PreCombat";
            case PayContext::InCombat:
                return "InCombat";
            case PayContext::Bleedout:
                return "Bleedout";
            case PayContext::Rescue:
                return "Rescue";
            case PayContext::TeammateContract:
                return "TeammateContract";
            case PayContext::None:
            default:
                return "None";
            }
        }

        void SendPayQuoteEventSync(const char* eventName, const char* contextName, float goldValue, RE::Actor* speaker)
        {
            if (!eventName || !eventName[0]) {
                return;
            }

            auto* src = SKSE::GetModCallbackEventSource();
            if (!src) {
                spdlog::warn("[TFD][PayModel] send pay quote event skipped event={} hasSource=0",
                    eventName);
                return;
            }

            const char* context = contextName ? contextName : "";
            SKSE::ModCallbackEvent ev{ eventName, context, goldValue, speaker };
            src->SendEvent(&ev);
        }

        std::string GetActorNameSafe(RE::Actor* actor)
        {
            if (!actor) {
                return "";
            }

            if (auto* dn = actor->GetDisplayFullName(); dn && dn[0] != '\0') {
                return std::string(dn);
            }
            if (auto* n = actor->GetName(); n && n[0] != '\0') {
                return std::string(n);
            }
            return "";
        }

        std::string ToLowerAscii(std::string_view in)
        {
            std::string out;
            out.reserve(in.size());
            for (char c : in) {
                out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            return out;
        }

        bool ContainsNoCase(std::string_view haystack, std::string_view needle)
        {
            if (haystack.empty() || needle.empty()) {
                return false;
            }

            const auto h = ToLowerAscii(haystack);
            const auto n = ToLowerAscii(needle);
            return h.find(n) != std::string::npos;
        }

        std::int32_t GetActorLevelSafe(RE::Actor* actor)
        {
            if (!actor) {
                return 1;
            }

            const auto level = actor->GetLevel();
            return std::max<std::int32_t>(1, static_cast<std::int32_t>(level));
        }

        RE::TESFaction* LookupFaction(const char* editorID)
        {
            if (!editorID || !editorID[0]) {
                return nullptr;
            }
            return RE::TESForm::LookupByEditorID<RE::TESFaction>(editorID);
        }

        RE::BGSKeyword* LookupKeyword(const char* editorID)
        {
            if (!editorID || !editorID[0]) {
                return nullptr;
            }
            return RE::TESForm::LookupByEditorID<RE::BGSKeyword>(editorID);
        }

        bool ActorIsInFactionByEditorID(RE::Actor* actor, const char* editorID)
        {
            if (!actor) {
                return false;
            }

            auto* faction = LookupFaction(editorID);
            return faction && actor->IsInFaction(faction);
        }

        bool ActorHasKeywordByEditorID(RE::Actor* actor, const char* editorID)
        {
            if (!actor) {
                return false;
            }

            auto* kw = LookupKeyword(editorID);
            return kw && actor->HasKeyword(kw);
        }

        bool ActorHasAnyFaction(RE::Actor* actor, std::initializer_list<const char*> editorIDs)
        {
            for (auto* id : editorIDs) {
                if (ActorIsInFactionByEditorID(actor, id)) {
                    return true;
                }
            }
            return false;
        }

        bool ActorHasAnyKeyword(RE::Actor* actor, std::initializer_list<const char*> editorIDs)
        {
            for (auto* id : editorIDs) {
                if (ActorHasKeywordByEditorID(actor, id)) {
                    return true;
                }
            }
            return false;
        }

        bool IsUnnaturalActor(RE::Actor* actor)
        {
            if (!actor) {
                return false;
            }

            return ActorHasAnyKeyword(actor, {
                       "ActorTypeUndead",
                       "ActorTypeDaedra",
                       "ActorTypeGhost"
                }) ||
                ActorHasAnyFaction(actor, {
                    "VampireFaction"
                    });
        }

        int ScoreLevelDelta(std::int32_t enemyLevel, std::int32_t playerLevel)
        {
            const auto delta = enemyLevel - playerLevel;
            if (delta <= -1) {
                return 0;
            }
            if (delta == 0) {
                return 10;
            }
            if (delta <= 4) {
                return 25;
            }
            if (delta <= 9) {
                return 50;
            }
            return 80;
        }

        int ScoreAbsoluteLevel(std::int32_t enemyLevel)
        {
            if (enemyLevel < 10) {
                return 0;
            }
            if (enemyLevel < 20) {
                return 5;
            }
            if (enemyLevel < 30) {
                return 10;
            }
            if (enemyLevel < 40) {
                return 20;
            }
            return 30;
        }

        int ScoreThreatTier(ThreatTier tier)
        {
            switch (tier) {
            case ThreatTier::Weak:
                return 0;
            case ThreatTier::Normal:
                return 15;
            case ThreatTier::Strong:
                return 35;
            case ThreatTier::Elite:
                return 60;
            case ThreatTier::BossTier:
                return 90;
            default:
                return 15;
            }
        }

        int ScoreDurabilityTier(DurabilityTier tier)
        {
            switch (tier) {
            case DurabilityTier::Low:
                return 0;
            case DurabilityTier::Medium:
                return 10;
            case DurabilityTier::High:
                return 25;
            case DurabilityTier::VeryHigh:
                return 45;
            default:
                return 10;
            }
        }

        int ScoreRaceClass(RaceClass rc)
        {
            switch (rc) {
            case RaceClass::Human:
                return 0;
            case RaceClass::Mer:
                return 5;
            case RaceClass::Beastfolk:
                return 5;
            case RaceClass::Orc:
                return 5;
            case RaceClass::Unnatural:
                return 15;
            default:
                return 0;
            }
        }

        int ScoreEnemyType(EnemyType type)
        {
            switch (type) {
            case EnemyType::Bandit:
                return 0;
            case EnemyType::Forsworn:
                return 10;
            case EnemyType::Necromancer:
            case EnemyType::Mage:
                return 20;
            case EnemyType::Mercenary:
            case EnemyType::Soldier:
                return 25;
            case EnemyType::Vampire:
                return 30;
            case EnemyType::Guard:
                return 50;
            case EnemyType::Thalmor:
                return 70;
            case EnemyType::CreatureHumanoid:
                return 20;
            case EnemyType::Unknown:
            default:
                return 10;
            }
        }

        int ScoreBossRef(bool isBossRef)
        {
            return isBossRef ? 50 : 0;
        }

        int ResolveEncounterBaseGold(const std::vector<ActorBreakdown>& actors)
        {
            int total = 0;
            for (const auto& entry : actors) {
                total += entry.goldValue;
            }
            return std::min(total, kEncounterGoldCap);
        }

        std::vector<RE::Actor*> ResolveEncounterActors(RE::Actor* speaker, PayContext context)
        {
            std::vector<RE::Actor*> out;
            std::unordered_set<std::uint32_t> seen;

            auto addUnique = [&](RE::Actor* actor) {
                if (!actor) {
                    return;
                }

                const auto id = actor->GetFormID();
                if (id == 0) {
                    return;
                }

                auto [_, inserted] = seen.insert(id);
                if (!inserted) {
                    return;
                }

                out.push_back(actor);
            };

            if (!speaker) {
                return out;
            }

            addUnique(speaker);

            if (context == PayContext::PreCombat || context == PayContext::InCombat) {
                bool hasTruceContext = false;

                auto activeTruceActors = TFD::HostilityController::CollectActiveTruceActors(speaker);
                if (!activeTruceActors.empty()) {
                    hasTruceContext = true;
                    for (auto* actor : activeTruceActors) {
                        addUnique(actor);
                    }
                }

                auto aliasTruceActors = TFD::Actor::Ops::CollectTruceActorsForSpeaker(speaker);
                if (!aliasTruceActors.empty()) {
                    hasTruceContext = true;
                    for (auto* actor : aliasTruceActors) {
                        addUnique(actor);
                    }
                }

                if (hasTruceContext) {
                    spdlog::info(
                        "[TFD][PayModel] truce scoped actors speaker={:08X} context={} actors={}",
                        speaker->GetFormID(),
                        static_cast<int>(context),
                        static_cast<unsigned>(out.size()));
                    return out;
                }
            }

            auto snapshot = TFD::Actor::BuildSnapshot(kScanRadius, true);
            const auto* info = TFD::Actor::FindActorInfo(snapshot, speaker);
            if (!info || info->coalitionID < 0) {
                return out;
            }

            auto crowd = TFD::Actor::ResolveCrowdCandidates(snapshot, info->coalitionID);
            for (auto* actor : crowd) {
                addUnique(actor);
            }

            return out;
        }
    }

    void Install()
    {
        std::scoped_lock lk(gLock);
        gCachedQuote = {};
        gSharedGoldGlobal = nullptr;
        gLoggedSharedGoldMissing = false;
        spdlog::info("[TFD][PayModel] Install");
    }

    void Shutdown()
    {
        std::scoped_lock lk(gLock);
        gCachedQuote = {};
        gSharedGoldGlobal = nullptr;
        gLoggedSharedGoldMissing = false;
        spdlog::info("[TFD][PayModel] Shutdown");
    }

    void ClearCachedEncounterQuote(const char* reason)
    {
        std::scoped_lock lk(gLock);
        if (gCachedQuote.valid) {
            spdlog::info(
                "[TFD][PayModel] clear cached quote speaker={:08X} context={} total={} reason={}",
                gCachedQuote.speakerFormID,
                static_cast<int>(gCachedQuote.context),
                gCachedQuote.quote.totalGold,
                reason ? reason : "unknown");
        }
        gCachedQuote = {};
    }

    bool PublishSharedGold(RE::Actor* speaker, PayContext context, const char* reason)
    {
        if (!speaker) {
            spdlog::warn("[TFD][PayModel] publish shared gold rejected speaker=<null> context={} reason={}",
                static_cast<int>(context),
                reason ? reason : "unknown");
            return false;
        }

        auto* payGlobal = ResolveSharedGoldGlobal();
        if (!payGlobal) {
            spdlog::warn("[TFD][PayModel] publish shared gold rejected speaker={:08X} context={} reason={} missing_global=1",
                speaker->GetFormID(),
                static_cast<int>(context),
                reason ? reason : "unknown");
            return false;
        }

        int gold = 0;
        bool hasMatchingCache = false;
        {
            std::scoped_lock lk(gLock);
            hasMatchingCache =
                gCachedQuote.valid &&
                gCachedQuote.speakerFormID == speaker->GetFormID() &&
                gCachedQuote.context == context;
            if (hasMatchingCache) {
                gold = gCachedQuote.quote.totalGold;
            }
        }

        if (!hasMatchingCache) {
            if (!PrimeEncounterQuote(speaker, context)) {
                spdlog::warn("[TFD][PayModel] publish shared gold prime failed speaker={:08X} context={} reason={}",
                    speaker->GetFormID(),
                    static_cast<int>(context),
                    reason ? reason : "unknown");
                return false;
            }

            std::scoped_lock lk(gLock);
            if (!gCachedQuote.valid ||
                gCachedQuote.speakerFormID != speaker->GetFormID() ||
                gCachedQuote.context != context) {
                spdlog::warn("[TFD][PayModel] publish shared gold cache mismatch speaker={:08X} context={} reason={}",
                    speaker->GetFormID(),
                    static_cast<int>(context),
                    reason ? reason : "unknown");
                return false;
            }
            gold = gCachedQuote.quote.totalGold;
        }

        payGlobal->value = static_cast<float>(gold);
        SendPayQuoteEventSync("TFDPayQuoteUpdate", PayContextName(context), static_cast<float>(gold), speaker);
        spdlog::info("[TFD][PayModel] publish shared gold speaker={:08X} context={} gold={} reason={} event=TFDPayQuoteUpdate",
            speaker->GetFormID(),
            static_cast<int>(context),
            gold,
            reason ? reason : "unknown");
        return true;
    }

    void ClearSharedGold(const char* reason)
    {
        auto* payGlobal = ResolveSharedGoldGlobal();
        if (!payGlobal) {
            return;
        }

        if (payGlobal->value != 0.0f) {
            spdlog::info("[TFD][PayModel] clear shared gold oldValue={:.0f} reason={}",
                payGlobal->value,
                reason ? reason : "unknown");
        }
        payGlobal->value = 0.0f;
        SendPayQuoteEventSync("TFDPayQuoteClear", "", 0.0f, nullptr);
    }

    int GetContextPercent(PayContext context)
    {
        switch (context) {
        case PayContext::PreCombat:
            return 0;
        case PayContext::InCombat:
            return 25;
        case PayContext::Bleedout:
            return 25;
        case PayContext::Rescue:
            return -40;
        case PayContext::TeammateContract:
            return 0;
        case PayContext::None:
        default:
            return 0;
        }
    }

    int ApplyContextAdjustment(int baseGold, PayContext context)
    {
        const int pct = GetContextPercent(context);
        const int delta = static_cast<int>(std::lround(static_cast<double>(baseGold) * static_cast<double>(pct) / 100.0));
        const int adjusted = baseGold + delta;
        return std::max(0, adjusted);
    }

    EnemyType ClassifyEnemyType(RE::Actor* actor)
    {
        if (!actor) {
            return EnemyType::Unknown;
        }

        if (ActorHasAnyFaction(actor, {
                "ThalmorFaction",
                "ThalmorSplinterFaction"
            })) {
            return EnemyType::Thalmor;
        }

        if (ActorHasAnyFaction(actor, {
                "GuardFaction",
                "CWImperialFaction",
                "CWSonsFaction"
            })) {
            return EnemyType::Guard;
        }

        if (ActorHasAnyFaction(actor, {
                "BanditFaction",
                "dunMistwatchBanditFaction",
                "dunWhiteRiverFaction",
                "dunValtheimFaction",
                "dunBannermistFaction",
                "dunHaltedStreamFaction"
            })) {
            return EnemyType::Bandit;
        }

        if (ActorHasAnyFaction(actor, {
                "ForswornFaction"
            })) {
            return EnemyType::Forsworn;
        }

        if (ActorHasAnyFaction(actor, {
                "NecromancerFaction"
            })) {
            return EnemyType::Necromancer;
        }

        if (ActorHasAnyFaction(actor, {
                "VampireFaction"
            })) {
            return EnemyType::Vampire;
        }

        if (ActorHasAnyKeyword(actor, {
                "ActorTypeUndead",
                "ActorTypeDaedra"
            })) {
            return EnemyType::CreatureHumanoid;
        }

        if (ActorHasKeywordByEditorID(actor, "ActorTypeNPC")) {
            return EnemyType::Soldier;
        }

        return EnemyType::Unknown;
    }

    ThreatTier ClassifyThreatTier(RE::Actor* actor, RE::Actor* player)
    {
        if (!actor) {
            return ThreatTier::Weak;
        }

        auto* resolvedPlayer = player ? player : RE::PlayerCharacter::GetSingleton();

        const auto actorLevel = GetActorLevelSafe(actor);
        const auto playerLevel = GetActorLevelSafe(resolvedPlayer);
        const auto delta = actorLevel - playerLevel;
        const bool isBoss = HasBossRefType(actor);

        if (isBoss || delta >= 10) {
            return ThreatTier::BossTier;
        }
        if (delta >= 5) {
            return ThreatTier::Elite;
        }
        if (delta >= 2) {
            return ThreatTier::Strong;
        }
        if (delta >= 0) {
            return ThreatTier::Normal;
        }
        return ThreatTier::Weak;
    }

    DurabilityTier ClassifyDurabilityTier(RE::Actor* actor)
    {
        if (!actor) {
            return DurabilityTier::Low;
        }

        const float hp = actor->GetActorValue(RE::ActorValue::kHealth);
        const float dr = actor->GetActorValue(RE::ActorValue::kDamageResist);

        const float effective = hp + (dr * 1.25f);

        if (effective >= 450.0f) {
            return DurabilityTier::VeryHigh;
        }
        if (effective >= 250.0f) {
            return DurabilityTier::High;
        }
        if (effective >= 120.0f) {
            return DurabilityTier::Medium;
        }
        return DurabilityTier::Low;
    }

    RaceClass ClassifyRaceClass(RE::Actor* actor)
    {
        if (!actor) {
            return RaceClass::Human;
        }

        if (IsUnnaturalActor(actor)) {
            return RaceClass::Unnatural;
        }

        auto* race = actor->GetRace();
        if (!race) {
            return RaceClass::Human;
        }

        const std::string raceName = race->GetName() ? race->GetName() : "";

        if (ContainsNoCase(raceName, "orc")) {
            return RaceClass::Orc;
        }
        if (ContainsNoCase(raceName, "argonian") || ContainsNoCase(raceName, "khajiit")) {
            return RaceClass::Beastfolk;
        }
        if (ContainsNoCase(raceName, "elf")) {
            return RaceClass::Mer;
        }

        return RaceClass::Human;
    }

    bool HasBossRefType(RE::Actor* actor)
    {
        if (!actor) {
            return false;
        }

        auto* loc = TFD::Location::GetLocationFromRef(actor);
        if (!loc) {
            return false;
        }

        auto* bossType = RE::TESForm::LookupByEditorID<RE::BGSLocationRefType>("Boss");
        if (!bossType) {
            return false;
        }

        for (std::uint32_t i = 0; i < loc->specialRefs.size(); ++i) {
            const auto& sref = loc->specialRefs[i];

            auto* type = sref.type;
            if (!type) {
                continue;
            }

            if (type != bossType && type->GetFormID() != bossType->GetFormID()) {
                continue;
            }

            const auto refID = sref.refData.refID;
            if (refID == 0) {
                continue;
            }

            auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(refID);
            if (!ref) {
                continue;
            }

            if (ref->GetFormID() == actor->GetFormID()) {
                return true;
            }
        }

        return false;
    }

    int ScoreToGold(int totalScore)
    {
        if (totalScore <= 20) {
            return 50;
        }
        if (totalScore <= 40) {
            return 100;
        }
        if (totalScore <= 60) {
            return 150;
        }
        if (totalScore <= 80) {
            return 250;
        }
        if (totalScore <= 110) {
            return 400;
        }
        if (totalScore <= 140) {
            return 600;
        }
        if (totalScore <= 180) {
            return 900;
        }
        return 1300;
    }

    ActorBreakdown BuildActorBreakdown(RE::Actor* actor, RE::Actor* player)
    {
        ActorBreakdown out{};
        if (!actor) {
            return out;
        }

        auto* resolvedPlayer = player ? player : RE::PlayerCharacter::GetSingleton();

        out.actorFormID = actor->GetFormID();
        out.actorName = GetActorNameSafe(actor);

        out.enemyType = ClassifyEnemyType(actor);
        out.threatTier = ClassifyThreatTier(actor, resolvedPlayer);
        out.durabilityTier = ClassifyDurabilityTier(actor);
        out.raceClass = ClassifyRaceClass(actor);
        out.isBossRef = HasBossRefType(actor);

        out.level = GetActorLevelSafe(actor);
        out.playerLevel = GetActorLevelSafe(resolvedPlayer);
        out.levelDelta = out.level - out.playerLevel;

        out.levelDeltaScore = ScoreLevelDelta(out.level, out.playerLevel);
        out.absoluteLevelScore = ScoreAbsoluteLevel(out.level);
        out.threatScore = ScoreThreatTier(out.threatTier);
        out.durabilityScore = ScoreDurabilityTier(out.durabilityTier);
        out.raceScore = ScoreRaceClass(out.raceClass);
        out.typeScore = ScoreEnemyType(out.enemyType);
        out.bossScore = ScoreBossRef(out.isBossRef);

        out.totalScore =
            out.levelDeltaScore +
            out.absoluteLevelScore +
            out.threatScore +
            out.durabilityScore +
            out.raceScore +
            out.typeScore +
            out.bossScore;

        out.goldValue = ScoreToGold(out.totalScore);
        return out;
    }

    EncounterQuote BuildEncounterQuote(RE::Actor* speaker, PayContext context)
    {
        EncounterQuote quote{};
        if (!speaker) {
            return quote;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto actors = ResolveEncounterActors(speaker, context);

        quote.context = context;
        quote.speakerFormID = speaker->GetFormID();
        quote.speakerName = GetActorNameSafe(speaker);

        for (auto* actor : actors) {
            if (!actor) {
                continue;
            }
            quote.actors.push_back(BuildActorBreakdown(actor, player));
        }

        quote.actorCount = static_cast<int>(quote.actors.size());
        quote.baseGold = ResolveEncounterBaseGold(quote.actors);
        quote.contextPercent = GetContextPercent(context);
        quote.totalGold = ApplyContextAdjustment(quote.baseGold, context);

        spdlog::info(
            "[TFD][PayModel] built quote speaker={:08X} context={} actors={} base={} pct={} total={}",
            quote.speakerFormID,
            static_cast<int>(quote.context),
            quote.actorCount,
            quote.baseGold,
            quote.contextPercent,
            quote.totalGold);

        for (const auto& entry : quote.actors) {
            spdlog::info(
                "[TFD][PayModel] actor={:08X} name='{}' type={} threat={} dura={} race={} boss={} score={} gold={}",
                entry.actorFormID,
                entry.actorName,
                static_cast<int>(entry.enemyType),
                static_cast<int>(entry.threatTier),
                static_cast<int>(entry.durabilityTier),
                static_cast<int>(entry.raceClass),
                entry.isBossRef ? 1 : 0,
                entry.totalScore,
                entry.goldValue);
        }

        return quote;
    }

    int BuildEncounterQuoteGold(RE::Actor* speaker, PayContext context)
    {
        return BuildEncounterQuote(speaker, context).totalGold;
    }

    bool PrimeEncounterQuote(RE::Actor* speaker, PayContext context)
    {
        if (!speaker) {
            return false;
        }

        auto quote = BuildEncounterQuote(speaker, context);

        std::scoped_lock lk(gLock);
        gCachedQuote.valid = true;
        gCachedQuote.context = context;
        gCachedQuote.speakerFormID = speaker->GetFormID();
        gCachedQuote.quote = std::move(quote);
        gCachedQuote.builtAtSec = NowSec();

        spdlog::info(
            "[TFD][PayModel] cached quote speaker={:08X} context={} base={} total={} builtAt={:.2f}",
            gCachedQuote.speakerFormID,
            static_cast<int>(gCachedQuote.context),
            gCachedQuote.quote.baseGold,
            gCachedQuote.quote.totalGold,
            gCachedQuote.builtAtSec);

        return true;
    }

    int GetCachedEncounterQuote(RE::Actor* speaker, PayContext context)
    {
        std::scoped_lock lk(gLock);
        if (!gCachedQuote.valid) {
            return 0;
        }

        if (speaker && gCachedQuote.speakerFormID != speaker->GetFormID()) {
            return 0;
        }

        if (context != PayContext::None && gCachedQuote.context != context) {
            return 0;
        }

        return gCachedQuote.quote.totalGold;
    }

    const EncounterQuote* GetCachedEncounterQuoteData(RE::Actor* speaker, PayContext context)
    {
        std::scoped_lock lk(gLock);
        if (!gCachedQuote.valid) {
            return nullptr;
        }

        if (speaker && gCachedQuote.speakerFormID != speaker->GetFormID()) {
            return nullptr;
        }

        if (context != PayContext::None && gCachedQuote.context != context) {
            return nullptr;
        }

        return &gCachedQuote.quote;
    }
}
