#pragma once

#include <cstdint>

namespace RE
{
    class Actor;

    namespace BSScript
    {
        class IVirtualMachine;
    }
}

namespace TFD::WorkNative
{
    bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm);
    void ResetRuntimeRecipeCache();
    void RefreshWorkDemandFactionsForBoss(RE::Actor* a_boss, const char* a_reason = nullptr);
    void ClearWorkDemandFactions(const char* a_reason = nullptr);
    RE::Actor* SelectNextBossWithExactWorkOffer(RE::Actor* a_preferredActor = nullptr, std::uint32_t a_excludeFormID = 0, const char* a_reason = nullptr);
    bool ActorHasExactWorkOffer(RE::Actor* a_actor);
}
