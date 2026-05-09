// src/MenuFramework.cpp
#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4099 4505 5054)
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <cmath>
#include <cctype>
#include <vector>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "TFDSettings.h"
#include "TFDLocation.h"
#include "TFDHostilityController.h"
#include "TFDPreCombatGreet.h"
#include "TFDInCombatGreet.h"
#include "TFDInCombat.h"
#include "TFDDefeatMonitor.h"
#include "TFDInteractionRouter.h"
#include "TFDTame.h"
#include "TFDFeedPopup.h"
#include "TFDStatusHUD.h"
#include "TFDActor.h"
#include "TFDTeammateManager.h"
#include "TFDFlowController.h"
#include "TFDCaptive.h"
#include "TFDRescue.h"
#include "TFDPleasureRuntime.h"
#include "TFDVictory.h"
#include "EditorIdCache.h"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "SKSEMenuFramework.h"

namespace TFD::DefeatMonitor
{
	bool IsDialogueCapableDefeatedEnemy(RE::Actor* actor);
}

namespace TFDMenu
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		static double NowSec()
		{
			static const auto t0 = Clock::now();
			return std::chrono::duration<double>(Clock::now() - t0).count();
		}

		static bool started = false;
		static bool menuRegistered = false;

		static std::atomic_bool inputSinkAdded{ false };
		static std::atomic_bool inputRetryThreadStarted{ false };

		static bool  uiEnabled = true;
		static float uiThreshold = 30.0f;
		static float uiPlayerGuardThreshold = 40.0f;
		static float uiPlayerGuardDamageScale = 0.10f;
		static float uiAllyThreshold = 20.0f;
		static float uiEnemyThreshold = 10.0f;
		static int   uiBleedSeconds = 10;
		static float uiScanRadius = 2500.0f;
		static float uiSweepRadius = 2500.0f;
		static bool  uiNpcOnly = true;

		static bool uiHotkeyEnabled = true;
		static int  uiHotkeyScanCode = 35;   // H default
		static int  uiHotkeyCooldownMs = 350;
		static bool uiHotkeyWave = true;

		static bool gCaptureHotkey = false;
		static Clock::time_point nextHotkey{};

		static Clock::time_point gNextTeammateManualDialogueOpen{};
		static std::unordered_map<RE::FormID, Clock::time_point> gTeammateManualDialogueCooldownUntil{};
		constexpr double kTeammateManualGlobalCooldownSec = 2.50;
		constexpr double kTeammateManualActorCooldownSec = 6.00;
		constexpr double kDownedTeammateRecoveryGlobalCooldownSec = 0.35;
		constexpr double kDownedTeammateRecoveryActorCooldownSec = 0.85;
		constexpr double kDownedTeammateRecoveryHoldSec = 10.0;

		static Clock::time_point gNextVictoryManualDialogueOpen{};
		static std::unordered_map<RE::FormID, Clock::time_point> gVictoryManualDialogueCooldownUntil{};
		constexpr double kVictoryManualGlobalCooldownSec = 0.65;
		constexpr double kVictoryManualActorCooldownSec = 1.50;
		constexpr double kVictoryDialogueReadyHoldSec = 4.00;

		static std::vector<TFD::Tame::ActiveSnapshot> gCreatureTeammateMenuRows{};
		static double gCreatureTeammateMenuRefreshRealSec = 0.0;
		static double gCreatureTeammateMenuRefreshGameDays = 0.0;
		static double gCreatureTeammateMenuLastRenderSec = -1000.0;
		static RE::FormID gCreatureTeammateFeedSessionId = 0;
		static RE::FormID gCreatureTeammateReleaseConfirmSessionId = 0;

		static RE::TESGlobal* gBossContainerMarkerState = nullptr;
		static RE::TESGlobal* gBossMarkerState = nullptr;
		static RE::TESGlobal* gCaptiveMarkerState = nullptr;
		static RE::TESGlobal* gCaptiveState = nullptr;
		static RE::TESGlobal* gContainerMarkerState = nullptr;
		static RE::TESGlobal* gDefeatState = nullptr;
		static RE::TESGlobal* gDialogueState = nullptr;
		static RE::TESGlobal* gEscapeRouteState = nullptr;
		static RE::TESGlobal* gEnemyFactionState = nullptr;
		static RE::TESGlobal* gEnemyRaceState = nullptr;
		static RE::TESGlobal* gHostileState = nullptr;
		static RE::TESGlobal* gInCombatState = nullptr;
		static RE::TESGlobal* gInteractionState = nullptr;
		static RE::TESGlobal* gJoinEnemyState = nullptr;
		static RE::TESGlobal* gLeftForDeadState = nullptr;
		static RE::TESGlobal* gPleasureState = nullptr;
		static RE::TESGlobal* gPreCombatState = nullptr;
		static RE::TESGlobal* gRecoveryState = nullptr;
		static RE::TESGlobal* gRescueMarkerState = nullptr;
		static RE::TESGlobal* gRescueState = nullptr;
		static RE::TESGlobal* gTeammateState = nullptr;
		static RE::TESGlobal* gVictoryState = nullptr;
		static bool gLoggedBossContainerMarkerStateFound = false;
		static bool gLoggedBossMarkerStateFound = false;
		static bool gLoggedCaptiveMarkerStateFound = false;
		static bool gLoggedCaptiveStateFound = false;
		static bool gLoggedContainerMarkerStateFound = false;
		static bool gLoggedDefeatStateFound = false;
		static bool gLoggedDialogueStateFound = false;
		static bool gLoggedEscapeRouteStateFound = false;
		static bool gLoggedEnemyFactionStateFound = false;
		static bool gLoggedEnemyRaceStateFound = false;
		static bool gLoggedHostileStateFound = false;
		static bool gLoggedInCombatStateFound = false;
		static bool gLoggedInteractionStateFound = false;
		static bool gLoggedJoinEnemyStateFound = false;
		static bool gLoggedLeftForDeadStateFound = false;
		static bool gLoggedPleasureStateFound = false;
		static bool gLoggedPreCombatStateFound = false;
		static bool gLoggedRecoveryStateFound = false;
		static bool gLoggedRescueMarkerStateFound = false;
		static bool gLoggedRescueStateFound = false;
		static bool gLoggedTeammateStateFound = false;
		static bool gLoggedVictoryStateFound = false;

		static void ResolveGlobal(RE::TESGlobal*& global, bool& logged, const char* editorId)
		{
			if (global || !editorId || !editorId[0]) {
				return;
			}

			global = RE::TESForm::LookupByEditorID<RE::TESGlobal>(editorId);
			if (global && !logged) {
				logged = true;
				spdlog::info("[TFD][Menu] {} resolved {:08X}", editorId, global->GetFormID());
			}
		}

		static void ResolveGlobals();
		static int GetGlobalValueInt(RE::TESGlobal* g);

		static RE::TESTopicInfo* ResolveTeammateGreetTopicInfo()
		{
			static RE::TESTopicInfo* info = nullptr;
			static bool attempted = false;

			if (!attempted) {
				attempted = true;

				// R47: Actor::SetDialogueWithPlayer expects a TESTopicInfo, not a TESTopic.
				// This is the INFO record behind the CK fragment TFD_TIF__05195939
				// under TFDDialogueTeammateGreet. Use plugin-local ID so runtime
				// load order does not matter.
				constexpr RE::FormID kTeammateGreetInfoLocalFormID = 0x00195939;
				constexpr std::string_view kPluginName{ "TFDEngine.esp" };

				if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
					info = dataHandler->LookupForm<RE::TESTopicInfo>(kTeammateGreetInfoLocalFormID, kPluginName);
				}

				if (info) {
					spdlog::info("[TFD][Menu] TFDDialogueTeammateGreet INFO resolved {:08X} local={:06X}",
						info->GetFormID(),
						kTeammateGreetInfoLocalFormID);
				}
				else {
					spdlog::warn("[TFD][Menu] TFDDialogueTeammateGreet INFO {:06X} not found in {}; teammate hard dialogue will fall back to default topic selection",
						kTeammateGreetInfoLocalFormID,
						kPluginName);
				}
			}

			return info;
		}



		static RE::TESTopicInfo* ResolveVictoryGreetTopicInfo()
		{
			static RE::TESTopicInfo* info = nullptr;
			static bool attempted = false;

			if (!attempted) {
				attempted = true;

				// R93O: Victory after a real PreCombat dialogue can leave vanilla topic
				// selection on the old dialogue route even when SetDialogueWithPlayer()
				// returns true. Use the explicit INFO behind TFD_TIF__05195937
				// so the Victory fragment always fires like direct Victory does.
				constexpr RE::FormID kVictoryGreetInfoLocalFormID = 0x00195937;
				constexpr std::string_view kPluginName{ "TFDEngine.esp" };

				if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
					info = dataHandler->LookupForm<RE::TESTopicInfo>(kVictoryGreetInfoLocalFormID, kPluginName);
				}

				if (info) {
					spdlog::info("[TFD][Menu] TFDDialogueVictoryGreet INFO resolved {:08X} local={:06X}",
						info->GetFormID(),
						kVictoryGreetInfoLocalFormID);
				}
				else {
					spdlog::warn("[TFD][Menu] TFDDialogueVictoryGreet INFO {:06X} not found in {}; victory hard dialogue will fall back to default topic selection",
						kVictoryGreetInfoLocalFormID,
						kPluginName);
				}
			}

			return info;
		}

		
			static RE::TESTopicInfo* ResolveInCombatGreetTopicInfo()
			{
				static RE::TESTopicInfo* info = nullptr;
				static bool attempted = false;

				if (!attempted) {
					attempted = true;

					// R93U: Manual activation of a suppressed InCombat truce actor must
					// not fall through to vanilla activation. Use the explicit INFO
					// behind TFD_TIF__0506FEA8, matching the R93P router opener.
					constexpr RE::FormID kInCombatGreetInfoLocalFormID = 0x0006FEA8;
					constexpr std::string_view kPluginName{ "TFDEngine.esp" };

					if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
						info = dataHandler->LookupForm<RE::TESTopicInfo>(kInCombatGreetInfoLocalFormID, kPluginName);
					}

					if (info) {
						spdlog::info("[TFD][Menu][R93U] TFDDialogueInCombatGreet INFO resolved {:08X} local={:06X}",
							info->GetFormID(),
							kInCombatGreetInfoLocalFormID);
					}
					else {
						spdlog::warn("[TFD][Menu][R93U] TFDDialogueInCombatGreet INFO {:06X} not found in {}; suppressed actor activation will be blocked",
							kInCombatGreetInfoLocalFormID,
							kPluginName);
					}
				}

				return info;
			}

			static bool IsInCombatTruceActivationActor(RE::Actor* actor)
			{
				if (!actor || actor == RE::PlayerCharacter::GetSingleton()) {
					return false;
				}

				const auto actorId = actor->GetFormID();
				const bool activePrimary = TFD::InCombat::IsActive() &&
					TFD::InCombat::GetPrimaryActorFormID() == actorId;
				const bool truceInCombat = TFD::HostilityController::GetMode(actor) == TFD::HostilityController::Mode::TruceInCombat;
				return activePrimary || truceInCombat;
			}

			static bool OpenInCombatTruceDialogueFromActivation(RE::Actor* actor, const char* reason)
			{
				if (!IsInCombatTruceActivationActor(actor)) {
					return false;
				}

				const auto actorFormID = actor ? actor->GetFormID() : 0u;
				const auto activePrimaryFormID = TFD::InCombat::IsActive() ? TFD::InCombat::GetPrimaryActorFormID() : 0u;
				const bool isActivePrimary = actorFormID != 0 && activePrimaryFormID != 0 && actorFormID == activePrimaryFormID;
				if (!isActivePrimary && TFD::HostilityController::GetMode(actor) == TFD::HostilityController::Mode::TruceInCombat) {
					// R94F: Suppressed crowd/ambient actors must not open the root InCombat
					// dialogue by manual activation. R94E logs showed the player could open
					// flow=10 on a non-primary crowd actor, causing SystemEvent speaker
					// mismatch while the real session speaker kept following forever.
					actor->SetDialogueWithPlayer(false, false, nullptr);
					spdlog::info(
						"[TFD][Menu][R94F] activate blocked non-primary incombat truce actor={:08X} primary={:08X} reason={} action=block_without_open",
						actorFormID,
						activePrimaryFormID,
						reason ? reason : "incombat_activation");
					return true;
				}

				const auto flowSnapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
				const bool pleasureRuntimeActive = TFD::PleasureRuntime::IsActive();
				const bool pleasureRuntimeBlocking = TFD::PleasureRuntime::IsBlocking();
				const bool inCombatPleasureSub =
					flowSnapshot.sub == TFD::FlowController::SubFlow::InCombatPleasure ||
					flowSnapshot.sub == TFD::FlowController::SubFlow::InCombatAfterPleasure;
				const bool flowHandoffHold = TFD::HostilityController::IsFlowHandoffHoldActive(actor);
				if (pleasureRuntimeActive || pleasureRuntimeBlocking || inCombatPleasureSub || flowHandoffHold) {
					actor->SetDialogueWithPlayer(false, false, nullptr);
					spdlog::info(
						"[TFD][Menu][R94B] activate blocked incombat truce during pleasure actor={:08X} reason={} phase={} source={} flowSub={} runtimeActive={} blocking={} handoff={} action=block_without_open",
						actor->GetFormID(),
						reason ? reason : "incombat_activation",
						TFD::PleasureRuntime::GetPhaseName(),
						TFD::PleasureRuntime::GetSourceContextName(),
						TFD::FlowController::Controller::ToString(flowSnapshot.sub),
						pleasureRuntimeActive ? 1 : 0,
						pleasureRuntimeBlocking ? 1 : 0,
						flowHandoffHold ? 1 : 0);
					return true;
				}

				if (!actor->IsAIEnabled()) {
					actor->EnableAI(true);
				}
				actor->AllowPCDialogue(true);

				if (auto* process = RE::ProcessLists::GetSingleton()) {
					process->StopCombatAndAlarmOnActor(actor, false);
				}
				actor->EvaluatePackage(false, true);
				actor->EvaluatePackage(true, true);

				auto* inCombatGreetInfo = ResolveInCombatGreetTopicInfo();
				actor->SetDialogueWithPlayer(false, false, nullptr);
				const bool opened = inCombatGreetInfo ? actor->SetDialogueWithPlayer(true, true, inCombatGreetInfo) : false;

				spdlog::info("[TFD][Menu][R93U] activate intercepted incombat truce actor={:08X} opened={} reason={} topicInfo={:08X} explicit={} suppressed={} primary={} action=block_vanilla",
					actor->GetFormID(),
					opened ? 1 : 0,
					reason ? reason : "incombat_activation",
					inCombatGreetInfo ? inCombatGreetInfo->GetFormID() : 0u,
					inCombatGreetInfo ? 1 : 0,
					TFD::HostilityController::IsSuppressed(actor) ? 1 : 0,
					(TFD::InCombat::IsActive() && TFD::InCombat::GetPrimaryActorFormID() == actor->GetFormID()) ? 1 : 0);

				return opened;
			}

