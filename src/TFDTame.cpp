#include "TFDTame.h"

#include "TFDActorScan.h"
#include "TFDBleedout.h"
#include "TFDHostilityController.h"
#include "TFDDefeatMonitor.h"
#include "TFDSettings.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <initializer_list>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace TFD::Tame
{
    using Session = TFD::HostilityController::Session;

    namespace
    {
        constexpr double kCompanionFeedHours = 3.0;
        constexpr double kCompanionMaxHours = 9.0;
        constexpr double kTameDurationSec = 60.0;
        constexpr std::int32_t kCalmFeedCost = 1;
        constexpr std::int32_t kCompanionFeedCost = 2;
        constexpr const char* kCreatureTeammateAssignEvent = "TFDCreatureTeammateAssign";
        constexpr float kLocalHostileSplashRadiusMin = 1000.0f;
        constexpr float kLocalHostileSplashRadiusMax = 1800.0f;
        constexpr std::size_t kTamePackMaxMembers = 6;

        inline RuntimeProviders g_runtimeProviders{};

        auto& Entries()
        {
            return TFD::HostilityController::Runtime::Entries();
        }

        auto& Sessions()
        {
            return TFD::HostilityController::Runtime::Sessions();
        }


        bool IsActorStillValid(RE::Actor* actor)
        {
            return TFD::HostilityController::Internal::ValidateActor(actor);
        }

        float GetLocalHostileSplashRadius()
        {
            const float settingsRadius = TFD::Settings::GetSweepRadius();
            return std::clamp(settingsRadius, kLocalHostileSplashRadiusMin, kLocalHostileSplashRadiusMax);
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
            return combatTarget && combatTarget->GetFormID() == player->GetFormID();
        }

        RE::TESRace* GetActorRace(RE::Actor* actor)
        {
            if (!actor) {
                return nullptr;
            }

            auto* base = actor->GetActorBase();
            return base ? base->GetRace() : nullptr;
        }

        bool IsSamePackSpecies(RE::Actor* actor, RE::Actor* primaryTarget)
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
            (void)scanEntry;
            if (!IsActorStillValid(actor) || !player || !primaryTarget) {
                return false;
            }

            if (actor->GetFormID() == player->GetFormID() || actor->GetFormID() == primaryTarget->GetFormID()) {
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

        bool IsActorBoundToDifferentActiveTameSession(RE::FormID actorId, RE::FormID targetSessionId)
        {
            auto entryIt = Entries().find(actorId);
            if (entryIt == Entries().end()) {
                return false;
            }
            if (entryIt->second.sessionId == targetSessionId) {
                return false;
            }

            auto sessionIt = Sessions().find(entryIt->second.sessionId);
            if (sessionIt == Sessions().end()) {
                return false;
            }

            return !sessionIt->second.finished && sessionIt->second.primaryMode == TFD::HostilityController::Mode::Tame;
        }
    }

    namespace Internal
    {
        std::vector<RE::FormID> BuildPackMemberIds(
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

            const float effectiveRadius = std::clamp(splashRadius, kLocalHostileSplashRadiusMin, kLocalHostileSplashRadiusMax);
            const float scanRadius = effectiveRadius + 256.0f;
            TFD::ActorScan::Rescan(scanRadius, false);

            const auto count = TFD::ActorScan::GetCount();
            for (int i = 0; i < count; ++i) {
                auto scanEntry = TFD::ActorScan::GetEntry(i);
                auto* actor = TFD::ActorScan::GetActor(i);
                if (!IsEligibleLocalSplashActor(actor, player, primaryTarget, scanEntry, effectiveRadius)) {
                    continue;
                }
                if (!IsSamePackSpecies(actor, primaryTarget)) {
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
                        "TFDTame: reject pack primary={:08X} actor={:08X} reason=actor_bound_to_other_tame_session",
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
                    "TFDTame: reject pack primary={:08X} reason=pack_too_large size={} max={}",
                    primaryId,
                    static_cast<unsigned int>(result.size()),
                    static_cast<unsigned int>(kTamePackMaxMembers));
                return {};
            }

            return result;
        }

        void RefreshSessionEntries(TFD::HostilityController::Session& session, double nowSec, double durationSec)
        {
            const double effectiveDurationSec = durationSec > 0.0 ? durationSec : kTameDurationSec;
            const double endTimeSec = TFD::HostilityController::Runtime::ClampTameEndTime(nowSec, nowSec + effectiveDurationSec);

            if (session.startTimeSec <= 0.0) {
                session.startTimeSec = nowSec;
            }
            if (session.disposition == TameDisposition::Calm) {
                session.lastCalmRefreshSec = nowSec;
            }
            session.endTimeSec = (session.disposition == TameDisposition::Companion) ? 0.0 : endTimeSec;
            session.invalidSinceSec = 0.0;
            session.armedSinceSec = 0.0;
            session.tooFarSinceSec = 0.0;
            session.tameStartleSinceSec = 0.0;
            session.lastPlayerSampleSec = 0.0;
            session.lastPlayerPos = {};
            session.hasPlayerSample = false;

            for (auto& [actorId, entry] : Entries()) {
                (void)actorId;
                if (entry.sessionId != session.sessionId) {
                    continue;
                }
                if (entry.startTimeSec <= 0.0) {
                    entry.startTimeSec = nowSec;
                }
                entry.endTimeSec = (session.disposition == TameDisposition::Companion) ? 0.0 : endTimeSec;
                entry.disposition = session.disposition;
                entry.companionExpireGameDays = session.companionExpireGameDays;
                entry.temporaryTeammateApplied = session.temporaryTeammateApplied;
            }

            spdlog::info(
                "TFDTame: timer refresh session={} target={:08X} durationSec={:.2f} endTimeSec={:.2f}",
                session.sessionId,
                session.primaryTargetId,
                effectiveDurationSec,
                session.endTimeSec);
        }
    }

    namespace
    {
        struct CategoryLists
        {
            std::array<RE::BGSListForm*, 2> baitLists{ nullptr, nullptr };
            std::size_t count{ 0 };
        };

        static RE::BGSListForm* LookupListAny(std::initializer_list<const char*> editorIDs)
        {
            for (auto* id : editorIDs) {
                if (!id || !id[0]) {
                    continue;
                }
                if (auto* list = RE::TESForm::LookupByEditorID<RE::BGSListForm>(id)) {
                    return list;
                }
            }
            return nullptr;
        }

        static RE::BGSListForm* ListUndead()
        {
            static RE::BGSListForm* cached = LookupListAny({ "TFDBaitableUndeadRaceFL" });
            return cached;
        }

        static RE::BGSListForm* ListApocrypha()
        {
            static RE::BGSListForm* cached = LookupListAny({ "TFDBaitableApocryphaRaceFL", "TFDBaitableApocryphaRace" });
            return cached;
        }

        static RE::BGSListForm* ListMammoth()
        {
            static RE::BGSListForm* cached = LookupListAny({ "TFDBaitableMammothRaceFL" });
            return cached;
        }

        static RE::BGSListForm* ListNetch()
        {
            static RE::BGSListForm* cached = LookupListAny({ "TFDBaitableNetchRaceFL" });
            return cached;
        }

        static RE::BGSListForm* ListHorker()
        {
            static RE::BGSListForm* cached = LookupListAny({ "TFDBaitableHorkerRaceFL" });
            return cached;
        }

        static RE::BGSListForm* ListBear()
        {
            static RE::BGSListForm* cached = LookupListAny({ "TFDBaitableBearRaceFL" });
            return cached;
        }

        static RE::BGSListForm* ListTroll()
        {
            static RE::BGSListForm* cached = LookupListAny({ "TFDBaitableTrollRaceFL" });
            return cached;
        }

        static RE::BGSListForm* ListFeline()
        {
            static RE::BGSListForm* cached = LookupListAny({ "TFDBaitableFelineRaceFL" });
            return cached;
        }

        static RE::BGSListForm* ListCanine()
        {
            static RE::BGSListForm* cached = LookupListAny({ "TFDBaitableCanineRaceFL" });
            return cached;
        }

        static RE::BGSListForm* ListBristleback()
        {
            static RE::BGSListForm* cached = LookupListAny({ "TFDBaitableBristlebackRaceFL" });
            return cached;
        }

        static RE::BGSListForm* ListHerbivore()
        {
            static RE::BGSListForm* cached = LookupListAny({ "TFDBaitableHerbivoreFL" });
            return cached;
        }

        static RE::BGSListForm* ListOmnivore()
        {
            static RE::BGSListForm* cached = LookupListAny({ "TFDBaitableOmnivoreFL" });
            return cached;
        }

        static RE::BGSListForm* ListCarnivore()
        {
            static RE::BGSListForm* cached = LookupListAny({ "TFDBaitableCarnivoreFL", "TFDCarnivoreFL" });
            return cached;
        }

        static RE::BGSListForm* BaitSoulGem()
        {
            static RE::BGSListForm* cached = LookupListAny({ "TFDBaitSoulGemFL" });
            return cached;
        }

        static RE::BGSListForm* BaitApocrypha()
        {
            static RE::BGSListForm* cached = LookupListAny({ "TFDBaitApocryphaFL" });
            return cached;
        }

        static RE::BGSListForm* BaitPlant()
        {
            static RE::BGSListForm* cached = LookupListAny({ "TFDBaitPlantFL" });
            return cached;
        }

        static RE::BGSListForm* BaitFish()
        {
            static RE::BGSListForm* cached = LookupListAny({ "TFDBaitFishFL" });
            return cached;
        }

        static RE::BGSListForm* BaitSmallMeat()
        {
            static RE::BGSListForm* cached = LookupListAny({ "TFDBaitSmallMeatFL" });
            return cached;
        }

        static RE::BGSListForm* BaitMediumMeat()
        {
            static RE::BGSListForm* cached = LookupListAny({ "TFDBaitMediumMeatFL" });
            return cached;
        }

        static RE::BGSListForm* BaitHeavyMeat()
        {
            static RE::BGSListForm* cached = LookupListAny({ "TFDBaitHeavyMeatFL" });
            return cached;
        }

        static bool ActorMatchesList(RE::Actor* actor, RE::BGSListForm* list)
        {
            if (!actor || !list) {
                return false;
            }

            if (list->HasForm(actor)) {
                return true;
            }

            if (auto* race = actor->GetRace(); race && list->HasForm(race)) {
                return true;
            }

            if (auto* base = actor->GetBaseObject(); base && list->HasForm(base)) {
                return true;
            }

            return false;
        }

        static CategoryLists ResolveCategoryLists(Category category)
        {
            CategoryLists result{};
            switch (category) {
            case Category::SoulGem:
                result.baitLists[0] = BaitSoulGem();
                result.count = result.baitLists[0] ? 1u : 0u;
                break;
            case Category::Apocrypha:
                result.baitLists[0] = BaitApocrypha();
                result.count = result.baitLists[0] ? 1u : 0u;
                break;
            case Category::Plant:
                result.baitLists[0] = BaitPlant();
                result.count = result.baitLists[0] ? 1u : 0u;
                break;
            case Category::Fish:
                result.baitLists[0] = BaitFish();
                result.baitLists[1] = BaitHeavyMeat();
                result.count = result.baitLists[1] ? 2u : (result.baitLists[0] ? 1u : 0u);
                break;
            case Category::SmallMeat:
                result.baitLists[0] = BaitSmallMeat();
                result.baitLists[1] = BaitMediumMeat();
                result.count = result.baitLists[1] ? 2u : (result.baitLists[0] ? 1u : 0u);
                break;
            case Category::MediumMeat:
                result.baitLists[0] = BaitMediumMeat();
                result.baitLists[1] = BaitPlant();
                result.count = result.baitLists[1] ? 2u : (result.baitLists[0] ? 1u : 0u);
                break;
            case Category::HeavyMeat:
                result.baitLists[0] = BaitHeavyMeat();
                result.baitLists[1] = BaitFish();
                result.count = result.baitLists[1] ? 2u : (result.baitLists[0] ? 1u : 0u);
                break;
            case Category::None:
            default:
                break;
            }
            return result;
        }

        static double ExtendSecondsForCategory(Category category)
        {
            switch (category) {
            case Category::SmallMeat:
                return 20.0;
            case Category::MediumMeat:
                return 30.0;
            case Category::HeavyMeat:
                return 45.0;
            case Category::Fish:
                return 30.0;
            case Category::Plant:
                return 25.0;
            case Category::SoulGem:
                return 40.0;
            case Category::Apocrypha:
                return 40.0;
            case Category::None:
            default:
                return 0.0;
            }
        }

        static Category DetectCategory(RE::Actor* target)
        {
            if (!target) {
                return Category::None;
            }

            if (ActorMatchesList(target, ListUndead())) {
                return Category::SoulGem;
            }
            if (ActorMatchesList(target, ListApocrypha())) {
                return Category::Apocrypha;
            }
            if (ActorMatchesList(target, ListMammoth())) {
                return Category::Plant;
            }
            if (ActorMatchesList(target, ListNetch())) {
                return Category::Plant;
            }
            if (ActorMatchesList(target, ListHorker())) {
                return Category::Fish;
            }
            if (ActorMatchesList(target, ListBear())) {
                return Category::HeavyMeat;
            }
            if (ActorMatchesList(target, ListTroll())) {
                return Category::HeavyMeat;
            }
            if (ActorMatchesList(target, ListFeline())) {
                return Category::MediumMeat;
            }
            if (ActorMatchesList(target, ListCanine())) {
                return Category::SmallMeat;
            }
            if (ActorMatchesList(target, ListBristleback())) {
                return Category::MediumMeat;
            }
            if (ActorMatchesList(target, ListHerbivore())) {
                return Category::Plant;
            }
            if (ActorMatchesList(target, ListOmnivore())) {
                return Category::MediumMeat;
            }
            if (ActorMatchesList(target, ListCarnivore())) {
                return Category::MediumMeat;
            }

            return Category::None;
        }
    }

    Category ResolveCategory(RE::Actor* target)
    {
        return DetectCategory(target);
    }

    std::vector<Option> CollectValidBaits(RE::Actor* player, RE::Actor* target)
    {
        std::vector<Option> result{};
        if (!player || !target) {
            return result;
        }

        const auto category = DetectCategory(target);
        const auto categoryLists = ResolveCategoryLists(category);
        if (category == Category::None || categoryLists.count == 0) {
            return result;
        }

        const auto inv = player->GetInventory([](RE::TESBoundObject&) {
            return true;
        }, true);

        std::unordered_map<RE::FormID, Option> dedup{};

        for (const auto& [item, invData] : inv) {
            const auto& [count, entry] = invData;
            (void)entry;
            if (!item || count <= 0) {
                continue;
            }

            Category matchedCategory = Category::None;
            for (std::size_t i = 0; i < categoryLists.count; ++i) {
                auto* list = categoryLists.baitLists[i];
                if (list && list->HasForm(item)) {
                    matchedCategory = category;
                    break;
                }
            }
            if (matchedCategory == Category::None) {
                continue;
            }

            Option opt{};
            opt.item = item;
            opt.name = item->GetName();
            opt.count = count;
            opt.extendSec = ExtendSecondsForCategory(matchedCategory);
            opt.category = matchedCategory;
            dedup[item->GetFormID()] = std::move(opt);
        }

        result.reserve(dedup.size());
        for (auto& [id, opt] : dedup) {
            (void)id;
            result.push_back(std::move(opt));
        }

        std::sort(result.begin(), result.end(), [](const Option& a, const Option& b) {
            if (a.extendSec != b.extendSec) {
                return a.extendSec > b.extendSec;
            }
            if (a.count != b.count) {
                return a.count > b.count;
            }
            const auto aId = a.item ? a.item->GetFormID() : 0u;
            const auto bId = b.item ? b.item->GetFormID() : 0u;
            return aId < bId;
        });

        spdlog::info(
            "TFDTame: collect target={:08X} category={} options={}",
            target->GetFormID(),
            ToString(category),
            result.size());

        return result;
    }

    std::optional<Option> SelectBestBait(const std::vector<Option>& options)
    {
        if (options.empty()) {
            return std::nullopt;
        }
        return options.front();
    }

    bool ConsumeBait(RE::Actor* player, RE::TESBoundObject* item, std::int32_t count)
    {
        if (!player || !item || count <= 0) {
            return false;
        }

        player->RemoveItem(item, count, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
        spdlog::info(
            "TFDTame: consume item={:08X} count={}",
            item->GetFormID(),
            count);
        return true;
    }

    const char* ToString(Category category)
    {
        switch (category) {
        case Category::SoulGem:
            return "SoulGem";
        case Category::Apocrypha:
            return "Apocrypha";
        case Category::Plant:
            return "Plant";
        case Category::Fish:
            return "Fish";
        case Category::SmallMeat:
            return "SmallMeat";
        case Category::MediumMeat:
            return "MediumMeat";
        case Category::HeavyMeat:
            return "HeavyMeat";
        case Category::None:
        default:
            return "None";
        }
    }

    bool CanStart(RE::Actor* actor)
    {
        return actor && !HasActiveSession(actor);
    }

    bool HasActiveSession(RE::Actor* actor)
    {
        if (!actor) {
            return false;
        }

        auto entryIt = Entries().find(actor->GetFormID());
        if (entryIt == Entries().end()) {
            return false;
        }

        auto sessionIt = Sessions().find(entryIt->second.sessionId);
        if (sessionIt == Sessions().end() || sessionIt->second.finished) {
            return false;
        }

        return sessionIt->second.primaryMode == TFD::HostilityController::Mode::Tame;
    }

    std::optional<RE::FormID> BeginSession(
        RE::Actor* player,
        RE::Actor* primaryTarget,
        double nowSec,
        bool allowDialogue,
        bool allowLocalSplash)
    {
        if (!CanStart(primaryTarget)) {
            if (primaryTarget) {
                spdlog::info(
                    "TFDTame: reject begin target={:08X} reason=active_tame_requires_feed",
                    primaryTarget->GetFormID());
            }
            return std::nullopt;
        }

        if (!allowLocalSplash) {
            return TFD::HostilityController::Internal::BeginTameBaseSession(player, primaryTarget, nowSec, allowDialogue);
        }

        bool tamePackRejected = false;
        auto tamePackIds = TFD::Tame::Internal::BuildPackMemberIds(
            player,
            primaryTarget,
            GetLocalHostileSplashRadius(),
            tamePackRejected);
        if (tamePackRejected || tamePackIds.empty()) {
            return std::nullopt;
        }

        for (auto actorId : tamePackIds) {
            auto* actor = TFD::HostilityController::Runtime::ResolveActor(actorId);
            if (!IsActorStillValid(actor) || !actor->Is3DLoaded()) {
                spdlog::info(
                    "TFDTame: reject pack primary={:08X} actor={:08X} reason=pack_member_not_ready_before_batch",
                    primaryTarget ? primaryTarget->GetFormID() : 0,
                    actorId);
                return std::nullopt;
            }

            if (!CanStart(actor)) {
                spdlog::info(
                    "TFDTame: reject pack primary={:08X} actor={:08X} reason=actor_already_has_active_tame",
                    primaryTarget ? primaryTarget->GetFormID() : 0,
                    actorId);
                return std::nullopt;
            }

            if (TFD::Tame::CollectValidBaits(player, actor).empty()) {
                spdlog::info(
                    "TFDTame: reject pack primary={:08X} actor={:08X} reason=no_valid_bait_for_pack_member",
                    primaryTarget ? primaryTarget->GetFormID() : 0,
                    actorId);
                return std::nullopt;
            }
        }

        std::vector<RE::FormID> createdSessionIds;
        createdSessionIds.reserve(tamePackIds.size());

        std::optional<RE::FormID> primarySessionId;
        for (auto actorId : tamePackIds) {
            auto* actor = TFD::HostilityController::Runtime::ResolveActor(actorId);
            if (!actor) {
                for (auto createdId : createdSessionIds) {
                    TFD::HostilityController::ReleaseSession(createdId, ReleaseReason::Generic);
                }
                spdlog::info(
                    "TFDTame: reject pack primary={:08X} actor={:08X} reason=pack_member_lost_during_batch",
                    primaryTarget ? primaryTarget->GetFormID() : 0,
                    actorId);
                return std::nullopt;
            }

            auto sessionId = TFD::HostilityController::Internal::BeginTameBaseSession(player, actor, nowSec, allowDialogue);
            if (!sessionId.has_value()) {
                for (auto createdId : createdSessionIds) {
                    TFD::HostilityController::ReleaseSession(createdId, ReleaseReason::Generic);
                }
                spdlog::info(
                    "TFDTame: reject pack primary={:08X} actor={:08X} reason=batch_session_begin_failed",
                    primaryTarget ? primaryTarget->GetFormID() : 0,
                    actorId);
                return std::nullopt;
            }

            createdSessionIds.push_back(*sessionId);
            if (primaryTarget && actorId == primaryTarget->GetFormID()) {
                primarySessionId = *sessionId;
            }
        }

        spdlog::info(
            "TFDTame: pack batch success primary={:08X} members={} primarySession={} separateSessions=1",
            primaryTarget ? primaryTarget->GetFormID() : 0,
            static_cast<unsigned int>(tamePackIds.size()),
            primarySessionId.value_or(0));

        return primarySessionId;
    }

    void InstallRuntimeProviders(RuntimeProviders providers)
    {
        g_runtimeProviders = std::move(providers);
    }

    void ResetRuntimeProviders()
    {
        g_runtimeProviders = {};
    }

    void ReleaseBleedNoSpeakerTameSession(const char* reason)
    {
        if (g_runtimeProviders.releaseBleedNoSpeakerTameSession) {
            g_runtimeProviders.releaseBleedNoSpeakerTameSession(reason);
            return;
        }
        TFD::Bleedout::ReleaseNoSpeakerTameSession(reason);
    }

    bool TryEnsureBleedNoSpeakerTameSession(const std::vector<RE::Actor*>& actors, const char* reason)
    {
        if (g_runtimeProviders.tryEnsureBleedNoSpeakerTameSession) {
            return g_runtimeProviders.tryEnsureBleedNoSpeakerTameSession(actors, reason);
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return false;
        }
        return TFD::Bleedout::TryEnsureNoSpeakerTameSession(actors, player, reason, {});
    }

    std::chrono::steady_clock::time_point GetBleedNoSpeakerTameLastAttempt()
    {
        return TFD::Bleedout::GetNoSpeakerTameLastAttempt();
    }

    void SetBleedNoSpeakerTameLastAttempt(std::chrono::steady_clock::time_point when)
    {
        TFD::Bleedout::SetNoSpeakerTameLastAttempt(when);
    }

    bool RecruitDefeatedCreatureAsTeammate(RE::Actor* actor, double nowSec)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !actor) {
            return false;
        }
        if (g_runtimeProviders.isCreatureDefeatedEnemy && !g_runtimeProviders.isCreatureDefeatedEnemy(actor)) {
            return false;
        }
        if (g_runtimeProviders.getDefeatedEnemyRemainingSeconds && g_runtimeProviders.getDefeatedEnemyRemainingSeconds(actor) <= 0.0) {
            return false;
        }

        auto session = BeginSession(player, actor, nowSec, false, false);
        if (!session.has_value()) {
            spdlog::warn("[TFD][Tame] defeated creature recruit failed actor={:08X} reason=begin_tame_failed", actor->GetFormID());
            return false;
        }
        if (!PromoteToCompanion(actor, 24.0)) {
            Release(actor, ReleaseReason::Generic);
            spdlog::warn("[TFD][Tame] defeated creature recruit failed actor={:08X} reason=promote_failed", actor->GetFormID());
            return false;
        }
        if (g_runtimeProviders.suppressDefeatedReentry) {
            g_runtimeProviders.suppressDefeatedReentry(actor, 6.0, "defeated_creature_recruit");
        }
        if (g_runtimeProviders.releaseBleedLock) {
            g_runtimeProviders.releaseBleedLock(actor, "defeated_creature_recruit", true);
        }
        if (g_runtimeProviders.restoreActorHealthToSafePct) {
            g_runtimeProviders.restoreActorHealthToSafePct(actor, TFD::Settings::GetEnemyDownedThresholdPct(), 0.12f, 0.58f, 0.92f, 45.0f, "defeated_creature_recruit");
        }
        if (actor->IsInCombat()) {
            actor->StopCombat();
        }
        actor->DrawWeaponMagicHands(false);
        spdlog::info("[TFD][Tame] defeated creature recruit actor={:08X}", actor->GetFormID());
        return true;
    }

    std::vector<ActiveSnapshot> GetActiveSnapshots(double nowSec)
    {
        if (nowSec <= 0.0) {
            nowSec = TFD::HostilityController::Runtime::NowSec();
        }

        std::vector<ActiveSnapshot> result{};
        result.reserve(Sessions().size());

        for (const auto& [sessionId, session] : Sessions()) {
            if (session.finished || session.primaryMode != TFD::HostilityController::Mode::Tame || session.primaryTargetId == 0) {
                continue;
            }

            ActiveSnapshot snap{};
            snap.actorId = session.primaryTargetId;
            snap.sessionId = sessionId;
            snap.mode = session.primaryMode;
            snap.disposition = session.disposition;
            snap.remainingTameSec = session.disposition == TameDisposition::Companion || session.endTimeSec <= 0.0 ?
                0.0 :
                (std::max)(0.0, session.endTimeSec - nowSec);

            if (session.disposition == TameDisposition::Companion && session.companionExpireGameDays > 0.0) {
                const double remainingDays = session.companionExpireGameDays - TFD::HostilityController::Runtime::GameDays();
                snap.remainingCompanionHours = (std::max)(0.0, remainingDays * 24.0);
            }

            if (auto* actor = TFD::HostilityController::Runtime::ResolveActor(snap.actorId)) {
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

        std::sort(result.begin(), result.end(), [](const ActiveSnapshot& a, const ActiveSnapshot& b) {
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

    std::vector<FeedOptionSnapshot> GetFeedOptions(RE::Actor* actor, FeedAction action)
    {
        std::vector<FeedOptionSnapshot> result{};
        if (!actor || !HasActiveSession(actor)) {
            return result;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return result;
        }

        const bool downed = TFD::DefeatMonitor::IsThresholdDownedActor(actor);
        const auto options = TFD::Tame::CollectValidBaits(player, actor);
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
                } else {
                    std::snprintf(buffer, sizeof(buffer), "%s x%d (cost %d, +%.0fh)", opt.name.c_str(), opt.count, cost, kCompanionFeedHours);
                }
            } else {
                if (downed) {
                    std::snprintf(buffer, sizeof(buffer), "%s x%d (cost %d, revive + heal, +%.0fs)", opt.name.c_str(), opt.count, cost, opt.extendSec);
                } else {
                    std::snprintf(buffer, sizeof(buffer), "%s x%d (cost %d, +%.0fs)", opt.name.c_str(), opt.count, cost, opt.extendSec);
                }
            }
            bait.label = buffer;
            result.push_back(std::move(bait));
        }

        return result;
    }

    bool ApplyFeed(RE::Actor* actor, RE::FormID itemId, FeedAction action)
    {
        if (!actor || itemId == 0 || !HasActiveSession(actor)) {
            return false;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return false;
        }

        const bool reviveAfterFeed = TFD::DefeatMonitor::IsThresholdDownedActor(actor);
        const auto options = GetFeedOptions(actor, action);
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
                ok = ExtendCompanionHours(actor, kCompanionFeedHours);
            } else {
                ok = PromoteToCompanion(actor, kCompanionFeedHours);
            }
        } else {
            ok = ExtendSession(actor, it->calmExtendSec, 0.0);
        }

        if (!ok) {
            return false;
        }

        if (!TFD::Tame::ConsumeBait(player, item, it->cost)) {
            return false;
        }

        if (reviveAfterFeed) {
            const float reviveHealPct = action == FeedAction::Teammate ? 60.0f : 45.0f;
            if (!TFD::DefeatMonitor::ReviveDownedAlly(actor, reviveHealPct)) {
                spdlog::warn("TFDTame: feed revive failed actor={:08X} action={}", actor->GetFormID(), action == FeedAction::Teammate ? "teammate" : "calm");
            }
        }

        return true;
    }

    bool ExtendSession(RE::Actor* actor, double addSec, double nowSec)
    {
        if (!actor || addSec <= 0.0) {
            return false;
        }

        if (nowSec <= 0.0) {
            nowSec = TFD::HostilityController::Runtime::NowSec();
        }

        auto entryIt = Entries().find(actor->GetFormID());
        if (entryIt == Entries().end()) {
            return false;
        }

        auto sessionIt = Sessions().find(entryIt->second.sessionId);
        if (sessionIt == Sessions().end() || sessionIt->second.finished) {
            return false;
        }

        Session& session = sessionIt->second;
        if (session.primaryMode != TFD::HostilityController::Mode::Tame || session.disposition == TameDisposition::Companion) {
            return false;
        }

        const double baseEndTime = session.endTimeSec > nowSec ? session.endTimeSec : nowSec;
        session.endTimeSec = TFD::HostilityController::Runtime::ClampTameEndTime(nowSec, baseEndTime + addSec);
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

        for (auto& [actorId, entry] : Entries()) {
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
            "TFDTame: extend session={} target={:08X} addSec={:.2f} endTimeSec={:.2f} calmRefreshSec={:.2f}",
            session.sessionId,
            session.primaryTargetId,
            addSec,
            session.endTimeSec,
            session.lastCalmRefreshSec);
        return true;
    }

    double GetRemainingTime(RE::Actor* actor, double nowSec)
    {
        if (!actor) {
            return 0.0;
        }

        if (nowSec <= 0.0) {
            nowSec = TFD::HostilityController::Runtime::NowSec();
        }

        auto entryIt = Entries().find(actor->GetFormID());
        if (entryIt == Entries().end()) {
            return 0.0;
        }

        auto sessionIt = Sessions().find(entryIt->second.sessionId);
        if (sessionIt == Sessions().end() || sessionIt->second.finished) {
            return 0.0;
        }

        const Session& session = sessionIt->second;
        if (session.primaryMode != TFD::HostilityController::Mode::Tame || session.disposition == TameDisposition::Companion || session.endTimeSec <= 0.0) {
            return 0.0;
        }

        return (std::max)(0.0, session.endTimeSec - nowSec);
    }

    TameDisposition GetDisposition(RE::Actor* actor)
    {
        if (!actor) {
            return TameDisposition::None;
        }

        auto entryIt = Entries().find(actor->GetFormID());
        if (entryIt == Entries().end()) {
            return TameDisposition::None;
        }

        auto sessionIt = Sessions().find(entryIt->second.sessionId);
        if (sessionIt == Sessions().end() || sessionIt->second.finished) {
            return entryIt->second.disposition;
        }

        return sessionIt->second.disposition;
    }

    bool PromoteToCompanion(RE::Actor* actor, double addHoursGameTime)
    {
        if (!actor || addHoursGameTime <= 0.0) {
            return false;
        }

        auto entryIt = Entries().find(actor->GetFormID());
        if (entryIt == Entries().end()) {
            return false;
        }

        auto sessionIt = Sessions().find(entryIt->second.sessionId);
        if (sessionIt == Sessions().end() || sessionIt->second.finished) {
            return false;
        }

        Session& session = sessionIt->second;
        if (session.primaryMode != TFD::HostilityController::Mode::Tame || session.primaryTargetId != actor->GetFormID()) {
            return false;
        }

        const double nowGameDays = TFD::HostilityController::Runtime::GameDays();
        const double addDays = addHoursGameTime / 24.0;
        const double maxDays = nowGameDays + (kCompanionMaxHours / 24.0);
        const bool wasCompanion = session.disposition == TameDisposition::Companion;
        const double baseGameDays = session.companionExpireGameDays > nowGameDays ? session.companionExpireGameDays : nowGameDays;
        const double oldExpireGameDays = session.companionExpireGameDays;
        const double newExpireGameDays = (std::min)(maxDays, baseGameDays + addDays);
        if (wasCompanion && newExpireGameDays <= oldExpireGameDays + 1e-6) {
            spdlog::info(
                "TFDTame: reject promote companion session={} target={:08X} reason=companion_at_max expireGameDays={:.4f}",
                session.sessionId,
                session.primaryTargetId,
                oldExpireGameDays);
            return false;
        }

        if (!wasCompanion) {
            TFD::HostilityController::Runtime::SendModEvent("TFDTameUnassign", actor);
            TFD::HostilityController::Runtime::SendModEvent(kCreatureTeammateAssignEvent, actor);
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

        TFD::HostilityController::Runtime::SyncSessionDisposition(session);

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
            "TFDTame: promote companion session={} target={:08X} addHours={:.2f} expireGameDays={:.4f}",
            session.sessionId,
            session.primaryTargetId,
            addHoursGameTime,
            session.companionExpireGameDays);
        return true;
    }

    bool ExtendCompanionHours(RE::Actor* actor, double addHoursGameTime)
    {
        if (!actor || addHoursGameTime <= 0.0) {
            return false;
        }

        auto entryIt = Entries().find(actor->GetFormID());
        if (entryIt == Entries().end()) {
            return false;
        }

        auto sessionIt = Sessions().find(entryIt->second.sessionId);
        if (sessionIt == Sessions().end() || sessionIt->second.finished) {
            return false;
        }

        Session& session = sessionIt->second;
        if (session.primaryMode != TFD::HostilityController::Mode::Tame || session.disposition != TameDisposition::Companion) {
            return false;
        }

        const double nowGameDays = TFD::HostilityController::Runtime::GameDays();
        const double addDays = addHoursGameTime / 24.0;
        const double maxDays = nowGameDays + (kCompanionMaxHours / 24.0);
        const double baseGameDays = session.companionExpireGameDays > nowGameDays ? session.companionExpireGameDays : nowGameDays;
        const double oldExpireGameDays = session.companionExpireGameDays;
        const double newExpireGameDays = (std::min)(maxDays, baseGameDays + addDays);
        if (newExpireGameDays <= oldExpireGameDays + 1e-6) {
            spdlog::info(
                "TFDTame: reject extend companion session={} target={:08X} reason=companion_at_max expireGameDays={:.4f}",
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
        TFD::HostilityController::Runtime::SyncSessionDisposition(session);

        spdlog::info(
            "TFDTame: extend companion session={} target={:08X} addHours={:.2f} expireGameDays={:.4f}",
            session.sessionId,
            session.primaryTargetId,
            addHoursGameTime,
            session.companionExpireGameDays);
        return true;
    }

    bool Release(RE::Actor* actor, ReleaseReason reason)
    {
        if (!actor) {
            return false;
        }

        auto entryIt = Entries().find(actor->GetFormID());
        if (entryIt == Entries().end()) {
            return false;
        }

        auto sessionIt = Sessions().find(entryIt->second.sessionId);
        if (sessionIt == Sessions().end() || sessionIt->second.finished || sessionIt->second.primaryMode != TFD::HostilityController::Mode::Tame) {
            return false;
        }

        TFD::HostilityController::ReleaseSession(sessionIt->second.sessionId, reason);
        return true;
    }

    bool IsCompanion(RE::Actor* actor)
    {
        return GetDisposition(actor) == TameDisposition::Companion;
    }

    double GetRemainingCompanionHours(RE::Actor* actor)
    {
        if (!actor) {
            return 0.0;
        }

        auto entryIt = Entries().find(actor->GetFormID());
        if (entryIt == Entries().end()) {
            return 0.0;
        }

        auto sessionIt = Sessions().find(entryIt->second.sessionId);
        if (sessionIt == Sessions().end() || sessionIt->second.finished) {
            return 0.0;
        }

        const Session& session = sessionIt->second;
        if (session.primaryMode != TFD::HostilityController::Mode::Tame || session.disposition != TameDisposition::Companion || session.companionExpireGameDays <= 0.0) {
            return 0.0;
        }

        const double remainingDays = session.companionExpireGameDays - TFD::HostilityController::Runtime::GameDays();
        return (std::max)(0.0, remainingDays * 24.0);
    }
}
