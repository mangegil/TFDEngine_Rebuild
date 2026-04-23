#include "TFDActor.h"

#include "TFDPreCombatGreet.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <string_view>

#include <RE/Skyrim.h>
#include <RE/A/ActorValues.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include "TFDDefeatMonitor.h"
#include "TFDInteractionRouter.h"
#include "TFDLocation.h"
#include "TFDHostilityController.h"
#include "TFDTame.h"
#include "TFDTeammateManager.h"
#include "TFDFlowController.h"
#include "TFDPayModel.h"
#include "TFDPleasureRuntime.h"
#include "TFDTransition.h"
#include "TFDExtortion.h"

namespace TFD::PreCombatGreet
{
    RE::Actor* GetRecentActorLockedNoLock(double maxAgeSec);

    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr double kManualWindowSec = 120.0;
        constexpr double kCooldownAfterDoneSec = 120.0;
        constexpr double kCooldownAfterFailSec = 5.0;
        constexpr double kCooldownAfterPlayerAttackSec = 1.0;
        constexpr double kRecentActorSoftAgeSec = 12.0;
        constexpr double kStickyReopenRetrySec = 0.90;
        constexpr double kStickyTerminalSuppressSec = 0.75;
        constexpr double kDialogueCloseResolveDelaySec = 1.20;
        constexpr double kNegotiationRefreshSec = 0.15;
        constexpr double kPreCombatOutcomeGraceSec = 1.25;
        constexpr double kPreCombatOutcomeRetryDelaySec = 0.20;
        constexpr unsigned kDialogueOpenRetryLimit = 3;
        constexpr double kDialogueOpenRetryDelaySec = 0.35;
        constexpr double kPostHandoffSettleBlockSec = 1.25;
        constexpr double kHotkeyCooldownSec = 3.0;

        constexpr const char* kPreCombatOutcomePayEvent = "TFDPreCombatOutcomePay";
        constexpr const char* kPreCombatOutcomeFightEvent = "TFDPreCombatOutcomeFight";
        constexpr const char* kPreCombatOutcomeCaptiveEvent = "TFDPreCombatOutcomeCaptive";
        constexpr const char* kPreCombatOutcomeJoinEnemyEvent = "TFDPreCombatOutcomeJoinEnemy";
        constexpr const char* kPreCombatOutcomeRecruitEvent = "TFDPreCombatOutcomeRecruit";
        constexpr const char* kPreCombatOutcomeReleaseEvent = "TFDPreCombatOutcomeRelease";
        constexpr const char* kPreCombatOutcomeFollowEvent = "TFDPreCombatOutcomeFollow";
        constexpr const char* kPreCombatOutcomeFollowEndEvent = "TFDPreCombatOutcomeFollowEnd";
        constexpr const char* kPreCombatOutcomePleasureEvent = "TFDPreCombatOutcomePleasure";
        constexpr const char* kPreCombatTerminalPendingEvent = "TFDPreCombatTerminalPending";
        constexpr const char* kPreCombatDialogueConfirmedEvent = "TFDPreCombatDialogueConfirmed";

        struct Pending
        {
            RE::FormID truceSessionId{ 0 };
            TFD::InteractionRouter::Action action{ TFD::InteractionRouter::Action::None };

            double expiresSec{ 0.0 };
            bool dialogueRequested{ false };
            bool dialogSeen{ false };
            bool assignSent{ false };

            bool stickyReopenPending{ false };
            bool terminalChoiceCommitted{ false };
            bool payFollowupPending{ false };
            bool pleasureChoiceCommitted{ false };
            double nextStickyRetrySec{ 0.0 };
            double stickySuppressTerminalUntilSec{ 0.0 };
            double nextPreserveHandoffLogSec{ 0.0 };

            bool dialogueClosePending{ false };
            double dialogueCloseResolveAtSec{ 0.0 };
            double nextNegotiationRefreshSec{ 0.0 };
            double postCloseOutcomeGraceUntilSec{ 0.0 };
            double nextPostCloseOutcomeLogSec{ 0.0 };
            unsigned dialogueOpenRetryCount{ 0 };
            double nextDialogueOpenRetrySec{ 0.0 };
        };

        std::atomic_bool gInstalled{ false };
        std::atomic_bool gSuspended{ false };
        std::atomic_bool gRunning{ false };
        std::atomic_flag gTickPending = ATOMIC_FLAG_INIT;
        std::thread gWorker{};

        std::mutex gLock;
        std::unordered_map<std::uint32_t, Pending> gPending;
        std::unordered_map<std::uint32_t, double> gCooldownUntil;
        Clock::time_point gT0 = Clock::now();

        std::uint32_t gRecentActorHandle = 0;
        double gRecentActorCachedAtSec = 0.0;
        double gRecentActorUntilSec = 0.0;
        RE::FormID gRecentActorCellFormID = 0;
        RE::FormID gRecentActorWorldspaceFormID = 0;
        bool gRecentActorInterior = false;
        double gRecentActorLastSoftAgeLogSec = 0.0;
        double gPostHandoffBlockUntilSec = 0.0;
        RE::FormID gPostHandoffBlockActorFormID = 0;
        double gHotkeyCooldownUntilSec = 0.0;
        RE::FormID gHotkeyCooldownActorFormID = 0;

        bool ResolvePreCombatTerminalOutcomeLocked(TFD::FlowController::PreCombatOutcome outcome, std::uint32_t actorFormID, const char* reason);
        void MarkTerminalChoiceCommittedLocked(Pending& pending, const char* reason);
        void ResetDialogueOpenRetryLocked(Pending& pending);
        void CancelPreCombatDialogueOpenLocked(RE::Actor* actor, Pending& pending, const char* reason);

        bool IsGraceEventName(std::string_view eventName)
        {
            return eventName == std::string_view("TFDPreCombatOutcomeRelease") ||
                eventName == std::string_view("TFDPreCombatOutcomeFollow") ||
                eventName == std::string_view("TFDPreCombatOutcomeReleaseEnd") ||
                eventName == std::string_view("TFDPreCombatOutcomeFollowEnd");
        }

        RE::Actor* ResolveActorFromEventArgRaw(const char* eventArg)
        {
            if (!eventArg || !*eventArg) {
                return nullptr;
            }

            char* end = nullptr;
            const auto raw = std::strtoul(eventArg, &end, 0);
            if (end == nullptr || end == eventArg) {
                return nullptr;
            }

            return RE::TESForm::LookupByID<RE::Actor>(static_cast<RE::FormID>(raw));
        }

        double NowSec()
        {
            return std::chrono::duration<double>(Clock::now() - gT0).count();
        }

        std::uint32_t GetHandleId(RE::Actor* actor)
        {
            return actor ? actor->GetHandle().native_handle() : 0;
        }

        bool IsDialogueOpen()
        {
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
        }

        bool IsPlayerDown()
        {
            RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return false;
            }

