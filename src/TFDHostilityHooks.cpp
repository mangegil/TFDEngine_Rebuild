#include "TFDHostilityHooks.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include <atomic>

#include "TFDHostilityController.h"
#include "TFDTame.h"
#include "TFDDefeatMonitor.h"
#include "TFDFactionManager.h"

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

                spdlog::info("[TFD][HostilityHooks] installed (DoDetect + UpdateCombat)");
            }

        private:
            static bool IsInvalidCombatTarget(RE::Actor* actor)
            {
                if (!actor) {
                    return true;
                }
                if (TFD::HostilityController::IsSuppressed(actor)) {
                    return true;
                }
                return !TFD::DefeatMonitor::IsThresholdCombatTargetValid(actor);
            }

            static bool ShouldPreservePlayerBleedTarget(RE::Character*, RE::Actor*)
            {
                return false;
            }

            static bool IsReleaseGraceActor(RE::Actor* actor)
            {
                return actor && TFD::FactionManager::HasReleaseFollowGrace(actor);
            }

            static void ClearInvalidCombatTarget(RE::Character* actor)
            {
                if (!actor) {
                    return;
                }

                auto targetSp = actor->GetActorRuntimeData().currentCombatTarget.get();
                auto* target = targetSp.get();
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (player && target == player) {
                    TFD::DefeatMonitor::NoteEnemyTargetingPlayer(actor);
                }

                if (!target || !IsInvalidCombatTarget(target)) {
                    return;
                }

                RE::Actor* replacement = nullptr;
                if (actor->IsPlayerTeammate() || TFD::Tame::IsCompanion(actor)) {
                    replacement = TFD::DefeatMonitor::ResolveBleedFollowerAggroTarget(actor);
                }
                if (!replacement) {
                    replacement = TFD::DefeatMonitor::ResolveBleedRedirectTarget(actor);
                }
                if (replacement && replacement != actor && replacement != target && !IsInvalidCombatTarget(replacement)) {
                    actor->GetActorRuntimeData().currentCombatTarget = replacement->GetHandle();
                    if (!actor->IsAIEnabled()) {
                        actor->EnableAI(true);
                    }
                    if ((actor->IsPlayerTeammate() || TFD::Tame::IsCompanion(actor)) && !actor->IsWeaponDrawn()) {
                        actor->DrawWeaponMagicHands(true);
                    }
                    actor->SetBeenAttacked(true);
                    replacement->SetBeenAttacked(true);
                    (void)actor->RequestDetectionLevel(replacement, RE::DETECTION_PRIORITY::kCritical);
                    (void)replacement->RequestDetectionLevel(actor, RE::DETECTION_PRIORITY::kCritical);
                    if (auto* process = RE::ProcessLists::GetSingleton()) {
                        process->ClearCachedFactionFightReactions();
                    }
                    actor->EvaluatePackage(false, true);
                    actor->EvaluatePackage(true, true);
                    replacement->EvaluatePackage(false, true);
                    replacement->EvaluatePackage(true, true);

                    spdlog::info("[TFD][HostilityHooks] swapped invalid combat target actor={:08X} old={:08X} new={:08X}",
                        actor->GetFormID(),
                        target ? target->GetFormID() : 0u,
                        replacement->GetFormID());
                    return;
                }

                actor->GetActorRuntimeData().currentCombatTarget = RE::ActorHandle{};
                if (auto* process = RE::ProcessLists::GetSingleton()) {
                    process->ClearCachedFactionFightReactions();
                }
                actor->EvaluatePackage(false, true);
                actor->EvaluatePackage(true, true);

                spdlog::info("[TFD][HostilityHooks] cleared invalid combat target actor={:08X} target={:08X}",
                    actor->GetFormID(),
                    target ? target->GetFormID() : 0u);
            }

            static void UpdateCombat(RE::Character* actor)
            {
                if (actor && (TFD::HostilityController::IsSuppressed(actor) || IsReleaseGraceActor(actor))) {
                    if (auto* process = RE::ProcessLists::GetSingleton()) {
                        const bool runDetection = process->runDetection;
                        process->runDetection = false;
                        process->ClearCachedFactionFightReactions();
                        process->StopCombatAndAlarmOnActor(actor, false);
                        process->runDetection = runDetection;
                    }

                    actor->GetActorRuntimeData().currentCombatTarget = RE::ActorHandle{};

                    if (actor->IsInCombat()) {
                        actor->StopCombat();
                    }

                    actor->EvaluatePackage(false, true);
                    actor->EvaluatePackage(true, true);
                    return;
                }

                auto targetSp = actor ? actor->GetActorRuntimeData().currentCombatTarget.get() : RE::NiPointer<RE::Actor>{};
                if (TFD::DefeatMonitor::IsObservedCombatCommitInProgress()) {
                    _UpdateCombat(actor);
                    return;
                }

                ClearInvalidCombatTarget(actor);
                _UpdateCombat(actor);
                ClearInvalidCombatTarget(actor);
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
                if ((target && (TFD::HostilityController::IsSuppressed(target) || IsReleaseGraceActor(target))) ||
                    (viewer && (TFD::HostilityController::IsSuppressed(viewer) || IsReleaseGraceActor(viewer)))) {
                    detectVal = -1000;
                    return nullptr;
                }

                auto* player = RE::PlayerCharacter::GetSingleton();
                if (viewer && target && player && target == player) {
                    TFD::DefeatMonitor::NoteEnemyTargetingPlayer(viewer);
                }

                if (target && !TFD::DefeatMonitor::IsThresholdCombatTargetValid(target)) {
                    detectVal = -1000;
                    return nullptr;
                }

                if (viewer && !TFD::DefeatMonitor::IsThresholdCombatTargetValid(viewer)) {
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
