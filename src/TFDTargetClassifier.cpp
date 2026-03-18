#include "TFDTargetClassifier.h"

#include <RE/B/BGSKeyword.h>
#include <RE/B/BGSListForm.h>
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESFile.h>
#include <RE/T/TESForm.h>

#include <cstdint>
#include <mutex>

namespace TFD::TargetClassifier
{
    namespace
    {
        constexpr float kMaxTameDistance = 768.0f;
        constexpr float kMaxTruceDistance = 1024.0f;

        constexpr const char* kPluginName = "TFDEngine.esp";

        // Local FormIDs from TFDEngine.esp
        constexpr std::uint32_t kDialogueCapableRacesLocalID = 0x00047618;
        constexpr std::uint32_t kDialogueCapableActorsLocalID = 0x00047619;
        constexpr std::uint32_t kSimpleCommandRacesLocalID = 0x0004761A;
        constexpr std::uint32_t kNonverbalIntelligentRacesLocalID = 0x0004761B;
        constexpr std::uint32_t kBeastRacesLocalID = 0x0004761C;

        // Skyrim.esm: ActorTypeNPC
        constexpr RE::FormID kActorTypeNpcFormID = 0x00013794;

        struct FormLists
        {
            RE::BGSListForm* dialogueCapableRaces{ nullptr };
            RE::BGSListForm* dialogueCapableActors{ nullptr };
            RE::BGSListForm* simpleCommandRaces{ nullptr };
            RE::BGSListForm* nonverbalIntelligentRaces{ nullptr };
            RE::BGSListForm* beastRaces{ nullptr };
            bool resolved{ false };
        };

        RE::TESNPC* GetActorBase(RE::Actor* actor)
        {
            if (!actor) {
                return nullptr;
            }

            return actor->GetActorBase();
        }

        RE::TESRace* GetRace(RE::Actor* actor)
        {
            auto* base = GetActorBase(actor);
            if (!base) {
                return nullptr;
            }

            return base->GetRace();
        }

        bool IsIgnoredActor(RE::Actor* actor)
        {
            return !actor || actor->IsPlayerRef();
        }

        RE::BGSKeyword* GetActorTypeNpcKeyword()
        {
            static RE::BGSKeyword* cached = nullptr;
            static bool tried = false;

            if (tried) {
                return cached;
            }

            tried = true;
            cached = RE::TESForm::LookupByID<RE::BGSKeyword>(kActorTypeNpcFormID);
            return cached;
        }

        bool HasSafeNpcKeyword(RE::Actor* actor)
        {
            if (!actor) {
                return false;
            }

            auto* kwNpc = GetActorTypeNpcKeyword();
            if (!kwNpc) {
                return false;
            }

            if (auto* race = GetRace(actor); race && race->HasKeyword(kwNpc)) {
                return true;
            }

            if (auto* base = GetActorBase(actor); base && base->HasKeyword(kwNpc)) {
                return true;
            }

            if (actor->HasKeyword(kwNpc)) {
                return true;
            }

            return false;
        }

        bool IsDistanceTooFarForTame(float distanceToPlayer)
        {
            return distanceToPlayer > kMaxTameDistance;
        }

        bool IsDistanceTooFarForTruce(float distanceToPlayer)
        {
            return distanceToPlayer > kMaxTruceDistance;
        }

        RE::FormID ResolveRuntimeFormID(std::uint32_t localFormID)
        {
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!dataHandler) {
                return 0;
            }

            auto* mod = dataHandler->LookupLoadedModByName(kPluginName);
            if (!mod) {
                return 0;
            }

            localFormID &= 0x00FFFFFF;

            if (mod->compileIndex != static_cast<std::uint32_t>(-1) &&
                mod->compileIndex != 0xFF) {
                return (static_cast<RE::FormID>(mod->compileIndex) << 24) | localFormID;
            }

            if (mod->smallFileCompileIndex != static_cast<std::uint32_t>(-1) &&
                mod->smallFileCompileIndex != 0xFFFF) {
                return 0xFE000000 |
                    ((static_cast<RE::FormID>(mod->smallFileCompileIndex) & 0xFFF) << 12) |
                    (localFormID & 0x00000FFF);
            }

            return 0;
        }

        template <class T>
        T* LookupOwnForm(std::uint32_t localFormID)
        {
            const auto runtimeFormID = ResolveRuntimeFormID(localFormID);
            if (!runtimeFormID) {
                return nullptr;
            }

            return RE::TESForm::LookupByID<T>(runtimeFormID);
        }

        bool ListHasForm(RE::BGSListForm* list, RE::TESForm* form)
        {
            return list && form && list->HasForm(form);
        }