            auto* state = player->AsActorState();
            return state && state->IsBleedingOut();
        }

        static RE::TESObjectCELL* GetPlayerParentCell()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            return player ? player->GetParentCell() : nullptr;
        }

        static RE::TESWorldSpace* GetCellWorldspace(RE::TESObjectCELL* cell)
        {
            if (!cell || cell->IsInteriorCell()) {
                return nullptr;
            }
            return cell->GetRuntimeData().worldSpace;
        }

        static bool IsPlayerStillInCachedSpace()
        {
            if (gRecentActorHandle == 0) {
                return false;
            }

            auto* cell = GetPlayerParentCell();
            if (!cell) {
                return false;
            }

            const bool isInterior = cell->IsInteriorCell();
            if (isInterior != gRecentActorInterior) {
                return false;
            }

            if (isInterior) {
                return cell->GetFormID() == gRecentActorCellFormID;
            }

            auto* ws = GetCellWorldspace(cell);
            const auto wsid = ws ? ws->GetFormID() : 0u;
            return wsid == gRecentActorWorldspaceFormID;
        }

        void CacheRecentActor(RE::Actor* actor, double holdSec, const char* reason)
        {
            if (!actor) {
                return;
            }

            gRecentActorHandle = actor->GetHandle().native_handle();
            gRecentActorCachedAtSec = NowSec();
            gRecentActorUntilSec = gRecentActorCachedAtSec + holdSec;
            if (auto* cell = GetPlayerParentCell()) {
                gRecentActorInterior = cell->IsInteriorCell();
                gRecentActorCellFormID = cell->GetFormID();
                auto* ws = GetCellWorldspace(cell);
                gRecentActorWorldspaceFormID = ws ? ws->GetFormID() : 0u;
            }
            else {
                gRecentActorInterior = false;
                gRecentActorCellFormID = 0;
                gRecentActorWorldspaceFormID = 0;
            }
            gRecentActorLastSoftAgeLogSec = 0.0;

            spdlog::info(
                "[TFD][PreCombatGreet] recent actor cached actor={:08X} hold={:.1f}s reason={}",
                actor->GetFormID(),
                holdSec,
                reason ? reason : "unknown");
        }

        void ClearRecentActor(const char* reason)
        {
            if (gRecentActorHandle == 0) {
                return;
            }

            spdlog::info(
                "[TFD][PreCombatGreet] recent actor cleared actorHandle={:08X} reason={}",
                gRecentActorHandle,
                reason ? reason : "unknown");

            gRecentActorHandle = 0;
            gRecentActorCachedAtSec = 0.0;
            gRecentActorUntilSec = 0.0;
            gRecentActorCellFormID = 0;
            gRecentActorWorldspaceFormID = 0;
            gRecentActorInterior = false;
            gRecentActorLastSoftAgeLogSec = 0.0;
        }

        void ClearPostHandoffBlockLocked(const char* reason)
        {
            if (gPostHandoffBlockUntilSec <= 0.0 && gPostHandoffBlockActorFormID == 0) {
                return;
            }

            spdlog::info(
                "[TFD][PreCombatGreet] post handoff block cleared actor={:08X} reason={}",
                gPostHandoffBlockActorFormID,
                reason ? reason : "unknown");

            gPostHandoffBlockUntilSec = 0.0;
            gPostHandoffBlockActorFormID = 0;
        }

        void ArmPostHandoffBlockLocked(RE::Actor* actor, double durationSec, const char* reason)
        {
            if (durationSec <= 0.0) {
                return;
            }

            const double now = NowSec();
            gPostHandoffBlockUntilSec = now + durationSec;
            gPostHandoffBlockActorFormID = actor ? actor->GetFormID() : 0u;

            spdlog::info(
                "[TFD][PreCombatGreet] post handoff block armed actor={:08X} duration={:.2f}s reason={}",
                gPostHandoffBlockActorFormID,
                durationSec,
                reason ? reason : "unknown");
        }

        void ClearHotkeyCooldownLocked(const char* reason)
        {
            if (gHotkeyCooldownUntilSec <= 0.0 && gHotkeyCooldownActorFormID == 0) {
                return;
            }

            spdlog::info(
                "[TFD][PreCombatGreet] hotkey cooldown cleared actor={:08X} reason={}",
                gHotkeyCooldownActorFormID,
                reason ? reason : "unknown");

            gHotkeyCooldownUntilSec = 0.0;
            gHotkeyCooldownActorFormID = 0;
        }

        void ArmHotkeyCooldownLocked(RE::Actor* actor, double durationSec, const char* reason)
        {
            if (durationSec <= 0.0) {
                return;
            }

            const double now = NowSec();
            gHotkeyCooldownUntilSec = now + durationSec;
            gHotkeyCooldownActorFormID = actor ? actor->GetFormID() : 0u;

            spdlog::info(
                "[TFD][PreCombatGreet] hotkey cooldown armed actor={:08X} duration={:.2f}s reason={}",
                gHotkeyCooldownActorFormID,
                durationSec,
                reason ? reason : "unknown");
        }

        bool IsHotkeyCooldownActiveLocked(double nowSec, double* outRemainingSec = nullptr)
        {
            if (gHotkeyCooldownUntilSec <= 0.0) {
                if (outRemainingSec) {
                    *outRemainingSec = 0.0;
                }
                return false;
            }

            if (nowSec >= gHotkeyCooldownUntilSec) {
                ClearHotkeyCooldownLocked("expired");
                if (outRemainingSec) {
                    *outRemainingSec = 0.0;
                }
                return false;
            }

            if (outRemainingSec) {
                *outRemainingSec = gHotkeyCooldownUntilSec - nowSec;
            }
            return true;
        }

        bool IsPostHandoffBlockActiveLocked(double nowSec, double* outRemainingSec = nullptr)
        {
            if (gPostHandoffBlockUntilSec <= 0.0) {
                if (outRemainingSec) {
                    *outRemainingSec = 0.0;
                }
                return false;
            }

            if (nowSec >= gPostHandoffBlockUntilSec) {
                ClearPostHandoffBlockLocked("expired");
                if (outRemainingSec) {
                    *outRemainingSec = 0.0;
                }
                return false;
            }

            if (outRemainingSec) {
                *outRemainingSec = gPostHandoffBlockUntilSec - nowSec;
            }
            return true;
        }

        bool HasStickyPendingLocked()
        {
            for (const auto& [handle, pending] : gPending) {
                if (pending.stickyReopenPending && pending.dialogueRequested && !pending.terminalChoiceCommitted) {
                    return true;
                }
            }
            return false;
        }

        bool HasCommittedTerminalPendingLocked()
        {
            for (const auto& [handle, pending] : gPending) {
                (void)handle;
                if (!pending.dialogueRequested) {
                    continue;
                }

                if (pending.terminalChoiceCommitted || pending.payFollowupPending) {
                    return true;
                }

                auto sp = RE::Actor::LookupByHandle(handle);
                if (TFD::Extortion::IsActive(sp.get())) {
                    return true;
                }
            }
            return false;
        }

        void SendBridgeEvent(const char* eventName, RE::TESForm* sender)
        {
            if (!eventName) {
                return;
            }

            auto* task = SKSE::GetTaskInterface();
            if (!task) {
                return;
            }

            const std::string name{ eventName };
            std::uint32_t handle = 0;

            if (auto* actor = sender ? sender->As<RE::Actor>() : nullptr) {
                handle = actor->GetHandle().native_handle();
            }

            task->AddTask([name, handle]() {
                RE::TESForm* outSender = nullptr;

                if (handle != 0) {
                    auto sp = RE::Actor::LookupByHandle(handle);
                    outSender = sp.get();
                    if (!outSender) {
                        return;
                    }
                }

                auto* src = SKSE::GetModCallbackEventSource();
                if (!src) {
                    return;
                }

                SKSE::ModCallbackEvent ev{ name.c_str(), "", 0.0f, outSender };
                src->SendEvent(&ev);
                });
        }

        struct BridgeEventNames
        {
            const char* assign{ nullptr };
            const char* clear{ nullptr };
            const char* clearAll{ nullptr };
        };

        BridgeEventNames GetBridgeEventNames(TFD::InteractionRouter::Action action)
        {
            switch (action) {
            case TFD::InteractionRouter::Action::TrucePreCombat:
                return { "TFDPreCombatAssign", "TFDPreCombatClear", "TFDPreCombatClearAll" };
            default:
                return {};
            }
        }

        void ClearAllBridgeAliases()
        {
            SendBridgeEvent("TFDPreCombatClearAll", nullptr);
        }

        void ClearDialogueClosePendingLocked(Pending& pending)
        {
            pending.dialogueClosePending = false;
            pending.dialogueCloseResolveAtSec = 0.0;
        }

        bool ShouldHoldForOutcomeAfterDialogueCloseLocked(RE::Actor* actor, Pending& pending, double nowSec)
        {
            if (!actor) {
                return false;
            }

            if (pending.action != TFD::InteractionRouter::Action::TrucePreCombat) {
                return false;
            }

            if (pending.terminalChoiceCommitted || pending.payFollowupPending || pending.pleasureChoiceCommitted) {
                return false;
            }

            if (nowSec >= pending.postCloseOutcomeGraceUntilSec) {
                return false;
            }

            const auto snapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
            const auto actorFormID = actor->GetFormID();
            const bool sameActor = snapshot.primaryActorFormID == 0 || snapshot.primaryActorFormID == actorFormID;
            const bool flowStillPreCombat = snapshot.root == TFD::FlowController::RootFlow::PreCombat;
            const bool flowPayFollowup = snapshot.sub == TFD::FlowController::SubFlow::PreCombatPayFollowup;
            const bool extortionActive = TFD::Extortion::HasActive();

            if (!sameActor) {
                return false;
            }

            if (!flowStillPreCombat && !flowPayFollowup && !extortionActive) {
                return false;
            }

            if (nowSec >= pending.nextPostCloseOutcomeLogSec) {
                pending.nextPostCloseOutcomeLogSec = nowSec + 0.50;
                spdlog::info(
                    "[TFD][PreCombatGreet] hold close abort actor={:08X} action={} root={} sub={} extortion={} graceUntil={:.2f} now={:.2f}",
                    actorFormID,
                    TFD::InteractionRouter::ToString(pending.action),
                    TFD::FlowController::Controller::ToString(snapshot.root),
                    TFD::FlowController::Controller::ToString(snapshot.sub),
                    extortionActive ? 1 : 0,
                    pending.postCloseOutcomeGraceUntilSec,
                    nowSec);
            }

            pending.dialogueClosePending = true;
            pending.dialogueCloseResolveAtSec = std::min(pending.postCloseOutcomeGraceUntilSec, nowSec + kPreCombatOutcomeRetryDelaySec);
            return true;
        }

        void ArmDialogueClosePendingLocked(RE::Actor* actor, Pending& pending, double nowSec, const char* reason)
        {
            if (pending.dialogueClosePending) {
                return;
            }

            pending.dialogueClosePending = true;
            pending.dialogueCloseResolveAtSec = nowSec + kDialogueCloseResolveDelaySec;
            pending.nextNegotiationRefreshSec = nowSec;
            if (pending.action == TFD::InteractionRouter::Action::TrucePreCombat &&
                !pending.terminalChoiceCommitted &&
                !pending.payFollowupPending &&
                !pending.pleasureChoiceCommitted &&
                pending.postCloseOutcomeGraceUntilSec < nowSec) {
                pending.postCloseOutcomeGraceUntilSec = nowSec + kPreCombatOutcomeGraceSec;
                pending.nextPostCloseOutcomeLogSec = nowSec;
            }

            spdlog::info(
                "[TFD][PreCombatGreet] dialogue close armed actor={:08X} action={} resolveIn={:.2f}s reason={} graceUntil={:.2f}",
                actor ? actor->GetFormID() : 0u,
                TFD::InteractionRouter::ToString(pending.action),
                kDialogueCloseResolveDelaySec,
                reason ? reason : "unknown",
                pending.postCloseOutcomeGraceUntilSec);
        }

        void EnforceNegotiationState(RE::Actor* actor, Pending& pending, double nowSec)
        {
            if (!actor || !actor->Is3DLoaded()) {
                return;
            }

            if (actor->IsInCombat()) {
                actor->StopCombat();
            }

            if (nowSec < pending.nextNegotiationRefreshSec) {
                return;
            }

            actor->EvaluatePackage();
            pending.nextNegotiationRefreshSec = nowSec + kNegotiationRefreshSec;
        }

        bool IsCandidate(RE::Actor* actor, RE::PlayerCharacter* player)
        {
            if (!actor || !player) {
                return false;
            }

            if (actor->IsDead() || actor->IsDisabled()) {
                return false;
            }

            if (!actor->Is3DLoaded()) {
                return false;
            }

            if (actor->GetFormID() == player->GetFormID()) {
                return false;
            }

            if (actor->IsPlayerTeammate() ||
                TFD::TeammateManager::IsActiveFollowerActor(actor) ||
                TFD::Tame::IsCompanion(actor)) {
                return false;
            }

            if (TFD::FlowController::IsPreCombatBlocked()) {
                return false;
            }

            if (TFD::Transition::IsRecoveryActive()) {
                return false;
            }

            if (IsPlayerDown()) {
                return false;
            }

            if (!actor->IsHostileToActor(player)) {
                return false;
            }

            return true;
        }

        bool IsActorStillValid(RE::Actor* actor)
        {
            if (!actor) {
                return false;
            }

            if (actor->IsDead()) {
                return false;
            }

            if (actor->IsDisabled()) {
                return false;
            }

            if (!actor->Is3DLoaded()) {
                return false;
            }

            return true;
        }

        bool IsPleasureDialogueHandoffAction(TFD::InteractionRouter::Action action)
        {
            return action == TFD::InteractionRouter::Action::TrucePreCombat;
        }

        Pending* FindPendingLocked(RE::Actor* actor)
        {
            if (!actor) {
                return nullptr;
            }

            auto it = gPending.find(GetHandleId(actor));
            if (it == gPending.end()) {
                return nullptr;
            }

            return std::addressof(it->second);
        }

        Pending* ResolveTerminalPendingLocked(RE::Actor* actor)
        {
            if (actor) {
                return FindPendingLocked(actor);
            }

            if (gPending.size() != 1) {
                return nullptr;
            }

            return std::addressof(gPending.begin()->second);
        }

        RE::Actor* ResolveSinglePendingActorLocked()
        {
            if (gPending.size() != 1) {
                return nullptr;
            }

            auto sp = RE::Actor::LookupByHandle(gPending.begin()->first);
            return sp.get();
        }

        RE::Actor* ResolvePleasureEventActor(const SKSE::ModCallbackEvent* ev)
        {
            if (!ev) {
                return nullptr;
            }

            if (auto* actor = ev->sender ? ev->sender->As<RE::Actor>() : nullptr) {
                return actor;
            }

            const auto* rawArg = ev->strArg.c_str();
            if (!rawArg || rawArg[0] == '\0') {
                return nullptr;
            }

            try {
                const auto actorId = static_cast<RE::FormID>(std::stoul(rawArg, nullptr, 0));
                return RE::TESForm::LookupByID<RE::Actor>(actorId);
            }
            catch (...) {
                return nullptr;
            }
        }

        std::uint32_t ResolveSinglePendingActorFormIDLocked()
        {
            if (gPending.size() != 1) {
                return 0;
            }

            auto it = gPending.begin();
            auto sp = RE::Actor::LookupByHandle(it->first);
            auto* actor = sp.get();
            return actor ? actor->GetFormID() : 0;
        }

        std::uint32_t ResolveFlowActorFormIDLocked(RE::Actor* preferred)
        {
            if (preferred) {
                return preferred->GetFormID();
            }

            const auto pendingFormID = ResolveSinglePendingActorFormIDLocked();
            if (pendingFormID != 0) {
                return pendingFormID;
            }

            return TFD::FlowController::Controller::GetSingleton().GetSnapshot().primaryActorFormID;
        }

        void MarkTerminalChoiceCommittedLocked(Pending& pending, const char* reason)
        {
            pending.terminalChoiceCommitted = true;
            pending.stickyReopenPending = false;
            pending.payFollowupPending = false;
            pending.pleasureChoiceCommitted = false;
            pending.stickySuppressTerminalUntilSec = 0.0;
            pending.nextStickyRetrySec = 0.0;
            pending.nextPreserveHandoffLogSec = 0.0;
            pending.postCloseOutcomeGraceUntilSec = 0.0;
            pending.nextPostCloseOutcomeLogSec = 0.0;
            ResetDialogueOpenRetryLocked(pending);
            ClearDialogueClosePendingLocked(pending);
            spdlog::info(
                "[TFD][PreCombatGreet] terminal choice committed action={} reason={}",
                TFD::InteractionRouter::ToString(pending.action),
                reason ? reason : "unknown");
        }

        void MarkPleasureChoiceCommittedLocked(Pending& pending, RE::Actor* actor, const char* reason)
        {
            pending.terminalChoiceCommitted = true;
            pending.stickyReopenPending = false;
            pending.payFollowupPending = false;
            pending.pleasureChoiceCommitted = true;
            pending.stickySuppressTerminalUntilSec = 0.0;
            pending.nextStickyRetrySec = 0.0;
            pending.nextPreserveHandoffLogSec = 0.0;
            pending.postCloseOutcomeGraceUntilSec = 0.0;
            pending.nextPostCloseOutcomeLogSec = 0.0;
            ResetDialogueOpenRetryLocked(pending);
            ClearDialogueClosePendingLocked(pending);
            if (actor) {
                CacheRecentActor(actor, 0.0, reason ? reason : "precombat_pleasure");
            }
            spdlog::info(
                "[TFD][PreCombatGreet] pleasure choice committed action={} actor={:08X} reason={}",
                TFD::InteractionRouter::ToString(pending.action),
                actor ? actor->GetFormID() : 0u,
                reason ? reason : "unknown");
        }

        bool ShouldSuppressTerminalEventLocked(const Pending& pending, RE::Actor* actor, const char* eventName)
        {
            if (!pending.dialogueRequested) {
                return false;
            }

            if (!pending.stickyReopenPending) {
                return false;
            }

            if (pending.terminalChoiceCommitted) {
                return false;
            }

            if (IsDialogueOpen()) {
                return false;
            }

            const double now = NowSec();
            if (now > pending.stickySuppressTerminalUntilSec) {
                return false;
            }

            spdlog::info(
                "[TFD][PreCombatGreet] suppress terminal event={} actor={:08X} action={} stickyUntil={:.2f} now={:.2f}",
                eventName ? eventName : "unknown",
                actor ? actor->GetFormID() : 0u,
                TFD::InteractionRouter::ToString(pending.action),
                pending.stickySuppressTerminalUntilSec,
                now);
            return true;
        }

        bool ResolvePreCombatTerminalOutcomeLocked(TFD::FlowController::PreCombatOutcome outcome, std::uint32_t actorFormID, const char* reason)
        {
            if (actorFormID == 0) {
                return false;
            }

            auto& flow = TFD::FlowController::Controller::GetSingleton();
            if (!flow.ResolvePreCombatOutcome(outcome, actorFormID, reason ? reason : "unknown")) {
                return false;
            }

            if (outcome == TFD::FlowController::PreCombatOutcome::RecruitEnemy ||
                outcome == TFD::FlowController::PreCombatOutcome::Release ||
                outcome == TFD::FlowController::PreCombatOutcome::Follow ||
                outcome == TFD::FlowController::PreCombatOutcome::Cancel ||
                outcome == TFD::FlowController::PreCombatOutcome::Failed) {
                return flow.CompleteTerminalContext(reason ? reason : "unknown");
            }

            return true;
        }

        void QueuePreCombatCaptiveTransition(std::uint32_t actorFormID, const char* reason)
        {
            if (actorFormID == 0) {
                return;
            }

            auto* task = SKSE::GetTaskInterface();
            if (!task) {
                spdlog::warn("[TFD][PreCombatGreet] captive transition queue failed actor={:08X} reason=no_task_interface source={}",
                    actorFormID,
                    reason ? reason : "unknown");
                return;
            }

            const std::string why = reason ? reason : "precombat_captive";
            task->AddTask([actorFormID, why]() {
                auto runtimeHandlers = TFD::Transition::DefeatGlue::BuildTransitionRuntimeHandlers();
                auto captiveHandlers = TFD::Transition::DefeatGlue::BuildTransitionCaptiveHandlers();

                if (!TFD::Transition::ResolveCaptiveMarkerForOutcome(runtimeHandlers)) {
                    spdlog::warn("[TFD][PreCombatGreet] captive transition rejected actor={:08X} reason=no_captive_marker source={}",
                        actorFormID,
                        why);
                    return;
                }

                const bool ok = TFD::Transition::CompleteCaptiveTransitionNow(why.c_str(), runtimeHandlers, captiveHandlers);
                if (ok) {
                    TFD::PreCombatGreet::OnCaptiveHandoffArrived();
                    spdlog::info("[TFD][PreCombatGreet] captive transition started actor={:08X} source={}",
                        actorFormID,
                        why);
                }
                else {
                    spdlog::warn("[TFD][PreCombatGreet] captive transition failed actor={:08X} source={}",
                        actorFormID,
                        why);
                }
                });
        }

        bool ShouldStickyReopenLocked(const Pending& pending)
        {
            (void)pending;
            // Final policy: only BleedOut stays sticky.
            // PreCombat must abort back to aggression when dialogue closes without a committed choice.
            return false;
        }

        bool IsDialogueOpenActiveForPreCombatLocked()
        {
            if (!TFD::InteractionRouter::DialogueOpen::IsActive()) {
                return false;
            }

            return TFD::InteractionRouter::DialogueOpen::GetMode() ==
                TFD::InteractionRouter::DialogueOpen::Mode::PreCombatTruce;
        }

        void ResetDialogueOpenRetryLocked(Pending& pending)
        {
            pending.dialogueOpenRetryCount = 0;
            pending.nextDialogueOpenRetrySec = 0.0;
        }

        void CancelPreCombatDialogueOpenLocked(RE::Actor* actor, Pending& pending, const char* reason)
        {
            pending.dialogSeen = true;
            pending.stickyReopenPending = false;
            pending.nextStickyRetrySec = 0.0;
            pending.stickySuppressTerminalUntilSec = 0.0;
            pending.nextPreserveHandoffLogSec = 0.0;
            ResetDialogueOpenRetryLocked(pending);
            ClearDialogueClosePendingLocked(pending);

            if (!IsDialogueOpenActiveForPreCombatLocked()) {
                return;
            }

            TFD::InteractionRouter::DialogueOpen::Cancel();
            spdlog::info(
                "[TFD][PreCombatGreet] cancel precombat dialogue open actor={:08X} action={} reason={}",
                actor ? actor->GetFormID() : 0u,
                TFD::InteractionRouter::ToString(pending.action),
                reason ? reason : "unknown");
        }

        bool TryRetryDialogueOpenLocked(RE::Actor* actor, Pending& pending, double nowSec, const char* reason)
        {
            if (!actor || !pending.dialogueRequested || pending.dialogSeen) {
                return false;
            }
            if (pending.action != TFD::InteractionRouter::Action::TrucePreCombat) {
                return false;
            }
            if (pending.terminalChoiceCommitted || pending.payFollowupPending || pending.pleasureChoiceCommitted) {
                return false;
            }
            if (IsDialogueOpenActiveForPreCombatLocked()) {
                return true;
            }
            if (nowSec < pending.nextDialogueOpenRetrySec) {
                return true;
            }
            if (pending.dialogueOpenRetryCount >= kDialogueOpenRetryLimit) {
                return false;
            }

            ++pending.dialogueOpenRetryCount;
            pending.nextDialogueOpenRetrySec = nowSec + kDialogueOpenRetryDelaySec;
            pending.stickyReopenPending = false;
            pending.nextStickyRetrySec = 0.0;
            pending.stickySuppressTerminalUntilSec = 0.0;
            ClearDialogueClosePendingLocked(pending);

            EnforceNegotiationState(actor, pending, nowSec);
            TFD::InteractionRouter::DialogueOpen::BeginPreCombatTruce(actor);
            CacheRecentActor(actor, 0.0, reason ? reason : "dialogue_open_retry");

            spdlog::info(
                "[TFD][PreCombatGreet] dialogue open retry actor={:08X} action={} retry={}/{} reason={}",
                actor->GetFormID(),
                TFD::InteractionRouter::ToString(pending.action),
                pending.dialogueOpenRetryCount,
                kDialogueOpenRetryLimit,
                reason ? reason : "unknown");

            return true;
        }

        TFD::Tame::ReleaseReason ResolveDialogueClosedReleaseReasonLocked(const Pending& pending)
        {
            auto snapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();

            if (pending.action == TFD::InteractionRouter::Action::TrucePreCombat) {
                if (snapshot.root == TFD::FlowController::RootFlow::InCombat) {
                    return TFD::Tame::ReleaseReason::FightChoice;
                }

                if (pending.terminalChoiceCommitted || pending.payFollowupPending || pending.pleasureChoiceCommitted) {
                    return TFD::Tame::ReleaseReason::FlowHandoff;
                }

                if (snapshot.root == TFD::FlowController::RootFlow::Captive ||
                    snapshot.root == TFD::FlowController::RootFlow::Victory ||
                    snapshot.terminalResolved ||
                    snapshot.root == TFD::FlowController::RootFlow::None) {
                    return TFD::Tame::ReleaseReason::FlowHandoff;
                }
            }

            return TFD::Tame::ReleaseReason::DialogueClosed;
        }

        void BeginStickyReopenLocked(RE::Actor* actor, Pending& pending, const char* reason)
        {
            if (!actor) {
                return;
            }

            const double now = NowSec();
            pending.dialogSeen = false;
            pending.stickyReopenPending = true;
            pending.terminalChoiceCommitted = false;
            pending.expiresSec = now + kManualWindowSec;
            pending.nextStickyRetrySec = now + kStickyReopenRetrySec;
            pending.stickySuppressTerminalUntilSec = now + kStickyTerminalSuppressSec;
            pending.nextPreserveHandoffLogSec = 0.0;
            ResetDialogueOpenRetryLocked(pending);
            CacheRecentActor(actor, 0.0, reason ? reason : "sticky_reopen");

            if (pending.action == TFD::InteractionRouter::Action::TrucePreCombat) {
                TFD::InteractionRouter::DialogueOpen::BeginPreCombatTruce(actor);
            }

            spdlog::info(
                "[TFD][PreCombatGreet] sticky reopen actor={:08X} action={} reason={}",
                actor->GetFormID(),
                TFD::InteractionRouter::ToString(pending.action),
                reason ? reason : "unknown");
        }

        void ArmPreCombatPayFollowupLocked(RE::Actor* actor, Pending& pending, const char* reason)
        {
            if (!actor) {
                return;
            }

            const double now = NowSec();
            pending.payFollowupPending = true;
            pending.pleasureChoiceCommitted = false;
            pending.dialogSeen = true;
            pending.terminalChoiceCommitted = false;
            pending.stickyReopenPending = false;
            pending.nextStickyRetrySec = now;
            pending.stickySuppressTerminalUntilSec = now + kStickyTerminalSuppressSec;
            pending.nextPreserveHandoffLogSec = 0.0;
            pending.postCloseOutcomeGraceUntilSec = 0.0;
            pending.nextPostCloseOutcomeLogSec = 0.0;
            ResetDialogueOpenRetryLocked(pending);
            ClearDialogueClosePendingLocked(pending);
            CacheRecentActor(actor, 0.0, reason ? reason : "precombat_pay_followup");
            spdlog::info(
                "[TFD][PreCombatGreet] pay followup armed actor={:08X} action={} reason={}",
                actor->GetFormID(),
                TFD::InteractionRouter::ToString(pending.action),
                reason ? reason : "unknown");
        }

        bool HasProtectedPleasurePendingLocked()
        {
            for (auto& [handle, pending] : gPending) {
                if (!pending.dialogueRequested) {
                    continue;
                }
                auto sp = RE::Actor::LookupByHandle(handle);
                auto* actor = sp.get();
                if (!actor) {
                    continue;
                }
                if (TFD::PleasureRuntime::ShouldProtectPendingDialogue(actor)) {
                    return true;
                }
            }
            return false;
        }

        void CleanupOne(
            RE::Actor* actor,
            Pending& pending,
            double cooldownSec,
            const char* reason,
            TFD::Tame::ReleaseReason releaseReason)
        {

            if (pending.truceSessionId != 0) {
                TFD::HostilityController::ReleaseSession(pending.truceSessionId, releaseReason);
                pending.truceSessionId = 0;
            }

            if (pending.assignSent) {
                const auto bridge = GetBridgeEventNames(pending.action);
                if (bridge.clear) {
                    SendBridgeEvent(bridge.clear, actor);
                }
                pending.assignSent = false;
            }

            pending.postCloseOutcomeGraceUntilSec = 0.0;
            pending.nextPostCloseOutcomeLogSec = 0.0;

            if (pending.action == TFD::InteractionRouter::Action::TrucePreCombat) {
                TFD::InteractionRouter::ClearInteractionStateValue();
                if (releaseReason == TFD::Tame::ReleaseReason::FlowHandoff) {
                    ArmPostHandoffBlockLocked(actor, kPostHandoffSettleBlockSec, reason ? reason : "dialogue_handoff");
                    ArmHotkeyCooldownLocked(actor, kHotkeyCooldownSec, reason ? reason : "dialogue_handoff");
                }
            }

            if (actor) {
                gCooldownUntil[GetHandleId(actor)] = NowSec() + cooldownSec;

                spdlog::info(
                    "[TFD][PreCombatGreet] cleanup reason={} actor={:08X} action={}",
                    reason ? reason : "unknown",
                    actor->GetFormID(),
                    TFD::InteractionRouter::ToString(pending.action));
            }
            else {
                spdlog::info(
                    "[TFD][PreCombatGreet] cleanup reason={} actor=<none> action={}",
                    reason ? reason : "unknown",
                    TFD::InteractionRouter::ToString(pending.action));
            }

        }

        void ClearAllPendingLocked()
        {
            ClearAllBridgeAliases();
            TFD::Extortion::CancelAll("precombat_clear_all_pending");
            TFD::PayModel::ClearSharedGold("precombat_clear_all_pending");

            bool hadPreCombatPending = false;

            for (auto& [handle, pending] : gPending) {
                auto sp = RE::Actor::LookupByHandle(handle);
                auto* actor = sp.get();

                if (pending.action == TFD::InteractionRouter::Action::TrucePreCombat) {
                    hadPreCombatPending = true;
                }

                if (pending.truceSessionId != 0) {
                    TFD::HostilityController::ReleaseSession(pending.truceSessionId, TFD::Tame::ReleaseReason::Generic);
                    pending.truceSessionId = 0;
                }

                if (pending.assignSent) {
                    const auto bridge = GetBridgeEventNames(pending.action);
                    if (bridge.clear) {
                        SendBridgeEvent(bridge.clear, actor);
                    }
                    pending.assignSent = false;
                }
            }

            gPending.clear();

            if (hadPreCombatPending) {
                TFD::InteractionRouter::ClearInteractionStateValue();
            }
        }

        void AbortOnPlayerAttack(const RE::TESHitEvent* ev)
        {
            if (!ev) {
                return;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return;
            }

            auto* targetRef = ev->target.get();
            auto* causeRef = ev->cause.get();
            if (!targetRef || !causeRef) {
                return;
            }

            auto* targetActor = targetRef->As<RE::Actor>();
            auto* causeActor = causeRef->As<RE::Actor>();
            if (!targetActor || !causeActor) {
                return;
            }

            if (causeActor->GetFormID() != player->GetFormID()) {
                return;
            }

            const auto handle = GetHandleId(targetActor);

            std::scoped_lock lk(gLock);

            auto it = gPending.find(handle);
            if (it == gPending.end()) {
                return;
            }

            spdlog::info(
                "[TFD][PreCombatGreet] player attacked active target -> abort actor={:08X}",
                targetActor->GetFormID());

            ClearAllBridgeAliases();

            CleanupOne(targetActor, it->second, kCooldownAfterPlayerAttackSec, "player_attack", TFD::Tame::ReleaseReason::PlayerAggression);
            gPending.erase(it);
        }

        class HitSink : public RE::BSTEventSink<RE::TESHitEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESHitEvent* ev,
                RE::BSTEventSource<RE::TESHitEvent>*) override
            {
                AbortOnPlayerAttack(ev);
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        HitSink gHitSink{};

        class PleasureEventSink final : public RE::BSTEventSink<SKSE::ModCallbackEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* ev, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
            {
                if (!ev) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                const auto* rawName = ev->eventName.c_str();
                const std::string_view name = rawName ? std::string_view(rawName) : std::string_view{};
                if (name.empty()) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                auto* actor = ResolvePleasureEventActor(ev);

                if (name == kPreCombatTerminalPendingEvent) {
                    RE::Actor* pendingActor = actor;
                    unsigned pendingCount = 0;
                    {
                        std::scoped_lock lk(gLock);
                        if (!pendingActor) {
                            pendingActor = ResolveSinglePendingActorLocked();
                        }
                        if (!pendingActor) {
                            pendingActor = GetRecentActorLockedNoLock(kRecentActorSoftAgeSec);
                        }
                        Pending* matchedPending = ResolveTerminalPendingLocked(pendingActor);
                        if (matchedPending && matchedPending->dialogueRequested && !matchedPending->dialogSeen) {
                            spdlog::warn(
                                "[TFD][PreCombatGreet] terminal pending rejected event={} actor={:08X} reason=dialogue_not_confirmed",
                                rawName ? rawName : "unknown",
                                pendingActor ? pendingActor->GetFormID() : 0u);
                            return RE::BSEventNotifyControl::kContinue;
                        }
                        if (matchedPending) {
                            MarkTerminalChoiceCommittedLocked(*matchedPending, rawName);
                            CancelPreCombatDialogueOpenLocked(pendingActor ? pendingActor : actor, *matchedPending, "terminal_pending");
                        }
                        pendingCount = static_cast<unsigned>(gPending.size());
                    }

                    TFD::Extortion::HandlePreCombatTerminalPendingEvent(pendingActor ? pendingActor : actor, rawName);
                    spdlog::info(
                        "[TFD][PreCombatGreet] terminal pending event={} arg={} actor={:08X} pendingActor={:08X} pendingCount={}",
                        rawName ? rawName : "unknown",
                        ev->strArg.c_str(),
                        actor ? actor->GetFormID() : 0u,
                        pendingActor ? pendingActor->GetFormID() : 0u,
                        pendingCount);
                    return RE::BSEventNotifyControl::kContinue;
                }

                if (name == kPreCombatDialogueConfirmedEvent) {
                    RE::Actor* confirmedActor = actor;
                    bool accepted = false;
                    unsigned pendingCount = 0;
                    {
                        std::scoped_lock lk(gLock);
                        if (!confirmedActor) {
                            confirmedActor = ResolveSinglePendingActorLocked();
                        }
                        if (!confirmedActor) {
                            confirmedActor = GetRecentActorLockedNoLock(kRecentActorSoftAgeSec);
                        }

                        if (auto* matchedPending = FindPendingLocked(confirmedActor)) {
                            matchedPending->dialogSeen = true;
                            matchedPending->stickyReopenPending = false;
                            matchedPending->nextStickyRetrySec = 0.0;
                            matchedPending->stickySuppressTerminalUntilSec = 0.0;
                            ResetDialogueOpenRetryLocked(*matchedPending);
                            ClearDialogueClosePendingLocked(*matchedPending);
                            accepted = true;
                        }
                        pendingCount = static_cast<unsigned>(gPending.size());
                    }

                    spdlog::info(
                        "[TFD][PreCombatGreet] dialogue confirmed event={} arg={} actor={:08X} accepted={} pendingCount={}",
                        rawName ? rawName : "unknown",
                        ev->strArg.c_str(),
                        confirmedActor ? confirmedActor->GetFormID() : 0u,
                        accepted ? 1 : 0,
                        pendingCount);
                    return RE::BSEventNotifyControl::kContinue;
                }

                if (name == kPreCombatOutcomePayEvent ||
                    name == kPreCombatOutcomeFightEvent ||
                    name == kPreCombatOutcomeCaptiveEvent ||
                    name == kPreCombatOutcomeJoinEnemyEvent ||
                    name == kPreCombatOutcomeRecruitEvent ||
                    name == kPreCombatOutcomeReleaseEvent ||
                    name == kPreCombatOutcomeFollowEvent ||
                    name == kPreCombatOutcomePleasureEvent) {
                    std::scoped_lock lk(gLock);
                    RE::Actor* pendingActor = actor ? actor : ResolveSinglePendingActorLocked();
                    if (!pendingActor) {
                        pendingActor = GetRecentActorLockedNoLock(kRecentActorSoftAgeSec);
                    }
                    const auto snapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
                    spdlog::info(
                        "[TFD][PreCombatGreet] outcome event={} arg={} sender={:08X} actor={:08X} pendingActor={:08X} pendingCount={} root={} sub={} primary={:08X}",
                        rawName ? rawName : "unknown",
                        ev->strArg.c_str(),
                        ev->sender ? ev->sender->GetFormID() : 0u,
                        actor ? actor->GetFormID() : 0u,
                        pendingActor ? pendingActor->GetFormID() : 0u,
                        static_cast<unsigned>(gPending.size()),
                        TFD::FlowController::Controller::ToString(snapshot.root),
                        TFD::FlowController::Controller::ToString(snapshot.sub),
                        snapshot.primaryActorFormID);
                    const auto actorFormID = ResolveFlowActorFormIDLocked(pendingActor ? pendingActor : actor);
                    if (actorFormID == 0) {
                        spdlog::warn(
                            "[TFD][PreCombatGreet] outcome event rejected event={} reason=no_actor",
                            rawName ? rawName : "unknown");
                        return RE::BSEventNotifyControl::kContinue;
                    }
                    if (snapshot.root == TFD::FlowController::RootFlow::PreCombat &&
                        snapshot.primaryActorFormID != 0 &&
                        snapshot.primaryActorFormID != actorFormID) {
                        spdlog::warn(
                            "[TFD][PreCombatGreet] outcome event rejected event={} actor={:08X} primary={:08X} reason=owner_mismatch",
                            rawName ? rawName : "unknown",
                            actorFormID,
                            snapshot.primaryActorFormID);
                        return RE::BSEventNotifyControl::kContinue;
                    }

                    Pending* matchedPending = ResolveTerminalPendingLocked(pendingActor);
                    if (matchedPending && matchedPending->dialogueRequested && !matchedPending->dialogSeen) {
                        spdlog::warn(
                            "[TFD][PreCombatGreet] outcome event rejected event={} actor={:08X} reason=dialogue_not_confirmed",
                            rawName ? rawName : "unknown",
                            actorFormID);
                        return RE::BSEventNotifyControl::kContinue;
                    }
                    if (matchedPending && ShouldSuppressTerminalEventLocked(*matchedPending, pendingActor, rawName)) {
                        return RE::BSEventNotifyControl::kContinue;
                    }
                    TFD::Extortion::HandlePreCombatOutcomeEvent(rawName, pendingActor ? pendingActor : actor);
                    bool shouldClearInteractionState = false;
                    if (name == kPreCombatOutcomePayEvent) {
                        bool startedExtortion = false;
                        if (matchedPending) {
                            MarkTerminalChoiceCommittedLocked(*matchedPending, rawName);
                        }
                        const bool resolved = ResolvePreCombatTerminalOutcomeLocked(TFD::FlowController::PreCombatOutcome::Pay, actorFormID, "mod_event_precombat_pay");
                        if (resolved) {
                            if (matchedPending && pendingActor) {
                                ArmPreCombatPayFollowupLocked(pendingActor, *matchedPending, rawName);
                            }
                            if (pendingActor) {
                                startedExtortion = TFD::Extortion::BeginPreCombat(pendingActor, "mod_event_precombat_pay");
                                if (startedExtortion) {
                                    CacheRecentActor(pendingActor, 0.0, "mod_event_precombat_pay");
                                }
                            }
                            spdlog::info("[TFD][PreCombatGreet] precombat pay accepted actor={:08X} -> extortion {}", actorFormID, startedExtortion ? "handoff" : "already_active");
                            shouldClearInteractionState = true;
                        }
                        else {
                            spdlog::warn("[TFD][PreCombatGreet] pay outcome rejected actor={:08X} reason=flow_reject", actorFormID);
                        }
                    }
                    else if (name == kPreCombatOutcomeFightEvent) {
                        if (matchedPending) {
                            MarkTerminalChoiceCommittedLocked(*matchedPending, rawName);
                        }
                        if (ResolvePreCombatTerminalOutcomeLocked(TFD::FlowController::PreCombatOutcome::Fight, actorFormID, "mod_event_precombat_fight")) {
                            shouldClearInteractionState = true;
                        }
                        else {
                            spdlog::warn("[TFD][PreCombatGreet] fight outcome rejected actor={:08X} reason=flow_reject", actorFormID);
                        }
                    }
                    else if (name == kPreCombatOutcomeCaptiveEvent) {
                        if (matchedPending) {
                            MarkTerminalChoiceCommittedLocked(*matchedPending, rawName);
                        }
                        if (ResolvePreCombatTerminalOutcomeLocked(TFD::FlowController::PreCombatOutcome::Captive, actorFormID, "mod_event_precombat_captive")) {
                            QueuePreCombatCaptiveTransition(actorFormID, "precombat_captive");
                            shouldClearInteractionState = true;
                        }
                        else {
                            spdlog::warn("[TFD][PreCombatGreet] captive outcome rejected actor={:08X} reason=flow_reject", actorFormID);
                        }
                    }
                    else if (name == kPreCombatOutcomeJoinEnemyEvent) {
                        if (matchedPending) {
                            MarkTerminalChoiceCommittedLocked(*matchedPending, rawName);
                        }
                        if (ResolvePreCombatTerminalOutcomeLocked(TFD::FlowController::PreCombatOutcome::JoinEnemy, actorFormID, "mod_event_precombat_join_enemy")) {
                            shouldClearInteractionState = true;
                        }
                        else {
                            spdlog::warn("[TFD][PreCombatGreet] join enemy outcome rejected actor={:08X} reason=flow_reject", actorFormID);
                        }
                    }
                    else if (name == kPreCombatOutcomeReleaseEvent) {
                        if (matchedPending) {
                            MarkTerminalChoiceCommittedLocked(*matchedPending, rawName);
                        }
                        if (ResolvePreCombatTerminalOutcomeLocked(TFD::FlowController::PreCombatOutcome::Release, actorFormID, "mod_event_precombat_release")) {
                            shouldClearInteractionState = true;
                        }
                        else {
                            spdlog::warn("[TFD][PreCombatGreet] release outcome rejected actor={:08X} reason=flow_reject", actorFormID);
                        }
                    }
                    else if (name == kPreCombatOutcomeFollowEvent) {
                        if (matchedPending) {
                            MarkTerminalChoiceCommittedLocked(*matchedPending, rawName);
                        }
                        if (ResolvePreCombatTerminalOutcomeLocked(TFD::FlowController::PreCombatOutcome::Follow, actorFormID, "mod_event_precombat_follow")) {
                            if (pendingActor) {
                                CacheRecentActor(pendingActor, 0.0, rawName);
                                const double followDurationSec = ev->numArg > 0.0f ? static_cast<double>(ev->numArg) : 60.0;
                                TFD::Actor::Ops::ApplyReleaseFollowGraceToSpeakerAndCrowd(pendingActor, followDurationSec, "precombat_follow");
                                spdlog::info("[TFD][PreCombatGreet] follow choice committed action=TrucePreCombat actor={:08X} reason={} duration={:.2f}",
                                    actorFormID,
                                    rawName ? rawName : "TFDPreCombatOutcomeFollow",
                                    followDurationSec);
                            }
                            shouldClearInteractionState = true;
                        }
                        else {
                            spdlog::warn("[TFD][PreCombatGreet] follow outcome rejected actor={:08X} reason=flow_reject", actorFormID);
                        }
                    }
                    else if (name == kPreCombatOutcomePleasureEvent) {
                        if (matchedPending) {
                            MarkPleasureChoiceCommittedLocked(*matchedPending, pendingActor, rawName);
                        }
                        if (!ResolvePreCombatTerminalOutcomeLocked(TFD::FlowController::PreCombatOutcome::Pleasure, actorFormID, "mod_event_precombat_pleasure")) {
                            spdlog::warn("[TFD][PreCombatGreet] pleasure outcome rejected actor={:08X} reason=flow_reject", actorFormID);
                        }
                    }
                    else {
                        if (matchedPending) {
                            MarkTerminalChoiceCommittedLocked(*matchedPending, rawName);
                        }
                        if (ResolvePreCombatTerminalOutcomeLocked(TFD::FlowController::PreCombatOutcome::RecruitEnemy, actorFormID, "mod_event_precombat_recruit")) {
                            shouldClearInteractionState = true;
                        }
                        else {
                            spdlog::warn("[TFD][PreCombatGreet] recruit outcome rejected actor={:08X} reason=flow_reject", actorFormID);
                        }
                    }

                    if (shouldClearInteractionState) {
                        TFD::InteractionRouter::ClearInteractionStateValue();
                    }
                    return RE::BSEventNotifyControl::kContinue;
                }

                return RE::BSEventNotifyControl::kContinue;
            }
        };

        PleasureEventSink gPleasureEventSink{};

        void TickUI()
        {
            struct Guard
            {
                ~Guard()
                {
                    gTickPending.clear(std::memory_order_release);
                }
            } guard;

            if (gSuspended.load(std::memory_order_acquire)) {
                std::scoped_lock lk(gLock);
                ClearAllPendingLocked();
                return;
            }

            if (auto* player = RE::PlayerCharacter::GetSingleton(); player && player->IsInCombat()) {
                std::scoped_lock lk(gLock);
                if (gRecentActorHandle != 0 && !HasStickyPendingLocked() && !TFD::Extortion::HasActive()) {
                    ClearRecentActor("player_entered_combat");
                }
            }

            if (TFD::Transition::IsRecoveryActive()) {
                std::scoped_lock lk(gLock);
                ClearAllPendingLocked();
                return;
            }

            if (IsPlayerDown()) {
                std::scoped_lock lk(gLock);
                ClearAllPendingLocked();
                return;
            }

            const double now = NowSec();

            if (TFD::FlowController::IsPreCombatBlocked()) {
                std::scoped_lock lk(gLock);
                const bool hasPending = !gPending.empty();
                const auto ctxKind = TFD::FlowController::GetDialogueContextKind();
                const auto holdKind = TFD::FlowController::GetPassiveHoldKind();
                const bool preserveAfterPleasureHandoff =
                    ctxKind == TFD::FlowController::DialogueContextKind::AfterPleasure &&
                    holdKind == TFD::FlowController::PassiveHoldKind::Pleasure;
                const bool preserveProtectedHandoff =
                    preserveAfterPleasureHandoff ||
                    HasCommittedTerminalPendingLocked() ||
                    (TFD::FlowController::IsPassiveHoldProtectedHandoff() &&
                        HasProtectedPleasurePendingLocked());

                if (!preserveProtectedHandoff) {
                    if (hasPending) {
                        spdlog::info("[TFD][PreCombatGreet] blocked ctx={} hold={} -> clear pending",
                            TFD::FlowController::GetDialogueContextName(),
                            TFD::FlowController::GetPassiveHoldName());
                    }
                    ClearAllPendingLocked();
                    return;
                }

                if (hasPending) {
                    bool shouldLog = false;
                    for (auto& [handle, pending] : gPending) {
                        (void)handle;
                        if (now >= pending.nextPreserveHandoffLogSec) {
                            pending.nextPreserveHandoffLogSec = now + 1.0;
                            shouldLog = true;
                            break;
                        }
                    }
                    if (shouldLog) {
                        spdlog::info("[TFD][PreCombatGreet] blocked ctx={} hold={} but preserve handoff",
                            TFD::FlowController::GetDialogueContextName(),
                            TFD::FlowController::GetPassiveHoldName());
                    }
                }
            }

            const bool dialogueOpen = IsDialogueOpen();

            TFD::HostilityController::Update(now);

            std::scoped_lock lk(gLock);

            for (auto it = gPending.begin(); it != gPending.end();) {
                auto sp = RE::Actor::LookupByHandle(it->first);
                auto* actor = sp.get();
                auto& pending = it->second;

                if (!IsActorStillValid(actor)) {
                    if (pending.truceSessionId != 0) {
                        TFD::HostilityController::ReleaseSession(pending.truceSessionId, TFD::Tame::ReleaseReason::Generic);
                    }
                    it = gPending.erase(it);
                    continue;
                }

                if (!TFD::HostilityController::IsSuppressed(actor)) {
                    CleanupOne(actor, pending, kCooldownAfterFailSec, "truce_lost", TFD::Tame::ReleaseReason::Generic);
                    it = gPending.erase(it);
                    continue;
                }

                if (pending.dialogueRequested) {
                    auto extortionTick = TFD::Extortion::TickPreCombat(actor, now, dialogueOpen, IsDialogueOpenActiveForPreCombatLocked());
                    if (extortionTick == TFD::Extortion::TickResult::Consumed) {
                        ++it;
                        continue;
                    }
                    if (extortionTick == TFD::Extortion::TickResult::AllowAbort) {
                        CacheRecentActor(actor, 0.0, "extortion_abort");
                        CleanupOne(actor, pending, kCooldownAfterDoneSec, "extortion_abort", TFD::Tame::ReleaseReason::DialogueClosed);
                        it = gPending.erase(it);
                        continue;
                    }

                    const bool nativeDialogueOpenPending = IsDialogueOpenActiveForPreCombatLocked();

                    if (pending.terminalChoiceCommitted && nativeDialogueOpenPending) {
                        CancelPreCombatDialogueOpenLocked(actor, pending, "terminal_choice_committed");
                        ++it;
                        continue;
                    }

                    if (dialogueOpen) {
                        pending.dialogSeen = true;
                        pending.stickyReopenPending = false;
                        pending.nextStickyRetrySec = 0.0;
                        pending.stickySuppressTerminalUntilSec = 0.0;
                        ResetDialogueOpenRetryLocked(pending);
                        ClearDialogueClosePendingLocked(pending);
                        ++it;
                        continue;
                    }

                    if (nativeDialogueOpenPending) {
                        EnforceNegotiationState(actor, pending, now);
                        ++it;
                        continue;
                    }

                    if (pending.stickyReopenPending && !pending.terminalChoiceCommitted && now >= pending.nextStickyRetrySec) {
                        BeginStickyReopenLocked(actor, pending, "sticky_watchdog");
                        ++it;
                        continue;
                    }

                    if (!pending.dialogSeen) {
                        if (TryRetryDialogueOpenLocked(actor, pending, now, "dialogue_open_timeout")) {
                            ++it;
                            continue;
                        }

                        CacheRecentActor(actor, 0.0, "dialogue_open_failed");
                        CleanupOne(actor, pending, kCooldownAfterFailSec, "dialogue_open_failed", TFD::Tame::ReleaseReason::DialogueClosed);
                        it = gPending.erase(it);
                        continue;
                    }

                    if (pending.dialogSeen) {
                        ArmDialogueClosePendingLocked(actor, pending, now, "dialogue_closed");
                        EnforceNegotiationState(actor, pending, now);

                        if (now < pending.dialogueCloseResolveAtSec) {
                            ++it;
                            continue;
                        }

                        ClearDialogueClosePendingLocked(pending);

                        if (ShouldHoldForOutcomeAfterDialogueCloseLocked(actor, pending, now)) {
                            ++it;
                            continue;
                        }

                        if (TFD::PleasureRuntime::ShouldProtectPendingDialogue(actor)) {
                            CacheRecentActor(actor, 0.0, "pleasure_runtime_handoff");
                            ++it;
                            continue;
                        }

                        if (ShouldStickyReopenLocked(pending)) {
                            BeginStickyReopenLocked(actor, pending, "dialogue_closed_no_choice");
                            ++it;
                            continue;
                        }

                        auto releaseReason = ResolveDialogueClosedReleaseReasonLocked(pending);
                        const bool isHandoff = releaseReason == TFD::Tame::ReleaseReason::FlowHandoff;
                        const bool isFightChoice = releaseReason == TFD::Tame::ReleaseReason::FightChoice;
                        const char* cleanupReason =
                            isHandoff ? "dialogue_handoff" :
                            (isFightChoice ? "fight_choice_rehostile" : "dialogue_closed_abort");
                        CacheRecentActor(actor, 0.0, cleanupReason);
                        CleanupOne(actor, pending, kCooldownAfterDoneSec, cleanupReason, releaseReason);
                        it = gPending.erase(it);
                        continue;
                    }
                }
                else {
                    if (actor->IsInCombat()) {
                        CleanupOne(actor, pending, kCooldownAfterFailSec, "tame_broken", TFD::Tame::ReleaseReason::TameBroken);
                        it = gPending.erase(it);
                        continue;
                    }
                }

                if (now >= pending.expiresSec) {
                    CleanupOne(actor, pending, kCooldownAfterFailSec, "hidden_failsafe_expired", TFD::Tame::ReleaseReason::HardFailsafeExpired);
                    it = gPending.erase(it);
                    continue;
                }

                ++it;
            }
        }

        void WorkerLoop()
        {
            while (gRunning.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(60));

                if (!gInstalled.load(std::memory_order_acquire)) {
                    continue;
                }

                if (gTickPending.test_and_set(std::memory_order_acq_rel)) {
                    continue;
                }

                auto* tasks = SKSE::GetTaskInterface();
                if (!tasks) {
                    gTickPending.clear(std::memory_order_release);
                    continue;
                }

                tasks->AddUITask([]() { TickUI(); });
            }
        }
    }

    void Install()
    {
        if (gInstalled.exchange(true)) {
            return;
        }

        if (auto* scripts = RE::ScriptEventSourceHolder::GetSingleton()) {
            scripts->AddEventSink(&gHitSink);
        }
        if (auto* src = SKSE::GetModCallbackEventSource()) {
            src->AddEventSink(&gPleasureEventSink);
        }

        TFD::Extortion::Install();

        gSuspended.store(false, std::memory_order_release);
        gRunning.store(true, std::memory_order_release);
        gWorker = std::thread(WorkerLoop);

        spdlog::info("[TFD][PreCombatGreet] Install");
    }

    void Shutdown()
    {
        if (!gInstalled.exchange(false)) {
            return;
        }

        if (auto* scripts = RE::ScriptEventSourceHolder::GetSingleton()) {
            scripts->RemoveEventSink(&gHitSink);
        }
        if (auto* src = SKSE::GetModCallbackEventSource()) {
            src->RemoveEventSink(&gPleasureEventSink);
        }

        gRunning.store(false, std::memory_order_release);

        if (gWorker.joinable()) {
            gWorker.join();
        }

        {
            std::scoped_lock lk(gLock);
            ClearAllPendingLocked();
            gCooldownUntil.clear();
            ClearRecentActor("shutdown");
            ClearPostHandoffBlockLocked("shutdown");
            ClearHotkeyCooldownLocked("shutdown");
        }

        TFD::Extortion::Shutdown();
        gSuspended.store(false, std::memory_order_release);

        spdlog::info("[TFD][PreCombatGreet] Shutdown");
    }

    void SetSuspended(bool suspended)
    {
        gSuspended.store(suspended, std::memory_order_release);

        if (suspended) {
            std::scoped_lock lk(gLock);
            ClearAllPendingLocked();
            ClearRecentActor("suspend");
            ClearPostHandoffBlockLocked("suspend");
            ClearHotkeyCooldownLocked("suspend");
        }
    }

    bool IsSuspended()
    {
        return gSuspended.load(std::memory_order_acquire);
    }

    bool BeginForActor(RE::Actor* actor, TFD::InteractionRouter::Action* outAction)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();

        if (outAction) {
            *outAction = TFD::InteractionRouter::Action::None;
        }

        if (TFD::Transition::IsRecoveryActive()) {
            return false;
        }

        if (!IsCandidate(actor, player)) {
            return false;
        }

        if (TFD::Extortion::IsActive(actor)) {
            spdlog::info("[TFD][PreCombatGreet] BeginForActor blocked actor={:08X} reason=extortion_active", actor ? actor->GetFormID() : 0u);
            return false;
        }

        const auto handle = GetHandleId(actor);
        const double now = NowSec();

        std::scoped_lock lk(gLock);

        double hotkeyCooldownRemainingSec = 0.0;
        if (IsHotkeyCooldownActiveLocked(now, &hotkeyCooldownRemainingSec)) {
            spdlog::info(
                "[TFD][PreCombatGreet] BeginForActor blocked actor={:08X} reason=hotkey_cooldown sourceActor={:08X} remaining={:.2f}s",
                actor ? actor->GetFormID() : 0u,
                gHotkeyCooldownActorFormID,
                hotkeyCooldownRemainingSec);
            return false;
        }

        double postHandoffRemainingSec = 0.0;
        if (IsPostHandoffBlockActiveLocked(now, &postHandoffRemainingSec)) {
            spdlog::info(
                "[TFD][PreCombatGreet] BeginForActor blocked actor={:08X} reason=post_handoff_settle sourceActor={:08X} remaining={:.2f}s",
                actor ? actor->GetFormID() : 0u,
                gPostHandoffBlockActorFormID,
                postHandoffRemainingSec);
            return false;
        }

        auto cooldownIt = gCooldownUntil.find(handle);
        if (cooldownIt != gCooldownUntil.end() && now < cooldownIt->second) {
            return false;
        }

        auto existingIt = gPending.find(handle);
        if (existingIt != gPending.end()) {
            if (outAction) {
                *outAction = existingIt->second.action;
            }
            return true;
        }

        if (!gPending.empty()) {
            spdlog::info(
                "[TFD][PreCombatGreet] BeginForActor blocked actor={:08X} reason=precombat_owner_busy pendingOwner={:08X} pendingCount={}",
                actor ? actor->GetFormID() : 0u,
                ResolveSinglePendingActorFormIDLocked(),
                static_cast<unsigned>(gPending.size()));
            return false;
        }

        const bool isCaptivePhase = false;
        const auto result = TFD::InteractionRouter::HandleHotkeyPress(
            player,
            actor,
            isCaptivePhase,
            now);

        spdlog::info(
            "[TFD][PreCombatGreet] BeginForActor actor={:08X} action={} executed={} dialogueRequested={} fail={}",
            actor->GetFormID(),
            TFD::InteractionRouter::ToString(result.action),
            result.executed ? 1 : 0,
            result.dialogueRequested ? 1 : 0,
            TFD::InteractionRouter::ToString(result.failReason));

        if (outAction) {
            *outAction = result.action;
        }

        if (!result.executed || result.sessionId == 0) {
            return false;
        }

        if (result.action != TFD::InteractionRouter::Action::TrucePreCombat) {
            TFD::HostilityController::ReleaseSession(result.sessionId, TFD::Tame::ReleaseReason::Generic);
            spdlog::warn(
                "[TFD][PreCombatGreet] rejected non-precombat action actor={:08X} action={}",
                actor->GetFormID(),
                TFD::InteractionRouter::ToString(result.action));
            return false;
        }

        Pending pending{};
        pending.truceSessionId = result.sessionId;
        pending.action = result.action;
        pending.expiresSec = now + kManualWindowSec;
        pending.dialogueRequested = result.dialogueRequested;
        pending.dialogSeen = false;
        pending.assignSent = false;
        pending.stickyReopenPending = false;
        pending.terminalChoiceCommitted = false;
        pending.nextStickyRetrySec = 0.0;
        pending.stickySuppressTerminalUntilSec = 0.0;
        pending.dialogueClosePending = false;
        pending.dialogueCloseResolveAtSec = 0.0;
        pending.nextNegotiationRefreshSec = 0.0;
        pending.postCloseOutcomeGraceUntilSec = 0.0;
        pending.nextPostCloseOutcomeLogSec = 0.0;
        pending.dialogueOpenRetryCount = 0;
        pending.nextDialogueOpenRetrySec = 0.0;

        if (result.dialogueRequested) {
            if (!TFD::HostilityController::CanOpenDialogue(actor)) {
                TFD::HostilityController::ReleaseSession(result.sessionId, TFD::Tame::ReleaseReason::Generic);
                return false;
            }
        }

        auto& flow = TFD::FlowController::Controller::GetSingleton();
        const bool flowAccepted = flow.BeginPreCombat(actor->GetFormID(), "precombat_begin");
        const bool gateAccepted = flowAccepted && (!result.dialogueRequested ||
            flow.BeginTruceDecision(actor->GetFormID(), "precombat_dialogue_begin"));

        if (!flowAccepted || !gateAccepted) {
            TFD::HostilityController::ReleaseSession(result.sessionId, TFD::Tame::ReleaseReason::Generic);
            spdlog::warn(
                "[TFD][PreCombatGreet] BeginForActor rejected by flow actor={:08X} flowAccepted={} gateAccepted={}",
                actor->GetFormID(),
                flowAccepted ? 1 : 0,
                gateAccepted ? 1 : 0);
            return false;
        }

        if (result.dialogueRequested) {
            (void)TFD::PayModel::PrimeEncounterQuote(actor, TFD::PayModel::PayContext::PreCombat);
            const bool payPublished = TFD::PayModel::PublishSharedGold(actor, TFD::PayModel::PayContext::PreCombat, "precombat_dialogue_begin");
            spdlog::info(
                "[TFD][PreCombatGreet] precombat pay prepared actor={:08X} published={} gold={}",
                actor->GetFormID(),
                payPublished ? 1 : 0,
                TFD::PayModel::GetCachedEncounterQuote(actor, TFD::PayModel::PayContext::PreCombat));
            CacheRecentActor(actor, 0.0, "begin");
        }

        gPending.emplace(handle, pending);

        if (result.dialogueRequested) {
            TFD::InteractionRouter::DialogueOpen::BeginPreCombatTruce(actor);
        }

        return true;
    }

    bool HandleGraceModEvent(const GraceEventContext& context, const GraceEventHandlers& handlers)
    {
        auto* actor = context.actor;
        if (!actor) {
            return false;
        }

        const std::string_view eventName = context.eventName ? std::string_view(context.eventName) : std::string_view{};
        if (!IsGraceEventName(eventName)) {
            return false;
        }
        if (eventName == std::string_view("TFDPreCombatOutcomeReleaseEnd")) {
            if (handlers.removeGrace) {
                handlers.removeGrace(actor, "precombat_release_end");
            }

            spdlog::info(
                "[TFD][PreCombatGreet] grace handled actor={:08X} reason=precombat_release_end",
                actor->GetFormID());
            return true;
        }

        if (eventName == std::string_view("TFDPreCombatOutcomeFollowEnd")) {
            if (handlers.removeGrace) {
                handlers.removeGrace(actor, "precombat_follow_end");
            }

            spdlog::info(
                "[TFD][PreCombatGreet] grace handled actor={:08X} reason=precombat_follow_end",
                actor->GetFormID());
            return true;
        }

        const char* graceReason =
            eventName == std::string_view("TFDPreCombatOutcomeFollow") ?
            "precombat_follow" :
            "precombat_release";

        const double durationSec = context.durationSec > 0.0 ? context.durationSec : 20.0;
        if (handlers.applyGrace) {
            handlers.applyGrace(actor, durationSec, graceReason);
        }

        spdlog::info(
            "[TFD][PreCombatGreet] grace handled actor={:08X} reason={} duration={}",
            actor->GetFormID(),
            graceReason,
            durationSec);
        return true;
    }

    bool HandleGraceModEventRaw(const char* eventName, const char* eventArg, double durationSec, const GraceEventHandlers& handlers)
    {
        GraceEventContext context{};
        context.eventName = eventName;
        context.actor = ResolveActorFromEventArgRaw(eventArg);
        context.durationSec = durationSec > 0.0 ? durationSec : 20.0;
        return HandleGraceModEvent(context, handlers);
    }

    bool HandleModEventRaw(const char* eventName, const char* eventArg, double durationSec, const GraceEventHandlers& handlers)
    {
        const std::string_view name = eventName ? std::string_view(eventName) : std::string_view{};
        if (!IsGraceEventName(name)) {
            return false;
        }
        return HandleGraceModEventRaw(eventName, eventArg, durationSec, handlers);
    }

    bool HandleModCallbackEvent(const SKSE::ModCallbackEvent* ev, const GraceEventHandlers& handlers)
    {
        if (!ev) {
            return false;
        }

        const char* eventName = ev->eventName.c_str();
        const char* eventArg = ev->strArg.c_str();
        const double durationSec = ev->numArg > 0.0f ? static_cast<double>(ev->numArg) : 20.0;
        return HandleModEventRaw(eventName, eventArg, durationSec, handlers);
    }

    RE::Actor* ResolveRecentAggressor(float radius, double maxAgeSec)
    {
        RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return nullptr;
        }

        RE::Actor* actor = GetRecentActor(maxAgeSec);
        if (!actor || actor == player) {
            return nullptr;
        }
        if (actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
            return nullptr;
        }
        if (actor->IsPlayerTeammate() || TFD::Tame::IsCompanion(actor)) {
            return nullptr;
        }

        RE::TESObjectCELL* playerCell = player->GetParentCell();
        RE::TESObjectCELL* actorCell = actor->GetParentCell();
        if (playerCell && actorCell != playerCell) {
            return nullptr;
        }

        RE::TESWorldSpace* playerWs = player->GetWorldspace();
        RE::TESWorldSpace* actorWs = actor->GetWorldspace();
        if (playerWs && actorWs != playerWs) {
            return nullptr;
        }

        const auto pp = player->GetPosition();
        const auto ap = actor->GetPosition();
        const float dx = ap.x - pp.x;
        const float dy = ap.y - pp.y;
        const float dz = ap.z - pp.z;
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (radius > 0.0f && dist > radius) {
            return nullptr;
        }

        if (!actor->IsHostileToActor(player) && !actor->IsInCombat()) {
            return nullptr;
        }

        spdlog::info(
            "[TFD][PreCombatGreet] using recent aggressor actor={:08X} dist={:.1f}",
            static_cast<std::uint32_t>(actor->GetFormID()),
            static_cast<double>(dist));
        return actor;
    }

    RE::Actor* ResolveRecentAggressorAndCache(float radius, RE::ActorHandle& cacheHandle, double maxAgeSec)
    {
        auto* actor = ResolveRecentAggressor(radius, maxAgeSec);
        if (actor) {
            cacheHandle = actor->GetHandle();
        }
        return actor;
    }

    RE::Actor* GetRecentActorLockedNoLock(double maxAgeSec)
    {
        if (gRecentActorHandle == 0) {
            return nullptr;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player && player->IsInCombat()) {
            if (!HasStickyPendingLocked() && !TFD::Extortion::HasActive()) {
                ClearRecentActor("player_entered_combat");
                return nullptr;
            }
        }

        const double now = NowSec();
        if (maxAgeSec > 0.0 && (now - gRecentActorCachedAtSec) > maxAgeSec) {
            if (gRecentActorLastSoftAgeLogSec <= 0.0) {
                spdlog::info("[TFD][PreCombatGreet] recent actor age exceeds soft limit but retained actorHandle={:08X} age={:.1f}s limit={:.1f}s",
                    gRecentActorHandle,
                    now - gRecentActorCachedAtSec,
                    maxAgeSec);
                gRecentActorLastSoftAgeLogSec = now;
            }
        }

        if (!IsPlayerStillInCachedSpace()) {
            ClearRecentActor("space_changed");
            return nullptr;
        }

        auto sp = RE::Actor::LookupByHandle(gRecentActorHandle);
        auto* actor = sp.get();
        if (!actor || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
            ClearRecentActor("invalid_actor");
            return nullptr;
        }

        return actor;
    }

    RE::Actor* GetRecentActor(double maxAgeSec)
    {
        std::scoped_lock lk(gLock);
        return GetRecentActorLockedNoLock(maxAgeSec);
    }

    void OnPreLoadGame()
    {
        TFD::Extortion::OnPreLoadGame();
        SetSuspended(true);
        CancelAll();
        gTickPending.clear(std::memory_order_release);

        {
            std::scoped_lock lk(gLock);
            ClearRecentActor("pre_load");
        }

        spdlog::info("[TFD][PreCombatGreet] OnPreLoadGame -> suspended + cleared");
    }

    void OnPostLoadGame()
    {
        TFD::Extortion::OnPostLoadGame();
        CancelAll();
        SetSuspended(false);
        gTickPending.clear(std::memory_order_release);

        {
            std::scoped_lock lk(gLock);
            ClearRecentActor("post_load");
        }

        spdlog::info("[TFD][PreCombatGreet] OnPostLoadGame -> resumed clean");
    }

    void OnLoadingScreenClosed()
    {
        std::scoped_lock lk(gLock);
        ClearRecentActor("loading_screen_closed");
    }

    void OnCaptiveHandoffArrived()
    {
        std::scoped_lock lk(gLock);
        ClearRecentActor("captive_handoff_complete");
    }

    void CancelAll()
    {
        std::scoped_lock lk(gLock);
        ClearAllPendingLocked();
        TFD::Extortion::CancelAll("precombat_cancel_all");
        TFD::PayModel::ClearSharedGold("precombat_cancel_all");
        gCooldownUntil.clear();
        ClearRecentActor("cancel_all");

        spdlog::info("[TFD][PreCombatGreet] CancelAll");
    }
}
