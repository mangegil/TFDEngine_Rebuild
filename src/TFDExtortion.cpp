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

namespace TFD::Extortion
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr double kPreCombatWindowSec = 20.0;
        constexpr double kRetryDelaySec = 0.75;
        constexpr double kCanonicalReopenDelaySec = 1.35;
        constexpr double kCloseGraceSec = 0.85;
        constexpr double kFollowupStableSec = 0.40;
        constexpr double kDuplicatePayProtectSec = 1.20;
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
            double nextRetrySec{ 0.0 };
            double closeGraceUntilSec{ 0.0 };
            double followupSeenAtSec{ 0.0 };
            double duplicatePayProtectUntilSec{ 0.0 };
            double expiresSec{ 0.0 };
        };

        std::mutex gLock;
        std::unordered_map<std::uint32_t, PreCombatState> gPreCombat;
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
    }

    void Install()
    {
        std::scoped_lock lk(gLock);
        gPreCombat.clear();
        spdlog::info("[TFD][Extortion] Install");
    }

    void Shutdown()
    {
        std::scoped_lock lk(gLock);
        gPreCombat.clear();
        spdlog::info("[TFD][Extortion] Shutdown");
    }

    void CancelAll(const char* reason)
    {
        std::scoped_lock lk(gLock);
        if (!gPreCombat.empty()) {
            spdlog::info(
                "[TFD][Extortion] CancelAll count={} reason={}",
                gPreCombat.size(),
                reason ? reason : "unknown");
        }
        gPreCombat.clear();
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

        std::scoped_lock lk(gLock);
        const auto handle = GetHandleId(actor);
        const auto nowSec = NowSec();
        auto it = gPreCombat.find(handle);
        if (it != gPreCombat.end() && it->second.active) {
            it->second.expiresSec = std::max(it->second.expiresSec, nowSec + kPreCombatWindowSec);
            it->second.closeGraceUntilSec = 0.0;
            it->second.duplicatePayProtectUntilSec = std::max(it->second.duplicatePayProtectUntilSec, nowSec + kDuplicatePayProtectSec);
            spdlog::info(
                "[TFD][Extortion] begin ignored actor={:08X} reason={} state=already_active protect={:.2f}s",
                actor->GetFormID(),
                reason ? reason : "unknown",
                kDuplicatePayProtectSec);
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
        state.nextRetrySec = 0.0;
        state.closeGraceUntilSec = 0.0;
        state.followupSeenAtSec = 0.0;
        state.duplicatePayProtectUntilSec = 0.0;
        state.expiresSec = nowSec + kPreCombatWindowSec;

        spdlog::info(
            "[TFD][Extortion] begin precombat actor={:08X} reason={} window={:.1f}s",
            actor->GetFormID(),
            reason ? reason : "unknown",
            kPreCombatWindowSec);
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

        bool cancelDialogue = false;
        {
            std::scoped_lock lk(gLock);
            auto it = gPreCombat.find(GetHandleId(actor));
            if (it == gPreCombat.end()) {
                return;
            }

            it->second.terminalCommitted = true;
            spdlog::info(
                "[TFD][Extortion] terminal outcome actor={:08X} event={}",
                actor->GetFormID(),
                eventName);
            gPreCombat.erase(it);
            cancelDialogue = true;
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

        bool cancelDialogue = false;
        {
            std::scoped_lock lk(gLock);
            auto it = gPreCombat.find(GetHandleId(actor));
            if (it == gPreCombat.end()) {
                return;
            }

            it->second.terminalCommitted = true;
            spdlog::info(
                "[TFD][Extortion] terminal pending actor={:08X} reason={}",
                actor->GetFormID(),
                reason ? reason : "unknown");
            gPreCombat.erase(it);
            cancelDialogue = true;
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
        if (nowSec >= state.expiresSec) {
            spdlog::info(
                "[TFD][Extortion] expired actor={:08X} menuSeen={} retries={}",
                actor->GetFormID(),
                state.menuSeen ? 1 : 0,
                state.retryCount);
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
            state.nextRetrySec = nowSec + kCanonicalReopenDelaySec;
            spdlog::info(
                "[TFD][Extortion] root close latched actor={:08X} stableTicks={} reopenDelay={:.2f}s",
                actor->GetFormID(),
                state.initialClosedStableTicks,
                kCanonicalReopenDelaySec);
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

                TFD::InteractionRouter::DialogueOpen::BeginPreCombatTruce(actor);
                if (canonical) {
                    spdlog::info(
                        "[TFD][Extortion] canonical reopen actor={:08X} retry={}/{} delay={:.2f}s",
                        actor->GetFormID(),
                        state.retryCount,
                        kMaxRetries + 1,
                        kRetryDelaySec);
                } else {
                    spdlog::info(
                        "[TFD][Extortion] reopen followup actor={:08X} retry={}/{} delay={:.2f}s",
                        actor->GetFormID(),
                        state.retryCount,
                        kMaxRetries + 1,
                        kRetryDelaySec);
                }
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

        if (state.closeGraceUntilSec <= 0.0) {
            state.closeGraceUntilSec = nowSec + kCloseGraceSec;
            spdlog::info(
                "[TFD][Extortion] close grace armed actor={:08X} grace={:.2f}s",
                actor->GetFormID(),
                kCloseGraceSec);
            return TickResult::Consumed;
        }

        if (nowSec < state.closeGraceUntilSec) {
            return TickResult::Consumed;
        }

        spdlog::info(
            "[TFD][Extortion] close grace expired actor={:08X} stableFor={:.2f}s",
            actor->GetFormID(),
            nowSec - state.followupSeenAtSec);
        gPreCombat.erase(it);
        return TickResult::AllowAbort;
    }
}
