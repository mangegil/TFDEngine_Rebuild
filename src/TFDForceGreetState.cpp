#include "TFDForceGreetState.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cstdint>

namespace TFD::ForceGreetState
{
    namespace
    {
        enum class Flow : std::uint8_t
        {
            AfterPleasure = 0,
            PleasureFailed = 1,
            Captive = 2,
            PreCombat = 3,
            InCombat = 4,
            Bleedout = 5,
            Victory = 6,
            Teammate = 7,
            Rescue = 8,
            Count = 9
        };

        struct FlowState
        {
            const char* label{ "" };
            const char* globalEditorID{ "" };
            RE::TESGlobal* global{ nullptr };
            int lastValue{ -1 };
        };

        std::array<FlowState, static_cast<std::size_t>(Flow::Count)> g_states{
            FlowState{ "AfterPleasure", "TFDAfterPleasureFGState", nullptr, -1 },
            FlowState{ "PleasureFailed", "TFDPleasureFailedFGState", nullptr, -1 },
            FlowState{ "Captive", "TFDCaptiveFGState", nullptr, -1 },
            FlowState{ "PreCombat", "TFDPreCombatFGState", nullptr, -1 },
            FlowState{ "InCombat", "TFDInCombatFGState", nullptr, -1 },
            FlowState{ "Bleedout", "TFDBleedoutFGState", nullptr, -1 },
            FlowState{ "Victory", "TFDVictoryFGState", nullptr, -1 },
            FlowState{ "Teammate", "TFDTeammateFGState", nullptr, -1 },
            FlowState{ "Rescue", "TFDRescueFGState", nullptr, -1 }
        };

        constexpr int kIdle = 0;
        constexpr int kOpenedNoCommit = 1;
        constexpr int kCommitted = 2;

        FlowState& Get(Flow a_flow)
        {
            return g_states[static_cast<std::size_t>(a_flow)];
        }

        RE::TESGlobal* Resolve(FlowState& a_state)
        {
            if (!a_state.global) {
                a_state.global = RE::TESForm::LookupByEditorID<RE::TESGlobal>(a_state.globalEditorID);
                if (!a_state.global) {
                    spdlog::warn("[TFD][ForceGreetState][R327A] missing global editorID={} label={}",
                        a_state.globalEditorID ? a_state.globalEditorID : "<null>",
                        a_state.label ? a_state.label : "<null>");
                }
            }
            return a_state.global;
        }

        bool Set(Flow a_flow, int a_value, const char* a_reason)
        {
            auto& state = Get(a_flow);
            auto* global = Resolve(state);
            if (!global) {
                return false;
            }

            const int previous = static_cast<int>(global->value);
            global->value = static_cast<float>(a_value);
            state.lastValue = a_value;

            spdlog::info("[TFD][ForceGreetState][R327A] {} {} -> {} reason={}",
                state.label ? state.label : "<unknown>",
                previous,
                a_value,
                a_reason ? a_reason : "");
            return true;
        }

        int GetValue(Flow a_flow)
        {
            auto& state = Get(a_flow);
            auto* global = Resolve(state);
            if (!global) {
                return -1;
            }

            return static_cast<int>(global->value);
        }

        bool SetOpenedNoDowngrade(Flow a_flow, const char* a_reason)
        {
            const int current = GetValue(a_flow);
            auto& state = Get(a_flow);
            if (current >= kCommitted) {
                spdlog::info("[TFD][ForceGreetState][R327A] {} opened ignored committed current={} reason={}",
                    state.label ? state.label : "<unknown>",
                    current,
                    a_reason ? a_reason : "SetOpened");
                return true;
            }
            if (current == kOpenedNoCommit) {
                spdlog::debug("[TFD][ForceGreetState][R327A] {} opened idempotent current=1 reason={}",
                    state.label ? state.label : "<unknown>",
                    a_reason ? a_reason : "SetOpened");
                return true;
            }
            return Set(a_flow, kOpenedNoCommit, a_reason);
        }

        bool SetCommittedNoDowngrade(Flow a_flow, const char* a_reason)
        {
            const int current = GetValue(a_flow);
            auto& state = Get(a_flow);
            if (current >= kCommitted) {
                spdlog::debug("[TFD][ForceGreetState][R327A] {} committed idempotent current={} reason={}",
                    state.label ? state.label : "<unknown>",
                    current,
                    a_reason ? a_reason : "SetCommitted");
                return true;
            }
            return Set(a_flow, kCommitted, a_reason);
        }

