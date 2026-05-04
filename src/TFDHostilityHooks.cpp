#include "TFDHostilityHooks.h"
#include "TFDActor.h"
#include "TFDCombatBehavior.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include <atomic>

#include "TFDHostilityController.h"
#include "TFDTame.h"
#include "TFDTeammateManager.h"
#include "TFDDefeatMonitor.h"
#include "TFDBleedout.h"
#include "TFDFlowController.h"

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

            static bool IsPlayerSideActor(RE::Actor* actor, RE::PlayerCharacter* player)
            {
                if (!actor || !player) {
                    return false;
                }

                return actor == player ||
                    actor->IsPlayerTeammate() ||
                    TFD::TeammateManager::IsActiveFollowerActor(actor) ||
                    TFD::Tame::IsCompanion(actor);
            }

            static bool IsPlayerSideCombatConflict(RE::Actor* actor, RE::Actor* target, RE::PlayerCharacter* player)
            {
                if (!actor || !target || !player) {
                    return false;
                }

                return IsPlayerSideActor(actor, player) && IsPlayerSideActor(target, player);
            }

            static void CalmPlayerSideCombatActor(RE::Actor* actor)
            {
                if (!actor) {
                    return;
                }

                actor->GetActorRuntimeData().currentCombatTarget = RE::ActorHandle{};
                if (auto* process = RE::ProcessLists::GetSingleton()) {
                    process->ClearCachedFactionFightReactions();
                    process->StopCombatAndAlarmOnActor(actor, false);
                }

                if (actor->IsInCombat()) {
                    actor->StopCombat();
                }

                actor->EvaluatePackage(false, true);
                actor->EvaluatePackage(true, true);
            }

            static bool ShouldPreservePlayerBleedTarget(RE::Character*, RE::Actor*)
            {
                return false;
            }

            static bool IsReleaseGraceActor(RE::Actor* actor)
            {
                return actor && TFD::Actor::Ops::HasReleaseFollowGrace(actor);
            }

            static RE::Actor* CurrentCombatTarget(RE::Actor* actor)
            {
                if (!actor) {
                    return nullptr;
                }
                auto sp = actor->GetActorRuntimeData().currentCombatTarget.get();
                return sp.get();
            }

            static std::uint32_t FormIDOrZero(RE::TESForm* form)
            {
                return form ? form->GetFormID() : 0u;
            }

            static bool ShouldTraceTargetTransition(RE::Character* actor, RE::Actor* before, RE::Actor* after)
            {
                auto* actorAsActor = static_cast<RE::Actor*>(actor);
                return actorAsActor && (
                    FormIDOrZero(before) != FormIDOrZero(after) ||
                    TFD::CombatBehavior::IsDiagnosticActor(actorAsActor) ||
                    TFD::CombatBehavior::IsDiagnosticActor(before) ||
                    TFD::CombatBehavior::IsDiagnosticActor(after) ||
                    TFD::CombatBehavior::IsDiagnosticPair(actorAsActor, before) ||
                    TFD::CombatBehavior::IsDiagnosticPair(actorAsActor, after));
            }

            static void LogTargetTransition(
                const char* phase,
                RE::Character* actor,
                RE::Actor* before,
                RE::Actor* afterPreClear,
                RE::Actor* afterVanilla,
                RE::Actor* afterPostClear)
            {
                if (!actor || !ShouldTraceTargetTransition(actor, before, afterPostClear)) {
                    return;
                }

                auto* actorAsActor = static_cast<RE::Actor*>(actor);
                spdlog::info(
                    "[TFD][HostilityHooksDiag] phase={} actor={:08X} before={:08X} afterPreClear={:08X} afterVanilla={:08X} afterPostClear={:08X} actorRole={} beforeRole={} preRole={} vanillaRole={} postRole={} pairBefore={} pairPre={} pairVanilla={} pairPost={} suppressed={} releaseGrace={} dialogue={} passive={} pleasure={} observedBleedout={} actorInCombat={}",
                    phase ? phase : "unknown",
                    actor->GetFormID(),
                    FormIDOrZero(before),
                    FormIDOrZero(afterPreClear),
                    FormIDOrZero(afterVanilla),
                    FormIDOrZero(afterPostClear),
                    TFD::CombatBehavior::DiagnosticRole(actorAsActor),
                    TFD::CombatBehavior::DiagnosticRole(before),
                    TFD::CombatBehavior::DiagnosticRole(afterPreClear),
                    TFD::CombatBehavior::DiagnosticRole(afterVanilla),
                    TFD::CombatBehavior::DiagnosticRole(afterPostClear),
                    TFD::CombatBehavior::IsDiagnosticPair(actorAsActor, before) ? 1 : 0,
                    TFD::CombatBehavior::IsDiagnosticPair(actorAsActor, afterPreClear) ? 1 : 0,
                    TFD::CombatBehavior::IsDiagnosticPair(actorAsActor, afterVanilla) ? 1 : 0,
                    TFD::CombatBehavior::IsDiagnosticPair(actorAsActor, afterPostClear) ? 1 : 0,
                    TFD::HostilityController::IsSuppressed(actor) ? 1 : 0,
                    IsReleaseGraceActor(actor) ? 1 : 0,
                    TFD::FlowController::IsDialogueContextActive() ? 1 : 0,
                    TFD::FlowController::IsPassiveHoldActive() ? 1 : 0,
                    TFD::FlowController::IsPleasureLockActive() ? 1 : 0,
                    TFD::Bleedout::DefeatGlue::IsObservedCombatCommitInProgress() ? 1 : 0,
                    actor->IsInCombat() ? 1 : 0);
            }

            static void SyncPlayerCombatFlow(RE::Character* actor)
            {
                if (!actor || !actor->IsPlayerRef()) {
                    return;
                }

                static std::atomic_bool gPlayerCombatActive{ false };
                static std::atomic<std::uint32_t> gPlayerCombatTargetId{ 0 };

                auto targetSp = actor->GetActorRuntimeData().currentCombatTarget.get();
                auto* target = targetSp.get();
                const auto targetId = target ? target->GetFormID() : 0u;
                const bool active = actor->IsInCombat() && targetId != 0;

                auto& flow = TFD::FlowController::Controller::GetSingleton();
                const bool wasActive = gPlayerCombatActive.load(std::memory_order_acquire);
                const auto lastTargetId = gPlayerCombatTargetId.load(std::memory_order_acquire);

                if (active) {
                    if (!wasActive || lastTargetId != targetId) {
                        flow.NotifyCombatStarted(targetId, "hostilityhooks_player_combat");
                    }
                    gPlayerCombatTargetId.store(targetId, std::memory_order_release);
                    gPlayerCombatActive.store(true, std::memory_order_release);
                    return;
                }

                if (wasActive) {
                    flow.NotifyCombatEnded("hostilityhooks_player_combat_end");
                }
                gPlayerCombatTargetId.store(0, std::memory_order_release);
                gPlayerCombatActive.store(false, std::memory_order_release);
            }

            static void ClearInvalidCombatTarget(RE::Character* actor, const char* phase)
            {
                if (!actor) {
                    return;
                }

                auto targetSp = actor->GetActorRuntimeData().currentCombatTarget.get();
                auto* target = targetSp.get();
                auto* player = RE::PlayerCharacter::GetSingleton();
                auto* actorAsActor = static_cast<RE::Actor*>(actor);
                const bool actorDiag = TFD::CombatBehavior::IsDiagnosticActor(actorAsActor);
                const bool targetDiag = TFD::CombatBehavior::IsDiagnosticActor(target);
                const bool pairDiag = TFD::CombatBehavior::IsDiagnosticPair(actorAsActor, target);

                if (target && TFD::CombatBehavior::ShouldPreserveCombatTarget(actorAsActor, target)) {
                    spdlog::info(
                        "[TFD][CombatOwnership] preserve active ally target actor={:08X} target={:08X} phase={} actorRole={} targetRole={} pairActive=1",
                        actor->GetFormID(),
                        target->GetFormID(),
                        phase ? phase : "unknown",
                        TFD::CombatBehavior::DiagnosticRole(actorAsActor),
                        TFD::CombatBehavior::DiagnosticRole(target));
                    return;
                }

                if (player && target && IsPlayerSideCombatConflict(actor, target, player)) {
                    const auto actorId = actor->GetFormID();
                    const auto targetId = target->GetFormID();
                    const bool actorTeammate = actor->IsPlayerTeammate();
                    const bool actorFollower = TFD::TeammateManager::IsActiveFollowerActor(actor);
                    const bool actorTame = TFD::Tame::IsCompanion(actor);

                    CalmPlayerSideCombatActor(actor);
                    spdlog::info(
                        "[TFD][HostilityHooks] cleared player-side combat target actor={:08X} target={:08X} actorTeammate={} actorFollower={} actorTame={} reason=player_side_conflict phase={} actorDiag={} targetDiag={} pairDiag={} actorRole={} targetRole={}",
                        actorId,
                        targetId,
                        actorTeammate ? 1 : 0,
                        actorFollower ? 1 : 0,
                        actorTame ? 1 : 0,
                        phase ? phase : "unknown",
                        actorDiag ? 1 : 0,
                        targetDiag ? 1 : 0,
                        pairDiag ? 1 : 0,
                        TFD::CombatBehavior::DiagnosticRole(actorAsActor),
                        TFD::CombatBehavior::DiagnosticRole(target));
                    return;
                }

                if (player && target == player && !IsPlayerSideActor(actor, player)) {
                    TFD::Bleedout::DefeatGlue::NoteEnemyTargetingPlayer(actor);
                }

                if (!target || !IsInvalidCombatTarget(target)) {
                    return;
                }

                RE::Actor* replacement = nullptr;
                if (actor->IsPlayerTeammate() || TFD::TeammateManager::IsActiveFollowerActor(actor) || TFD::Tame::IsCompanion(actor)) {
                    replacement = TFD::Bleedout::DefeatGlue::ResolveBleedFollowerAggroTarget(actor);
                }
                if (!replacement) {
                    replacement = TFD::Bleedout::DefeatGlue::ResolveBleedRedirectTarget(actor);
                }
                if (replacement && replacement != actor && replacement != target && !IsInvalidCombatTarget(replacement)) {
                    actor->GetActorRuntimeData().currentCombatTarget = replacement->GetHandle();
                    if (!actor->IsAIEnabled()) {
                        actor->EnableAI(true);
                    }
                    if ((actor->IsPlayerTeammate() || TFD::TeammateManager::IsActiveFollowerActor(actor) || TFD::Tame::IsCompanion(actor)) && !actor->IsWeaponDrawn()) {
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

                    spdlog::info("[TFD][HostilityHooks] swapped invalid combat target actor={:08X} old={:08X} new={:08X} phase={} actorDiag={} oldDiag={} pairDiag={} actorRole={} oldRole={} newRole={}",
                        actor->GetFormID(),
                        target ? target->GetFormID() : 0u,
                        replacement->GetFormID(),
                        phase ? phase : "unknown",
                        actorDiag ? 1 : 0,
                        targetDiag ? 1 : 0,
                        pairDiag ? 1 : 0,
                        TFD::CombatBehavior::DiagnosticRole(actorAsActor),
                        TFD::CombatBehavior::DiagnosticRole(target),
                        TFD::CombatBehavior::DiagnosticRole(replacement));
                    return;
                }

                actor->GetActorRuntimeData().currentCombatTarget = RE::ActorHandle{};
                if (auto* process = RE::ProcessLists::GetSingleton()) {
                    process->ClearCachedFactionFightReactions();
                }
                actor->EvaluatePackage(false, true);
                actor->EvaluatePackage(true, true);

                spdlog::info("[TFD][HostilityHooks] cleared invalid combat target actor={:08X} target={:08X} phase={} actorDiag={} targetDiag={} pairDiag={} actorRole={} targetRole={} dialogue={} passive={} pleasure={}",
                    actor->GetFormID(),
                    target ? target->GetFormID() : 0u,
                    phase ? phase : "unknown",
                    actorDiag ? 1 : 0,
                    targetDiag ? 1 : 0,
                    pairDiag ? 1 : 0,
                    TFD::CombatBehavior::DiagnosticRole(actorAsActor),
                    TFD::CombatBehavior::DiagnosticRole(target),
                    TFD::FlowController::IsDialogueContextActive() ? 1 : 0,
                    TFD::FlowController::IsPassiveHoldActive() ? 1 : 0,
                    TFD::FlowController::IsPleasureLockActive() ? 1 : 0);
            }

            static void UpdateCombat(RE::Character* actor)
            {
                if (actor && (TFD::HostilityController::IsSuppressed(actor) || IsReleaseGraceActor(actor))) {
                    auto* before = CurrentCombatTarget(actor);

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
                    auto* after = CurrentCombatTarget(actor);
                    LogTargetTransition("suppressed_or_release_grace", actor, before, after, after, after);
                    return;
                }

                if (TFD::Bleedout::DefeatGlue::IsObservedCombatCommitInProgress()) {
                    auto* before = CurrentCombatTarget(actor);
                    _UpdateCombat(actor);
                    auto* after = CurrentCombatTarget(actor);
                    LogTargetTransition("observed_bleedout_update", actor, before, before, after, after);
                    return;
                }

                auto* before = CurrentCombatTarget(actor);
                ClearInvalidCombatTarget(actor, "pre_clear");
                auto* afterPreClear = CurrentCombatTarget(actor);

                const bool preserveNormalPair = TFD::CombatBehavior::ShouldPreserveCombatTarget(static_cast<RE::Actor*>(actor), afterPreClear);

                _UpdateCombat(actor);
                auto* afterVanilla = CurrentCombatTarget(actor);

                if (preserveNormalPair && afterPreClear && afterVanilla != afterPreClear) {
                    auto* actorAsActor = static_cast<RE::Actor*>(actor);
                    actor->GetActorRuntimeData().currentCombatTarget = afterPreClear->GetHandle();
                    actor->SetBeenAttacked(true);
                    afterPreClear->SetBeenAttacked(true);
                    (void)actor->RequestDetectionLevel(afterPreClear, RE::DETECTION_PRIORITY::kCritical);
                    (void)afterPreClear->RequestDetectionLevel(actorAsActor, RE::DETECTION_PRIORITY::kCritical);
                    if (auto* process = RE::ProcessLists::GetSingleton()) {
                        process->ClearCachedFactionFightReactions();
                    }
                    spdlog::info(
                        "[TFD][CombatOwnership] restored ally target after vanilla actor={:08X} restored={:08X} vanillaAfter={:08X} actorRole={} targetRole={} actorCombat={}",
                        actor->GetFormID(),
                        afterPreClear->GetFormID(),
                        afterVanilla ? afterVanilla->GetFormID() : 0u,
                        TFD::CombatBehavior::DiagnosticRole(actorAsActor),
                        TFD::CombatBehavior::DiagnosticRole(afterPreClear),
                        actor->IsInCombat() ? 1 : 0);
                    afterVanilla = CurrentCombatTarget(actor);
                }

                ClearInvalidCombatTarget(actor, "post_clear");
                auto* afterPostClear = CurrentCombatTarget(actor);

                LogTargetTransition("normal_update", actor, before, afterPreClear, afterVanilla, afterPostClear);
                SyncPlayerCombatFlow(actor);
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
                if (viewer && target && player && IsPlayerSideCombatConflict(viewer, target, player)) {
                    detectVal = -1000;
                    return nullptr;
                }

                if (viewer && target && player && target == player && !IsPlayerSideActor(viewer, player)) {
                    TFD::Bleedout::DefeatGlue::NoteEnemyTargetingPlayer(viewer);
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
