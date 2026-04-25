#include "TFDExtortion.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <string_view>

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

#include "TFDInteractionRouter.h"
#include "TFDHostilityController.h"

namespace TFD::Extortion
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr double kPreCombatWindowSec = 30.0;
        constexpr double kRetryDelaySec = 0.75;
        constexpr double kCanonicalReopenDelaySec = 1.35;
        constexpr double kSingleSpeakerCanonicalReopenDelaySec = 3.50;
        constexpr double kFollowupStableSec = 0.40;
        constexpr double kPayFollowupTerminalWaitSec = 14.00;
        constexpr double kPayFollowupPassiveRefreshSec = 0.35;
        constexpr double kPayFollowupHoldLogSec = 1.50;
        constexpr double kDuplicatePayProtectSec = 1.20;
        constexpr double kTerminalBarrierSec = 3.00;
        constexpr std::uint32_t kInitialCloseStableTicksNeeded = 1;
        constexpr std::uint32_t kMaxRetries = 1;

        struct PreCombatState
        {
            bool active{ false };
            bool waitForInitialClose{ false };
            bool menuSeen{ false };
            bool terminalCommitted{ false };
            bool initialCloseLatched{ false };
            std::uint32_t initialClosedStableTicks{ 0 };
            std::uint32_t retryCount{ 0 };
            std::uint32_t truceMemberCount{ 0 };
            bool singleSpeakerSession{ false };
            double nextRetrySec{ 0.0 };
            double closeGraceUntilSec{ 0.0 };
            double followupSeenAtSec{ 0.0 };
            double followupClosedAtSec{ 0.0 };
            double terminalOutcomeWaitUntilSec{ 0.0 };
            double nextPassiveRefreshSec{ 0.0 };
            double nextOutcomeWaitLogSec{ 0.0 };
            double duplicatePayProtectUntilSec{ 0.0 };
            double expiresSec{ 0.0 };
        };

        std::mutex gLock;
        std::unordered_map<std::uint32_t, PreCombatState> gPreCombat;
        std::unordered_map<std::uint32_t, double> gTerminalBarrierUntil;
        Clock::time_point gT0 = Clock::now();

        double NowSec()
        {
            return std::chrono::duration<double>(Clock::now() - gT0).count();
        }

        std::uint32_t GetHandleId(RE::Actor* actor)
        {
            return actor ? actor->GetHandle().native_handle() : 0;
        }

        void EraseLocked(RE::Actor* actor, const char* reason)
        {
            if (!actor) {
                return;
            }

            const auto handle = GetHandleId(actor);
            if (handle == 0) {
                return;
            }

            auto it = gPreCombat.find(handle);
            if (it == gPreCombat.end()) {
                return;
            }

            spdlog::info(
                "[TFD][Extortion] cleared actor={:08X} reason={}",
                actor->GetFormID(),
                reason ? reason : "unknown");
            gPreCombat.erase(it);
        }

        bool IsTerminalBarrierActiveLocked(std::uint32_t handle, double nowSec, double* outRemainingSec = nullptr)
        {
            if (outRemainingSec) {
                *outRemainingSec = 0.0;
            }

            if (handle == 0) {
                return false;
            }

            auto it = gTerminalBarrierUntil.find(handle);
            if (it == gTerminalBarrierUntil.end()) {
                return false;
            }

            if (nowSec >= it->second) {
                gTerminalBarrierUntil.erase(it);
                return false;
            }

            if (outRemainingSec) {
                *outRemainingSec = it->second - nowSec;
            }
            return true;
        }

        void ArmTerminalBarrierLocked(RE::Actor* actor, const char* reason)
        {
            if (!actor) {
                return;
            }

            const auto handle = GetHandleId(actor);
            if (handle == 0) {
                return;
            }

            const auto nowSec = NowSec();
            const auto untilSec = nowSec + kTerminalBarrierSec;
            const auto [it, inserted] = gTerminalBarrierUntil.insert_or_assign(handle, untilSec);
            (void)it;
            spdlog::info(
                "[TFD][Extortion] terminal barrier armed actor={:08X} reason={} duration={:.2f}s state={}",
                actor->GetFormID(),
                reason ? reason : "unknown",
                kTerminalBarrierSec,
                inserted ? "new" : "refresh");
        }
    }

    void Install()
    {
        std::scoped_lock lk(gLock);
        gPreCombat.clear();
        gTerminalBarrierUntil.clear();
        spdlog::info("[TFD][Extortion] Install terminal_wait_v3 soft_followup_all active");
    }

    void Shutdown()
    {
        std::scoped_lock lk(gLock);
        gPreCombat.clear();
        gTerminalBarrierUntil.clear();
        spdlog::info("[TFD][Extortion] Shutdown");
    }

    void CancelAll(const char* reason)
    {
        std::scoped_lock lk(gLock);
        if (!gPreCombat.empty() || !gTerminalBarrierUntil.empty()) {
            spdlog::info(
                "[TFD][Extortion] CancelAll count={} barriers={} reason={}",
                gPreCombat.size(),
                gTerminalBarrierUntil.size(),
                reason ? reason : "unknown");
        }
        gPreCombat.clear();
        gTerminalBarrierUntil.clear();
    }

    void OnPreLoadGame()
    {
        CancelAll("pre_load");
    }

    void OnPostLoadGame()
    {
        CancelAll("post_load");
    }

    bool BeginPreCombat(RE::Actor* actor, const char* reason)
    {
        if (!actor) {
            return false;
        }

        if (TFD::InteractionRouter::DialogueOpen::IsTemporaryDialogueCooldownActive(actor)) {
            spdlog::info(
                "[TFD][Extortion] begin blocked actor={:08X} reason={} state=temporary_dialogue_cooldown",
                actor->GetFormID(),
                reason ? reason : "unknown");
            return false;
        }

        const auto truceMembers = TFD::HostilityController::CollectActiveTruceActors(actor);
        const auto truceMemberCount = static_cast<std::uint32_t>(truceMembers.size());
        const bool singleSpeakerSession = truceMemberCount <= 1;

        std::scoped_lock lk(gLock);
        const auto handle = GetHandleId(actor);
        const auto nowSec = NowSec();

        double barrierRemainingSec = 0.0;
        if (IsTerminalBarrierActiveLocked(handle, nowSec, &barrierRemainingSec)) {
            spdlog::info(
                "[TFD][Extortion] begin blocked actor={:08X} reason={} barrierRemaining={:.2f}s",
                actor->GetFormID(),
                reason ? reason : "unknown",
                barrierRemainingSec);
            return false;
        }

        auto it = gPreCombat.find(handle);
        if (it != gPreCombat.end() && it->second.active) {
            it->second.expiresSec = std::max(it->second.expiresSec, nowSec + kPreCombatWindowSec);
            it->second.closeGraceUntilSec = 0.0;
            it->second.terminalOutcomeWaitUntilSec = 0.0;
            it->second.nextPassiveRefreshSec = 0.0;
            it->second.nextOutcomeWaitLogSec = 0.0;
            it->second.duplicatePayProtectUntilSec = std::max(it->second.duplicatePayProtectUntilSec, nowSec + kDuplicatePayProtectSec);
            it->second.truceMemberCount = truceMemberCount;
            it->second.singleSpeakerSession = singleSpeakerSession;
            spdlog::info(
                "[TFD][Extortion] begin ignored actor={:08X} reason={} state=already_active protect={:.2f}s members={} single={}",
                actor->GetFormID(),
                reason ? reason : "unknown",
                kDuplicatePayProtectSec,
                truceMemberCount,
                singleSpeakerSession ? 1 : 0);
            return false;
        }

        auto& state = gPreCombat[handle];
        state.active = true;
        state.waitForInitialClose = true;
        state.menuSeen = false;
        state.terminalCommitted = false;
        state.initialCloseLatched = false;
        state.initialClosedStableTicks = 0;
        state.retryCount = 0;
        state.truceMemberCount = truceMemberCount;
        state.singleSpeakerSession = singleSpeakerSession;
        state.nextRetrySec = 0.0;
        state.closeGraceUntilSec = 0.0;
        state.followupSeenAtSec = 0.0;
        state.followupClosedAtSec = 0.0;
        state.terminalOutcomeWaitUntilSec = 0.0;
        state.nextPassiveRefreshSec = 0.0;
        state.nextOutcomeWaitLogSec = 0.0;
        state.duplicatePayProtectUntilSec = 0.0;
        state.expiresSec = nowSec + kPreCombatWindowSec;

        spdlog::info(
            "[TFD][Extortion] begin precombat actor={:08X} reason={} window={:.1f}s members={} single={}",
            actor->GetFormID(),
            reason ? reason : "unknown",
            kPreCombatWindowSec,
            truceMemberCount,
            singleSpeakerSession ? 1 : 0);
        return true;
    }

    void HandlePreCombatOutcomeEvent(const char* eventName, RE::Actor* actor)
    {
        if (!actor || !eventName || !*eventName) {
            return;
        }

        const std::string_view name{ eventName };
        if (name == std::string_view("TFDPreCombatOutcomePay")) {
            return;
        }

        bool cancelDialogue = true;
        {
            std::scoped_lock lk(gLock);
            ArmTerminalBarrierLocked(actor, eventName);
            auto it = gPreCombat.find(GetHandleId(actor));
            if (it != gPreCombat.end()) {
                it->second.terminalCommitted = true;
                spdlog::info(
                    "[TFD][Extortion] terminal outcome actor={:08X} event={}",
                    actor->GetFormID(),
                    eventName);
                gPreCombat.erase(it);
            }
        }

        if (cancelDialogue) {
            TFD::InteractionRouter::DialogueOpen::Cancel();
        }
    }

    void HandlePreCombatTerminalPendingEvent(RE::Actor* actor, const char* reason)
    {
        if (!actor) {
            return;
        }

        bool cancelDialogue = true;
        {
            std::scoped_lock lk(gLock);
            ArmTerminalBarrierLocked(actor, reason);
            auto it = gPreCombat.find(GetHandleId(actor));
            if (it != gPreCombat.end()) {
                it->second.terminalCommitted = true;
                spdlog::info(
                    "[TFD][Extortion] terminal pending actor={:08X} reason={}",
                    actor->GetFormID(),
                    reason ? reason : "unknown");
                gPreCombat.erase(it);
            }
        }

        if (cancelDialogue) {
            TFD::InteractionRouter::DialogueOpen::Cancel();
        }
    }

    bool HasActive()
    {
        std::scoped_lock lk(gLock);
        return !gPreCombat.empty();
    }

    bool IsActive(RE::Actor* actor)
    {
        if (!actor) {
            return false;
        }

        std::scoped_lock lk(gLock);
        return gPreCombat.contains(GetHandleId(actor));
    }

    TickResult TickPreCombat(
        RE::Actor* actor,
        double nowSec,
        bool dialogueOpen,
        bool dialogueOpenActiveForPreCombat)
    {
        if (!actor) {
            return TickResult::NotActive;
        }

        std::scoped_lock lk(gLock);
        auto it = gPreCombat.find(GetHandleId(actor));
        if (it == gPreCombat.end()) {
            return TickResult::NotActive;
        }

        auto& state = it->second;

        if (TFD::InteractionRouter::DialogueOpen::IsTemporaryDialogueCooldownActive(actor)) {
            spdlog::info(
                "[TFD][Extortion] tick stopped actor={:08X} reason=temporary_dialogue_cooldown menuSeen={} retries={}",
                actor->GetFormID(),
                state.menuSeen ? 1 : 0,
                state.retryCount);
            gPreCombat.erase(it);
            return TickResult::NotActive;
        }

        double barrierRemainingSec = 0.0;
        if (IsTerminalBarrierActiveLocked(GetHandleId(actor), nowSec, &barrierRemainingSec)) {
            spdlog::info(
                "[TFD][Extortion] tick blocked actor={:08X} reason=terminal_barrier remaining={:.2f}s menuSeen={} retries={}",
                actor->GetFormID(),
                barrierRemainingSec,
                state.menuSeen ? 1 : 0,
                state.retryCount);
            gPreCombat.erase(it);
            return TickResult::NotActive;
        }

        if (nowSec >= state.expiresSec) {
            spdlog::info(
                "[TFD][Extortion] expired actor={:08X} menuSeen={} retries={} followupClosed={} closedFor={:.2f}s terminalWaitUntil={:.2f}",
                actor->GetFormID(),
                state.menuSeen ? 1 : 0,
                state.retryCount,
                state.followupClosedAtSec > 0.0 ? 1 : 0,
                state.followupClosedAtSec > 0.0 ? nowSec - state.followupClosedAtSec : 0.0,
                state.terminalOutcomeWaitUntilSec);
            gPreCombat.erase(it);
            return TickResult::AllowAbort;
        }

        if (state.waitForInitialClose) {
            if (dialogueOpen || dialogueOpenActiveForPreCombat) {
                state.initialCloseLatched = true;
                state.initialClosedStableTicks = 0;
                return TickResult::Consumed;
            }

            // Pay handoff stays immediate from the user's perspective, but we still
            // require the root dialogue to be stably closed for a couple of UI ticks
            // before issuing the followup reopen. This avoids reopening on the same
            // frame the root menu is still tearing down.
            if (!state.initialCloseLatched) {
                state.initialCloseLatched = true;
                state.initialClosedStableTicks = 1;
                return TickResult::Consumed;
            }

            if (state.initialClosedStableTicks < kInitialCloseStableTicksNeeded) {
                ++state.initialClosedStableTicks;
                return TickResult::Consumed;
            }

            state.waitForInitialClose = false;
            const double reopenDelaySec = state.singleSpeakerSession ? kSingleSpeakerCanonicalReopenDelaySec : kCanonicalReopenDelaySec;
            state.nextRetrySec = nowSec + reopenDelaySec;
            spdlog::info(
                "[TFD][Extortion] root close latched actor={:08X} stableTicks={} reopenDelay={:.2f}s members={} single={}",
                actor->GetFormID(),
                state.initialClosedStableTicks,
                reopenDelaySec,
                state.truceMemberCount,
                state.singleSpeakerSession ? 1 : 0);
        }

        if (dialogueOpen) {
            if (!state.menuSeen) {
                spdlog::info(
                    "[TFD][Extortion] followup menu seen actor={:08X}",
                    actor->GetFormID());
            }
            state.menuSeen = true;
            if (state.followupSeenAtSec <= 0.0) {
                state.followupSeenAtSec = nowSec;
            }
            state.followupClosedAtSec = 0.0;
            state.terminalOutcomeWaitUntilSec = 0.0;
            state.nextPassiveRefreshSec = 0.0;
            state.nextOutcomeWaitLogSec = 0.0;
            state.closeGraceUntilSec = 0.0;
            return TickResult::Consumed;
        }

        if (dialogueOpenActiveForPreCombat) {
            return TickResult::Consumed;
        }

        if (!state.menuSeen) {
            if (state.retryCount < kMaxRetries + 1 && nowSec >= state.nextRetrySec) {
                const bool canonical = (state.retryCount == 0);
                ++state.retryCount;
                state.nextRetrySec = nowSec + kRetryDelaySec;

                // Make the first followup reopen deterministic: hard-reset any stale
                // dialogue-open handshake before issuing the canonical reopen.
                if (canonical) {
                    TFD::InteractionRouter::DialogueOpen::Cancel();
                }

                // Pay followup must not reopen through the root PreCombatTruce
                // forcegreet path. Even when crowd members exist, the crowd is already
                // tracked through the truce bridge; reopening the root forcegreet can
                // fire the root greeting fragment again and keep the CK package alive
                // during the Recruit outcome window. Use the soft followup route for
                // every Pay followup retry.
                TFD::InteractionRouter::DialogueOpen::BeginPreCombatFollowup(actor);
                spdlog::info(
                    "[TFD][Extortion] soft followup all actor={:08X} retry={}/{} delay={:.2f}s members={} single={} canonical={}",
                    actor->GetFormID(),
                    state.retryCount,
                    kMaxRetries + 1,
                    kRetryDelaySec,
                    state.truceMemberCount,
                    state.singleSpeakerSession ? 1 : 0,
                    canonical ? 1 : 0);
                return TickResult::Consumed;
            }

            if (state.retryCount >= kMaxRetries + 1 && nowSec >= state.nextRetrySec) {
                spdlog::info(
                    "[TFD][Extortion] retries exhausted actor={:08X}",
                    actor->GetFormID());
                gPreCombat.erase(it);
                return TickResult::AllowAbort;
            }

            return TickResult::Consumed;
        }

        if (nowSec < state.duplicatePayProtectUntilSec) {
            return TickResult::Consumed;
        }

        if ((nowSec - state.followupSeenAtSec) < kFollowupStableSec) {
            return TickResult::Consumed;
        }

        if (state.followupClosedAtSec <= 0.0) {
            state.followupClosedAtSec = nowSec;
            state.terminalOutcomeWaitUntilSec = nowSec + kPayFollowupTerminalWaitSec;
            state.expiresSec = std::max(state.expiresSec, state.terminalOutcomeWaitUntilSec);
            state.nextPassiveRefreshSec = 0.0;
            state.nextOutcomeWaitLogSec = nowSec;
            spdlog::info(
                "[TFD][Extortion] terminal outcome wait armed actor={:08X} wait={:.2f}s expiresIn={:.2f}s members={} single={}",
                actor->GetFormID(),
                kPayFollowupTerminalWaitSec,
                state.expiresSec - nowSec,
                state.truceMemberCount,
                state.singleSpeakerSession ? 1 : 0);
            return TickResult::Consumed;
        }

        // Once Pay followup has actually opened, a closed DialogueMenu is no
        // longer a reliable abort signal. Papyrus TIF chains can submit
        // Recruit/Release/Join/Follow a few seconds after the menu vanishes,
        // especially after the player already owns a teammate. Keep the native
        // truce alive until a terminal pending/outcome event arrives or until
        // the explicit Pay followup window expires. This prevents
        // DialogueClosed -> rehostile from racing ahead of RecruitPending.
        if (nowSec >= state.nextPassiveRefreshSec) {
            actor->StopCombat();
            actor->StopAlarmOnActor();
            actor->EvaluatePackage(false, true);
            state.nextPassiveRefreshSec = nowSec + kPayFollowupPassiveRefreshSec;
        }

        if (nowSec >= state.nextOutcomeWaitLogSec) {
            state.nextOutcomeWaitLogSec = nowSec + kPayFollowupHoldLogSec;
            spdlog::info(
                "[TFD][Extortion] terminal outcome wait holding actor={:08X} closedFor={:.2f}s expiresIn={:.2f}s members={} single={}",
                actor->GetFormID(),
                nowSec - state.followupClosedAtSec,
                std::max(0.0, state.expiresSec - nowSec),
                state.truceMemberCount,
                state.singleSpeakerSession ? 1 : 0);
        }

        return TickResult::Consumed;
    }
}