        bool PapyrusResetAfterPleasure(RE::StaticFunctionTag*) { return ResetAfterPleasure(); }
        bool PapyrusSetAfterPleasureOpened(RE::StaticFunctionTag*) { return SetAfterPleasureOpened(); }
        bool PapyrusSetAfterPleasureCommitted(RE::StaticFunctionTag*) { return SetAfterPleasureCommitted(); }
        int PapyrusGetAfterPleasureState(RE::StaticFunctionTag*) { return GetAfterPleasureState(); }
        bool PapyrusIsAfterPleasureCommitted(RE::StaticFunctionTag*) { return IsAfterPleasureCommitted(); }

        bool PapyrusResetPleasureFailed(RE::StaticFunctionTag*) { return ResetPleasureFailed(); }
        bool PapyrusSetPleasureFailedOpened(RE::StaticFunctionTag*) { return SetPleasureFailedOpened(); }
        bool PapyrusSetPleasureFailedCommitted(RE::StaticFunctionTag*) { return SetPleasureFailedCommitted(); }
        int PapyrusGetPleasureFailedState(RE::StaticFunctionTag*) { return GetPleasureFailedState(); }
        bool PapyrusIsPleasureFailedCommitted(RE::StaticFunctionTag*) { return IsPleasureFailedCommitted(); }

        bool PapyrusResetCaptive(RE::StaticFunctionTag*) { return ResetCaptive(); }
        bool PapyrusSetCaptiveOpened(RE::StaticFunctionTag*) { return SetCaptiveOpened(); }
        bool PapyrusSetCaptiveCommitted(RE::StaticFunctionTag*) { return SetCaptiveCommitted(); }
        int PapyrusGetCaptiveState(RE::StaticFunctionTag*) { return GetCaptiveState(); }
        bool PapyrusIsCaptiveCommitted(RE::StaticFunctionTag*) { return IsCaptiveCommitted(); }

        bool PapyrusResetPreCombat(RE::StaticFunctionTag*) { return ResetPreCombat(); }
        bool PapyrusSetPreCombatOpened(RE::StaticFunctionTag*) { return SetPreCombatOpened(); }
        bool PapyrusSetPreCombatCommitted(RE::StaticFunctionTag*) { return SetPreCombatCommitted(); }
        int PapyrusGetPreCombatState(RE::StaticFunctionTag*) { return GetPreCombatState(); }
        bool PapyrusIsPreCombatCommitted(RE::StaticFunctionTag*) { return IsPreCombatCommitted(); }

        bool PapyrusResetInCombat(RE::StaticFunctionTag*) { return ResetInCombat(); }
        bool PapyrusSetInCombatOpened(RE::StaticFunctionTag*) { return SetInCombatOpened(); }
        bool PapyrusSetInCombatCommitted(RE::StaticFunctionTag*) { return SetInCombatCommitted(); }
        int PapyrusGetInCombatState(RE::StaticFunctionTag*) { return GetInCombatState(); }
        bool PapyrusIsInCombatCommitted(RE::StaticFunctionTag*) { return IsInCombatCommitted(); }

        bool PapyrusResetBleedout(RE::StaticFunctionTag*) { return ResetBleedout(); }
        bool PapyrusSetBleedoutOpened(RE::StaticFunctionTag*) { return SetBleedoutOpened(); }
        bool PapyrusSetBleedoutCommitted(RE::StaticFunctionTag*) { return SetBleedoutCommitted(); }
        int PapyrusGetBleedoutState(RE::StaticFunctionTag*) { return GetBleedoutState(); }
        bool PapyrusIsBleedoutCommitted(RE::StaticFunctionTag*) { return IsBleedoutCommitted(); }

        bool PapyrusResetVictory(RE::StaticFunctionTag*) { return ResetVictory(); }
        bool PapyrusSetVictoryOpened(RE::StaticFunctionTag*) { return SetVictoryOpened(); }
        bool PapyrusSetVictoryCommitted(RE::StaticFunctionTag*) { return SetVictoryCommitted(); }
        int PapyrusGetVictoryState(RE::StaticFunctionTag*) { return GetVictoryState(); }
        bool PapyrusIsVictoryCommitted(RE::StaticFunctionTag*) { return IsVictoryCommitted(); }

