#include "TFDVictory.h"

#include "TFDActor.h"
#include "TFDFlowController.h"
#include "TFDRecruit.h"
#include "TFDSettings.h"
#include "TFDTeammateManager.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace TFD::Victory
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr std::uint8_t kEnemyLockKindValue = 2;
        constexpr double kEnemyKnockSeconds = 10.0;
        constexpr double kDefeatedReentrySuppressSeconds = 6.0;
        constexpr auto kDialogueOpenTimeout = std::chrono::milliseconds(5000);
        constexpr auto kChoiceCommitGrace = std::chrono::milliseconds(6000);
        constexpr auto kPleasureDialogueCloseGuard = std::chrono::milliseconds(25000);
        constexpr auto kPleasureDialogueCloseRetryDelay = std::chrono::milliseconds(150);
        constexpr auto kPleasureReturnDelayedBleedoutStartDelay = std::chrono::milliseconds(3600);
        constexpr auto kPleasureReturnVisualGraphSafeInitialDelay = kPleasureReturnDelayedBleedoutStartDelay;
        constexpr auto kPleasureReturnBleedoutReassertInitialDelay = kPleasureReturnDelayedBleedoutStartDelay;
        constexpr auto kPleasureReturnBleedoutReassertInterval = std::chrono::milliseconds(450);
        constexpr auto kPleasureReturnBleedoutReassertStopBeforeTimeout = std::chrono::milliseconds(250);
        constexpr std::uint8_t kPleasureReturnBleedoutReassertMaxAttempts = 40;
        constexpr std::uint8_t kPleasureReturnBleedoutDialogueStopMinAttempts = 3;
        constexpr float kPleasureReturnBleedoutHealthBufferPct = 2.0f;
        constexpr float kPleasureReturnBleedoutMinPct = 5.0f;
        constexpr float kPleasureReturnBleedoutMaxPct = 35.0f;
        constexpr float kPleasureReturnBleedoutMinAbsHp = 1.0f;
        constexpr auto kKillPrimeDelay = std::chrono::milliseconds(100);
        constexpr auto kKillDamageDelay = std::chrono::milliseconds(450);
        constexpr auto kKillDeathSettleDelay = std::chrono::milliseconds(300);
        constexpr auto kKillDeathConfirmDelay = std::chrono::milliseconds(1500);
        constexpr auto kKillEngineConfirmDelay = std::chrono::milliseconds(1000);
        constexpr auto kLootDialogueCloseTimeout = std::chrono::milliseconds(10000);
        constexpr auto kLootInventoryDispatchDelay = std::chrono::milliseconds(100);
        constexpr auto kLootInventoryDispatchRetryDelay = std::chrono::milliseconds(250);
        constexpr auto kLootInventoryOpenTimeout = std::chrono::milliseconds(7000);
        constexpr std::uint8_t kLootInventoryDispatchMaxAttempts = 3;
        constexpr auto kLootReleasePrepareDelay = std::chrono::milliseconds(100);
        constexpr auto kLootGraphStepDelay = std::chrono::milliseconds(150);
        constexpr auto kLootGetUpMinimumSettle = std::chrono::milliseconds(1800);
        constexpr auto kLootGetUpMaximumSettle = std::chrono::milliseconds(4500);
        constexpr double kLootReentrySuppressSeconds = 3.0;
        constexpr float kLootHealthBonusPct = 8.0f;
        constexpr auto kRecruitDialogueCloseTimeout = std::chrono::milliseconds(10000);
        constexpr auto kRecruitCommitDelay = std::chrono::milliseconds(350);
        constexpr auto kRecruitDefeatedClearDelay = std::chrono::milliseconds(250);
        constexpr auto kRecruitPostClearCommitDelay = std::chrono::milliseconds(0);
        constexpr auto kRecruitDeferredPackageDelay = std::chrono::milliseconds(1000);
        constexpr auto kRecruitGraphStepDelay = std::chrono::milliseconds(150);
        constexpr auto kRecruitBleedoutExitMinimumDelay = std::chrono::milliseconds(900);
        constexpr auto kRecruitBleedoutExitCheckDelay = std::chrono::milliseconds(250);
        constexpr auto kRecruitBleedoutExitMaximumWait = std::chrono::milliseconds(4000);
        constexpr auto kRecruitHitReactStartDelay = std::chrono::milliseconds(150);
        constexpr auto kRecruitHitReactStopDelay = std::chrono::milliseconds(350);
        constexpr auto kRecruitHitReactSettleDelay = std::chrono::milliseconds(1200);
        constexpr auto kRecruitHitReactMaximumSettle = std::chrono::milliseconds(2500);
        constexpr auto kRecruitHitDiagnosticWindow = std::chrono::seconds(60);
        constexpr auto kRecruitHitDiagnosticFirstDelay = std::chrono::milliseconds(250);
        constexpr auto kRecruitHitDiagnosticSecondDelay = std::chrono::milliseconds(1000);
        constexpr auto kRecruitGetUpMinimumSettle = std::chrono::milliseconds(4500);
        constexpr auto kRecruitGetUpMaximumSettle = std::chrono::milliseconds(6500);
        constexpr double kRecruitReentrySuppressSeconds = 12.0;
        constexpr double kRecruitCommitPendingSeconds = 15.0;
        constexpr float kRecruitHealthBonusPct = 8.0f;
        constexpr bool kEnemyDefeatedVisualBleedoutEnabled = true;
        constexpr bool kEnemySoftEnterHardStateDelayEnabled = true;
        constexpr auto kEnemySoftEnterHardStateDelay = std::chrono::milliseconds(900);
        constexpr auto kEnemyVisualGraphSafeInitialDelay = std::chrono::milliseconds(320);
        constexpr auto kEnemyVisualGraphSafeQuietWindow = std::chrono::milliseconds(350);
        constexpr auto kEnemyVisualGraphSafeRetryDelay = std::chrono::milliseconds(120);
        constexpr auto kEnemyVisualGraphSafeMaxDelay = std::chrono::milliseconds(950);
        constexpr auto kEnemyVisualFirstHardeningSettleDelay = std::chrono::milliseconds(450);
        constexpr auto kEnemyVisualFirstRetryDelay = std::chrono::milliseconds(250);
        constexpr std::uint8_t kEnemyVisualFirstMaxAttempts = 2;
        constexpr auto kEnemySoftEnterPressureQuietWindow = std::chrono::milliseconds(900);
        constexpr auto kEnemySoftEnterPressureRetryDelay = std::chrono::milliseconds(500);
        constexpr auto kEnemySoftEnterPressureMaxExtraDelay = std::chrono::milliseconds(4000);
        constexpr auto kEnemyNpcCombatHardeningSettleDelay = std::chrono::milliseconds(2200);
        constexpr float kEnemySoftEnterPressureScanRadius = 4096.0f;
        constexpr RE::FormID kVictoryGreetInfoLocalFormID = 0x00195937;
        constexpr std::string_view kPluginName{ "TFDEngine.esp" };

        enum class KillStage : std::uint8_t
        {
            None = 0,
            WaitingForDialogueClose,
            PrimePending,
            DamagePending,
            AwaitingDeath,
            AwaitingEngineDeath
        };

        enum class LootStage : std::uint8_t
        {
            None = 0,
            WaitingForDialogueClose,
            InventoryDispatchPending,
            WaitingForInventoryOpen,
            InventoryOpen,
            ReleasePending,
            BleedoutStopPending,
            GetUpPending,
            PackageRefreshPending
        };

        enum class RecruitStage : std::uint8_t
        {
            None = 0,
            WaitingForDialogueClose,
            CommitPending,
            BleedoutStopPending,
            BleedoutExitPending,
            HitReactStartPending,
            HitReactStopPending,
            GetUpPending,
            DefeatedClearPending,
            RecruitCommitPending,
            PackageRefreshPending
        };

        struct EnemyEntry
        {
            RE::ActorHandle handle{};
            float thresholdPct{ 0.0f };

            float savedHealRate{ 0.0f };
            float savedHealRateMult{ 100.0f };
            float savedCombatHealRateMult{ 1.0f };
            bool regenOverridden{ false };

            float savedAggression{ 1.0f };
            bool aggressionOverridden{ false };

            bool managed{ true };
            bool factionApplied{ false };
            bool autoDeathIssued{ false };
            bool fatalDamageApplied{ false };
            int aliasSlot{ -1 };
            Clock::time_point deadline{};
            bool countdownHeld{ false };
            std::uint32_t heldSessionID{ 0 };
            bool initialPackageRefreshDone{ false };
            bool packageRefreshAfterHardeningPending{ false };
            bool visualBleedoutStarted{ false };
            bool visualBleedoutStopSent{ false };
            bool softEnterActive{ false };
            bool softEnterHardStateApplied{ false };
            bool softEnterPendingLogged{ false };
            Clock::time_point softEnterHardStateDue{};
            Clock::time_point softEnterHardStateMaxDue{};
            Clock::time_point softEnterLastPressureSeen{};
            RE::FormID softEnterLastPressureCauseFormID{ 0 };
            RE::FormID softEnterLastPressureOtherFormID{ 0 };
            std::uint16_t softEnterPressureHitCount{ 0 };
            std::uint8_t softEnterPressureDeferrals{ 0 };
            bool npcCombatDefeat{ false };
            bool npcCombatHardeningSettleLogged{ false };
            bool pleasureReturnVisualHoldActive{ false };
            bool pleasureReturnVisualHoldLogged{ false };
            bool pleasureReturnReassertActive{ false };
            bool pleasureReturnPackageHoldActive{ false };
            std::uint8_t pleasureReturnReassertAttempts{ 0 };
            Clock::time_point pleasureReturnReassertNextDue{};
            Clock::time_point pleasureReturnReassertUntil{};
            bool visualBleedoutStartPending{ false };
            bool visualBleedoutStartDecisionLogged{ false };
            std::uint8_t visualBleedoutStartAttempts{ 0 };
            std::uint8_t visualBleedoutGraphSafeDeferrals{ 0 };
            Clock::time_point visualBleedoutStartDue{};
            Clock::time_point visualBleedoutStartMaxDue{};
            Clock::time_point visualBleedoutGraphSafeLastHoldLog{};
            RE::FormID visualBleedoutLastUnsafeTargetFormID{ 0 };
        };

        struct LootTransitionState
        {
            RE::ActorHandle handle{};
            bool wasBleedingOut{ false };
        };

        struct RecruitTransitionState
        {
            RE::ActorHandle handle{};
            bool visualBleedoutOwned{ false };
            bool bleedoutStopSent{ false };
            bool bleedoutExitObservedBeforeGetUp{ false };
            bool actorBleedingBeforeGetUp{ false };
            bool getUpStartSent{ false };
            bool hitReactStartSent{ false };
            bool hitReactFallbackStartSent{ false };
            bool hitReactStopSent{ false };
            bool hitReactFallbackStopSent{ false };
            bool getUpForcedAfterExitTimeout{ false };
            std::uint16_t bleedoutExitWaitTicks{ 0 };
            bool defeatedOwnershipClearedBeforeGetUp{ false };
            bool aliasFactionClearedBeforeGetUp{ false };
            bool regenRestoredBeforeGetUp{ false };
            bool passiveHeldThroughGetUp{ false };
            bool passiveRestoredOnFailure{ false };
            float savedAggression{ 1.0f };
            bool aggressionOverridden{ false };
            bool preGetUpCommitAttempted{ false };
            bool preGetUpCommitSkipped{ false };
            bool preGetUpRawHostileBefore{ false };
            bool preGetUpRawHostileAfter{ false };
            bool commitAttempted{ false };
            bool commitSkipped{ false };
            bool rawHostileAfter{ false };
            bool teammateRegistered{ false };
        };

        struct RecruitHitDiagnosticState
        {
            RE::ActorHandle handle{};
            RE::FormID actorFormID{ 0 };
            RE::FormID causeFormID{ 0 };
            std::uint32_t recruitSessionID{ 0 };
            bool active{ false };
            bool playerHitObserved{ false };
            bool firstAfterHitLogged{ false };
            bool secondAfterHitLogged{ false };
            Clock::time_point armedAt{};
            Clock::time_point expireAt{};
            Clock::time_point firstAfterHitDue{};
            Clock::time_point secondAfterHitDue{};
            RE::NiPoint3 armPosition{};
            RE::NiPoint3 hitPosition{};
        };

        std::atomic_bool g_installed{ false };
        std::atomic_bool g_running{ false };
        std::atomic_bool g_loadTransition{ false };
        std::atomic_flag g_tickPending = ATOMIC_FLAG_INIT;
        std::thread g_worker{};
        std::recursive_mutex g_lock;
        std::unordered_map<RE::FormID, EnemyEntry> g_enemyEntries{};
        Clock::time_point g_lastLifecycleLog{};

        SessionSnapshot g_session{};
        Clock::time_point g_sessionOpenDeadline{};
        Clock::time_point g_choiceCommitDeadline{};
        Clock::time_point g_killDeadline{};
        Clock::time_point g_killFinalizeNotBefore{};
        KillStage g_killStage{ KillStage::None };
        Clock::time_point g_lootDeadline{};
        Clock::time_point g_lootFinalizeDeadline{};
        LootStage g_lootStage{ LootStage::None };
        LootTransitionState g_lootTransition{};
        std::uint8_t g_lootInventoryDispatchAttempts{ 0 };
        Clock::time_point g_recruitDeadline{};
        Clock::time_point g_recruitFinalizeDeadline{};
        RecruitStage g_recruitStage{ RecruitStage::None };
        RecruitTransitionState g_recruitTransition{};
        RecruitHitDiagnosticState g_recruitHitDiagnostic{};
        std::uint32_t g_nextSessionID{ 1 };
        RE::TESGlobal* g_conditionState = nullptr;
        RE::TESTopicInfo* g_victoryGreetInfo = nullptr;
        Clock::time_point g_pleasureDialogueCloseGuardUntil{};
        Clock::time_point g_pleasureDialogueCloseLastQueued{};
        std::uint32_t g_pleasureDialogueCloseGuardSessionID{ 0 };
        RE::ActorHandle g_pleasureDialogueCloseGuardHandle{};

        Clock::time_point Now()
        {
            return Clock::now();
        }

        std::string ReasonText(std::string_view reason)
        {
            return reason.empty() ? std::string{ "-" } : std::string{ reason };
        }

        const char* ToString(KillStage stage)
        {
            switch (stage) {
            case KillStage::None:
                return "None";
            case KillStage::WaitingForDialogueClose:
                return "WaitingForDialogueClose";
            case KillStage::PrimePending:
                return "PrimePending";
            case KillStage::DamagePending:
                return "DamagePending";
            case KillStage::AwaitingDeath:
                return "AwaitingDeath";
            case KillStage::AwaitingEngineDeath:
                return "AwaitingEngineDeath";
            default:
                return "Unknown";
            }
        }

        const char* ToString(LootStage stage)
        {
            switch (stage) {
            case LootStage::None:
                return "None";
            case LootStage::WaitingForDialogueClose:
                return "WaitingForDialogueClose";
            case LootStage::InventoryDispatchPending:
                return "InventoryDispatchPending";
            case LootStage::WaitingForInventoryOpen:
                return "WaitingForInventoryOpen";
            case LootStage::InventoryOpen:
                return "InventoryOpen";
            case LootStage::ReleasePending:
                return "ReleasePending";
            case LootStage::BleedoutStopPending:
                return "BleedoutStopPending";
            case LootStage::GetUpPending:
                return "GetUpPending";
            case LootStage::PackageRefreshPending:
                return "PackageRefreshPending";
            default:
                return "Unknown";
            }
        }

        const char* ToString(RecruitStage stage)
        {
            switch (stage) {
            case RecruitStage::None:
                return "None";
            case RecruitStage::WaitingForDialogueClose:
                return "WaitingForDialogueClose";
            case RecruitStage::CommitPending:
                return "CommitPending";
            case RecruitStage::BleedoutStopPending:
                return "BleedoutStopPending";
            case RecruitStage::BleedoutExitPending:
                return "BleedoutExitPending";
            case RecruitStage::HitReactStartPending:
                return "HitReactStartPending";
            case RecruitStage::HitReactStopPending:
                return "HitReactStopPending";
            case RecruitStage::GetUpPending:
                return "GetUpPending";
            case RecruitStage::DefeatedClearPending:
                return "DefeatedClearPending";
            case RecruitStage::RecruitCommitPending:
                return "RecruitCommitPending";
            case RecruitStage::PackageRefreshPending:
                return "PackageRefreshPending";
            default:
                return "Unknown";
            }
        }

        bool PapyrusRequestKill(RE::StaticFunctionTag*, RE::Actor* speaker)
        {
            return RequestKill(speaker, "papyrus_victory_kill_fragment");
        }

        bool PapyrusRequestLoot(RE::StaticFunctionTag*, RE::Actor* speaker)
        {
            return RequestLoot(speaker, "papyrus_victory_loot_fragment");
        }

        bool PapyrusRequestRecruit(RE::StaticFunctionTag*, RE::Actor* speaker)
        {
            return RequestRecruit(speaker, "papyrus_victory_recruit_fragment");
        }

        bool PapyrusRequestPleasure(RE::StaticFunctionTag*, RE::Actor* speaker)
        {
            return RequestPleasure(speaker, "papyrus_victory_pleasure_fragment");
        }

        bool PapyrusCompletePleasureHandoff(RE::StaticFunctionTag*, RE::Actor* speaker, bool started)
        {
            return CompletePleasureHandoff(speaker, started, started ? "papyrus_victory_pleasure_started" : "papyrus_victory_pleasure_start_failed");
        }

        bool PapyrusCompletePleasureScene(RE::StaticFunctionTag*, RE::Actor* speaker, bool sceneSucceeded)
        {
            return CompletePleasureScene(speaker, sceneSucceeded, sceneSucceeded ? "papyrus_victory_pleasure_scene_succeeded" : "papyrus_victory_pleasure_scene_failed");
        }

        void SetConditionState(int value)
        {
            if (!g_conditionState) {
                g_conditionState = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDVictoryState");
            }
            if (g_conditionState) {
                g_conditionState->value = static_cast<float>(value);
            }
        }

        void RefreshConditionStateLocked()
        {
            int value = 0;
            if (g_session.active) {
                value = 2;
            }
            else {
                for (const auto& [_formID, entry] : g_enemyEntries) {
                    if (entry.managed) {
                        value = 1;
                        break;
                    }
                }
            }
            SetConditionState(value);
        }

        float GetActorHealthPct(RE::Actor* actor)
        {
            if (!actor) {
                return 0.0f;
            }
            const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
            const float hpNow = (std::max)(0.0f, actor->GetActorValue(RE::ActorValue::kHealth));
            return (hpNow / hpMax) * 100.0f;
        }

        float DistanceOrZero(const RE::NiPoint3& a, const RE::NiPoint3& b)
        {
            return a.GetDistance(b);
        }

        void LogRecruitHitDiagnosticSampleLocked(
            RE::Actor* actor,
            RE::Actor* cause,
            const char* phase,
            std::string_view reason)
        {
            const auto reasonText = ReasonText(reason);
            const auto* actorState = actor ? actor->AsActorState() : nullptr;
            const bool bleeding = actorState && actorState->IsBleedingOut();
            const bool dead = actor && actor->IsDead();
            const bool disabled = actor && actor->IsDisabled();
            const bool loaded = actor && actor->Is3DLoaded();
            const bool inCombat = actor && actor->IsInCombat();
            const bool playerTeammate = actor && actor->IsPlayerTeammate();
            const bool activeFollower = actor && TFD::TeammateManager::IsActiveFollowerActor(actor);
            const bool playerSide = actor && TFD::TeammateManager::IsPlayerSideTeammateActor(actor);
            const bool tfdManaged = actor && TFD::TeammateManager::IsTFDManagedTeammateActor(actor);
            const bool rawHostile = actor && TFD::Recruit::IsRawHostileToPlayer(actor, RE::PlayerCharacter::GetSingleton());
            const float hp = actor ? actor->GetActorValue(RE::ActorValue::kHealth) : 0.0f;
            const float hpPct = actor ? GetActorHealthPct(actor) : 0.0f;
            const auto pos = actor ? actor->GetPosition() : RE::NiPoint3{};
            const float distFromArm = actor && g_recruitHitDiagnostic.active ?
                DistanceOrZero(pos, g_recruitHitDiagnostic.armPosition) :
                0.0f;
            const float distFromHit = actor && g_recruitHitDiagnostic.playerHitObserved ?
                DistanceOrZero(pos, g_recruitHitDiagnostic.hitPosition) :
                0.0f;

            spdlog::info(
                "[TFD][Victory][R421A] Recruit hit diagnostic sample phase={} actor={:08X} session={} cause={:08X} reason={} dead={} disabled={} loaded={} bleeding={} inCombat={} hp={:.2f} hpPct={:.1f} playerTeammate={} activeFollower={} playerSide={} tfdManaged={} rawHostile={} pos=({:.1f},{:.1f},{:.1f}) distFromArm={:.1f} distFromHit={:.1f} noBehaviorChange=1",
                phase ? phase : "unknown",
                actor ? actor->GetFormID() : g_recruitHitDiagnostic.actorFormID,
                g_recruitHitDiagnostic.recruitSessionID,
                cause ? cause->GetFormID() : g_recruitHitDiagnostic.causeFormID,
                reasonText,
                dead ? 1 : 0,
                disabled ? 1 : 0,
                loaded ? 1 : 0,
                bleeding ? 1 : 0,
                inCombat ? 1 : 0,
                hp,
                hpPct,
                playerTeammate ? 1 : 0,
                activeFollower ? 1 : 0,
                playerSide ? 1 : 0,
                tfdManaged ? 1 : 0,
                rawHostile ? 1 : 0,
                pos.x,
                pos.y,
                pos.z,
                distFromArm,
                distFromHit);
        }

        void ArmRecruitHitDiagnosticLocked(RE::Actor* actor, std::uint32_t sessionID, std::string_view reason)
        {
            if (!actor || actor->IsDead() || actor->IsDisabled()) {
                g_recruitHitDiagnostic = RecruitHitDiagnosticState{};
                return;
            }

            const auto now = Now();
            g_recruitHitDiagnostic = RecruitHitDiagnosticState{};
            g_recruitHitDiagnostic.handle = actor->GetHandle();
            g_recruitHitDiagnostic.actorFormID = actor->GetFormID();
            g_recruitHitDiagnostic.recruitSessionID = sessionID;
            g_recruitHitDiagnostic.active = true;
            g_recruitHitDiagnostic.armedAt = now;
            g_recruitHitDiagnostic.expireAt = now + kRecruitHitDiagnosticWindow;
            g_recruitHitDiagnostic.armPosition = actor->GetPosition();

            spdlog::info(
                "[TFD][Victory][R421A] Recruit hit diagnostic armed actor={:08X} session={} windowMs={} reason={} watch=player_hit_after_recruit noBehaviorChange=1",
                actor->GetFormID(),
                sessionID,
                std::chrono::duration_cast<std::chrono::milliseconds>(kRecruitHitDiagnosticWindow).count(),
                ReasonText(reason));
            LogRecruitHitDiagnosticSampleLocked(actor, nullptr, "armed_after_recruit_finalize", reason);
        }

        void TickRecruitHitDiagnosticLocked(Clock::time_point now)
        {
            if (!g_recruitHitDiagnostic.active) {
                return;
            }

            auto actorSP = g_recruitHitDiagnostic.handle.get();
            auto* actor = actorSP.get();
            if (!actor || actor->IsDead() || actor->IsDisabled()) {
                spdlog::warn(
                    "[TFD][Victory][R421A] Recruit hit diagnostic stopped actor={:08X} session={} reason=actor_invalid dead={} disabled={} noBehaviorChange=1",
                    g_recruitHitDiagnostic.actorFormID,
                    g_recruitHitDiagnostic.recruitSessionID,
                    actor && actor->IsDead() ? 1 : 0,
                    actor && actor->IsDisabled() ? 1 : 0);
                g_recruitHitDiagnostic = RecruitHitDiagnosticState{};
                return;
            }

            if (g_recruitHitDiagnostic.playerHitObserved) {
                if (!g_recruitHitDiagnostic.firstAfterHitLogged &&
                    g_recruitHitDiagnostic.firstAfterHitDue.time_since_epoch().count() != 0 &&
                    now >= g_recruitHitDiagnostic.firstAfterHitDue) {
                    g_recruitHitDiagnostic.firstAfterHitLogged = true;
                    LogRecruitHitDiagnosticSampleLocked(actor, nullptr, "after_player_hit_250ms", "victory_recruit_player_hit_diagnostic");
                }

                if (!g_recruitHitDiagnostic.secondAfterHitLogged &&
                    g_recruitHitDiagnostic.secondAfterHitDue.time_since_epoch().count() != 0 &&
                    now >= g_recruitHitDiagnostic.secondAfterHitDue) {
                    g_recruitHitDiagnostic.secondAfterHitLogged = true;
                    LogRecruitHitDiagnosticSampleLocked(actor, nullptr, "after_player_hit_1000ms", "victory_recruit_player_hit_diagnostic");
                    spdlog::info(
                        "[TFD][Victory][R421A] Recruit hit diagnostic completed actor={:08X} session={} result=first_player_hit_observed noBehaviorChange=1",
                        g_recruitHitDiagnostic.actorFormID,
                        g_recruitHitDiagnostic.recruitSessionID);
                    g_recruitHitDiagnostic = RecruitHitDiagnosticState{};
                    return;
                }
            }

            if (g_recruitHitDiagnostic.expireAt.time_since_epoch().count() != 0 &&
                now >= g_recruitHitDiagnostic.expireAt) {
                spdlog::info(
                    "[TFD][Victory][R421A] Recruit hit diagnostic expired actor={:08X} session={} playerHitObserved={} windowMs={} noBehaviorChange=1",
                    g_recruitHitDiagnostic.actorFormID,
                    g_recruitHitDiagnostic.recruitSessionID,
                    g_recruitHitDiagnostic.playerHitObserved ? 1 : 0,
                    std::chrono::duration_cast<std::chrono::milliseconds>(kRecruitHitDiagnosticWindow).count());
                g_recruitHitDiagnostic = RecruitHitDiagnosticState{};
            }
        }

        bool IsUsableManualCandidate(RE::Actor* actor)
        {
            if (!actor) {
                return false;
            }
            if (actor->IsDisabled() || actor->IsDead()) {
                return false;
            }
            return true;
        }

        RE::TESTopicInfo* ResolveVictoryGreetTopicInfo()
        {
            if (!g_victoryGreetInfo) {
                if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
                    g_victoryGreetInfo = dataHandler->LookupForm<RE::TESTopicInfo>(
                        kVictoryGreetInfoLocalFormID,
                        kPluginName);
                }

                if (g_victoryGreetInfo) {
                    spdlog::info(
                        "[TFD][Victory][R394A] Victory Greet INFO resolved form={:08X} local={:06X}",
                        g_victoryGreetInfo->GetFormID(),
                        kVictoryGreetInfoLocalFormID);
                }
                else {
                    spdlog::warn(
                        "[TFD][Victory][R394A] Victory Greet INFO missing local={:06X} plugin={}",
                        kVictoryGreetInfoLocalFormID,
                        kPluginName);
                }
            }
            return g_victoryGreetInfo;
        }

        bool IsDialogueMenuOpen()
        {
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
        }

        bool QueueDialogueMenuHide(bool requestedSyncProcess)
        {
            auto* queue = RE::UIMessageQueue::GetSingleton();
            auto* strings = RE::InterfaceStrings::GetSingleton();
            if (!queue || !strings) {
                return false;
            }

            // P36A: do not call UIMessageQueue::ProcessCommands from the Victory
            // dialogue fragment path.  Crash logs 202/203 both die in HUD/crosshair
            // update immediately after the synchronous dialogue hide on cycle two.
            // Queue the hide and let the normal UI pump process it safely.
            (void)requestedSyncProcess;
            queue->AddMessage(strings->dialogueMenu, RE::UI_MESSAGE_TYPE::kHide, nullptr);
            return true;
        }

        bool CloseVictoryDialogueMenu(RE::Actor* actor, std::string_view reason, bool processNow)
        {
            const bool wasOpen = IsDialogueMenuOpen();
            if (actor && !actor->IsDisabled() && !actor->IsDead()) {
                actor->SetDialogueWithPlayer(false, false, nullptr);
                actor->AllowPCDialogue(false);
            }

            const bool hideQueued = QueueDialogueMenuHide(processNow);
            spdlog::info(
                "[TFD][Victory][P36A] Victory dialogue close requested actor={:08X} reason={} wasOpen={} hideQueued={} requestedProcessNow={} processedNow=0 pcDialogueDisabled=1",
                actor ? actor->GetFormID() : 0u,
                ReasonText(reason),
                wasOpen ? 1 : 0,
                hideQueued ? 1 : 0,
                processNow ? 1 : 0);
            return wasOpen || hideQueued;
        }

        void ClearPleasureDialogueCloseGuardLocked(std::string_view reason)
        {
            if (g_pleasureDialogueCloseGuardSessionID != 0) {
                spdlog::info(
                    "[TFD][Victory][P36A] Pleasure dialogue close guard cleared session={} reason={}",
                    g_pleasureDialogueCloseGuardSessionID,
                    ReasonText(reason));
            }
            g_pleasureDialogueCloseGuardUntil = {};
            g_pleasureDialogueCloseLastQueued = {};
            g_pleasureDialogueCloseGuardSessionID = 0;
            g_pleasureDialogueCloseGuardHandle = RE::ActorHandle{};
        }

        void ArmPleasureDialogueCloseGuardLocked(RE::Actor* actor, std::uint32_t sessionID, Clock::time_point now, std::string_view reason)
        {
            if (!actor || sessionID == 0) {
                return;
            }

            g_pleasureDialogueCloseGuardUntil = now + kPleasureDialogueCloseGuard;
            g_pleasureDialogueCloseLastQueued = {};
            g_pleasureDialogueCloseGuardSessionID = sessionID;
            g_pleasureDialogueCloseGuardHandle = actor->GetHandle();
            spdlog::info(
                "[TFD][Victory][P36A] Pleasure dialogue close guard armed actor={:08X} session={} durationMs={} reason={}",
                actor->GetFormID(),
                sessionID,
                kPleasureDialogueCloseGuard.count(),
                ReasonText(reason));
        }

        void MaintainPleasureDialogueCloseGuardLocked(Clock::time_point now)
        {
            if (g_pleasureDialogueCloseGuardSessionID == 0) {
                return;
            }

            const bool valid =
                g_session.active &&
                g_session.sessionID == g_pleasureDialogueCloseGuardSessionID &&
                g_session.phase == SessionPhase::PleasureCommitted &&
                now < g_pleasureDialogueCloseGuardUntil;

            if (!valid) {
                ClearPleasureDialogueCloseGuardLocked("guard_not_valid");
                return;
            }

            if (!IsDialogueMenuOpen()) {
                return;
            }

            if (g_pleasureDialogueCloseLastQueued.time_since_epoch().count() != 0 &&
                now < g_pleasureDialogueCloseLastQueued + kPleasureDialogueCloseRetryDelay) {
                return;
            }

            g_pleasureDialogueCloseLastQueued = now;
            auto actorSP = g_pleasureDialogueCloseGuardHandle.get();
            auto* actor = actorSP.get();
            if (actor && !actor->IsDisabled() && !actor->IsDead()) {
                actor->SetDialogueWithPlayer(false, false, nullptr);
                actor->AllowPCDialogue(false);
            }
            const bool hideQueued = QueueDialogueMenuHide(false);
            spdlog::info(
                "[TFD][Victory][P36A] Pleasure dialogue reopen suppressed actor={:08X} session={} hideQueued={} retryMs={} pcDialogueDisabled=1",
                actor ? actor->GetFormID() : g_session.selectedActorFormID,
                g_session.sessionID,
                hideQueued ? 1 : 0,
                kPleasureDialogueCloseRetryDelay.count());
        }

        RE::FormID GetOpenLootTargetActorFormID()
        {
            auto* ui = RE::UI::GetSingleton();
            if (!ui || !ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME)) {
                return 0;
            }

            auto targetSP = RE::Actor::LookupByHandle(RE::ContainerMenu::GetTargetRefHandle());
            auto* target = targetSP.get();
            return target ? target->GetFormID() : 0;
        }

        bool IsSelectedLootContainerOpen(RE::FormID expectedActorFormID)
        {
            return expectedActorFormID != 0 &&
                   GetOpenLootTargetActorFormID() == expectedActorFormID;
        }

        std::uint32_t NextSessionIDLocked()
        {
            auto value = g_nextSessionID++;
            if (value == 0) {
                value = g_nextSessionID++;
            }
            return value;
        }

        struct ThreatGateResult
        {
            bool blocked{ false };
            RE::FormID actorFormID{ 0 };
            float distance{ 0.0f };
        };

        ThreatGateResult FindBlockingPlayerThreat(RE::Actor* selectedActor)
        {
            ThreatGateResult result{};
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                result.blocked = true;
                return result;
            }

            std::unordered_set<RE::FormID> defeatedIDs{};
            {
                std::scoped_lock lk(g_lock);
                defeatedIDs.reserve(g_enemyEntries.size());
                for (const auto& [formID, _entry] : g_enemyEntries) {
                    defeatedIDs.insert(formID);
                }
            }

            TFD::Actor::ScanOptions options{};
            options.radius = (std::max)(128.0f, TFD::Settings::GetSweepRadius());
            options.npcOnly = false;
            const auto snapshot = TFD::Actor::BuildSnapshot(player, options);
            const auto playerFormID = player->GetFormID();

            float bestDistance = options.radius + 1.0f;
            for (const auto& info : snapshot.actors) {
                auto* actor = info.get();
                if (!actor || actor == player || actor == selectedActor || actor->IsDead() || actor->IsDisabled()) {
                    continue;
                }
                if (defeatedIDs.contains(actor->GetFormID())) {
                    continue;
                }
                if (!info.standing || !info.inCombat || info.playerSide) {
                    continue;
                }

                bool targetsPlayer = info.currentTargetFormID == playerFormID;
                if (!targetsPlayer) {
                    auto targetSP = actor->GetActorRuntimeData().currentCombatTarget.get();
                    targetsPlayer = targetSP.get() == player;
                }
                if (!targetsPlayer) {
                    continue;
                }

                if (info.dist < bestDistance) {
                    bestDistance = info.dist;
                    result.blocked = true;
                    result.actorFormID = actor->GetFormID();
                    result.distance = info.dist;
                }
            }
            return result;
        }

        bool ReleaseEntryByIDLocked(RE::FormID formID, std::string_view reason, bool playGetUp);

        void ClearLootTransitionLocked(std::string_view reason)
        {
            if (g_lootStage != LootStage::None) {
                spdlog::info(
                    "[TFD][Victory][R396B] Loot transition cleared actor={:08X} stage={} reason={}",
                    g_session.selectedActorFormID,
                    ToString(g_lootStage),
                    ReasonText(reason));
            }
            g_lootTransition = LootTransitionState{};
            g_lootInventoryDispatchAttempts = 0;
            g_lootDeadline = {};
            g_lootFinalizeDeadline = {};
            g_lootStage = LootStage::None;
        }

        void ClearRecruitTransitionLocked(std::string_view reason)
        {
            if (g_recruitStage != RecruitStage::None) {
                spdlog::info(
                    "[TFD][Victory][R400D] Recruit transition cleared actor={:08X} stage={} reason={}",
                    g_session.selectedActorFormID,
                    ToString(g_recruitStage),
                    ReasonText(reason));
            }
            g_recruitTransition = RecruitTransitionState{};
            g_recruitDeadline = {};
            g_recruitFinalizeDeadline = {};
            g_recruitStage = RecruitStage::None;
        }

        void ResetSessionLocked(std::string_view reason, bool restartCountdown)
        {
            const auto previous = g_session;
            const auto now = Now();

            if (previous.active && previous.selectedActorFormID != 0) {
                if (auto it = g_enemyEntries.find(previous.selectedActorFormID); it != g_enemyEntries.end()) {
                    auto& entry = it->second;
                    if (entry.countdownHeld &&
                        (entry.heldSessionID == 0 || entry.heldSessionID == previous.sessionID)) {
                        entry.countdownHeld = false;
                        entry.heldSessionID = 0;
                        entry.autoDeathIssued = false;
                        entry.fatalDamageApplied = false;
                        if (restartCountdown) {
                            entry.deadline = now + std::chrono::milliseconds(
                                static_cast<int>(kEnemyKnockSeconds * 1000.0));
                        }
                    }
                }
            }

            ClearPleasureDialogueCloseGuardLocked(reason);

            g_session = SessionSnapshot{};
            g_sessionOpenDeadline = {};
            g_choiceCommitDeadline = {};
            g_killDeadline = {};
            g_killFinalizeNotBefore = {};
            g_killStage = KillStage::None;
            ClearLootTransitionLocked(reason);
            ClearRecruitTransitionLocked(reason);
            RefreshConditionStateLocked();

            if (previous.active || previous.selectedActorFormID != 0) {
                spdlog::info(
                    "[TFD][Victory][R394A] session closed actor={:08X} session={} phase={} countdownRestart={} reason={}",
                    previous.selectedActorFormID,
                    previous.sessionID,
                    ToString(previous.phase),
                    restartCountdown ? kEnemyKnockSeconds : 0.0,
                    ReasonText(reason));
            }
        }

        void ArmCommittedKillLocked(Clock::time_point now, std::string_view reason)
        {
            if (!g_session.active ||
                g_session.phase != SessionPhase::KillCommitted ||
                g_session.selectedActorFormID == 0) {
                return;
            }

            if (g_killStage != KillStage::WaitingForDialogueClose &&
                g_killStage != KillStage::None) {
                return;
            }

            g_killStage = KillStage::PrimePending;
            g_killDeadline = now + kKillPrimeDelay;
            spdlog::info(
                "[TFD][Victory][R395B] Kill execution armed actor={:08X} session={} stage={} reason={} dialogueOpen={}",
                g_session.selectedActorFormID,
                g_session.sessionID,
                ToString(g_killStage),
                ReasonText(reason),
                IsDialogueMenuOpen() ? 1 : 0);
        }

        void TickCommittedKillLocked(Clock::time_point now)
        {
            if (!g_session.active ||
                g_session.phase != SessionPhase::KillCommitted ||
                g_session.selectedActorFormID == 0) {
                return;
            }

            const auto actorFormID = g_session.selectedActorFormID;
            const auto sessionID = g_session.sessionID;
            auto it = g_enemyEntries.find(actorFormID);
            if (it == g_enemyEntries.end() || !it->second.managed) {
                spdlog::error(
                    "[TFD][Victory][R395B] Kill execution lost defeated entry actor={:08X} session={} action=close_session",
                    actorFormID,
                    sessionID);
                ResetSessionLocked("kill_entry_missing", false);
                return;
            }

            auto actorSP = it->second.handle.get();
            auto* actor = actorSP.get();
            if (!actor || actor->IsDisabled()) {
                spdlog::error(
                    "[TFD][Victory][R395B] Kill execution invalid actor={:08X} session={} action=release_entry",
                    actorFormID,
                    sessionID);
                (void)ReleaseEntryByIDLocked(actorFormID, "victory_kill_invalid_actor", false);
                return;
            }

            if (actor->IsDead()) {
                if (g_killFinalizeNotBefore.time_since_epoch().count() != 0 &&
                    now < g_killFinalizeNotBefore) {
                    return;
                }

                const auto completedStage = g_killStage;
                const bool released = ReleaseEntryByIDLocked(actorFormID, "victory_kill_confirmed", false);
                spdlog::info(
                    "[TFD][Victory][R395B] Kill finalized actor={:08X} session={} dead=1 releasedManaged={} aliasFactionCleared={} stage={} no_getup=1",
                    actorFormID,
                    sessionID,
                    released ? 1 : 0,
                    released ? 1 : 0,
                    ToString(completedStage));
                return;
            }

            if (g_killStage == KillStage::WaitingForDialogueClose) {
                if (!IsDialogueMenuOpen()) {
                    ArmCommittedKillLocked(now, "dialogue_already_closed");
                }
                return;
            }

            if (g_killStage == KillStage::PrimePending) {
                if (g_killDeadline.time_since_epoch().count() == 0 || now < g_killDeadline) {
                    return;
                }

                // Release the talking state before death. This is not a Get Up path.
                // It mirrors the confirmed auto-death staging instead of killing the
                // actor synchronously from inside the TopicInfo fragment.
                actor->SetDialogueWithPlayer(false, false, nullptr);
                if (it->second.visualBleedoutStarted && !it->second.visualBleedoutStopSent) {
                    actor->NotifyAnimationGraph("BleedoutStop");
                    it->second.visualBleedoutStopSent = true;
                }
                actor->EvaluatePackage(false, true);
                actor->EvaluatePackage(true, true);

                g_killStage = KillStage::DamagePending;
                g_killDeadline = now + kKillDamageDelay;
                spdlog::info(
                    "[TFD][Victory][R395B] Kill death stage primed actor={:08X} session={} stage={} no_getup=1",
                    actorFormID,
                    sessionID,
                    ToString(g_killStage));
                return;
            }

            if (g_killStage == KillStage::DamagePending) {
                if (g_killDeadline.time_since_epoch().count() == 0 || now < g_killDeadline) {
                    return;
                }

                const float hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
                const float fatalDamage = (std::max)(25.0f, hpNow + 5000.0f);
                actor->RestoreActorValue(
                    RE::ACTOR_VALUE_MODIFIER::kDamage,
                    RE::ActorValue::kHealth,
                    -fatalDamage);

                g_killStage = KillStage::AwaitingDeath;
                g_killFinalizeNotBefore = now + kKillDeathSettleDelay;
                g_killDeadline = now + kKillDeathConfirmDelay;
                spdlog::info(
                    "[TFD][Victory][R395B] Kill fatal damage applied actor={:08X} session={} hpBefore={:.2f} damage={:.2f} stage={}",
                    actorFormID,
                    sessionID,
                    hpNow,
                    fatalDamage,
                    ToString(g_killStage));
                return;
            }

            if (g_killStage == KillStage::AwaitingDeath) {
                if (g_killDeadline.time_since_epoch().count() == 0 || now < g_killDeadline) {
                    return;
                }

                // The normal fatal-damage path should already have killed the
                // actor. If it did not, use the engine's non-instant kill path so
                // the death graph still gets a chance to run. Never use
                // KillImmediate for a committed Victory choice.
                const float hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
                const float fatalDamage = (std::max)(25.0f, hpNow + 5000.0f);
                actor->KillImpl(RE::PlayerCharacter::GetSingleton(), fatalDamage, true, false);
                g_killStage = KillStage::AwaitingEngineDeath;
                g_killFinalizeNotBefore = now + kKillDeathSettleDelay;
                g_killDeadline = now + kKillEngineConfirmDelay;
                spdlog::warn(
                    "[TFD][Victory][R395B] Kill engine fallback actor={:08X} session={} damage={:.2f} ragdollInstant=0 stage={}",
                    actorFormID,
                    sessionID,
                    fatalDamage,
                    ToString(g_killStage));
                return;
            }

            if (g_killStage == KillStage::AwaitingEngineDeath &&
                g_killDeadline.time_since_epoch().count() != 0 &&
                now >= g_killDeadline) {
                if (actor->IsDead()) {
                    return;
                }

                spdlog::error(
                    "[TFD][Victory][R395B] Kill failed after engine fallback actor={:08X} session={} action=cancel_restart_countdown no_kill_immediate=1",
                    actorFormID,
                    sessionID);
                ResetSessionLocked("kill_failed_after_engine_fallback", true);
            }
        }

        void ApplyRegenOverride(RE::Actor* actor, EnemyEntry& entry)
        {
            auto* avo = actor ? actor->AsActorValueOwner() : nullptr;
            if (!avo) {
                return;
            }

            if (!entry.regenOverridden) {
                entry.savedHealRate = avo->GetActorValue(RE::ActorValue::kHealRate);
                entry.savedHealRateMult = avo->GetActorValue(RE::ActorValue::kHealRateMult);
                entry.savedCombatHealRateMult = avo->GetActorValue(RE::ActorValue::kCombatHealthRegenMultiply);
                entry.regenOverridden = true;
            }

            avo->SetActorValue(RE::ActorValue::kHealRate, 0.0f);
            avo->SetActorValue(RE::ActorValue::kHealRateMult, 0.0f);
            avo->SetActorValue(RE::ActorValue::kCombatHealthRegenMultiply, 0.0f);
        }

        void RestoreRegenOverride(RE::Actor* actor, const EnemyEntry& entry)
        {
            if (!entry.regenOverridden) {
                return;
            }

            auto* avo = actor ? actor->AsActorValueOwner() : nullptr;
            if (!avo) {
                return;
            }

            avo->SetActorValue(RE::ActorValue::kHealRate, entry.savedHealRate);
            avo->SetActorValue(RE::ActorValue::kHealRateMult, entry.savedHealRateMult);
            avo->SetActorValue(RE::ActorValue::kCombatHealthRegenMultiply, entry.savedCombatHealRateMult);
        }

        bool DispatchLootInventoryOpen(RE::Actor* actor)
        {
            if (!actor || actor->IsDisabled() || actor->IsDead()) {
                return false;
            }

            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm) {
                return false;
            }

            auto* policy = vm->GetObjectHandlePolicy();
            if (!policy) {
                return false;
            }

            const auto handle = policy->GetHandleForObject(actor->GetFormType(), actor);
            if (handle == policy->EmptyHandle()) {
                return false;
            }

            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback{};
            auto* args = RE::MakeFunctionArguments(true);
            return vm->DispatchMethodCall(handle, "Actor", "OpenInventory", args, callback);
        }

        float RestoreLootReleaseHealth(RE::Actor* actor, float thresholdPct)
        {
            if (!actor) {
                return 0.0f;
            }

            const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
            const float hpBefore = actor->GetActorValue(RE::ActorValue::kHealth);
            const float safePct = std::clamp((thresholdPct + kLootHealthBonusPct) / 100.0f, 0.25f, 0.99f);
            const float target = (std::max)(25.0f, hpMax * safePct);
            if (hpBefore + 0.001f < target) {
                actor->RestoreActorValue(
                    RE::ACTOR_VALUE_MODIFIER::kDamage,
                    RE::ActorValue::kHealth,
                    target - hpBefore);
            }
            return actor->GetActorValue(RE::ActorValue::kHealth);
        }

        float SetPleasureReturnBleedoutHealth(RE::Actor* actor, float thresholdPct, std::string_view reason)
        {
            if (!actor) {
                return 0.0f;
            }

            const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
            const float hpBefore = actor->GetActorValue(RE::ActorValue::kHealth);
            const float targetPct = std::clamp(
                (thresholdPct - kPleasureReturnBleedoutHealthBufferPct) / 100.0f,
                kPleasureReturnBleedoutMinPct / 100.0f,
                kPleasureReturnBleedoutMaxPct / 100.0f);
            const float target = std::clamp(
                hpMax * targetPct,
                kPleasureReturnBleedoutMinAbsHp,
                (std::max)(kPleasureReturnBleedoutMinAbsHp, hpMax * 0.95f));

            if (hpBefore > target + 0.001f) {
                actor->RestoreActorValue(
                    RE::ACTOR_VALUE_MODIFIER::kDamage,
                    RE::ActorValue::kHealth,
                    -(hpBefore - target));
            }
            else if (hpBefore + 0.001f < target) {
                actor->RestoreActorValue(
                    RE::ACTOR_VALUE_MODIFIER::kDamage,
                    RE::ActorValue::kHealth,
                    target - hpBefore);
            }

            const float hpAfter = actor->GetActorValue(RE::ActorValue::kHealth);
            spdlog::info(
                "[TFD][Victory][P34B] Pleasure return bleedout health actor={:08X} reason={} hpBefore={:.2f} hpAfter={:.2f} hpMax={:.2f} thresholdPct={:.1f} targetPct={:.1f}",
                actor->GetFormID(),
                ReasonText(reason),
                hpBefore,
                hpAfter,
                hpMax,
                thresholdPct,
                targetPct * 100.0f);
            return hpAfter;
        }

        bool BeginLootReleaseLocked(Clock::time_point now)
        {
            if (!g_session.active ||
                g_session.phase != SessionPhase::LootCommitted ||
                g_session.selectedActorFormID == 0 ||
                g_lootStage != LootStage::ReleasePending) {
                return false;
            }

            const auto actorFormID = g_session.selectedActorFormID;
            const auto sessionID = g_session.sessionID;
            auto it = g_enemyEntries.find(actorFormID);
            if (it == g_enemyEntries.end() || !it->second.managed) {
                spdlog::error(
                    "[TFD][Victory][R396B] Loot release lost defeated entry actor={:08X} session={} action=close_session",
                    actorFormID,
                    sessionID);
                ResetSessionLocked("loot_entry_missing", false);
                return false;
            }

            auto& entry = it->second;
            auto actorSP = entry.handle.get();
            auto* actor = actorSP.get();
            if (!actor || actor->IsDisabled() || actor->IsDead()) {
                spdlog::warn(
                    "[TFD][Victory][R396B] Loot release invalid actor={:08X} session={} dead={} action=release_without_getup",
                    actorFormID,
                    sessionID,
                    actor && actor->IsDead() ? 1 : 0);
                (void)ReleaseEntryByIDLocked(actorFormID, "victory_loot_invalid_actor", false);
                return false;
            }

            const bool wasBleedingOut = entry.visualBleedoutStarted && !entry.visualBleedoutStopSent;
            const float hpBefore = actor->GetActorValue(RE::ActorValue::kHealth);
            const float hpAfter = RestoreLootReleaseHealth(actor, entry.thresholdPct);

            TFD::Actor::Ops::SuppressDefeatedEnemyReentry(
                actor,
                kLootReentrySuppressSeconds,
                "victory_loot_controlled_release");

            // Keep the defeated registry/faction and passive aggression owned until
            // the one-shot Get Up has settled. This follows the outcome contract:
            // Get Up first, then leave the alias/faction and become free to attack.
            entry.countdownHeld = true;
            entry.heldSessionID = sessionID;
            g_lootTransition.handle = entry.handle;
            g_lootTransition.wasBleedingOut = wasBleedingOut;

            actor->SetDialogueWithPlayer(false, false, nullptr);
            g_lootStage = LootStage::BleedoutStopPending;
            g_lootDeadline = now + kLootReleasePrepareDelay;

            spdlog::info(
                "[TFD][Victory][R399A] Loot defeated visual release armed actor={:08X} session={} aliasFactionHeld=1 hpBefore={:.2f} hpAfter={:.2f} threshold={:.1f} visualBleedoutOwned={} passiveHeldUntilRelease=1 stage={}",
                actorFormID,
                sessionID,
                hpBefore,
                hpAfter,
                entry.thresholdPct,
                wasBleedingOut ? 1 : 0,
                ToString(g_lootStage));
            return true;
        }

        void TickCommittedLootLocked(Clock::time_point now)
        {
            if (!g_session.active ||
                g_session.phase != SessionPhase::LootCommitted ||
                g_session.selectedActorFormID == 0) {
                return;
            }

            const auto actorFormID = g_session.selectedActorFormID;
            const auto sessionID = g_session.sessionID;

            if (g_lootStage == LootStage::WaitingForDialogueClose) {
                if (!IsDialogueMenuOpen()) {
                    g_lootStage = LootStage::InventoryDispatchPending;
                    g_lootInventoryDispatchAttempts = 0;
                    g_lootDeadline = now + kLootInventoryDispatchDelay;
                    spdlog::info(
                        "[TFD][Victory][R396B] Loot dialogue closed actor={:08X} session={} stage={} nativeDispatchDelayMs={}",
                        actorFormID,
                        sessionID,
                        ToString(g_lootStage),
                        kLootInventoryDispatchDelay.count());
                    return;
                }

                if (g_lootDeadline.time_since_epoch().count() != 0 && now >= g_lootDeadline) {
                    spdlog::warn(
                        "[TFD][Victory][R396B] Loot cancelled actor={:08X} session={} gate=dialogue_close_timeout countdownRestart=10",
                        actorFormID,
                        sessionID);
                    ResetSessionLocked("loot_dialogue_close_timeout", true);
                }
                return;
            }

            if (g_lootStage == LootStage::InventoryDispatchPending) {
                if (g_lootDeadline.time_since_epoch().count() != 0 && now < g_lootDeadline) {
                    return;
                }

                auto entryIt = g_enemyEntries.find(actorFormID);
                if (entryIt == g_enemyEntries.end() || !entryIt->second.managed) {
                    spdlog::warn(
                        "[TFD][Victory][R396B] Loot inventory dispatch cancelled actor={:08X} session={} gate=missing_defeated_entry countdownRestart=10",
                        actorFormID,
                        sessionID);
                    ResetSessionLocked("loot_inventory_dispatch_missing_entry", true);
                    return;
                }

                auto actorSP = entryIt->second.handle.get();
                auto* actor = actorSP.get();
                if (!actor || actor->IsDisabled() || actor->IsDead()) {
                    spdlog::warn(
                        "[TFD][Victory][R396B] Loot inventory dispatch cancelled actor={:08X} session={} gate=invalid_defeated_actor countdownRestart=10",
                        actorFormID,
                        sessionID);
                    ResetSessionLocked("loot_inventory_dispatch_invalid_actor", true);
                    return;
                }

                ++g_lootInventoryDispatchAttempts;
                g_lootStage = LootStage::WaitingForInventoryOpen;
                g_lootDeadline = now + kLootInventoryOpenTimeout;
                const bool dispatched = DispatchLootInventoryOpen(actor);
                if (dispatched || g_lootStage == LootStage::InventoryOpen) {
                    spdlog::info(
                        "[TFD][Victory][R396B] Loot inventory native dispatch actor={:08X} session={} attempt={} dispatched={} stage={} method=Actor.OpenInventory forceOpen=1",
                        actorFormID,
                        sessionID,
                        static_cast<unsigned>(g_lootInventoryDispatchAttempts),
                        dispatched ? 1 : 0,
                        ToString(g_lootStage));
                    return;
                }

                if (g_lootInventoryDispatchAttempts < kLootInventoryDispatchMaxAttempts) {
                    g_lootStage = LootStage::InventoryDispatchPending;
                    g_lootDeadline = now + kLootInventoryDispatchRetryDelay;
                    spdlog::warn(
                        "[TFD][Victory][R396B] Loot inventory native dispatch retry actor={:08X} session={} attempt={} maxAttempts={} retryDelayMs={}",
                        actorFormID,
                        sessionID,
                        static_cast<unsigned>(g_lootInventoryDispatchAttempts),
                        static_cast<unsigned>(kLootInventoryDispatchMaxAttempts),
                        kLootInventoryDispatchRetryDelay.count());
                    return;
                }

                spdlog::error(
                    "[TFD][Victory][R396B] Loot inventory native dispatch failed actor={:08X} session={} attempts={} action=cancel_and_restart_countdown",
                    actorFormID,
                    sessionID,
                    static_cast<unsigned>(g_lootInventoryDispatchAttempts));
                ResetSessionLocked("loot_inventory_native_dispatch_failed", true);
                return;
            }

            if (g_lootStage == LootStage::WaitingForInventoryOpen) {
                if (IsSelectedLootContainerOpen(actorFormID)) {
                    g_lootStage = LootStage::InventoryOpen;
                    g_lootDeadline = {};
                    spdlog::info(
                        "[TFD][Victory][R396B] Loot inventory open confirmed by native poll actor={:08X} session={} stage={} target={:08X}",
                        actorFormID,
                        sessionID,
                        ToString(g_lootStage),
                        GetOpenLootTargetActorFormID());
                    return;
                }

                if (g_lootDeadline.time_since_epoch().count() != 0 && now >= g_lootDeadline) {
                    spdlog::warn(
                        "[TFD][Victory][R396B] Loot cancelled actor={:08X} session={} gate=inventory_open_timeout currentTarget={:08X} countdownRestart=10",
                        actorFormID,
                        sessionID,
                        GetOpenLootTargetActorFormID());
                    ResetSessionLocked("loot_inventory_open_timeout", true);
                }
                return;
            }

            if (g_lootStage == LootStage::InventoryOpen) {
                return;
            }

            if (g_lootDeadline.time_since_epoch().count() != 0 && now < g_lootDeadline) {
                return;
            }

            if (g_lootStage == LootStage::ReleasePending) {
                (void)BeginLootReleaseLocked(now);
                return;
            }

            auto actorSP = g_lootTransition.handle.get();
            auto* actor = actorSP.get();
            if (!actor || actor->IsDisabled() || actor->IsDead()) {
                spdlog::warn(
                    "[TFD][Victory][R396B] Loot transition actor lost actor={:08X} session={} stage={} action=close_session",
                    actorFormID,
                    sessionID,
                    ToString(g_lootStage));
                ResetSessionLocked("loot_transition_actor_lost", false);
                return;
            }

            if (g_lootStage == LootStage::BleedoutStopPending) {
                if (g_lootTransition.wasBleedingOut) {
                    actor->NotifyAnimationGraph("BleedoutStop");
                    if (auto entryIt = g_enemyEntries.find(actorFormID); entryIt != g_enemyEntries.end()) {
                        entryIt->second.visualBleedoutStopSent = true;
                    }
                }
                g_lootStage = LootStage::GetUpPending;
                g_lootDeadline = now + kLootGraphStepDelay;
                spdlog::info(
                    "[TFD][Victory][R399A] Loot graph release prepared actor={:08X} session={} BleedoutStopSent={} visualOwned={} stage={}",
                    actorFormID,
                    sessionID,
                    g_lootTransition.wasBleedingOut ? 1 : 0,
                    g_lootTransition.wasBleedingOut ? 1 : 0,
                    ToString(g_lootStage));
                return;
            }

            if (g_lootStage == LootStage::GetUpPending) {
                actor->NotifyAnimationGraph("GetUpStart");
                g_lootStage = LootStage::PackageRefreshPending;
                g_lootDeadline = now + kLootGetUpMinimumSettle;
                g_lootFinalizeDeadline = now + kLootGetUpMaximumSettle;
                spdlog::info(
                    "[TFD][Victory][R399A] Loot controlled Get Up sent actor={:08X} session={} GetUpStartCount=1 minSettleMs={} maxSettleMs={} QueueNiNodeUpdate=0 repeatedEvaluatePackage=0 stage={}",
                    actorFormID,
                    sessionID,
                    kLootGetUpMinimumSettle.count(),
                    kLootGetUpMaximumSettle.count(),
                    ToString(g_lootStage));
                return;
            }

            if (g_lootStage == LootStage::PackageRefreshPending) {
                const auto* actorState = actor->AsActorState();
                const bool stillBleedingOut = actorState && actorState->IsBleedingOut();
                if (stillBleedingOut &&
                    g_lootFinalizeDeadline.time_since_epoch().count() != 0 &&
                    now < g_lootFinalizeDeadline) {
                    return;
                }

                auto entryIt = g_enemyEntries.find(actorFormID);
                if (entryIt == g_enemyEntries.end()) {
                    spdlog::error(
                        "[TFD][Victory][R396B] Loot finalization lost defeated entry actor={:08X} session={} action=close_session",
                        actorFormID,
                        sessionID);
                    ResetSessionLocked("loot_finalize_entry_missing", false);
                    return;
                }

                auto entry = entryIt->second;
                g_enemyEntries.erase(entryIt);
                RestoreRegenOverride(actor, entry);
                TFD::Actor::Ops::ClearDefeatedEnemyState(
                    actor,
                    entry.aliasSlot,
                    entry.factionApplied,
                    entry.managed,
                    entry.autoDeathIssued,
                    entry.fatalDamageApplied,
                    entry.deadline,
                    entry.savedAggression,
                    entry.aggressionOverridden,
                    "victory_loot_complete");

                // One package evaluation after Get Up and after alias/faction/passive
                // cleanup. No graph retry, node rebuild, ragdoll repair, or loop.
                actor->EvaluatePackage(false, true);

                const bool hostileToPlayer = [&]() {
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    return player && actor->IsHostileToActor(player);
                }();

                g_lootTransition = LootTransitionState{};
                g_lootStage = LootStage::None;
                g_lootDeadline = {};
                g_lootFinalizeDeadline = {};

                spdlog::info(
                    "[TFD][Victory][R399A] Loot finalized actor={:08X} session={} visualReleaseSettled=1 releasedManaged=1 aliasFactionCleared=1 passiveRestored=1 GetUpStartCount=1 packageEvaluateCount=1 stillBleedingOut={} mayAttackPlayer={} no_queue_node_update=1",
                    actorFormID,
                    sessionID,
                    stillBleedingOut ? 1 : 0,
                    hostileToPlayer ? 1 : 0);
                ResetSessionLocked("victory_loot_complete", false);
            }
        }

        float RestoreRecruitReleaseHealth(RE::Actor* actor, float thresholdPct)
        {
            if (!actor) {
                return 0.0f;
            }

            const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
            const float hpBefore = actor->GetActorValue(RE::ActorValue::kHealth);
            const float safePct = std::clamp((thresholdPct + kRecruitHealthBonusPct) / 100.0f, 0.25f, 0.99f);
            const float target = (std::max)(25.0f, hpMax * safePct);
            if (hpBefore + 0.001f < target) {
                actor->RestoreActorValue(
                    RE::ACTOR_VALUE_MODIFIER::kDamage,
                    RE::ActorValue::kHealth,
                    target - hpBefore);
            }
            return actor->GetActorValue(RE::ActorValue::kHealth);
        }

        bool CommitRecruitAfterVisualReleaseLocked(Clock::time_point now);

        bool BeginRecruitCommitLocked(Clock::time_point now)
        {
            if (!g_session.active ||
                g_session.phase != SessionPhase::RecruitCommitted ||
                g_session.selectedActorFormID == 0 ||
                g_recruitStage != RecruitStage::CommitPending) {
                return false;
            }

            const auto actorFormID = g_session.selectedActorFormID;
            const auto sessionID = g_session.sessionID;
            auto entryIt = g_enemyEntries.find(actorFormID);
            if (entryIt == g_enemyEntries.end() || !entryIt->second.managed) {
                spdlog::error(
                    "[TFD][Victory][R400D] Recruit prepare lost defeated entry actor={:08X} session={} action=close_session",
                    actorFormID,
                    sessionID);
                ResetSessionLocked("recruit_entry_missing", false);
                return false;
            }

            auto& entry = entryIt->second;
            auto actorSP = entry.handle.get();
            auto* actor = actorSP.get();
            if (!actor || actor->IsDisabled() || actor->IsDead()) {
                spdlog::warn(
                    "[TFD][Victory][R400D] Recruit prepare invalid actor={:08X} session={} dead={} action=cancel_restart_countdown",
                    actorFormID,
                    sessionID,
                    actor && actor->IsDead() ? 1 : 0);
                ResetSessionLocked("recruit_invalid_actor", true);
                return false;
            }

            if (TFD::TeammateManager::GetRecruitSlotsFree() == 0) {
                spdlog::warn(
                    "[TFD][Victory][R400D] Recruit prepare rejected actor={:08X} session={} gate=no_recruit_slot countdownRestart=10",
                    actorFormID,
                    sessionID);
                ResetSessionLocked("recruit_no_slot", true);
                return false;
            }

            const bool visualBleedoutOwned = entry.visualBleedoutStarted && !entry.visualBleedoutStopSent;
            const float hpBefore = actor->GetActorValue(RE::ActorValue::kHealth);
            const float hpAfter = RestoreRecruitReleaseHealth(actor, entry.thresholdPct);

            TFD::Actor::Ops::SuppressDefeatedEnemyReentry(
                actor,
                kRecruitReentrySuppressSeconds,
                "victory_recruit_visual_release_hold_defeated");

            actor->SetDialogueWithPlayer(false, false, nullptr);
            if (actor->IsInCombat()) {
                actor->StopCombat();
            }
            if (auto* process = RE::ProcessLists::GetSingleton()) {
                process->StopCombatAndAlarmOnActor(actor, false);
            }

            // R402A: do not let the actor enter the GetUp graph while still
            // raw-hostile. Keep defeated visual ownership through BleedoutStop/GetUp,
            // but install the recruit faction/runtime profile before the graph release.
            // Teammate alias/package work is still deferred until after visual settle.
            g_recruitTransition.handle = actor->GetHandle();
            g_recruitTransition.visualBleedoutOwned = visualBleedoutOwned;
            g_recruitTransition.bleedoutStopSent = false;
            g_recruitTransition.bleedoutExitObservedBeforeGetUp = false;
            g_recruitTransition.actorBleedingBeforeGetUp = false;
            g_recruitTransition.getUpStartSent = false;
            g_recruitTransition.hitReactStartSent = false;
            g_recruitTransition.hitReactFallbackStartSent = false;
            g_recruitTransition.hitReactStopSent = false;
            g_recruitTransition.hitReactFallbackStopSent = false;
            g_recruitTransition.getUpForcedAfterExitTimeout = false;
            g_recruitTransition.bleedoutExitWaitTicks = 0;
            g_recruitTransition.defeatedOwnershipClearedBeforeGetUp = false;
            g_recruitTransition.aliasFactionClearedBeforeGetUp = false;
            g_recruitTransition.regenRestoredBeforeGetUp = false;
            g_recruitTransition.passiveHeldThroughGetUp = entry.aggressionOverridden;
            g_recruitTransition.passiveRestoredOnFailure = false;
            g_recruitTransition.savedAggression = entry.savedAggression;
            g_recruitTransition.aggressionOverridden = entry.aggressionOverridden;
            g_recruitTransition.preGetUpCommitAttempted = false;
            g_recruitTransition.preGetUpCommitSkipped = false;
            g_recruitTransition.preGetUpRawHostileBefore = true;
            g_recruitTransition.preGetUpRawHostileAfter = true;
            g_recruitTransition.commitAttempted = false;
            g_recruitTransition.commitSkipped = false;
            g_recruitTransition.rawHostileAfter = true;
            g_recruitTransition.teammateRegistered = false;

            TFD::Recruit::MarkRecruitCommitPending(
                actor,
                kRecruitCommitPendingSeconds,
                TFD::Recruit::SourceFlow::Dialogue,
                "victory_recruit_pre_getup_dehostile");

            TFD::Recruit::CommitOptions preOptions{};
            preOptions.sourceFlow = TFD::Recruit::SourceFlow::Dialogue;
            preOptions.reason = "victory_recruit_pre_getup_dehostile";
            preOptions.quarantineHostileFactions = true;
            preOptions.clearCombat = true;
            preOptions.evaluatePackage = false;
            preOptions.detailedLog = true;
            preOptions.throttleObserve = false;
            preOptions.ensurePacifyAlliance = true;
            preOptions.applyRuntimeProfile = true;

            auto preCommit = TFD::Recruit::CommitRecruit(actor, preOptions);
            if (preCommit.rawHostileAfter && preCommit.attempted && !preCommit.skipped) {
                TFD::Recruit::MarkRecruitCommitPending(
                    actor,
                    kRecruitCommitPendingSeconds,
                    TFD::Recruit::SourceFlow::Dialogue,
                    "victory_recruit_pre_getup_dehostile_retry");
                preOptions.reason = "victory_recruit_pre_getup_dehostile_retry";
                preCommit = TFD::Recruit::CommitRecruit(actor, preOptions);
            }

            g_recruitTransition.preGetUpCommitAttempted = preCommit.attempted;
            g_recruitTransition.preGetUpCommitSkipped = preCommit.skipped;
            g_recruitTransition.preGetUpRawHostileBefore = preCommit.rawHostileBefore;
            g_recruitTransition.preGetUpRawHostileAfter = preCommit.rawHostileAfter;

            if (!preCommit.attempted || (preCommit.skipped && preCommit.rawHostileAfter)) {
                TFD::Recruit::ClearRecruitCommitPending(actor, "victory_recruit_pre_getup_dehostile_failed");
                spdlog::warn(
                    "[TFD][Victory][R421A] Recruit pre-GetUp dehostile failed actor={:08X} session={} attempted={} skipped={} rawBefore={} rawAfter={} hostileAfter={} action=restart_countdown_no_graph_release",
                    actorFormID,
                    sessionID,
                    preCommit.attempted ? 1 : 0,
                    preCommit.skipped ? 1 : 0,
                    preCommit.rawHostileBefore ? 1 : 0,
                    preCommit.rawHostileAfter ? 1 : 0,
                    preCommit.hostileFactionMatchesAfter);
                ResetSessionLocked("recruit_pre_getup_dehostile_failed", true);
                return false;
            }

            if (!visualBleedoutOwned) {
                g_recruitStage = RecruitStage::DefeatedClearPending;
                g_recruitDeadline = now + kRecruitDefeatedClearDelay;
                g_recruitFinalizeDeadline = now + kRecruitGetUpMaximumSettle;
                spdlog::info(
                    "[TFD][Victory][R421A] Recruit package-hold visual release skipped actor={:08X} session={} hpBefore={:.2f} hpAfter={:.2f} visualBleedoutOwned=0 noBleedoutStop=1 noFlinch=1 noGetUpStart=1 visualReleaseSkipped=1 aliasFactionHeld=1 defeatedEntryHeld=1 passiveHeldThroughPackageHold={} preCommitAttempted={} preCommitSkipped={} rawBefore={} rawAfter={} hostileAfter={} teammateCommitBeforeClear=1 teammatePackageDeferred=1 evaluateDuringPrepare=0 defeatedClearDelayMs={} stage={}",
                    actorFormID,
                    sessionID,
                    hpBefore,
                    hpAfter,
                    g_recruitTransition.passiveHeldThroughGetUp ? 1 : 0,
                    preCommit.attempted ? 1 : 0,
                    preCommit.skipped ? 1 : 0,
                    preCommit.rawHostileBefore ? 1 : 0,
                    preCommit.rawHostileAfter ? 1 : 0,
                    preCommit.hostileFactionMatchesAfter,
                    kRecruitDefeatedClearDelay.count(),
                    ToString(g_recruitStage));
                return true;
            }

            g_recruitStage = RecruitStage::BleedoutStopPending;
            g_recruitDeadline = now + kRecruitGraphStepDelay;
            spdlog::info(
                "[TFD][Victory][R421A] Recruit pre-GetUp dehostile armed actor={:08X} session={} hpBefore={:.2f} hpAfter={:.2f} visualBleedoutOwned={} aliasFactionHeld=1 defeatedEntryHeld=1 passiveHeldThroughGetUp={} preCommitAttempted={} preCommitSkipped={} rawBefore={} rawAfter={} hostileAfter={} teammateCommitBeforeBleedoutStop=1 teammatePackageDeferred=1 evaluateDuringPrepare=0 graphStepDelayMs={} bleedoutExitMinDelayMs={} bleedoutExitMaxWaitMs={} noDisableEnable=1 flinchRefresh=0 bleedoutStopOnlyExit=1 stage={}",
                actorFormID,
                sessionID,
                hpBefore,
                hpAfter,
                visualBleedoutOwned ? 1 : 0,
                g_recruitTransition.passiveHeldThroughGetUp ? 1 : 0,
                preCommit.attempted ? 1 : 0,
                preCommit.skipped ? 1 : 0,
                preCommit.rawHostileBefore ? 1 : 0,
                preCommit.rawHostileAfter ? 1 : 0,
                preCommit.hostileFactionMatchesAfter,
                kRecruitGraphStepDelay.count(),
                kRecruitBleedoutExitMinimumDelay.count(),
                kRecruitBleedoutExitMaximumWait.count(),
                ToString(g_recruitStage));
            return true;
        }

        bool ClearRecruitDefeatedOwnershipAfterGetUpLocked(Clock::time_point now)
        {
            if (!g_session.active ||
                g_session.phase != SessionPhase::RecruitCommitted ||
                g_session.selectedActorFormID == 0 ||
                g_recruitStage != RecruitStage::DefeatedClearPending) {
                return false;
            }

            const auto actorFormID = g_session.selectedActorFormID;
            const auto sessionID = g_session.sessionID;
            auto entryIt = g_enemyEntries.find(actorFormID);
            if (entryIt == g_enemyEntries.end() || !entryIt->second.managed) {
                spdlog::error(
                    "[TFD][Victory][R421A] Recruit post-flinch-refresh ownership clear lost defeated entry actor={:08X} session={} action=close_session",
                    actorFormID,
                    sessionID);
                ResetSessionLocked("recruit_post_hit_refresh_entry_missing", false);
                return false;
            }

            auto entry = entryIt->second;
            auto actorSP = entry.handle.get();
            auto* actor = actorSP.get();
            if (!actor || actor->IsDisabled() || actor->IsDead()) {
                spdlog::warn(
                    "[TFD][Victory][R421A] Recruit post-flinch-refresh ownership clear invalid actor={:08X} session={} dead={} action=close_session",
                    actorFormID,
                    sessionID,
                    actor && actor->IsDead() ? 1 : 0);
                ResetSessionLocked("recruit_post_hit_refresh_clear_invalid_actor", false);
                return false;
            }

            g_enemyEntries.erase(entryIt);
            RestoreRegenOverride(actor, entry);
            TFD::Actor::Ops::ClearDefeatedEnemyMirror(
                actor,
                entry.aliasSlot,
                entry.factionApplied,
                "victory_recruit_post_hit_refresh_visual_clear");

            // R402A: the actor was already dehostiled before GetUpStart.
            // Mark a short pending window again so the post-flinch-refresh settle commit can
            // safely re-apply the recruit runtime profile after defeated ownership is gone.
            TFD::Recruit::MarkRecruitCommitPending(
                actor,
                kRecruitCommitPendingSeconds,
                TFD::Recruit::SourceFlow::Dialogue,
                "victory_recruit_post_hit_refresh_settle_after_pre_hit_refresh_commit");

            g_recruitTransition.defeatedOwnershipClearedBeforeGetUp = false;
            g_recruitTransition.aliasFactionClearedBeforeGetUp = false;
            g_recruitTransition.regenRestoredBeforeGetUp = false;
            g_recruitTransition.savedAggression = entry.savedAggression;
            g_recruitTransition.aggressionOverridden = entry.aggressionOverridden;
            g_recruitTransition.passiveHeldThroughGetUp = entry.aggressionOverridden;

            g_recruitStage = RecruitStage::RecruitCommitPending;
            g_recruitDeadline = now + kRecruitPostClearCommitDelay;
            spdlog::info(
                "[TFD][Victory][R421A] Recruit defeated ownership cleared after OHAF-style Flinch refresh actor={:08X} session={} defeatedEntryRemoved=1 aliasFactionCleared=1 regenRestored={} passiveHeldThroughGetUp={} teammateCommitBeforeFlinch=1 postFlinchRefreshSettleCommit=1 postClearDelayMs={} atomicCommit=1 stage={}",
                actorFormID,
                sessionID,
                entry.regenOverridden ? 1 : 0,
                entry.aggressionOverridden ? 1 : 0,
                kRecruitPostClearCommitDelay.count(),
                ToString(g_recruitStage));

            // R402A: no defeated-clear -> recruit-commit gap, and the first
            // dehostile commit already happened before BleedoutStop/GetUpStart.
            // This second commit is a post-flinch-refresh settle pass after defeated faction/alias clear.
            return CommitRecruitAfterVisualReleaseLocked(now);
        }

        bool CommitRecruitAfterVisualReleaseLocked(Clock::time_point now)
        {
            if (!g_session.active ||
                g_session.phase != SessionPhase::RecruitCommitted ||
                g_session.selectedActorFormID == 0 ||
                g_recruitStage != RecruitStage::RecruitCommitPending) {
                return false;
            }

            const auto actorFormID = g_session.selectedActorFormID;
            const auto sessionID = g_session.sessionID;
            auto actorSP = g_recruitTransition.handle.get();
            auto* actor = actorSP.get();
            if (!actor || actor->IsDisabled() || actor->IsDead()) {
                spdlog::warn(
                    "[TFD][Victory][R421A] Recruit post-flinch-refresh settle commit invalid actor={:08X} session={} dead={} action=close_session",
                    actorFormID,
                    sessionID,
                    actor && actor->IsDead() ? 1 : 0);
                ResetSessionLocked("recruit_post_hit_refresh_settle_invalid_actor", false);
                return false;
            }

            TFD::Recruit::CommitOptions options{};
            options.sourceFlow = TFD::Recruit::SourceFlow::Dialogue;
            options.reason = "victory_recruit_post_hit_refresh_settle_after_pre_hit_refresh_commit";
            options.quarantineHostileFactions = true;
            options.clearCombat = true;
            options.evaluatePackage = false;
            options.detailedLog = true;
            options.throttleObserve = false;
            options.ensurePacifyAlliance = true;
            options.applyRuntimeProfile = true;

            const auto commit = TFD::Recruit::CommitRecruit(actor, options);
            g_recruitTransition.commitAttempted = commit.attempted;
            g_recruitTransition.commitSkipped = commit.skipped;
            g_recruitTransition.rawHostileAfter = commit.rawHostileAfter;

            const bool playerSideAfterSettle = TFD::TeammateManager::IsPlayerSideTeammateActor(actor);
            const bool settledAfterGetUp =
                commit.attempted &&
                !commit.rawHostileAfter &&
                commit.hostileFactionMatchesAfter == 0 &&
                (playerSideAfterSettle || !commit.skipped);

            if (!settledAfterGetUp) {
                TFD::Recruit::ClearRecruitCommitPending(actor, "victory_recruit_post_hit_refresh_settle_failed");
                actor->EvaluatePackage(false, true);
                spdlog::warn(
                    "[TFD][Victory][R421A] Recruit post-flinch-refresh settle commit failed actor={:08X} session={} attempted={} skipped={} rawAfter={} hostileAfter={} playerSideAfter={} action=close_session",
                    actorFormID,
                    sessionID,
                    commit.attempted ? 1 : 0,
                    commit.skipped ? 1 : 0,
                    commit.rawHostileAfter ? 1 : 0,
                    commit.hostileFactionMatchesAfter,
                    playerSideAfterSettle ? 1 : 0);
                ResetSessionLocked("recruit_post_hit_refresh_settle_failed", false);
                return false;
            }

            g_recruitStage = RecruitStage::PackageRefreshPending;
            g_recruitDeadline = now + kRecruitDeferredPackageDelay;
            g_recruitFinalizeDeadline = now + kRecruitGetUpMaximumSettle;
            spdlog::info(
                "[TFD][Victory][R421A] Recruit post-flinch-refresh settle commit completed actor={:08X} session={} removedHostileFactions={} ensuredState={} playerFaction={} runtimeProfile={} rawAfter={} skipped={} playerSideAfter={} preGetUpRawAfter={} deferredPackageDelayMs={} evalDuringCommit=0 stage={}",
                actorFormID,
                sessionID,
                commit.removedHostileFactions,
                commit.ensuredStateFactions,
                commit.playerFactionEnsured ? 1 : 0,
                commit.runtimeProfileApplied ? 1 : 0,
                commit.rawHostileAfter ? 1 : 0,
                commit.skipped ? 1 : 0,
                playerSideAfterSettle ? 1 : 0,
                g_recruitTransition.preGetUpRawHostileAfter ? 1 : 0,
                kRecruitDeferredPackageDelay.count(),
                ToString(g_recruitStage));
            return true;
        }

        void TickCommittedRecruitLocked(Clock::time_point now)
        {
            if (!g_session.active ||
                g_session.phase != SessionPhase::RecruitCommitted ||
                g_session.selectedActorFormID == 0) {
                return;
            }

            const auto actorFormID = g_session.selectedActorFormID;
            const auto sessionID = g_session.sessionID;

            if (g_recruitStage == RecruitStage::WaitingForDialogueClose) {
                if (!IsDialogueMenuOpen()) {
                    g_recruitStage = RecruitStage::CommitPending;
                    g_recruitDeadline = now + kRecruitCommitDelay;
                    spdlog::info(
                        "[TFD][Victory][R421A] Recruit dialogue close observed before delayed BleedoutStop release actor={:08X} session={} stage={} postDialogueCommitDelayMs={} deferBleedoutStopUntilDialogueClose=1",
                        actorFormID,
                        sessionID,
                        ToString(g_recruitStage),
                        kRecruitCommitDelay.count());
                    return;
                }

                if (g_recruitDeadline.time_since_epoch().count() != 0 && now >= g_recruitDeadline) {
                    spdlog::warn(
                        "[TFD][Victory][R421A] Recruit cancelled actor={:08X} session={} gate=dialogue_close_timeout countdownRestart=10 deferBleedoutStopUntilDialogueClose=1",
                        actorFormID,
                        sessionID);
                    ResetSessionLocked("recruit_dialogue_close_timeout", true);
                }
                return;
            }

            if (g_recruitStage == RecruitStage::CommitPending) {
                if (g_recruitDeadline.time_since_epoch().count() != 0 && now < g_recruitDeadline) {
                    return;
                }
                (void)BeginRecruitCommitLocked(now);
                return;
            }

            auto actorSP = g_recruitTransition.handle.get();
            auto* actor = actorSP.get();
            if (!actor || actor->IsDisabled() || actor->IsDead()) {
                spdlog::warn(
                    "[TFD][Victory][R400D] Recruit transition actor lost actor={:08X} session={} stage={} action=close_session",
                    actorFormID,
                    sessionID,
                    ToString(g_recruitStage));
                ResetSessionLocked("recruit_transition_actor_lost", false);
                return;
            }

            if (g_recruitDeadline.time_since_epoch().count() != 0 && now < g_recruitDeadline) {
                return;
            }

            if (g_recruitStage == RecruitStage::BleedoutStopPending) {
                bool bleedoutStopSent = false;
                if (g_recruitTransition.visualBleedoutOwned) {
                    bleedoutStopSent = actor->NotifyAnimationGraph("BleedoutStop");
                    if (auto entryIt = g_enemyEntries.find(actorFormID); entryIt != g_enemyEntries.end()) {
                        entryIt->second.visualBleedoutStopSent = bleedoutStopSent;
                    }
                }

                g_recruitTransition.bleedoutStopSent = bleedoutStopSent;
                g_recruitStage = RecruitStage::BleedoutExitPending;
                g_recruitDeadline = now + (g_recruitTransition.visualBleedoutOwned ?
                    kRecruitBleedoutExitMinimumDelay :
                    kRecruitGraphStepDelay);
                g_recruitFinalizeDeadline = now + kRecruitBleedoutExitMaximumWait;
                spdlog::info(
                    "[TFD][Victory][R421A] Recruit graph step BleedoutStop actor={:08X} session={} visualBleedoutOwned={} BleedoutStopSent={} minExitDelayMs={} maxExitWaitMs={} stage={}",
                    actorFormID,
                    sessionID,
                    g_recruitTransition.visualBleedoutOwned ? 1 : 0,
                    bleedoutStopSent ? 1 : 0,
                    (g_recruitTransition.visualBleedoutOwned ? kRecruitBleedoutExitMinimumDelay : kRecruitGraphStepDelay).count(),
                    kRecruitBleedoutExitMaximumWait.count(),
                    ToString(g_recruitStage));
                return;
            }

            if (g_recruitStage == RecruitStage::BleedoutExitPending) {
                const auto* actorState = actor->AsActorState();
                const bool stillBleedingOut = actorState && actorState->IsBleedingOut();
                const bool exitTimedOut =
                    g_recruitFinalizeDeadline.time_since_epoch().count() != 0 &&
                    now >= g_recruitFinalizeDeadline;

                if (stillBleedingOut && !exitTimedOut) {
                    ++g_recruitTransition.bleedoutExitWaitTicks;
                    g_recruitDeadline = now + kRecruitBleedoutExitCheckDelay;
                    spdlog::info(
                        "[TFD][Victory][R421A] Recruit waiting bleedout exit actor={:08X} session={} stillBleedingOut=1 waitTick={} nextCheckMs={} maxExitWaitMs={} stage={}",
                        actorFormID,
                        sessionID,
                        static_cast<unsigned>(g_recruitTransition.bleedoutExitWaitTicks),
                        kRecruitBleedoutExitCheckDelay.count(),
                        kRecruitBleedoutExitMaximumWait.count(),
                        ToString(g_recruitStage));
                    return;
                }

                g_recruitTransition.bleedoutExitObservedBeforeGetUp = !stillBleedingOut;
                g_recruitTransition.actorBleedingBeforeGetUp = stillBleedingOut;
                g_recruitTransition.getUpForcedAfterExitTimeout = stillBleedingOut && exitTimedOut;
                g_recruitTransition.hitReactStartSent = false;
                g_recruitTransition.hitReactFallbackStartSent = false;
                g_recruitTransition.hitReactStopSent = false;
                g_recruitTransition.hitReactFallbackStopSent = false;
                g_recruitStage = RecruitStage::DefeatedClearPending;
                g_recruitDeadline = now + kRecruitDefeatedClearDelay;
                spdlog::info(
                    "[TFD][Victory][R421A] Recruit bleedout exit window completed actor={:08X} session={} exitObserved={} actorBleedingBeforeClear={} forcedAfterExitTimeout={} waitTicks={} nextClearDelayMs={} stage={} noGetUpStart=1 noDisableEnable=1 noFlinch=1 bleedoutStopOnlyExit=1",
                    actorFormID,
                    sessionID,
                    g_recruitTransition.bleedoutExitObservedBeforeGetUp ? 1 : 0,
                    g_recruitTransition.actorBleedingBeforeGetUp ? 1 : 0,
                    g_recruitTransition.getUpForcedAfterExitTimeout ? 1 : 0,
                    static_cast<unsigned>(g_recruitTransition.bleedoutExitWaitTicks),
                    kRecruitDefeatedClearDelay.count(),
                    ToString(g_recruitStage));
                return;
            }

            if (g_recruitStage == RecruitStage::HitReactStartPending) {
                const auto* actorState = actor->AsActorState();
                const bool stillBleedingOut = actorState && actorState->IsBleedingOut();

                // R421A: Maxsu OHAF config for humanoid defaultmale/defaultfemale uses
                // AnimationEventName=Flinch and GraphVariableFloatName=blendFlinch.
                // Send the same graph event directly instead of the generic
                // staggerStart/staggerStop pair used by R413/R415.
                const bool flinchSent = actor->NotifyAnimationGraph("Flinch");

                g_recruitTransition.actorBleedingBeforeGetUp = stillBleedingOut;
                g_recruitTransition.getUpStartSent = false;
                g_recruitTransition.hitReactStartSent = flinchSent;
                g_recruitTransition.hitReactFallbackStartSent = false;
                g_recruitStage = RecruitStage::HitReactStopPending;
                g_recruitDeadline = now + kRecruitHitReactStopDelay;
                g_recruitFinalizeDeadline = now + kRecruitHitReactMaximumSettle;
                spdlog::info(
                    "[TFD][Victory][R421A] Recruit OHAF-style Flinch graph refresh sent actor={:08X} session={} flinchSent={} actorBleedingBeforeFlinch={} exitObservedBeforeFlinch={} forcedAfterExitTimeout={} settleGateDelayMs={} maxSettleMs={} graphEvent=Flinch noDamage=1 noDisableEnable=1 noStaggerStart=1 stage={}",
                    actorFormID,
                    sessionID,
                    flinchSent ? 1 : 0,
                    stillBleedingOut ? 1 : 0,
                    g_recruitTransition.bleedoutExitObservedBeforeGetUp ? 1 : 0,
                    g_recruitTransition.getUpForcedAfterExitTimeout ? 1 : 0,
                    kRecruitHitReactStopDelay.count(),
                    kRecruitHitReactMaximumSettle.count(),
                    ToString(g_recruitStage));
                return;
            }

            if (g_recruitStage == RecruitStage::HitReactStopPending) {
                // R421A: Flinch is a one-shot OHAF-style graph event. Do not send
                // staggerStop/recoilStop here, because the original OHAF path only
                // sends the configured event once on TESHitEvent.
                g_recruitTransition.hitReactStopSent = false;
                g_recruitTransition.hitReactFallbackStopSent = false;
                g_recruitStage = RecruitStage::DefeatedClearPending;
                g_recruitDeadline = now + kRecruitHitReactSettleDelay;
                g_recruitFinalizeDeadline = now + kRecruitHitReactMaximumSettle;
                spdlog::info(
                    "[TFD][Victory][R421A] Recruit OHAF-style Flinch graph refresh settle actor={:08X} session={} flinchSent={} settleDelayMs={} maxSettleMs={} graphEvent=Flinch noDamage=1 noDisableEnable=1 noStopEvent=1 stage={}",
                    actorFormID,
                    sessionID,
                    g_recruitTransition.hitReactStartSent ? 1 : 0,
                    kRecruitHitReactSettleDelay.count(),
                    kRecruitHitReactMaximumSettle.count(),
                    ToString(g_recruitStage));
                return;
            }

            if (g_recruitStage == RecruitStage::GetUpPending) {
                const auto* actorState = actor->AsActorState();
                const bool stillBleedingOut = actorState && actorState->IsBleedingOut();
                const bool getUpStartSent = actor->NotifyAnimationGraph("GetUpStart");
                g_recruitTransition.actorBleedingBeforeGetUp = stillBleedingOut;
                g_recruitTransition.getUpStartSent = getUpStartSent;
                g_recruitStage = RecruitStage::DefeatedClearPending;
                g_recruitDeadline = now + kRecruitGetUpMinimumSettle;
                g_recruitFinalizeDeadline = now + kRecruitGetUpMaximumSettle;
                spdlog::info(
                    "[TFD][Victory][R421A] Recruit controlled Get Up sent after bleedout exit actor={:08X} session={} GetUpStartSent={} actorBleedingBeforeGetUp={} exitObservedBeforeGetUp={} forcedAfterExitTimeout={} bleedoutExitWaitTicks={} minSettleMs={} maxSettleMs={} preCommitAttempted={} preRawAfter={} QueueNiNodeUpdate=0 repeatedEvaluatePackage=0 stage={}",
                    actorFormID,
                    sessionID,
                    getUpStartSent ? 1 : 0,
                    stillBleedingOut ? 1 : 0,
                    g_recruitTransition.bleedoutExitObservedBeforeGetUp ? 1 : 0,
                    g_recruitTransition.getUpForcedAfterExitTimeout ? 1 : 0,
                    static_cast<unsigned>(g_recruitTransition.bleedoutExitWaitTicks),
                    kRecruitGetUpMinimumSettle.count(),
                    kRecruitGetUpMaximumSettle.count(),
                    g_recruitTransition.preGetUpCommitAttempted ? 1 : 0,
                    g_recruitTransition.preGetUpRawHostileAfter ? 1 : 0,
                    ToString(g_recruitStage));
                return;
            }

            if (g_recruitStage == RecruitStage::DefeatedClearPending) {
                if (g_recruitDeadline.time_since_epoch().count() != 0 && now < g_recruitDeadline) {
                    return;
                }
                (void)ClearRecruitDefeatedOwnershipAfterGetUpLocked(now);
                return;
            }

            if (g_recruitStage == RecruitStage::RecruitCommitPending) {
                if (g_recruitDeadline.time_since_epoch().count() != 0 && now < g_recruitDeadline) {
                    return;
                }
                (void)CommitRecruitAfterVisualReleaseLocked(now);
                return;
            }

            if (g_recruitStage == RecruitStage::PackageRefreshPending) {
                if (g_recruitDeadline.time_since_epoch().count() != 0 && now < g_recruitDeadline) {
                    return;
                }

                const auto* actorState = actor->AsActorState();
                const bool stillBleedingOut = actorState && actorState->IsBleedingOut();
                const bool registered = TFD::TeammateManager::RegisterOrRefreshTeammateNowDeferredPackage(
                    actor,
                    "victory_recruit_finalize_deferred_package");
                g_recruitTransition.teammateRegistered = registered;

                bool evaluatedPackage = false;
                if (registered && actor->Is3DLoaded()) {
                    actor->EvaluatePackage(false, true);
                    evaluatedPackage = true;
                }

                TFD::TeammateManager::QueueHumanoidTeammateCatchupAfterLoad("victory_recruit_postload_visual_repair");

                const bool playerSide = TFD::TeammateManager::IsPlayerSideTeammateActor(actor);
                const bool tfdManaged = TFD::TeammateManager::IsTFDManagedTeammateActor(actor);
                const bool rawHostile = TFD::Recruit::IsRawHostileToPlayer(actor, RE::PlayerCharacter::GetSingleton());

                spdlog::info(
                    "[TFD][Victory][R421A] Recruit finalized actor={:08X} session={} teammateRegistered={} playerSide={} tfdManaged={} rawHostile={} stillBleedingOut={} bleedoutStopSent={} bleedoutExitObservedBeforeGetUp={} actorBleedingBeforeGetUp={} getUpStartSent={} forcedAfterExitTimeout={} bleedoutExitWaitTicks={} preGetUpCommitAttempted={} preGetUpCommitSkipped={} preGetUpRawBefore={} preGetUpRawAfter={} postFlinchRefreshCommitAttempted={} postFlinchRefreshCommitSkipped={} rawHostileAfterPostCommit={} defeatedOwnershipClearedAfterGetUp={} aliasFactionClearedAfterGetUp={} passiveHeldThroughGetUp={} packageEvaluateCount={} no_queue_node_update=1 teammateCommitBeforeGetUp=1 teammatePackageAfterVisualSettle=1 postLoadRepairQueued=1 flinchSent={} hitReactFallbackStartSent={} hitReactStopSent={} hitReactFallbackStopSent={} noGetUpStart=1 noDisableEnable=1 bleedoutStopOnlyExit=1",
                    actorFormID,
                    sessionID,
                    registered ? 1 : 0,
                    playerSide ? 1 : 0,
                    tfdManaged ? 1 : 0,
                    rawHostile ? 1 : 0,
                    stillBleedingOut ? 1 : 0,
                    g_recruitTransition.bleedoutStopSent ? 1 : 0,
                    g_recruitTransition.bleedoutExitObservedBeforeGetUp ? 1 : 0,
                    g_recruitTransition.actorBleedingBeforeGetUp ? 1 : 0,
                    g_recruitTransition.getUpStartSent ? 1 : 0,
                    g_recruitTransition.getUpForcedAfterExitTimeout ? 1 : 0,
                    static_cast<unsigned>(g_recruitTransition.bleedoutExitWaitTicks),
                    g_recruitTransition.preGetUpCommitAttempted ? 1 : 0,
                    g_recruitTransition.preGetUpCommitSkipped ? 1 : 0,
                    g_recruitTransition.preGetUpRawHostileBefore ? 1 : 0,
                    g_recruitTransition.preGetUpRawHostileAfter ? 1 : 0,
                    g_recruitTransition.commitAttempted ? 1 : 0,
                    g_recruitTransition.commitSkipped ? 1 : 0,
                    g_recruitTransition.rawHostileAfter ? 1 : 0,
                    g_recruitTransition.defeatedOwnershipClearedBeforeGetUp ? 0 : 1,
                    g_recruitTransition.aliasFactionClearedBeforeGetUp ? 0 : 1,
                    g_recruitTransition.passiveHeldThroughGetUp ? 1 : 0,
                    evaluatedPackage ? 1 : 0,
                    g_recruitTransition.hitReactStartSent ? 1 : 0,
                    g_recruitTransition.hitReactFallbackStartSent ? 1 : 0,
                    g_recruitTransition.hitReactStopSent ? 1 : 0,
                    g_recruitTransition.hitReactFallbackStopSent ? 1 : 0);

                ArmRecruitHitDiagnosticLocked(actor, sessionID, "victory_recruit_finalized");

                TFD::Recruit::ClearRecruitCommitPending(actor, "victory_recruit_finalized");
                ClearRecruitTransitionLocked("victory_recruit_complete");
                ResetSessionLocked("victory_recruit_complete", false);
            }
        }

        RE::Actor* ResolveCurrentCombatTarget(RE::Actor* actor)
        {
            if (!actor) {
                return nullptr;
            }
            auto sp = actor->GetActorRuntimeData().currentCombatTarget.get();
            return sp.get();
        }

        bool HasNonPlayerCombatTarget(RE::Actor* actor)
        {
            if (!actor || actor->IsDead() || actor->IsDisabled()) {
                return false;
            }

            auto* target = ResolveCurrentCombatTarget(actor);
            if (!target || target->IsDead() || target->IsDisabled()) {
                return false;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            if (target == player) {
                return false;
            }

            if (TFD::TeammateManager::IsPlayerSideTeammateActor(target)) {
                return false;
            }

            return actor->IsInCombat() || actor->IsWeaponDrawn();
        }

        bool IsSoftEnterPressureActor(RE::Actor* actor)
        {
            if (!actor || actor->IsDead() || actor->IsDisabled()) {
                return false;
            }
            auto it = g_enemyEntries.find(actor->GetFormID());
            return it != g_enemyEntries.end() &&
                it->second.managed &&
                it->second.softEnterActive &&
                !it->second.softEnterHardStateApplied;
        }

        void RecordSoftEnterPressureLocked(
            RE::FormID softActorFormID,
            RE::FormID causeFormID,
            RE::FormID otherFormID,
            std::string_view reason,
            const char* source)
        {
            auto it = g_enemyEntries.find(softActorFormID);
            if (it == g_enemyEntries.end()) {
                return;
            }

            auto& entry = it->second;
            if (!entry.managed || !entry.softEnterActive || entry.softEnterHardStateApplied) {
                return;
            }

            entry.softEnterLastPressureSeen = Now();
            entry.softEnterLastPressureCauseFormID = causeFormID;
            entry.softEnterLastPressureOtherFormID = otherFormID;
            if (entry.softEnterPressureHitCount < 0xFFFFu) {
                ++entry.softEnterPressureHitCount;
            }

            spdlog::info(
                "[TFD][Victory][R421A] enemy soft defeated enter pressure recorded actor={:08X} cause={:08X} other={:08X} source={} reason={} hitCount={} quietWindowMs={} hardeningWillWait=1 teammateCausedEdgeGuard=1",
                softActorFormID,
                causeFormID,
                otherFormID,
                source ? source : "unknown",
                ReasonText(reason),
                static_cast<unsigned>(entry.softEnterPressureHitCount),
                kEnemySoftEnterPressureQuietWindow.count());
        }

        struct SoftEnterPressureProbe
        {
            bool active{ false };
            bool targetInCombat{ false };
            bool targetWeaponDrawn{ false };
            bool targetHasPlayerSideTarget{ false };
            bool targetHasNonPlayerCombatTarget{ false };
            bool playerSideAttackerFound{ false };
            bool stopIssued{ false };
            std::uint32_t playerSideAttackerCount{ 0 };
            RE::FormID firstAttackerFormID{ 0 };
            RE::FormID targetTargetFormID{ 0 };
        };

        struct VictoryEntryContextGate
        {
            bool allowed{ true };
            TFD::FlowController::RootFlow root{ TFD::FlowController::RootFlow::None };
            TFD::FlowController::RootFlow contextRoot{ TFD::FlowController::RootFlow::None };
            TFD::FlowController::DecisionGate gate{ TFD::FlowController::DecisionGate::None };
            TFD::FlowController::SubFlow sub{ TFD::FlowController::SubFlow::None };
            bool terminalResolved{ false };
            const char* reason{ "allowed" };
        };

        bool IsVictoryEntryBlockedRoot(TFD::FlowController::RootFlow root)
        {
            using TFD::FlowController::RootFlow;
            switch (root) {
            case RootFlow::Bleedout:
            case RootFlow::Captive:
            case RootFlow::Rescue:
            case RootFlow::Recovery:
            case RootFlow::LeftForDead:
                return true;
            default:
                return false;
            }
        }

        VictoryEntryContextGate BuildVictoryEntryContextGate()
        {
            VictoryEntryContextGate gate{};
            const auto snapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
            gate.root = snapshot.root;
            gate.contextRoot = snapshot.contextRoot;
            gate.gate = snapshot.gate;
            gate.sub = snapshot.sub;
            gate.terminalResolved = snapshot.terminalResolved;

            if (IsVictoryEntryBlockedRoot(snapshot.root)) {
                gate.allowed = false;
                gate.reason = "blocked_root";
                return gate;
            }
            if (IsVictoryEntryBlockedRoot(snapshot.contextRoot)) {
                gate.allowed = false;
                gate.reason = "blocked_context_root";
                return gate;
            }
            if (snapshot.gate == TFD::FlowController::DecisionGate::PlayerBleedout ||
                snapshot.gate == TFD::FlowController::DecisionGate::EnemyBleedout) {
                gate.allowed = false;
                gate.reason = "bleed_decision_gate";
                return gate;
            }
            if (snapshot.terminalResolved && snapshot.root != TFD::FlowController::RootFlow::None) {
                gate.allowed = false;
                gate.reason = "terminal_resolved_root";
                return gate;
            }
            return gate;
        }

        const char* RootName(TFD::FlowController::RootFlow root)
        {
            return TFD::FlowController::Controller::ToString(root);
        }

        const char* DecisionGateName(TFD::FlowController::DecisionGate gate)
        {
            return TFD::FlowController::Controller::ToString(gate);
        }

        const char* SubFlowName(TFD::FlowController::SubFlow sub)
        {
            return TFD::FlowController::Controller::ToString(sub);
        }

        SoftEnterPressureProbe ProbeSoftEnterPressureLocked(RE::Actor* actor)
        {
            SoftEnterPressureProbe result{};
            if (!actor || actor->IsDead() || actor->IsDisabled()) {
                return result;
            }

            result.targetInCombat = actor->IsInCombat();
            result.targetWeaponDrawn = actor->IsWeaponDrawn();

            auto* currentTarget = ResolveCurrentCombatTarget(actor);
            result.targetTargetFormID = currentTarget ? currentTarget->GetFormID() : 0u;
            result.targetHasPlayerSideTarget = currentTarget &&
                !currentTarget->IsDead() &&
                !currentTarget->IsDisabled() &&
                TFD::TeammateManager::IsPlayerSideTeammateActor(currentTarget);
            result.targetHasNonPlayerCombatTarget = HasNonPlayerCombatTarget(actor);

            auto* player = RE::PlayerCharacter::GetSingleton();
            TFD::Actor::ScanOptions options{};
            options.radius = (std::max)(kEnemySoftEnterPressureScanRadius, TFD::Settings::GetSweepRadius());
            options.npcOnly = false;
            const auto snapshot = TFD::Actor::BuildSnapshot(player, options);
            const auto actorFormID = actor->GetFormID();

            for (const auto& info : snapshot.actors) {
                auto* teammate = info.get();
                if (!teammate || teammate == actor || teammate == player || teammate->IsDead() || teammate->IsDisabled()) {
                    continue;
                }
                if (!info.playerSide || !info.standing) {
                    continue;
                }

                bool targetsSoftActor = info.currentTargetFormID == actorFormID;
                if (!targetsSoftActor) {
                    targetsSoftActor = info.getCurrentTarget() == actor;
                }
                if (!targetsSoftActor) {
                    continue;
                }

                result.active = true;
                result.playerSideAttackerFound = true;
                ++result.playerSideAttackerCount;
                if (result.firstAttackerFormID == 0) {
                    result.firstAttackerFormID = teammate->GetFormID();
                }

                // Local suppression only: stop a converted teammate from continuing
                // to land extra hits on an actor that is already pending Victory defeated hardening.
                teammate->StopCombat();
                teammate->GetActorRuntimeData().currentCombatTarget = RE::ActorHandle{};
                result.stopIssued = true;
            }

            if (result.targetHasPlayerSideTarget) {
                result.active = true;
                actor->StopCombat();
                actor->GetActorRuntimeData().currentCombatTarget = RE::ActorHandle{};
                result.stopIssued = true;
            }

            return result;
        }

        bool IsVisualGraphUnsafeForBleedoutStartLocked(RE::Actor* actor, EnemyEntry& entry, Clock::time_point now, RE::FormID& targetFormID, std::int64_t& quietRemainingMs, const char*& reason)
        {
            targetFormID = 0u;
            quietRemainingMs = 0;
            reason = "safe";
            if (!actor || actor->IsDisabled() || actor->IsDead()) {
                reason = "invalid_actor";
                return false;
            }

            if (entry.softEnterLastPressureSeen.time_since_epoch().count() != 0) {
                const auto quietUntil = entry.softEnterLastPressureSeen + kEnemyVisualGraphSafeQuietWindow;
                if (now < quietUntil) {
                    quietRemainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(quietUntil - now).count();
                    reason = "recent_hit_pressure";
                    return true;
                }
            }

            auto* currentTarget = ResolveCurrentCombatTarget(actor);
            if (currentTarget && !currentTarget->IsDead() && !currentTarget->IsDisabled()) {
                targetFormID = currentTarget->GetFormID();
                const bool targetsPlayer = currentTarget == RE::PlayerCharacter::GetSingleton();
                const bool targetsPlayerSide = targetsPlayer || TFD::TeammateManager::IsPlayerSideTeammateActor(currentTarget);
                if (!targetsPlayerSide && actor->IsInCombat() && actor->IsWeaponDrawn()) {
                    entry.npcCombatDefeat = true;
                }
                if (targetsPlayerSide && actor->IsInCombat() && actor->IsWeaponDrawn()) {
                    reason = targetsPlayer ? "combat_weapon_player_target" : "combat_weapon_player_side_target";
                    return true;
                }
            }

            return false;
        }

        bool MaintainVisualFirstBleedoutStartLocked(RE::Actor* actor, EnemyEntry& entry, Clock::time_point now)
        {
            if (!actor || actor->IsDisabled() || actor->IsDead()) {
                return false;
            }

            if (!entry.visualBleedoutStartDecisionLogged) {
                if (kEnemyDefeatedVisualBleedoutEnabled) {
                    if (entry.visualBleedoutStartDue.time_since_epoch().count() != 0 &&
                        now < entry.visualBleedoutStartDue) {
                        if (entry.visualBleedoutGraphSafeLastHoldLog.time_since_epoch().count() == 0 ||
                            now >= entry.visualBleedoutGraphSafeLastHoldLog + std::chrono::milliseconds(350)) {
                            entry.visualBleedoutGraphSafeLastHoldLog = now;
                            spdlog::info(
                                "[TFD][Victory][P33N] enemy visual graph-safe held actor={:08X} hpPct={:.1f} threshold={:.1f} reason=initial_settle remainingMs={} actorInCombat={} actorWeaponDrawn={} hardeningDelayed=1 softEnter=1 graphSafe=1",
                                actor->GetFormID(),
                                GetActorHealthPct(actor),
                                entry.thresholdPct,
                                std::chrono::duration_cast<std::chrono::milliseconds>(entry.visualBleedoutStartDue - now).count(),
                                actor->IsInCombat() ? 1 : 0,
                                actor->IsWeaponDrawn() ? 1 : 0);
                        }
                        return true;
                    }

                    RE::FormID unsafeTargetFormID = 0u;
                    std::int64_t quietRemainingMs = 0;
                    const char* unsafeReason = "safe";
                    const bool unsafe = IsVisualGraphUnsafeForBleedoutStartLocked(actor, entry, now, unsafeTargetFormID, quietRemainingMs, unsafeReason);
                    const bool canHoldForGraph = entry.visualBleedoutStartMaxDue.time_since_epoch().count() == 0 ||
                        now < entry.visualBleedoutStartMaxDue;
                    if (unsafe && canHoldForGraph) {
                        if (entry.visualBleedoutGraphSafeDeferrals < 0xFFu) {
                            ++entry.visualBleedoutGraphSafeDeferrals;
                        }
                        entry.visualBleedoutLastUnsafeTargetFormID = unsafeTargetFormID;
                        entry.visualBleedoutStartDue = now + kEnemyVisualGraphSafeRetryDelay;
                        if (entry.visualBleedoutGraphSafeLastHoldLog.time_since_epoch().count() == 0 ||
                            now >= entry.visualBleedoutGraphSafeLastHoldLog + std::chrono::milliseconds(350)) {
                            entry.visualBleedoutGraphSafeLastHoldLog = now;
                            spdlog::info(
                                "[TFD][Victory][P33N] enemy visual graph-safe held actor={:08X} hpPct={:.1f} threshold={:.1f} reason={} unsafeTarget={:08X} quietRemainingMs={} deferrals={} retryDelayMs={} maxDelayMs={} actorInCombat={} actorWeaponDrawn={} hardeningDelayed=1 softEnter=1 graphSafe=1",
                                actor->GetFormID(),
                                GetActorHealthPct(actor),
                                entry.thresholdPct,
                                unsafeReason,
                                unsafeTargetFormID,
                                quietRemainingMs,
                                static_cast<unsigned>(entry.visualBleedoutGraphSafeDeferrals),
                                kEnemyVisualGraphSafeRetryDelay.count(),
                                kEnemyVisualGraphSafeMaxDelay.count(),
                                actor->IsInCombat() ? 1 : 0,
                                actor->IsWeaponDrawn() ? 1 : 0);
                        }
                        return true;
                    }

                    if (unsafe && !canHoldForGraph) {
                        spdlog::warn(
                            "[TFD][Victory][P33N] enemy visual graph-safe timeout actor={:08X} hpPct={:.1f} threshold={:.1f} reason={} unsafeTarget={:08X} deferrals={} action=send_bleedoutstart hardeningDelayed=1 softEnter=1 graphSafe=1",
                            actor->GetFormID(),
                            GetActorHealthPct(actor),
                            entry.thresholdPct,
                            unsafeReason,
                            unsafeTargetFormID,
                            static_cast<unsigned>(entry.visualBleedoutGraphSafeDeferrals));
                    }

                    if (entry.visualBleedoutStartAttempts < 0xFFu) {
                        ++entry.visualBleedoutStartAttempts;
                    }
                    const bool sent = actor->NotifyAnimationGraph("BleedoutStart");
                    entry.visualBleedoutStarted = sent;
                    entry.visualBleedoutStopSent = false;

                    if (!sent && entry.visualBleedoutStartAttempts < kEnemyVisualFirstMaxAttempts) {
                        entry.visualBleedoutStartPending = true;
                        entry.visualBleedoutStartDue = now + kEnemyVisualFirstRetryDelay;
                        spdlog::info(
                            "[TFD][Victory][P33N] enemy graph-safe BleedoutStart retry armed actor={:08X} attempt={} hpPct={:.1f} threshold={:.1f} actorInCombat={} actorWeaponDrawn={} retryDelayMs={} hardeningDelayed=1 softEnter=1 graphSafe=1",
                            actor->GetFormID(),
                            static_cast<unsigned>(entry.visualBleedoutStartAttempts),
                            GetActorHealthPct(actor),
                            entry.thresholdPct,
                            actor->IsInCombat() ? 1 : 0,
                            actor->IsWeaponDrawn() ? 1 : 0,
                            kEnemyVisualFirstRetryDelay.count());
                        return true;
                    }

                    entry.visualBleedoutStartDecisionLogged = true;
                    entry.visualBleedoutStartPending = true;
                    entry.visualBleedoutStartDue = now + kEnemyVisualFirstHardeningSettleDelay;
                    if (entry.npcCombatDefeat) {
                        const auto npcSettleDue = now + kEnemyNpcCombatHardeningSettleDelay;
                        if (entry.softEnterHardStateDue.time_since_epoch().count() == 0 ||
                            entry.softEnterHardStateDue < npcSettleDue) {
                            entry.softEnterHardStateDue = npcSettleDue;
                        }
                        if (entry.softEnterHardStateMaxDue.time_since_epoch().count() == 0 ||
                            entry.softEnterHardStateMaxDue < npcSettleDue + kEnemySoftEnterPressureMaxExtraDelay) {
                            entry.softEnterHardStateMaxDue = npcSettleDue + kEnemySoftEnterPressureMaxExtraDelay;
                        }
                    }
                    spdlog::info(
                        "[TFD][Victory][P33N] enemy graph-safe BleedoutStart decided actor={:08X} sent={} attempts={} hpPct={:.1f} threshold={:.1f} actorInCombat={} actorWeaponDrawn={} graphDeferrals={} lastUnsafeTarget={:08X} hardeningDelayed=1 visualSettleMs={} softHardeningDelayMs={} softEnter=1 graphSafe=1",
                        actor->GetFormID(),
                        sent ? 1 : 0,
                        static_cast<unsigned>(entry.visualBleedoutStartAttempts),
                        GetActorHealthPct(actor),
                        entry.thresholdPct,
                        actor->IsInCombat() ? 1 : 0,
                        actor->IsWeaponDrawn() ? 1 : 0,
                        static_cast<unsigned>(entry.visualBleedoutGraphSafeDeferrals),
                        entry.visualBleedoutLastUnsafeTargetFormID,
                        kEnemyVisualFirstHardeningSettleDelay.count(),
                        std::chrono::duration_cast<std::chrono::milliseconds>(kEnemySoftEnterHardStateDelay).count());
                    return true;
                }

                entry.visualBleedoutStarted = false;
                entry.visualBleedoutStopSent = false;
                entry.visualBleedoutStartPending = false;
                entry.visualBleedoutStartDecisionLogged = true;
                entry.visualBleedoutStartDue = {};
                spdlog::info(
                    "[TFD][Victory][P33N] enemy graph-safe BleedoutStart skipped actor={:08X} reason=disabled hardeningContinues=1 softEnter=1 graphSafe=1",
                    actor->GetFormID());
                return false;
            }

            if (entry.visualBleedoutStartPending) {
                if (entry.visualBleedoutStartDue.time_since_epoch().count() != 0 &&
                    now < entry.visualBleedoutStartDue) {
                    return true;
                }
                entry.visualBleedoutStartPending = false;
                entry.visualBleedoutStartDue = {};
            }

            return false;
        }

        void ClearPleasureReturnReassertLocked(EnemyEntry& entry)
        {
            entry.pleasureReturnReassertActive = false;
            entry.pleasureReturnReassertAttempts = 0;
            entry.pleasureReturnReassertNextDue = {};
            entry.pleasureReturnReassertUntil = {};
        }

        bool MaintainPleasureReturnBleedoutReassertLocked(RE::Actor* actor, EnemyEntry& entry, Clock::time_point now)
        {
            if (!entry.pleasureReturnReassertActive) {
                return entry.pleasureReturnPackageHoldActive;
            }

            if (!actor || actor->IsDisabled() || actor->IsDead()) {
                ClearPleasureReturnReassertLocked(entry);
                entry.pleasureReturnPackageHoldActive = false;
                return false;
            }

            if (entry.deadline.time_since_epoch().count() != 0) {
                auto until = entry.deadline;
                if (until > now + kPleasureReturnBleedoutReassertStopBeforeTimeout) {
                    until -= kPleasureReturnBleedoutReassertStopBeforeTimeout;
                }
                if (entry.pleasureReturnReassertUntil.time_since_epoch().count() == 0 ||
                    entry.pleasureReturnReassertUntil < until) {
                    entry.pleasureReturnReassertUntil = until;
                }
            }

            if (entry.pleasureReturnReassertNextDue.time_since_epoch().count() == 0) {
                entry.pleasureReturnReassertNextDue = now + kPleasureReturnBleedoutReassertInitialDelay;
            }

            const bool beforeUntil = entry.pleasureReturnReassertUntil.time_since_epoch().count() == 0 ||
                now <= entry.pleasureReturnReassertUntil;
            const bool canAttempt = entry.pleasureReturnReassertAttempts < kPleasureReturnBleedoutReassertMaxAttempts;

            if (beforeUntil && canAttempt) {
                if (now >= entry.pleasureReturnReassertNextDue) {
                    if (entry.pleasureReturnReassertAttempts < 0xFFu) {
                        ++entry.pleasureReturnReassertAttempts;
                    }
                    const auto attemptNo = entry.pleasureReturnReassertAttempts;
                    const bool sent = actor->NotifyAnimationGraph("BleedoutStart");
                    entry.visualBleedoutStarted = entry.visualBleedoutStarted || sent;
                    entry.visualBleedoutStopSent = false;
                    // P55A: sent=1 only means the animation event was accepted by the graph.
                    // P54A proved that accepted events can still be swallowed by late OStim/XPMSE/OSED
                    // cleanup. Keep pulsing through the post-OStim burst window instead of stopping
                    // after the first accepted event.
                    entry.pleasureReturnReassertNextDue = now + kPleasureReturnBleedoutReassertInterval;

                    const auto remainingMs = entry.pleasureReturnReassertUntil.time_since_epoch().count() != 0 &&
                        entry.pleasureReturnReassertUntil > now ?
                        std::chrono::duration_cast<std::chrono::milliseconds>(entry.pleasureReturnReassertUntil - now).count() :
                        0LL;
                    spdlog::info(
                        "[TFD][Victory][P56A] Pleasure return delayed BleedoutStart burst actor={:08X} sent={} attempt={} maxAttempts={} hpPct={:.1f} threshold={:.1f} remainingMs={} delayedBurstStartMs={} packageHeld=1 mirrorHeld=1 evaluateHeld=1 continueAfterSuccess=1 sentIsNotVisualProof=1",
                        actor->GetFormID(),
                        sent ? 1 : 0,
                        static_cast<unsigned>(attemptNo),
                        static_cast<unsigned>(kPleasureReturnBleedoutReassertMaxAttempts),
                        GetActorHealthPct(actor),
                        entry.thresholdPct,
                        remainingMs,
                        kPleasureReturnDelayedBleedoutStartDelay.count());
                }
                return true;
            }

            // P56A: even after delayed BleedoutStart burst attempts are accepted, exhausted,
            // or manually stopped by a new player action, keep the defeated mirror/package and
            // EvaluatePackage held. Native Victory owns manual dialogue and timeout from
            // g_enemyEntries; the defeated package is not needed during this post-OStim grace
            // and can pull the actor standing.
            return entry.pleasureReturnPackageHoldActive || entry.pleasureReturnReassertActive;
        }

        bool StopPleasureReturnBurstForPlayerActionLocked(
            RE::Actor* actor,
            EnemyEntry& entry,
            std::string_view reason,
            bool forceStop)
        {
            if (!entry.pleasureReturnReassertActive) {
                return false;
            }

            const auto* actorState = actor ? actor->AsActorState() : nullptr;
            const bool actorBleeding = actorState && actorState->IsBleedingOut();
            const auto attempts = entry.pleasureReturnReassertAttempts;
            if (!forceStop && !actorBleeding && attempts < kPleasureReturnBleedoutDialogueStopMinAttempts) {
                return false;
            }

            ClearPleasureReturnReassertLocked(entry);
            entry.pleasureReturnVisualHoldActive = false;
            entry.pleasureReturnVisualHoldLogged = false;
            entry.pleasureReturnPackageHoldActive = true;
            entry.initialPackageRefreshDone = false;
            entry.packageRefreshAfterHardeningPending = true;

            spdlog::info(
                "[TFD][Victory][P56A] Pleasure return delayed burst stopped for player action actor={:08X} reason={} forceStop={} attempts={} actorBleeding={} visualStarted={} packageHeld=1 mirrorHeld=1 evaluateHeld=1",
                actor ? actor->GetFormID() : 0u,
                ReasonText(reason),
                forceStop ? 1 : 0,
                static_cast<unsigned>(attempts),
                actorBleeding ? 1 : 0,
                entry.visualBleedoutStarted ? 1 : 0);
            return true;
        }

        void MaintainEnemyState(RE::Actor* actor, EnemyEntry& entry, bool initialEntry)
        {
            if (!actor || actor->IsDisabled() || actor->IsDead()) {
                return;
            }

            if (kEnemySoftEnterHardStateDelayEnabled &&
                entry.softEnterActive &&
                !entry.softEnterHardStateApplied) {
                const auto now = Now();

                // P33N: normal enemy defeat captures immediately, then sends BleedoutStart at the first graph-safe quiet edge.
                // P55A: Victory Pleasure return is different. OStim/body graph cleanup can accept the first
                // BleedoutStart too early, causing the actor to collapse, blink standing, then collapse again.
                // Suppress that immediate visual pass and let the delayed post-OStim BleedoutStart below become
                // the official return visual.
                const bool pleasureReturnDelayedOfficialBleedout =
                    entry.pleasureReturnReassertActive ||
                    entry.pleasureReturnVisualHoldActive ||
                    entry.pleasureReturnPackageHoldActive;
                if (pleasureReturnDelayedOfficialBleedout && !entry.visualBleedoutStartDecisionLogged) {
                    entry.visualBleedoutStartPending = false;
                    entry.visualBleedoutStartDecisionLogged = true;
                    entry.visualBleedoutStartAttempts = 0;
                    entry.visualBleedoutGraphSafeDeferrals = 0;
                    entry.visualBleedoutStartDue = {};
                    entry.visualBleedoutStartMaxDue = {};
                    entry.visualBleedoutGraphSafeLastHoldLog = Clock::time_point{};
                    entry.visualBleedoutLastUnsafeTargetFormID = 0;
                    spdlog::info(
                        "[TFD][Victory][P56A] Pleasure return immediate BleedoutStart suppressed actor={:08X} hpPct={:.1f} threshold={:.1f} delayedBurstStartMs={} hardStopCombat=0 hardMirror=0 hardEvaluate=0 reason=post_ostim_cleanup_window",
                        actor->GetFormID(),
                        GetActorHealthPct(actor),
                        entry.thresholdPct,
                        kPleasureReturnDelayedBleedoutStartDelay.count());
                }
                if (!pleasureReturnDelayedOfficialBleedout && MaintainVisualFirstBleedoutStartLocked(actor, entry, now)) {
                    return;
                }

                if (entry.softEnterHardStateDue.time_since_epoch().count() != 0 &&
                    now < entry.softEnterHardStateDue) {
                    if (initialEntry || !entry.softEnterPendingLogged) {
                        entry.softEnterPendingLogged = true;
                        spdlog::info(
                            "[TFD][Victory][P33N] enemy hardening pending after graph-safe visual actor={:08X} hpPct={:.1f} threshold={:.1f} delayMs={} hardStopCombat=0 hardMirror=0 hardEvaluate=0 graphSafe=1 softEnter=1",
                            actor->GetFormID(),
                            GetActorHealthPct(actor),
                            entry.thresholdPct,
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                entry.softEnterHardStateDue - now)
                                .count());
                    }
                    if (entry.npcCombatDefeat && !entry.npcCombatHardeningSettleLogged) {
                        entry.npcCombatHardeningSettleLogged = true;
                        spdlog::info(
                            "[TFD][Victory][P50A] NPC-caused defeated hardening delayed actor={:08X} hpPct={:.1f} threshold={:.1f} remainingMs={} target={:08X} aliasMirrorDelayed=1 packageHeld=1 reason=npc_combat_bleedout_settle",
                            actor->GetFormID(),
                            GetActorHealthPct(actor),
                            entry.thresholdPct,
                            std::chrono::duration_cast<std::chrono::milliseconds>(entry.softEnterHardStateDue - now).count(),
                            ResolveCurrentCombatTarget(actor) ? ResolveCurrentCombatTarget(actor)->GetFormID() : 0u);
                    }
                    return;
                }

                const auto pressure = ProbeSoftEnterPressureLocked(actor);
                const bool hasRecentPressure = entry.softEnterLastPressureSeen.time_since_epoch().count() != 0 &&
                    now < entry.softEnterLastPressureSeen + kEnemySoftEnterPressureQuietWindow;
                if (pressure.targetHasNonPlayerCombatTarget ||
                    (hasRecentPressure && actor->IsInCombat() && !pressure.targetHasPlayerSideTarget)) {
                    entry.npcCombatDefeat = true;
                }
                if (entry.npcCombatDefeat && !entry.npcCombatHardeningSettleLogged) {
                    entry.npcCombatHardeningSettleLogged = true;
                    entry.softEnterHardStateDue = now + kEnemyNpcCombatHardeningSettleDelay;
                    if (entry.softEnterHardStateMaxDue.time_since_epoch().count() == 0 ||
                        entry.softEnterHardStateMaxDue < entry.softEnterHardStateDue + kEnemySoftEnterPressureMaxExtraDelay) {
                        entry.softEnterHardStateMaxDue = entry.softEnterHardStateDue + kEnemySoftEnterPressureMaxExtraDelay;
                    }
                    spdlog::info(
                        "[TFD][Victory][P50A] NPC-caused defeated hardening delayed actor={:08X} hpPct={:.1f} threshold={:.1f} remainingMs={} target={:08X} aliasMirrorDelayed=1 packageHeld=1 reason=npc_combat_late_pressure",
                        actor->GetFormID(),
                        GetActorHealthPct(actor),
                        entry.thresholdPct,
                        kEnemyNpcCombatHardeningSettleDelay.count(),
                        pressure.targetTargetFormID);
                    return;
                }
                const bool canDeferPressure = entry.softEnterHardStateMaxDue.time_since_epoch().count() == 0 ||
                    now < entry.softEnterHardStateMaxDue;

                if ((pressure.active || hasRecentPressure) && canDeferPressure) {
                    if (entry.softEnterPressureDeferrals < 0xFFu) {
                        ++entry.softEnterPressureDeferrals;
                    }
                    const auto quietRemainingMs = hasRecentPressure ?
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            (entry.softEnterLastPressureSeen + kEnemySoftEnterPressureQuietWindow) - now)
                            .count() :
                        0LL;
                    entry.softEnterHardStateDue = now + kEnemySoftEnterPressureRetryDelay;
                    spdlog::info(
                        "[TFD][Victory][P33N] enemy hardening deferred after graph-safe visual actor={:08X} hpPct={:.1f} threshold={:.1f} actorInCombat={} actorWeaponDrawn={} activePressure={} recentPressure={} quietRemainingMs={} playerSideAttackers={} firstAttacker={:08X} targetTarget={:08X} stopIssued={} pressureHits={} deferrals={} retryDelayMs={} maxExtraDelayMs={} graphSafe=1 teammateCausedEdgeGuard=1 npcCombatDefeat={}",
                        actor->GetFormID(),
                        GetActorHealthPct(actor),
                        entry.thresholdPct,
                        actor->IsInCombat() ? 1 : 0,
                        actor->IsWeaponDrawn() ? 1 : 0,
                        pressure.active ? 1 : 0,
                        hasRecentPressure ? 1 : 0,
                        quietRemainingMs,
                        pressure.playerSideAttackerCount,
                        pressure.firstAttackerFormID,
                        pressure.targetTargetFormID,
                        pressure.stopIssued ? 1 : 0,
                        static_cast<unsigned>(entry.softEnterPressureHitCount),
                        static_cast<unsigned>(entry.softEnterPressureDeferrals),
                        kEnemySoftEnterPressureRetryDelay.count(),
                        kEnemySoftEnterPressureMaxExtraDelay.count(),
                        entry.npcCombatDefeat ? 1 : 0);
                    return;
                }

                if ((pressure.active || hasRecentPressure) && !canDeferPressure) {
                    spdlog::warn(
                        "[TFD][Victory][P33N] enemy hardening pressure timeout actor={:08X} hpPct={:.1f} threshold={:.1f} activePressure={} recentPressure={} pressureHits={} deferrals={} action=force_hardening graphSafe=1",
                        actor->GetFormID(),
                        GetActorHealthPct(actor),
                        entry.thresholdPct,
                        pressure.active ? 1 : 0,
                        hasRecentPressure ? 1 : 0,
                        static_cast<unsigned>(entry.softEnterPressureHitCount),
                        static_cast<unsigned>(entry.softEnterPressureDeferrals));
                }

                // P33N: the graph-safe visual decision/pending settle is handled before
                // soft delay and pressure gates above. Hardening reaches this point only
                // after the visual attempt has either settled or fallen back.

                if (entry.pleasureReturnVisualHoldActive) {
                    entry.pleasureReturnVisualHoldActive = false;
                    entry.pleasureReturnVisualHoldLogged = false;
                    entry.initialPackageRefreshDone = false;
                    entry.packageRefreshAfterHardeningPending = true;
                    entry.pleasureReturnPackageHoldActive = true;
                    if (!entry.pleasureReturnReassertActive) {
                        entry.pleasureReturnReassertActive = true;
                        entry.pleasureReturnReassertAttempts = 0;
                        entry.pleasureReturnReassertNextDue = now + kPleasureReturnBleedoutReassertInitialDelay;
                        entry.pleasureReturnReassertUntil = entry.deadline.time_since_epoch().count() != 0 ?
                            entry.deadline - kPleasureReturnBleedoutReassertStopBeforeTimeout :
                            now + std::chrono::milliseconds(static_cast<int>(kEnemyKnockSeconds * 1000.0));
                    }
                    spdlog::info(
                        "[TFD][Victory][P56A] Pleasure return visual hold converted to delayed official bleedout actor={:08X} hpPct={:.1f} threshold={:.1f} aliasSlot={} factionApplied={} visualStarted={} delayedBurstStartMs={} retryIntervalMs={} maxAttempts={} packageHeld=1 mirrorHeld=1 evaluateHeld=1 reason=timed_second_stage",
                        actor->GetFormID(),
                        GetActorHealthPct(actor),
                        entry.thresholdPct,
                        entry.aliasSlot,
                        entry.factionApplied ? 1 : 0,
                        entry.visualBleedoutStarted ? 1 : 0,
                        kPleasureReturnDelayedBleedoutStartDelay.count(),
                        kPleasureReturnBleedoutReassertInterval.count(),
                        static_cast<unsigned>(kPleasureReturnBleedoutReassertMaxAttempts));
                }

                const bool pleasureReturnPackageHold =
                    entry.pleasureReturnReassertActive || entry.pleasureReturnPackageHoldActive;
                entry.softEnterHardStateApplied = true;
                spdlog::info(
                    "[TFD][Victory][P33N] enemy graph-safe visual hardening actor={:08X} hpPct={:.1f} threshold={:.1f} actorInCombat={} hardStopCombat=1 hardMirror={} hardEvaluate={} visualStarted={} visualSettleMs={} softEnter=1 npcCombatDefeat={} pleasureReturnReassert={}",
                    actor->GetFormID(),
                    GetActorHealthPct(actor),
                    entry.thresholdPct,
                    actor->IsInCombat() ? 1 : 0,
                    pleasureReturnPackageHold ? 0 : 1,
                    pleasureReturnPackageHold ? 0 : 1,
                    entry.visualBleedoutStarted ? 1 : 0,
                    kEnemyVisualFirstHardeningSettleDelay.count(),
                    entry.npcCombatDefeat ? 1 : 0,
                    pleasureReturnPackageHold ? 1 : 0);
            }

            ApplyRegenOverride(actor, entry);
            TFD::Actor::Ops::ApplyDefeatedEnemyPassiveOverride(actor, entry.savedAggression, entry.aggressionOverridden);

            if (entry.pleasureReturnVisualHoldActive) {
                entry.pleasureReturnVisualHoldActive = false;
                entry.pleasureReturnVisualHoldLogged = false;
                if (!entry.pleasureReturnReassertActive) {
                    entry.pleasureReturnReassertActive = true;
                    entry.pleasureReturnReassertAttempts = 0;
                    entry.pleasureReturnReassertNextDue = Now() + kPleasureReturnBleedoutReassertInitialDelay;
                    entry.pleasureReturnReassertUntil = entry.deadline.time_since_epoch().count() != 0 ?
                        entry.deadline - kPleasureReturnBleedoutReassertStopBeforeTimeout :
                        Now() + std::chrono::milliseconds(static_cast<int>(kEnemyKnockSeconds * 1000.0));
                }
                entry.initialPackageRefreshDone = false;
                entry.packageRefreshAfterHardeningPending = true;
                entry.pleasureReturnPackageHoldActive = true;
                spdlog::warn(
                    "[TFD][Victory][P56A] Pleasure return visual hold fallback converted to delayed official bleedout actor={:08X} hpPct={:.1f} threshold={:.1f} aliasSlot={} factionApplied={} visualStarted={} packageHeld=1 mirrorHeld=1 evaluateHeld=1 reason=hardening_already_applied",
                    actor->GetFormID(),
                    GetActorHealthPct(actor),
                    entry.thresholdPct,
                    entry.aliasSlot,
                    entry.factionApplied ? 1 : 0,
                    entry.visualBleedoutStarted ? 1 : 0);
            }

            const bool pleasureReturnPackageHold = MaintainPleasureReturnBleedoutReassertLocked(actor, entry, Now());

            if (!pleasureReturnPackageHold) {
                TFD::Actor::Ops::SyncDefeatedEnemyMirror(actor, entry.aliasSlot, entry.factionApplied);
            }

            if (actor->IsInCombat()) {
                actor->StopCombat();
            }
            if (auto* process = RE::ProcessLists::GetSingleton()) {
                process->StopCombatAndAlarmOnActor(actor, false);
            }

            // P33N: BleedoutStart is attempted at the first graph-safe quiet edge before Enemy alias/package hardening.
            // P55A: after OStim return, keep alias/package EvaluatePackage held while BleedoutStart is reasserted
            // through the post-OStim graph cleanup window. Native Victory state stays active, so manual greet
            // and timeout still work without relying on the defeated package as the visual owner.

            if (!pleasureReturnPackageHold && (initialEntry || entry.packageRefreshAfterHardeningPending) && !entry.initialPackageRefreshDone) {
                actor->EvaluatePackage(false, true);
                actor->EvaluatePackage(true, true);
                entry.initialPackageRefreshDone = true;
                entry.packageRefreshAfterHardeningPending = false;
            }
        }

        void ArmPleasureReturnBleedoutLocked(RE::Actor* actor, EnemyEntry& entry, Clock::time_point now, std::string_view reason)
        {
            if (!actor || actor->IsDisabled() || actor->IsDead()) {
                return;
            }

            (void)SetPleasureReturnBleedoutHealth(actor, entry.thresholdPct, reason);

            // P55A: post-OStim return must not send BleedoutStart immediately. P53A proved
            // that the first accepted event can be swallowed or collapse-blink the actor, and only
            // the later event around the post-OStim cleanup window settles. Keep the native Victory
            // entry as the owner, delay the BleedoutStart burst, and keep mirror/package
            // EvaluatePackage deferred through the grace window.

            entry.deadline = now + kPleasureReturnDelayedBleedoutStartDelay + std::chrono::milliseconds(
                static_cast<int>(kEnemyKnockSeconds * 1000.0));
            entry.autoDeathIssued = false;
            entry.fatalDamageApplied = false;

            entry.softEnterActive = true;
            entry.softEnterHardStateApplied = false;
            entry.softEnterPendingLogged = false;
            entry.softEnterHardStateDue = now + kEnemySoftEnterHardStateDelay;
            entry.softEnterHardStateMaxDue = entry.softEnterHardStateDue + kEnemySoftEnterPressureMaxExtraDelay;
            entry.softEnterLastPressureSeen = Clock::time_point{};
            entry.softEnterLastPressureCauseFormID = 0;
            entry.softEnterLastPressureOtherFormID = 0;
            entry.softEnterPressureHitCount = 0;
            entry.softEnterPressureDeferrals = 0;
            entry.npcCombatDefeat = false;
            entry.npcCombatHardeningSettleLogged = false;
            entry.pleasureReturnVisualHoldActive = true;
            entry.pleasureReturnVisualHoldLogged = false;
            entry.pleasureReturnReassertActive = true;
            entry.pleasureReturnPackageHoldActive = true;
            entry.pleasureReturnReassertAttempts = 0;
            entry.pleasureReturnReassertNextDue = now + kPleasureReturnBleedoutReassertInitialDelay;
            entry.pleasureReturnReassertUntil = entry.deadline.time_since_epoch().count() != 0 ?
                entry.deadline - kPleasureReturnBleedoutReassertStopBeforeTimeout :
                now + std::chrono::milliseconds(static_cast<int>(kEnemyKnockSeconds * 1000.0));
            entry.visualBleedoutStarted = false;
            entry.visualBleedoutStopSent = false;
            entry.visualBleedoutStartPending = false;
            entry.visualBleedoutStartDecisionLogged = false;
            entry.visualBleedoutStartAttempts = 0;
            entry.visualBleedoutGraphSafeDeferrals = 0;
            entry.visualBleedoutStartDue = {};
            entry.visualBleedoutStartMaxDue = {};
            entry.visualBleedoutGraphSafeLastHoldLog = Clock::time_point{};
            entry.visualBleedoutLastUnsafeTargetFormID = 0;
            entry.initialPackageRefreshDone = false;
            entry.packageRefreshAfterHardeningPending = true;

            spdlog::info(
                "[TFD][Victory][P56A] Pleasure return-to-bleedout armed actor={:08X} reason={} graceAfterDelayedBurstStart={:.1f} immediateVisualSuppressed=1 delayedBurstStartMs={} totalDeadlineDelayMs={} graphSafeMaxDelayMs={} hardStateDelayMs={} retryIntervalMs={} retryMaxAttempts={} lowHealthRearmed=1 mirrorDeferredThroughGrace=1 packageDeferredThroughGrace=1 evaluateDeferredThroughGrace=1",
                actor->GetFormID(),
                ReasonText(reason),
                kEnemyKnockSeconds,
                kPleasureReturnDelayedBleedoutStartDelay.count(),
                (kPleasureReturnDelayedBleedoutStartDelay + std::chrono::milliseconds(static_cast<int>(kEnemyKnockSeconds * 1000.0))).count(),
                kEnemyVisualGraphSafeMaxDelay.count(),
                std::chrono::duration_cast<std::chrono::milliseconds>(kEnemySoftEnterHardStateDelay).count(),
                kPleasureReturnBleedoutReassertInterval.count(),
                static_cast<unsigned>(kPleasureReturnBleedoutReassertMaxAttempts));
        }

        bool TryGetDefeatedEnemyStateHook(
            RE::Actor* actor,
            std::uint8_t* lockKindValue,
            bool* defeatedManaged,
            Clock::time_point* deadline)
        {
            if (!actor) {
                return false;
            }

            std::scoped_lock lk(g_lock);
            const auto it = g_enemyEntries.find(actor->GetFormID());
            if (it == g_enemyEntries.end()) {
                return false;
            }

            if (lockKindValue) {
                *lockKindValue = kEnemyLockKindValue;
            }
            if (defeatedManaged) {
                *defeatedManaged = it->second.managed;
            }
            if (deadline) {
                *deadline = it->second.deadline;
            }
            return true;
        }

        bool ReleaseEntryByIDLocked(RE::FormID formID, std::string_view reason, bool playGetUp)
        {
            const auto it = g_enemyEntries.find(formID);
            if (it == g_enemyEntries.end()) {
                return false;
            }

            if (g_session.active && g_session.selectedActorFormID == formID) {
                ResetSessionLocked("selected_enemy_released", false);
            }

            auto entry = it->second;
            g_enemyEntries.erase(it);

            auto actorSP = entry.handle.get();
            auto* actor = actorSP.get();
            const auto reasonText = ReasonText(reason);

            RestoreRegenOverride(actor, entry);

            if (actor && reasonText.find("recruit") != std::string::npos) {
                TFD::Actor::Ops::SuppressDefeatedEnemyReentry(
                    actor,
                    kDefeatedReentrySuppressSeconds,
                    reasonText.c_str());
            }

            TFD::Actor::Ops::ClearDefeatedEnemyState(
                actor,
                entry.aliasSlot,
                entry.factionApplied,
                entry.managed,
                entry.autoDeathIssued,
                entry.fatalDamageApplied,
                entry.deadline,
                entry.savedAggression,
                entry.aggressionOverridden,
                reasonText.c_str());

            if (playGetUp && actor && !actor->IsDead() && !actor->IsDisabled()) {
                // R394A preserves the confirmed no-fake-bleedout baseline. Generic
                // release only refreshes the package; outcome-specific physical
                // Get Up will be reconstructed later behind one Victory transition.
                actor->EvaluatePackage(false, true);
                actor->EvaluatePackage(true, true);
            }

            RefreshConditionStateLocked();

            spdlog::info(
                "[TFD][Victory][R394A] defeated enemy released actor={:08X} reason={} packageRefresh={}",
                formID,
                reasonText,
                playGetUp ? 1 : 0);
            return true;
        }

        void ClearAllEnemyEntriesLocked(std::string_view reason)
        {
            const auto reasonText = ReasonText(reason);
            ResetSessionLocked(reasonText, false);
            std::vector<RE::FormID> ids{};
            ids.reserve(g_enemyEntries.size());
            for (const auto& [formID, _entry] : g_enemyEntries) {
                ids.push_back(formID);
            }
            for (const auto formID : ids) {
                (void)ReleaseEntryByIDLocked(formID, reasonText, false);
            }
            g_enemyEntries.clear();
            g_lastLifecycleLog = {};
            g_recruitHitDiagnostic = RecruitHitDiagnosticState{};
            TFD::Actor::Ops::ClearAllDefeatedEnemyMirrors(reasonText.c_str());
            RefreshConditionStateLocked();
        }

        void WorkerLoop()
        {
            while (g_running.load(std::memory_order_acquire)) {
                if (!g_tickPending.test_and_set(std::memory_order_acq_rel)) {
                    if (auto* task = SKSE::GetTaskInterface()) {
                        task->AddTask([]() {
                            struct TickGuard
                            {
                                ~TickGuard()
                                {
                                    g_tickPending.clear(std::memory_order_release);
                                }
                            } guard;
                            Tick();
                        });
                    }
                    else {
                        g_tickPending.clear(std::memory_order_release);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }

    const char* ToString(SessionPhase phase)
    {
        switch (phase) {
        case SessionPhase::Empty:
            return "Empty";
        case SessionPhase::OpeningDialogue:
            return "OpeningDialogue";
        case SessionPhase::DialogueOpen:
            return "DialogueOpen";
        case SessionPhase::AwaitingChoiceCommit:
            return "AwaitingChoiceCommit";
        case SessionPhase::KillCommitted:
            return "KillCommitted";
        case SessionPhase::LootCommitted:
            return "LootCommitted";
        case SessionPhase::RecruitCommitted:
            return "RecruitCommitted";
        case SessionPhase::PleasureCommitted:
            return "PleasureCommitted";
        default:
            return "Unknown";
        }
    }

    bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm)
    {
        if (!a_vm) {
            return false;
        }

        a_vm->RegisterFunction("RequestKill", "TFDVictoryNative", PapyrusRequestKill);
        a_vm->RegisterFunction("RequestLoot", "TFDVictoryNative", PapyrusRequestLoot);
        a_vm->RegisterFunction("RequestRecruit", "TFDVictoryNative", PapyrusRequestRecruit);
        a_vm->RegisterFunction("RequestPleasure", "TFDVictoryNative", PapyrusRequestPleasure);
        a_vm->RegisterFunction("CompletePleasureHandoff", "TFDVictoryNative", PapyrusCompletePleasureHandoff);
        a_vm->RegisterFunction("CompletePleasureScene", "TFDVictoryNative", PapyrusCompletePleasureScene);
        spdlog::info("[TFD][Victory][P33P] Papyrus native registered functions=RequestKill,RequestLoot,RequestRecruit,RequestPleasure,CompletePleasureHandoff,CompletePleasureScene owner=TFDVictory");
        return true;
    }

    void Install()
    {
        if (g_installed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        g_loadTransition.store(false, std::memory_order_release);
        g_running.store(true, std::memory_order_release);
        {
            std::scoped_lock lk(g_lock);
            ClearAllEnemyEntriesLocked("install");
            g_nextSessionID = 1;
            g_victoryGreetInfo = nullptr;
            SetConditionState(0);
        }

        TFD::Actor::Ops::InstallDefeatedEnemyStateHooks(
            TFD::Actor::Ops::DefeatedEnemyStateHooks{ &TryGetDefeatedEnemyStateHook });
        g_worker = std::thread([]() { WorkerLoop(); });

        spdlog::info(
            "[TFD][Victory][R421A] enemy defeat owner installed policy=threshold_notify registry_countdown_autodeath_owner selected_actor_session_greet_cancel_kill_staged_death loot_native_inventory_dispatch_container_observer_controlled_release recruit_pre_getup_dehostile_defeated_clear_teammate_handoff package_hold_defeated_delayed_bleedoutstart delayed_bleedoutstart no_reassert no_forcegreet no_generic_flow no_node_rebuild recruit_hit_diagnostic_passthrough soft_defeated_enter_R421A teammate_pressure_quiet_window victory_context_guard_P32B victory_pleasure_dialogue_async_close_guard_P36A pleasure_return_low_health_bleedout_P34B pleasure_return_delayed_burst_P56A npc_combat_defeat_hardening_settle_P50A");
    }

    void Shutdown()
    {
        if (!g_installed.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        g_running.store(false, std::memory_order_release);
        if (g_worker.joinable()) {
            g_worker.join();
        }
        g_tickPending.clear(std::memory_order_release);
        TFD::Actor::Ops::InstallDefeatedEnemyStateHooks(TFD::Actor::Ops::DefeatedEnemyStateHooks{});
        {
            std::scoped_lock lk(g_lock);
            ClearAllEnemyEntriesLocked("shutdown");
            SetConditionState(0);
        }
        spdlog::info("[TFD][Victory][R394A] enemy defeat owner shutdown");
    }

    bool NotifyEnemyBelowThreshold(RE::Actor* actor, float thresholdPct, std::string_view reason)
    {
        if (!g_installed.load(std::memory_order_acquire) ||
            !TFD::Settings::GetEnabled() ||
            g_loadTransition.load(std::memory_order_acquire) ||
            !actor || actor->IsDisabled() || actor->IsDead()) {
            return false;
        }

        const float clampedThreshold = std::clamp(thresholdPct, 2.0f, 95.0f);
        if (GetActorHealthPct(actor) > clampedThreshold) {
            return false;
        }

        const auto contextGate = BuildVictoryEntryContextGate();
        std::scoped_lock lk(g_lock);
        const auto formID = actor->GetFormID();
        if (auto it = g_enemyEntries.find(formID); it != g_enemyEntries.end()) {
            it->second.thresholdPct = clampedThreshold;
            if (HasNonPlayerCombatTarget(actor)) {
                it->second.npcCombatDefeat = true;
            }
            if (!contextGate.allowed) {
                spdlog::info(
                    "[TFD][Victory][P32B] existing enemy threshold ignored and released actor={:08X} reason={} root={} context={} gate={} sub={} terminal={} noHardening=1 noBleedoutStart=1",
                    formID,
                    contextGate.reason,
                    RootName(contextGate.root),
                    RootName(contextGate.contextRoot),
                    DecisionGateName(contextGate.gate),
                    SubFlowName(contextGate.sub),
                    contextGate.terminalResolved ? 1 : 0);
                (void)ReleaseEntryByIDLocked(formID, contextGate.reason, false);
                return false;
            }
            MaintainEnemyState(actor, it->second, false);
            RefreshConditionStateLocked();
            return true;
        }

        if (!contextGate.allowed) {
            spdlog::info(
                "[TFD][Victory][P32B] enemy threshold ignored actor={:08X} hpPct={:.1f} threshold={:.1f} reason={} root={} context={} gate={} sub={} terminal={} noEntry=1 noHardening=1 noBleedoutStart=1",
                formID,
                GetActorHealthPct(actor),
                clampedThreshold,
                contextGate.reason,
                RootName(contextGate.root),
                RootName(contextGate.contextRoot),
                DecisionGateName(contextGate.gate),
                SubFlowName(contextGate.sub),
                contextGate.terminalResolved ? 1 : 0);
            return false;
        }

        if (!TFD::Actor::Ops::IsDefeatedEnemyCandidate(actor)) {
            return false;
        }

        EnemyEntry entry{};
        entry.handle = actor->GetHandle();
        entry.thresholdPct = clampedThreshold;
        entry.npcCombatDefeat = HasNonPlayerCombatTarget(actor);
        entry.npcCombatHardeningSettleLogged = false;
        entry.deadline = Now() + std::chrono::milliseconds(
            static_cast<int>(kEnemyKnockSeconds * 1000.0));

        auto [it, inserted] = g_enemyEntries.emplace(formID, std::move(entry));
        if (!inserted) {
            return false;
        }

        if (kEnemySoftEnterHardStateDelayEnabled) {
            it->second.softEnterActive = true;
            it->second.softEnterHardStateApplied = false;
            it->second.softEnterPendingLogged = false;
            const auto softEnterNow = Now();
            it->second.softEnterHardStateDue = softEnterNow + kEnemySoftEnterHardStateDelay;
            it->second.softEnterHardStateMaxDue = it->second.softEnterHardStateDue + kEnemySoftEnterPressureMaxExtraDelay;
            it->second.softEnterLastPressureSeen = Clock::time_point{};
            it->second.softEnterLastPressureCauseFormID = 0;
            it->second.softEnterLastPressureOtherFormID = 0;
            it->second.softEnterPressureHitCount = 0;
            it->second.softEnterPressureDeferrals = 0;
            it->second.visualBleedoutStartAttempts = 0;
            it->second.visualBleedoutGraphSafeDeferrals = 0;
            it->second.visualBleedoutStartPending = true;
            it->second.visualBleedoutStartDue = softEnterNow + kEnemyVisualGraphSafeInitialDelay;
            it->second.visualBleedoutStartMaxDue = softEnterNow + kEnemyVisualGraphSafeMaxDelay;
            it->second.visualBleedoutGraphSafeLastHoldLog = Clock::time_point{};
            it->second.visualBleedoutLastUnsafeTargetFormID = 0;
        }

        MaintainEnemyState(actor, it->second, true);
        RefreshConditionStateLocked();
        spdlog::info(
            "[TFD][Victory][P33N] enemy threshold committed actor={:08X} hpPct={:.1f} threshold={:.1f} countdown={:.1f} reason={} victoryState=1 visualStartDelayMs={} graphSafeMaxDelayMs={} hardStateDelayMs={} initialStopCombat=0 initialMirror=0 initialEvaluate=0 graphSafeBleedoutStart=1 graphSafe=1",
            formID,
            GetActorHealthPct(actor),
            clampedThreshold,
            kEnemyKnockSeconds,
            ReasonText(reason),
            kEnemyVisualGraphSafeInitialDelay.count(),
            kEnemyVisualGraphSafeMaxDelay.count(),
            std::chrono::duration_cast<std::chrono::milliseconds>(
                kEnemySoftEnterHardStateDelay)
                .count());
        return true;
    }

    void Tick()
    {
        if (!g_installed.load(std::memory_order_acquire) ||
            !TFD::Settings::GetEnabled() ||
            g_loadTransition.load(std::memory_order_acquire)) {
            return;
        }

        const auto contextGate = BuildVictoryEntryContextGate();
        std::scoped_lock lk(g_lock);
        const auto now = Now();
        TickRecruitHitDiagnosticLocked(now);
        MaintainPleasureDialogueCloseGuardLocked(now);

        if (g_session.active &&
            g_session.phase == SessionPhase::OpeningDialogue &&
            g_sessionOpenDeadline.time_since_epoch().count() != 0 &&
            now >= g_sessionOpenDeadline) {
            ResetSessionLocked("dialogue_open_timeout", true);
        }

        if (g_session.active &&
            g_session.phase == SessionPhase::AwaitingChoiceCommit &&
            g_choiceCommitDeadline.time_since_epoch().count() != 0 &&
            now >= g_choiceCommitDeadline) {
            ResetSessionLocked("dialogue_closed_without_committed_outcome", true);
        }

        TickCommittedKillLocked(now);
        TickCommittedLootLocked(now);
        TickCommittedRecruitLocked(now);

        std::vector<std::tuple<RE::FormID, std::string, bool>> releases{};
        bool servicedAny = false;

        for (auto& [formID, entry] : g_enemyEntries) {
            servicedAny = true;
            auto actorSP = entry.handle.get();
            auto* actor = actorSP.get();

            const bool ownedKillEntry =
                g_session.active &&
                g_session.phase == SessionPhase::KillCommitted &&
                g_session.selectedActorFormID == formID;
            const bool ownedLootEntry =
                g_session.active &&
                g_session.phase == SessionPhase::LootCommitted &&
                g_session.selectedActorFormID == formID;
            const bool ownedRecruitEntry =
                g_session.active &&
                g_session.phase == SessionPhase::RecruitCommitted &&
                g_session.selectedActorFormID == formID;
            const bool ownedPleasureEntry =
                g_session.active &&
                g_session.phase == SessionPhase::PleasureCommitted &&
                g_session.selectedActorFormID == formID;
            if (ownedKillEntry || ownedLootEntry || ownedRecruitEntry || ownedPleasureEntry) {
                // Outcome-specific ticks are the only owners while a selected actor
                // is committed. Generic countdown/dead cleanup must not race them.
                continue;
            }

            if (!actor || actor->IsDisabled()) {
                releases.emplace_back(formID, "invalid", false);
                continue;
            }
            if (actor->IsDead()) {
                releases.emplace_back(formID, "dead", false);
                continue;
            }

            if (!contextGate.allowed) {
                spdlog::info(
                    "[TFD][Victory][P32B] enemy defeated visual entry cancelled actor={:08X} reason={} root={} context={} gate={} sub={} terminal={} softActive={} hardApplied={} bleedStartPending={} bleedStarted={} noHardening=1 noBleedoutStart=1",
                    formID,
                    contextGate.reason,
                    RootName(contextGate.root),
                    RootName(contextGate.contextRoot),
                    DecisionGateName(contextGate.gate),
                    SubFlowName(contextGate.sub),
                    contextGate.terminalResolved ? 1 : 0,
                    entry.softEnterActive ? 1 : 0,
                    entry.softEnterHardStateApplied ? 1 : 0,
                    entry.visualBleedoutStartPending ? 1 : 0,
                    entry.visualBleedoutStarted ? 1 : 0);
                releases.emplace_back(formID, contextGate.reason, false);
                continue;
            }

            MaintainEnemyState(actor, entry, false);

            if (entry.countdownHeld) {
                const bool validHold =
                    g_session.active &&
                    g_session.sessionID == entry.heldSessionID &&
                    g_session.selectedActorFormID == formID;
                if (validHold) {
                    continue;
                }

                spdlog::warn(
                    "[TFD][Victory][R394A] stale countdown hold released actor={:08X} heldSession={} activeSession={}",
                    formID,
                    entry.heldSessionID,
                    g_session.sessionID);
                entry.countdownHeld = false;
                entry.heldSessionID = 0;
                entry.autoDeathIssued = false;
                entry.fatalDamageApplied = false;
                entry.deadline = now + std::chrono::milliseconds(
                    static_cast<int>(kEnemyKnockSeconds * 1000.0));
            }

            if (entry.deadline.time_since_epoch().count() == 0 || now < entry.deadline) {
                continue;
            }

            if (!entry.autoDeathIssued) {
                if (actor->IsEssential() || actor->IsProtected()) {
                    spdlog::warn(
                        "[TFD][Victory][R394A] auto-death skipped actor={:08X} reason=protected_or_essential",
                        formID);
                    releases.emplace_back(formID, "timeout_skip_kill", true);
                    continue;
                }

                entry.autoDeathIssued = true;
                entry.fatalDamageApplied = false;
                entry.deadline = now + std::chrono::milliseconds(450);

                // Auto-death exits the optional one-shot defeated visual before
                // fatal damage. This is not a Get Up path and does not reopen any
                // dialogue or generic flow.
                if (entry.visualBleedoutStarted && !entry.visualBleedoutStopSent) {
                    actor->NotifyAnimationGraph("BleedoutStop");
                    entry.visualBleedoutStopSent = true;
                }
                actor->EvaluatePackage(false, true);
                actor->EvaluatePackage(true, true);
                spdlog::info(
                    "[TFD][Victory][R399A] auto-death queued actor={:08X} visualStopSent={}",
                    formID,
                    entry.visualBleedoutStopSent ? 1 : 0);
                continue;
            }

            if (!entry.fatalDamageApplied) {
                if (actor->IsDead()) {
                    releases.emplace_back(formID, "timeout_dead", false);
                    continue;
                }

                const float hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
                const float fatalDamage = (std::max)(25.0f, hpNow + 5000.0f);
                actor->RestoreActorValue(
                    RE::ACTOR_VALUE_MODIFIER::kDamage,
                    RE::ActorValue::kHealth,
                    -fatalDamage);
                entry.fatalDamageApplied = true;
                entry.deadline = now + std::chrono::milliseconds(1500);
                spdlog::info(
                    "[TFD][Victory][R394A] auto-death damage actor={:08X} hpBefore={:.2f} damage={:.2f}",
                    formID,
                    hpNow,
                    fatalDamage);
                continue;
            }

            if (actor->IsDead()) {
                releases.emplace_back(formID, "timeout_dead", false);
                continue;
            }

            // Keep the confirmed baseline's final fallback grace: the entry's
            // post-damage deadline must be at least 1.5 seconds old.
            if ((now - entry.deadline) >= std::chrono::milliseconds(1500)) {
                spdlog::warn(
                    "[TFD][Victory][R394A] auto-death fallback kill actor={:08X}",
                    formID);
                actor->KillImmediate();
                releases.emplace_back(formID, "timeout_dead_fallback", false);
            }
        }

        for (const auto& [formID, reason, playGetUp] : releases) {
            (void)ReleaseEntryByIDLocked(formID, reason, playGetUp);
        }

        if (servicedAny &&
            (g_lastLifecycleLog.time_since_epoch().count() == 0 ||
                (now - g_lastLifecycleLog) >= std::chrono::milliseconds(1500))) {
            g_lastLifecycleLog = now;
            spdlog::info(
                "[TFD][Victory][R394A] enemy defeat lifecycle serviced active={}",
                static_cast<unsigned>(g_enemyEntries.size()));
        }
    }

    bool ReleaseManagedEnemy(RE::Actor* actor, std::string_view reason, bool playGetUp)
    {
        if (!actor) {
            return false;
        }
        std::scoped_lock lk(g_lock);
        return ReleaseEntryByIDLocked(actor->GetFormID(), reason, playGetUp);
    }

    void SuppressAutoDeathForExternalFight(RE::Actor* actor, double seconds, std::string_view reason)
    {
        const auto reasonText = ReasonText(reason);
        if (!actor || actor->IsDisabled() || actor->IsDead()) {
            spdlog::warn(
                "[TFD][Victory][R394A] external fight release skipped actor={:08X} reason={} invalid=1",
                actor ? actor->GetFormID() : 0u,
                reasonText);
            return;
        }

        const double safeSeconds = (std::max)(2.0, seconds);
        TFD::Actor::Ops::SuppressDefeatedEnemyReentry(actor, safeSeconds, reasonText.c_str());
        const bool released = ReleaseManagedEnemy(actor, reason, true);

        // This preserves the already-stable external PleasureFailed -> Fight
        // behavior. The reconstructed Victory outcomes will not use this path.
        actor->NotifyAnimationGraph("BleedoutStop");
        actor->NotifyAnimationGraph("GetUpStart");
        actor->EvaluatePackage(false, true);
        actor->EvaluatePackage(true, true);

        spdlog::info(
            "[TFD][Victory][R394A] external fight release actor={:08X} seconds={:.1f} releasedManaged={} reason={}",
            actor->GetFormID(),
            safeSeconds,
            released ? 1 : 0,
            reasonText);
    }

    void RestoreActorHealthToSafePct(
        RE::Actor* actor,
        float thresholdPct,
        float bonusPct,
        float minSafePct,
        float maxSafePct,
        float minAbsHp,
        std::string_view reason)
    {
        if (!actor || actor->IsDead() || actor->IsDisabled()) {
            return;
        }

        const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
        const float hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
        auto pctToUnit = [](float value) { return value > 1.0f ? (value / 100.0f) : value; };
        const float threshold = std::clamp(pctToUnit(thresholdPct), 0.05f, 0.95f);
        const float bonus = std::clamp(pctToUnit(bonusPct), 0.0f, 0.95f);
        const float minSafe = std::clamp(pctToUnit(minSafePct), 0.05f, 1.0f);
        const float maxSafe = std::clamp(pctToUnit(maxSafePct), minSafe, 1.0f);
        const float safePct = std::clamp(threshold + bonus, minSafe, maxSafe);
        const float target = (std::max)(minAbsHp, hpMax * safePct);

        if (hpNow + 0.001f < target) {
            actor->RestoreActorValue(
                RE::ACTOR_VALUE_MODIFIER::kDamage,
                RE::ActorValue::kHealth,
                target - hpNow);
            spdlog::info(
                "[TFD][Victory][R394A] recover actor hp actor={:08X} reason={} from={:.2f} to={:.2f} thresholdPct={:.1f}",
                actor->GetFormID(),
                ReasonText(reason),
                hpNow,
                target,
                thresholdPct);
        }
    }

    void ResetForLoad(std::string_view reason)
    {
        std::scoped_lock lk(g_lock);
        ClearAllEnemyEntriesLocked(reason.empty() ? std::string_view{ "reset_for_load" } : reason);
        SetConditionState(0);
        spdlog::info(
            "[TFD][Victory][R394A] reset for load reason={}",
            ReasonText(reason));
    }

    void SetLoadTransition(bool active, std::string_view reason)
    {
        g_loadTransition.store(active, std::memory_order_release);
        if (active) {
            ResetForLoad(reason.empty() ? std::string_view{ "load_transition" } : reason);
        }
        spdlog::info(
            "[TFD][Victory][R394A] load transition active={} reason={}",
            active ? 1 : 0,
            ReasonText(reason));
    }

    bool BeginManualInteraction(RE::Actor* selectedActor, std::string_view reason)
    {
        const auto reasonText = ReasonText(reason);
        if (!IsUsableManualCandidate(selectedActor)) {
            spdlog::info(
                "[TFD][Victory][R394A] manual interaction rejected actor={:08X} reason={} gate=invalid_actor",
                selectedActor ? selectedActor->GetFormID() : 0u,
                reasonText);
            return false;
        }

        const auto actorFormID = selectedActor->GetFormID();
        {
            std::scoped_lock lk(g_lock);
            const auto it = g_enemyEntries.find(actorFormID);
            if (it == g_enemyEntries.end() || !it->second.managed) {
                spdlog::info(
                    "[TFD][Victory][R394A] manual interaction rejected actor={:08X} reason={} gate=not_managed",
                    actorFormID,
                    reasonText);
                return false;
            }

            if (g_session.active) {
                spdlog::info(
                    "[TFD][Victory][R394A] manual interaction consumed actor={:08X} reason={} gate=session_busy activeActor={:08X} session={} phase={}",
                    actorFormID,
                    reasonText,
                    g_session.selectedActorFormID,
                    g_session.sessionID,
                    ToString(g_session.phase));
                return true;
            }

            if (it->second.softEnterActive && !it->second.softEnterHardStateApplied) {
                spdlog::info(
                    "[TFD][Victory][P33K] manual interaction consumed actor={:08X} reason={} gate=visual_entry_pending softActive={} hardApplied={} visualPending={} visualStarted={} visualDecision={} noDialogueBeforeVisualSettle=1",
                    actorFormID,
                    reasonText,
                    it->second.softEnterActive ? 1 : 0,
                    it->second.softEnterHardStateApplied ? 1 : 0,
                    it->second.visualBleedoutStartPending ? 1 : 0,
                    it->second.visualBleedoutStarted ? 1 : 0,
                    it->second.visualBleedoutStartDecisionLogged ? 1 : 0);
                return true;
            }

            if (it->second.autoDeathIssued || it->second.fatalDamageApplied) {
                spdlog::info(
                    "[TFD][Victory][R394A] manual interaction consumed actor={:08X} reason={} gate=auto_death_committed",
                    actorFormID,
                    reasonText);
                return true;
            }
        }

        if (IsDialogueMenuOpen()) {
            spdlog::info(
                "[TFD][Victory][R394A] manual interaction consumed actor={:08X} reason={} gate=dialogue_menu_busy",
                actorFormID,
                reasonText);
            return true;
        }

        const auto threat = FindBlockingPlayerThreat(selectedActor);
        if (threat.blocked) {
            spdlog::info(
                "[TFD][Victory][R394A] manual interaction consumed actor={:08X} reason={} gate=standing_enemy_targets_player threat={:08X} distance={:.1f}",
                actorFormID,
                reasonText,
                threat.actorFormID,
                threat.distance);
            return true;
        }

        std::uint32_t sessionID = 0;
        {
            std::scoped_lock lk(g_lock);
            auto it = g_enemyEntries.find(actorFormID);
            if (it == g_enemyEntries.end() || !it->second.managed ||
                it->second.autoDeathIssued || it->second.fatalDamageApplied) {
                spdlog::info(
                    "[TFD][Victory][R394A] manual interaction consumed actor={:08X} reason={} gate=state_changed_before_commit",
                    actorFormID,
                    reasonText);
                return true;
            }
            if (g_session.active) {
                return true;
            }
            if (it->second.softEnterActive && !it->second.softEnterHardStateApplied) {
                spdlog::info(
                    "[TFD][Victory][P33K] manual interaction consumed actor={:08X} reason={} gate=visual_entry_pending_after_recheck softActive={} hardApplied={} visualPending={} visualStarted={} visualDecision={} noDialogueBeforeVisualSettle=1",
                    actorFormID,
                    reasonText,
                    it->second.softEnterActive ? 1 : 0,
                    it->second.softEnterHardStateApplied ? 1 : 0,
                    it->second.visualBleedoutStartPending ? 1 : 0,
                    it->second.visualBleedoutStarted ? 1 : 0,
                    it->second.visualBleedoutStartDecisionLogged ? 1 : 0);
                return true;
            }
            if (it->second.pleasureReturnReassertActive && !it->second.visualBleedoutStarted) {
                spdlog::info(
                    "[TFD][Victory][P56A] manual interaction consumed actor={:08X} reason={} gate=pleasure_return_delayed_bleedout_pending visualStarted=0 attempts={} delayedBurstStartMs={} noDialogueBeforeVisualSettle=1",
                    actorFormID,
                    reasonText,
                    static_cast<unsigned>(it->second.pleasureReturnReassertAttempts),
                    kPleasureReturnDelayedBleedoutStartDelay.count());
                return true;
            }

            sessionID = NextSessionIDLocked();
            auto& entry = it->second;
            entry.deadline = Now() + std::chrono::milliseconds(
                static_cast<int>(kEnemyKnockSeconds * 1000.0));
            entry.countdownHeld = true;
            entry.heldSessionID = sessionID;
            entry.autoDeathIssued = false;
            entry.fatalDamageApplied = false;
            if (entry.pleasureReturnVisualHoldActive) {
                entry.pleasureReturnVisualHoldActive = false;
                entry.pleasureReturnVisualHoldLogged = false;
                if (!entry.pleasureReturnReassertActive) {
                    entry.pleasureReturnReassertActive = true;
                    entry.pleasureReturnReassertAttempts = 0;
                    entry.pleasureReturnReassertNextDue = Now() + kPleasureReturnBleedoutReassertInitialDelay;
                    entry.pleasureReturnReassertUntil = entry.deadline.time_since_epoch().count() != 0 ?
                        entry.deadline - kPleasureReturnBleedoutReassertStopBeforeTimeout :
                        Now() + std::chrono::milliseconds(static_cast<int>(kEnemyKnockSeconds * 1000.0));
                }
                entry.initialPackageRefreshDone = false;
                entry.packageRefreshAfterHardeningPending = true;
                entry.pleasureReturnPackageHoldActive = true;
                spdlog::info(
                    "[TFD][Victory][P56A] Pleasure return delayed visual hold released for manual dialogue actor={:08X} reason={} aliasSlot={} factionApplied={} reassertActive={} packageHeld=1 mirrorHeld=1 evaluateHeld=1",
                    actorFormID,
                    reasonText,
                    entry.aliasSlot,
                    entry.factionApplied ? 1 : 0,
                    entry.pleasureReturnReassertActive ? 1 : 0);
            }
            MaintainEnemyState(selectedActor, entry, false);

            g_session.active = true;
            g_session.sessionID = sessionID;
            g_session.selectedActorFormID = actorFormID;
            g_session.phase = SessionPhase::OpeningDialogue;
            g_sessionOpenDeadline = Now() + kDialogueOpenTimeout;
            g_choiceCommitDeadline = {};
            g_killDeadline = {};
            g_killFinalizeNotBefore = {};
            g_killStage = KillStage::None;
            SetConditionState(2);

            spdlog::info(
                "[TFD][Victory][R394A] session begin actor={:08X} session={} phase={} countdownReset={:.1f} countdownHeld=1 state=2 reason={}",
                actorFormID,
                sessionID,
                ToString(g_session.phase),
                kEnemyKnockSeconds,
                reasonText);
        }

        selectedActor->AllowPCDialogue(true);
        auto* greetInfo = ResolveVictoryGreetTopicInfo();
        const bool opened = greetInfo && selectedActor->SetDialogueWithPlayer(true, true, greetInfo);

        {
            std::scoped_lock lk(g_lock);
            if (g_session.active &&
                g_session.sessionID == sessionID &&
                g_session.selectedActorFormID == actorFormID) {
                if (!opened) {
                    spdlog::warn(
                        "[TFD][Victory][R394A] greet open failed actor={:08X} session={} topicInfo={:08X} action=cancel_restart_countdown",
                        actorFormID,
                        sessionID,
                        greetInfo ? greetInfo->GetFormID() : 0u);
                    ResetSessionLocked("greet_open_failed", true);
                }
                else {
                    if (IsDialogueMenuOpen() && g_session.phase == SessionPhase::OpeningDialogue) {
                        g_session.phase = SessionPhase::DialogueOpen;
                        g_sessionOpenDeadline = {};
                    }
                    if (auto it = g_enemyEntries.find(actorFormID); it != g_enemyEntries.end()) {
                        (void)StopPleasureReturnBurstForPlayerActionLocked(
                            selectedActor,
                            it->second,
                            "manual_dialogue_opened",
                            false);
                    }
                    spdlog::info(
                        "[TFD][Victory][R394A] greet open result actor={:08X} session={} opened=1 topicInfo={:08X} phase={} no_getup=1",
                        actorFormID,
                        sessionID,
                        greetInfo->GetFormID(),
                        ToString(g_session.phase));
                }
            }
        }

        return true;
    }

    void NotifyDialogueMenuStateChanged(bool opening)
    {
        std::scoped_lock lk(g_lock);
        if (!g_session.active) {
            return;
        }

        if (opening) {
            if (g_session.phase == SessionPhase::OpeningDialogue) {
                g_session.phase = SessionPhase::DialogueOpen;
                g_sessionOpenDeadline = {};
                if (auto it = g_enemyEntries.find(g_session.selectedActorFormID); it != g_enemyEntries.end()) {
                    auto actorSP = it->second.handle.get();
                    (void)StopPleasureReturnBurstForPlayerActionLocked(
                        actorSP.get(),
                        it->second,
                        "dialogue_menu_opened",
                        false);
                }
                spdlog::info(
                    "[TFD][Victory][R394A] dialogue menu opened actor={:08X} session={} phase={}",
                    g_session.selectedActorFormID,
                    g_session.sessionID,
                    ToString(g_session.phase));
            }
            else if (g_session.phase == SessionPhase::PleasureCommitted) {
                MaintainPleasureDialogueCloseGuardLocked(Now());
                spdlog::info(
                    "[TFD][Victory][P36A] dialogue menu opening suppressed during PleasureCommitted actor={:08X} session={} closeGuardSession={}",
                    g_session.selectedActorFormID,
                    g_session.sessionID,
                    g_pleasureDialogueCloseGuardSessionID);
            }
            return;
        }

        if (g_session.phase == SessionPhase::DialogueOpen) {
            // TopicInfo end fragments may run several seconds after DialogueMenu
            // emits its close event. Keep ownership for a bounded commit window so
            // a chosen outcome is not mistaken for Cancel. If no outcome arrives,
            // Tick() performs the normal cancel and restarts the ten-second count.
            g_session.phase = SessionPhase::AwaitingChoiceCommit;
            g_sessionOpenDeadline = {};
            g_choiceCommitDeadline = Now() + kChoiceCommitGrace;
            spdlog::info(
                "[TFD][Victory][R396B] dialogue closed awaiting choice actor={:08X} session={} phase={} graceMs={}",
                g_session.selectedActorFormID,
                g_session.sessionID,
                ToString(g_session.phase),
                kChoiceCommitGrace.count());
        }
        else if (g_session.phase == SessionPhase::AwaitingChoiceCommit) {
            spdlog::info(
                "[TFD][Victory][R395B] duplicate dialogue close ignored actor={:08X} session={} phase={}",
                g_session.selectedActorFormID,
                g_session.sessionID,
                ToString(g_session.phase));
        }
        else if (g_session.phase == SessionPhase::KillCommitted) {
            ArmCommittedKillLocked(Now(), "dialogue_menu_closed_after_kill_commit");
        }
        else if (g_session.phase == SessionPhase::LootCommitted) {
            if (g_lootStage == LootStage::WaitingForDialogueClose) {
                g_lootStage = LootStage::InventoryDispatchPending;
                g_lootInventoryDispatchAttempts = 0;
                g_lootDeadline = Now() + kLootInventoryDispatchDelay;
                spdlog::info(
                    "[TFD][Victory][R396B] Loot dialogue close observed actor={:08X} session={} stage={} nativeDispatchDelayMs={}",
                    g_session.selectedActorFormID,
                    g_session.sessionID,
                    ToString(g_lootStage),
                    kLootInventoryDispatchDelay.count());
            }
            else {
                spdlog::info(
                    "[TFD][Victory][R396B] Loot dialogue close ignored actor={:08X} session={} stage={} reason=already_advanced",
                    g_session.selectedActorFormID,
                    g_session.sessionID,
                    ToString(g_lootStage));
            }
        }
        else if (g_session.phase == SessionPhase::RecruitCommitted) {
            if (g_recruitStage == RecruitStage::WaitingForDialogueClose) {
                g_recruitStage = RecruitStage::CommitPending;
                g_recruitDeadline = Now() + kRecruitCommitDelay;
                spdlog::info(
                    "[TFD][Victory][R421A] Recruit dialogue close observed before delayed BleedoutStop release actor={:08X} session={} stage={} postDialogueCommitDelayMs={} deferBleedoutStopUntilDialogueClose=1",
                    g_session.selectedActorFormID,
                    g_session.sessionID,
                    ToString(g_recruitStage),
                    kRecruitCommitDelay.count());
            }
            else {
                spdlog::info(
                    "[TFD][Victory][R421A] Recruit dialogue close ignored actor={:08X} session={} stage={} reason=already_advanced",
                    g_session.selectedActorFormID,
                    g_session.sessionID,
                    ToString(g_recruitStage));
            }
        }
        else if (g_session.phase == SessionPhase::PleasureCommitted) {
            spdlog::info(
                "[TFD][Victory][P33O] Pleasure dialogue close ignored while handoff pending actor={:08X} session={} phase={}",
                g_session.selectedActorFormID,
                g_session.sessionID,
                ToString(g_session.phase));
        }
        else {
            spdlog::info(
                "[TFD][Victory][R394A] dialogue close ignored actor={:08X} session={} phase={} reason=awaiting_owned_open",
                g_session.selectedActorFormID,
                g_session.sessionID,
                ToString(g_session.phase));
        }
    }

    void NotifyContainerMenuStateChanged(bool opening)
    {
        std::scoped_lock lk(g_lock);
        if (!g_session.active || g_session.phase != SessionPhase::LootCommitted) {
            return;
        }

        if (opening) {
            const auto targetFormID = GetOpenLootTargetActorFormID();
            if ((g_lootStage == LootStage::InventoryDispatchPending ||
                    g_lootStage == LootStage::WaitingForInventoryOpen) &&
                targetFormID == g_session.selectedActorFormID) {
                g_lootStage = LootStage::InventoryOpen;
                g_lootDeadline = {};
                spdlog::info(
                    "[TFD][Victory][R396B] Loot inventory opened actor={:08X} session={} stage={} target={:08X} countdownHeld=1",
                    g_session.selectedActorFormID,
                    g_session.sessionID,
                    ToString(g_lootStage),
                    targetFormID);
            }
            else {
                spdlog::info(
                    "[TFD][Victory][R396B] ContainerMenu open ignored actor={:08X} session={} stage={} target={:08X} reason=not_selected_loot_target",
                    g_session.selectedActorFormID,
                    g_session.sessionID,
                    ToString(g_lootStage),
                    targetFormID);
            }
            return;
        }

        if (g_lootStage == LootStage::InventoryOpen) {
            g_lootStage = LootStage::ReleasePending;
            g_lootDeadline = Now() + kLootReleasePrepareDelay;
            spdlog::info(
                "[TFD][Victory][R396B] Loot inventory closed actor={:08X} session={} stage={} releaseDelayMs={}",
                g_session.selectedActorFormID,
                g_session.sessionID,
                ToString(g_lootStage),
                kLootReleasePrepareDelay.count());
        }
        else {
            spdlog::info(
                "[TFD][Victory][R396B] Loot inventory close ignored actor={:08X} session={} stage={}",
                g_session.selectedActorFormID,
                g_session.sessionID,
                ToString(g_lootStage));
        }
    }

    bool RequestKill(RE::Actor* speaker, std::string_view reason)
    {
        const auto reasonText = ReasonText(reason);
        if (!g_installed.load(std::memory_order_acquire) ||
            !TFD::Settings::GetEnabled() ||
            g_loadTransition.load(std::memory_order_acquire) ||
            !speaker || speaker->IsDisabled() || speaker->IsDead()) {
            spdlog::warn(
                "[TFD][Victory][R395B] Kill rejected actor={:08X} gate=invalid_runtime reason={}",
                speaker ? speaker->GetFormID() : 0u,
                reasonText);
            return false;
        }

        const auto actorFormID = speaker->GetFormID();
        std::scoped_lock lk(g_lock);
        const bool commitPhase =
            g_session.phase == SessionPhase::DialogueOpen ||
            g_session.phase == SessionPhase::AwaitingChoiceCommit;
        if (!g_session.active ||
            g_session.selectedActorFormID != actorFormID ||
            !commitPhase) {
            spdlog::warn(
                "[TFD][Victory][R395B] Kill rejected actor={:08X} gate=session_mismatch active={} selected={:08X} session={} phase={} reason={}",
                actorFormID,
                g_session.active ? 1 : 0,
                g_session.selectedActorFormID,
                g_session.sessionID,
                ToString(g_session.phase),
                reasonText);
            return false;
        }

        auto it = g_enemyEntries.find(actorFormID);
        if (it == g_enemyEntries.end() ||
            !it->second.managed ||
            it->second.autoDeathIssued ||
            it->second.fatalDamageApplied) {
            spdlog::warn(
                "[TFD][Victory][R395B] Kill rejected actor={:08X} gate=defeated_entry_invalid session={} reason={}",
                actorFormID,
                g_session.sessionID,
                reasonText);
            return false;
        }

        if (speaker->IsEssential() || speaker->IsProtected()) {
            spdlog::warn(
                "[TFD][Victory][R395B] Kill rejected actor={:08X} gate=protected_or_essential session={} reason={}",
                actorFormID,
                g_session.sessionID,
                reasonText);
            return false;
        }

        StopPleasureReturnBurstForPlayerActionLocked(speaker, it->second, "kill_commit", true);

        const auto sessionID = g_session.sessionID;
        const auto requestPhase = g_session.phase;
        const bool dialogueOpen = IsDialogueMenuOpen();
        g_session.phase = SessionPhase::KillCommitted;
        g_sessionOpenDeadline = {};
        g_choiceCommitDeadline = {};
        g_killFinalizeNotBefore = {};
        SetConditionState(1);

        // Keep the selected actor and countdown owned until death is confirmed on
        // a later Tick. No synchronous KillImmediate is allowed inside the fragment.
        it->second.countdownHeld = true;
        it->second.heldSessionID = sessionID;
        it->second.autoDeathIssued = false;
        it->second.fatalDamageApplied = false;

        ClearRecruitTransitionLocked("kill_commit_prepare");
        g_killStage = dialogueOpen ?
            KillStage::WaitingForDialogueClose :
            KillStage::PrimePending;
        g_killDeadline = dialogueOpen ?
            Clock::time_point{} :
            Now() + kKillPrimeDelay;

        spdlog::info(
            "[TFD][Victory][R395B] Kill commit accepted actor={:08X} session={} phase={} requestPhase={} dialogueOpen={} killStage={} reason={} no_getup=1",
            actorFormID,
            sessionID,
            ToString(g_session.phase),
            ToString(requestPhase),
            dialogueOpen ? 1 : 0,
            ToString(g_killStage),
            reasonText);
        return true;
    }

    bool RequestLoot(RE::Actor* speaker, std::string_view reason)
    {
        const auto reasonText = ReasonText(reason);
        if (!g_installed.load(std::memory_order_acquire) ||
            !TFD::Settings::GetEnabled() ||
            g_loadTransition.load(std::memory_order_acquire) ||
            !speaker || speaker->IsDisabled() || speaker->IsDead()) {
            spdlog::warn(
                "[TFD][Victory][R396B] Loot rejected actor={:08X} gate=invalid_runtime reason={}",
                speaker ? speaker->GetFormID() : 0u,
                reasonText);
            return false;
        }

        const auto actorFormID = speaker->GetFormID();
        std::scoped_lock lk(g_lock);
        const bool commitPhase =
            g_session.phase == SessionPhase::DialogueOpen ||
            g_session.phase == SessionPhase::AwaitingChoiceCommit;
        if (!g_session.active ||
            g_session.selectedActorFormID != actorFormID ||
            !commitPhase) {
            spdlog::warn(
                "[TFD][Victory][R396B] Loot rejected actor={:08X} gate=session_mismatch active={} selected={:08X} session={} phase={} reason={}",
                actorFormID,
                g_session.active ? 1 : 0,
                g_session.selectedActorFormID,
                g_session.sessionID,
                ToString(g_session.phase),
                reasonText);
            return false;
        }

        auto it = g_enemyEntries.find(actorFormID);
        if (it == g_enemyEntries.end() ||
            !it->second.managed ||
            it->second.autoDeathIssued ||
            it->second.fatalDamageApplied) {
            spdlog::warn(
                "[TFD][Victory][R396B] Loot rejected actor={:08X} gate=defeated_entry_invalid session={} reason={}",
                actorFormID,
                g_session.sessionID,
                reasonText);
            return false;
        }

        StopPleasureReturnBurstForPlayerActionLocked(speaker, it->second, "loot_commit", true);

        const auto sessionID = g_session.sessionID;
        const auto requestPhase = g_session.phase;
        const bool dialogueOpen = IsDialogueMenuOpen();
        const auto now = Now();

        g_session.phase = SessionPhase::LootCommitted;
        g_sessionOpenDeadline = {};
        g_choiceCommitDeadline = {};
        g_killDeadline = {};
        g_killFinalizeNotBefore = {};
        g_killStage = KillStage::None;
        ClearLootTransitionLocked("loot_commit_prepare");
        ClearRecruitTransitionLocked("loot_commit_prepare");
        SetConditionState(1);

        it->second.countdownHeld = true;
        it->second.heldSessionID = sessionID;
        it->second.autoDeathIssued = false;
        it->second.fatalDamageApplied = false;

        g_lootStage = dialogueOpen ?
            LootStage::WaitingForDialogueClose :
            LootStage::InventoryDispatchPending;
        g_lootInventoryDispatchAttempts = 0;
        g_lootDeadline = now + (dialogueOpen ?
            kLootDialogueCloseTimeout :
            kLootInventoryDispatchDelay);

        spdlog::info(
            "[TFD][Victory][R396B] Loot commit accepted actor={:08X} session={} phase={} requestPhase={} dialogueOpen={} lootStage={} reason={} countdownHeld=1 inventoryOwner=TFDVictory nativeMethod=Actor.OpenInventory fragment=request_only completionOwner=TFDVictory",
            actorFormID,
            sessionID,
            ToString(g_session.phase),
            ToString(requestPhase),
            dialogueOpen ? 1 : 0,
            ToString(g_lootStage),
            reasonText);
        return true;
    }

    bool RequestRecruit(RE::Actor* speaker, std::string_view reason)
    {
        const auto reasonText = ReasonText(reason);
        if (!g_installed.load(std::memory_order_acquire) ||
            !TFD::Settings::GetEnabled() ||
            g_loadTransition.load(std::memory_order_acquire) ||
            !speaker || speaker->IsDisabled() || speaker->IsDead()) {
            spdlog::warn(
                "[TFD][Victory][R400D] Recruit rejected actor={:08X} gate=invalid_runtime reason={}",
                speaker ? speaker->GetFormID() : 0u,
                reasonText);
            return false;
        }

        const auto actorFormID = speaker->GetFormID();
        std::scoped_lock lk(g_lock);
        const bool commitPhase =
            g_session.phase == SessionPhase::DialogueOpen ||
            g_session.phase == SessionPhase::AwaitingChoiceCommit;
        if (!g_session.active ||
            g_session.selectedActorFormID != actorFormID ||
            !commitPhase) {
            spdlog::warn(
                "[TFD][Victory][R400D] Recruit rejected actor={:08X} gate=session_mismatch active={} selected={:08X} session={} phase={} reason={}",
                actorFormID,
                g_session.active ? 1 : 0,
                g_session.selectedActorFormID,
                g_session.sessionID,
                ToString(g_session.phase),
                reasonText);
            return false;
        }

        auto it = g_enemyEntries.find(actorFormID);
        if (it == g_enemyEntries.end() ||
            !it->second.managed ||
            it->second.autoDeathIssued ||
            it->second.fatalDamageApplied) {
            spdlog::warn(
                "[TFD][Victory][R400D] Recruit rejected actor={:08X} gate=defeated_entry_invalid session={} reason={}",
                actorFormID,
                g_session.sessionID,
                reasonText);
            return false;
        }

        if (TFD::TeammateManager::GetRecruitSlotsFree() == 0) {
            spdlog::warn(
                "[TFD][Victory][R400D] Recruit rejected actor={:08X} gate=no_recruit_slot session={} reason={}",
                actorFormID,
                g_session.sessionID,
                reasonText);
            return false;
        }

        StopPleasureReturnBurstForPlayerActionLocked(speaker, it->second, "recruit_commit", true);

        const auto sessionID = g_session.sessionID;
        const auto requestPhase = g_session.phase;
        const bool dialogueOpen = IsDialogueMenuOpen();
        const auto now = Now();

        g_session.phase = SessionPhase::RecruitCommitted;
        g_sessionOpenDeadline = {};
        g_choiceCommitDeadline = {};
        g_killDeadline = {};
        g_killFinalizeNotBefore = {};
        g_killStage = KillStage::None;
        ClearLootTransitionLocked("recruit_commit_prepare");
        ClearRecruitTransitionLocked("recruit_commit_prepare");
        SetConditionState(1);

        it->second.countdownHeld = true;
        it->second.heldSessionID = sessionID;
        it->second.autoDeathIssued = false;
        it->second.fatalDamageApplied = false;

        // R421A: delayed vanilla bleedout visual is owned by the soft-enter state.
        // The option fragment only commits the choice; the visual release waits
        // for DialogueMenu/MenuTopicManager to close before sending BleedoutStop.
        const bool waitForDialogueClose = dialogueOpen;
        if (waitForDialogueClose) {
            g_recruitStage = RecruitStage::WaitingForDialogueClose;
            g_recruitDeadline = now + kRecruitDialogueCloseTimeout;
        }
        else {
            g_recruitStage = RecruitStage::CommitPending;
            g_recruitDeadline = now + kRecruitCommitDelay;
        }

        spdlog::info(
            "[TFD][Victory][R421A] Recruit commit accepted actor={:08X} session={} phase={} requestPhase={} dialogueOpen={} recruitStage={} reason={} countdownHeld=1 fragment=request_only owner=TFDVictory deferBleedoutStopUntilDialogueClose={} immediateCommit={} commitDelayMs={} dialogueCloseTimeoutMs={}",
            actorFormID,
            sessionID,
            ToString(g_session.phase),
            ToString(requestPhase),
            dialogueOpen ? 1 : 0,
            ToString(g_recruitStage),
            reasonText,
            waitForDialogueClose ? 1 : 0,
            waitForDialogueClose ? 0 : 1,
            kRecruitCommitDelay.count(),
            kRecruitDialogueCloseTimeout.count());
        return true;
    }

    bool RequestPleasure(RE::Actor* speaker, std::string_view reason)
    {
        const auto reasonText = ReasonText(reason);
        if (!g_installed.load(std::memory_order_acquire) ||
            !TFD::Settings::GetEnabled() ||
            g_loadTransition.load(std::memory_order_acquire) ||
            !speaker || speaker->IsDisabled() || speaker->IsDead()) {
            spdlog::warn(
                "[TFD][Victory][P33O] Pleasure rejected actor={:08X} gate=invalid_runtime reason={}",
                speaker ? speaker->GetFormID() : 0u,
                reasonText);
            return false;
        }

        const auto actorFormID = speaker->GetFormID();
        std::uint32_t sessionID = 0;
        SessionPhase requestPhase = SessionPhase::Empty;
        bool dialogueOpen = false;
        bool visualBleedoutOwned = false;
        bool visualStopSent = false;

        {
            std::scoped_lock lk(g_lock);
            const bool commitPhase =
                g_session.phase == SessionPhase::DialogueOpen ||
                g_session.phase == SessionPhase::AwaitingChoiceCommit;
            if (!g_session.active ||
                g_session.selectedActorFormID != actorFormID ||
                !commitPhase) {
                spdlog::warn(
                    "[TFD][Victory][P33O] Pleasure rejected actor={:08X} gate=session_mismatch active={} selected={:08X} session={} phase={} reason={}",
                    actorFormID,
                    g_session.active ? 1 : 0,
                    g_session.selectedActorFormID,
                    g_session.sessionID,
                    ToString(g_session.phase),
                    reasonText);
                return false;
            }

            auto it = g_enemyEntries.find(actorFormID);
            if (it == g_enemyEntries.end() ||
                !it->second.managed ||
                it->second.autoDeathIssued ||
                it->second.fatalDamageApplied) {
                spdlog::warn(
                    "[TFD][Victory][P33O] Pleasure rejected actor={:08X} gate=defeated_entry_invalid session={} reason={}",
                    actorFormID,
                    g_session.sessionID,
                    reasonText);
                return false;
            }

            StopPleasureReturnBurstForPlayerActionLocked(speaker, it->second, "pleasure_commit", true);

            sessionID = g_session.sessionID;
            requestPhase = g_session.phase;
            dialogueOpen = IsDialogueMenuOpen();

            g_session.phase = SessionPhase::PleasureCommitted;
            g_sessionOpenDeadline = {};
            g_choiceCommitDeadline = {};
            g_killDeadline = {};
            g_killFinalizeNotBefore = {};
            g_killStage = KillStage::None;
            ClearLootTransitionLocked("pleasure_commit_prepare");
            ClearRecruitTransitionLocked("pleasure_commit_prepare");
            SetConditionState(0);
            ArmPleasureDialogueCloseGuardLocked(speaker, sessionID, Now(), "victory_pleasure_commit");

            it->second.countdownHeld = true;
            it->second.heldSessionID = sessionID;
            it->second.autoDeathIssued = false;
            it->second.fatalDamageApplied = false;

            visualBleedoutOwned = it->second.visualBleedoutStarted && !it->second.visualBleedoutStopSent;
            if (visualBleedoutOwned) {
                visualStopSent = speaker->NotifyAnimationGraph("BleedoutStop");
                if (visualStopSent) {
                    it->second.visualBleedoutStopSent = true;
                }
            }

            spdlog::info(
                "[TFD][Victory][P36A] Pleasure commit accepted actor={:08X} session={} phase={} requestPhase={} dialogueOpen={} visualBleedoutOwned={} BleedoutStopSent={} reason={} countdownHeld=1 source=VictorySpecial closeGuard=1 victoryState=0",
                actorFormID,
                sessionID,
                ToString(g_session.phase),
                ToString(requestPhase),
                dialogueOpen ? 1 : 0,
                visualBleedoutOwned ? 1 : 0,
                visualStopSent ? 1 : 0,
                reasonText);
        }

        (void)CloseVictoryDialogueMenu(speaker, "victory_pleasure_commit", false);
        return true;
    }

    bool CompletePleasureHandoff(RE::Actor* speaker, bool started, std::string_view reason)
    {
        const auto reasonText = ReasonText(reason);
        if (!speaker) {
            spdlog::warn("[TFD][Victory][P33O] Pleasure handoff complete rejected reason=no_speaker started={} source={}", started ? 1 : 0, reasonText);
            return false;
        }

        const auto actorFormID = speaker->GetFormID();
        std::scoped_lock lk(g_lock);
        if (!g_session.active ||
            g_session.selectedActorFormID != actorFormID ||
            g_session.phase != SessionPhase::PleasureCommitted) {
            spdlog::warn(
                "[TFD][Victory][P33O] Pleasure handoff complete rejected actor={:08X} started={} active={} selected={:08X} session={} phase={} reason={}",
                actorFormID,
                started ? 1 : 0,
                g_session.active ? 1 : 0,
                g_session.selectedActorFormID,
                g_session.sessionID,
                ToString(g_session.phase),
                reasonText);
            return false;
        }

        if (!started) {
            ResetSessionLocked("victory_pleasure_start_failed", true);
            spdlog::warn(
                "[TFD][Victory][P33O] Pleasure handoff failed actor={:08X} action=restart_countdown reason={}",
                actorFormID,
                reasonText);
            return false;
        }

        auto it = g_enemyEntries.find(actorFormID);
        if (it == g_enemyEntries.end()) {
            ResetSessionLocked("victory_pleasure_handoff_lost_entry", false);
            spdlog::warn(
                "[TFD][Victory][P33P] Pleasure handoff failed actor={:08X} gate=missing_defeated_entry reason={}",
                actorFormID,
                reasonText);
            return false;
        }

        auto& entry = it->second;
        SetConditionState(0);
        ArmPleasureDialogueCloseGuardLocked(speaker, g_session.sessionID, Now(), "victory_pleasure_handoff");
        if (entry.visualBleedoutStarted && !entry.visualBleedoutStopSent) {
            const bool stopSent = speaker->NotifyAnimationGraph("BleedoutStop");
            if (stopSent) {
                entry.visualBleedoutStopSent = true;
            }
            spdlog::info(
                "[TFD][Victory][P33P] Pleasure handoff visual stop actor={:08X} sent={} reason={}",
                actorFormID,
                stopSent ? 1 : 0,
                reasonText);
        }

        (void)RestoreLootReleaseHealth(speaker, entry.thresholdPct);
        ApplyRegenOverride(speaker, entry);
        TFD::Actor::Ops::ApplyDefeatedEnemyPassiveOverride(speaker, entry.savedAggression, entry.aggressionOverridden);
        TFD::Actor::Ops::ClearDefeatedEnemyMirror(speaker, entry.aliasSlot, entry.factionApplied, "victory_pleasure_scene_suspended");

        if (speaker->IsInCombat()) {
            speaker->StopCombat();
        }
        if (auto* process = RE::ProcessLists::GetSingleton()) {
            process->StopCombatAndAlarmOnActor(speaker, false);
        }
        speaker->EvaluatePackage(false, true);
        speaker->EvaluatePackage(true, true);

        spdlog::info(
            "[TFD][Victory][P36A] Pleasure handoff armed actor={:08X} session={} reason={} ownership=held countdownHeld=1 defeatedEntryHeld=1 mirrorSuspended=1 releaseAtSceneEnd=0 closeGuard=1 victoryState=0",
            actorFormID,
            g_session.sessionID,
            reasonText);
        return true;
    }

    bool CompletePleasureScene(RE::Actor* speaker, bool sceneSucceeded, std::string_view reason)
    {
        const auto reasonText = ReasonText(reason);
        if (!speaker) {
            spdlog::warn("[TFD][Victory][P33P] Pleasure scene complete rejected reason=no_speaker succeeded={}", sceneSucceeded ? 1 : 0);
            return false;
        }

        const auto actorFormID = speaker->GetFormID();
        std::scoped_lock lk(g_lock);
        if (!g_session.active ||
            g_session.selectedActorFormID != actorFormID ||
            g_session.phase != SessionPhase::PleasureCommitted) {
            spdlog::warn(
                "[TFD][Victory][P33P] Pleasure scene complete rejected actor={:08X} succeeded={} active={} selected={:08X} session={} phase={} reason={}",
                actorFormID,
                sceneSucceeded ? 1 : 0,
                g_session.active ? 1 : 0,
                g_session.selectedActorFormID,
                g_session.sessionID,
                ToString(g_session.phase),
                reasonText);
            return false;
        }

        auto it = g_enemyEntries.find(actorFormID);
        if (it == g_enemyEntries.end()) {
            ResetSessionLocked("victory_pleasure_scene_lost_entry", false);
            spdlog::warn(
                "[TFD][Victory][P33P] Pleasure scene complete failed actor={:08X} gate=missing_defeated_entry succeeded={} reason={}",
                actorFormID,
                sceneSucceeded ? 1 : 0,
                reasonText);
            return false;
        }

        auto actorSP = it->second.handle.get();
        auto* actor = actorSP.get();
        if (!actor || actor->IsDisabled()) {
            ResetSessionLocked("victory_pleasure_scene_invalid_actor", false);
            spdlog::warn(
                "[TFD][Victory][P33P] Pleasure scene complete failed actor={:08X} gate=invalid_actor succeeded={} reason={}",
                actorFormID,
                sceneSucceeded ? 1 : 0,
                reasonText);
            return false;
        }

        if (actor->IsDead()) {
            ResetSessionLocked("victory_pleasure_scene_actor_dead", false);
            (void)ReleaseEntryByIDLocked(actorFormID, "victory_pleasure_scene_actor_dead", false);
            spdlog::info(
                "[TFD][Victory][P33P] Pleasure scene complete actor already dead actor={:08X} succeeded={} reason={}",
                actorFormID,
                sceneSucceeded ? 1 : 0,
                reasonText);
            return true;
        }

        const auto sessionID = g_session.sessionID;
        ResetSessionLocked(sceneSucceeded ? "victory_pleasure_scene_succeeded" : "victory_pleasure_scene_failed", true);

        auto postIt = g_enemyEntries.find(actorFormID);
        if (postIt == g_enemyEntries.end()) {
            spdlog::warn(
                "[TFD][Victory][P33P] Pleasure scene complete lost entry after session reset actor={:08X} session={} succeeded={} reason={}",
                actorFormID,
                sessionID,
                sceneSucceeded ? 1 : 0,
                reasonText);
            return false;
        }

        ArmPleasureReturnBleedoutLocked(actor, postIt->second, Now(), reason);
        RefreshConditionStateLocked();

        spdlog::info(
            "[TFD][Victory][P36A] Pleasure lifecycle returned to defeated countdown actor={:08X} session={} succeeded={} graceAfterDelayedBurstStart={:.1f} delayedBurstStartMs={} reason={} reopenViaManualActivation=1",
            actorFormID,
            sessionID,
            sceneSucceeded ? 1 : 0,
            kEnemyKnockSeconds,
            kPleasureReturnDelayedBleedoutStartDelay.count(),
            reasonText);
        return true;
    }

    void ObserveRecentRecruitHit(RE::Actor* target, RE::Actor* cause, std::string_view reason)
    {
        std::scoped_lock lk(g_lock);

        // R421A: the same hit sink is also used as a lightweight pressure sensor
        // for enemies that have entered soft Victory defeated state but are not
        // hardened yet. This catches the teammate-caused edge where real hit
        // traffic continues during the soft-enter window.
        const bool targetIsSoft = IsSoftEnterPressureActor(target);
        const bool causeIsSoft = IsSoftEnterPressureActor(cause);
        const bool causeIsPlayerSide = cause && TFD::TeammateManager::IsPlayerSideTeammateActor(cause);
        const bool targetIsPlayerSide = target && TFD::TeammateManager::IsPlayerSideTeammateActor(target);

        if (targetIsSoft && causeIsPlayerSide) {
            RecordSoftEnterPressureLocked(
                target->GetFormID(),
                cause ? cause->GetFormID() : 0u,
                cause ? cause->GetFormID() : 0u,
                reason,
                "hit_target_soft_cause_player_side");
        }
        if (causeIsSoft && (targetIsPlayerSide || !target)) {
            RecordSoftEnterPressureLocked(
                cause->GetFormID(),
                cause->GetFormID(),
                target ? target->GetFormID() : 0u,
                reason,
                target ? "hit_cause_soft_target_player_side" : "hit_cause_soft_target_unknown");
        }

        if (!target) {
            return;
        }

        if (!g_recruitHitDiagnostic.active ||
            g_recruitHitDiagnostic.actorFormID == 0 ||
            target->GetFormID() != g_recruitHitDiagnostic.actorFormID) {
            return;
        }

        const auto now = Now();
        if (g_recruitHitDiagnostic.expireAt.time_since_epoch().count() != 0 &&
            now >= g_recruitHitDiagnostic.expireAt) {
            spdlog::info(
                "[TFD][Victory][R421A] Recruit hit diagnostic ignored expired actor={:08X} session={} reason={} noBehaviorChange=1",
                g_recruitHitDiagnostic.actorFormID,
                g_recruitHitDiagnostic.recruitSessionID,
                ReasonText(reason));
            g_recruitHitDiagnostic = RecruitHitDiagnosticState{};
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        const bool causeIsPlayer =
            cause && player && cause->GetFormID() == player->GetFormID();
        if (!causeIsPlayer) {
            spdlog::info(
                "[TFD][Victory][R421A] Recruit hit diagnostic observed non-player hit actor={:08X} session={} cause={:08X} reason={} ignored=1 noBehaviorChange=1",
                target->GetFormID(),
                g_recruitHitDiagnostic.recruitSessionID,
                cause ? cause->GetFormID() : 0u,
                ReasonText(reason));
            return;
        }

        if (g_recruitHitDiagnostic.playerHitObserved) {
            spdlog::info(
                "[TFD][Victory][R421A] Recruit hit diagnostic duplicate player hit actor={:08X} session={} cause={:08X} reason={} ignored=1 noBehaviorChange=1",
                target->GetFormID(),
                g_recruitHitDiagnostic.recruitSessionID,
                cause->GetFormID(),
                ReasonText(reason));
            return;
        }

        g_recruitHitDiagnostic.playerHitObserved = true;
        g_recruitHitDiagnostic.causeFormID = cause->GetFormID();
        g_recruitHitDiagnostic.hitPosition = target->GetPosition();
        g_recruitHitDiagnostic.firstAfterHitDue = now + kRecruitHitDiagnosticFirstDelay;
        g_recruitHitDiagnostic.secondAfterHitDue = now + kRecruitHitDiagnosticSecondDelay;

        spdlog::info(
            "[TFD][Victory][R421A] Recruit hit diagnostic player hit observed actor={:08X} session={} cause={:08X} firstDelayMs={} secondDelayMs={} reason={} noBehaviorChange=1",
            target->GetFormID(),
            g_recruitHitDiagnostic.recruitSessionID,
            cause->GetFormID(),
            kRecruitHitDiagnosticFirstDelay.count(),
            kRecruitHitDiagnosticSecondDelay.count(),
            ReasonText(reason));
        LogRecruitHitDiagnosticSampleLocked(target, cause, "hit_event_immediate", reason);
    }

    RE::FormID FindPendingEnemyVisualEntry(RE::Actor* observer, float radius)
    {
        std::scoped_lock lk(g_lock);
        const auto observerPos = observer ? observer->GetPosition() : RE::NiPoint3{};
        const bool hasObserver = observer != nullptr;
        const float maxDistance = (std::max)(0.0f, radius);

        for (const auto& [formID, entry] : g_enemyEntries) {
            if (!entry.managed ||
                !entry.softEnterActive ||
                entry.softEnterHardStateApplied ||
                entry.autoDeathIssued ||
                entry.fatalDamageApplied) {
                continue;
            }

            auto actorSp = RE::Actor::LookupByHandle(entry.handle.native_handle());
            auto* actor = actorSp.get();
            if (!actor || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
                continue;
            }

            if (hasObserver && maxDistance > 0.0f) {
                const float dist = DistanceOrZero(actor->GetPosition(), observerPos);
                if (dist > maxDistance) {
                    continue;
                }
            }

            return formID;
        }

        return 0;
    }

    bool IsCombatBehaviorSuppressedActor(RE::Actor* actor)
    {
        if (!actor) {
            return false;
        }

        const auto actorFormID = actor->GetFormID();
        if (actorFormID == 0) {
            return false;
        }

        std::scoped_lock lk(g_lock);
        if (g_session.active && g_session.selectedActorFormID == actorFormID) {
            return true;
        }

        auto it = g_enemyEntries.find(actorFormID);
        return it != g_enemyEntries.end() &&
            it->second.managed &&
            !it->second.autoDeathIssued &&
            !it->second.fatalDamageApplied;
    }

    bool IsCombatBehaviorSuppressed()
    {
        std::scoped_lock lk(g_lock);
        if (g_session.active) {
            return true;
        }

        for (const auto& [_formID, entry] : g_enemyEntries) {
            if (entry.managed &&
                !entry.autoDeathIssued &&
                !entry.fatalDamageApplied) {
                return true;
            }
        }

        return false;
    }

    void ResetSession(std::string_view reason)
    {
        std::scoped_lock lk(g_lock);
        ResetSessionLocked(reason, true);
    }

    bool IsSessionActive()
    {
        std::scoped_lock lk(g_lock);
        return g_session.active;
    }

    SessionSnapshot GetSessionSnapshot()
    {
        std::scoped_lock lk(g_lock);
        return g_session;
    }
}
