#pragma once

#include <RE/Skyrim.h>

#include <optional>
#include <string>
#include <vector>

namespace TFD::TameBait
{
    enum class Category : std::uint8_t
    {
        None = 0,
        SoulGem,
        Apocrypha,
        Plant,
        Fish,
        SmallMeat,
        MediumMeat,
        HeavyMeat
    };

    struct Option
    {
        RE::TESBoundObject* item{ nullptr };
        std::string name{};
        std::int32_t count{ 0 };
        double extendSec{ 0.0 };
        Category category{ Category::None };
    };

    Category ResolveCategory(RE::Actor* target);
    std::vector<Option> CollectValidBaits(RE::Actor* player, RE::Actor* target);
    std::optional<Option> SelectBestBait(const std::vector<Option>& options);
    bool ConsumeBait(RE::Actor* player, RE::TESBoundObject* item, std::int32_t count = 1);
    const char* ToString(Category category);
}
