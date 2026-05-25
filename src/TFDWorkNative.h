#pragma once

namespace RE
{
    namespace BSScript
    {
        class IVirtualMachine;
    }
}

namespace TFD::WorkNative
{
    bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm);
    void ResetRuntimeRecipeCache();
}
