#include "TFDTargetClassifier.h"

#include <string_view>

namespace TFD::TargetClassifier
{
    namespace
    {
        constexpr float kMaxTameDistance = 768.0f;
        constexpr float kMaxTruceDistance = 1024.0f;

        bool ContainsInsensitive(std::string_view text, std::string_view needle)
        {
            if (needle.empty() || text.size() < needle.size()) {
                return false;
            }

            for (std::size_t i = 0; i + needle.size() <= text.size(); ++i) {
                bool match = true;
                for (std::size_t j = 0; j < needle.size(); ++j) {
                    char a = text[i + j];
                    char b = needle[j];

                    if (a >= 'A' && a <= 'Z') {
                        a = static_cast<char>(a - 'A' + 'a');
                    }
                    if (b >= 'A' && b <= 'Z') {
                        b = static_cast<char>(b - 'A' + 'a');
                    }

                    if (a != b) {
                        match = false;
                        break;
                    }
                }

                if (match) {
                    return true;
                }
            }

            return false;
        }

        bool RaceNameContains(RE::Actor* actor, std::string_view needle)
        {
            if (!actor) {
                return false;
            }

            auto* base = actor->GetActorBase();
            if (!base) {
                return false;
            }

            auto* race = base->GetRace();
            if (!race) {
                return false;
            }

            const char* raceName = race->GetName();
            if (!raceName) {
                return false;
            }

            return ContainsInsensitive(raceName, needle);
        }

        bool IsIgnoredActor(RE::Actor* actor)
        {
            if (!actor) {
                return true;
            }

            if (actor->IsPlayerRef()) {
                return true;
            }

            return false;
        }

        bool IsDragonActor(RE::Actor* actor)
        {
            return RaceNameContains(actor, "dragon");
        }

        bool IsGiantActor(RE::Actor* actor)
        {
            return RaceNameContains(actor, "giant");
        }

        bool IsHumanoidNegotiable(RE::Actor* actor)
        {
            if (!actor) {
                return false;
            }

            if (IsDragonActor(actor) || IsGiantActor(actor)) {
                return false;
            }

            const bool looksCreature =
                RaceNameContains(actor, "wolf") ||
                RaceNameContains(actor, "bear") ||
                RaceNameContains(actor, "sabre") ||
                RaceNameContains(actor, "troll") ||
                RaceNameContains(actor, "spider") ||
                RaceNameContains(actor, "chaurus") ||
                RaceNameContains(actor, "atronach") ||
                RaceNameContains(actor, "hound") ||
                RaceNameContains(actor, "skeever");

            return !looksCreature;
        }

        bool IsNonNegotiableCreature(RE::Actor* actor)
        {
            if (!actor) {
                return false;
            }

            if (IsDragonActor(actor) || IsGiantActor(actor) || IsHumanoidNegotiable(actor)) {
                return false;
            }

            return
                RaceNameContains(actor, "wolf") ||
                RaceNameContains(actor, "bear") ||
                RaceNameContains(actor, "sabre") ||
                RaceNameContains(actor, "troll") ||
                RaceNameContains(actor, "spider") ||
                RaceNameContains(actor, "chaurus") ||
                RaceNameContains(actor, "skeever") ||
                RaceNameContains(actor, "atronach") ||
                RaceNameContains(actor, "hound");
        }

        bool IsDistanceTooFarForTame(float distanceToPlayer)
        {
            return distanceToPlayer > kMaxTameDistance;
        }

        bool IsDistanceTooFarForTruce(float distanceToPlayer)
        {
            return distanceToPlayer > kMaxTruceDistance;
        }
    }

    bool IsValidActor(RE::Actor* actor)
    {
        if (!actor) {
            return false;
        }

        if (actor->IsPlayerRef()) {
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

    bool IsNegotiable(RE::Actor* actor)
    {
        if (!IsValidActor(actor)) {
            return false;
        }

        if (IsDragonActor(actor)) {
            return true;
        }

        if (IsGiantActor(actor)) {
            return true;
        }

        if (IsHumanoidNegotiable(actor)) {
            return true;
        }

        return false;
    }

    bool IsCreature(RE::Actor* actor)
    {
        if (!IsValidActor(actor)) {
            return false;
        }

        if (IsNegotiable(actor)) {
            return false;
        }

        if (IsNonNegotiableCreature(actor)) {
            return true;
        }

        return false;
    }

    bool CanUseTruce(RE::Actor* actor)
    {
        if (!IsValidActor(actor)) {
            return false;
        }

        if (!IsNegotiable(actor)) {
            return false;
        }

        return true;
    }

    bool CanUseTame(RE::Actor* actor)
    {
        if (!IsValidActor(actor)) {
            return false;
        }

        if (!IsCreature(actor)) {
            return false;
        }

        return true;
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

        if (IsNegotiable(target)) {
            if (!CanUseTruce(target)) {
                result.valid = false;
                result.rejectReason = RejectReason::NotNegotiable;
                return result;
            }

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

        if (IsCreature(target)) {
            if (!CanUseTame(target)) {
                result.valid = false;
                result.rejectReason = RejectReason::NotCreature;
                return result;
            }

            if (targetInCombat) {
                result.valid = false;
                result.rejectReason = RejectReason::TameRequiresPreCombat;
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
            result.negotiable = false;
            result.tameable = true;
            result.allowDialogue = false;
            result.requiresPreCombat = true;
            result.allowsInCombat = false;
            return result;
        }

        result.kind = TargetKind::Ignore;
        result.intent = InteractionIntent::None;
        result.rejectReason = RejectReason::UnsafeState;
        result.valid = false;
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
}