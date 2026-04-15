#include "TFDExtortion.h"

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
        constexpr double kRetryDelaySec = 1.25;
        constexpr double kCloseGraceSec = 0.55;
        constexpr std::uint32_t kMaxRetries = 1;

        struct PreCombatState
        {
            bool active{ false };
            bool waitForInitialClose{ false };
            bool menuSeen{ false };
            bool terminalCommitted{ false };
            std::uint32_t retryCount{ 0 };
            double nextRetrySec{ 0.0 };
            double closeGraceUntilSec{ 0.0 };
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

    void BeginPreCombat(RE::Actor* actor, const char* reason)
    {
        if (!actor) {
            return;
        }

        std::scoped_lock lk(gLock);
        auto& state = gPreCombat[GetHandleId(actor)];
        state.active = true;
        state.waitForInitialClose = true;
        state.menuSeen = false;
        state.terminalCommitted = false;
        state.retryCount = 0;
        state.nextRetrySec = 0.0;
        state.closeGraceUntilSec = 0.0;
        state.expiresSec = NowSec() + kPreCombatWindowSec;

        spdlog::info(
            "[TFD][Extortion] begin precombat actor={:08X} reason={} window={:.1f}s",
            actor->GetFormID(),
            reason ? reason : "unknown",
            kPreCombatWindowSec);
    }

    void HandlePreCombatOutcomeEvent(const char* eventName, RE::Actor* actor)
    {
        if (!actor || !eventName || !*eventName) {
            return;
        }

        const std::string_view name{ eventName };
        if (name == std::string_view("TFDPreCombatOutcomePay")) {
            BeginPreCombat(actor, eventName);
            return;
        }

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

    bool TickPreCombat(
        RE::Actor* actor,
        double nowSec,
        bool dialogueOpen,
        bool dialogueOpenActiveForPreCombat)
    {
        if (!actor) {
            return false;
        }

        std::scoped_lock lk(gLock);
        auto it = gPreCombat.find(GetHandleId(actor));
        if (it == gPreCombat.end()) {
            return false;
        }

        auto& state = it->second;
        if (nowSec >= state.expiresSec) {
            spdlog::info(
                "[TFD][Extortion] expired actor={:08X} menuSeen={} retries={}",
                actor->GetFormID(),
                state.menuSeen ? 1 : 0,
                state.retryCount);
            gPreCombat.erase(it);
            return false;
        }

        if (state.waitForInitialClose) {
            if (dialogueOpen || dialogueOpenActiveForPreCombat) {
                return true;
            }

            state.waitForInitialClose = false;
            state.nextRetrySec = nowSec;
        }

        if (dialogueOpen) {
            if (!state.menuSeen) {
                spdlog::info(
                    "[TFD][Extortion] followup menu seen actor={:08X}",
                    actor->GetFormID());
            }
            state.menuSeen = true;
            state.closeGraceUntilSec = 0.0;
            return true;
        }

        if (dialogueOpenActiveForPreCombat) {
            return true;
        }

        if (!state.menuSeen) {
            if (state.retryCount < kMaxRetries + 1 && nowSec >= state.nextRetrySec) {
                ++state.retryCount;
                state.nextRetrySec = nowSec + kRetryDelaySec;
                TFD::InteractionRouter::DialogueOpen::BeginPreCombatTruce(actor);
                spdlog::info(
                    "[TFD][Extortion] reopen followup actor={:08X} retry={}/{}",
                    actor->GetFormID(),
                    state.retryCount,
                    kMaxRetries + 1);
                return true;
            }

            if (state.retryCount >= kMaxRetries + 1 && nowSec >= state.nextRetrySec) {
                spdlog::info(
                    "[TFD][Extortion] retries exhausted actor={:08X}",
                    actor->GetFormID());
                gPreCombat.erase(it);
                return false;
            }

            return true;
        }

        if (state.closeGraceUntilSec <= 0.0) {
            state.closeGraceUntilSec = nowSec + kCloseGraceSec;
            spdlog::info(
                "[TFD][Extortion] close grace armed actor={:08X} grace={:.2f}s",
                actor->GetFormID(),
                kCloseGraceSec);
            return true;
        }

        if (nowSec < state.closeGraceUntilSec) {
            return true;
        }

        spdlog::info(
            "[TFD][Extortion] close grace expired actor={:08X}",
            actor->GetFormID());
        gPreCombat.erase(it);
        return false;
    }
}