        bool PapyrusResetTeammate(RE::StaticFunctionTag*) { return ResetTeammate(); }
        bool PapyrusSetTeammateOpened(RE::StaticFunctionTag*) { return SetTeammateOpened(); }
        bool PapyrusSetTeammateCommitted(RE::StaticFunctionTag*) { return SetTeammateCommitted(); }
        int PapyrusGetTeammateState(RE::StaticFunctionTag*) { return GetTeammateState(); }
        bool PapyrusIsTeammateCommitted(RE::StaticFunctionTag*) { return IsTeammateCommitted(); }

        bool PapyrusResetRescue(RE::StaticFunctionTag*) { return ResetRescue(); }
        bool PapyrusSetRescueOpened(RE::StaticFunctionTag*) { return SetRescueOpened(); }
        bool PapyrusSetRescueCommitted(RE::StaticFunctionTag*) { return SetRescueCommitted(); }
        int PapyrusGetRescueState(RE::StaticFunctionTag*) { return GetRescueState(); }
        bool PapyrusIsRescueCommitted(RE::StaticFunctionTag*) { return IsRescueCommitted(); }
    }

    void ResetAll(const char* a_reason)
    {
        Set(Flow::AfterPleasure, kIdle, a_reason ? a_reason : "reset_all");
        Set(Flow::PleasureFailed, kIdle, a_reason ? a_reason : "reset_all");
        Set(Flow::Captive, kIdle, a_reason ? a_reason : "reset_all");
        Set(Flow::PreCombat, kIdle, a_reason ? a_reason : "reset_all");
        Set(Flow::InCombat, kIdle, a_reason ? a_reason : "reset_all");
        Set(Flow::Bleedout, kIdle, a_reason ? a_reason : "reset_all");
        Set(Flow::Victory, kIdle, a_reason ? a_reason : "reset_all");
        Set(Flow::Teammate, kIdle, a_reason ? a_reason : "reset_all");
        Set(Flow::Rescue, kIdle, a_reason ? a_reason : "reset_all");
    }

    bool ResetAfterPleasure() { return Set(Flow::AfterPleasure, kIdle, "ResetAfterPleasure"); }
    bool SetAfterPleasureOpened() { return Set(Flow::AfterPleasure, kOpenedNoCommit, "SetAfterPleasureOpened"); }
    bool SetAfterPleasureCommitted() { return Set(Flow::AfterPleasure, kCommitted, "SetAfterPleasureCommitted"); }
    int GetAfterPleasureState() { return GetValue(Flow::AfterPleasure); }
    bool IsAfterPleasureCommitted() { return GetAfterPleasureState() >= kCommitted; }

    bool ResetPleasureFailed() { return Set(Flow::PleasureFailed, kIdle, "ResetPleasureFailed"); }
    bool SetPleasureFailedOpened() { return Set(Flow::PleasureFailed, kOpenedNoCommit, "SetPleasureFailedOpened"); }
    bool SetPleasureFailedCommitted() { return Set(Flow::PleasureFailed, kCommitted, "SetPleasureFailedCommitted"); }
    int GetPleasureFailedState() { return GetValue(Flow::PleasureFailed); }
    bool IsPleasureFailedCommitted() { return GetPleasureFailedState() >= kCommitted; }

    bool ResetCaptive() { return Set(Flow::Captive, kIdle, "ResetCaptive"); }
    bool SetCaptiveOpened() { return SetOpenedNoDowngrade(Flow::Captive, "SetCaptiveOpened"); }
    bool SetCaptiveCommitted() { return SetCommittedNoDowngrade(Flow::Captive, "SetCaptiveCommitted"); }
    int GetCaptiveState() { return GetValue(Flow::Captive); }
    bool IsCaptiveCommitted() { return GetCaptiveState() >= kCommitted; }

    bool ResetPreCombat() { return Set(Flow::PreCombat, kIdle, "ResetPreCombat"); }
    bool SetPreCombatOpened() { return SetOpenedNoDowngrade(Flow::PreCombat, "SetPreCombatOpened"); }
    bool SetPreCombatCommitted() { return SetCommittedNoDowngrade(Flow::PreCombat, "SetPreCombatCommitted"); }
    int GetPreCombatState() { return GetValue(Flow::PreCombat); }
    bool IsPreCombatCommitted() { return GetPreCombatState() >= kCommitted; }

