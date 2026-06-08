#include "TFDWorkNative.h"

#include <RE/Skyrim.h>
#include <RE/B/BGSConstructibleObject.h>
#include <RE/E/Effect.h>
#include <RE/M/MagicItem.h>
#include <RE/T/TESContainer.h>
#include <RE/T/TESDataHandler.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "EditorIdCache.h"
#include "TFDLocation.h"

namespace TFD::WorkNative
{
    namespace
    {
        enum class WorkStation : std::int32_t
        {
            None = 0,
            Forge = 1,
            Smelter = 2,
            Tanning = 3,
            SharpeningWheel = 4,
            Workbench = 5,
            ChoppingBlock = 6,
            Cooking = 7,
            Alchemy = 8,
            Enchanting = 9
        };

        struct RecipeIngredient
        {
            RE::TESForm* item{ nullptr };
            std::int32_t count{ 0 };
        };

        struct WorkRecipe
        {
            WorkStation station{ WorkStation::None };
            RE::TESForm* result{ nullptr };
            std::int32_t resultCount{ 1 };
            std::array<RecipeIngredient, 3> ingredients{};
            std::int32_t ingredientCount{ 0 };
            RE::FormID recipeFormID{ 0 };
            std::string recipeEditorID{};
            std::string resultEditorID{};
            std::int32_t estimatedTier{ 0 };
        };

        WorkRecipe g_lastWorkRecipe{};
        bool g_hasWorkRecipe{ false };

        enum class WorkJob : std::int32_t
        {
            None = 0,
            Mining = 1,
            Crafting = 2,
            Pleasure = 3,
            Chopping = 4,
            Cooking = 5,
            Alchemy = 6,
            Enchanting = 7,
            Improve = 8,
            NoJob = 9
        };

        struct SelectedWorkJob
        {
            WorkJob job{ WorkJob::None };
            WorkStation station{ WorkStation::None };
            std::int32_t miningState{ 0 };
            bool hasRecipe{ false };
            WorkRecipe recipe{};
            std::string reason{};
        };

        struct ActorWorkMemory
        {
            WorkJob lastJob{ WorkJob::None };
            WorkStation lastStation{ WorkStation::None };
            std::uint32_t repeatStreak{ 0 };
            std::uint32_t recentMask{ 0 };
            bool improveUsedThisRuntime{ false };
            std::vector<RE::FormID> improvedResultFormIDs{};
        };

        SelectedWorkJob g_selectedWorkJob{};
        bool g_hasSelectedWorkJob{ false };
        RE::FormID g_selectedWorkActorID{ 0 };
        std::unordered_map<RE::FormID, ActorWorkMemory> g_actorWorkMemory{};

        RE::TESGlobal* g_miningStateGlobal{ nullptr };
        RE::TESGlobal* g_craftingStateGlobal{ nullptr };
        RE::TESGlobal* g_workJobTypeGlobal{ nullptr };
        RE::TESGlobal* g_workAssignmentStateGlobal{ nullptr };
        RE::TESGlobal* g_forgeStateGlobal{ nullptr };
        RE::TESGlobal* g_smelterStateGlobal{ nullptr };
        RE::TESGlobal* g_tanningStateGlobal{ nullptr };
        RE::TESGlobal* g_sharpeningStateGlobal{ nullptr };
        RE::TESGlobal* g_workbenchStateGlobal{ nullptr };
        RE::TESGlobal* g_choppingStateGlobal{ nullptr };
        RE::TESGlobal* g_cookingStateGlobal{ nullptr };
        RE::TESGlobal* g_alchemyStateGlobal{ nullptr };
        RE::TESGlobal* g_enchantingStateGlobal{ nullptr };
        RE::TESFaction* g_desireFaction{ nullptr };

        struct PlannedActorWork
        {
            RE::FormID actorID{ 0 };
            std::vector<SelectedWorkJob> jobs{};
            std::uint32_t usedMask{ 0 };
            WorkJob lastSelectedJob{ WorkJob::None };
            WorkStation lastSelectedStation{ WorkStation::None };
            bool noJobCooldown{ false };
        };

        std::unordered_map<RE::FormID, PlannedActorWork> g_sessionPlans{};
        std::vector<RE::FormID> g_sessionBossOrder{};
        RE::FormID g_sessionSelectedActorID{ 0 };
        std::uint32_t g_sessionSerial{ 0 };
        bool g_hasSessionPlan{ false };

