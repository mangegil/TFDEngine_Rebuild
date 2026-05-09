#include "TFDRelease.h"

#include "TFDActor.h"
#include "TFDBleedoutGreet.h"
#include "TFDFlowController.h"
#include "TFDHostilityController.h"
#include "TFDInCombatGreet.h"
#include "TFDInteractionRouter.h"
#include "TFDPreCombatGreet.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>

namespace TFD::Release
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        enum class Phase : std::uint8_t
        {
            None = 0,
            Armed,
            GraceActive
        };

        struct State
        {
            Phase phase{ Phase::None };
            RE::ActorHandle actor{};
            RE::FormID actorFormID{ 0 };
            double armedAtSec{ 0.0 };
            double graceEndsAtSec{ 0.0 };
            double graceSeconds{ 0.0 };
            Source source{ Source::None };
            std::string reason{};
        };

        std::mutex gLock;
        State gState{};
        Clock::time_point gT0 = Clock::now();

        double NowSec()
        {
            return std::chrono::duration<double>(Clock::now() - gT0).count();
        }

        RE::Actor* ResolveActor(const RE::ActorHandle& handle)
        {
            if (!handle) {
                return nullptr;
            }
            auto sp = RE::Actor::LookupByHandle(handle.native_handle());
            return sp.get();
        }

        RE::Actor* ResolveActorFromEventArg(const char* eventArg, RE::TESForm* sender = nullptr)
        {
            if (eventArg && *eventArg) {
                char* end = nullptr;
                const auto raw = std::strtoul(eventArg, &end, 0);
                if (end != nullptr && end != eventArg) {
                    if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(static_cast<RE::FormID>(raw))) {
                        return actor;
                    }
                }
            }

            return sender ? sender->As<RE::Actor>() : nullptr;
        }

        bool MapReleaseEvent(std::string_view name, Source& source, const char*& reason, double& defaultGrace)
        {
            if (name == std::string_view("TFDPreCombatOutcomeRelease")) {
                source = Source::PreCombat;
                reason = "precombat_release_to_vanilla";
                defaultGrace = 8.0;
                return true;
            }
            if (name == std::string_view("TFDInCombatOutcomeRelease")) {
                source = Source::InCombat;
                reason = "incombat_release_to_vanilla";
                defaultGrace = 10.0;
                return true;
            }
            if (name == std::string_view("TFDBleedoutOutcomeRelease")) {
                source = Source::Bleedout;
                reason = "bleedout_release_to_vanilla";
                defaultGrace = 10.0;
                return true;
            }
            if (name == std::string_view("TFDPleasureOutcomeRelease")) {
                source = Source::AfterPleasure;
                reason = "pleasure_release_to_vanilla";
                defaultGrace = 10.0;
                return true;
            }
            return false;
        }

        void ArmRelease(RE::Actor* actor, double graceSeconds, Source source, const char* reason)
        {
            if (!actor) {
                return;
            }

            std::scoped_lock lk(gLock);
            gState.phase = Phase::Armed;
            gState.actor = actor->GetHandle();
            gState.actorFormID = actor->GetFormID();
            gState.armedAtSec = NowSec();
            gState.graceEndsAtSec = 0.0;
            gState.graceSeconds = graceSeconds > 0.0 ? graceSeconds : 8.0;
            gState.source = source;
            gState.reason = reason ? reason : "release_to_vanilla";

            spdlog::info(
                "[TFD][Release] armed actor={:08X} source={} grace={:.1f}s reason={}",
                gState.actorFormID,
                ToString(source),
                gState.graceSeconds,
                gState.reason);
        }

        void BeginGracePhase(const State& state)
        {
            auto* actor = ResolveActor(state.actor);
            if (!actor) {
                spdlog::warn(
                    "[TFD][Release] begin skipped actor={:08X} source={} reason=invalid_actor",
                    state.actorFormID,
                    ToString(state.source));
                return;
            }

            TFD::InteractionRouter::DialogueOpen::Cancel();
            TFD::PreCombatGreet::CancelAll();
            TFD::InCombatGreet::CancelAll("release_begin");
            TFD::BleedoutGreet::Cancel("release_begin");

            TFD::Actor::Ops::ApplyReleaseFollowGraceToSpeakerAndCrowd(
                actor,
                state.graceSeconds,
                state.reason.c_str());

            (void)TFD::HostilityController::ReleaseActiveTruceSessionForActor(
                actor,
                TFD::HostilityController::ReleaseReason::FlowHandoff,
                false);

            spdlog::info(
                "[TFD][Release] begin actor={:08X} source={} grace={:.1f}s reason={}",
                actor->GetFormID(),
                ToString(state.source),
                state.graceSeconds,
                state.reason);
        }

        void FinalizeRelease(const State& state)
        {
            auto* actor = ResolveActor(state.actor);
            if (actor) {
                TFD::Actor::Ops::RemoveReleaseFollowGraceFromSpeakerAndCrowd(actor, "release_to_vanilla_end");
            }

            TFD::HostilityController::ClearAllTemporaryHostility();
            TFD::FlowController::Controller::GetSingleton().ResetRuntime("release_to_vanilla");
            (void)TFD::FlowController::QueueBridgeModEvent("TFDPreCombatClearAll");
            (void)TFD::FlowController::QueueBridgeModEvent("TFDTruceClearAll");
            (void)TFD::FlowController::QueueBridgeModEvent("TFDInCombatClearAll");
            (void)TFD::FlowController::QueueBridgeModEvent("TFDBleedoutClearAll");

            spdlog::info(
                "[TFD][Release] finalize actor={:08X} source={} reason={}",
                state.actorFormID,
                ToString(state.source),
                state.reason);
        }
    }

    void Install()
    {
        Reset();
        spdlog::info("[TFD][Release] Install");
    }

    void Reset()
    {
        State previous{};
        {
            std::scoped_lock lk(gLock);
            previous = gState;
            gState = {};
        }

        // R93W: if a load/reset happens during release-to-vanilla grace, do not
        // leave TFDPacifyFaction / release helper markers stuck on the actor pack.
        // PreCombat already depends on this grace lifecycle; clearing the runtime
        // without removing its actor-side markers is what makes the passive state
        // survive across saves.
        if (previous.phase != Phase::None) {
            if (auto* actor = ResolveActor(previous.actor)) {
                TFD::Actor::Ops::RemoveReleaseFollowGraceFromSpeakerAndCrowd(actor, "release_reset_r93w");
            }
            spdlog::info(
                "[TFD][Release][R93W] reset cleared active release actor={:08X} source={} phase={} reason={}",
                previous.actorFormID,
                ToString(previous.source),
                static_cast<unsigned int>(previous.phase),
                previous.reason.empty() ? "-" : previous.reason.c_str());
        }
    }

    void Tick()
    {
        State beginState{};
        State finalizeState{};
        bool runBegin = false;
        bool runFinalize = false;
        const auto now = NowSec();

        {
            std::scoped_lock lk(gLock);
            if (gState.phase == Phase::Armed) {
                beginState = gState;
                gState.phase = Phase::GraceActive;
                gState.graceEndsAtSec = now + gState.graceSeconds;
                runBegin = true;
            } else if (gState.phase == Phase::GraceActive && now >= gState.graceEndsAtSec) {
                finalizeState = gState;
                gState = {};
                runFinalize = true;
            }
        }

        if (runBegin) {
            BeginGracePhase(beginState);
        }
        if (runFinalize) {
            FinalizeRelease(finalizeState);
        }
    }

    bool BeginRelease(RE::Actor* actor, double graceSeconds, Source source, const char* reason)
    {
        if (!actor) {
            return false;
        }
        ArmRelease(actor, graceSeconds, source, reason);
        return true;
    }

    bool HandleModCallbackEvent(const SKSE::ModCallbackEvent* ev)
    {
        if (!ev) {
            return false;
        }

        const auto* rawName = ev->eventName.c_str();
        const std::string_view name = rawName ? std::string_view(rawName) : std::string_view{};
        if (name.empty()) {
            return false;
        }

        Source source = Source::None;
        const char* reason = nullptr;
        double defaultGrace = 0.0;
        if (!MapReleaseEvent(name, source, reason, defaultGrace)) {
            return false;
        }

        auto* actor = ResolveActorFromEventArg(ev->strArg.c_str(), ev->sender);
        if (!actor) {
            spdlog::warn(
                "[TFD][Release] event={} ignored reason=invalid_actor arg={}",
                std::string(name),
                ev->strArg.c_str());
            return true;
        }

        const double graceSeconds = ev->numArg > 0.0f ? static_cast<double>(ev->numArg) : defaultGrace;
        ArmRelease(actor, graceSeconds, source, reason);
        return true;
    }

    bool IsActive()
    {
        std::scoped_lock lk(gLock);
        return gState.phase != Phase::None;
    }

    const char* ToString(Source source)
    {
        switch (source) {
        case Source::PreCombat:
            return "PreCombat";
        case Source::InCombat:
            return "InCombat";
        case Source::Bleedout:
            return "Bleedout";
        case Source::AfterPleasure:
            return "AfterPleasure";
        default:
            return "None";
        }
    }
}