    bool ResetInCombat() { return Set(Flow::InCombat, kIdle, "ResetInCombat"); }
    bool SetInCombatOpened() { return SetOpenedNoDowngrade(Flow::InCombat, "SetInCombatOpened"); }
    bool SetInCombatCommitted() { return SetCommittedNoDowngrade(Flow::InCombat, "SetInCombatCommitted"); }
    int GetInCombatState() { return GetValue(Flow::InCombat); }
    bool IsInCombatCommitted() { return GetInCombatState() >= kCommitted; }

    bool ResetBleedout() { return Set(Flow::Bleedout, kIdle, "ResetBleedout"); }
    bool SetBleedoutOpened() { return SetOpenedNoDowngrade(Flow::Bleedout, "SetBleedoutOpened"); }
    bool SetBleedoutCommitted() { return SetCommittedNoDowngrade(Flow::Bleedout, "SetBleedoutCommitted"); }
    int GetBleedoutState() { return GetValue(Flow::Bleedout); }
    bool IsBleedoutCommitted() { return GetBleedoutState() >= kCommitted; }

    bool ResetVictory() { return Set(Flow::Victory, kIdle, "ResetVictory"); }
    bool SetVictoryOpened() { return SetOpenedNoDowngrade(Flow::Victory, "SetVictoryOpened"); }
    bool SetVictoryCommitted() { return SetCommittedNoDowngrade(Flow::Victory, "SetVictoryCommitted"); }
    int GetVictoryState() { return GetValue(Flow::Victory); }
    bool IsVictoryCommitted() { return GetVictoryState() >= kCommitted; }

    bool ResetTeammate() { return Set(Flow::Teammate, kIdle, "ResetTeammate"); }
    bool SetTeammateOpened() { return SetOpenedNoDowngrade(Flow::Teammate, "SetTeammateOpened"); }
    bool SetTeammateCommitted() { return SetCommittedNoDowngrade(Flow::Teammate, "SetTeammateCommitted"); }
    int GetTeammateState() { return GetValue(Flow::Teammate); }
    bool IsTeammateCommitted() { return GetTeammateState() >= kCommitted; }

    bool ResetRescue() { return Set(Flow::Rescue, kIdle, "ResetRescue"); }
    bool SetRescueOpened() { return SetOpenedNoDowngrade(Flow::Rescue, "SetRescueOpened"); }
    bool SetRescueCommitted() { return SetCommittedNoDowngrade(Flow::Rescue, "SetRescueCommitted"); }
    int GetRescueState() { return GetValue(Flow::Rescue); }
    bool IsRescueCommitted() { return GetRescueState() >= kCommitted; }

    bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm)
    {
        if (!a_vm) {
            return false;
        }

        a_vm->RegisterFunction("ResetAfterPleasure", "TFDForceGreetStateNative", PapyrusResetAfterPleasure);
        a_vm->RegisterFunction("SetAfterPleasureOpened", "TFDForceGreetStateNative", PapyrusSetAfterPleasureOpened);
        a_vm->RegisterFunction("SetAfterPleasureCommitted", "TFDForceGreetStateNative", PapyrusSetAfterPleasureCommitted);
        a_vm->RegisterFunction("GetAfterPleasureState", "TFDForceGreetStateNative", PapyrusGetAfterPleasureState);
        a_vm->RegisterFunction("IsAfterPleasureCommitted", "TFDForceGreetStateNative", PapyrusIsAfterPleasureCommitted);

        a_vm->RegisterFunction("ResetPleasureFailed", "TFDForceGreetStateNative", PapyrusResetPleasureFailed);
        a_vm->RegisterFunction("SetPleasureFailedOpened", "TFDForceGreetStateNative", PapyrusSetPleasureFailedOpened);
        a_vm->RegisterFunction("SetPleasureFailedCommitted", "TFDForceGreetStateNative", PapyrusSetPleasureFailedCommitted);
        a_vm->RegisterFunction("GetPleasureFailedState", "TFDForceGreetStateNative", PapyrusGetPleasureFailedState);
        a_vm->RegisterFunction("IsPleasureFailedCommitted", "TFDForceGreetStateNative", PapyrusIsPleasureFailedCommitted);

        a_vm->RegisterFunction("ResetCaptive", "TFDForceGreetStateNative", PapyrusResetCaptive);
        a_vm->RegisterFunction("SetCaptiveOpened", "TFDForceGreetStateNative", PapyrusSetCaptiveOpened);
        a_vm->RegisterFunction("SetCaptiveCommitted", "TFDForceGreetStateNative", PapyrusSetCaptiveCommitted);
        a_vm->RegisterFunction("GetCaptiveState", "TFDForceGreetStateNative", PapyrusGetCaptiveState);
        a_vm->RegisterFunction("IsCaptiveCommitted", "TFDForceGreetStateNative", PapyrusIsCaptiveCommitted);

        a_vm->RegisterFunction("ResetPreCombat", "TFDForceGreetStateNative", PapyrusResetPreCombat);
        a_vm->RegisterFunction("SetPreCombatOpened", "TFDForceGreetStateNative", PapyrusSetPreCombatOpened);
        a_vm->RegisterFunction("SetPreCombatCommitted", "TFDForceGreetStateNative", PapyrusSetPreCombatCommitted);
        a_vm->RegisterFunction("GetPreCombatState", "TFDForceGreetStateNative", PapyrusGetPreCombatState);
        a_vm->RegisterFunction("IsPreCombatCommitted", "TFDForceGreetStateNative", PapyrusIsPreCombatCommitted);

        a_vm->RegisterFunction("ResetInCombat", "TFDForceGreetStateNative", PapyrusResetInCombat);
        a_vm->RegisterFunction("SetInCombatOpened", "TFDForceGreetStateNative", PapyrusSetInCombatOpened);
        a_vm->RegisterFunction("SetInCombatCommitted", "TFDForceGreetStateNative", PapyrusSetInCombatCommitted);
        a_vm->RegisterFunction("GetInCombatState", "TFDForceGreetStateNative", PapyrusGetInCombatState);
        a_vm->RegisterFunction("IsInCombatCommitted", "TFDForceGreetStateNative", PapyrusIsInCombatCommitted);

        a_vm->RegisterFunction("ResetBleedout", "TFDForceGreetStateNative", PapyrusResetBleedout);
        a_vm->RegisterFunction("SetBleedoutOpened", "TFDForceGreetStateNative", PapyrusSetBleedoutOpened);
        a_vm->RegisterFunction("SetBleedoutCommitted", "TFDForceGreetStateNative", PapyrusSetBleedoutCommitted);
        a_vm->RegisterFunction("GetBleedoutState", "TFDForceGreetStateNative", PapyrusGetBleedoutState);
        a_vm->RegisterFunction("IsBleedoutCommitted", "TFDForceGreetStateNative", PapyrusIsBleedoutCommitted);

        a_vm->RegisterFunction("ResetVictory", "TFDForceGreetStateNative", PapyrusResetVictory);
        a_vm->RegisterFunction("SetVictoryOpened", "TFDForceGreetStateNative", PapyrusSetVictoryOpened);
        a_vm->RegisterFunction("SetVictoryCommitted", "TFDForceGreetStateNative", PapyrusSetVictoryCommitted);
        a_vm->RegisterFunction("GetVictoryState", "TFDForceGreetStateNative", PapyrusGetVictoryState);
        a_vm->RegisterFunction("IsVictoryCommitted", "TFDForceGreetStateNative", PapyrusIsVictoryCommitted);

        a_vm->RegisterFunction("ResetTeammate", "TFDForceGreetStateNative", PapyrusResetTeammate);
        a_vm->RegisterFunction("SetTeammateOpened", "TFDForceGreetStateNative", PapyrusSetTeammateOpened);
        a_vm->RegisterFunction("SetTeammateCommitted", "TFDForceGreetStateNative", PapyrusSetTeammateCommitted);
        a_vm->RegisterFunction("GetTeammateState", "TFDForceGreetStateNative", PapyrusGetTeammateState);
        a_vm->RegisterFunction("IsTeammateCommitted", "TFDForceGreetStateNative", PapyrusIsTeammateCommitted);

        a_vm->RegisterFunction("ResetRescue", "TFDForceGreetStateNative", PapyrusResetRescue);
        a_vm->RegisterFunction("SetRescueOpened", "TFDForceGreetStateNative", PapyrusSetRescueOpened);
        a_vm->RegisterFunction("SetRescueCommitted", "TFDForceGreetStateNative", PapyrusSetRescueCommitted);
        a_vm->RegisterFunction("GetRescueState", "TFDForceGreetStateNative", PapyrusGetRescueState);
        a_vm->RegisterFunction("IsRescueCommitted", "TFDForceGreetStateNative", PapyrusIsRescueCommitted);

        spdlog::info("[TFD][ForceGreetState][R327A] Papyrus natives registered for FGState framework AfterPleasure/PleasureFailed/Captive/PreCombat/InCombat/Bleedout/Victory/Teammate/Rescue");
        return true;
    }
}
