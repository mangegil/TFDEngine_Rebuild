#include "TFDTameBait.h"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

namespace TFD::TameBait
{
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
            "TFDTameBait: collect target={:08X} category={} options={}",
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
            "TFDTameBait: consume item={:08X} count={}",
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
}
