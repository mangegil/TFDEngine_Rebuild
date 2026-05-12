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

    // Runtime diagnostics for short-lived combat advisory targets while the player is in bleedout battle observe.
    bool IsDiagnosticActor(RE::Actor* actor);
    bool IsDiagnosticPair(RE::Actor* actor, RE::Actor* target);
    const char* DiagnosticRole(RE::Actor* actor);

    // Normal combat ownership stays disabled. The current implementation only protects the bleedout
    // battle-observe assist advisor so teammates keep pressure on standing threats while the player is down.
    bool IsNormalCombatAssistActive();
    bool IsActiveCombatAlly(RE::Actor* actor);
    bool IsActiveCombatThreat(RE::Actor* actor);
    bool IsActiveCombatPair(RE::Actor* actor, RE::Actor* target);
    bool ShouldPreserveCombatTarget(RE::Actor* actor, RE::Actor* target);
    bool ShouldSuppressTeammatePackageRepair(RE::Actor* actor, const char* reason = nullptr);
}
