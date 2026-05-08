#pragma once

namespace RE
{
    class Actor;

    namespace BSScript
    {
        class IVirtualMachine;
    }
}

namespace TFD::CombatBehavior
{
    void Install();
    void Shutdown();
    void ResetForLoad();
    bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm);

    // Diagnostic helpers. R73 also uses this runtime state as short-lived normal-combat ownership.
    bool IsDiagnosticActor(RE::Actor* actor);
    bool IsDiagnosticPair(RE::Actor* actor, RE::Actor* target);
    const char* DiagnosticRole(RE::Actor* actor);

    // Normal combat ownership helpers. These must return false during truce, bleedout, captive,
    // pleasure, passive hold, terminal outcome, or release grace contexts.
    bool IsNormalCombatAssistActive();
    bool IsActiveCombatAlly(RE::Actor* actor);
    bool IsActiveCombatThreat(RE::Actor* actor);
    bool IsActiveCombatPair(RE::Actor* actor, RE::Actor* target);
    bool ShouldPreserveCombatTarget(RE::Actor* actor, RE::Actor* target);
    bool ShouldSuppressTeammatePackageRepair(RE::Actor* actor, const char* reason = nullptr);
}
