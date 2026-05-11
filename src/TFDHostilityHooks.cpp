#include "TFDHostilityHooks.h"

#include "TFDHostilityController.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdint>

#ifdef SKYRIM_SUPPORT_AE
#define TFD_RELID(SE, AE) REL::ID(AE)
#define TFD_OFFSET(SE, AE) AE
#else
#define TFD_RELID(SE, AE) REL::ID(SE)
#define TFD_OFFSET(SE, AE) SE
#endif

namespace TFD::HostilityHooks
{
    namespace
    {
        class Hooks
        {
        public:
            static void Install()
            {
                if (gInstalled.exchange(true, std::memory_order_acq_rel)) {
                    return;
                }

                SKSE::AllocTrampoline(1 << 7);
                auto& trampoline = SKSE::GetTrampoline();

                REL::Relocation<std::uintptr_t> detectTarget{ TFD_RELID(41659, 42742), TFD_OFFSET(0x526, 0x67B) };
                _DoDetect = trampoline.write_call<5>(detectTarget.address(), DoDetect);

                REL::Relocation<std::uintptr_t> characterVtbl{ RE::Character::VTABLE[0] };
                _UpdateCombat = characterVtbl.write_vfunc(0xE4, UpdateCombat);

                spdlog::info("[TFD][HostilityHooks] R93H scoped suppression hooks installed (DoDetect + UpdateCombat; suppressed actors only)");
            }

        private:
            static bool IsScopedSuppressed(RE::Actor* actor)
            {
                return actor && TFD::HostilityController::IsActorTemporarilySuppressed(actor);
            }

            // R100C: Keep the virtual UpdateCombat hook lightweight.
            //
            // This hook is entered from the engine combat update path. Calling heavy
            // engine mutators from inside it (StopCombatAndAlarmOnActor, StopCombat,
            // DrawWeaponMagicHands, EvaluatePackage, or spdlog formatting that touches
            // actor state after those calls) can re-enter combat/package code while the
            // engine is already iterating combat state. Crash logs from the bleedout
            // hit transition pointed at this hook with phase=UpdateCombat on stack.
            //
            // The real suppression work is already done safely from
            // HostilityController::Update() through ApplySuppression(). Here we only
            // block the original UpdateCombat call for temporarily suppressed actors.

            static void UpdateCombat(RE::Character* character)
            {
                auto* actor = static_cast<RE::Actor*>(character);
                if (IsScopedSuppressed(actor)) {
                    return;
                }

                _UpdateCombat(character);
            }

            static std::uint8_t* DoDetect(
                RE::Actor* viewer,
                RE::Actor* target,
                std::int32_t& detectVal,
                std::uint8_t& unk04,
                std::uint8_t& unk05,
                std::uint32_t& unk06,
                RE::NiPoint3& pos,
                float& unk08,
                float& unk09,
                float& unk10)
            {
                if (IsScopedSuppressed(viewer) || IsScopedSuppressed(target)) {
                    detectVal = -1000;
                    return nullptr;
                }

                return _DoDetect(viewer, target, detectVal, unk04, unk05, unk06, pos, unk08, unk09, unk10);
            }

            static inline std::atomic_bool gInstalled{ false };
            static inline REL::Relocation<decltype(UpdateCombat)> _UpdateCombat;
            static inline REL::Relocation<decltype(DoDetect)> _DoDetect;
        };
    }

    void Install()
    {
        Hooks::Install();
    }
}
