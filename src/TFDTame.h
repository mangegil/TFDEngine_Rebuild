#pragma once

#include <RE/Skyrim.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace TFD::HostilityController
{
    enum class Mode : std::uint8_t;
    enum class ReleaseReason : std::uint8_t;
}

namespace TFD::Tame
{
    enum class TameDisposition : std::uint8_t
    {
        None = 0,
        Calm = 1,
        Companion = 2
    };

    struct ActiveSnapshot
    {
        RE::FormID actorId{ 0 };
        RE::FormID sessionId{ 0 };
        TFD::HostilityController::Mode mode{ static_cast<TFD::HostilityController::Mode>(0) };
        TameDisposition disposition{ TameDisposition::None };
        double remainingTameSec{ 0.0 };
        double remainingCompanionHours{ 0.0 };
        bool loaded{ false };
        std::string actorName{};
    };

    enum class FeedAction : std::uint8_t
    {
        Calm = 0,
        Teammate
    };

    struct FeedOptionSnapshot
    {
        RE::FormID itemId{ 0 };
        std::string label{};
        std::string itemName{};
        std::int32_t count{ 0 };
        std::int32_t cost{ 0 };
        double calmExtendSec{ 0.0 };
        FeedAction action{ FeedAction::Calm };
    };

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

    using ReleaseReason = TFD::HostilityController::ReleaseReason;

    Category ResolveCategory(RE::Actor* target);
    std::vector<Option> CollectValidBaits(RE::Actor* player, RE::Actor* target);
    std::optional<Option> SelectBestBait(const std::vector<Option>& options);
    bool ConsumeBait(RE::Actor* player, RE::TESBoundObject* item, std::int32_t count = 1);
    const char* ToString(Category category);

    bool CanStart(RE::Actor* actor);
    bool HasActiveSession(RE::Actor* actor);

    std::optional<RE::FormID> BeginSession(
        RE::Actor* player,
        RE::Actor* primaryTarget,
        double nowSec,
        bool allowDialogue = false,
        bool allowLocalSplash = false);

    std::vector<ActiveSnapshot> GetActiveSnapshots(double nowSec = 0.0);
    std::vector<FeedOptionSnapshot> GetFeedOptions(RE::Actor* actor, FeedAction action);
    bool ApplyFeed(RE::Actor* actor, RE::FormID itemId, FeedAction action);
    bool ExtendSession(RE::Actor* actor, double addSec, double nowSec);
    double GetRemainingTime(RE::Actor* actor, double nowSec);
    TameDisposition GetDisposition(RE::Actor* actor);
    bool PromoteToCompanion(RE::Actor* actor, double addHoursGameTime);
    bool ExtendCompanionHours(RE::Actor* actor, double addHoursGameTime);
    bool Release(RE::Actor* actor, ReleaseReason reason = static_cast<ReleaseReason>(0));
    bool IsCompanion(RE::Actor* actor);
    double GetRemainingCompanionHours(RE::Actor* actor);
}