        bool ListHasActor(RE::BGSListForm* list, RE::Actor* actor)
        {
            if (!list || !actor) {
                return false;
            }

            if (list->HasForm(actor)) {
                return true;
            }

            auto* base = GetActorBase(actor);
            if (base && list->HasForm(base)) {
                return true;
            }

            return false;
        }

        FormLists& GetFormLists()
        {
            static FormLists lists;
            static std::once_flag initFlag;

            std::call_once(initFlag, []() {
                lists.dialogueCapableRaces =
                    LookupOwnForm<RE::BGSListForm>(kDialogueCapableRacesLocalID);
                lists.dialogueCapableActors =
                    LookupOwnForm<RE::BGSListForm>(kDialogueCapableActorsLocalID);
                lists.simpleCommandRaces =
                    LookupOwnForm<RE::BGSListForm>(kSimpleCommandRacesLocalID);
                lists.nonverbalIntelligentRaces =
                    LookupOwnForm<RE::BGSListForm>(kNonverbalIntelligentRacesLocalID);
                lists.beastRaces =
                    LookupOwnForm<RE::BGSListForm>(kBeastRacesLocalID);

                lists.resolved = true;
            });

            return lists;
        }

        bool IsDialogueActorOverride(RE::Actor* actor)
        {
            if (!actor) {
                return false;
            }

            auto& lists = GetFormLists();
            return ListHasActor(lists.dialogueCapableActors, actor);
        }

        bool IsDialogueRaceListed(RE::Actor* actor)
        {
            if (!actor) {
                return false;
            }

            auto* race = GetRace(actor);
            if (!race) {
                return false;
            }

            auto& lists = GetFormLists();
            return ListHasForm(lists.dialogueCapableRaces, race);
        }

        bool HasExplicitDialogueCapability(RE::Actor* actor)
        {
            return IsDialogueActorOverride(actor) || IsDialogueRaceListed(actor);
        }

        CreatureClass GetBaseCreatureClassFromLists(RE::Actor* actor)
        {
            if (!actor) {
                return CreatureClass::None;
            }

            auto* race = GetRace(actor);
            if (!race) {
                return CreatureClass::None;
            }

            auto& lists = GetFormLists();

            if (ListHasForm(lists.simpleCommandRaces, race)) {
                return CreatureClass::SimpleCommand;
            }

            if (ListHasForm(lists.nonverbalIntelligentRaces, race)) {
                return CreatureClass::NonverbalIntelligent;
            }

            if (ListHasForm(lists.beastRaces, race)) {
                return CreatureClass::Beast;
            }

            return CreatureClass::None;
        }

