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
#include <utility>
#include <vector>

#include "EditorIdCache.h"

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
                    lower.find("armorbench") != std::string::npos;
            case WorkStation::Cooking:
                return lower.find("cook") != std::string::npos || lower.find("cooking") != std::string::npos || lower.find("cookpot") != std::string::npos;
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
            if (station == WorkStation::Forge || station == WorkStation::Smelter || station == WorkStation::Tanning ||
                station == WorkStation::SharpeningWheel || station == WorkStation::Workbench) {
                if (HasSpecialWorkBlockToken(editor)) {
                    return true;
                }
            }
            if (station == WorkStation::Forge || station == WorkStation::Smelter || station == WorkStation::Tanning) {
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
            auto* data = RE::TESDataHandler::GetSingleton();
            if (!data) {
                return nullptr;
            }
            return data->LookupForm<RE::TESForm>(localFormID, "Skyrim.esm");
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

            if (!IsCoreGameWorkForm(recipe) || !IsCoreGameWorkForm(result)) {
                return false;
            }

            WorkRecipe candidate{};
            candidate.station = station;
            candidate.result = result;
            candidate.resultCount = RecipeResultCount(recipe);
            candidate.recipeFormID = recipe->GetFormID();
            candidate.recipeEditorID = EditorID(recipe);
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

        std::vector<WorkRecipe> CollectConstructibleRecipes(WorkStation station, std::int32_t playerLevel, std::int32_t skill)
        {
            std::vector<WorkRecipe> result;
            auto* data = RE::TESDataHandler::GetSingleton();
            if (!data) {
                return result;
            }

            auto& recipes = data->GetFormArray<RE::BGSConstructibleObject>();
            for (auto* recipe : recipes) {
                WorkRecipe candidate{};
                if (!BuildConstructibleRecipe(recipe, station, playerLevel, skill, candidate)) {
                    continue;
                }
                result.push_back(candidate);
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
            auto forge = CollectConstructibleRecipes(WorkStation::Forge, playerLevel, std::max(smithingSkill, enchantingSkill));
            auto weaponImprove = CollectConstructibleRecipes(WorkStation::SharpeningWheel, playerLevel, std::max(smithingSkill, enchantingSkill));
            auto armorImprove = CollectConstructibleRecipes(WorkStation::Workbench, playerLevel, std::max(smithingSkill, enchantingSkill));
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
            auto recipes = CollectConstructibleRecipes(station, playerLevel, skill);
            if (recipes.empty()) {
                return false;
            }
            out = recipes[RandomIndex(static_cast<std::uint32_t>(recipes.size()))];
            return true;
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

    void ResetRuntimeRecipeCache()
    {
        g_lastWorkRecipe = WorkRecipe{};
        g_hasWorkRecipe = false;
        spdlog::info("[TFD][WorkNative][W21] runtime recipe cache reset");
    }

    bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm)
    {
        if (!a_vm) {
            return false;
        }

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

        spdlog::info("[TFD][WorkNative][W21] Papyrus natives registered core-game recipe filter active; Skyforge/NordHero blocked; enchanted tempering blocked; basic tempering allowlist active");
        return true;
    }
}
