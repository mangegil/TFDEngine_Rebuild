#include "TFDCombatBehavior.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include <atomic>

namespace TFD::CombatBehavior
{
    namespace
    {
        std::atomic_bool g_installed{ false };

        RE::Actor* PapyrusGetPendingCombatAssistTarget(RE::StaticFunctionTag*, RE::Actor*)
        {
            return nullptr;
        }

        bool PapyrusHasPendingCombatAssistTarget(RE::StaticFunctionTag*, RE::Actor*)
        {
            return false;
        }
    }

    void Install()
    {
        if (g_installed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        spdlog::info("[TFD][CombatBehavior] installed R86 real vanilla assist isolation diagnostic: normal combat worker disabled, pending assist disabled");
    }

    void Shutdown()
    {
        if (!g_installed.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        spdlog::info("[TFD][CombatBehavior] shutdown R86 real vanilla assist isolation diagnostic");
    }

    void ResetForLoad()
    {
        spdlog::info("[TFD][CombatBehavior] reset runtime state R86 real vanilla assist isolation diagnostic");
    }

    bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm)
    {
        if (!a_vm) {
            return false;
        }

        a_vm->RegisterFunction(
            "GetPendingCombatAssistTarget",
            "TFDCombatBehaviorNative",
            PapyrusGetPendingCombatAssistTarget);

        a_vm->RegisterFunction(
            "HasPendingCombatAssistTarget",
            "TFDCombatBehaviorNative",
            PapyrusHasPendingCombatAssistTarget);

        spdlog::info("[TFD][CombatBehavior] Papyrus natives registered R86 real vanilla assist isolation diagnostic");
        return true;
    }

    bool IsNormalCombatAssistActive()
    {
        return false;
    }

    bool IsActiveCombatAlly(RE::Actor*)
    {
        return false;
    }

    bool IsActiveCombatThreat(RE::Actor*)
    {
        return false;
    }

    bool IsActiveCombatPair(RE::Actor*, RE::Actor*)
    {
        return false;
    }

    bool ShouldPreserveCombatTarget(RE::Actor*, RE::Actor*)
    {
        return false;
    }

    bool ShouldSuppressTeammatePackageRepair(RE::Actor*, const char*)
    {
        return false;
    }

    bool IsDiagnosticActor(RE::Actor*)
    {
        return false;
    }

    bool IsDiagnosticPair(RE::Actor*, RE::Actor*)
    {
        return false;
    }

    const char* DiagnosticRole(RE::Actor*)
    {
        return "none";
    }
}
