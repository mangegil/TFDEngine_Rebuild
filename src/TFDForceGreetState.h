#pragma once

namespace RE
{
    namespace BSScript
    {
        class IVirtualMachine;
    }
}

namespace TFD::ForceGreetState
{
    bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm);

    void ResetAll(const char* a_reason = nullptr);

    bool ResetAfterPleasure();
    bool SetAfterPleasureOpened();
    bool SetAfterPleasureCommitted();
    int GetAfterPleasureState();
    bool IsAfterPleasureCommitted();

    bool ResetPleasureFailed();
    bool SetPleasureFailedOpened();
    bool SetPleasureFailedCommitted();
    int GetPleasureFailedState();
    bool IsPleasureFailedCommitted();

    bool ResetCaptive();
    bool SetCaptiveOpened();
    bool SetCaptiveCommitted();
    int GetCaptiveState();
    bool IsCaptiveCommitted();

    bool ResetPreCombat();
    bool SetPreCombatOpened();
    bool SetPreCombatCommitted();
    int GetPreCombatState();
    bool IsPreCombatCommitted();

    bool ResetInCombat();
    bool SetInCombatOpened();
    bool SetInCombatCommitted();
    int GetInCombatState();
    bool IsInCombatCommitted();

    bool ResetBleedout();
    bool SetBleedoutOpened();
    bool SetBleedoutCommitted();
    int GetBleedoutState();
    bool IsBleedoutCommitted();

    bool ResetVictory();
    bool SetVictoryOpened();
    bool SetVictoryCommitted();
    int GetVictoryState();
    bool IsVictoryCommitted();

    bool ResetTeammate();
    bool SetTeammateOpened();
    bool SetTeammateCommitted();
    int GetTeammateState();
    bool IsTeammateCommitted();

    bool ResetRescue();
    bool SetRescueOpened();
    bool SetRescueCommitted();
    int GetRescueState();
    bool IsRescueCommitted();
}
