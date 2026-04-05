#pragma once

#include <cstdint>
#include <mutex>
#include <string_view>

namespace TFD::Flow
{
    enum class RootFlow : std::uint8_t
    {
        None = 0,
        PreCombat,
        InCombat,
        Captive,
        Victory
    };

    enum class DecisionGate : std::uint8_t
    {
        None = 0,
        PlayerBleedout,
        EnemyBleedout,
        Truce
    };

    enum class CaptiveMode : std::uint8_t
    {
        None = 0,
        Kidnapped,
        Surrendered,
        JoinedEnemy
    };

    enum class SubFlow : std::uint8_t
    {
        None = 0,

        PreCombatPleasure,
        PreCombatAfterPleasure,
        InCombatPleasure,
        InCombatAfterPleasure,
        VictoryPleasure,
        VictoryAfterPleasure,

        CaptiveIdle,
        CaptivePleasure,
        CaptiveAfterPleasure,
        WorkForEnemy,
        EscapeAttempt,
        EscapeFailed,
        Recapture,
        JoinedEnemyIdle
    };

    enum class PreCombatOutcome : std::uint8_t
    {
        None = 0,
        Pay,
        Pleasure,
        Fight,
        Captive,
        JoinEnemy,
        Cancel,
        Failed
    };

    enum class BleedoutOutcome : std::uint8_t
    {
        None = 0,
        Pay,
        Pleasure,
        Captive,
        Cancel,
        Failed
    };

    enum class VictoryOutcome : std::uint8_t
    {
        None = 0,
        RecruitEnemy,
        KillEnemy,
        Pleasure,
        Cancel,
        Failed
    };

    enum class CaptiveOutcome : std::uint8_t
    {
        None = 0,
        Pleasure,
        WorkForEnemy,
        EscapeStarted,
        EscapeSucceeded,
        EscapeFailed,
        Recaptured,
        Release,
        Cancel,
        Failed
    };

    struct Snapshot
    {
        RootFlow root{ RootFlow::None };
        RootFlow contextRoot{ RootFlow::None };
        DecisionGate gate{ DecisionGate::None };
        CaptiveMode captiveMode{ CaptiveMode::None };
        SubFlow sub{ SubFlow::None };
        std::uint32_t token{ 0 };
        std::uint32_t primaryActorFormID{ 0 };
        bool terminalResolved{ false };
        bool locked{ false };
    };

    class Controller
    {
    public:
        static Controller& GetSingleton();

        Snapshot GetSnapshot() const;

        void ResetRuntime(std::string_view reason);
        void ResetForLoad(std::string_view reason);

        bool BeginPreCombat(std::uint32_t actorFormID, std::string_view reason);
        bool BeginInCombat(std::uint32_t actorFormID, std::string_view reason);
        bool BeginCaptive(std::uint32_t actorFormID, CaptiveMode mode, std::string_view reason);
        bool BeginVictory(std::uint32_t actorFormID, std::string_view reason);

        bool BeginTruceDecision(std::uint32_t actorFormID, std::string_view reason);
        bool BeginPlayerBleedoutDecision(std::uint32_t actorFormID, std::string_view reason);
        bool BeginEnemyBleedoutDecision(std::uint32_t actorFormID, std::string_view reason);

        bool ResolvePreCombatOutcome(PreCombatOutcome outcome, std::uint32_t actorFormID, std::string_view reason);
        bool ResolveBleedoutOutcome(BleedoutOutcome outcome, std::uint32_t actorFormID, std::string_view reason);
        bool ResolveVictoryOutcome(VictoryOutcome outcome, std::uint32_t actorFormID, std::string_view reason);
        bool ResolveCaptiveOutcome(CaptiveOutcome outcome, std::uint32_t actorFormID, std::string_view reason);

        bool BeginAfterPleasure(std::uint32_t actorFormID, std::string_view reason);
        bool CompleteAfterPleasure(std::string_view reason);
        bool CompleteTerminalContext(std::string_view reason);

        void NotifyCombatStarted(std::uint32_t actorFormID, std::string_view reason);
        void NotifyCombatEnded(std::string_view reason);
        void NotifyPlayerBleedout(std::uint32_t actorFormID, std::string_view reason);
        void NotifyEnemyBleedout(std::uint32_t actorFormID, std::string_view reason);

        bool CanStartPreCombat() const;
        bool CanStartInCombatTruce() const;
        bool CanEnterCaptive() const;
        bool CanEnterVictory() const;
        bool IsCaptiveContext() const;
        bool IsJoinedEnemyMode() const;
        bool IsBleedDecisionActive() const;

        static const char* ToString(RootFlow value);
        static const char* ToString(DecisionGate value);
        static const char* ToString(CaptiveMode value);
        static const char* ToString(SubFlow value);
        static const char* ToString(PreCombatOutcome value);
        static const char* ToString(BleedoutOutcome value);
        static const char* ToString(VictoryOutcome value);
        static const char* ToString(CaptiveOutcome value);

    private:
        Controller() = default;

        bool BeginRootLocked(RootFlow next, std::uint32_t actorFormID, std::string_view reason);
        bool TransitionRootLocked(RootFlow next, std::uint32_t actorFormID, std::string_view reason);
        bool BeginCaptiveLocked(std::uint32_t actorFormID, CaptiveMode mode, std::string_view reason);
        bool EnterTerminalContextLocked(RootFlow contextRoot, SubFlow sub, std::uint32_t actorFormID, std::string_view reason);
        bool CompleteTerminalContextLocked(std::string_view reason);
        bool RejectLocked(std::string_view op, std::string_view reason) const;
        void SetPrimaryActorLocked(std::uint32_t actorFormID);
        void ClearDecisionLocked();
        void ClearSubLocked();
        void BumpTokenLocked();
        void ClearTerminalLocked();
        void ClearAllLocked();
        void SetCaptiveIdleLocked();
        void RefreshFlowGlobalsLocked();

        mutable std::mutex _lock;
        Snapshot _snapshot{};
        bool _combatActive{ false };
    };
}