static RE::TESObjectREFR* GetCrosshairTargetRef()
		{
			auto* pickData = RE::CrosshairPickData::GetSingleton();
			if (!pickData) {
				return nullptr;
			}

			// Prefer the actual activation target. If the crosshair is on a chest,
			// door, container, harvestable, or other non-actor reference, TFD must
			// not steal the E key just because a teammate stands near the player.
			if (auto* ref = pickData->target.get().get()) {
				return ref;
			}

			// Some actor picks are exposed through targetActor when target is empty.
			if (auto* ref = pickData->targetActor.get().get()) {
				return ref;
			}

			return nullptr;
		}

		static Clock::duration SecondsToClockDuration(double seconds)
		{
			return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>((std::max)(0.0, seconds)));
		}

		static void PruneTeammateManualDialogueCooldowns(Clock::time_point now)
		{
			for (auto it = gTeammateManualDialogueCooldownUntil.begin(); it != gTeammateManualDialogueCooldownUntil.end();) {
				if (now >= it->second) {
					it = gTeammateManualDialogueCooldownUntil.erase(it);
				}
				else {
					++it;
				}
			}
		}

		static bool IsTeammateManualDialogueBlocked(RE::Actor* actor, Clock::time_point now, const char*& outReason, double& outRemainingSec, bool allowDownedRecoveryRetry = false)
		{
			outReason = "none";
			outRemainingSec = 0.0;

			if (!actor) {
				outReason = "no_actor";
				return true;
			}

			ResolveGlobals();

			const bool downedRecoveryHold = allowDownedRecoveryRetry && TFD::TeammateManager::IsDownedTeammateRecoveryDialogueHoldActor(actor);

			if (TFD::PleasureRuntime::IsActive() || TFD::PleasureRuntime::IsBlocking()) {
				outReason = TFD::PleasureRuntime::GetPhaseName();
				return true;
			}

			if (TFD::FlowController::IsDialogueContextActive()) {
				outReason = TFD::FlowController::GetDialogueContextName();
				return true;
			}

			if (GetGlobalValueInt(gDialogueState) == 1) {
				if (!downedRecoveryHold) {
					outReason = "dialogue_state_in";
					return true;
				}
				spdlog::info("[TFD][Menu] downed teammate recovery retry bypassed stale dialogue state actor={:08X}", actor->GetFormID());
			}

			if (GetGlobalValueInt(gPleasureState) != 0) {
				outReason = "pleasure_state_active";
				return true;
			}

			if (now < gNextTeammateManualDialogueOpen) {
				if (!downedRecoveryHold) {
					outReason = "global_cooldown";
					outRemainingSec = std::chrono::duration<double>(gNextTeammateManualDialogueOpen - now).count();
					return true;
				}
				spdlog::info("[TFD][Menu] downed teammate recovery retry bypassed global cooldown actor={:08X}", actor->GetFormID());
			}

			PruneTeammateManualDialogueCooldowns(now);

			const auto formID = actor->GetFormID();
			if (auto it = gTeammateManualDialogueCooldownUntil.find(formID); it != gTeammateManualDialogueCooldownUntil.end()) {
				if (now < it->second) {
					if (!downedRecoveryHold) {
						outReason = "actor_cooldown";
						outRemainingSec = std::chrono::duration<double>(it->second - now).count();
						return true;
					}
					spdlog::info("[TFD][Menu] downed teammate recovery retry bypassed actor cooldown actor={:08X}", actor->GetFormID());
					gTeammateManualDialogueCooldownUntil.erase(it);
				}
				else {
					gTeammateManualDialogueCooldownUntil.erase(it);
				}
			}

			return false;
		}

		static void ArmTeammateManualDialogueCooldown(RE::Actor* actor, Clock::time_point now, const char* reason, double globalSeconds = kTeammateManualGlobalCooldownSec, double actorSeconds = kTeammateManualActorCooldownSec)
		{
			gNextTeammateManualDialogueOpen = now + SecondsToClockDuration(globalSeconds);

			if (actor) {
				const auto actorUntil = now + SecondsToClockDuration(actorSeconds);
				gTeammateManualDialogueCooldownUntil.insert_or_assign(actor->GetFormID(), actorUntil);

				spdlog::info(
					"[TFD][Menu] teammate manual dialogue cooldown armed actor={:08X} global={:.2f}s actor={:.2f}s reason={}",
					actor->GetFormID(),
					globalSeconds,
					actorSeconds,
					reason ? reason : "unknown");
			}
		}

		static void PruneVictoryManualDialogueCooldowns(Clock::time_point now)
		{
			for (auto it = gVictoryManualDialogueCooldownUntil.begin(); it != gVictoryManualDialogueCooldownUntil.end();) {
				if (now >= it->second) {
					it = gVictoryManualDialogueCooldownUntil.erase(it);
				}
				else {
					++it;
				}
			}
		}

		static bool IsVictoryManualDialogueBlocked(RE::Actor* actor, Clock::time_point now, const char*& outReason, double& outRemainingSec)
		{
			outReason = "none";
			outRemainingSec = 0.0;

			if (!actor) {
				outReason = "no_actor";
				return true;
			}

			if (now < gNextVictoryManualDialogueOpen) {
				outReason = "global_cooldown";
				outRemainingSec = std::chrono::duration<double>(gNextVictoryManualDialogueOpen - now).count();
				return true;
			}

			PruneVictoryManualDialogueCooldowns(now);

			const auto formID = actor->GetFormID();
			if (auto it = gVictoryManualDialogueCooldownUntil.find(formID); it != gVictoryManualDialogueCooldownUntil.end()) {
				if (now < it->second) {
					outReason = "actor_cooldown";
					outRemainingSec = std::chrono::duration<double>(it->second - now).count();
					return true;
				}
				gVictoryManualDialogueCooldownUntil.erase(it);
			}

			return false;
		}

		static void ArmVictoryManualDialogueCooldown(RE::Actor* actor, Clock::time_point now, const char* reason)
		{
			gNextVictoryManualDialogueOpen = now + SecondsToClockDuration(kVictoryManualGlobalCooldownSec);

			if (actor) {
				const auto actorUntil = now + SecondsToClockDuration(kVictoryManualActorCooldownSec);
				gVictoryManualDialogueCooldownUntil.insert_or_assign(actor->GetFormID(), actorUntil);

				spdlog::info(
					"[TFD][Menu] victory manual dialogue cooldown armed actor={:08X} global={:.2f}s actor={:.2f}s reason={}",
					actor->GetFormID(),
					kVictoryManualGlobalCooldownSec,
					kVictoryManualActorCooldownSec,
					reason ? reason : "unknown");
			}
		}

		static void ResolveGlobals()
		{
			ResolveGlobal(gBossContainerMarkerState, gLoggedBossContainerMarkerStateFound, "TFDBossContainerMarkerState");
			ResolveGlobal(gBossMarkerState, gLoggedBossMarkerStateFound, "TFDBossMarkerState");
			ResolveGlobal(gCaptiveMarkerState, gLoggedCaptiveMarkerStateFound, "TFDCaptiveMarkerState");
			ResolveGlobal(gCaptiveState, gLoggedCaptiveStateFound, "TFDCaptiveState");
			ResolveGlobal(gContainerMarkerState, gLoggedContainerMarkerStateFound, "TFDContainerMarkerState");
			ResolveGlobal(gDefeatState, gLoggedDefeatStateFound, "TFDDefeatState");
			ResolveGlobal(gDialogueState, gLoggedDialogueStateFound, "TFDDialogueState");
			ResolveGlobal(gEscapeRouteState, gLoggedEscapeRouteStateFound, "TFDEscapeRouteState");
			ResolveGlobal(gEnemyFactionState, gLoggedEnemyFactionStateFound, "TFDEnemyFactionState");
			ResolveGlobal(gEnemyRaceState, gLoggedEnemyRaceStateFound, "TFDEnemyRaceState");
			ResolveGlobal(gHostileState, gLoggedHostileStateFound, "TFDHostileState");
			ResolveGlobal(gInCombatState, gLoggedInCombatStateFound, "TFDInCombatState");
			ResolveGlobal(gInteractionState, gLoggedInteractionStateFound, "TFDInteractionState");
			ResolveGlobal(gJoinEnemyState, gLoggedJoinEnemyStateFound, "TFDJoinEnemyState");
			ResolveGlobal(gLeftForDeadState, gLoggedLeftForDeadStateFound, "TFDLeftForDeadState");
			ResolveGlobal(gPleasureState, gLoggedPleasureStateFound, "TFDPleasureState");
			ResolveGlobal(gPreCombatState, gLoggedPreCombatStateFound, "TFDPreCombatState");
			ResolveGlobal(gRecoveryState, gLoggedRecoveryStateFound, "TFDRecoveryState");
			ResolveGlobal(gRescueMarkerState, gLoggedRescueMarkerStateFound, "TFDRescueMarkerState");
			ResolveGlobal(gRescueState, gLoggedRescueStateFound, "TFDRescueState");
			ResolveGlobal(gTeammateState, gLoggedTeammateStateFound, "TFDTeammateState");
			ResolveGlobal(gVictoryState, gLoggedVictoryStateFound, "TFDVictoryState");
		}

		static float GetGlobalValue(RE::TESGlobal* g)
		{
			return g ? g->value : 0.0f;
		}

		static int GetGlobalValueInt(RE::TESGlobal* g)
		{
			return static_cast<int>(std::lround(GetGlobalValue(g)));
		}

		static void SetGlobalInt(RE::TESGlobal* g, int value)
		{
			if (g) {
				g->value = static_cast<float>(value);
			}
		}

		static void SetInteractionStateValue(int value)
		{
			TFD::InteractionRouter::SetInteractionStateValue(value);
		}

		static void ClearInteractionStateValue()
		{
			TFD::InteractionRouter::ClearInteractionStateValue();
		}

		static const char* DecodeAvailabilityState(int value)
		{
			switch (value) {
			case 0:
				return "Unavailable";
			case 1:
				return "Available";
			default:
				return "Custom";
			}
		}

		static const char* DecodeCaptiveState(int value)
		{
			switch (value) {
			case 0:
				return "Free";
			case 1:
				return "Kidnapped";
			case 2:
				return "Escape Phase";
			default:
				return "Custom";
			}
		}

		static const char* DecodeDefeatState(int value)
		{
			switch (value) {
			case 0:
				return "Not in Combat";
			case 1:
				return "No";
			case 2:
				return "Yes";
			default:
				return "Custom";
			}
		}

		static const char* DecodeDialogueState(int value)
		{
			switch (value) {
			case 0:
				return "Out";
			case 1:
				return "In";
			default:
				return "Custom";
			}
		}

		static const char* DecodeEscapeRouteState(int value)
		{
			switch (value) {
			case 0:
				return "Unavailable";
			case 1:
				return "Lockpicking";
			case 2:
				return "Get Away";
			default:
				return "Custom";
			}
		}

		static const char* DecodeEnemyFactionState(int value)
		{
			switch (value) {
			case 0:
				return "None";
			case 1:
				return "Single/Same";
			case 2:
				return "Mixed";
			default:
				return "Custom";
			}
		}

		static const char* DecodeEnemyRaceState(int value)
		{
			switch (value) {
			case 0:
				return "None";
			case 1:
				return "Single/Same";
			case 2:
				return "Mixed";
			default:
				return "Custom";
			}
		}

		static const char* DecodeHostileState(int value)
		{
			switch (value) {
			case 0:
				return "None";
			case 1:
				return "Duel";
			case 2:
				return "War";
			default:
				return "Custom";
			}
		}

		static const char* DecodeInCombatState(int value)
		{
			switch (value) {
			case 0:
				return "No";
			case 1:
				return "Yes";
			default:
				return "Custom";
			}
		}

		static const char* DecodeInteractionState(int value)
		{
			switch (value) {
			case 0:
				return "None";
			case 1:
				return "PreCombat";
			case 2:
				return "InCombat";
			case 3:
				return "Tame";
			case 4:
				return "Call Captor";
			case 5:
				return "Bleedout";
			case 6:
				return "Escape";
			default:
				return "Custom";
			}
		}

		static const char* DecodeJoinEnemyState(int value)
		{
			switch (value) {
			case 0:
				return "Unavailable";
			case 1:
				return "In Faction";
			case 2:
				return "Multiple Factions";
			default:
				return "Custom";
			}
		}

		static const char* DecodeLeftForDeadState(int value)
		{
			switch (value) {
			case 0:
				return "No";
			case 1:
				return "Yes";
			default:
				return "Custom";
			}
		}

		static const char* DecodeRecoveryState(int value)
		{
			switch (value) {
			case 0:
				return "No Potion";
			case 1:
				return "Has Potion";
			default:
				return "Custom";
			}
		}

		static const char* DecodeRescueState(int value)
		{
			switch (value) {
			case 0:
				return "No";
			case 1:
				return "Yes";
			default:
				return "Custom";
			}
		}

		static const char* DecodePleasureState(int value)
		{
			switch (value) {
			case 0:
				return "Unavailable";
			case 1:
				return "Pleasure";
			case 2:
				return "AfterPleasure";
			default:
				return "Custom";
			}
		}

		static const char* DecodePreCombatState(int value)
		{
			switch (value) {
			case 0:
				return "No";
			case 1:
				return "Yes";
			default:
				return "Custom";
			}
		}

		static const char* DecodeTeammateState(int value)
		{
			switch (value) {
			case 0:
				return "Unavailable";
			case 1:
				return "Has Teammate";
			case 2:
				return "Has Teammates";
			default:
				return "Custom";
			}
		}

		static const char* DecodeVictoryState(int value)
		{
			switch (value) {
			case 0:
				return "Neutral";
			case 1:
				return "No";
			case 2:
				return "Yes";
			default:
				return "Custom";
			}
		}

		static void RenderGlobalStateLine(const char* label, RE::TESGlobal* global, const char* decoded = nullptr)
		{
			if (!label) {
				return;
			}

			if (!global) {
				ImGuiMCP::Text("%s: <missing>", label);
				return;
			}

			if (decoded && decoded[0]) {
				ImGuiMCP::Text("%s: %s", label, decoded);
			}
			else {
				ImGuiMCP::Text("%s: %.0f", label, GetGlobalValue(global));
			}
		}

		static void DumpGlobalStateToLog()
		{
			ResolveGlobals();
			spdlog::info("[TFD][Menu][GlobalState] TFDBossContainerMarkerState={} ({})", GetGlobalValueInt(gBossContainerMarkerState), DecodeAvailabilityState(GetGlobalValueInt(gBossContainerMarkerState)));
			spdlog::info("[TFD][Menu][GlobalState] TFDBossMarkerState={} ({})", GetGlobalValueInt(gBossMarkerState), DecodeAvailabilityState(GetGlobalValueInt(gBossMarkerState)));
			spdlog::info("[TFD][Menu][GlobalState] TFDCaptiveMarkerState={} ({})", GetGlobalValueInt(gCaptiveMarkerState), DecodeAvailabilityState(GetGlobalValueInt(gCaptiveMarkerState)));
			spdlog::info("[TFD][Menu][GlobalState] TFDCaptiveState={} ({})", GetGlobalValueInt(gCaptiveState), DecodeCaptiveState(GetGlobalValueInt(gCaptiveState)));
			spdlog::info("[TFD][Menu][GlobalState] TFDContainerMarkerState={} ({})", GetGlobalValueInt(gContainerMarkerState), DecodeAvailabilityState(GetGlobalValueInt(gContainerMarkerState)));
			spdlog::info("[TFD][Menu][GlobalState] TFDDefeatState={} ({})", GetGlobalValueInt(gDefeatState), DecodeDefeatState(GetGlobalValueInt(gDefeatState)));
			spdlog::info("[TFD][Menu][GlobalState] TFDDialogueState={} ({})", GetGlobalValueInt(gDialogueState), DecodeDialogueState(GetGlobalValueInt(gDialogueState)));
			spdlog::info("[TFD][Menu][GlobalState] TFDEscapeRouteState={} ({})", GetGlobalValueInt(gEscapeRouteState), DecodeEscapeRouteState(GetGlobalValueInt(gEscapeRouteState)));
			spdlog::info("[TFD][Menu][GlobalState] TFDEnemyFactionState={} ({})", GetGlobalValueInt(gEnemyFactionState), DecodeEnemyFactionState(GetGlobalValueInt(gEnemyFactionState)));
			spdlog::info("[TFD][Menu][GlobalState] TFDEnemyRaceState={} ({})", GetGlobalValueInt(gEnemyRaceState), DecodeEnemyRaceState(GetGlobalValueInt(gEnemyRaceState)));
			spdlog::info("[TFD][Menu][GlobalState] TFDHostileState={} ({})", GetGlobalValueInt(gHostileState), DecodeHostileState(GetGlobalValueInt(gHostileState)));
			spdlog::info("[TFD][Menu][GlobalState] TFDInCombatState={} ({})", GetGlobalValueInt(gInCombatState), DecodeInCombatState(GetGlobalValueInt(gInCombatState)));
			spdlog::info("[TFD][Menu][GlobalState] TFDInteractionState={} ({})", GetGlobalValueInt(gInteractionState), DecodeInteractionState(GetGlobalValueInt(gInteractionState)));
			spdlog::info("[TFD][Menu][GlobalState] TFDJoinEnemyState={} ({})", GetGlobalValueInt(gJoinEnemyState), DecodeJoinEnemyState(GetGlobalValueInt(gJoinEnemyState)));
			spdlog::info("[TFD][Menu][GlobalState] TFDLeftForDeadState={} ({})", GetGlobalValueInt(gLeftForDeadState), DecodeLeftForDeadState(GetGlobalValueInt(gLeftForDeadState)));
			spdlog::info("[TFD][Menu][GlobalState] TFDPleasureState={} ({})", GetGlobalValueInt(gPleasureState), DecodePleasureState(GetGlobalValueInt(gPleasureState)));
			spdlog::info("[TFD][Menu][GlobalState] TFDPreCombatState={} ({})", GetGlobalValueInt(gPreCombatState), DecodePreCombatState(GetGlobalValueInt(gPreCombatState)));
			spdlog::info("[TFD][Menu][GlobalState] TFDRecoveryState={} ({})", GetGlobalValueInt(gRecoveryState), DecodeRecoveryState(GetGlobalValueInt(gRecoveryState)));
			spdlog::info("[TFD][Menu][GlobalState] TFDRescueMarkerState={} ({})", GetGlobalValueInt(gRescueMarkerState), DecodeAvailabilityState(GetGlobalValueInt(gRescueMarkerState)));
			spdlog::info("[TFD][Menu][GlobalState] TFDRescueState={} ({})", GetGlobalValueInt(gRescueState), DecodeRescueState(GetGlobalValueInt(gRescueState)));
			spdlog::info("[TFD][Menu][GlobalState] TFDTeammateState={} ({})", GetGlobalValueInt(gTeammateState), DecodeTeammateState(GetGlobalValueInt(gTeammateState)));
			spdlog::info("[TFD][Menu][GlobalState] TFDVictoryState={} ({})", GetGlobalValueInt(gVictoryState), DecodeVictoryState(GetGlobalValueInt(gVictoryState)));
		}

		static const char* SafeStr(const char* s)
		{
			return (s && s[0]) ? s : "";
		}

		static double CurrentGameDays()
		{
			auto* calendar = RE::Calendar::GetSingleton();
			return calendar ? static_cast<double>(calendar->rawDaysPassed) : 0.0;
		}

		static void RefreshCreatureTeammateMenuRows(bool resetInlineState)
		{
			const double now = NowSec();
			TFD::HostilityController::Update(now);
			gCreatureTeammateMenuRows = TFD::Tame::GetActiveSnapshots(now);
			gCreatureTeammateMenuRefreshRealSec = now;
			gCreatureTeammateMenuRefreshGameDays = CurrentGameDays();
			if (resetInlineState) {
				gCreatureTeammateFeedSessionId = 0;
				gCreatureTeammateReleaseConfirmSessionId = 0;
				return;
			}

			auto keepSession = [](RE::FormID sessionId) {
				return sessionId != 0 && std::any_of(
					gCreatureTeammateMenuRows.begin(),
					gCreatureTeammateMenuRows.end(),
					[&](const TFD::Tame::ActiveSnapshot& snap) { return snap.sessionId == sessionId; });
				};
			if (!keepSession(gCreatureTeammateFeedSessionId)) {
				gCreatureTeammateFeedSessionId = 0;
			}
			if (!keepSession(gCreatureTeammateReleaseConfirmSessionId)) {
				gCreatureTeammateReleaseConfirmSessionId = 0;
			}
		}

		static double GetDisplayRemainingTameSec(const TFD::Tame::ActiveSnapshot& snap)
		{
			const double elapsed = NowSec() - gCreatureTeammateMenuRefreshRealSec;
			return (std::max)(0.0, snap.remainingTameSec - elapsed);
		}

		static double GetDisplayRemainingCompanionHours(const TFD::Tame::ActiveSnapshot& snap)
		{
			const double elapsedHours = (CurrentGameDays() - gCreatureTeammateMenuRefreshGameDays) * 24.0;
			return (std::max)(0.0, snap.remainingCompanionHours - elapsedHours);
		}

		static std::string FormatCountdownClock(double totalSeconds)
		{
			const auto secs = static_cast<int>(std::floor((std::max)(0.0, totalSeconds) + 0.5));
			const int hours = secs / 3600;
			const int mins = (secs % 3600) / 60;
			const int rem = secs % 60;
			char buffer[64];
			if (hours > 0) {
				std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", hours, mins, rem);
			}
			else {
				std::snprintf(buffer, sizeof(buffer), "%02d:%02d", mins, rem);
			}
			return buffer;
		}

		static std::string FormatCreatureTimer(const TFD::Tame::ActiveSnapshot& snap)
		{
			if (snap.disposition == TFD::Tame::TameDisposition::Companion) {
				return FormatCountdownClock(GetDisplayRemainingCompanionHours(snap) * 3600.0);
			}

			return FormatCountdownClock(GetDisplayRemainingTameSec(snap));
		}

		static const char* CreatureStateLabel(const TFD::Tame::ActiveSnapshot& snap)
		{
			switch (snap.disposition) {
			case TFD::Tame::TameDisposition::Companion:
				return "Teammate";
			case TFD::Tame::TameDisposition::Calm:
				return "Tame";
			default:
				return "Unknown";
			}
		}

		static RE::BGSLocation* GetCellLocation(RE::TESObjectREFR* ref)
		{
			auto* cell = ref ? ref->GetParentCell() : nullptr;
			return cell ? cell->GetLocation() : nullptr;
		}

		static RE::BGSLocationRefType* LookupLocRefType(const char* editorID)
		{
			if (!editorID || !editorID[0]) {
				return nullptr;
			}
			return RE::TESForm::LookupByEditorID<RE::BGSLocationRefType>(editorID);
		}

		static bool LocationHasSpecialRefTypeByEditorID(RE::BGSLocation* loc, const char* editorID)
		{
			if (!loc) {
				return false;
			}

			auto* want = LookupLocRefType(editorID);
			if (!want) {
				return false;
			}

			for (std::uint32_t i = 0; i < loc->specialRefs.size(); ++i) {
				const auto& sref = loc->specialRefs[i];
				auto* type = sref.type;
				if (!type) {
					continue;
				}
				if (type == want || type->GetFormID() == want->GetFormID()) {
					return true;
				}
			}

			return false;
		}

		static void RenderSpecialRefSummary(const char* label, RE::BGSLocation* loc)
		{
			const bool hasCenter = LocationHasSpecialRefTypeByEditorID(loc, "LocationCenterMarker");
			const bool hasInside = LocationHasSpecialRefTypeByEditorID(loc, "InsideEntrance");
			const bool hasOutside = LocationHasSpecialRefTypeByEditorID(loc, "OutsideEntrance");
			const bool hasBoss = LocationHasSpecialRefTypeByEditorID(loc, "Boss");
			const bool hasBossContainer = LocationHasSpecialRefTypeByEditorID(loc, "BossContainer");
			const bool hasCaptive = LocationHasSpecialRefTypeByEditorID(loc, "CaptiveMarker");

			ImGuiMCP::Text(
				"%s: Center=%d Inside=%d Outside=%d Boss=%d BossContainer=%d Captive=%d",
				label,
				hasCenter ? 1 : 0,
				hasInside ? 1 : 0,
				hasOutside ? 1 : 0,
				hasBoss ? 1 : 0,
				hasBossContainer ? 1 : 0,
				hasCaptive ? 1 : 0);
		}

		static void RenderSpecialRefs(const char* label, RE::BGSLocation* loc)
		{
			ImGuiMCP::Text("%s", label);

			if (!loc) {
				ImGuiMCP::Text("  <null loc>");
				return;
			}

			const auto count = static_cast<std::uint32_t>(loc->specialRefs.size());
			ImGuiMCP::Text("  Count=%u", count);

			for (std::uint32_t i = 0; i < count && i < 12; ++i) {
				const auto& sref = loc->specialRefs[i];
				auto* type = sref.type;
				auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(sref.refData.refID);
				auto* base = ref ? ref->GetBaseObject() : nullptr;

				const auto typeId = type ? type->GetFormID() : 0;
				const auto refId = sref.refData.refID;
				const auto typeEdid = type ? TFD::Util::GetEditorId(type) : std::string{};
				const auto refEdid = ref ? TFD::Util::GetEditorId(ref) : std::string{};
				const auto baseEdid = base ? TFD::Util::GetEditorId(base) : std::string{};

				ImGuiMCP::Text(
					"  [%u] Type=0x%08X %s | Ref=0x%08X %s | Base=%s",
					i,
					typeId,
					typeEdid.c_str(),
					refId,
					refEdid.c_str(),
					baseEdid.c_str());
			}
		}

		static void RenderLocationLine(const char* label, RE::BGSLocation* loc)
		{
			const auto formId = loc ? loc->GetFormID() : 0;
			const auto editorId = loc ? TFD::Util::GetEditorId(loc) : std::string{};
			const char* name = (loc && loc->GetName()) ? loc->GetName() : "";

			const bool isInn = TFD::Location::LocationHasKeywordByEditorID(loc, "LocTypeInn");
			const bool isDwelling = TFD::Location::LocationHasKeywordByEditorID(loc, "LocTypeDwelling");
			const bool isCandidate = TFD::Location::IsRescueCandidateLocation(loc);

			ImGuiMCP::Text(
				"%s: 0x%08X | %s | %s | Candidate=%d Inn=%d Dwelling=%d",
				label,
				formId,
				editorId.c_str(),
				SafeStr(name),
				isCandidate ? 1 : 0,
				isInn ? 1 : 0,
				isDwelling ? 1 : 0);
		}

		static void RenderLocationChain(const char* label, RE::BGSLocation* seed)
		{
			ImGuiMCP::Text("%s", label);

			RE::BGSLocation* cur = seed;
			for (int i = 0; i < 8; ++i) {
				if (!cur) {
					ImGuiMCP::Text("  [%d] <null>", i);
					break;
				}

				const auto formId = cur->GetFormID();
				const auto editorId = TFD::Util::GetEditorId(cur);
				const char* name = cur->GetName() ? cur->GetName() : "";

				const bool isInn = TFD::Location::LocationHasKeywordByEditorID(cur, "LocTypeInn");
				const bool isDwelling = TFD::Location::LocationHasKeywordByEditorID(cur, "LocTypeDwelling");
				const bool isCandidate = TFD::Location::IsRescueCandidateLocation(cur);

				ImGuiMCP::Text(
					"  [%d] 0x%08X | %s | %s | Candidate=%d Inn=%d Dwelling=%d",
					i,
					formId,
					editorId.c_str(),
					SafeStr(name),
					isCandidate ? 1 : 0,
					isInn ? 1 : 0,
					isDwelling ? 1 : 0);

				cur = cur->parentLoc;
			}
		}

		static bool ContainsNoCase(std::string_view haystack, std::string_view needle)
		{
			if (needle.empty() || haystack.size() < needle.size()) {
				return false;
			}

			auto lower = [](char c) {
				return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				};

			for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
				bool ok = true;
				for (std::size_t j = 0; j < needle.size(); ++j) {
					if (lower(haystack[i + j]) != lower(needle[j])) {
						ok = false;
						break;
					}
				}
				if (ok) {
					return true;
				}
			}
			return false;
		}

		static bool IsBedLikeBase(RE::TESBoundObject* base)
		{
			if (!base) {
				return false;
			}

			if (base->GetFormType() != RE::FormType::Furniture) {
				return false;
			}

			const auto edid = TFD::Util::GetEditorId(base);
			const char* name = base->GetName() ? base->GetName() : "";

			if (ContainsNoCase(edid, "BedRoll") || ContainsNoCase(name, "Bed Roll")) {
				return true;
			}
			if (ContainsNoCase(edid, "Bed") || ContainsNoCase(name, "Bed")) {
				return true;
			}

			return false;
		}

		static void RenderNearbyBedRefs(RE::TESObjectREFR* player)
		{
			ImGuiMCP::Text("Nearby Bed / BedRoll Monitor");

			if (!player) {
				ImGuiMCP::Text("  <null player>");
				return;
			}

			auto* cell = player->GetParentCell();
			if (!cell) {
				ImGuiMCP::Text("  <null cell>");
				return;
			}

			auto& runtime = cell->GetRuntimeData();
			std::uint32_t shown = 0;

			for (const auto& refPtr : runtime.references) {
				RE::TESObjectREFR* ref = refPtr.get();
				if (!ref) {
					continue;
				}

				RE::TESBoundObject* base = ref->GetBaseObject();
				if (!IsBedLikeBase(base)) {
					continue;
				}

				const auto refEdid = TFD::Util::GetEditorId(ref);
				const auto baseEdid = TFD::Util::GetEditorId(base);
				const char* baseName = (base && base->GetName()) ? base->GetName() : "";
				const auto pos = ref->GetPosition();

				ImGuiMCP::Text(
					"  [%u] Ref=0x%08X %s | Base=%s | Name=%s | Pos=%.0f %.0f %.0f",
					shown,
					ref->GetFormID(),
					refEdid.c_str(),
					baseEdid.c_str(),
					SafeStr(baseName),
					pos.x,
					pos.y,
					pos.z);

				++shown;
				if (shown >= 12) {
					break;
				}
			}

			if (shown == 0) {
				ImGuiMCP::Text("  <no nearby bed-like furniture refs found in current cell>");
			}
		}

		static void RenderLocationMonitor()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				ImGuiMCP::Text("Player: <null>");
				return;
			}

			auto* cell = player->GetParentCell();
			auto* world = cell ? cell->worldSpace : nullptr;

			auto* currentLoc = player->GetCurrentLocation();
			auto* cellLoc = GetCellLocation(player);
			auto* resolvedLoc = TFD::Location::GetLocationFromRef(player);

			auto* targetFromCurrent = TFD::Location::ResolveRescueTargetLocation(currentLoc);
			auto* targetFromCell = TFD::Location::ResolveRescueTargetLocation(cellLoc);
			auto* targetFromResolved = TFD::Location::ResolveRescueTargetLocation(resolvedLoc);

			ImGuiMCP::Separator();
			ImGuiMCP::Text("Location / Cell Monitor");

			ImGuiMCP::Text(
				"Cell: 0x%08X | %s | interior=%d",
				cell ? cell->GetFormID() : 0,
				cell ? TFD::Util::GetEditorId(cell).c_str() : "",
				(cell && cell->IsInteriorCell()) ? 1 : 0);

			ImGuiMCP::Text(
				"Worldspace: 0x%08X | %s",
				world ? world->GetFormID() : 0,
				world ? TFD::Util::GetEditorId(world).c_str() : "");

			RenderLocationLine("CurrentLocation()", currentLoc);
			RenderLocationLine("CellLocation()", cellLoc);
			RenderLocationLine("TFD GetLocationFromRef()", resolvedLoc);

			RenderLocationLine("Target from Current", targetFromCurrent);
			RenderLocationLine("Target from Cell", targetFromCell);
			RenderLocationLine("Target from Resolved", targetFromResolved);

			RenderLocationChain("Parent Chain from CurrentLocation()", currentLoc);
			RenderLocationChain("Parent Chain from CellLocation()", cellLoc);
			RenderLocationChain("Parent Chain from TFD GetLocationFromRef()", resolvedLoc);

			ImGuiMCP::Separator();
			ImGuiMCP::Text("Location Ref Type Monitor");
			RenderSpecialRefSummary("Summary from Current", currentLoc);
			RenderSpecialRefSummary("Summary from Cell", cellLoc);
			RenderSpecialRefSummary("Summary from Resolved", resolvedLoc);
			RenderSpecialRefs("SpecialRefs from CurrentLocation()", currentLoc);
			RenderSpecialRefs("SpecialRefs from TFD GetLocationFromRef()", resolvedLoc);

			ImGuiMCP::Separator();
			RenderNearbyBedRefs(player);
		}


		static std::uint32_t GetCaptivePhaseRaw()
		{
			return TFD::DefeatMonitor::GetCaptivePhaseRaw();
		}

		static const char* GetCaptivePhaseName()
		{
			return TFD::DefeatMonitor::GetCaptivePhaseName();
		}

		static bool IsCaptiveFamily()
		{
			return TFD::DefeatMonitor::IsCaptiveFamily();
		}

		static bool IsPreCombatPhase()
		{
			ResolveGlobals();
			return GetGlobalValue(gPreCombatState) >= 0.5f;
		}

		static const char* KeyNameFromScanCode(std::uint32_t code)
		{
			switch (code) {
			case 1: return "Esc";
			case 2: return "1";
			case 3: return "2";
			case 4: return "3";
			case 5: return "4";
			case 6: return "5";
			case 7: return "6";
			case 8: return "7";
			case 9: return "8";
			case 10: return "9";
			case 11: return "0";
			case 14: return "Backspace";
			case 15: return "Tab";
			case 16: return "Q";
			case 17: return "W";
			case 18: return "E";
			case 19: return "R";
			case 20: return "T";
			case 21: return "Y";
			case 22: return "U";
			case 23: return "I";
			case 24: return "O";
			case 25: return "P";
			case 30: return "A";
			case 31: return "S";
			case 32: return "D";
			case 33: return "F";
			case 34: return "G";
			case 35: return "H";
			case 36: return "J";
			case 37: return "K";
			case 38: return "L";
			case 44: return "Z";
			case 45: return "X";
			case 46: return "C";
			case 47: return "V";
			case 48: return "B";
			case 49: return "N";
			case 50: return "M";
			case 57: return "Space";
			case 59: return "F1";
			case 60: return "F2";
			case 61: return "F3";
			case 62: return "F4";
			case 63: return "F5";
			case 64: return "F6";
			case 65: return "F7";
			case 66: return "F8";
			case 67: return "F9";
			case 68: return "F10";
			case 87: return "F11";
			case 88: return "F12";
			default: return nullptr;
			}
		}

		static std::string KeyLabel(std::uint32_t code)
		{
			if (const char* k = KeyNameFromScanCode(code)) {
				return std::string(k);
			}
			char buf[32]{};
			std::snprintf(buf, sizeof(buf), "Scan %u", code);
			return std::string(buf);
		}

		static void SyncFromCore()
		{
			uiEnabled = TFD::Settings::GetEnabled();
			uiThreshold = TFD::Settings::GetDefeatThresholdPct();
			uiPlayerGuardThreshold = TFD::Settings::GetPlayerGuardThresholdPct();
			uiPlayerGuardDamageScale = TFD::Settings::GetPlayerGuardDamageScale();
			uiAllyThreshold = TFD::Settings::GetAllyDownedThresholdPct();
			uiEnemyThreshold = TFD::Settings::GetEnemyDownedThresholdPct();
			uiBleedSeconds = TFD::Settings::GetBleedWindowSeconds();
			uiScanRadius = TFD::Settings::GetScanRadius();
			uiSweepRadius = TFD::Settings::GetSweepRadius();

			uiHotkeyEnabled = TFD::Settings::GetHotkeyEnabled();
			uiHotkeyScanCode = static_cast<int>(TFD::Settings::GetHotkeyScanCode());
			uiHotkeyCooldownMs = TFD::Settings::GetHotkeyCooldownMs();
			uiHotkeyWave = TFD::Settings::GetHotkeyWave();
		}

		static void ApplyToCore()
		{
			TFD::Settings::SetEnabled(uiEnabled);
			TFD::Settings::SetDefeatThresholdPct(uiThreshold);
			TFD::Settings::SetPlayerGuardThresholdPct(uiPlayerGuardThreshold);
			TFD::Settings::SetPlayerGuardDamageScale(uiPlayerGuardDamageScale);
			TFD::Settings::SetAllyDownedThresholdPct(uiAllyThreshold);
			TFD::Settings::SetEnemyDownedThresholdPct(uiEnemyThreshold);
			TFD::Settings::SetBleedWindowSeconds(uiBleedSeconds);
			TFD::Settings::SetScanRadius(uiScanRadius);
			TFD::Settings::SetSweepRadius(uiSweepRadius);

			TFD::Settings::SetHotkeyEnabled(uiHotkeyEnabled);
			TFD::Settings::SetHotkeyScanCode(static_cast<std::uint32_t>(std::max(1, uiHotkeyScanCode)));
			TFD::Settings::SetHotkeyCooldownMs(uiHotkeyCooldownMs);
			TFD::Settings::SetHotkeyWave(uiHotkeyWave);
		}

		enum class CaptureResult
		{
			None,
			Cancel,
			Captured
		};

		static CaptureResult PollRebindKey(std::uint32_t& outScanCode)
		{
			static constexpr std::array<int, 53> kVKs = {
				VK_ESCAPE,
				'1','2','3','4','5','6','7','8','9','0',
				'Q','W','E','R','T','Y','U','I','O','P',
				'A','S','D','F','G','H','J','K','L',
				'Z','X','C','V','B','N','M',
				VK_SPACE,
				VK_F1,VK_F2,VK_F3,VK_F4,VK_F5,VK_F6,VK_F7,VK_F8,VK_F9,VK_F10,VK_F11,VK_F12,
				VK_TAB, VK_BACK
			};

			for (int vk : kVKs) {
				const SHORT s = GetAsyncKeyState(vk);
				if ((s & 0x0001) == 0) {
					continue;
				}

				if (vk == VK_ESCAPE) {
					return CaptureResult::Cancel;
				}

				const UINT sc = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
				if (sc == 0) {
					continue;
				}

				outScanCode = static_cast<std::uint32_t>(sc & 0xFF);
				return CaptureResult::Captured;
			}

			return CaptureResult::None;
		}

		static const char* YesNo(bool v)
		{
			return v ? "Yes" : "No";
		}

		static bool HasPrefixNoCase(std::string_view value, std::string_view prefix)
		{
			if (prefix.empty() || value.size() < prefix.size()) {
				return false;
			}

			auto lower = [](char c) {
				return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				};

			for (std::size_t i = 0; i < prefix.size(); ++i) {
				if (lower(value[i]) != lower(prefix[i])) {
					return false;
				}
			}
			return true;
		}

		static void RenderLocationBrief(const char* label, RE::BGSLocation* loc)
		{
			const auto formId = loc ? loc->GetFormID() : 0;
			const auto editorId = loc ? TFD::Util::GetEditorId(loc) : std::string{};
			const char* name = (loc && loc->GetName()) ? loc->GetName() : "";

			ImGuiMCP::Text("%s: 0x%08X | %s | %s", label, formId, editorId.c_str(), SafeStr(name));
		}

		static void RenderCellBrief(const char* label, RE::TESObjectCELL* cell)
		{
			ImGuiMCP::Text(
				"%s: 0x%08X | %s | interior=%d",
				label,
				cell ? cell->GetFormID() : 0,
				cell ? TFD::Util::GetEditorId(cell).c_str() : "",
				(cell && cell->IsInteriorCell()) ? 1 : 0);
		}

		static void RenderRefBrief(const char* label, RE::TESObjectREFR* ref)
		{
			auto* base = ref ? ref->GetBaseObject() : nullptr;
			auto* cell = ref ? ref->GetParentCell() : nullptr;
			auto* loc = ref ? TFD::Location::GetLocationFromRef(ref) : nullptr;
			const auto refEdid = ref ? TFD::Util::GetEditorId(ref) : std::string{};
			const auto baseEdid = base ? TFD::Util::GetEditorId(base) : std::string{};
			const char* baseName = (base && base->GetName()) ? base->GetName() : "";

			ImGuiMCP::Text(
				"%s: 0x%08X | %s | base=%s | name=%s",
				label,
				ref ? ref->GetFormID() : 0,
				refEdid.c_str(),
				baseEdid.c_str(),
				SafeStr(baseName));
			RenderCellBrief("  Cell", cell);
			RenderLocationBrief("  Loc", loc);
		}

		static bool IsShiftDown()
		{
			return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 ||
				(GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0 ||
				(GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
		}

		static void RenderInlineFeedChoices(RE::Actor* actor, const TFD::Tame::ActiveSnapshot& snap)
		{
			if (!actor) {
				ImGuiMCP::Text("Feed unavailable: creature not loaded.");
				return;
			}

			const bool downed = TFD::DefeatMonitor::IsThresholdDownedActor(actor);
			if (downed) {
				ImGuiMCP::Text("This creature is downed. Feeding will revive and heal it.");
			}

			auto renderFeedGroup = [&](const char* title, TFD::Tame::FeedAction action) {
				auto options = TFD::Tame::GetFeedOptions(actor, action);
				ImGuiMCP::Text("%s", title);
				if (options.empty()) {
					ImGuiMCP::Text("  No valid bait.");
					return;
				}
				for (const auto& opt : options) {
					char buttonLabel[256];
					std::snprintf(buttonLabel, sizeof(buttonLabel), "%s##feed_%08X_%u_%08X", opt.label.c_str(), snap.actorId, snap.sessionId, opt.itemId);
					if (ImGuiMCP::Button(buttonLabel)) {
						if (TFD::Tame::ApplyFeed(actor, opt.itemId, action)) {
							if (action == TFD::Tame::FeedAction::Teammate) {
								RE::DebugNotification("TFD: Teammate fed.");
							}
							else {
								RE::DebugNotification("TFD: Calm feed applied.");
							}
							gCreatureTeammateFeedSessionId = 0;
							RefreshCreatureTeammateMenuRows(false);
						}
						else {
							RE::DebugNotification("TFD: Feed failed.");
						}
					}
				}
				};

			ImGuiMCP::Indent();
			if (snap.disposition == TFD::Tame::TameDisposition::Companion) {
				renderFeedGroup("Teammate Feed", TFD::Tame::FeedAction::Teammate);
			}
			else {
				renderFeedGroup("Calm Feed", TFD::Tame::FeedAction::Calm);
				ImGuiMCP::Separator();
				renderFeedGroup("Teammate Feed", TFD::Tame::FeedAction::Teammate);
			}
			if (ImGuiMCP::Button((std::string("Close Feed##") + std::to_string(snap.sessionId)).c_str())) {
				gCreatureTeammateFeedSessionId = 0;
			}
			ImGuiMCP::Unindent();
		}

		static void RenderInlineReleaseConfirm(RE::Actor* actor, const TFD::Tame::ActiveSnapshot& snap)
		{
			ImGuiMCP::Indent();
			ImGuiMCP::Text("Release this creature?");
			char confirmLabel[64];
			char cancelLabel[64];
			std::snprintf(confirmLabel, sizeof(confirmLabel), "Confirm Release##%u", snap.sessionId);
			std::snprintf(cancelLabel, sizeof(cancelLabel), "Cancel##%u", snap.sessionId);
			if (ImGuiMCP::Button(confirmLabel)) {
				bool released = false;
				if (actor) {
					released = TFD::Tame::Release(actor, TFD::Tame::ReleaseReason::Generic);
				}
				else {
					TFD::HostilityController::ReleaseSession(snap.sessionId, TFD::Tame::ReleaseReason::Generic);
					released = true;
				}
				if (released) {
					RE::DebugNotification("TFD: Creature released.");
					gCreatureTeammateReleaseConfirmSessionId = 0;
					RefreshCreatureTeammateMenuRows(false);
				}
				else {
					RE::DebugNotification("TFD: Release failed.");
				}
			}
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button(cancelLabel)) {
				gCreatureTeammateReleaseConfirmSessionId = 0;
			}
			ImGuiMCP::Unindent();
		}

		static void RenderCombatRulesPage()
		{
			ResolveGlobals();

			ImGuiMCP::Text("RULES OF COMBAT");
			ImGuiMCP::Separator();

			if (ImGuiMCP::CollapsingHeader("A. TFD")) {
				if (ImGuiMCP::Checkbox("Enable TFD", &uiEnabled)) {
					ApplyToCore();
				}
			}

			if (ImGuiMCP::CollapsingHeader("B. Threshold")) {
				if (ImGuiMCP::SliderFloat("Player Threshold (%)", &uiThreshold, 2.0f, 95.0f, "%.0f%%")) {
					if (uiPlayerGuardThreshold < uiThreshold) {
						uiPlayerGuardThreshold = uiThreshold;
					}
					ApplyToCore();
				}

				if (ImGuiMCP::SliderFloat("Player Guard Threshold (%)", &uiPlayerGuardThreshold, uiThreshold, 95.0f, "%.0f%%")) {
					ApplyToCore();
				}

				if (ImGuiMCP::SliderFloat("Player Guard Damage Scale", &uiPlayerGuardDamageScale, 0.0f, 1.0f, "%.2f")) {
					ApplyToCore();
				}

				ImGuiMCP::Text("Scale 1.00 = normal damage | 0.00 = no damage while guard is active");

				if (ImGuiMCP::SliderFloat("Ally Threshold (%)", &uiAllyThreshold, 2.0f, 95.0f, "%.0f%%")) {
					ApplyToCore();
				}

				if (ImGuiMCP::SliderFloat("Enemy Threshold (%)", &uiEnemyThreshold, 2.0f, 95.0f, "%.0f%%")) {
					ApplyToCore();
				}

				ImGuiMCP::Text("General Threshold: pending native setting");
			}

			if (ImGuiMCP::CollapsingHeader("C. Interaction")) {
				if (ImGuiMCP::Checkbox("Enable Interaction (H / Shift+H)", &uiHotkeyEnabled)) {
					ApplyToCore();
				}

				ImGuiMCP::Text("Current Key: %s", KeyLabel(static_cast<std::uint32_t>(uiHotkeyScanCode)).c_str());
				ImGuiMCP::Text("Default Key: H / Shift+H");

				if (!gCaptureHotkey) {
					if (ImGuiMCP::Button("Rebind Interaction Hotkey")) {
						gCaptureHotkey = true;
					}
				}
				else {
					ImGuiMCP::Text("Press a key now... (Esc = cancel)");

					std::uint32_t newCode = 0;
					switch (PollRebindKey(newCode)) {
					case CaptureResult::Cancel:
						gCaptureHotkey = false;
						RE::DebugNotification("TFD: Rebind cancelled");
						break;

					case CaptureResult::Captured:
						uiHotkeyScanCode = static_cast<int>(newCode);
						ApplyToCore();
						gCaptureHotkey = false;
						{
							std::string msg = "TFD: Hotkey = ";
							msg += KeyLabel(newCode);
							RE::DebugNotification(msg.c_str());
						}
						break;

					case CaptureResult::None:
					default:
						break;
					}
				}
			}

			if (ImGuiMCP::CollapsingHeader("D. Teammate List")) {
				const double nowMenu = NowSec();
				const bool creaturePanelJustOpened = (nowMenu - gCreatureTeammateMenuLastRenderSec) > 0.75;
				gCreatureTeammateMenuLastRenderSec = nowMenu;
				if (creaturePanelJustOpened) {
					RefreshCreatureTeammateMenuRows(true);
				}

				if (gCreatureTeammateMenuRows.empty()) {
					ImGuiMCP::Text("No active creature tame or teammate sessions.");
				}
				else {
					for (const auto& snap : gCreatureTeammateMenuRows) {
						RE::Actor* actor = snap.loaded ? RE::TESForm::LookupByID<RE::Actor>(snap.actorId) : nullptr;
						const bool actorDowned = actor && TFD::DefeatMonitor::IsThresholdDownedActor(actor);
						ImGuiMCP::Separator();
						ImGuiMCP::Text("%s", snap.actorName.c_str());
						ImGuiMCP::Text("State: %s%s", actorDowned ? "Downed | " : "", CreatureStateLabel(snap));
						ImGuiMCP::Text("Remaining: %s", FormatCreatureTimer(snap).c_str());
						ImGuiMCP::Text("Actor: 0x%08X | Session: %u%s", snap.actorId, snap.sessionId, snap.loaded ? "" : " | not loaded");
						char feedLabel[64];
						char releaseLabel[64];
						const char* feedText = gCreatureTeammateFeedSessionId == snap.sessionId ? "Hide Feed" : (actorDowned ? "Feed / Revive" : "Feed");
						std::snprintf(feedLabel, sizeof(feedLabel), "%s##%u", feedText, snap.sessionId);
						std::snprintf(releaseLabel, sizeof(releaseLabel), "%s##%u", gCreatureTeammateReleaseConfirmSessionId == snap.sessionId ? "Cancel Release" : "Release", snap.sessionId);

						ImGuiMCP::BeginDisabled(actor == nullptr);
						if (ImGuiMCP::Button(feedLabel)) {
							gCreatureTeammateFeedSessionId = (gCreatureTeammateFeedSessionId == snap.sessionId) ? 0 : snap.sessionId;
							gCreatureTeammateReleaseConfirmSessionId = 0;
						}
						ImGuiMCP::EndDisabled();
						ImGuiMCP::SameLine();
						if (ImGuiMCP::Button(releaseLabel)) {
							if (gCreatureTeammateReleaseConfirmSessionId == snap.sessionId) {
								gCreatureTeammateReleaseConfirmSessionId = 0;
							}
							else {
								gCreatureTeammateReleaseConfirmSessionId = snap.sessionId;
								gCreatureTeammateFeedSessionId = 0;
							}
						}

						if (gCreatureTeammateFeedSessionId == snap.sessionId) {
							RenderInlineFeedChoices(actor, snap);
						}
						if (gCreatureTeammateReleaseConfirmSessionId == snap.sessionId) {
							RenderInlineReleaseConfirm(actor, snap);
						}
					}
				}
			}
		}

		static void RenderFlowIndicatorSection()
		{
			const auto snap = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
			ImGuiMCP::Text("Root: %s", TFD::FlowController::Controller::ToString(snap.root));
			ImGuiMCP::Text("Context Root: %s", TFD::FlowController::Controller::ToString(snap.contextRoot));
			ImGuiMCP::Text("Gate: %s", TFD::FlowController::Controller::ToString(snap.gate));
			ImGuiMCP::Text("SubFlow: %s", TFD::FlowController::Controller::ToString(snap.sub));
			ImGuiMCP::Text("Captive Mode: %s", TFD::FlowController::Controller::ToString(snap.captiveMode));
			ImGuiMCP::Text("Token: %u", snap.token);
			ImGuiMCP::Text("Primary Actor: %08X", snap.primaryActorFormID);
			ImGuiMCP::Text("Terminal Resolved: %s", YesNo(snap.terminalResolved));
			ImGuiMCP::Text("Locked: %s", YesNo(snap.locked));
		}

		static void RenderDebugGlobalsSection()
		{
			RenderGlobalStateLine("Boss Container", gBossContainerMarkerState, DecodeAvailabilityState(GetGlobalValueInt(gBossContainerMarkerState)));
			RenderGlobalStateLine("Boss", gBossMarkerState, DecodeAvailabilityState(GetGlobalValueInt(gBossMarkerState)));
			RenderGlobalStateLine("Captive Marker", gCaptiveMarkerState, DecodeAvailabilityState(GetGlobalValueInt(gCaptiveMarkerState)));
			RenderGlobalStateLine("Captive", gCaptiveState, DecodeCaptiveState(GetGlobalValueInt(gCaptiveState)));
			RenderGlobalStateLine("Container", gContainerMarkerState, DecodeAvailabilityState(GetGlobalValueInt(gContainerMarkerState)));
			RenderGlobalStateLine("Defeat", gDefeatState, DecodeDefeatState(GetGlobalValueInt(gDefeatState)));
			RenderGlobalStateLine("Dialogue", gDialogueState, DecodeDialogueState(GetGlobalValueInt(gDialogueState)));
			RenderGlobalStateLine("Escape Route", gEscapeRouteState, DecodeEscapeRouteState(GetGlobalValueInt(gEscapeRouteState)));
			RenderGlobalStateLine("Enemy Faction", gEnemyFactionState, DecodeEnemyFactionState(GetGlobalValueInt(gEnemyFactionState)));
			RenderGlobalStateLine("Enemy Race", gEnemyRaceState, DecodeEnemyRaceState(GetGlobalValueInt(gEnemyRaceState)));
			RenderGlobalStateLine("Hostile", gHostileState, DecodeHostileState(GetGlobalValueInt(gHostileState)));
			RenderGlobalStateLine("In Combat", gInCombatState, DecodeInCombatState(GetGlobalValueInt(gInCombatState)));
			RenderGlobalStateLine("Interaction", gInteractionState, DecodeInteractionState(GetGlobalValueInt(gInteractionState)));
			RenderGlobalStateLine("Join Enemy", gJoinEnemyState, DecodeJoinEnemyState(GetGlobalValueInt(gJoinEnemyState)));
			RenderGlobalStateLine("Left For Dead", gLeftForDeadState, DecodeLeftForDeadState(GetGlobalValueInt(gLeftForDeadState)));
			RenderGlobalStateLine("Pleasure", gPleasureState, DecodePleasureState(GetGlobalValueInt(gPleasureState)));
			RenderGlobalStateLine("Pre Combat", gPreCombatState, DecodePreCombatState(GetGlobalValueInt(gPreCombatState)));
			RenderGlobalStateLine("Recovery", gRecoveryState, DecodeRecoveryState(GetGlobalValueInt(gRecoveryState)));
			RenderGlobalStateLine("Rescue Marker", gRescueMarkerState, DecodeAvailabilityState(GetGlobalValueInt(gRescueMarkerState)));
			RenderGlobalStateLine("Rescue", gRescueState, DecodeRescueState(GetGlobalValueInt(gRescueState)));
			RenderGlobalStateLine("Teammate", gTeammateState, DecodeTeammateState(GetGlobalValueInt(gTeammateState)));
			RenderGlobalStateLine("Victory", gVictoryState, DecodeVictoryState(GetGlobalValueInt(gVictoryState)));

			ImGuiMCP::Separator();
			if (ImGuiMCP::Button("Dump Global State To Log")) {
				DumpGlobalStateToLog();
			}
		}

		static void RenderQuestAliasMonitorSection();
		static void RenderCaptiveRescueToolsSection()
		{
			ImGuiMCP::Text("Captive Marker");
			RenderRefBrief("Location Captive Marker", TFD::Location::GetCachedCaptiveMarker());
			RenderGlobalStateLine("Captive", gCaptiveState, DecodeCaptiveState(GetGlobalValueInt(gCaptiveState)));
			RenderGlobalStateLine("Escape Route", gEscapeRouteState, DecodeEscapeRouteState(GetGlobalValueInt(gEscapeRouteState)));

			if (ImGuiMCP::Button("Rescan Captive Marker")) {
				TFD::Location::RescanCaptiveMarker();
				TFD::Location::DumpContextToLog();
			}
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Teleport -> Captive Marker")) {
				TFD::Location::TeleportToCaptiveMarker();
				TFD::HostilityController::ScheduleStopCombatWaves(TFD::Settings::GetSweepRadius(), true, 6, 180);
			}

			ImGuiMCP::Separator();
			ImGuiMCP::Text("Rescue Marker");

			auto* safeLoc = TFD::Location::GetMostRecentCachedSafeLocation();
			auto* rescueRef = TFD::Location::ResolveMostRecentCachedRescueDestination(true);
			RenderLocationBrief("Cached Safe Location", safeLoc);
			RenderRefBrief("Preferred Rescue Destination", rescueRef);

			TFD::Location::SafeCheckpoint cp{};
			if (safeLoc && TFD::Location::GetLastSafeCheckpointForLocation(safeLoc, cp)) {
				RenderRefBrief("Rescue Cache Inside Marker", RE::TESForm::LookupByID<RE::TESObjectREFR>(cp.insideEntranceRefId));
				RenderRefBrief("Rescue Cache Center Marker", RE::TESForm::LookupByID<RE::TESObjectREFR>(cp.centerMarkerRefId));
				RenderRefBrief("Rescue Cache Entry Door", RE::TESForm::LookupByID<RE::TESObjectREFR>(cp.entryDoorRefId));
			}

			TFD::Location::ApprovedBed bed{};
			if (safeLoc && TFD::Location::GetBestApprovedBedForLocation(safeLoc, bed)) {
				RenderRefBrief("Approved Rescue Bed", RE::TESForm::LookupByID<RE::TESObjectREFR>(bed.bedRefId));
			}

			if (ImGuiMCP::Button("Dump Rescue Cache To Log")) {
				TFD::Location::DumpRescueCacheToLog();
			}
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Clear Rescue Cache")) {
				TFD::Location::ClearRescueCache();
			}
		}


		static const char* DecodeAggressionValue(float value)
		{
			const int rounded = static_cast<int>(std::lround(value));
			switch (rounded) {
			case 0:
				return "Unaggressive";
			case 1:
				return "Aggressive";
			case 2:
				return "VeryAggressive";
			case 3:
				return "Frenzied";
			default:
				return "Unknown";
			}
		}

		static void RenderActorsDebugSection()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				ImGuiMCP::Text("Player not ready");
				return;
			}

			constexpr float kDebugRadius = 8000.0f;
			const auto snapshot = TFD::Actor::BuildSnapshot(kDebugRadius, false);
			const RE::FormID playerId = player->GetFormID();

			ImGuiMCP::SetWindowFontScale(0.90f);
			ImGuiMCP::Text("Radius: %.0f | Scanned Actors: %d", kDebugRadius, static_cast<int>(snapshot.actors.size()));
			ImGuiMCP::Separator();

			for (const auto& info : snapshot.actors) {
				auto* actor = info.get();
				if (!actor) {
					continue;
				}

				const bool targetPlayer = info.currentTargetFormID == playerId;
				bool targetPlayerSide = targetPlayer;
				if (!targetPlayer && info.currentTargetFormID != 0) {
					if (const auto* targetInfo = TFD::Actor::FindActorInfo(snapshot, info.currentTargetFormID)) {
						targetPlayerSide = targetInfo->playerSide;
					}
					else if (auto targetSp = actor->GetActorRuntimeData().currentCombatTarget.get()) {
						if (auto* currentTarget = targetSp.get()) {
							targetPlayerSide = currentTarget->IsPlayerRef() || currentTarget->IsPlayerTeammate() || TFD::TeammateManager::IsActiveFollowerActor(currentTarget);
						}
					}
				}

				const float aggressionValue = actor->GetActorValue(RE::ActorValue::kAggression);
				const bool actorInCombat = actor->IsInCombat() || info.inCombat;
				const bool committedHostile = actorInCombat || targetPlayerSide;
				const auto creatureClass = TFD::Actor::Interaction::GetCreatureClass(actor);
				const auto classifyNow = TFD::Actor::Interaction::ClassifyTarget(player, actor, false, committedHostile, info.dist);
				const bool truceAble =
					classifyNow.valid &&
					classifyNow.intent == TFD::Actor::Interaction::Intent::Truce &&
					TFD::HostilityController::CanStartTruce(actor);
				const bool preCombatContext =
					!actorInCombat &&
					!targetPlayer &&
					!targetPlayerSide &&
					info.currentTargetFormID == 0;
				const bool inCombatContext =
					actorInCombat &&
					(targetPlayer || targetPlayerSide || info.currentTargetFormID != 0);
				const bool preCombatTruceAble = truceAble && preCombatContext;
				const bool inCombatTruceAble = truceAble && inCombatContext;

				const char* actorName = actor->GetName();
				if (!actorName || !actorName[0]) {
					actorName = "<unnamed>";
				}

				ImGuiMCP::BulletText(
					"%s | %08X | dist=%.1f | class=%s",
					actorName,
					actor->GetFormID(),
					info.dist,
					TFD::Actor::Interaction::ToString(creatureClass));

				ImGuiMCP::Indent();
				ImGuiMCP::Text(
					"aggr=%d(%s) | hostile=%s | combat=%s | targetPlayer=%s | targetPlayerSide=%s",
					static_cast<int>(std::lround(aggressionValue)),
					DecodeAggressionValue(aggressionValue),
					YesNo(info.hostileToPlayer),
					YesNo(actorInCombat),
					YesNo(targetPlayer),
					YesNo(targetPlayerSide));

				ImGuiMCP::Text(
					"currentTarget=%08X | valid=%s | intent=%s | truceAble=%s | reject=%s",
					info.currentTargetFormID,
					YesNo(classifyNow.valid),
					TFD::Actor::Interaction::ToString(classifyNow.intent),
					YesNo(truceAble),
					TFD::Actor::Interaction::ToString(classifyNow.rejectReason));

				ImGuiMCP::Text(
					"preCombatTruceAble=%s | inCombatTruceAble=%s",
					YesNo(preCombatTruceAble),
					YesNo(inCombatTruceAble));
				ImGuiMCP::Unindent();
				ImGuiMCP::Spacing();
			}

			ImGuiMCP::SetWindowFontScale(1.0f);
		}

		static void RenderDebugPage()
		{
			ResolveGlobals();

			ImGuiMCP::Text("DEBUG");
			ImGuiMCP::Separator();

			if (ImGuiMCP::CollapsingHeader("Flow Indicator")) {
				RenderFlowIndicatorSection();
			}

			if (ImGuiMCP::CollapsingHeader("Global State")) {
				RenderDebugGlobalsSection();
			}

			if (ImGuiMCP::CollapsingHeader("Actors")) {
				RenderActorsDebugSection();
			}

			if (ImGuiMCP::CollapsingHeader("Quest Alias Monitor")) {
				RenderQuestAliasMonitorSection();

			}

			if (ImGuiMCP::CollapsingHeader("Captive / Rescue Tools")) {
				RenderCaptiveRescueToolsSection();
			}
		}

		struct QuestAliasReadLock
		{
			RE::BSReadWriteLock& lock;
			explicit QuestAliasReadLock(RE::BSReadWriteLock& a_lock) : lock(a_lock) { lock.LockForRead(); }
			~QuestAliasReadLock() { lock.UnlockForRead(); }
		};

		static void RenderQuestAliasEntry(RE::BGSBaseAlias* alias)
		{
			if (!alias) {
				ImGuiMCP::BulletText("<null alias>");
				return;
			}

			const char* aliasName = alias->aliasName.c_str();
			const char* typeName = alias->QType().c_str();

			if (auto* refAlias = skyrim_cast<RE::BGSRefAlias*>(alias)) {
				auto* ref = refAlias->GetReference();
				auto* actor = refAlias->GetActorReference();
				ImGuiMCP::BulletText(
					"%s | %s | %s",
					SafeStr(aliasName),
					SafeStr(typeName),
					ref ? "FILLED" : "EMPTY");
				if (ref) {
					ImGuiMCP::Indent();
					RenderRefBrief(actor ? "Actor Ref" : "Ref", ref);
					ImGuiMCP::Unindent();
				}
				return;
			}

			if (skyrim_cast<RE::BGSLocAlias*>(alias)) {
				ImGuiMCP::BulletText("%s | %s | runtime fill monitor unavailable", SafeStr(aliasName), SafeStr(typeName));
				return;
			}

			ImGuiMCP::BulletText("%s | %s", SafeStr(aliasName), SafeStr(typeName));
		}

		static void RenderQuestAliasMonitorSection()
		{
			auto* data = RE::TESDataHandler::GetSingleton();
			if (!data) {
				ImGuiMCP::Text("TESDataHandler not ready");
				return;
			}

			auto& quests = data->GetFormArray<RE::TESQuest>();
			std::vector<RE::TESQuest*> tfdQuests;
			tfdQuests.reserve(32);

			for (auto* quest : quests) {
				if (!quest) {
					continue;
				}

				const auto editorId = TFD::Util::GetEditorId(quest);
				if (!HasPrefixNoCase(editorId, "TFD")) {
					continue;
				}

				if (quest->aliases.empty()) {
					continue;
				}

				tfdQuests.push_back(quest);
			}

			std::sort(tfdQuests.begin(), tfdQuests.end(), [](RE::TESQuest* a, RE::TESQuest* b) {
				return TFD::Util::GetEditorId(a) < TFD::Util::GetEditorId(b);
				});

			ImGuiMCP::Text("TFD Quests With Aliases: %d", static_cast<int>(tfdQuests.size()));

			for (auto* quest : tfdQuests) {
				if (!quest) {
					continue;
				}

				QuestAliasReadLock aliasLock(quest->aliasAccessLock);

				int filledRefCount = 0;
				for (auto* baseAlias : quest->aliases) {
					auto* refAlias = skyrim_cast<RE::BGSRefAlias*>(baseAlias);
					if (refAlias && refAlias->GetReference()) {
						++filledRefCount;
					}
				}

				const auto editorId = TFD::Util::GetEditorId(quest);
				const char* name = quest->GetName() ? quest->GetName() : "";
				std::string header = editorId;
				if (name[0]) {
					header += " | ";
					header += name;
				}
				header += " | aliases=";
				header += std::to_string(static_cast<int>(quest->aliases.size()));
				header += " | filled=";
				header += std::to_string(filledRefCount);

				if (ImGuiMCP::CollapsingHeader(header.c_str())) {
					ImGuiMCP::Text(
						"Quest: 0x%08X | running=%s | enabled=%s | active=%s",
						quest->GetFormID(),
						YesNo(quest->IsRunning()),
						YesNo(quest->IsEnabled()),
						YesNo(quest->IsActive()));

					for (auto* baseAlias : quest->aliases) {
						RenderQuestAliasEntry(baseAlias);
					}
				}
			}
		}

		static void TryRegisterMenu()
		{
			if (menuRegistered) {
				return;
			}

			auto mod = GetModuleHandleW(L"SKSEMenuFramework.dll");
			if (!mod) {
				mod = GetModuleHandleW(L"SKSEMenuFramework");
			}
			if (!mod) {
				spdlog::warn("[TFD][SMF] SMF module not loaded yet");
				return;
			}

			::menuFramework = mod;

			SyncFromCore();

			SKSEMenuFramework::SetSection("TFD");
			SKSEMenuFramework::AddSectionItem("RULES OF COMBAT", RenderCombatRulesPage);
			SKSEMenuFramework::AddSectionItem("DEBUG", RenderDebugPage);
			TFD::FeedPopup::Init();
			TFD::StatusHUD::Init();

			menuRegistered = true;
			spdlog::info("[TFD][SMF] menu registered OK");
			RE::DebugNotification("TFD: SMF menu registered");
		}

		constexpr std::uint32_t kScanCodeActivate = 0x12;
		constexpr std::uint32_t kScanCodeEscape = 0x01;
		constexpr std::uint32_t kScanCodeEnter = 0x1C;
		constexpr std::uint32_t kScanCodeNumpadEnter = 0x9C;
		constexpr std::uint32_t kScanCodeUp = 0xC8;
		constexpr std::uint32_t kScanCodeDown = 0xD0;

		constexpr double kPopupNavDebounce = 0.25;
		constexpr double kPopupConfirmDebounce = 0.20;

		class InputSink : public RE::BSTEventSink<RE::InputEvent*>
		{
		private:
			Clock::time_point nextPopupNav{};
			Clock::time_point nextPopupConfirm{};

		public:
			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* evns, RE::BSTEventSource<RE::InputEvent*>*) override
			{
				if (!evns) {
					return RE::BSEventNotifyControl::kContinue;
				}

				for (auto* e = *evns; e; e = e->next) {
					auto* btn = e->AsButtonEvent();
					if (!btn) {
						continue;
					}
					if (btn->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
						continue;
					}
					if (!btn->IsPressed()) {
						continue;
					}

					const auto code = static_cast<std::uint32_t>(btn->GetIDCode());

					if (code == kScanCodeActivate) {
						auto* ui = RE::UI::GetSingleton();
						if (!TFD::FeedPopup::IsOpen() &&
							(!ui || (!ui->IsMenuOpen(RE::MainMenu::MENU_NAME) &&
								!ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) &&
								!ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME) &&
								!ui->IsMenuOpen(RE::Console::MENU_NAME) &&
								!ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME) &&
								!ui->IsMenuOpen(RE::JournalMenu::MENU_NAME) &&
								!ui->IsMenuOpen(RE::LockpickingMenu::MENU_NAME)))) {
							auto* player = RE::PlayerCharacter::GetSingleton();
							if (player && !TFD::Captive::IsStandardCaptiveActive()) {
								auto openTeammateDialogue = [&](RE::Actor* teammateTalkTarget, const char* sourceReason) -> bool {
									if (!teammateTalkTarget) {
										return false;
									}

									const std::string_view teammateSource = sourceReason ? std::string_view(sourceReason) : std::string_view{};
									const bool downedDialogueOpen =
										teammateSource.find("downed") != std::string_view::npos ||
										teammateSource.find("defeated_redirect") != std::string_view::npos ||
										TFD::Actor::IsDownByHealthThreshold(teammateTalkTarget, TFD::Settings::GetAllyDownedThresholdPct());

									if (downedDialogueOpen) {
										TFD::TeammateManager::ArmDownedTeammateRecoveryDialogueHold(
											teammateTalkTarget,
											kDownedTeammateRecoveryHoldSec,
											sourceReason ? sourceReason : "teammate_downed_recovery_dialogue");
									}

									const auto teammateNow = Clock::now();
									const char* blockReason = "none";
									double blockRemainingSec = 0.0;
									if (IsTeammateManualDialogueBlocked(teammateTalkTarget, teammateNow, blockReason, blockRemainingSec, downedDialogueOpen)) {
										spdlog::info(
											"[TFD][Menu] teammate manual dialogue blocked target={:08X} source={} reason={} remaining={:.2f}s pleasurePhase={} dialogueContext={} dialogueState={} downedRecovery={}",
											teammateTalkTarget->GetFormID(),
											sourceReason ? sourceReason : "unknown",
											blockReason ? blockReason : "unknown",
											blockRemainingSec,
											TFD::PleasureRuntime::GetPhaseName(),
											TFD::FlowController::GetDialogueContextName(),
											GetGlobalValueInt(gDialogueState),
											downedDialogueOpen ? 1 : 0);
										return true;
									}

									const bool refreshed = TFD::TeammateManager::RegisterOrRefreshTeammateNow(teammateTalkTarget, sourceReason ? sourceReason : "teammate_activate_dialogue");

									if (!teammateTalkTarget->IsAIEnabled()) {
										teammateTalkTarget->EnableAI(true);
									}

									auto combatTargetSp = teammateTalkTarget->GetActorRuntimeData().currentCombatTarget.get();
									auto* combatTarget = combatTargetSp.get();
									const bool combatTargetPlayerSide =
										combatTarget == player ||
										(combatTarget && (TFD::TeammateManager::IsActiveFollowerActor(combatTarget) || TFD::TeammateManager::IsPlayerSideTeammateActor(combatTarget)));
									const bool shouldClearStaleCombat =
										teammateTalkTarget->IsHostileToActor(player) ||
										combatTargetPlayerSide ||
										(teammateTalkTarget->IsInCombat() && !combatTarget);

									if (shouldClearStaleCombat || downedDialogueOpen) {
										teammateTalkTarget->StopCombat();
										if (auto* process = RE::ProcessLists::GetSingleton()) {
											process->StopCombatAndAlarmOnActor(teammateTalkTarget, false);
										}
										if (teammateTalkTarget->IsWeaponDrawn()) {
											teammateTalkTarget->DrawWeaponMagicHands(false);
										}
									}

									teammateTalkTarget->AllowPCDialogue(true);
									if (!downedDialogueOpen) {
										teammateTalkTarget->EvaluatePackage(false, true);
										teammateTalkTarget->EvaluatePackage(true, true);
									}
									else {
										spdlog::info(
											"[TFD][Menu] downed teammate recovery dialogue prepared target={:08X} source={} hp={:.1f} downByThreshold={} bleedout={}",
											teammateTalkTarget->GetFormID(),
											sourceReason ? sourceReason : "unknown",
											teammateTalkTarget->GetActorValue(RE::ActorValue::kHealth),
											TFD::Actor::IsDownByHealthThreshold(teammateTalkTarget, TFD::Settings::GetAllyDownedThresholdPct()) ? 1 : 0,
											(teammateTalkTarget->AsActorState() && teammateTalkTarget->AsActorState()->IsBleedingOut()) ? 1 : 0);
									}

									auto* teammateGreetInfo = ResolveTeammateGreetTopicInfo();
									if (!downedDialogueOpen) {
										teammateTalkTarget->SetDialogueWithPlayer(false, false, nullptr);
									}
									const bool opened = teammateTalkTarget->SetDialogueWithPlayer(true, true, teammateGreetInfo);

									spdlog::info(
										"[TFD][Menu] activate hard-open teammate dialogue target={:08X} opened={} refreshed={} source={} topicInfo={:08X} staleCombatClear={} playerTeammate={} activeFollower={} playerSide={} downedRecovery={}",
										teammateTalkTarget->GetFormID(),
										opened ? 1 : 0,
										refreshed ? 1 : 0,
										sourceReason ? sourceReason : "unknown",
										teammateGreetInfo ? teammateGreetInfo->GetFormID() : 0u,
										shouldClearStaleCombat ? 1 : 0,
										teammateTalkTarget->IsPlayerTeammate() ? 1 : 0,
										TFD::TeammateManager::IsActiveFollowerActor(teammateTalkTarget) ? 1 : 0,
										TFD::TeammateManager::IsPlayerSideTeammateActor(teammateTalkTarget) ? 1 : 0,
										downedDialogueOpen ? 1 : 0);

									if (opened) {
										if (downedDialogueOpen) {
											ArmTeammateManualDialogueCooldown(
												teammateTalkTarget,
												teammateNow,
												"hard_open_downed_teammate_recovery_dialogue",
												kDownedTeammateRecoveryGlobalCooldownSec,
												kDownedTeammateRecoveryActorCooldownSec);
										}
										else {
											ArmTeammateManualDialogueCooldown(teammateTalkTarget, teammateNow, "hard_open_teammate_manual_dialogue");
										}
										return true;
									}

									return false;
									};

								auto openVictoryDialogue = [&](RE::Actor* defeatedTalkTarget, const char* sourceReason) -> bool {
									if (!defeatedTalkTarget) {
										return false;
									}

									const auto victoryNow = Clock::now();
									const char* blockReason = "none";
									double blockRemainingSec = 0.0;
									if (IsVictoryManualDialogueBlocked(defeatedTalkTarget, victoryNow, blockReason, blockRemainingSec)) {
										spdlog::info(
											"[TFD][Menu] victory manual dialogue blocked target={:08X} source={} reason={} remaining={:.2f}s",
											defeatedTalkTarget->GetFormID(),
											sourceReason ? sourceReason : "victory_activate_dialogue",
											blockReason ? blockReason : "unknown",
											blockRemainingSec);
										return true;
									}

									TFD::TeammateManager::SetPendingDefeatedDialogueTarget(defeatedTalkTarget);
									TFD::Victory::ArmDialogueReadyHold(defeatedTalkTarget, kVictoryDialogueReadyHoldSec, sourceReason ? sourceReason : "victory_activate_dialogue");
									TFD::Victory::SetStateValue(2);
									const bool flowOk = TFD::FlowController::Controller::GetSingleton().RequestVictory(
										defeatedTalkTarget->GetFormID(),
										sourceReason ? sourceReason : "victory_activate_dialogue");
									spdlog::info("[TFD][Menu] activate victory flow request target={:08X} ok={} state=2 source={}",
										defeatedTalkTarget->GetFormID(),
										flowOk ? 1 : 0,
										sourceReason ? sourceReason : "victory_activate_dialogue");

									if (!defeatedTalkTarget->IsAIEnabled()) {
										defeatedTalkTarget->EnableAI(true);
									}
									defeatedTalkTarget->AllowPCDialogue(true);
									if (auto* process = RE::ProcessLists::GetSingleton()) {
										process->StopCombatAndAlarmOnActor(defeatedTalkTarget, false);
									}
									defeatedTalkTarget->EvaluatePackage(false, true);
									defeatedTalkTarget->EvaluatePackage(true, true);

									// RefreshObservedState may run in the same frame and downgrade VictoryState
									// to No while other enemies are still present. Force it back to Yes
									// immediately before opening so CK dialogue conditions stay valid.
									TFD::Victory::SetStateValue(2);

									auto* victoryGreetInfo = ResolveVictoryGreetTopicInfo();
									defeatedTalkTarget->SetDialogueWithPlayer(false, false, nullptr);
									const bool opened = defeatedTalkTarget->SetDialogueWithPlayer(true, true, victoryGreetInfo);
									if (opened) {
										TFD::Victory::SetStateValue(2);
										ArmVictoryManualDialogueCooldown(defeatedTalkTarget, victoryNow, "hard_open_victory_manual_dialogue");
									}
									spdlog::info("[TFD][Menu][R93O] activate opened defeated victory dialogue target={:08X} opened={} action=native_activation_dialogue source={} topicInfo={:08X} explicit={} reset=1",
										defeatedTalkTarget->GetFormID(),
										opened ? 1 : 0,
										sourceReason ? sourceReason : "victory_activate_dialogue",
										victoryGreetInfo ? victoryGreetInfo->GetFormID() : 0u,
										victoryGreetInfo ? 1 : 0);
									return opened;
									};

								// R55: the normal teammate dialogue opener must be crosshair-exact.
								// The R54 proximity scanner was too aggressive and could steal the E key
								// from containers/chests when a teammate stood beside the player.
								// If the crosshair has a non-actor activation target, never run TFD's
								// teammate/Victory scans; let vanilla activation handle that target.
								auto* crosshairRef = GetCrosshairTargetRef();
								auto* crosshairActor = crosshairRef ? crosshairRef->As<RE::Actor>() : nullptr;

								if (crosshairRef && !crosshairActor) {
									spdlog::info("[TFD][Menu] activate skipped TFD dialogue: crosshair target is non-actor ref={:08X} action=allow_vanilla_activation",
										crosshairRef->GetFormID());
								}
								else if (crosshairActor && crosshairActor != player) {
									if (TFD::TeammateManager::IsTFDManagedTeammateActor(crosshairActor)) {
										if (openTeammateDialogue(crosshairActor, "teammate_crosshair_activate_dialogue")) {
											return RE::BSEventNotifyControl::kStop;
										}
									}
									else if (TFD::Actor::Ops::IsDialogueCapableDefeatedEnemy(crosshairActor)) {
										if (openVictoryDialogue(crosshairActor, "victory_crosshair_activate_dialogue")) {
											return RE::BSEventNotifyControl::kStop;
										}
									}
									else if (IsInCombatTruceActivationActor(crosshairActor)) {
										(void)OpenInCombatTruceDialogueFromActivation(crosshairActor, "incombat_crosshair_activate_dialogue");
										return RE::BSEventNotifyControl::kStop;
									}
									else {
										spdlog::info("[TFD][Menu] activate crosshair actor not owned by TFD actor={:08X} action=allow_vanilla_activation",
											crosshairActor->GetFormID());
									}
								}
								else {
									// No crosshair target. Keep a conservative fallback for awkward downed
									// bodies and defeated enemies, but do not proximity-open standing
									// teammates anymore. Standing teammate dialogue requires direct crosshair.
									if (auto* downedTeammateTalkTarget = TFD::InteractionRouter::PickExactDownedTeammateDialogueTarget(768.0f)) {
										if (openTeammateDialogue(downedTeammateTalkTarget, "teammate_downed_fallback_activate_dialogue")) {
											return RE::BSEventNotifyControl::kStop;
										}
									}

									if (auto* defeatedTalkTarget = TFD::InteractionRouter::PickExactDialogueDefeatedTarget(512.0f)) {
										if (TFD::TeammateManager::IsTFDManagedTeammateActor(defeatedTalkTarget)) {
											spdlog::info("[TFD][Menu] activate redirected defeated target to teammate dialogue target={:08X} reason=player_side_teammate_fallback", defeatedTalkTarget->GetFormID());
											if (openTeammateDialogue(defeatedTalkTarget, "teammate_defeated_redirect_dialogue")) {
												return RE::BSEventNotifyControl::kStop;
											}
										}
										else if (openVictoryDialogue(defeatedTalkTarget, "victory_fallback_activate_dialogue")) {
											return RE::BSEventNotifyControl::kStop;
										}
									}
									else {
										spdlog::info("[TFD][Menu] activate TFD dialogue miss no_crosshair_target action=allow_vanilla_activation");
									}
								}
							}
						}
					}

					if (TFD::FeedPopup::IsOpen()) {
						const auto nowPopup = Clock::now();
						switch (code) {
						case kScanCodeUp:
							if (nowPopup >= nextPopupNav && TFD::FeedPopup::PrevSelection()) {
								nextPopupNav = nowPopup + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(kPopupNavDebounce));
								return RE::BSEventNotifyControl::kStop;
							}
							return RE::BSEventNotifyControl::kStop;
						case kScanCodeDown:
							if (nowPopup >= nextPopupNav && TFD::FeedPopup::NextSelection()) {
								nextPopupNav = nowPopup + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(kPopupNavDebounce));
								return RE::BSEventNotifyControl::kStop;
							}
							return RE::BSEventNotifyControl::kStop;
						case kScanCodeEnter:
						case kScanCodeNumpadEnter:
							if (nowPopup >= nextPopupConfirm && TFD::FeedPopup::ConfirmSelection()) {
								nextPopupConfirm = nowPopup + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(kPopupConfirmDebounce));
								return RE::BSEventNotifyControl::kStop;
							}
							return RE::BSEventNotifyControl::kStop;
						case kScanCodeEscape:
							if (nowPopup >= nextPopupConfirm && TFD::FeedPopup::CancelSelection()) {
								nextPopupConfirm = nowPopup + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(kPopupConfirmDebounce));
								return RE::BSEventNotifyControl::kStop;
							}
							return RE::BSEventNotifyControl::kStop;
						default:
							break;
						}

						if (code == TFD::Settings::GetHotkeyScanCode()) {
							RE::DebugNotification("TFD: Feed selection pending.");
							return RE::BSEventNotifyControl::kStop;
						}
					}

					if (!TFD::Settings::GetHotkeyEnabled()) {
						continue;
					}

					if (code != TFD::Settings::GetHotkeyScanCode()) {
						continue;
					}

					auto* ui = RE::UI::GetSingleton();
					if (ui) {
						if (ui->IsMenuOpen(RE::MainMenu::MENU_NAME) ||
							ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) ||
							ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME) ||
							ui->IsMenuOpen(RE::Console::MENU_NAME) ||
							ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME) ||
							ui->IsMenuOpen(RE::JournalMenu::MENU_NAME) ||
							ui->IsMenuOpen(RE::LockpickingMenu::MENU_NAME)) {
							continue;
						}
					}

					const auto now = Clock::now();
					if (now < nextHotkey) {
						continue;
					}
					nextHotkey = now + std::chrono::milliseconds((std::max)(0, TFD::Settings::GetHotkeyCooldownMs()));

					auto* player = RE::PlayerCharacter::GetSingleton();
					if (!player) {
						continue;
					}

					ResolveGlobals();
					ClearInteractionStateValue();

					if (TFD::FeedPopup::IsOpen()) {
						RE::DebugNotification("TFD: Feed selection pending.");
						continue;
					}

					const bool shiftDown = IsShiftDown();

					if (TFD::Settings::GetHotkeyWave()) {
						player->NotifyAnimationGraph("IdleWave");
					}

					const int dialogueStateRaw = GetGlobalValueInt(gDialogueState);
					const auto flowSnapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
					if (dialogueStateRaw == 1) {
						RE::DebugNotification("TFD: Dialogue Busy");
						continue;
					}

					const auto ownedFlowResult = TFD::InteractionRouter::HandleFlowOwnedPrimaryHotkey(player, flowSnapshot);
					if (ownedFlowResult.handled) {
						if (ownedFlowResult.interactionState != 0) {
							SetInteractionStateValue(ownedFlowResult.interactionState);
						}
						if (ownedFlowResult.notification) {
							RE::DebugNotification(ownedFlowResult.notification);
						}
						if (!ownedFlowResult.success && ownedFlowResult.interactionState != 6) {
							ClearInteractionStateValue();
						}
						continue;
					}

					if (shiftDown) {
						const auto shiftResult = TFD::InteractionRouter::ExecuteShiftHotkey(
							player,
							NowSec(),
							1400.0f);

						if (!shiftResult.handled) {
							RE::DebugNotification("TFD: Interaction Failed");
							ClearInteractionStateValue();
							continue;
						}

						if (shiftResult.interactionState != 0) {
							SetInteractionStateValue(shiftResult.interactionState);
						}

						if (shiftResult.action == TFD::InteractionRouter::ShiftHotkeyAction::OpenFeedPopup &&
							shiftResult.success &&
							shiftResult.target) {
							if (!TFD::FeedPopup::Open(shiftResult.target)) {
								RE::DebugNotification("TFD: No Available Tame Commands");
								ClearInteractionStateValue();
							}
							continue;
						}

						if (shiftResult.notification) {
							RE::DebugNotification(shiftResult.notification);
						}

						if (!shiftResult.success) {
							ClearInteractionStateValue();
						}

						continue;
					}

					const auto primaryResult = TFD::InteractionRouter::ExecutePrimaryHotkey(
						player,
						flowSnapshot,
						NowSec(),
						3500.0f,
						true);

					if (!primaryResult.handled) {
						RE::DebugNotification("TFD: Interaction Failed");
						ClearInteractionStateValue();
						continue;
					}

					if (primaryResult.interactionState != 0) {
						SetInteractionStateValue(primaryResult.interactionState);
					}

					if (primaryResult.notification) {
						RE::DebugNotification(primaryResult.notification);
					}

					if (!primaryResult.success) {
						ClearInteractionStateValue();
					}

					continue;
				}

				return RE::BSEventNotifyControl::kContinue;
			}
		};

		static InputSink gInputSink;

		static void TryInstallInputSink()
		{
			if (inputSinkAdded.load()) {
				return;
			}

			auto* input = RE::BSInputDeviceManager::GetSingleton();
			if (!input) {
				return;
			}

			input->AddEventSink(&gInputSink);
			inputSinkAdded.store(true);
			spdlog::info("[TFD][Menu] InputSink added");
			RE::DebugNotification("TFD: Hotkey input ready");
		}

		static void StartInputRetryThread()
		{
			if (inputRetryThreadStarted.exchange(true)) {
				return;
			}

			std::thread([]() {
				for (int i = 0; i < 120 && !inputSinkAdded.load(); ++i) {
					std::this_thread::sleep_for(std::chrono::milliseconds(500));

					auto* tasks = SKSE::GetTaskInterface();
					if (!tasks) {
						continue;
					}

					tasks->AddUITask([]() {
						TryInstallInputSink();
						});
				}
				}).detach();
		}

		static void OnSkseMessage(SKSE::MessagingInterface::Message* m)
		{
			if (!m) {
				return;
			}

			if (m->type == SKSE::MessagingInterface::kDataLoaded) {
				TryRegisterMenu();
				ResolveGlobals();
				TryInstallInputSink();
				StartInputRetryThread();
			}

			if (m->type == SKSE::MessagingInterface::kInputLoaded) {
				TryInstallInputSink();
				StartInputRetryThread();
			}

			if (m->type == SKSE::MessagingInterface::kPreLoadGame) {
				ResolveGlobals();
				TFD::FeedPopup::Close();
				TFD::PreCombatGreet::OnPreLoadGame();
				TFD::DefeatMonitor::SetLoadTransition(true);
				spdlog::info("[TFD][Menu] PreLoadGame -> prepare only");
			}

			if (m->type == SKSE::MessagingInterface::kPostLoadGame ||
				m->type == SKSE::MessagingInterface::kNewGame) {
				ResolveGlobals();
				TFD::FeedPopup::Close();
				TFD::PreCombatGreet::OnPostLoadGame();
				spdlog::info("[TFD][Menu] PostLoad/NewGame -> bridge refresh only");
			}
		}
	}

	void Init()
	{
		if (started) {
			return;
		}
		started = true;

		if (!TFD::Settings::IsInitialized()) {
			spdlog::warn("[TFD][Menu] Settings were not initialized before menu init");
		}

		spdlog::info("[TFD][Menu] Init()");

		TFD::PreCombatGreet::Install();

		TryRegisterMenu();
		ResolveGlobals();
		TryInstallInputSink();
		StartInputRetryThread();

		if (auto* msg = SKSE::GetMessagingInterface()) {
			msg->RegisterListener(OnSkseMessage);
		}
		else {
			spdlog::warn("[TFD][Menu] MessagingInterface null");
		}
	}
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif
