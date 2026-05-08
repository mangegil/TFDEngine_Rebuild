#include "TFDStatusHUD.h"

#include "TFDActor.h"
#include "TFDSettings.h"
#include "TFDTeammateManager.h"

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4099 4505 5054)
#endif
#include "SKSEMenuFramework.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <chrono>

#include <spdlog/spdlog.h>

namespace TFD::StatusHUD
{
    namespace
    {
        constexpr float kIconSize = 56.0f;
        constexpr float kWindowSize = 64.0f;
        constexpr float kTopMargin = 48.0f;
        constexpr float kLiveScanRadius = 4200.0f;
        constexpr auto kLiveScanInterval = std::chrono::milliseconds(300);
        constexpr auto kIncombatHoldDuration = std::chrono::milliseconds(1200);
        constexpr auto kPrecombatHoldDuration = std::chrono::milliseconds(1400);
        constexpr auto kVictoryHoldDuration = std::chrono::milliseconds(2200);

        struct TextureSlot
        {
            PlayerFlowState state{ PlayerFlowState::Neutral };
            const char* iconPath{ "" };
            ImGuiMCP::ImTextureID texture{ nullptr };
            bool attempted{ false };
        };

        static std::atomic_bool gEnabled{ true };
        static std::atomic_bool gHudRegistered{ false };
        static SKSEMenuFramework::Model::HudElement* gHudElement = nullptr;

        static std::optional<PlayerFlowState> gManualState{};
        static PlayerFlowState gLastLoggedState = PlayerFlowState::Neutral;
        static bool gHasLoggedState = false;

        static RE::TESGlobal* gCaptiveState = nullptr;
        static RE::TESGlobal* gDefeatState = nullptr;
        static RE::TESGlobal* gInCombatState = nullptr;
        static RE::TESGlobal* gVictoryState = nullptr;
        static RE::TESGlobal* gPreCombatState = nullptr;
        static bool gLoggedCaptiveState = false;
        static bool gLoggedDefeatState = false;
        static bool gLoggedInCombatState = false;
        static bool gLoggedVictoryState = false;
        static bool gLoggedPreCombatState = false;

        static PlayerFlowState gCachedLiveState = PlayerFlowState::Neutral;
        static bool gLiveScanValid = false;
        static std::chrono::steady_clock::time_point gLastLiveScan{};
        static std::chrono::steady_clock::time_point gLastIncombatSeen{};
        static std::chrono::steady_clock::time_point gLastPrecombatSeen{};
        static std::chrono::steady_clock::time_point gLastVictorySeen{};

        static std::array<TextureSlot, 6> gPlayerTextures{
            TextureSlot{ PlayerFlowState::Neutral, "Data/Interface/TFD/Icons/State_Neutral.png" },
            TextureSlot{ PlayerFlowState::Precombat, "Data/Interface/TFD/Icons/State_Precombat.png" },
            TextureSlot{ PlayerFlowState::Incombat, "Data/Interface/TFD/Icons/State_Incombat.png" },
            TextureSlot{ PlayerFlowState::Victory, "Data/Interface/TFD/Icons/State_Victory.png" },
            TextureSlot{ PlayerFlowState::Defeat, "Data/Interface/TFD/Icons/State_Defeat.png" },
            TextureSlot{ PlayerFlowState::Captive, "Data/Interface/TFD/Icons/State_Captive.png" }
        };

        static const char* StateName(PlayerFlowState state)
        {
            switch (state) {
            case PlayerFlowState::Neutral:
                return "Neutral";
            case PlayerFlowState::Precombat:
                return "Precombat";
            case PlayerFlowState::Incombat:
                return "Incombat";
            case PlayerFlowState::Victory:
                return "Victory";
            case PlayerFlowState::Defeat:
                return "Defeat";
            case PlayerFlowState::Captive:
                return "Captive";
            default:
                return "Unknown";
            }
        }

        static void ResolveGlobal(RE::TESGlobal*& global, bool& logged, const char* editorId)
        {
            if (global || !editorId || !editorId[0]) {
                return;
            }

            global = RE::TESForm::LookupByEditorID<RE::TESGlobal>(editorId);
            if (global && !logged) {
                logged = true;
                spdlog::info("[TFD][StatusHUD] {} resolved {:08X}", editorId, global->GetFormID());
            }
        }

        static void ResolveGlobals()
        {
            ResolveGlobal(gCaptiveState, gLoggedCaptiveState, "TFDCaptiveState");
            ResolveGlobal(gDefeatState, gLoggedDefeatState, "TFDDefeatState");
            ResolveGlobal(gInCombatState, gLoggedInCombatState, "TFDInCombatState");
            ResolveGlobal(gVictoryState, gLoggedVictoryState, "TFDVictoryState");
            ResolveGlobal(gPreCombatState, gLoggedPreCombatState, "TFDPreCombatState");
        }

        static int GetGlobalValueInt(RE::TESGlobal* global)
        {
            if (!global) {
                return 0;
            }

            return static_cast<int>(std::lround(global->value));
        }

        static bool IsPlayerInWorld()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return false;
            }

            return player->GetParentCell() != nullptr;
        }

        static bool ShouldRenderHud()
        {
            auto* ui = RE::UI::GetSingleton();
            if (ui && (ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME))) {
                return false;
            }

            return IsPlayerInWorld();
        }

        static bool IsPlayerSideTarget(RE::Actor* actor, RE::Actor* player)
        {
            if (!actor || !player) {
                return false;
            }

            if (actor == player) {
                return true;
            }

            return actor->IsPlayerTeammate() ||
                TFD::TeammateManager::IsActiveFollowerActor(actor) ||
                TFD::TeammateManager::IsTFDManagedTeammateActor(actor);
        }

        static bool HasLineOfSightBetween(RE::Actor* from, RE::TESObjectREFR* to)
        {
            if (!from || !to) {
                return false;
            }

            bool hasLOSData = false;
            return from->HasLineOfSight(to, hasLOSData);
        }

        static bool IsPlayerSideActorForHud(const TFD::Actor::ActorInfo& info, RE::Actor* actor)
        {
            if (!actor) {
                return false;
            }

            return info.playerSide ||
                actor->IsPlayerTeammate() ||
                TFD::TeammateManager::IsActiveFollowerActor(actor) ||
                TFD::TeammateManager::IsTFDManagedTeammateActor(actor);
        }

        static bool IsLikelyHostileEnemy(const TFD::Actor::ActorInfo& info, RE::Actor* actor, RE::Actor* player, bool hostileToPlayer, bool targetsPlayerSide)
        {
            if (!actor || !player || actor == player || IsPlayerSideActorForHud(info, actor)) {
                return false;
            }

            return hostileToPlayer || targetsPlayerSide || info.inCombat || info.isBattleParticipant;
        }

        static PlayerFlowState EvaluateLivePlayerState(RE::Actor* player)
        {
            if (!player || !player->GetParentCell()) {
                return PlayerFlowState::Neutral;
            }

            const auto now = std::chrono::steady_clock::now();
            if (gLiveScanValid && (now - gLastLiveScan) < kLiveScanInterval) {
                return gCachedLiveState;
            }

            gLastLiveScan = now;
            gLiveScanValid = true;

            TFD::Actor::ScanOptions options{};
            options.radius = kLiveScanRadius;
            options.npcOnly = false;

            const auto snapshot = TFD::Actor::BuildSnapshot(player, options);
            const auto playerFormID = player->GetFormID();
            const bool playerInCombat = player->IsInCombat();
            auto* playerTarget = TFD::Actor::GetCurrentTarget(player);

            bool hasActiveCombatHostile = false;
            bool hasMutualLosHostile = false;
            bool hasDownedHostile = false;

            for (const auto& info : snapshot.actors) {
                auto* actor = info.get();
                if (!actor || actor == player || actor->IsDead() || actor->IsDisabled()) {
                    continue;
                }

                if (IsPlayerSideActorForHud(info, actor)) {
                    continue;
                }

                auto* target = info.getCurrentTarget();
                const bool targetsPlayerSide =
                    info.currentTargetFormID == playerFormID ||
                    IsPlayerSideTarget(target, player);
                const bool hostileToPlayer = info.hostileToPlayer || actor->IsHostileToActor(player);

                // Authoritative defeated-enemy state must be checked before hostility.
                // The defeat monitor intentionally pacifies locked enemies, so the last
                // enemy downed by a teammate can stop being hostile even though the
                // situation is still a Victory state until that enemy dies or is converted.
                const bool defeatedKnocked = TFD::Actor::Ops::IsDefeatedEnemyKnocked(actor);
                if (defeatedKnocked) {
                    hasDownedHostile = true;
                    continue;
                }

                const bool likelyHostileEnemy = IsLikelyHostileEnemy(info, actor, player, hostileToPlayer, targetsPlayerSide);

                if (!likelyHostileEnemy) {
                    continue;
                }

                const bool downedByHealth = TFD::Actor::IsDownByHealthThreshold(actor, TFD::Settings::GetEnemyDownedThresholdPct());
                if (downedByHealth) {
                    hasDownedHostile = true;
                    continue;
                }

                if (!info.standing) {
                    continue;
                }

                const bool playerHasLosToActor = HasLineOfSightBetween(player, actor);
                const bool actorHasLosToPlayer = HasLineOfSightBetween(actor, player);
                const bool mutualLos = playerHasLosToActor && actorHasLosToPlayer;

                // Incombat is actor intent / chase / attack state. It must not require LOS,
                // because enemies can keep chasing after turning a corner.
                if (targetsPlayerSide ||
                    (playerInCombat && (playerTarget == actor || hostileToPlayer)) ||
                    (info.inCombat && hostileToPlayer && (mutualLos || info.dist <= 1800.0f))) {
                    hasActiveCombatHostile = true;
                    break;
                }

                // Precombat is only a visible mutual-warning state. If either side cannot
                // see the other, HUD should settle back to Neutral instead of flickering.
                if (hostileToPlayer && mutualLos) {
                    hasMutualLosHostile = true;
                }
            }

            if (hasActiveCombatHostile) {
                gLastIncombatSeen = now;
            }
            if (hasDownedHostile) {
                gLastVictorySeen = now;
            }
            if (hasMutualLosHostile) {
                gLastPrecombatSeen = now;
            }

            if ((now - gLastIncombatSeen) <= kIncombatHoldDuration) {
                gCachedLiveState = PlayerFlowState::Incombat;
            }
            else if ((now - gLastVictorySeen) <= kVictoryHoldDuration) {
                gCachedLiveState = PlayerFlowState::Victory;
            }
            else if ((now - gLastPrecombatSeen) <= kPrecombatHoldDuration) {
                gCachedLiveState = PlayerFlowState::Precombat;
            }
            else {
                gCachedLiveState = PlayerFlowState::Neutral;
            }

            return gCachedLiveState;
        }

        static PlayerFlowState EvaluatePlayerState()
        {
            if (gManualState.has_value()) {
                return *gManualState;
            }

            ResolveGlobals();

            auto* player = RE::PlayerCharacter::GetSingleton();

            // Main player flow priority:
            // Captive > Defeat > Incombat > Victory > Precombat > Neutral.
            // R92D keeps Precombat as a mutual-LOS warning state and makes Victory
            // follow authoritative defeated-enemy locks, including enemies downed by teammates.
            if (GetGlobalValueInt(gCaptiveState) > 0) {
                return PlayerFlowState::Captive;
            }

            if (GetGlobalValueInt(gDefeatState) >= 2) {
                return PlayerFlowState::Defeat;
            }

            const auto liveState = EvaluateLivePlayerState(player);

            if (GetGlobalValueInt(gInCombatState) > 0 || liveState == PlayerFlowState::Incombat) {
                return PlayerFlowState::Incombat;
            }

            if (GetGlobalValueInt(gVictoryState) >= 2 || liveState == PlayerFlowState::Victory) {
                return PlayerFlowState::Victory;
            }

            // Do not trust raw TFDPreCombatState alone for HUD. The visual Precombat icon
            // means: hostile is nearby and mutual line-of-sight exists.
            if (liveState == PlayerFlowState::Precombat) {
                return PlayerFlowState::Precombat;
            }

            return PlayerFlowState::Neutral;
        }

        static TextureSlot* FindTextureSlot(PlayerFlowState state)
        {
            for (auto& slot : gPlayerTextures) {
                if (slot.state == state) {
                    return &slot;
                }
            }

            return &gPlayerTextures[0];
        }

        static ImGuiMCP::ImTextureID GetTexture(PlayerFlowState state)
        {
            auto* slot = FindTextureSlot(state);
            if (!slot) {
                return nullptr;
            }

            if (!slot->attempted) {
                slot->attempted = true;
                slot->texture = SKSEMenuFramework::LoadTexture(slot->iconPath);
                if (slot->texture) {
                    spdlog::info("[TFD][StatusHUD] texture loaded state={} path={}", StateName(state), slot->iconPath);
                }
                else {
                    spdlog::warn("[TFD][StatusHUD] texture load failed state={} path={}", StateName(state), slot->iconPath);
                }
            }

            return slot->texture;
        }

        static void LogStateChange(PlayerFlowState state)
        {
            if (gHasLoggedState && gLastLoggedState == state) {
                return;
            }

            gHasLoggedState = true;
            gLastLoggedState = state;
            spdlog::info("[TFD][StatusHUD] player_state={}", StateName(state));
        }

        static void RenderPlayerStateIcon()
        {
            if (!gEnabled.load(std::memory_order_acquire)) {
                return;
            }

            if (!ShouldRenderHud()) {
                return;
            }

            const auto state = EvaluatePlayerState();
            LogStateChange(state);

            auto* viewport = ImGuiMCP::GetMainViewport();
            float centerX = 640.0f;
            float topY = kTopMargin;
            if (viewport) {
                centerX = viewport->Pos.x + (viewport->Size.x * 0.5f);
                topY = viewport->Pos.y + kTopMargin;
            }

            ImGuiMCP::SetNextWindowPos(ImGuiMCP::ImVec2(centerX, topY), ImGuiMCP::ImGuiCond_Always, ImGuiMCP::ImVec2(0.5f, 0.0f));
            ImGuiMCP::SetNextWindowSize(ImGuiMCP::ImVec2(kWindowSize, kWindowSize), ImGuiMCP::ImGuiCond_Always);

            constexpr ImGuiMCP::ImGuiWindowFlags kFlags =
                ImGuiMCP::ImGuiWindowFlags_NoDecoration |
                ImGuiMCP::ImGuiWindowFlags_NoInputs |
                ImGuiMCP::ImGuiWindowFlags_NoBackground |
                ImGuiMCP::ImGuiWindowFlags_NoSavedSettings |
                ImGuiMCP::ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiMCP::ImGuiWindowFlags_NoNav;

            bool open = true;
            if (ImGuiMCP::Begin("TFD Status HUD##PlayerFlow", &open, kFlags)) {
                if (auto texture = GetTexture(state)) {
                    ImGuiMCP::Image(texture, ImGuiMCP::ImVec2(kIconSize, kIconSize));
                }
                else {
                    ImGuiMCP::Text("TFD");
                    ImGuiMCP::Text("%s", StateName(state));
                }
            }
            ImGuiMCP::End();
        }
    }

    void Init()
    {
        if (gHudRegistered.load(std::memory_order_acquire)) {
            return;
        }

        if (!EnsureMenuFrameworkLoaded()) {
            spdlog::warn("[TFD][StatusHUD] SKSEMenuFramework not loaded; HUD not registered");
            return;
        }

        gHudElement = SKSEMenuFramework::AddHudElement(RenderPlayerStateIcon);
        if (!gHudElement) {
            spdlog::warn("[TFD][StatusHUD] AddHudElement returned null");
            return;
        }

        gHudRegistered.store(true, std::memory_order_release);
        spdlog::info("[TFD][StatusHUD] HUD element registered (R92D)");
    }

    void ResetForLoad()
    {
        gManualState.reset();
        gHasLoggedState = false;
        gLiveScanValid = false;
        gCachedLiveState = PlayerFlowState::Neutral;
        gLastIncombatSeen = {};
        gLastPrecombatSeen = {};
        gLastVictorySeen = {};
        ResolveGlobals();
    }

    void SetEnabled(bool enabled)
    {
        gEnabled.store(enabled, std::memory_order_release);
    }

    void SetManualPlayerState(PlayerFlowState state)
    {
        gManualState = state;
        gHasLoggedState = false;
    }

    void ClearManualPlayerState()
    {
        gManualState.reset();
        gHasLoggedState = false;
    }
}