        std::string Lower(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
                });
            return value;
        }

        bool ContainsInsensitive(const std::string& value, const char* needle)
        {
            if (!needle || !needle[0]) {
                return false;
            }
            return Lower(value).find(Lower(needle)) != std::string::npos;
        }

        std::string EditorID(RE::TESForm* form)
        {
            return TFD::Util::GetEditorId(form);
        }

        bool IsUsableCampfireEditor(const std::string& editorID)
        {
            if (editorID.empty()) {
                return false;
            }

            // R225A: Usable Campfires exposes UC_Campfire* activators and a
            // UC_CampfireUse dummy furniture/menu. Recipes shown there are the
            // mod's UC_CampfireRecipe* COBJ records, not arbitrary vanilla cooking
            // pot recipes such as Cabbage Soup.
            return ContainsInsensitive(editorID, "UC_Campfire") ||
                ContainsInsensitive(editorID, "UsableCampfire") ||
                ContainsInsensitive(editorID, "CampfireUse");
        }

        bool IsCoreGameWorkForm(RE::TESForm* form)
        {
            if (!form) {
                return false;
            }

            const auto formID = form->GetFormID();
            const auto loadOrderIndex = static_cast<std::uint8_t>((formID >> 24) & 0xFF);

            // Captive work must stay deterministic and reachable. Random recipes from
            // cosmetic/item mods often use custom crafting conditions, hidden stations,
            // or nonstandard inventory behavior. Keep radiant work assignment limited to
            // Skyrim.esm + official DLC masters in the normal SE load order.
            return loadOrderIndex <= 0x04;
        }

        bool HasSpecialWorkBlockToken(const std::string& lowerText)
        {
            if (lowerText.empty()) {
                return false;
            }

            // These are not normal captive-work recipes. They are unique, quest-gated,
            // hidden-station, non-playable, or menu-scripted objects that can pass the
            // simple bench-keyword/core-master filter but still never appear at an
            // ordinary nearby workstation.
            return lowerText.find("aetherial") != std::string::npos ||
                lowerText.find("aetherium") != std::string::npos ||
                lowerText.find("dlc1ld") != std::string::npos ||
                lowerText.find("nightingale") != std::string::npos ||
                lowerText.find("skyforge") != std::string::npos ||
                lowerText.find("nordhero") != std::string::npos ||
                lowerText.find("nord hero") != std::string::npos ||
                lowerText.find("ancientnord") != std::string::npos ||
                lowerText.find("ancient nord") != std::string::npos ||
                lowerText.find("draugr") != std::string::npos ||
                lowerText.find("honed") != std::string::npos ||
                lowerText.find("chillrend") != std::string::npos ||
                lowerText.find("dawnbreaker") != std::string::npos ||
                lowerText.find("bladeofwoe") != std::string::npos ||
                lowerText.find("mehrunes") != std::string::npos ||
                lowerText.find("molag") != std::string::npos ||
                lowerText.find("wabbajack") != std::string::npos ||
                lowerText.find("sanguine") != std::string::npos ||
                lowerText.find("volendrung") != std::string::npos ||
                lowerText.find("spellbreaker") != std::string::npos ||
                lowerText.find("azura") != std::string::npos ||
                lowerText.find("hircine") != std::string::npos ||
                lowerText.find("savior") != std::string::npos ||
                lowerText.find("saviour") != std::string::npos ||
                lowerText.find("ebonyblade") != std::string::npos ||
                lowerText.find("maceof") != std::string::npos ||
                lowerText.find("nonplayable") != std::string::npos ||
                lowerText.find("non_playable") != std::string::npos ||
                lowerText.find("dummy") != std::string::npos ||
                lowerText.find("token") != std::string::npos ||
                lowerText.find("quest") != std::string::npos ||
                lowerText.find("crest") != std::string::npos;
        }

        bool IsRejectedSpecialWorkText(const std::string& text)
        {
            return HasSpecialWorkBlockToken(Lower(text));
        }

        template <class T>
        auto* Ptr(T& value)
        {
            using Clean = std::remove_reference_t<T>;
            if constexpr (std::is_pointer_v<Clean>) {
                return value;
            }
            else {
                return std::addressof(value);
            }
        }

        template <class Field>
        RE::TESForm* CoerceForm(Field value)
        {
            using Clean = std::remove_cv_t<std::remove_reference_t<Field>>;
            if constexpr (std::is_pointer_v<Clean> && std::is_convertible_v<Clean, RE::TESForm*>) {
                return value;
            }
            else {
                return nullptr;
            }
        }

        template <class Component>
        RE::TESForm* ComponentItem(Component& component)
        {
            auto* ptr = Ptr(component);
            if (!ptr) {
                return nullptr;
            }

            if constexpr (requires { ptr->object; }) {
                if (auto* form = CoerceForm(ptr->object)) {
                    return form;
                }
            }
            if constexpr (requires { ptr->obj; }) {
                if (auto* form = CoerceForm(ptr->obj)) {
                    return form;
                }
            }
            if constexpr (requires { ptr->item; }) {
                if (auto* form = CoerceForm(ptr->item)) {
                    return form;
                }
            }
            if constexpr (requires { ptr->form; }) {
                if (auto* form = CoerceForm(ptr->form)) {
                    return form;
                }
            }
            if constexpr (requires { ptr->component; }) {
                if (auto* form = CoerceForm(ptr->component)) {
                    return form;
                }
            }
            if constexpr (requires { ptr->componentForm; }) {
                if (auto* form = CoerceForm(ptr->componentForm)) {
                    return form;
                }
            }
            return nullptr;
        }

        template <class Component>
        std::int32_t ComponentCount(Component& component)
        {
            auto* ptr = Ptr(component);
            if (!ptr) {
                return 0;
            }

            if constexpr (requires { ptr->count; }) {
                return std::max(1, static_cast<std::int32_t>(ptr->count));
            }
            else if constexpr (requires { ptr->quantity; }) {
                return std::max(1, static_cast<std::int32_t>(ptr->quantity));
            }
            else if constexpr (requires { ptr->numItems; }) {
                return std::max(1, static_cast<std::int32_t>(ptr->numItems));
            }
            else if constexpr (requires { ptr->amount; }) {
                return std::max(1, static_cast<std::int32_t>(ptr->amount));
            }
            else {
                return 1;
            }
        }

        template <class Recipe>
        RE::TESForm* RecipeResult(Recipe* recipe)
        {
            if (!recipe) {
                return nullptr;
            }

            if constexpr (requires { recipe->createdItem; }) {
                return recipe->createdItem;
            }
            else if constexpr (requires { recipe->createdObject; }) {
                return recipe->createdObject;
            }
            else if constexpr (requires { recipe->resultItem; }) {
                return recipe->resultItem;
            }
            else if constexpr (requires { recipe->item; }) {
                return recipe->item;
            }
            else if constexpr (requires { recipe->data.createdItem; }) {
                return recipe->data.createdItem;
            }
            else {
                return nullptr;
            }
        }

        template <class Recipe>
        std::int32_t RecipeResultCount(Recipe* recipe)
        {
            if (!recipe) {
                return 1;
            }

            if constexpr (requires { recipe->createdItemCount; }) {
                return std::max(1, static_cast<std::int32_t>(recipe->createdItemCount));
            }
            else if constexpr (requires { recipe->amountProduced; }) {
                return std::max(1, static_cast<std::int32_t>(recipe->amountProduced));
            }
            else if constexpr (requires { recipe->createdCount; }) {
                return std::max(1, static_cast<std::int32_t>(recipe->createdCount));
            }
            else if constexpr (requires { recipe->resultCount; }) {
                return std::max(1, static_cast<std::int32_t>(recipe->resultCount));
            }
            else if constexpr (requires { recipe->data.createdItemCount; }) {
                return std::max(1, static_cast<std::int32_t>(recipe->data.createdItemCount));
            }
            else {
                return 1;
            }
        }

        template <class Recipe>
        RE::BGSKeyword* RecipeBenchKeyword(Recipe* recipe)
        {
            if (!recipe) {
                return nullptr;
            }

            if constexpr (requires { recipe->benchKeyword; }) {
                return recipe->benchKeyword;
            }
            else if constexpr (requires { recipe->workbenchKeyword; }) {
                return recipe->workbenchKeyword;
            }
            else if constexpr (requires { recipe->stationKeyword; }) {
                return recipe->stationKeyword;
            }
            else if constexpr (requires { recipe->data.benchKeyword; }) {
                return recipe->data.benchKeyword;
            }
            else {
                return nullptr;
            }
        }

        template <class Container, class Func>
        void ForEachTESContainerLike(Container& container, Func&& fn)
        {
            if constexpr (requires { container.numContainerObjects; container.containerObjects; }) {
                const auto count = static_cast<std::uint32_t>(container.numContainerObjects);
                auto** entries = container.containerObjects;
                if (!entries || count == 0) {
                    return;
                }

                for (std::uint32_t i = 0; i < count; ++i) {
                    auto* entry = entries[i];
                    if (!entry) {
                        continue;
                    }
                    fn(ComponentItem(*entry), ComponentCount(*entry));
                }
            }
            else if constexpr (requires { container.numEntries; container.entries; }) {
                const auto count = static_cast<std::uint32_t>(container.numEntries);
                auto** entries = container.entries;
                if (!entries || count == 0) {
                    return;
                }

                for (std::uint32_t i = 0; i < count; ++i) {
                    auto* entry = entries[i];
                    if (!entry) {
                        continue;
                    }
                    fn(ComponentItem(*entry), ComponentCount(*entry));
                }
            }
        }

        template <class Recipe, class Func>
        void ForEachRequiredItem(Recipe* recipe, Func&& fn)
        {
            if (!recipe) {
                return;
            }

            // CommonLibSSE-NG exposes BGSConstructibleObject::requiredItems as TESContainer on SE 1.5.97.
            // TESContainer is not a C++ range, so never range-for it directly.
            if constexpr (requires { recipe->requiredItems.numContainerObjects; recipe->requiredItems.containerObjects; }) {
                ForEachTESContainerLike(recipe->requiredItems, std::forward<Func>(fn));
            }
            else if constexpr (requires { recipe->data.requiredItems.numContainerObjects; recipe->data.requiredItems.containerObjects; }) {
                ForEachTESContainerLike(recipe->data.requiredItems, std::forward<Func>(fn));
            }
            else if constexpr (requires { recipe->components.size(); recipe->components[0]; }) {
                for (auto& component : recipe->components) {
                    fn(ComponentItem(component), ComponentCount(component));
                }
            }
            else if constexpr (requires { recipe->ingredients.size(); recipe->ingredients[0]; }) {
                for (auto& component : recipe->ingredients) {
                    fn(ComponentItem(component), ComponentCount(component));
                }
            }
        }

        template <class EffectItem>
        RE::TESForm* EffectBaseForm(EffectItem& effect)
        {
            auto* ptr = Ptr(effect);
            if (!ptr) {
                return nullptr;
            }

            if constexpr (requires { ptr->baseEffect; }) {
                return ptr->baseEffect;
            }
            else if constexpr (requires { ptr->effectSetting; }) {
                return ptr->effectSetting;
            }
            else if constexpr (requires { ptr->effect; }) {
                return ptr->effect;
            }
            else if constexpr (requires { ptr->mgef; }) {
                return ptr->mgef;
            }
            else {
                return nullptr;
            }
        }

        template <class Ingredient, class Func>
        void ForEachIngredientEffect(Ingredient* ingredient, Func&& fn)
        {
            if (!ingredient) {
                return;
            }

            if constexpr (requires { ingredient->effects; }) {
                for (auto& effect : ingredient->effects) {
                    if (auto* base = EffectBaseForm(effect)) {
                        fn(base);
                    }
                }
            }
            else if constexpr (requires { ingredient->effectList.effects; }) {
                for (auto& effect : ingredient->effectList.effects) {
                    if (auto* base = EffectBaseForm(effect)) {
                        fn(base);
                    }
                }
            }
            else if constexpr (requires { ingredient->listOfEffects; }) {
                for (auto& effect : ingredient->listOfEffects) {
                    if (auto* base = EffectBaseForm(effect)) {
                        fn(base);
                    }
                }
            }
        }

        bool BenchKeywordMatches(WorkStation station, RE::BGSKeyword* keyword)
        {
            const auto editor = EditorID(keyword);
            if (editor.empty()) {
                return false;
            }
            const auto lower = Lower(editor);

            switch (station) {
            case WorkStation::Forge:
                return lower.find("forge") != std::string::npos ||
                    lower.find("anvil") != std::string::npos ||
                    lower.find("blacksmithforge") != std::string::npos;
            case WorkStation::Smelter:
                return lower.find("smelt") != std::string::npos;
            case WorkStation::Tanning:
                return lower.find("tanning") != std::string::npos || lower.find("tan") != std::string::npos;
            case WorkStation::SharpeningWheel:
                return lower.find("sharpen") != std::string::npos || lower.find("grindstone") != std::string::npos;
            case WorkStation::Workbench:
                return (lower.find("armor") != std::string::npos && lower.find("bench") != std::string::npos) ||
                    lower.find("armorbench") != std::string::npos ||
                    lower.find("armortable") != std::string::npos ||
                    lower.find("armor table") != std::string::npos ||
                    lower.find("smithingarmortable") != std::string::npos;
            case WorkStation::Cooking:
                return lower.find("cook") != std::string::npos ||
                    lower.find("cooking") != std::string::npos ||
                    lower.find("cookpot") != std::string::npos ||
                    lower.find("campfire") != std::string::npos ||
                    lower.find("campfireuse") != std::string::npos;
            default:
                return false;
            }
        }

        bool IsRejectedCookingResult(RE::TESForm* result)
        {
            if (!result) {
                return true;
            }

            const auto editor = Lower(EditorID(result));
            if (editor.empty()) {
                return false;
            }
            return editor.find("raw") != std::string::npos || editor.find("uncooked") != std::string::npos;
        }

        bool LastCookingStationIsUsableCampfire()
        {
            auto* ref = TFD::Location::GetLastCaptiveWorkCraftingRefForState(static_cast<int>(WorkStation::Cooking));
            if (!ref) {
                ref = TFD::Location::GetLastCaptiveWorkCraftingRef();
            }
            if (!ref) {
                return false;
            }

            const auto refEditor = EditorID(ref);
            const auto baseEditor = ref->GetBaseObject() ? EditorID(ref->GetBaseObject()) : std::string{};
            const bool usableCampfire = IsUsableCampfireEditor(refEditor) || IsUsableCampfireEditor(baseEditor);
            if (usableCampfire) {
                spdlog::info("[TFD][WorkNative][R225A] cooking station detected as UsableCampfire ref={:08X} base={:08X} refEditor='{}' baseEditor='{}'",
                    ref->GetFormID(),
                    ref->GetBaseObject() ? ref->GetBaseObject()->GetFormID() : 0u,
                    refEditor,
                    baseEditor);
            }
            return usableCampfire;
        }

        bool IsUsableCampfireCookingRecipe(const WorkRecipe& recipe)
        {
            if (recipe.station != WorkStation::Cooking) {
                return false;
            }
            return ContainsInsensitive(recipe.recipeEditorID, "UC_CampfireRecipe") ||
                ContainsInsensitive(recipe.recipeEditorID, "UC_Campfire");
        }

        bool HasBaseEnchantment(RE::TESForm* form)
        {
            if (!form) {
                return false;
            }

            if (auto* weapon = form->As<RE::TESObjectWEAP>()) {
                if constexpr (requires { weapon->formEnchanting; }) {
                    if (weapon->formEnchanting) {
                        return true;
                    }
                }
            }

            if (auto* armor = form->As<RE::TESObjectARMO>()) {
                if constexpr (requires { armor->formEnchanting; }) {
                    if (armor->formEnchanting) {
                        return true;
                    }
                }
            }

            return false;
        }

        bool HasTemperingArtifactBlockToken(RE::TESForm* form)
        {
            if (!form) {
                return true;
            }

            const auto editor = Lower(EditorID(form));
            if (editor.empty()) {
                return false;
            }

            return HasSpecialWorkBlockToken(editor) ||
                editor.find("artifact") != std::string::npos ||
                editor.find("unique") != std::string::npos ||
                editor.find("daedricartifact") != std::string::npos;
        }

        bool HasBasicCaptiveTemperingAllowToken(const std::string& lowerText)
        {
            if (lowerText.empty()) {
                return false;
            }

            // First-pass captive tempering must stay boring and reliable. These
            // families are ordinary low-tier equipment that can be improved at
            // standard grindstones/workbenches without quest progression, hidden
            // stations, or artifact/unique-item rules.
            return lowerText.find("iron") != std::string::npos ||
                lowerText.find("steel") != std::string::npos ||
                lowerText.find("imperial") != std::string::npos ||
                lowerText.find("hide") != std::string::npos ||
                lowerText.find("leather") != std::string::npos ||
                lowerText.find("studded") != std::string::npos ||
                lowerText.find("fur") != std::string::npos;
        }

        bool HasTemperingFamilyBlockToken(const std::string& lowerText)
        {
            if (lowerText.empty()) {
                return true;
            }

            // Vanilla/DLC still contains many non-normal tempering families: draugr
            // loot, Skyforge/Nord Hero, faction/quest gear, high-tier perk families,
            // and unique-looking equipment. Keep them out of radiant captive work
            // until we have explicit condition/perk evaluation.
            return lowerText.find("draugr") != std::string::npos ||
                lowerText.find("ancient") != std::string::npos ||
                lowerText.find("honed") != std::string::npos ||
                lowerText.find("nordhero") != std::string::npos ||
                lowerText.find("nord hero") != std::string::npos ||
                lowerText.find("skyforge") != std::string::npos ||
                lowerText.find("nordic") != std::string::npos ||
                lowerText.find("falmer") != std::string::npos ||
                lowerText.find("forsworn") != std::string::npos ||
                lowerText.find("silver") != std::string::npos ||
                lowerText.find("blades") != std::string::npos ||
                lowerText.find("dwarven") != std::string::npos ||
                lowerText.find("dwemer") != std::string::npos ||
                lowerText.find("elven") != std::string::npos ||
                lowerText.find("orcish") != std::string::npos ||
                lowerText.find("scaled") != std::string::npos ||
                lowerText.find("plate") != std::string::npos ||
                lowerText.find("glass") != std::string::npos ||
                lowerText.find("ebony") != std::string::npos ||
                lowerText.find("daedric") != std::string::npos ||
                lowerText.find("dragon") != std::string::npos ||
                lowerText.find("stalhrim") != std::string::npos ||
                lowerText.find("bonemold") != std::string::npos ||
                lowerText.find("chitin") != std::string::npos ||
                lowerText.find("amber") != std::string::npos ||
                lowerText.find("madness") != std::string::npos;
        }

        bool IsAllowedBasicCaptiveTemperingResult(RE::TESForm* form)
        {
            if (!form || !IsCoreGameWorkForm(form)) {
                return false;
            }

            const auto lower = Lower(EditorID(form));
            if (lower.empty()) {
                return false;
            }

            if (HasSpecialWorkBlockToken(lower) || HasTemperingFamilyBlockToken(lower)) {
                return false;
            }

            return HasBasicCaptiveTemperingAllowToken(lower);
        }

        bool IsRejectedConstructibleResult(WorkStation station, RE::TESForm* result)
        {
            if (!result) {
                return true;
            }

            // R182: Forge captive work is an equipment-demand job, not a generic
            // COBJ picker. Reject Hearthfire/CC furniture/building parts such as
            // BYOHHouseInteriorPart017Anvil01 so objectives never become
            // "Work on Blacksmith Anvil" and then wait for a non-inventory result.
            if (station == WorkStation::Forge) {
                if (result->As<RE::TESObjectWEAP>() == nullptr && result->As<RE::TESObjectARMO>() == nullptr) {
                    return true;
                }
                return HasBaseEnchantment(result) ||
                    HasTemperingArtifactBlockToken(result) ||
                    !IsAllowedBasicCaptiveTemperingResult(result);
            }

            // Tempering recipes must point at an inventory item that can actually be
            // given to the player and improved at the selected station. Some mods/CC
            // add constructible objects that use smithing workbench keywords to create
            // furniture or placeable workshop objects; those can produce objectives like
            // "Improve Armorer Workbench" and leave the player with nothing valid to temper.
            if (station == WorkStation::SharpeningWheel) {
                return result->As<RE::TESObjectWEAP>() == nullptr ||
                    HasBaseEnchantment(result) ||
                    HasTemperingArtifactBlockToken(result) ||
                    !IsAllowedBasicCaptiveTemperingResult(result);
            }
            if (station == WorkStation::Workbench) {
                return result->As<RE::TESObjectARMO>() == nullptr ||
                    HasBaseEnchantment(result) ||
                    HasTemperingArtifactBlockToken(result) ||
                    !IsAllowedBasicCaptiveTemperingResult(result);
            }

            const auto editor = Lower(EditorID(result));
            if (station == WorkStation::Cooking) {
                return IsRejectedCookingResult(result);
            }
            if (station == WorkStation::Smelter || station == WorkStation::Tanning) {
                if (HasSpecialWorkBlockToken(editor)) {
                    return true;
                }
                if (editor.find("recipe") != std::string::npos || editor.find("dummy") != std::string::npos) {
                    return true;
                }
            }
            return false;
        }

        std::int32_t EstimateTierFromText(std::string text)
        {
            text = Lower(std::move(text));

            // Unique/quest-gated special equipment is not valid radiant captive work,
            // even if the COBJ technically has a forge keyword.
            if (HasSpecialWorkBlockToken(text)) return 100;

            // High-end vanilla and DLC/CC material families.
            if (text.find("dragon") != std::string::npos) return 100;
            if (text.find("daedric") != std::string::npos || text.find("daedra") != std::string::npos) return 90;
            if (text.find("madness") != std::string::npos) return 90;
            if (text.find("ebony") != std::string::npos) return 80;
            if (text.find("stalhrim") != std::string::npos) return 80;
            if (text.find("glass") != std::string::npos) return 70;
            if (text.find("malachite") != std::string::npos) return 70;
            if (text.find("amber") != std::string::npos) return 70;

            // Mid-tier smithing families.
            if (text.find("orcish") != std::string::npos) return 50;
            if (text.find("orichalc") != std::string::npos) return 50;
            if (text.find("scaled") != std::string::npos) return 50;
            if (text.find("plate") != std::string::npos) return 50;
            if (text.find("dwarven") != std::string::npos) return 30;
            if (text.find("dwemer") != std::string::npos) return 30;
            if (text.find("elven") != std::string::npos) return 30;
            if (text.find("moonstone") != std::string::npos) return 30;
            if (text.find("nordic") != std::string::npos) return 30;
            if (text.find("bonemold") != std::string::npos) return 30;
            if (text.find("chitin") != std::string::npos) return 30;

            // Early smithing families.
            if (text.find("steel") != std::string::npos) return 20;
            if (text.find("silver") != std::string::npos) return 20;
            if (text.find("imperial") != std::string::npos) return 20;

            // Any Creation Club constructible that we do not understand should not be treated as iron-tier.
            // Many CC recipes are condition/quest/perk-gated. If their material tier is unknown, keep them away
            // from very low level/skill players instead of assigning an uncraftable job.
            if (text.find("ccbgssse") != std::string::npos || text.find("creationclub") != std::string::npos) return 50;

            // Low/basic families.
            if (text.find("iron") != std::string::npos) return 0;
            if (text.find("hide") != std::string::npos) return 0;
            if (text.find("leather") != std::string::npos) return 0;
            if (text.find("fur") != std::string::npos) return 0;

            // Potion naming tiers.
            if (text.find("minor") != std::string::npos) return 0;
            if (text.find("plentiful") != std::string::npos) return 25;
            if (text.find("vigorous") != std::string::npos) return 50;
            if (text.find("extreme") != std::string::npos) return 75;
            if (text.find("ultimate") != std::string::npos) return 100;

            return 0;
        }

        std::int32_t MinLevelForTier(std::int32_t tier)
        {
            if (tier <= 0) return 1;
            if (tier <= 20) return 5;
            if (tier <= 30) return 10;
            if (tier <= 50) return 20;
            if (tier <= 70) return 30;
            if (tier <= 80) return 35;
            if (tier <= 90) return 40;
            return 45;
        }

        bool SkillLevelAllowed(std::int32_t tier, std::int32_t playerLevel, std::int32_t skill)
        {
            if (tier <= 0) {
                return true;
            }

            // Skill is the primary gate. Player level alone must not unlock advanced recipes,
            // otherwise low-smithing players can receive uncraftable forge jobs.
            if (skill < tier) {
                return false;
            }

            // Player level is only a secondary sanity gate so very high-skill characters still scale upward,
            // but absurdly early high-tier recipes stay filtered.
            const auto minLevel = MinLevelForTier(tier);
            if (playerLevel + 5 < minLevel) {
                return false;
            }

            return true;
        }

        std::int32_t RelevantSkill(WorkStation station, std::int32_t smithing, std::int32_t alchemy, std::int32_t enchanting)
        {
            switch (station) {
            case WorkStation::Forge:
            case WorkStation::Smelter:
            case WorkStation::Tanning:
            case WorkStation::SharpeningWheel:
            case WorkStation::Workbench:
                return smithing;
            case WorkStation::Alchemy:
                return alchemy;
            case WorkStation::Enchanting:
                return enchanting;
            default:
                return 0;
            }
        }

        std::uint32_t RandomIndex(std::uint32_t count)
        {
            if (count <= 1) {
                return 0;
            }

            static std::mt19937 rng{
                static_cast<std::mt19937::result_type>(
                    std::chrono::steady_clock::now().time_since_epoch().count())
            };
            std::uniform_int_distribution<std::uint32_t> dist(0, count - 1);
            return dist(rng);
        }

        RE::TESForm* LookupSkyrimForm(RE::FormID localFormID)
        {
            const auto compactID = localFormID & 0x00FFFFFFu;

            if (auto* data = RE::TESDataHandler::GetSingleton()) {
                if (auto* form = data->LookupForm<RE::TESForm>(compactID, "Skyrim.esm")) {
                    return form;
                }
            }

            // R178: Some SE 1.5.97/CommonLib builds do not resolve TESForm through
            // TESDataHandler::LookupForm<TESForm>() reliably for generic Skyrim.esm
            // misc items. Fall back to the live form table because Skyrim.esm keeps
            // load order 00 in normal SE runtime. This is required for OreIron and
            // Firewood, otherwise native rejects Mining/Chopping while CK dialogue
            // correctly sees the resource globals.
            return RE::TESForm::LookupByID<RE::TESForm>(compactID);
        }

        void ResolveGlobal(RE::TESGlobal*& global, const char* editorID)
        {
            if (!global && editorID && editorID[0]) {
                global = RE::TESForm::LookupByEditorID<RE::TESGlobal>(editorID);
            }
        }

        std::int32_t ReadGlobalInt(RE::TESGlobal* global)
        {
            if (!global) {
                return 0;
            }
            return static_cast<std::int32_t>(global->value);
        }

        void WriteGlobalInt(RE::TESGlobal* global, std::int32_t value)
        {
            if (global) {
                global->value = static_cast<float>(value);
            }
        }

        void ResolveWorkResourceGlobals()
        {
            ResolveGlobal(g_miningStateGlobal, "TFDMiningState");
            ResolveGlobal(g_craftingStateGlobal, "TFDCraftingState");
            ResolveGlobal(g_workJobTypeGlobal, "TFDWorkJobType");
            ResolveGlobal(g_workAssignmentStateGlobal, "TFDWorkAssignmentState");
            ResolveGlobal(g_forgeStateGlobal, "TFDForgeState");
            ResolveGlobal(g_smelterStateGlobal, "TFDSmelterState");
            ResolveGlobal(g_tanningStateGlobal, "TFDTanningState");
            ResolveGlobal(g_sharpeningStateGlobal, "TFDSharpeningState");
            ResolveGlobal(g_workbenchStateGlobal, "TFDWorkbenchState");
            ResolveGlobal(g_choppingStateGlobal, "TFDChoppingState");
            ResolveGlobal(g_cookingStateGlobal, "TFDCookingState");
            ResolveGlobal(g_alchemyStateGlobal, "TFDAlchemyState");
            ResolveGlobal(g_enchantingStateGlobal, "TFDEnchantingState");
        }

        std::int32_t CurrentMiningState()
        {
            ResolveWorkResourceGlobals();
            return ReadGlobalInt(g_miningStateGlobal);
        }

        RE::TESFaction* ResolveDesireFaction()
        {
            if (!g_desireFaction) {
                g_desireFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDDesireFaction");
            }
            return g_desireFaction;
        }

        bool ActorHasNativeWorkDesire(RE::Actor* actor)
        {
            auto* faction = ResolveDesireFaction();
            return actor && faction && actor->IsInFaction(faction);
        }

        std::int32_t LegacyWorkGlobalJobType(WorkJob job)
        {
            // ESP/objective contract:
            // 0 = None/No Job, 1 = Mining, 2 = shared Crafting family, 3 = Pleasure.
            // Native internal jobs Chopping/Cooking/Alchemy/Enchanting/Improve stay raw internally,
            // but must publish as global job 2 plus TFDCraftingState subtype.
            switch (job) {
            case WorkJob::Mining:
                return 1;
            case WorkJob::Pleasure:
                return 3;
            case WorkJob::Crafting:
            case WorkJob::Chopping:
            case WorkJob::Cooking:
            case WorkJob::Alchemy:
            case WorkJob::Enchanting:
            case WorkJob::Improve:
                return 2;
            case WorkJob::None:
            case WorkJob::NoJob:
            default:
                return 0;
            }
        }

        void PublishWorkGlobals(const SelectedWorkJob& selected, const char* reason)
        {
            ResolveWorkResourceGlobals();
            const auto rawJobType = selected.job == WorkJob::NoJob ? 0 : static_cast<std::int32_t>(selected.job);
            const auto globalJobType = LegacyWorkGlobalJobType(selected.job);
            const auto assignment = 1; // R181: No Job is an offer line; keep Talk state so CK can display/click it.
            const auto publishedMiningState = selected.job == WorkJob::Mining ? selected.miningState : 0;
            const auto publishedCraftingState = selected.station != WorkStation::None ? static_cast<std::int32_t>(selected.station) : 0;

            WriteGlobalInt(g_workJobTypeGlobal, globalJobType);
            WriteGlobalInt(g_workAssignmentStateGlobal, assignment);
            // R176K: these globals are now an offer mirror, not only raw ambient availability.
            // This prevents CK job lines from leaking when the prepared offer is a different job.
            WriteGlobalInt(g_miningStateGlobal, publishedMiningState);
            WriteGlobalInt(g_craftingStateGlobal, publishedCraftingState);

            spdlog::info("[TFD][WorkNative][R176K] publish globals rawJob={} globalJob={} assignment={} mining={} crafting={} reason={}",
                rawJobType,
                globalJobType,
                assignment,
                publishedMiningState,
                publishedCraftingState,
                reason ? reason : "unknown");
        }

        bool StationAvailable(WorkStation station)
        {
            ResolveWorkResourceGlobals();
            switch (station) {
            case WorkStation::Forge:
                return ReadGlobalInt(g_forgeStateGlobal) > 0;
            case WorkStation::Smelter:
                return ReadGlobalInt(g_smelterStateGlobal) > 0;
            case WorkStation::Tanning:
                return ReadGlobalInt(g_tanningStateGlobal) > 0;
            case WorkStation::SharpeningWheel:
                return ReadGlobalInt(g_sharpeningStateGlobal) > 0;
            case WorkStation::Workbench:
                return ReadGlobalInt(g_workbenchStateGlobal) > 0;
            case WorkStation::ChoppingBlock:
                return ReadGlobalInt(g_choppingStateGlobal) > 0;
            case WorkStation::Cooking:
                return ReadGlobalInt(g_cookingStateGlobal) > 0;
            case WorkStation::Alchemy:
                return ReadGlobalInt(g_alchemyStateGlobal) > 0;
            case WorkStation::Enchanting:
                return ReadGlobalInt(g_enchantingStateGlobal) > 0;
            default:
                return false;
            }
        }

        RE::TESBoundObject* BoundObject(RE::TESForm* form)
        {
            return form ? form->As<RE::TESBoundObject>() : nullptr;
        }

        std::int32_t ReferenceItemCount(RE::TESObjectREFR* ref, RE::TESForm* form)
        {
            auto* item = BoundObject(form);
            if (!ref || !item) {
                return 0;
            }
            const auto inv = ref->GetInventory([item](RE::TESBoundObject& obj) { return &obj == item; }, true);
            auto it = inv.find(item);
            if (it == inv.end()) {
                return 0;
            }
            return std::max(0, it->second.first);
        }

        bool ActorHasItem(RE::Actor* actor, RE::TESForm* form, std::int32_t count = 1)
        {
            if (!actor || !form) {
                return false;
            }
            return ReferenceItemCount(actor, form) >= std::max(1, count);
        }

        constexpr std::uint32_t kArmorNeedHead = 1u << 0;
        constexpr std::uint32_t kArmorNeedBody = 1u << 1;
        constexpr std::uint32_t kArmorNeedHands = 1u << 2;
        constexpr std::uint32_t kArmorNeedFeet = 1u << 3;
        constexpr std::uint32_t kArmorNeedShield = 1u << 4;
        constexpr std::uint32_t kArmorNeedOther = 1u << 5;

        template <class T>
        std::uint32_t ToUInt32(T value)
        {
            if constexpr (requires(T v) { v.underlying(); }) {
                return static_cast<std::uint32_t>(value.underlying());
            }
            else {
                return static_cast<std::uint32_t>(value);
            }
        }

        std::uint32_t SlotValue(RE::BIPED_MODEL::BipedObjectSlot slot)
        {
            return ToUInt32(slot);
        }

        bool HasSlot(std::uint32_t slotMask, RE::BIPED_MODEL::BipedObjectSlot slot)
        {
            return (slotMask & SlotValue(slot)) != 0;
        }

        std::uint32_t ArmorSlotMask(RE::TESObjectARMO* armor)
        {
            if (!armor) {
                return 0;
            }

            std::uint32_t slotMask = 0;
            if constexpr (requires(RE::TESObjectARMO* a) { a->GetSlotMask(); }) {
                slotMask = ToUInt32(armor->GetSlotMask());
            }
            else if constexpr (requires(RE::TESObjectARMO* a) { a->bipedModelData.bipedObjectSlots; }) {
                slotMask = ToUInt32(armor->bipedModelData.bipedObjectSlots);
            }

            if constexpr (requires(RE::TESObjectARMO* a) { a->IsShield(); }) {
                if (armor->IsShield()) {
                    slotMask |= SlotValue(RE::BIPED_MODEL::BipedObjectSlot::kShield);
                }
            }

            return slotMask;
        }

        std::uint32_t ArmorNeedMaskFromText(const std::string& lower)
        {
            std::uint32_t mask = 0;
            if (lower.empty()) {
                return mask;
            }

            if (lower.find("shield") != std::string::npos) {
                mask |= kArmorNeedShield;
            }
            if (lower.find("boots") != std::string::npos || lower.find("shoe") != std::string::npos || lower.find("feet") != std::string::npos) {
                mask |= kArmorNeedFeet;
            }
            if (lower.find("gauntlet") != std::string::npos || lower.find("glove") != std::string::npos || lower.find("hands") != std::string::npos) {
                mask |= kArmorNeedHands;
            }
            if (lower.find("helmet") != std::string::npos || lower.find("helm") != std::string::npos || lower.find("hood") != std::string::npos || lower.find("head") != std::string::npos) {
                mask |= kArmorNeedHead;
            }
            if (lower.find("cuirass") != std::string::npos || lower.find("armor") != std::string::npos || lower.find("armour") != std::string::npos || lower.find("body") != std::string::npos) {
                if ((mask & (kArmorNeedShield | kArmorNeedFeet | kArmorNeedHands | kArmorNeedHead)) == 0) {
                    mask |= kArmorNeedBody;
                }
            }

            return mask;
        }

        std::uint32_t ArmorNeedMask(RE::TESObjectARMO* armor)
        {
            if (!armor) {
                return 0;
            }

            const auto slotMask = ArmorSlotMask(armor);
            std::uint32_t demandMask = 0;
            if (HasSlot(slotMask, RE::BIPED_MODEL::BipedObjectSlot::kShield)) {
                demandMask |= kArmorNeedShield;
            }
            if (HasSlot(slotMask, RE::BIPED_MODEL::BipedObjectSlot::kFeet) || HasSlot(slotMask, RE::BIPED_MODEL::BipedObjectSlot::kCalves)) {
                demandMask |= kArmorNeedFeet;
            }
            if (HasSlot(slotMask, RE::BIPED_MODEL::BipedObjectSlot::kHands) || HasSlot(slotMask, RE::BIPED_MODEL::BipedObjectSlot::kForearms)) {
                demandMask |= kArmorNeedHands;
            }
            if (HasSlot(slotMask, RE::BIPED_MODEL::BipedObjectSlot::kHead) || HasSlot(slotMask, RE::BIPED_MODEL::BipedObjectSlot::kHair) ||
                HasSlot(slotMask, RE::BIPED_MODEL::BipedObjectSlot::kLongHair) || HasSlot(slotMask, RE::BIPED_MODEL::BipedObjectSlot::kCirclet)) {
                demandMask |= kArmorNeedHead;
            }
            if (HasSlot(slotMask, RE::BIPED_MODEL::BipedObjectSlot::kBody)) {
                demandMask |= kArmorNeedBody;
            }

            if (demandMask == 0) {
                demandMask = ArmorNeedMaskFromText(Lower(EditorID(armor)));
            }
            if (demandMask == 0) {
                demandMask = kArmorNeedOther;
            }
            return demandMask;
        }

        struct ActorDemandProfile
        {
            bool hasWeapon{ false };
            std::uint32_t armorNeedMask{ 0 };
            bool hasOre{ false };
            bool hasFirewood{ false };
            bool hasFood{ false };
            bool hasPotion{ false };
            bool hasPoison{ false };
            bool hasIngot{ false };
            bool hasLeather{ false };
            bool hasLeatherStrips{ false };
        };

        void AddWornArmorToProfile(RE::Actor* actor, ActorDemandProfile& profile, RE::BIPED_MODEL::BipedObjectSlot slot)
        {
            if (!actor) {
                return;
            }
            if (auto* worn = actor->GetWornArmor(slot, false); worn) {
                profile.armorNeedMask |= ArmorNeedMask(worn);
            }
        }

        RE::TESForm* OreForMiningState(std::int32_t miningState);
        RE::TESForm* FirewoodForm();

        bool FormIsAnyOre(RE::TESForm* form)
        {
            if (!form) {
                return false;
            }
            for (std::int32_t state = 1; state <= 9; ++state) {
                if (form == OreForMiningState(state)) {
                    return true;
                }
            }
            return false;
        }

        ActorDemandProfile BuildActorDemandProfile(RE::Actor* actor)
        {
            ActorDemandProfile profile{};
            if (!actor) {
                return profile;
            }

            const auto inv = actor->GetInventory([](RE::TESBoundObject& obj) {
                (void)obj;
                return true;
                }, true);

            for (const auto& [item, invData] : inv) {
                const auto& [count, entry] = invData;
                (void)entry;
                if (!item || count <= 0) {
                    continue;
                }

                if (item->As<RE::TESObjectWEAP>()) {
                    profile.hasWeapon = true;
                }
                if (auto* armor = item->As<RE::TESObjectARMO>()) {
                    profile.armorNeedMask |= ArmorNeedMask(armor);
                }
                if (item == FirewoodForm()) {
                    profile.hasFirewood = true;
                }
                if (FormIsAnyOre(item)) {
                    profile.hasOre = true;
                }
                if (item->As<RE::TESObjectMISC>()) {
                    const auto lowerEditor = Lower(EditorID(item));
                    if (lowerEditor.find("ingot") != std::string::npos) {
                        profile.hasIngot = true;
                    }
                    if (lowerEditor.find("leatherstrips") != std::string::npos || lowerEditor.find("leatherstrip") != std::string::npos) {
                        profile.hasLeatherStrips = true;
                    }
                    else if (lowerEditor == "leather" || lowerEditor.find("leather01") != std::string::npos || lowerEditor.rfind("leather", 0) == 0) {
                        profile.hasLeather = true;
                    }
                }
                if (auto* alchemyItem = item->As<RE::AlchemyItem>()) {
                    if (alchemyItem->IsFood()) {
                        profile.hasFood = true;
                    }
                    else if (alchemyItem->IsPoison()) {
                        profile.hasPoison = true;
                    }
                    else {
                        profile.hasPotion = true;
                    }
                }
            }

            if (auto* right = actor->GetEquippedObject(false); right && right->As<RE::TESObjectWEAP>()) {
                profile.hasWeapon = true;
            }
            if (auto* left = actor->GetEquippedObject(true); left && left->As<RE::TESObjectWEAP>()) {
                profile.hasWeapon = true;
            }

            AddWornArmorToProfile(actor, profile, RE::BIPED_MODEL::BipedObjectSlot::kHead);
            AddWornArmorToProfile(actor, profile, RE::BIPED_MODEL::BipedObjectSlot::kBody);
            AddWornArmorToProfile(actor, profile, RE::BIPED_MODEL::BipedObjectSlot::kHands);
            AddWornArmorToProfile(actor, profile, RE::BIPED_MODEL::BipedObjectSlot::kFeet);
            AddWornArmorToProfile(actor, profile, RE::BIPED_MODEL::BipedObjectSlot::kShield);

            return profile;
        }

        constexpr std::uint32_t kRequiredBasicArmorMask = kArmorNeedHead | kArmorNeedBody | kArmorNeedHands | kArmorNeedFeet | kArmorNeedShield;

        bool ActorHasMissingBasicGearDemand(const ActorDemandProfile& profile)
        {
            return !profile.hasWeapon || ((profile.armorNeedMask & kRequiredBasicArmorMask) != kRequiredBasicArmorMask);
        }

        bool ActorHasTemperingDemand(const ActorDemandProfile& profile)
        {
            return profile.hasWeapon || ((profile.armorNeedMask & kRequiredBasicArmorMask) != 0);
        }

        enum class WorkDemandFactionKind : std::uint32_t
        {
            Mining = 0,
            Chopping,
            Crafting,
            Tempering,
            Cooking,
            Alchemy,
            Enchanting,
            Smelting,
            Tanning,
            NoJob,
            Count
        };

        constexpr std::array<const char*, static_cast<std::size_t>(WorkDemandFactionKind::Count)> kWorkDemandFactionEditorIDs{
            "TFDMiningFaction",
            "TFDChoppingFaction",
            "TFDCraftingFaction",
            "TFDTemperingFaction",
            "TFDCookingFaction",
            "TFDAlchemyFaction",
            "TFDEnchantingFaction",
            "TFDSmeltingFaction",
            "TFDTanningFaction",
            "TFDNoJobFaction"
        };

        std::array<RE::TESFaction*, static_cast<std::size_t>(WorkDemandFactionKind::Count)> g_workDemandFactions{};

        struct WorkDemandFactionEntry
        {
            RE::ObjectRefHandle actor{};
            RE::FormID actorFormID{ 0 };
            RE::FormID factionFormID{ 0 };
            bool addedByTFD{ false };
        };

        std::vector<WorkDemandFactionEntry> g_workDemandFactionEntries{};

        RE::TESFaction* ResolveWorkDemandFaction(WorkDemandFactionKind kind)
        {
            const auto index = static_cast<std::size_t>(kind);
            if (index >= g_workDemandFactions.size()) {
                return nullptr;
            }
            if (!g_workDemandFactions[index]) {
                const char* editorID = kWorkDemandFactionEditorIDs[index];
                g_workDemandFactions[index] = RE::TESForm::LookupByEditorID<RE::TESFaction>(editorID);
                if (!g_workDemandFactions[index]) {
                    spdlog::warn("[TFD][WorkNative][DemandFaction] missing faction editorId={}", editorID ? editorID : "<null>");
                }
            }
            return g_workDemandFactions[index];
        }

        RE::Actor* ResolveTrackedDemandActor(const WorkDemandFactionEntry& entry)
        {
            RE::Actor* actor = nullptr;
            if (entry.actor) {
                auto sp = RE::Actor::LookupByHandle(entry.actor.native_handle());
                actor = sp ? sp.get() : nullptr;
            }
            if (!actor && entry.actorFormID != 0) {
                actor = RE::TESForm::LookupByID<RE::Actor>(entry.actorFormID);
            }
            return actor;
        }

        bool IsDemandFactionTracked(RE::FormID actorFormID, RE::FormID factionFormID)
        {
            if (actorFormID == 0 || factionFormID == 0) {
                return false;
            }
            for (const auto& entry : g_workDemandFactionEntries) {
                if (entry.actorFormID == actorFormID && entry.factionFormID == factionFormID) {
                    return true;
                }
            }
            return false;
        }

        void RemoveDemandFactionFromActor(RE::Actor* actor, RE::TESFaction* faction, const char* reason, bool removeEvenIfUntracked = true)
        {
            if (!actor || !faction) {
                return;
            }

            const auto actorID = actor->GetFormID();
            const auto factionID = faction->GetFormID();
            bool trackedAdded = false;
            bool tracked = false;

            for (const auto& entry : g_workDemandFactionEntries) {
                if (entry.actorFormID == actorID && entry.factionFormID == factionID) {
                    tracked = true;
                    trackedAdded = entry.addedByTFD;
                    break;
                }
            }

            const bool hadFaction = actor->IsInFaction(faction);
            if (hadFaction && (removeEvenIfUntracked || trackedAdded)) {
                actor->RemoveFromFaction(faction);
            }

            const auto oldSize = g_workDemandFactionEntries.size();
            g_workDemandFactionEntries.erase(
                std::remove_if(
                    g_workDemandFactionEntries.begin(),
                    g_workDemandFactionEntries.end(),
                    [actorID, factionID](const WorkDemandFactionEntry& entry) {
                        return entry.actorFormID == actorID && entry.factionFormID == factionID;
                    }),
                g_workDemandFactionEntries.end());

            if (hadFaction || tracked || oldSize != g_workDemandFactionEntries.size()) {
                spdlog::info("[TFD][WorkNative][DemandFaction] removed actor={:08X} faction={:08X} had={} tracked={} trackedAdded={} reason={}",
                    actorID,
                    factionID,
                    hadFaction ? 1 : 0,
                    tracked ? 1 : 0,
                    trackedAdded ? 1 : 0,
                    reason ? reason : "unknown");
            }
        }

        void ClearDemandFactionsForActor(RE::Actor* actor, const char* reason)
        {
            if (!actor) {
                return;
            }
            for (std::size_t i = 0; i < kWorkDemandFactionEditorIDs.size(); ++i) {
                RemoveDemandFactionFromActor(actor, ResolveWorkDemandFaction(static_cast<WorkDemandFactionKind>(i)), reason, true);
            }
        }

        void ApplyDemandFactionToActor(RE::Actor* actor, WorkDemandFactionKind kind, const char* reason)
        {
            if (!actor || actor->IsDead() || actor->IsDisabled()) {
                return;
            }
            auto* faction = ResolveWorkDemandFaction(kind);
            if (!faction) {
                return;
            }

            const auto actorID = actor->GetFormID();
            const auto factionID = faction->GetFormID();
            const bool hadFaction = actor->IsInFaction(faction);
            if (!hadFaction) {
                actor->AddToFaction(faction, 0);
            }

            if (!IsDemandFactionTracked(actorID, factionID)) {
                WorkDemandFactionEntry entry{};
                entry.actor = actor->GetHandle();
                entry.actorFormID = actorID;
                entry.factionFormID = factionID;
                entry.addedByTFD = !hadFaction;
                g_workDemandFactionEntries.push_back(entry);
            }

            spdlog::info("[TFD][WorkNative][DemandFaction] applied actor={:08X} faction={} added={} had={} reason={}",
                actorID,
                kWorkDemandFactionEditorIDs[static_cast<std::size_t>(kind)],
                hadFaction ? 0 : 1,
                hadFaction ? 1 : 0,
                reason ? reason : "unknown");
        }

        void ClearWorkDemandFactionsInternal(const char* reason)
        {
            auto entries = g_workDemandFactionEntries;
            for (const auto& entry : entries) {
                auto* actor = ResolveTrackedDemandActor(entry);
                auto* faction = entry.factionFormID != 0 ? RE::TESForm::LookupByID<RE::TESFaction>(entry.factionFormID) : nullptr;
                if (actor && faction) {
                    RemoveDemandFactionFromActor(actor, faction, reason, false);
                }
            }
            g_workDemandFactionEntries.clear();
            spdlog::info("[TFD][WorkNative][DemandFaction] cleared all tracked reason={}", reason ? reason : "unknown");
        }

        std::uint32_t WorkJobMaskValue(WorkJob job)
        {
            const auto index = static_cast<std::uint32_t>(job);
            if (index == 0 || index >= 31) {
                return 0;
            }
            return 1u << index;
        }

        bool IsImproveUsedForActorRuntime(RE::FormID actorID)
        {
            if (actorID == 0) {
                return false;
            }
            const auto it = g_actorWorkMemory.find(actorID);
            return it != g_actorWorkMemory.end() && it->second.improveUsedThisRuntime;
        }

        bool IsImproveResultHandledForActorRuntime(RE::FormID actorID, RE::FormID resultID)
        {
            if (actorID == 0 || resultID == 0) {
                return false;
            }
            const auto it = g_actorWorkMemory.find(actorID);
            if (it == g_actorWorkMemory.end()) {
                return false;
            }
            const auto& handled = it->second.improvedResultFormIDs;
            return std::find(handled.begin(), handled.end(), resultID) != handled.end();
        }

        void MarkImproveResultHandledForActorRuntime(RE::FormID actorID, RE::FormID resultID, const char* reason)
        {
            if (actorID == 0 || resultID == 0) {
                return;
            }
            auto& memory = g_actorWorkMemory[actorID];
            if (std::find(memory.improvedResultFormIDs.begin(), memory.improvedResultFormIDs.end(), resultID) == memory.improvedResultFormIDs.end()) {
                memory.improvedResultFormIDs.push_back(resultID);
                spdlog::info("[TFD][WorkNative][R189] improve result handled actor={:08X} result={:08X} handledCount={} reason={}",
                    actorID,
                    resultID,
                    memory.improvedResultFormIDs.size(),
                    reason ? reason : "unknown");
            }
        }

        bool IsJobUsedThisWorkSession(RE::FormID actorID, WorkJob job)
        {
            if (actorID == 0) {
                return false;
            }
            const auto mask = WorkJobMaskValue(job);
            if (mask == 0) {
                return false;
            }
            const auto it = g_sessionPlans.find(actorID);
            if (it == g_sessionPlans.end()) {
                return false;
            }
            return (it->second.usedMask & mask) != 0;
        }

        void PublishNoJobDemandForActor(RE::Actor* actor, const char* reason)
        {
            if (!actor || actor->IsDead() || actor->IsDisabled()) {
                return;
            }
            ClearDemandFactionsForActor(actor, reason ? reason : "no_job_demand_clear_actor");
            ApplyDemandFactionToActor(actor, WorkDemandFactionKind::NoJob, reason ? reason : "no_job_demand");
            spdlog::info("[TFD][WorkNative][R181] no-job demand published actor={:08X} reason={}",
                actor->GetFormID(),
                reason ? reason : "unknown");
        }

        void RefreshWorkDemandFactionsForBossInternal(RE::Actor* actor, const char* reason)
        {
            if (!actor || actor->IsDead() || actor->IsDisabled()) {
                return;
            }

            // R184: this function is no longer allowed to publish raw inventory demand.
            // CK dialogue conditions must only see job factions after native has built
            // exact playable offers.  The exact publisher runs from BuildSessionPlan /
            // SelectSession / RequestJob, where player skills and station/resource state
            // are available.  Public callers such as TFDCaptive may still use this as a
            // stale-faction clear before Papyrus prepares the actual offer list.
            ClearDemandFactionsForActor(actor, reason ? reason : "refresh_exact_demand_clear_actor");

            const auto profile = BuildActorDemandProfile(actor);
            spdlog::info("[TFD][WorkNative][R184] raw demand refresh deferred actor={:08X} inv[weapon={} armorMask={:08X} ore={} ingot={} firewood={} leather={} strips={} food={} potion={} poison={} desire={}] reason={}",
                actor->GetFormID(),
                profile.hasWeapon ? 1 : 0,
                profile.armorNeedMask,
                profile.hasOre ? 1 : 0,
                profile.hasIngot ? 1 : 0,
                profile.hasFirewood ? 1 : 0,
                profile.hasLeather ? 1 : 0,
                profile.hasLeatherStrips ? 1 : 0,
                profile.hasFood ? 1 : 0,
                profile.hasPotion ? 1 : 0,
                profile.hasPoison ? 1 : 0,
                ActorHasNativeWorkDesire(actor) ? 1 : 0,
                reason ? reason : "unknown");
        }

        bool ActorHasExactItemOrEquipped(RE::Actor* actor, RE::TESForm* form)
        {
            if (!actor || !form) {
                return false;
            }
            if (ActorHasItem(actor, form, 1)) {
                return true;
            }
            if (auto* right = actor->GetEquippedObject(false); right == form) {
                return true;
            }
            if (auto* left = actor->GetEquippedObject(true); left == form) {
                return true;
            }
            if (auto* armor = form->As<RE::TESObjectARMO>()) {
                const auto needMask = ArmorNeedMask(armor);
                const auto slotMask = ArmorSlotMask(armor);
                const auto check = [&](RE::BIPED_MODEL::BipedObjectSlot slot) {
                    if (auto* worn = actor->GetWornArmor(slot, false); worn && worn == armor) {
                        return true;
                    }
                    return false;
                };
                if ((needMask & kArmorNeedHead) != 0 && check(RE::BIPED_MODEL::BipedObjectSlot::kHead)) return true;
                if ((needMask & kArmorNeedBody) != 0 && check(RE::BIPED_MODEL::BipedObjectSlot::kBody)) return true;
                if ((needMask & kArmorNeedHands) != 0 && check(RE::BIPED_MODEL::BipedObjectSlot::kHands)) return true;
                if ((needMask & kArmorNeedFeet) != 0 && check(RE::BIPED_MODEL::BipedObjectSlot::kFeet)) return true;
                if ((needMask & kArmorNeedShield) != 0 && check(RE::BIPED_MODEL::BipedObjectSlot::kShield)) return true;
                (void)slotMask;
            }
            return false;
        }

        bool ActorHasAnyWeapon(RE::Actor* actor)
        {
            return BuildActorDemandProfile(actor).hasWeapon;
        }

        bool ActorHasAnyArmor(RE::Actor* actor)
        {
            return BuildActorDemandProfile(actor).armorNeedMask != 0;
        }

        RE::TESForm* OreForMiningState(std::int32_t miningState)
        {
            switch (miningState) {
            case 1:
                return LookupSkyrimForm(0x00071CF3); // OreIron
            case 2:
                return LookupSkyrimForm(0x0005ACDB); // OreCorundum
            case 3:
                return LookupSkyrimForm(0x0005ACDF); // OreSilver
            case 4:
                return LookupSkyrimForm(0x0005ACDE); // OreGold
            case 5:
                return LookupSkyrimForm(0x0005ACE2); // OreQuicksilver
            case 6:
                return LookupSkyrimForm(0x0005ACE0); // OreMoonstone
            case 7:
                return LookupSkyrimForm(0x0005ACE1); // OreMalachite
            case 8:
                return LookupSkyrimForm(0x0005ACDD); // OreOrichalcum
            case 9:
                return LookupSkyrimForm(0x0005ACDC); // OreEbony
            default:
                return nullptr;
            }
        }

        RE::TESForm* FirewoodForm()
        {
            return LookupSkyrimForm(0x0006F993);
        }

        RE::Actor* LookupActor(RE::FormID formID)
        {
            return formID != 0 ? RE::TESForm::LookupByID<RE::Actor>(formID) : nullptr;
        }

        bool IsUsableWorkBossActor(RE::Actor* actor)
        {
            return actor && !actor->IsDead() && !actor->IsDisabled();
        }

        std::vector<RE::Actor*> CollectWorkSessionBossActors(RE::Actor* preferredActor)
        {
            std::vector<RE::Actor*> result{};
            std::vector<RE::FormID> seen{};

            auto pushActor = [&](RE::Actor* actor) {
                if (!IsUsableWorkBossActor(actor)) {
                    return;
                }
                const auto id = actor->GetFormID();
                if (id == 0 || std::find(seen.begin(), seen.end(), id) != seen.end()) {
                    return;
                }
                seen.push_back(id);
                result.push_back(actor);
            };

            pushActor(preferredActor);
            (void)TFD::Location::ResolveNearestCaptiveStorageTarget(preferredActor);

            TFD::Location::CaptiveStorageDebugSnapshot snapshot{};
            if (TFD::Location::GetLastCaptiveStorageDebugSnapshot(snapshot)) {
                for (const auto id : snapshot.bossActorFormIDs) {
                    pushActor(LookupActor(id));
                }
            }

            return result;
        }

        bool ActorAlreadySatisfiedForRecipe(RE::Actor* actor, const WorkRecipe& recipe)
        {
            if (!actor || !recipe.result) {
                return false;
            }

            const auto profile = BuildActorDemandProfile(actor);
            if (recipe.result->As<RE::TESObjectWEAP>()) {
                return profile.hasWeapon;
            }
            if (auto* armor = recipe.result->As<RE::TESObjectARMO>()) {
                const auto needMask = ArmorNeedMask(armor);
                return needMask != 0 && (profile.armorNeedMask & needMask) != 0;
            }
            if (auto* alchemyItem = recipe.result->As<RE::AlchemyItem>()) {
                if (alchemyItem->IsFood()) {
                    return profile.hasFood;
                }
                if (alchemyItem->IsPoison()) {
                    return profile.hasPoison;
                }
                return profile.hasPotion;
            }
            return ActorHasItem(actor, recipe.result, std::max(1, recipe.resultCount));
        }

        bool ActorHasImproveDemand(RE::Actor* actor, WorkStation station)
        {
            if (!actor) {
                return false;
            }
            const auto profile = BuildActorDemandProfile(actor);
            if (station == WorkStation::SharpeningWheel) {
                return profile.hasWeapon;
            }
            if (station == WorkStation::Workbench) {
                return profile.armorNeedMask != 0;
            }
            return false;
        }

        bool ActorHasAnyGearDemand(RE::Actor* actor)
        {
            const auto profile = BuildActorDemandProfile(actor);
            return profile.hasWeapon || profile.armorNeedMask != 0;
        }

        bool ActorCanImproveRecipeResult(RE::Actor* actor, const WorkRecipe& recipe)
        {
            if (!actor || !recipe.result) {
                return false;
            }
            return ActorHasExactItemOrEquipped(actor, recipe.result);
        }

        bool ForgeRecipeFillsMissingBasicGearDemand(RE::Actor* actor, const WorkRecipe& recipe)
        {
            if (!actor || !recipe.result) {
                return false;
            }

            const auto profile = BuildActorDemandProfile(actor);
            if (recipe.result->As<RE::TESObjectWEAP>()) {
                return !profile.hasWeapon;
            }

            if (auto* armor = recipe.result->As<RE::TESObjectARMO>()) {
                const auto needMask = ArmorNeedMask(armor) & kRequiredBasicArmorMask;
                if (needMask == 0) {
                    return false;
                }
                return (profile.armorNeedMask & needMask) == 0;
            }

            return false;
        }

        std::uint32_t RandomIndex(std::uint32_t count);
        std::int32_t RelevantSkill(WorkStation station, std::int32_t smithing, std::int32_t alchemy, std::int32_t enchanting);
        std::vector<WorkRecipe> CollectConstructibleRecipes(WorkStation station, std::int32_t playerLevel, std::int32_t skill, bool usableCampfireOnly);
        bool BuildAlchemyRecipe(std::int32_t playerLevel, std::int32_t alchemySkill, WorkRecipe& out);
        bool BuildEnchantingRecipe(std::int32_t playerLevel, std::int32_t smithingSkill, std::int32_t enchantingSkill, WorkRecipe& out);

        bool DemandRecipeForStation(RE::Actor* actor, WorkStation station, std::int32_t playerLevel, std::int32_t smithingSkill, std::int32_t alchemySkill, std::int32_t enchantingSkill, WorkRecipe& out)
        {
            if (!StationAvailable(station)) {
                return false;
            }

            if (station == WorkStation::SharpeningWheel || station == WorkStation::Workbench) {
                if (!ActorHasImproveDemand(actor, station)) {
                    return false;
                }
            }

            std::vector<WorkRecipe> recipes;
            if (station == WorkStation::Alchemy) {
                WorkRecipe recipe{};
                if (BuildAlchemyRecipe(playerLevel, alchemySkill, recipe) && !ActorAlreadySatisfiedForRecipe(actor, recipe)) {
                    recipes.push_back(recipe);
                }
            }
            else if (station == WorkStation::Enchanting) {
                WorkRecipe recipe{};
                if (BuildEnchantingRecipe(playerLevel, smithingSkill, enchantingSkill, recipe) && !ActorAlreadySatisfiedForRecipe(actor, recipe)) {
                    recipes.push_back(recipe);
                }
            }
            else {
                const auto skill = RelevantSkill(station, smithingSkill, alchemySkill, enchantingSkill);
                const bool usableCampfireOnly = station == WorkStation::Cooking && LastCookingStationIsUsableCampfire();
                auto pool = CollectConstructibleRecipes(station, playerLevel, skill, usableCampfireOnly);
                for (const auto& recipe : pool) {
                    if (station == WorkStation::SharpeningWheel || station == WorkStation::Workbench) {
                        // R176H/R189: Improve must target an item the actor already owns/wears,
                        // and must not repeat the same base item that was already assigned
                        // during this captive runtime. Crafting creates missing gear; Improve
                        // upgrades existing gear.
                        if (!ActorCanImproveRecipeResult(actor, recipe)) {
                            continue;
                        }
                        const auto actorID = actor ? actor->GetFormID() : 0u;
                        const auto resultID = recipe.result ? recipe.result->GetFormID() : 0u;
                        if (IsImproveResultHandledForActorRuntime(actorID, resultID)) {
                            spdlog::info("[TFD][WorkNative][R189] improve recipe skipped handled actor={:08X} result={:08X} editor='{}' station={}",
                                actorID,
                                resultID,
                                recipe.resultEditorID,
                                static_cast<int>(station));
                            continue;
                        }
                    }
                    else if (station == WorkStation::Forge) {
                        // R182: Forge work must fill the Boss' missing equipment slot.
                        // Do not accept generic forge COBJ results just because the actor
                        // does not own that exact form.
                        if (!ForgeRecipeFillsMissingBasicGearDemand(actor, recipe)) {
                            continue;
                        }
                    }
                    else {
                        // R176H: crafting must not duplicate a filled inventory/equipment need slot.
                        if (ActorAlreadySatisfiedForRecipe(actor, recipe)) {
                            continue;
                        }
                    }
                    recipes.push_back(recipe);
                }
            }

            if (recipes.empty()) {
                return false;
            }
            out = recipes[RandomIndex(static_cast<std::uint32_t>(recipes.size()))];
            spdlog::info("[TFD][WorkNative][R176H] demand recipe accepted actor={:08X} station={} result={:08X} resultEditor='{}' pool={}",
                actor ? actor->GetFormID() : 0u,
                static_cast<int>(station),
                out.result ? out.result->GetFormID() : 0u,
                out.resultEditorID,
                recipes.size());
            return true;
        }

        std::uint32_t JobMask(WorkJob job)
        {
            const auto index = static_cast<std::uint32_t>(job);
            if (index == 0 || index >= 31) {
                return 0;
            }
            return 1u << index;
        }

        void CommitSelectedJobForActor(RE::FormID actorID, WorkJob job, WorkStation station, const char* source)
        {
            if (actorID == 0 || job == WorkJob::None || job == WorkJob::NoJob) {
                spdlog::info("[TFD][WorkNative][R176F] commit skipped actor={:08X} job={} station={} source={}",
                    actorID,
                    static_cast<int>(job),
                    static_cast<int>(station),
                    source ? source : "unknown");
                return;
            }

            const auto mask = JobMask(job);
            auto planIt = g_sessionPlans.find(actorID);
            if (planIt != g_sessionPlans.end() && mask != 0) {
                planIt->second.usedMask |= mask;
                planIt->second.lastSelectedJob = job;
                planIt->second.lastSelectedStation = station;
            }

            auto& memory = g_actorWorkMemory[actorID];
            if (job == WorkJob::Improve) {
                memory.improveUsedThisRuntime = true;
                if (g_hasSelectedWorkJob &&
                    g_selectedWorkActorID == actorID &&
                    g_selectedWorkJob.job == WorkJob::Improve &&
                    g_selectedWorkJob.hasRecipe &&
                    g_selectedWorkJob.recipe.result) {
                    MarkImproveResultHandledForActorRuntime(actorID, g_selectedWorkJob.recipe.result->GetFormID(), source ? source : "commit_improve");
                }
            }
            if (memory.lastJob == job && memory.lastStation == station) {
                ++memory.repeatStreak;
            }
            else {
                memory.repeatStreak = 1;
            }
            memory.lastJob = job;
            memory.lastStation = station;
            memory.recentMask = ((memory.recentMask << 1) | mask) & 0x00FFFFFFu;

            spdlog::info("[TFD][WorkNative][R176F] commit job actor={:08X} job={} station={} usedMask={:08X} repeat={} source={}",
                actorID,
                static_cast<int>(job),
                static_cast<int>(station),
                planIt != g_sessionPlans.end() ? planIt->second.usedMask : 0u,
                memory.repeatStreak,
                source ? source : "unknown");

            if (auto* actor = LookupActor(actorID)) {
                RefreshWorkDemandFactionsForBossInternal(actor, job == WorkJob::Improve ? "commit_improve_refresh_demand" : "commit_job_refresh_demand");
            }
        }

        bool CandidateMatchesHint(const SelectedWorkJob& candidate, WorkJob hintJob, WorkStation hintStation)
        {
            if (hintJob != WorkJob::None && candidate.job != hintJob) {
                return false;
            }
            if (hintStation != WorkStation::None && candidate.station != hintStation) {
                return false;
            }
            return true;
        }

        void PublishSelectedJob(const SelectedWorkJob& selected, RE::FormID actorId, const char* source)
        {
            g_selectedWorkJob = selected;
            g_hasSelectedWorkJob = selected.job != WorkJob::None;
            g_selectedWorkActorID = actorId;
            g_hasWorkRecipe = selected.hasRecipe;
            g_lastWorkRecipe = selected.hasRecipe ? selected.recipe : WorkRecipe{};

            if (selected.job == WorkJob::NoJob && actorId != 0) {
                PublishNoJobDemandForActor(LookupActor(actorId), source ? source : "publish_selected_no_job");
            }

            spdlog::info(
                "[TFD][WorkNative][R175A] selected job={} station={} miningState={} actor={:08X} hasRecipe={} result={:08X} resultEditor='{}' reason='{}' source={}",
                static_cast<int>(selected.job),
                static_cast<int>(selected.station),
                selected.miningState,
                actorId,
                selected.hasRecipe ? 1 : 0,
                selected.hasRecipe && selected.recipe.result ? selected.recipe.result->GetFormID() : 0u,
                selected.hasRecipe ? selected.recipe.resultEditorID : std::string{},
                selected.reason,
                source ? source : "unknown");
        }

        WorkStation StationFromHint(std::int32_t hintStation)
        {
            if (hintStation < 0 || hintStation > 9) {
                return WorkStation::None;
            }
            return static_cast<WorkStation>(hintStation);
        }

        WorkJob JobFromHint(std::int32_t hintJob)
        {
            if (hintJob < 0 || hintJob > 9) {
                return WorkJob::None;
            }
            return static_cast<WorkJob>(hintJob);
        }

        bool BuildConstructibleRecipe(RE::BGSConstructibleObject* recipe, WorkStation station, std::int32_t playerLevel, std::int32_t skill, WorkRecipe& out)
        {
            if (!recipe) {
                return false;
            }

            auto* bench = RecipeBenchKeyword(recipe);
            if (!BenchKeywordMatches(station, bench)) {
                return false;
            }

            auto* result = RecipeResult(recipe);
            if (IsRejectedConstructibleResult(station, result)) {
                return false;
            }

            const auto recipeEditorID = EditorID(recipe);
            const auto benchEditorID = EditorID(bench);
            const bool usableCampfireRecipe = station == WorkStation::Cooking &&
                (ContainsInsensitive(recipeEditorID, "UC_CampfireRecipe") ||
                    ContainsInsensitive(benchEditorID, "UC_Campfire") ||
                    ContainsInsensitive(benchEditorID, "CampfireUse"));

            if (!IsCoreGameWorkForm(result)) {
                return false;
            }
            if (!IsCoreGameWorkForm(recipe) && !usableCampfireRecipe) {
                return false;
            }

            WorkRecipe candidate{};
            candidate.station = station;
            candidate.result = result;
            candidate.resultCount = RecipeResultCount(recipe);
            candidate.recipeFormID = recipe->GetFormID();
            candidate.recipeEditorID = recipeEditorID;
            candidate.resultEditorID = EditorID(result);

            if (station == WorkStation::Forge || station == WorkStation::Smelter || station == WorkStation::Tanning ||
                station == WorkStation::SharpeningWheel || station == WorkStation::Workbench) {
                if (IsRejectedSpecialWorkText(candidate.recipeEditorID) || IsRejectedSpecialWorkText(candidate.resultEditorID)) {
                    return false;
                }
            }

            candidate.estimatedTier = EstimateTierFromText(candidate.recipeEditorID + " " + candidate.resultEditorID);

            if (!SkillLevelAllowed(candidate.estimatedTier, playerLevel, skill)) {
                return false;
            }

            bool invalid = false;
            ForEachRequiredItem(recipe, [&](RE::TESForm* item, std::int32_t count) {
                if (invalid) {
                    return;
                }
                if (!item || count <= 0) {
                    invalid = true;
                    return;
                }

                if (!IsCoreGameWorkForm(item)) {
                    invalid = true;
                    return;
                }

                if (station == WorkStation::Forge || station == WorkStation::Smelter || station == WorkStation::Tanning ||
                    station == WorkStation::SharpeningWheel || station == WorkStation::Workbench) {
                    if (IsRejectedSpecialWorkText(EditorID(item))) {
                        invalid = true;
                        return;
                    }
                }

                // Tempering COBJ data may include the base item as a requirement on some setups.
                // The Boss gives the item-to-improve separately, so do not duplicate it as a helper ingredient.
                if ((station == WorkStation::SharpeningWheel || station == WorkStation::Workbench) && item == candidate.result) {
                    return;
                }

                if (candidate.ingredientCount >= 3) {
                    invalid = true;
                    return;
                }
                candidate.ingredients[static_cast<std::size_t>(candidate.ingredientCount)] = RecipeIngredient{ item, count };
                ++candidate.ingredientCount;
                });

            if (invalid || candidate.ingredientCount <= 0) {
                return false;
            }

            out = candidate;
            return true;
        }

        std::vector<WorkRecipe> CollectConstructibleRecipes(WorkStation station, std::int32_t playerLevel, std::int32_t skill, bool usableCampfireOnly)
        {
            std::vector<WorkRecipe> result;
            auto* data = RE::TESDataHandler::GetSingleton();
            if (!data) {
                return result;
            }

            std::uint32_t skippedUsableCampfireMismatch = 0;
            auto& recipes = data->GetFormArray<RE::BGSConstructibleObject>();
            for (auto* recipe : recipes) {
                WorkRecipe candidate{};
                if (!BuildConstructibleRecipe(recipe, station, playerLevel, skill, candidate)) {
                    continue;
                }
                if (usableCampfireOnly && station == WorkStation::Cooking && !IsUsableCampfireCookingRecipe(candidate)) {
                    ++skippedUsableCampfireMismatch;
                    continue;
                }
                result.push_back(candidate);
            }

            if (usableCampfireOnly && station == WorkStation::Cooking) {
                spdlog::info("[TFD][WorkNative][R225A] cooking recipe pool restricted to UsableCampfire recipes accepted={} skippedVanillaOrWrongBench={}",
                    result.size(),
                    skippedUsableCampfireMismatch);
            }
            return result;
        }

        template <class Ingredient>
        std::vector<RE::FormID> IngredientEffectIDs(Ingredient* ingredient)
        {
            std::vector<RE::FormID> ids;
            ForEachIngredientEffect(ingredient, [&](RE::TESForm* effect) {
                if (effect) {
                    ids.push_back(effect->GetFormID());
                }
                });
            return ids;
        }

        bool SharesAnyEffect(const std::vector<RE::FormID>& a, const std::vector<RE::FormID>& b)
        {
            for (const auto x : a) {
                for (const auto y : b) {
                    if (x != 0 && x == y) {
                        return true;
                    }
                }
            }
            return false;
        }

        RE::TESForm* PickAlchemyDisplayPotion(std::int32_t playerLevel, std::int32_t alchemySkill)
        {
            auto* data = RE::TESDataHandler::GetSingleton();
            if (!data) {
                return nullptr;
            }

            std::vector<RE::TESForm*> candidates;
            auto& potions = data->GetFormArray<RE::AlchemyItem>();
            for (auto* potion : potions) {
                if (!potion || !IsCoreGameWorkForm(potion)) {
                    continue;
                }
                const auto editor = EditorID(potion);
                const auto lower = Lower(editor);
                if (lower.empty()) {
                    continue;
                }
                if (lower.find("poison") != std::string::npos) {
                    continue;
                }
                if (lower.find("potion") == std::string::npos && lower.find("restore") == std::string::npos && lower.find("fortify") == std::string::npos) {
                    continue;
                }
                const auto tier = EstimateTierFromText(editor);
                if (!SkillLevelAllowed(tier, playerLevel, alchemySkill)) {
                    continue;
                }
                candidates.push_back(potion);
            }

            if (candidates.empty()) {
                return LookupSkyrimForm(0x0003EADD); // fallback: common low-tier potion if present
            }
            return candidates[RandomIndex(static_cast<std::uint32_t>(candidates.size()))];
        }

        bool BuildAlchemyRecipe(std::int32_t playerLevel, std::int32_t alchemySkill, WorkRecipe& out)
        {
            auto* data = RE::TESDataHandler::GetSingleton();
            if (!data) {
                return false;
            }

            struct IngredientCandidate
            {
                RE::TESForm* form{ nullptr };
                std::string editor{};
                std::vector<RE::FormID> effectIDs{};
                std::int32_t tier{ 0 };
            };

            std::vector<IngredientCandidate> ingredients;
            auto& forms = data->GetFormArray<RE::IngredientItem>();
            for (auto* ingredient : forms) {
                if (!ingredient || !IsCoreGameWorkForm(ingredient)) {
                    continue;
                }
                const auto editor = EditorID(ingredient);
                const auto effects = IngredientEffectIDs(ingredient);
                if (effects.empty()) {
                    continue;
                }
                const auto tier = EstimateTierFromText(editor);
                if (!SkillLevelAllowed(tier, playerLevel, alchemySkill)) {
                    continue;
                }
                ingredients.push_back(IngredientCandidate{ ingredient, editor, effects, tier });
            }

            if (ingredients.size() < 2) {
                return false;
            }

            std::vector<WorkRecipe> candidates;
            for (std::size_t i = 0; i < ingredients.size(); ++i) {
                for (std::size_t j = i + 1; j < ingredients.size(); ++j) {
                    if (!SharesAnyEffect(ingredients[i].effectIDs, ingredients[j].effectIDs)) {
                        continue;
                    }
                    WorkRecipe recipe{};
                    recipe.station = WorkStation::Alchemy;
                    recipe.result = PickAlchemyDisplayPotion(playerLevel, alchemySkill);
                    recipe.resultCount = 1;
                    recipe.ingredients[0] = RecipeIngredient{ ingredients[i].form, 1 };
                    recipe.ingredients[1] = RecipeIngredient{ ingredients[j].form, 1 };
                    recipe.ingredientCount = 2;
                    recipe.recipeEditorID = std::string("alchemy_pair:") + ingredients[i].editor + "+" + ingredients[j].editor;
                    recipe.resultEditorID = EditorID(recipe.result);
                    recipe.estimatedTier = std::max(ingredients[i].tier, ingredients[j].tier);
                    candidates.push_back(recipe);
                    if (candidates.size() >= 512) {
                        break;
                    }
                }
                if (candidates.size() >= 512) {
                    break;
                }
            }

            if (candidates.empty()) {
                return false;
            }
            out = candidates[RandomIndex(static_cast<std::uint32_t>(candidates.size()))];
            return out.result != nullptr;
        }

        RE::TESForm* SoulGemForEnchantingSkill(std::int32_t enchantingSkill)
        {
            // Keep the first pass conservative. These are filled soul gem variants from Skyrim.esm.
            if (enchantingSkill >= 75) {
                if (auto* form = LookupSkyrimForm(0x0002E4FF)) return form; // Grand Soul Gem filled
            }
            if (enchantingSkill >= 50) {
                if (auto* form = LookupSkyrimForm(0x0002E4FB)) return form; // Greater Soul Gem filled
            }
            if (enchantingSkill >= 25) {
                if (auto* form = LookupSkyrimForm(0x0002E4F3)) return form; // Common Soul Gem filled
            }
            if (auto* form = LookupSkyrimForm(0x0002E4E5)) return form; // Lesser Soul Gem filled
            return LookupSkyrimForm(0x0002E4E3); // Petty Soul Gem filled fallback
        }

        bool BuildEnchantingRecipe(std::int32_t playerLevel, std::int32_t smithingSkill, std::int32_t enchantingSkill, WorkRecipe& out)
        {
            std::vector<WorkRecipe> pool;
            auto forge = CollectConstructibleRecipes(WorkStation::Forge, playerLevel, std::max(smithingSkill, enchantingSkill), false);
            auto weaponImprove = CollectConstructibleRecipes(WorkStation::SharpeningWheel, playerLevel, std::max(smithingSkill, enchantingSkill), false);
            auto armorImprove = CollectConstructibleRecipes(WorkStation::Workbench, playerLevel, std::max(smithingSkill, enchantingSkill), false);
            pool.insert(pool.end(), forge.begin(), forge.end());
            pool.insert(pool.end(), weaponImprove.begin(), weaponImprove.end());
            pool.insert(pool.end(), armorImprove.begin(), armorImprove.end());

            if (pool.empty()) {
                return false;
            }

            auto candidate = pool[RandomIndex(static_cast<std::uint32_t>(pool.size()))];
            auto* soulGem = SoulGemForEnchantingSkill(enchantingSkill);
            if (!candidate.result || !soulGem) {
                return false;
            }

            WorkRecipe recipe{};
            recipe.station = WorkStation::Enchanting;
            recipe.result = candidate.result;
            recipe.resultCount = 1;
            recipe.ingredients[0] = RecipeIngredient{ soulGem, 1 };
            recipe.ingredientCount = 1;
            recipe.recipeFormID = candidate.recipeFormID;
            recipe.recipeEditorID = std::string("enchanting_item:") + candidate.recipeEditorID;
            recipe.resultEditorID = candidate.resultEditorID;
            recipe.estimatedTier = std::max(candidate.estimatedTier, enchantingSkill);
            out = recipe;
            return true;
        }

        bool PickRecipeForStation(WorkStation station, std::int32_t playerLevel, std::int32_t smithingSkill, std::int32_t alchemySkill, std::int32_t enchantingSkill, WorkRecipe& out)
        {
            if (station == WorkStation::Alchemy) {
                return BuildAlchemyRecipe(playerLevel, alchemySkill, out);
            }
            if (station == WorkStation::Enchanting) {
                return BuildEnchantingRecipe(playerLevel, smithingSkill, enchantingSkill, out);
            }

            const auto skill = RelevantSkill(station, smithingSkill, alchemySkill, enchantingSkill);
            const bool usableCampfireOnly = station == WorkStation::Cooking && LastCookingStationIsUsableCampfire();
            auto recipes = CollectConstructibleRecipes(station, playerLevel, skill, usableCampfireOnly);
            if (recipes.empty()) {
                return false;
            }
            out = recipes[RandomIndex(static_cast<std::uint32_t>(recipes.size()))];
            return true;
        }

        struct WorkJobCandidate
        {
            SelectedWorkJob selected{};
        };

        void AddRecipeCandidate(std::vector<WorkJobCandidate>& candidates, WorkJob job, WorkStation station, std::int32_t playerLevel, std::int32_t smithingSkill, std::int32_t alchemySkill, std::int32_t enchantingSkill, RE::Actor* actor, const char* reason)
        {
            WorkRecipe recipe{};
            if (!DemandRecipeForStation(actor, station, playerLevel, smithingSkill, alchemySkill, enchantingSkill, recipe)) {
                spdlog::info("[TFD][WorkNative][R176K] candidate rejected actor={:08X} job={} station={} reason={} detail=no_matching_demand_recipe",
                    actor ? actor->GetFormID() : 0u,
                    static_cast<int>(job),
                    static_cast<int>(station),
                    reason ? reason : "recipe");
                return;
            }

            SelectedWorkJob selected{};
            selected.job = job;
            selected.station = station;
            selected.miningState = 0;
            selected.hasRecipe = true;
            selected.recipe = recipe;
            selected.reason = reason ? reason : "recipe";
            candidates.push_back(WorkJobCandidate{ selected });
            spdlog::info("[TFD][WorkNative][R176K] candidate accepted actor={:08X} job={} station={} result={:08X} editor='{}' reason={}",
                actor ? actor->GetFormID() : 0u,
                static_cast<int>(job),
                static_cast<int>(station),
                recipe.result ? recipe.result->GetFormID() : 0u,
                recipe.resultEditorID,
                reason ? reason : "recipe");
        }

        void AddSimpleCandidate(std::vector<WorkJobCandidate>& candidates, WorkJob job, WorkStation station, std::int32_t miningState, const char* reason)
        {
            SelectedWorkJob selected{};
            selected.job = job;
            selected.station = station;
            selected.miningState = miningState;
            selected.hasRecipe = false;
            selected.reason = reason ? reason : "simple";
            candidates.push_back(WorkJobCandidate{ selected });
        }

        void BuildCaptiveWorkJobPool(std::vector<WorkJobCandidate>& candidates, RE::Actor* actor, std::int32_t playerLevel, std::int32_t smithingSkill, std::int32_t alchemySkill, std::int32_t enchantingSkill, bool pleasureEligible)
        {
            candidates.clear();

            const auto profile = BuildActorDemandProfile(actor);
            const auto actorID = actor ? actor->GetFormID() : 0u;
            const auto memoryIt = g_actorWorkMemory.find(actorID);
            const std::size_t handledImproveCount = memoryIt != g_actorWorkMemory.end() ? memoryIt->second.improvedResultFormIDs.size() : 0u;
            const bool hasGearForImprove = ActorHasTemperingDemand(profile);
            const bool hasMissingGearForCrafting = ActorHasMissingBasicGearDemand(profile);
            spdlog::info("[TFD][WorkNative][R180] demand actor={:08X} inv[weapon={} armorMask={:08X} ore={} firewood={} food={} potion={} poison={}] pleasureEligible={} gearForImprove={} handledImprove={} missingGearForCraft={}",
                actor ? actor->GetFormID() : 0u,
                profile.hasWeapon ? 1 : 0,
                profile.armorNeedMask,
                profile.hasOre ? 1 : 0,
                profile.hasFirewood ? 1 : 0,
                profile.hasFood ? 1 : 0,
                profile.hasPotion ? 1 : 0,
                profile.hasPoison ? 1 : 0,
                pleasureEligible ? 1 : 0,
                hasGearForImprove ? 1 : 0,
                static_cast<int>(handledImproveCount),
                hasMissingGearForCrafting ? 1 : 0);

            // R189: Improve is no longer blocked once per Boss. It is blocked per
            // handled base item, so a Boss can ask to improve boots after sword,
            // but cannot repeat the same already-assigned sword/armor form.
            if (hasGearForImprove) {
                AddRecipeCandidate(candidates, WorkJob::Improve, WorkStation::SharpeningWheel, playerLevel, smithingSkill, alchemySkill, enchantingSkill, actor, "improve_owned_weapon_first");
                AddRecipeCandidate(candidates, WorkJob::Improve, WorkStation::Workbench, playerLevel, smithingSkill, alchemySkill, enchantingSkill, actor, "improve_owned_armor_first");
            }
            else {
                spdlog::info("[TFD][WorkNative][R189] candidate rejected actor={:08X} job={} reason=improve_unavailable gear={} handledImprove={}",
                    actor ? actor->GetFormID() : 0u,
                    static_cast<int>(WorkJob::Improve),
                    ActorHasTemperingDemand(profile) ? 1 : 0,
                    handledImproveCount);
            }

            const auto miningState = CurrentMiningState();
            auto* ore = OreForMiningState(miningState);
            if (miningState > 0 && miningState <= 8 && ore && !profile.hasOre && !ActorHasItem(actor, ore, 1)) {
                AddSimpleCandidate(candidates, WorkJob::Mining, WorkStation::None, miningState, "mining_missing_ore");
                spdlog::info("[TFD][WorkNative][R176K] candidate accepted actor={:08X} job={} station=0 miningState={} reason=mining_missing_ore",
                    actor ? actor->GetFormID() : 0u,
                    static_cast<int>(WorkJob::Mining),
                    miningState);
            }
            else {
                spdlog::info("[TFD][WorkNative][R176K] candidate rejected actor={:08X} job={} station=0 miningState={} reason=mining oreForm={} hasOre={} actorHasOre={}",
                    actor ? actor->GetFormID() : 0u,
                    static_cast<int>(WorkJob::Mining),
                    miningState,
                    ore ? ore->GetFormID() : 0u,
                    profile.hasOre ? 1 : 0,
                    ore && ActorHasItem(actor, ore, 1) ? 1 : 0);
            }

            auto* firewood = FirewoodForm();
            if (StationAvailable(WorkStation::ChoppingBlock) && firewood && !profile.hasFirewood && !ActorHasItem(actor, firewood, 1)) {
                AddSimpleCandidate(candidates, WorkJob::Chopping, WorkStation::ChoppingBlock, 0, "firewood_missing");
                spdlog::info("[TFD][WorkNative][R176K] candidate accepted actor={:08X} job={} station={} reason=firewood_missing",
                    actor ? actor->GetFormID() : 0u,
                    static_cast<int>(WorkJob::Chopping),
                    static_cast<int>(WorkStation::ChoppingBlock));
            }
            else {
                spdlog::info("[TFD][WorkNative][R176K] candidate rejected actor={:08X} job={} station={} reason=chopping station={} firewoodForm={} hasFirewood={} actorHasFirewood={}",
                    actor ? actor->GetFormID() : 0u,
                    static_cast<int>(WorkJob::Chopping),
                    static_cast<int>(WorkStation::ChoppingBlock),
                    StationAvailable(WorkStation::ChoppingBlock) ? 1 : 0,
                    firewood ? firewood->GetFormID() : 0u,
                    profile.hasFirewood ? 1 : 0,
                    firewood && ActorHasItem(actor, firewood, 1) ? 1 : 0);
            }

            if (hasMissingGearForCrafting) {
                AddRecipeCandidate(candidates, WorkJob::Crafting, WorkStation::Forge, playerLevel, smithingSkill, alchemySkill, enchantingSkill, actor, "forge_missing_basic_gear_slot");
            }
            else {
                spdlog::info("[TFD][WorkNative][R177] candidate rejected actor={:08X} job={} station={} reason=basic_gear_slots_satisfied",
                    actor ? actor->GetFormID() : 0u,
                    static_cast<int>(WorkJob::Crafting),
                    static_cast<int>(WorkStation::Forge));
            }            // R184: keep immature work types out of the prepared-offer pool until
            // their exact target / completion / report contracts are stable.  If these
            // enter the pool too early, CK can expose a line whose objective cannot be
            // completed.
            spdlog::info("[TFD][WorkNative][R184] deferred immature work types actor={:08X} smelting=held tanning=held alchemy=held enchanting=held",
                actor ? actor->GetFormID() : 0u);

            AddRecipeCandidate(candidates, WorkJob::Cooking, WorkStation::Cooking, playerLevel, smithingSkill, alchemySkill, enchantingSkill, actor, "cooking_missing_food");

            if (pleasureEligible) {
                AddSimpleCandidate(candidates, WorkJob::Pleasure, WorkStation::None, 0, "desire_faction_demand");
                spdlog::info("[TFD][WorkNative][R176K] candidate accepted actor={:08X} job={} station=0 reason=desire_faction_demand",
                    actor ? actor->GetFormID() : 0u,
                    static_cast<int>(WorkJob::Pleasure));
            }
        }

        SelectedWorkJob MakeNoJobSelection(const char* reason)
        {
            SelectedWorkJob selected{};
            selected.job = WorkJob::NoJob;
            selected.station = WorkStation::None;
            selected.miningState = 0;
            selected.hasRecipe = false;
            selected.reason = reason ? reason : "no_job";
            return selected;
        }

        int WorkJobSelectionPriority(const SelectedWorkJob& job)
        {
            switch (job.job) {
            case WorkJob::Pleasure:
                return 10;
            case WorkJob::Cooking:
                return 20;
            case WorkJob::Crafting:
                return 30;
            case WorkJob::Mining:
                return 40;
            case WorkJob::Chopping:
                return 50;
            case WorkJob::Improve:
                return 60;
            default:
                return 1000;
            }
        }

        SelectedWorkJob SelectBestPriorityJob(const std::vector<SelectedWorkJob>& jobs, const char* reason)
        {
            if (jobs.empty()) {
                return MakeNoJobSelection("priority_empty");
            }

            int bestPriority = 100000;
            std::vector<SelectedWorkJob> best{};
            for (const auto& job : jobs) {
                const int priority = WorkJobSelectionPriority(job);
                if (priority < bestPriority) {
                    bestPriority = priority;
                    best.clear();
                    best.push_back(job);
                }
                else if (priority == bestPriority) {
                    best.push_back(job);
                }
            }

            auto selected = best.empty() ? jobs[RandomIndex(static_cast<std::uint32_t>(jobs.size()))] : best[RandomIndex(static_cast<std::uint32_t>(best.size()))];
            selected.reason += ":priority";
            if (reason && reason[0]) {
                selected.reason += ":";
                selected.reason += reason;
            }
            return selected;
        }

        std::vector<SelectedWorkJob> BuildActorWorkPlan(RE::Actor* actor, std::int32_t playerLevel, std::int32_t smithingSkill, std::int32_t alchemySkill, std::int32_t enchantingSkill)
        {
            std::vector<WorkJobCandidate> pool{};
            BuildCaptiveWorkJobPool(pool, actor, playerLevel, smithingSkill, alchemySkill, enchantingSkill, ActorHasNativeWorkDesire(actor));

            std::vector<SelectedWorkJob> result{};
            result.reserve(pool.size());
            for (const auto& candidate : pool) {
                result.push_back(candidate.selected);
            }
            return result;
        }

        SelectedWorkJob SelectFromActorPlan(RE::FormID actorID, const std::vector<SelectedWorkJob>& jobs, const char* source)
        {
            (void)source;

            if (jobs.empty()) {
                return MakeNoJobSelection("actor_pool_empty");
            }

            // R183: do not hard-block whole job categories with usedMask.
            // Demand is now inventory-driven: Mining/Chopping disappear after the Boss receives
            // ore/firewood, Crafting may legitimately repeat for another missing gear slot,
            // and Improve has its own runtime guard. The old usedMask filter caused valid
            // remaining demands to collapse into TFDNoJobFaction.
            std::vector<SelectedWorkJob> available = jobs;

            const ActorWorkMemory* memory = nullptr;
            auto memoryIt = g_actorWorkMemory.find(actorID);
            if (memoryIt != g_actorWorkMemory.end()) {
                memory = &memoryIt->second;
            }

            std::vector<SelectedWorkJob> antiRepeat{};
            antiRepeat.reserve(available.size());
            for (const auto& job : available) {
                if (available.size() > 1 && memory && job.job == memory->lastJob && job.station == memory->lastStation) {
                    continue;
                }
                antiRepeat.push_back(job);
            }
            if (antiRepeat.empty()) {
                antiRepeat = available;
            }

            return SelectBestPriorityJob(antiRepeat, "session_plan");
        }

        bool SelectCaptiveWorkJob(RE::Actor* actor, WorkJob hintJob, WorkStation hintStation, std::int32_t playerLevel, std::int32_t smithingSkill, std::int32_t alchemySkill, std::int32_t enchantingSkill, bool pleasureEligible, SelectedWorkJob& out)
        {
            std::vector<WorkJobCandidate> pool;
            BuildCaptiveWorkJobPool(pool, actor, playerLevel, smithingSkill, alchemySkill, enchantingSkill, pleasureEligible || ActorHasNativeWorkDesire(actor));
            if (pool.empty()) {
                out = MakeNoJobSelection("pool_empty");
                return true;
            }

            const auto actorId = actor ? actor->GetFormID() : 0u;

            // R183: explicit dialogue fragments are authoritative. If the player clicked
            // Crafting, native must lock a Crafting job; if the player clicked Improve,
            // native must lock Improve. Anti-repeat must not rewrite dialog A into job B.
            if (hintJob != WorkJob::None || hintStation != WorkStation::None) {
                for (const auto& candidate : pool) {
                    if (CandidateMatchesHint(candidate.selected, hintJob, hintStation)) {
                        out = candidate.selected;
                        out.reason += ":hint_locked";
                        return true;
                    }
                }

                out = MakeNoJobSelection("hint_invalid_no_reroll");
                spdlog::info("[TFD][WorkNative][R183] locked hint invalid actor={:08X} hintJob={} hintStation={} poolSize={} -> no job",
                    actorId,
                    static_cast<int>(hintJob),
                    static_cast<int>(hintStation),
                    pool.size());
                return true;
            }

            auto memoryIt = g_actorWorkMemory.find(actorId);
            const ActorWorkMemory* memory = memoryIt != g_actorWorkMemory.end() ? &memoryIt->second : nullptr;

            std::vector<WorkJobCandidate> filtered;
            filtered.reserve(pool.size());
            for (const auto& candidate : pool) {
                if (pool.size() > 1 && memory && candidate.selected.job == memory->lastJob && candidate.selected.station == memory->lastStation) {
                    continue;
                }
                filtered.push_back(candidate);
            }
            if (filtered.empty()) {
                filtered = pool;
            }

            std::vector<SelectedWorkJob> filteredJobs;
            filteredJobs.reserve(filtered.size());
            for (const auto& candidate : filtered) {
                filteredJobs.push_back(candidate.selected);
            }
            out = SelectBestPriorityJob(filteredJobs, "request_pool");
            return true;
        }

        bool WorkJobHasDialogueFaction(const SelectedWorkJob& job, WorkDemandFactionKind& outKind)
        {
            switch (job.job) {
            case WorkJob::Mining:
                outKind = WorkDemandFactionKind::Mining;
                return true;
            case WorkJob::Chopping:
                outKind = WorkDemandFactionKind::Chopping;
                return job.station == WorkStation::ChoppingBlock;
            case WorkJob::Crafting:
                // R184: only plain forge equipment crafting is mature.
                // Smelting and tanning stay held even if a legacy recipe exists.
                outKind = WorkDemandFactionKind::Crafting;
                return job.station == WorkStation::Forge && job.hasRecipe && job.recipe.result;
            case WorkJob::Improve:
                outKind = WorkDemandFactionKind::Tempering;
                return (job.station == WorkStation::SharpeningWheel || job.station == WorkStation::Workbench) && job.hasRecipe && job.recipe.result;
            case WorkJob::Cooking:
                outKind = WorkDemandFactionKind::Cooking;
                return job.station == WorkStation::Cooking && job.hasRecipe && job.recipe.result;
            default:
                return false;
            }
        }

        std::uint32_t PublishExactWorkOfferFactionsForBoss(RE::Actor* actor, const std::vector<SelectedWorkJob>& jobs, const char* reason)
        {
            if (!actor || actor->IsDead() || actor->IsDisabled()) {
                return 0;
            }

            ClearDemandFactionsForActor(actor, reason ? reason : "publish_exact_offers_clear_actor");

            std::array<bool, static_cast<std::size_t>(WorkDemandFactionKind::Count)> applied{};
            std::uint32_t publishedFactionCount = 0;
            std::uint32_t exactOfferCount = 0;
            bool hasPleasureOffer = false;

            for (const auto& job : jobs) {
                if (job.job == WorkJob::Pleasure) {
                    hasPleasureOffer = true;
                    ++exactOfferCount;
                    continue;
                }

                WorkDemandFactionKind kind = WorkDemandFactionKind::NoJob;
                if (!WorkJobHasDialogueFaction(job, kind)) {
                    continue;
                }

                const auto index = static_cast<std::size_t>(kind);
                if (index >= applied.size() || applied[index]) {
                    ++exactOfferCount;
                    continue;
                }

                ApplyDemandFactionToActor(actor, kind, reason ? reason : "publish_exact_offer");
                applied[index] = true;
                ++publishedFactionCount;
                ++exactOfferCount;
            }

            if (exactOfferCount == 0 && !hasPleasureOffer) {
                ApplyDemandFactionToActor(actor, WorkDemandFactionKind::NoJob, reason ? reason : "publish_exact_no_job");
            }

            spdlog::info("[TFD][WorkNative][R184] exact offer factions actor={:08X} offers={} published={} pleasure={} noJob={} reason={}",
                actor->GetFormID(),
                exactOfferCount,
                publishedFactionCount,
                hasPleasureOffer ? 1 : 0,
                (exactOfferCount == 0 && !hasPleasureOffer) ? 1 : 0,
                reason ? reason : "unknown");

            return exactOfferCount;
        }

        bool HasExactPlayableOffer(const std::vector<SelectedWorkJob>& jobs)
        {
            for (const auto& job : jobs) {
                if (job.job == WorkJob::Pleasure) {
                    return true;
                }
                WorkDemandFactionKind kind = WorkDemandFactionKind::NoJob;
                if (WorkJobHasDialogueFaction(job, kind)) {
                    return true;
                }
            }
            return false;
        }

        int BestOfferPriorityForJobs(const std::vector<SelectedWorkJob>& jobs)
        {
            int best = 100000;
            for (const auto& job : jobs) {
                if (job.job == WorkJob::Pleasure || WorkJobSelectionPriority(job) < 1000) {
                    best = std::min(best, WorkJobSelectionPriority(job));
                }
            }
            return best;
        }

        std::int32_t BuildSessionPlanInternal(RE::Actor* preferredActor, std::int32_t playerLevel, std::int32_t smithingSkill, std::int32_t alchemySkill, std::int32_t enchantingSkill, const char* source)
        {
            g_sessionPlans.clear();
            g_sessionBossOrder.clear();
            g_sessionSelectedActorID = 0;
            g_hasSessionPlan = true;
            ++g_sessionSerial;
            ClearWorkDemandFactionsInternal(source ? source : "build_session_plan_reset_demand");

            (void)TFD::Location::RefreshCaptiveWorkResourceState(true, source ? source : "r176_session_plan");
            ResolveWorkResourceGlobals();

            auto bosses = CollectWorkSessionBossActors(preferredActor);
            spdlog::info("[TFD][WorkNative][R176] BuildWorkSessionPlan serial={} bosses={} preferred={:08X} resources[mining={} forge={} smelter={} tanning={} wheel={} bench={} chop={} cooking={} alchemy={} enchanting={}] source={}",
                g_sessionSerial,
                bosses.size(),
                preferredActor ? preferredActor->GetFormID() : 0u,
                ReadGlobalInt(g_miningStateGlobal),
                ReadGlobalInt(g_forgeStateGlobal),
                ReadGlobalInt(g_smelterStateGlobal),
                ReadGlobalInt(g_tanningStateGlobal),
                ReadGlobalInt(g_sharpeningStateGlobal),
                ReadGlobalInt(g_workbenchStateGlobal),
                ReadGlobalInt(g_choppingStateGlobal),
                ReadGlobalInt(g_cookingStateGlobal),
                ReadGlobalInt(g_alchemyStateGlobal),
                ReadGlobalInt(g_enchantingStateGlobal),
                source ? source : "unknown");

            for (auto* boss : bosses) {
                if (!IsUsableWorkBossActor(boss)) {
                    continue;
                }
                const auto actorID = boss->GetFormID();
                auto jobs = BuildActorWorkPlan(boss, playerLevel, smithingSkill, alchemySkill, enchantingSkill);
                const auto exactOffers = PublishExactWorkOfferFactionsForBoss(boss, jobs, source ? source : "build_session_plan_exact_offers");
                PlannedActorWork plan{};
                plan.actorID = actorID;
                plan.jobs = jobs;
                g_sessionPlans[actorID] = plan;
                g_sessionBossOrder.push_back(actorID);

                std::string jobList{};
                for (const auto& job : jobs) {
                    if (!jobList.empty()) {
                        jobList += ",";
                    }
                    jobList += std::to_string(static_cast<int>(job.job));
                    if (job.station != WorkStation::None) {
                        jobList += ":" + std::to_string(static_cast<int>(job.station));
                    }
                }
                spdlog::info("[TFD][WorkNative][R184] ActorPlan actor={:08X} jobs=[{}] exactOffers={} desire={}",
                    actorID,
                    jobList,
                    exactOffers,
                    ActorHasNativeWorkDesire(boss) ? 1 : 0);
            }

            RE::Actor* selectedActor = nullptr;
            int selectedActorPriority = 100000;
            for (const auto actorID : g_sessionBossOrder) {
                auto* actor = LookupActor(actorID);
                if (!IsUsableWorkBossActor(actor)) {
                    continue;
                }
                const auto planIt = g_sessionPlans.find(actorID);
                if (planIt == g_sessionPlans.end() || !HasExactPlayableOffer(planIt->second.jobs)) {
                    continue;
                }
                const int priority = BestOfferPriorityForJobs(planIt->second.jobs);
                const bool preferredTie = preferredActor && actorID == preferredActor->GetFormID() && priority == selectedActorPriority;
                if (!selectedActor || priority < selectedActorPriority || preferredTie) {
                    selectedActor = actor;
                    selectedActorPriority = priority;
                }
            }

            if (!selectedActor && IsUsableWorkBossActor(preferredActor)) {
                selectedActor = preferredActor;
            }

            SelectedWorkJob selected{};
            if (selectedActor) {
                const auto actorID = selectedActor->GetFormID();
                auto it = g_sessionPlans.find(actorID);
                if (it != g_sessionPlans.end()) {
                    selected = SelectFromActorPlan(actorID, it->second.jobs, source);
                    g_sessionSelectedActorID = actorID;
                }
                else {
                    selected = MakeNoJobSelection("selected_actor_no_plan");
                }
                PublishSelectedJob(selected, actorID, source ? source : "session_plan");
            }
            else {
                selected = MakeNoJobSelection("no_valid_boss");
                PublishSelectedJob(selected, 0, source ? source : "session_plan_no_boss");
            }

            PublishWorkGlobals(selected, source ? source : "session_plan");
            return selected.job == WorkJob::NoJob ? 0 : static_cast<std::int32_t>(selected.job);
        }

        std::int32_t PapyrusBuildCaptiveWorkSessionPlan(RE::StaticFunctionTag*, RE::Actor* preferredActor, std::int32_t playerLevel, std::int32_t smithingSkill, std::int32_t alchemySkill, std::int32_t enchantingSkill)
        {
            return BuildSessionPlanInternal(preferredActor, playerLevel, smithingSkill, alchemySkill, enchantingSkill, "papyrus_build_session_plan");
        }

        std::int32_t PapyrusSelectCaptiveWorkSessionJobForActor(RE::StaticFunctionTag*, RE::Actor* actor, std::int32_t playerLevel, std::int32_t smithingSkill, std::int32_t alchemySkill, std::int32_t enchantingSkill)
        {
            if (!IsUsableWorkBossActor(actor)) {
                auto selected = MakeNoJobSelection("invalid_actor_select");
                PublishSelectedJob(selected, actor ? actor->GetFormID() : 0u, "papyrus_select_session_invalid");
                PublishWorkGlobals(selected, "papyrus_select_session_invalid");
                return 0;
            }

            const auto actorID = actor->GetFormID();
            if (!g_hasSessionPlan || g_sessionPlans.find(actorID) == g_sessionPlans.end()) {
                (void)BuildSessionPlanInternal(actor, playerLevel, smithingSkill, alchemySkill, enchantingSkill, "papyrus_select_rebuild_missing_plan");
            }

            // R183: report/next-offer must reflect current Boss inventory.
            // The old session plan was built before the Boss received ore/firewood/gear,
            // so it could keep stale jobs or exhaust valid remaining demands.
            auto& currentPlan = g_sessionPlans[actorID];
            currentPlan.actorID = actorID;
            currentPlan.jobs = BuildActorWorkPlan(actor, playerLevel, smithingSkill, alchemySkill, enchantingSkill);
            (void)PublishExactWorkOfferFactionsForBoss(actor, currentPlan.jobs, "papyrus_select_actor_exact_offers");

            auto it = g_sessionPlans.find(actorID);
            SelectedWorkJob selected{};
            if (it != g_sessionPlans.end()) {
                selected = SelectFromActorPlan(actorID, it->second.jobs, "papyrus_select_actor");
            }
            else {
                selected = MakeNoJobSelection("actor_plan_missing_after_rebuild");
            }

            g_sessionSelectedActorID = actorID;
            PublishSelectedJob(selected, actorID, "papyrus_select_actor");
            PublishWorkGlobals(selected, "papyrus_select_actor");
            return selected.job == WorkJob::NoJob ? 0 : static_cast<std::int32_t>(selected.job);
        }

        std::int32_t PapyrusClearCaptiveWorkSessionPlan(RE::StaticFunctionTag*)
        {
            g_sessionPlans.clear();
            g_sessionBossOrder.clear();
            g_sessionSelectedActorID = 0;
            g_selectedWorkActorID = 0;
            g_selectedWorkJob = SelectedWorkJob{};
            g_hasSelectedWorkJob = false;
            g_hasWorkRecipe = false;
            g_lastWorkRecipe = WorkRecipe{};
            g_hasSessionPlan = false;
            ClearWorkDemandFactionsInternal("papyrus_clear_session_plan");
            spdlog::info("[TFD][WorkNative][R177] ClearWorkSessionPlan selectedActor and demand factions cleared");
            return 1;
        }

        std::int32_t PapyrusRequestCaptiveWorkJob(RE::StaticFunctionTag*, RE::Actor* actor, std::int32_t hintJob, std::int32_t hintStation, std::int32_t playerLevel, std::int32_t smithingSkill, std::int32_t alchemySkill, std::int32_t enchantingSkill, bool pleasureEligible)
        {
            g_selectedWorkJob = SelectedWorkJob{};
            g_hasSelectedWorkJob = false;
            g_hasWorkRecipe = false;
            g_lastWorkRecipe = WorkRecipe{};

            if (!actor || actor->IsDead() || actor->IsDisabled()) {
                spdlog::warn("[TFD][WorkNative][R175A] request rejected invalid actor hintJob={} hintStation={}", hintJob, hintStation);
                return static_cast<std::int32_t>(WorkJob::None);
            }

            auto exactJobs = BuildActorWorkPlan(actor, playerLevel, smithingSkill, alchemySkill, enchantingSkill);
            (void)PublishExactWorkOfferFactionsForBoss(actor, exactJobs, "papyrus_request_exact_offers");

            SelectedWorkJob selected{};
            const auto nativeHintJob = JobFromHint(hintJob);
            const auto nativeHintStation = StationFromHint(hintStation);
            SelectCaptiveWorkJob(actor, nativeHintJob, nativeHintStation, playerLevel, smithingSkill, alchemySkill, enchantingSkill, pleasureEligible, selected);
            PublishSelectedJob(selected, actor->GetFormID(), "papyrus_request");
            PublishWorkGlobals(selected, "papyrus_request");
            return selected.job == WorkJob::NoJob ? 0 : static_cast<std::int32_t>(selected.job);
        }

        std::int32_t PapyrusCommitCaptiveWorkJob(RE::StaticFunctionTag*, RE::Actor* actor, std::int32_t jobType, std::int32_t stationType)
        {
            if (!actor || actor->IsDead() || actor->IsDisabled()) {
                spdlog::warn("[TFD][WorkNative][R176F] commit rejected invalid actor job={} station={}", jobType, stationType);
                return 0;
            }

            const auto job = JobFromHint(jobType);
            const auto station = StationFromHint(stationType);
            CommitSelectedJobForActor(actor->GetFormID(), job, station, "papyrus_commit");
            return 1;
        }

        std::int32_t PapyrusGetSelectedWorkJobType(RE::StaticFunctionTag*)
        {
            if (!g_hasSelectedWorkJob || g_selectedWorkJob.job == WorkJob::NoJob) {
                return 0;
            }
            return static_cast<std::int32_t>(g_selectedWorkJob.job);
        }

        std::int32_t PapyrusGetSelectedWorkCraftingState(RE::StaticFunctionTag*)
        {
            return g_hasSelectedWorkJob ? static_cast<std::int32_t>(g_selectedWorkJob.station) : 0;
        }

        std::int32_t PapyrusGetSelectedWorkMiningState(RE::StaticFunctionTag*)
        {
            return g_hasSelectedWorkJob ? g_selectedWorkJob.miningState : 0;
        }

        std::int32_t PapyrusGetSelectedWorkActorFormID(RE::StaticFunctionTag*)
        {
            return static_cast<std::int32_t>(g_selectedWorkActorID);
        }

        std::int32_t PapyrusPickRandomWorkRecipe(RE::StaticFunctionTag*, std::int32_t stationType, std::int32_t playerLevel, std::int32_t smithingSkill, std::int32_t alchemySkill, std::int32_t enchantingSkill)
        {
            WorkRecipe recipe{};
            const auto station = static_cast<WorkStation>(stationType);
            if (!PickRecipeForStation(station, playerLevel, smithingSkill, alchemySkill, enchantingSkill, recipe)) {
                g_hasWorkRecipe = false;
                g_lastWorkRecipe = WorkRecipe{};
                spdlog::warn("[TFD][WorkNative][W21] no core recipe found station={} playerLevel={} smithing={} alchemy={} enchanting={}", stationType, playerLevel, smithingSkill, alchemySkill, enchantingSkill);
                return 0;
            }

            g_lastWorkRecipe = recipe;
            g_hasWorkRecipe = true;
            spdlog::info("[TFD][WorkNative][W21] picked core recipe station={} recipe={:08X} recipeEditor='{}' result={:08X} resultEditor='{}' resultCount={} ingredients={} tier={} level={} skills[smi={} alc={} enc={}]",
                stationType,
                g_lastWorkRecipe.recipeFormID,
                g_lastWorkRecipe.recipeEditorID,
                g_lastWorkRecipe.result ? g_lastWorkRecipe.result->GetFormID() : 0,
                g_lastWorkRecipe.resultEditorID,
                g_lastWorkRecipe.resultCount,
                g_lastWorkRecipe.ingredientCount,
                g_lastWorkRecipe.estimatedTier,
                playerLevel,
                smithingSkill,
                alchemySkill,
                enchantingSkill);

            for (std::int32_t i = 0; i < g_lastWorkRecipe.ingredientCount; ++i) {
                const auto& ingredient = g_lastWorkRecipe.ingredients[static_cast<std::size_t>(i)];
                spdlog::info("[TFD][WorkNative][W21] recipe ingredient slot={} item={:08X} editor='{}' count={}",
                    i + 1,
                    ingredient.item ? ingredient.item->GetFormID() : 0,
                    EditorID(ingredient.item),
                    ingredient.count);
            }
            return 1;
        }

        std::int32_t PapyrusPickRandomCookingRecipe(RE::StaticFunctionTag* tag)
        {
            return PapyrusPickRandomWorkRecipe(tag, static_cast<std::int32_t>(WorkStation::Cooking), 1, 0, 0, 0);
        }

        RE::TESForm* PapyrusGetPickedWorkRecipeResult(RE::StaticFunctionTag*)
        {
            return g_hasWorkRecipe ? g_lastWorkRecipe.result : nullptr;
        }

        std::int32_t PapyrusGetPickedWorkRecipeResultCount(RE::StaticFunctionTag*)
        {
            return g_hasWorkRecipe ? std::max(1, g_lastWorkRecipe.resultCount) : 0;
        }

        RE::TESForm* PapyrusGetPickedWorkRecipeIngredient(RE::StaticFunctionTag*, std::int32_t slot)
        {
            if (!g_hasWorkRecipe || slot < 1 || slot > 3) {
                return nullptr;
            }
            return g_lastWorkRecipe.ingredients[static_cast<std::size_t>(slot - 1)].item;
        }

        std::int32_t PapyrusGetPickedWorkRecipeIngredientCount(RE::StaticFunctionTag*, std::int32_t slot)
        {
            if (!g_hasWorkRecipe || slot < 1 || slot > 3) {
                return 0;
            }
            return g_lastWorkRecipe.ingredients[static_cast<std::size_t>(slot - 1)].count;
        }

        std::int32_t PapyrusGetPickedWorkRecipeStationType(RE::StaticFunctionTag*)
        {
            return g_hasWorkRecipe ? static_cast<std::int32_t>(g_lastWorkRecipe.station) : 0;
        }
    }

    RE::Actor* SelectNextBossWithExactWorkOffer(RE::Actor* a_preferredActor, std::uint32_t a_excludeFormID, const char* a_reason)
    {
        std::vector<RE::FormID> order = g_sessionBossOrder;
        if (order.empty()) {
            auto bosses = CollectWorkSessionBossActors(a_preferredActor);
            for (auto* boss : bosses) {
                if (IsUsableWorkBossActor(boss)) {
                    order.push_back(boss->GetFormID());
                }
            }
        }

        RE::Actor* bestActor = nullptr;
        int bestPriority = 100000;
        for (const auto actorID : order) {
            if (actorID == 0 || actorID == a_excludeFormID) {
                continue;
            }
            auto* actor = LookupActor(actorID);
            if (!IsUsableWorkBossActor(actor)) {
                continue;
            }

            auto planIt = g_sessionPlans.find(actorID);
            if (planIt == g_sessionPlans.end()) {
                continue;
            }
            if (!HasExactPlayableOffer(planIt->second.jobs)) {
                spdlog::info("[TFD][WorkNative][R189] boss skipped no exact offer actor={:08X} reason={}",
                    actorID,
                    a_reason ? a_reason : "unknown");
                continue;
            }

            const int priority = BestOfferPriorityForJobs(planIt->second.jobs);
            if (!bestActor || priority < bestPriority) {
                bestActor = actor;
                bestPriority = priority;
            }
        }

        spdlog::info("[TFD][WorkNative][R189] select next exact-offer boss preferred={:08X} exclude={:08X} result={:08X} priority={} plans={} reason={}",
            a_preferredActor ? a_preferredActor->GetFormID() : 0u,
            a_excludeFormID,
            bestActor ? bestActor->GetFormID() : 0u,
            bestPriority,
            g_sessionPlans.size(),
            a_reason ? a_reason : "unknown");
        return bestActor;
    }

    bool ActorHasExactWorkOffer(RE::Actor* a_actor)
    {
        if (!IsUsableWorkBossActor(a_actor)) {
            return false;
        }
        const auto it = g_sessionPlans.find(a_actor->GetFormID());
        return it != g_sessionPlans.end() && HasExactPlayableOffer(it->second.jobs);
    }

    void ResetRuntimeRecipeCache()
    {
        g_lastWorkRecipe = WorkRecipe{};
        g_hasWorkRecipe = false;
        g_selectedWorkJob = SelectedWorkJob{};
        g_hasSelectedWorkJob = false;
        g_selectedWorkActorID = 0;
        g_sessionPlans.clear();
        g_sessionBossOrder.clear();
        g_sessionSelectedActorID = 0;
        g_hasSessionPlan = false;
        g_actorWorkMemory.clear();
        ClearWorkDemandFactionsInternal("reset_runtime_recipe_cache");
        spdlog::info("[TFD][WorkNative][R177] runtime recipe/job/session cache and demand factions reset");
    }

    void RefreshWorkDemandFactionsForBoss(RE::Actor* a_boss, const char* a_reason)
    {
        RefreshWorkDemandFactionsForBossInternal(a_boss, a_reason ? a_reason : "public_refresh_work_demand");
    }

    void ClearWorkDemandFactions(const char* a_reason)
    {
        ClearWorkDemandFactionsInternal(a_reason ? a_reason : "public_clear_work_demand");
    }

    bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm)
    {
        if (!a_vm) {
            return false;
        }

        a_vm->RegisterFunction("BuildCaptiveWorkSessionPlan", "TFDWorkNative", PapyrusBuildCaptiveWorkSessionPlan);
        a_vm->RegisterFunction("SelectCaptiveWorkSessionJobForActor", "TFDWorkNative", PapyrusSelectCaptiveWorkSessionJobForActor);
        a_vm->RegisterFunction("ClearCaptiveWorkSessionPlan", "TFDWorkNative", PapyrusClearCaptiveWorkSessionPlan);
        a_vm->RegisterFunction("RequestCaptiveWorkJob", "TFDWorkNative", PapyrusRequestCaptiveWorkJob);
        a_vm->RegisterFunction("GetSelectedWorkJobType", "TFDWorkNative", PapyrusGetSelectedWorkJobType);
        a_vm->RegisterFunction("GetSelectedWorkCraftingState", "TFDWorkNative", PapyrusGetSelectedWorkCraftingState);
        a_vm->RegisterFunction("GetSelectedWorkMiningState", "TFDWorkNative", PapyrusGetSelectedWorkMiningState);
        a_vm->RegisterFunction("GetSelectedWorkActorFormID", "TFDWorkNative", PapyrusGetSelectedWorkActorFormID);
        a_vm->RegisterFunction("CommitCaptiveWorkJob", "TFDWorkNative", PapyrusCommitCaptiveWorkJob);

        a_vm->RegisterFunction("PickRandomWorkRecipe", "TFDWorkNative", PapyrusPickRandomWorkRecipe);
        a_vm->RegisterFunction("GetPickedWorkRecipeResult", "TFDWorkNative", PapyrusGetPickedWorkRecipeResult);
        a_vm->RegisterFunction("GetPickedWorkRecipeResultCount", "TFDWorkNative", PapyrusGetPickedWorkRecipeResultCount);
        a_vm->RegisterFunction("GetPickedWorkRecipeIngredient", "TFDWorkNative", PapyrusGetPickedWorkRecipeIngredient);
        a_vm->RegisterFunction("GetPickedWorkRecipeIngredientCount", "TFDWorkNative", PapyrusGetPickedWorkRecipeIngredientCount);
        a_vm->RegisterFunction("GetPickedWorkRecipeStationType", "TFDWorkNative", PapyrusGetPickedWorkRecipeStationType);

        // Backward compatibility for W13 PEX during transition.
        a_vm->RegisterFunction("PickRandomCookingRecipe", "TFDWorkNative", PapyrusPickRandomCookingRecipe);
        a_vm->RegisterFunction("GetPickedCookingRecipeResult", "TFDWorkNative", PapyrusGetPickedWorkRecipeResult);
        a_vm->RegisterFunction("GetPickedCookingRecipeResultCount", "TFDWorkNative", PapyrusGetPickedWorkRecipeResultCount);
        a_vm->RegisterFunction("GetPickedCookingRecipeIngredient", "TFDWorkNative", PapyrusGetPickedWorkRecipeIngredient);
        a_vm->RegisterFunction("GetPickedCookingRecipeIngredientCount", "TFDWorkNative", PapyrusGetPickedWorkRecipeIngredientCount);

        spdlog::info("[TFD][WorkNative][R177] Papyrus natives registered; demand faction publisher active; actor inventory demand drives Work dialogue flags");
        return true;
    }
}
