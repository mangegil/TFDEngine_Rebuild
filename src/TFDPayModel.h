#pragma once

#include <RE/Skyrim.h>

#include <cstdint>
#include <string>
#include <vector>

namespace TFD::PayModel
{
    enum class PayContext : std::uint8_t
    {
        None = 0,
        PreCombat,
        InCombat,
        Bleedout,
        Rescue,
        TeammateContract
    };

    enum class EnemyType : std::uint8_t
    {
        Unknown = 0,
        Bandit,
        Forsworn,
        Necromancer,
        Mage,
        Mercenary,
        Soldier,
        Guard,
        Thalmor,
        Vampire,
        CreatureHumanoid
    };

    enum class ThreatTier : std::uint8_t
    {
        Weak = 0,
        Normal,
        Strong,
        Elite,
        BossTier
    };

    enum class DurabilityTier : std::uint8_t
    {
        Low = 0,
        Medium,
        High,
        VeryHigh
    };

    enum class RaceClass : std::uint8_t
    {
        Human = 0,
        Mer,
        Beastfolk,
        Orc,
        Unnatural
    };

    struct ActorBreakdown
    {
        std::uint32_t actorFormID{ 0 };
        std::string actorName{};

        EnemyType enemyType{ EnemyType::Unknown };
        ThreatTier threatTier{ ThreatTier::Normal };
        DurabilityTier durabilityTier{ DurabilityTier::Medium };
        RaceClass raceClass{ RaceClass::Human };

        bool isBossRef{ false };
        std::int32_t level{ 1 };
        std::int32_t playerLevel{ 1 };
        std::int32_t levelDelta{ 0 };

        int levelDeltaScore{ 0 };
        int absoluteLevelScore{ 0 };
        int threatScore{ 0 };
        int durabilityScore{ 0 };
        int raceScore{ 0 };
        int typeScore{ 0 };
        int bossScore{ 0 };

        int totalScore{ 0 };
        int goldValue{ 0 };
    };

    struct EncounterQuote
    {
        PayContext context{ PayContext::None };

        std::uint32_t speakerFormID{ 0 };
        std::string speakerName{};

        int actorCount{ 0 };
        int baseGold{ 0 };
        int contextPercent{ 0 };
        int totalGold{ 0 };

        std::vector<ActorBreakdown> actors;
    };

    void Install();
    void Shutdown();

    void ClearCachedEncounterQuote(const char* reason = "clear");
    bool PublishSharedGold(RE::Actor* speaker, PayContext context, const char* reason = "publish");
    bool ClearSharedGoldForContext(PayContext context, const char* reason = "clear_context");
    void ClearSharedGold(const char* reason = "clear");

    [[nodiscard]] int GetContextPercent(PayContext context);
    [[nodiscard]] int ApplyContextAdjustment(int baseGold, PayContext context);

    [[nodiscard]] EncounterQuote BuildEncounterQuote(RE::Actor* speaker, PayContext context);
    [[nodiscard]] int BuildEncounterQuoteGold(RE::Actor* speaker, PayContext context);

    bool PrimeEncounterQuote(RE::Actor* speaker, PayContext context);
    [[nodiscard]] int GetCachedEncounterQuote(RE::Actor* speaker = nullptr, PayContext context = PayContext::None);
    [[nodiscard]] const EncounterQuote* GetCachedEncounterQuoteData(RE::Actor* speaker = nullptr, PayContext context = PayContext::None);

    [[nodiscard]] EnemyType ClassifyEnemyType(RE::Actor* actor);
    [[nodiscard]] ThreatTier ClassifyThreatTier(RE::Actor* actor, RE::Actor* player = nullptr);
    [[nodiscard]] DurabilityTier ClassifyDurabilityTier(RE::Actor* actor);
    [[nodiscard]] RaceClass ClassifyRaceClass(RE::Actor* actor);
    [[nodiscard]] bool HasBossRefType(RE::Actor* actor);

    [[nodiscard]] ActorBreakdown BuildActorBreakdown(RE::Actor* actor, RE::Actor* player = nullptr);
    [[nodiscard]] int ScoreToGold(int totalScore);
}