        bool AllowsDialogueForClass(RE::Actor* actor, CreatureClass value)
        {
            if (!actor) {
                return false;
            }

            switch (value) {
            case CreatureClass::FullDialogue:
                return true;
            default:
                return false;
            }
        }
    }

    bool IsValidActor(RE::Actor* actor)
    {
        if (!actor) {
            return false;
        }

        if (actor->IsDead()) {
            return false;
        }

        if (IsIgnoredActor(actor)) {
            return false;
        }

        return true;
    }

    CreatureClass GetCreatureClass(RE::Actor* actor)
    {
        if (!IsValidActor(actor)) {
            return CreatureClass::None;
        }

        // Highest priority: explicit actor override.
        // Use this when a specific actor must always behave as full dialogue.
        if (IsDialogueActorOverride(actor)) {
            return CreatureClass::FullDialogue;
        }

        // Second layer: any safe ActorTypeNPC should always behave as full dialogue.
        // This keeps humanoid NPCs on the dialogue path even if they are not in explicit lists.
        if (HasSafeNpcKeyword(actor)) {
            return CreatureClass::FullDialogue;
        }

        // Third layer: explicit dialogue-capable races should stay on the full dialogue path
        // even if they also belong to one of the creature family lists.
        if (IsDialogueRaceListed(actor)) {
            return CreatureClass::FullDialogue;
        }

        // Fourth layer: creature family comes from creature race lists.
        const auto listedCreatureClass = GetBaseCreatureClassFromLists(actor);
        if (listedCreatureClass != CreatureClass::None) {
            return listedCreatureClass;
        }

        // Final fallback stays creature-like, but NPCs should have been promoted above.
        return CreatureClass::UnknownFallback;
    }

    bool IsNegotiable(RE::Actor* actor)
    {
        return GetCreatureClass(actor) == CreatureClass::FullDialogue;
    }

    bool IsCreature(RE::Actor* actor)
    {
        switch (GetCreatureClass(actor)) {
        case CreatureClass::SimpleCommand:
        case CreatureClass::NonverbalIntelligent:
        case CreatureClass::Beast:
        case CreatureClass::UnknownFallback:
            return true;
        default:
            return false;
        }
    }

    bool CanUseTruce(RE::Actor* actor)
    {
        if (!IsValidActor(actor)) {
            return false;
        }

        return GetCreatureClass(actor) != CreatureClass::None;
    }

    bool CanUseTame(RE::Actor* actor)
    {
        if (!IsValidActor(actor)) {
            return false;
        }

        return IsCreature(actor);
    }

    ClassifyResult ClassifyForHotkey(
        RE::Actor* player,
        RE::Actor* target,
        bool isCaptivePhase,
        bool targetInCombat,
        float distanceToPlayer)
    {
        ClassifyResult result{};

        if (!player || !target) {
            result.valid = false;
            result.rejectReason = RejectReason::InvalidActor;
            return result;
        }

        if (isCaptivePhase) {
            result.valid = false;
            result.rejectReason = RejectReason::CaptiveOnlyMode;
            return result;
        }

        if (!IsValidActor(target)) {
            result.valid = false;
            result.rejectReason = RejectReason::InvalidActor;
            return result;
        }

        result.creatureClass = GetCreatureClass(target);
        result.allowDialogue = AllowsDialogueForClass(target, result.creatureClass);

        if (result.creatureClass == CreatureClass::None) {
            result.kind = TargetKind::Ignore;
            result.intent = InteractionIntent::None;
            result.rejectReason = RejectReason::UnsafeState;
            result.valid = false;
            return result;
        }

        if (result.creatureClass == CreatureClass::FullDialogue) {
            if (IsDistanceTooFarForTruce(distanceToPlayer)) {
                result.valid = false;
                result.rejectReason = RejectReason::TooFar;
                return result;
            }

            result.kind = TargetKind::Negotiable;
            result.intent = InteractionIntent::Truce;
            result.rejectReason = RejectReason::None;
            result.valid = true;
            result.negotiable = true;
            result.tameable = false;
            result.allowDialogue = true;
            result.requiresPreCombat = false;
            result.allowsInCombat = true;
            return result;
        }

        if (IsDistanceTooFarForTame(distanceToPlayer)) {
            result.valid = false;
            result.rejectReason = RejectReason::TooFar;
            return result;
        }

        result.kind = TargetKind::Creature;
        result.intent = InteractionIntent::Tame;
        result.rejectReason = RejectReason::None;
        result.valid = true;
        result.negotiable = true;
        result.tameable = true;
        result.allowDialogue = AllowsDialogueForClass(target, result.creatureClass);
        result.requiresPreCombat = !targetInCombat;
        result.allowsInCombat = true;
        return result;
    }

    const char* ToString(TargetKind value)

    {
        switch (value) {
        case TargetKind::None:
            return "None";
        case TargetKind::Negotiable:
            return "Negotiable";
        case TargetKind::Creature:
            return "Creature";
        case TargetKind::Ignore:
            return "Ignore";
        default:
            return "Unknown";
        }
    }

    const char* ToString(InteractionIntent value)
    {
        switch (value) {
        case InteractionIntent::None:
            return "None";
        case InteractionIntent::Tame:
            return "Tame";
        case InteractionIntent::Truce:
            return "Truce";
        default:
            return "Unknown";
        }
    }

    const char* ToString(RejectReason value)
    {
        switch (value) {
        case RejectReason::None:
            return "None";
        case RejectReason::InvalidActor:
            return "InvalidActor";
        case RejectReason::NotNegotiable:
            return "NotNegotiable";
        case RejectReason::NotCreature:
            return "NotCreature";
        case RejectReason::TameRequiresPreCombat:
            return "TameRequiresPreCombat";
        case RejectReason::TooFar:
            return "TooFar";
        case RejectReason::UnsafeState:
            return "UnsafeState";
        case RejectReason::CaptiveOnlyMode:
            return "CaptiveOnlyMode";
        default:
            return "Unknown";
        }
    }

    const char* ToString(CreatureClass value)
    {
        switch (value) {
        case CreatureClass::None:
            return "None";
        case CreatureClass::FullDialogue:
            return "FullDialogue";
        case CreatureClass::SimpleCommand:
            return "SimpleCommand";
        case CreatureClass::NonverbalIntelligent:
            return "NonverbalIntelligent";
        case CreatureClass::Beast:
            return "Beast";
        case CreatureClass::UnknownFallback:
            return "UnknownFallback";
        default:
            return "Unknown";
        }
    }
}
