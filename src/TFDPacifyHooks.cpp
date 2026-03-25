#include "TFDPacifyHooks.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include <atomic>

#include "TFDPacify.h"
#include "TFDDefeatMonitor.h"

#ifdef SKYRIM_SUPPORT_AE
#define TFD_RELID(SE, AE) REL::ID(AE)
#define TFD_OFFSET(SE, AE) AE
#else
#define TFD_RELID(SE, AE) REL::ID(SE)
#define TFD_OFFSET(SE, AE) SE
#endif

namespace TFD::PacifyHooks
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

                spdlog::info("[TFD][PacifyHooks] installed (DoDetect + UpdateCombat)");
            }

        private:
            static void ClearBlockedPlayerTarget(RE::Character* actor)
            {
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!actor || !player || actor == player) {
                    return;
                }

                if (!TFD::DefeatMonitor::IsPlayerBleedHoldTargetBlocked()) {
                    return;
                }

                auto targetSp = actor->GetActorRuntimeData().currentCombatTarget.get();
                auto* target = targetSp.get();
                if (target != player) {
                    return;
                }

                actor->GetActorRuntimeData().currentCombatTarget = RE::ActorHandle{};
                if (auto* process = RE::ProcessLists::GetSingleton()) {
                    process->ClearCachedFactionFightReactions();
                }
            }

            static void UpdateCombat(RE::Character* actor)
            {
                if (actor && TFD::Pacify::IsPacified(actor)) {
                    if (auto* process = RE::ProcessLists::GetSingleton()) {
                        const bool runDetection = process->runDetection;
                        process->runDetection = false;
                        process->ClearCachedFactionFightReactions();
                        process->StopCombatAndAlarmOnActor(actor, false);
                        process->runDetection = runDetection;
                    }

                    if (actor->IsInCombat()) {
                        actor->StopCombat();
                    }

                    return;
                }

                ClearBlockedPlayerTarget(actor);
                _UpdateCombat(actor);
                ClearBlockedPlayerTarget(actor);
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
                if ((target && TFD::Pacify::IsPacified(target)) ||
                    (viewer && TFD::Pacify::IsPacified(viewer))) {
                    detectVal = -1000;
                    return nullptr;
                }

                auto* player = RE::PlayerCharacter::GetSingleton();
                if (player && viewer && target == player && viewer != player && TFD::DefeatMonitor::IsPlayerBleedHoldTargetBlocked()) {
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
