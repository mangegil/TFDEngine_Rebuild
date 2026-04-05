// src/MenuFramework.cpp
#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

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

#include <spdlog/spdlog.h>

#include "TFDSettings.h"
#include "TFDLocation.h"
#include "TFDActorScan.h"
#include "TFDAntiAggro.h"
#include "TFDForceGreet.h"
#include "TFDPreCombatGreet.h"
#include "TFDDefeatMonitor.h"
#include "TFDTargetClassifier.h"
#include "TFDInteractionRouter.h"
#include "TFDPacify.h"
#include "TFDTameBait.h"
#include "TFDFeedPopup.h"
#include "TFDCompanionRestore.h"
#include "TFDFlowController.h"
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

		static std::vector<TFD::Pacify::ActiveTameSnapshot> gCreatureTeammateMenuRows{};
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
			ResolveGlobals();
			SetGlobalInt(gInteractionState, value);
		}

		static void ClearInteractionStateValue()
		{
			SetInteractionStateValue(0);
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
				return "Not in Combat";
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
			TFD::Pacify::Update(now);
			gCreatureTeammateMenuRows = TFD::Pacify::GetActiveTameSnapshots(now);
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
					[&](const TFD::Pacify::ActiveTameSnapshot& snap) { return snap.sessionId == sessionId; });
				};
			if (!keepSession(gCreatureTeammateFeedSessionId)) {
				gCreatureTeammateFeedSessionId = 0;
			}
			if (!keepSession(gCreatureTeammateReleaseConfirmSessionId)) {
				gCreatureTeammateReleaseConfirmSessionId = 0;
			}
		}

		static double GetDisplayRemainingTameSec(const TFD::Pacify::ActiveTameSnapshot& snap)
		{
			const double elapsed = NowSec() - gCreatureTeammateMenuRefreshRealSec;
			return (std::max)(0.0, snap.remainingTameSec - elapsed);
		}

		static double GetDisplayRemainingCompanionHours(const TFD::Pacify::ActiveTameSnapshot& snap)
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

		static std::string FormatCreatureTimer(const TFD::Pacify::ActiveTameSnapshot& snap)
		{
			if (snap.disposition == TFD::Pacify::TameDisposition::Companion) {
				return FormatCountdownClock(GetDisplayRemainingCompanionHours(snap) * 3600.0);
			}

			return FormatCountdownClock(GetDisplayRemainingTameSec(snap));
		}

		static const char* CreatureStateLabel(const TFD::Pacify::ActiveTameSnapshot& snap)
		{
			switch (snap.disposition) {
			case TFD::Pacify::TameDisposition::Companion:
				return "Teammate";
			case TFD::Pacify::TameDisposition::Calm:
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

		// Source of truth captive = native defeat monitor
		static bool IsCaptivePhase()
		{
			return TFD::DefeatMonitor::IsCaptivePhase();
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

		static void SendBridgeEvent(const char* eventName)
		{
			if (!eventName) {
				return;
			}

			auto* src = SKSE::GetModCallbackEventSource();
			if (!src) {
				return;
			}

			SKSE::ModCallbackEvent e(eventName, "", 0.0f, nullptr);
			src->SendEvent(&e);
		}

		static void SendBridgeAssignActor(const char* eventName, RE::Actor* actor)
		{
			if (!eventName || !actor) {
				return;
			}

			auto* src = SKSE::GetModCallbackEventSource();
			if (!src) {
				return;
			}

			SKSE::ModCallbackEvent e(eventName, "", 0.0f, actor);
			src->SendEvent(&e);
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

		static bool IsMenuActorSameBleedSpace(RE::Actor* actor, RE::Actor* player)
		{
			if (!actor || !player) {
				return false;
			}

			auto* actorCell = actor->GetParentCell();
			auto* playerCell = player->GetParentCell();
			if (!actorCell || !playerCell) {
				return false;
			}

			const bool actorInterior = actorCell->IsInteriorCell();
			const bool playerInterior = playerCell->IsInteriorCell();
			if (actorInterior != playerInterior) {
				return false;
			}

			if (playerInterior) {
				return actorCell == playerCell;
			}

			auto* actorWs = actor->GetWorldspace();
			auto* playerWs = player->GetWorldspace();
			return actorWs && playerWs && actorWs == playerWs;
		}

		static bool MenuActorHasLineOfSightToPlayer(RE::Actor* actor, RE::Actor* player)
		{
			if (!actor || !player) {
				return false;
			}
			bool hasLOSData = false;
			return actor->HasLineOfSight(player, hasLOSData);
		}

		static bool IsCaptorPickerSupportedActor(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || actor == player) {
				return false;
			}
			if (actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
				return false;
			}
			if (actor->IsPlayerTeammate() || TFD::Pacify::IsCompanion(actor)) {
				return false;
			}
			const bool isNPC = actor->HasKeywordString("ActorTypeNPC");
			const bool isCreature = actor->HasKeywordString("ActorTypeCreature");
			return isNPC || isCreature;
		}

		static RE::Actor* PickCaptorSameCellLoaded(float radius)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return nullptr;
			}

			const float searchRadius = (std::max)(radius, 12288.0f);
			TFD::ActorScan::Rescan(searchRadius, false);

			RE::Actor* best = nullptr;
			float bestScore = 1.0e30f;

			const auto n = TFD::ActorScan::GetCount();
			for (int i = 0; i < n; ++i) {
				auto e = TFD::ActorScan::GetEntry(i);
				auto* a = TFD::ActorScan::GetActor(i);
				if (!a) continue;
				if (!IsCaptorPickerSupportedActor(a)) continue;
				if (!IsMenuActorSameBleedSpace(a, player)) continue;
				if (e.dist > searchRadius) continue;
				if (!MenuActorHasLineOfSightToPlayer(a, player)) continue;

				float score = e.dist;
				if (e.hostile || a->IsHostileToActor(player)) score -= 140.0f;
				if (e.inCombat || a->IsInCombat()) score -= 100.0f;
				if (score < bestScore) {
					bestScore = score;
					best = a;
				}
			}

			return best;
		}

		static void ApplyCellHotkeyCalmBubble(RE::Actor* player, RE::Actor* primaryTarget, float radius)
		{
			if (!player || !primaryTarget) {
				return;
			}

			const float sweepRadius = (std::max)(radius, (std::max)(TFD::Settings::GetSweepRadius(), 12000.0f));
			TFD::AntiAggro::SweepOnce(sweepRadius, true);
			TFD::AntiAggro::ScheduleWaves(sweepRadius, true, 10, 120);
			TFD::ActorScan::Rescan(sweepRadius, false);

			auto* pCell = player->GetParentCell();
			const auto count = TFD::ActorScan::GetCount();
			for (int i = 0; i < count; ++i) {
				auto entry = TFD::ActorScan::GetEntry(i);
				auto* actor = TFD::ActorScan::GetActor(i);
				if (!actor || actor->IsDead() || actor->IsDisabled()) {
					continue;
				}
				if (!actor->Is3DLoaded()) {
					continue;
				}
				if (actor->GetFormID() == player->GetFormID()) {
					continue;
				}
				if (pCell && actor->GetParentCell() != pCell) {
					continue;
				}
				if (actor->GetFormID() != primaryTarget->GetFormID() &&
					!entry.hostile &&
					!entry.inCombat) {
					continue;
				}

				if (auto* process = RE::ProcessLists::GetSingleton()) {
					const bool runDetection = process->runDetection;
					process->runDetection = false;
					process->ClearCachedFactionFightReactions();
					process->StopCombatAndAlarmOnActor(actor, false);
					process->runDetection = runDetection;
				}
				actor->StopCombat();
				if (actor->IsWeaponDrawn()) {
					actor->DrawWeaponMagicHands(false);
				}
				actor->EvaluatePackage(true, false);
			}

			spdlog::info("[TFD][Hotkey] cell calm bubble primary={:08X} radius={:.0f}", primaryTarget->GetFormID(), sweepRadius);
		}


		static float GetActorFrontDot2D(RE::Actor* a, RE::PlayerCharacter* player)
		{
			if (!a || !player) {
				return -1.0f;
			}

			const auto pa = player->GetPosition();
			const auto pb = a->GetPosition();

			const float dx = pb.x - pa.x;
			const float dy = pb.y - pa.y;
			const float d2 = dx * dx + dy * dy;
			if (d2 <= 1.0f) {
				return 1.0f;
			}

			const float len = std::sqrt(d2);
			const float ang = player->GetAngleZ();
			const float fx = std::sin(ang);
			const float fy = std::cos(ang);
			const float nx = dx / len;
			const float ny = dy / len;
			return nx * fx + ny * fy;
		}

		static bool IsActorCloseAndFront(RE::Actor* a, RE::PlayerCharacter* player, float maxDist)
		{
			if (!a || !player) {
				return false;
			}

			const auto pa = player->GetPosition();
			const auto pb = a->GetPosition();

			const float dx = pb.x - pa.x;
			const float dy = pb.y - pa.y;
			const float d2 = dx * dx + dy * dy;
			if (d2 > (maxDist * maxDist)) {
				return false;
			}

			return GetActorFrontDot2D(a, player) >= 0.20f;
		}

		enum class HotkeyPickMode
		{
			None = 0,
			TrucePreCombat,
			Tame,
			TruceInCombat
		};

		static int InteractionStateForPickMode(HotkeyPickMode mode)
		{
			switch (mode) {
			case HotkeyPickMode::TrucePreCombat:
				return 1;
			case HotkeyPickMode::TruceInCombat:
				return 2;
			case HotkeyPickMode::Tame:
				return 3;
			default:
				return 0;
			}
		}

		static bool IsNonHostileActiveTameFollower(RE::Actor* actor, const TFD::ActorScan::Entry& entry)
		{
			if (!actor) {
				return false;
			}

			if (!TFD::Pacify::IsPacified(actor)) {
				return false;
			}

			if (TFD::Pacify::GetMode(actor) != TFD::Pacify::Mode::Tame) {
				return false;
			}

			const bool inCombat = actor->IsInCombat() || entry.inCombat;
			return !entry.hostile && !inCombat;
		}

		static bool IsShiftDown()
		{
			return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 ||
				(GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0 ||
				(GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
		}

		static RE::Actor* PickExactDialogueDefeatedTargetSameCellLoaded(float radius)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return nullptr;
			}

			TFD::ActorScan::Rescan(radius, false);

			RE::Actor* best = nullptr;
			float bestScore = -1.0e30f;

			const auto count = TFD::ActorScan::GetCount();
			for (int i = 0; i < count; ++i) {
				auto entry = TFD::ActorScan::GetEntry(i);
				auto* actor = TFD::ActorScan::GetActor(i);
				if (!actor) {
					continue;
				}
				if (actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
					continue;
				}
				if (entry.dist > radius) {
					continue;
				}
				if (actor->GetParentCell() != player->GetParentCell()) {
					continue;
				}
				if (!TFD::DefeatMonitor::IsDialogueCapableDefeatedEnemy(actor)) {
					continue;
				}

				const float frontDot = GetActorFrontDot2D(actor, player);
				if (frontDot < 0.75f) {
					continue;
				}

				float score = (frontDot * 100000.0f) - entry.dist;
				if (frontDot >= 0.96f) {
					score += 4000.0f;
				}
				else if (frontDot >= 0.90f) {
					score += 2000.0f;
				}

				if (score > bestScore) {
					bestScore = score;
					best = actor;
				}
			}

			return best;
		}

		static RE::Actor* PickExactActiveTameTargetSameCellLoaded(float radius)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return nullptr;
			}

			auto scoreActor = [&](RE::Actor* actor, const TFD::ActorScan::Entry& entry) -> float {
				if (!actor) {
					return -1.0e30f;
				}
				if (actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
					return -1.0e30f;
				}
				if (!TFD::Pacify::HasActiveTameSession(actor)) {
					return -1.0e30f;
				}
				if (entry.dist > radius) {
					return -1.0e30f;
				}

				const float frontDot = GetActorFrontDot2D(actor, player);
				if (frontDot < 0.80f) {
					return -1.0e30f;
				}

				float score = (frontDot * 100000.0f) - entry.dist;
				if (frontDot >= 0.98f) {
					score += 6000.0f;
				}
				else if (frontDot >= 0.94f) {
					score += 3500.0f;
				}
				else if (frontDot >= 0.90f) {
					score += 1500.0f;
				}
				if (actor->IsInCombat() || entry.inCombat) {
					score += 50.0f;
				}
				return score;
				};

			TFD::ActorScan::Rescan(radius, false);

			RE::Actor* best = nullptr;
			float bestScore = -1.0e30f;

			const auto count = TFD::ActorScan::GetCount();
			for (int i = 0; i < count; ++i) {
				auto entry = TFD::ActorScan::GetEntry(i);
				auto* actor = TFD::ActorScan::GetActor(i);
				const float score = scoreActor(actor, entry);
				if (score > bestScore) {
					bestScore = score;
					best = actor;
				}
			}

			if (!best) {
				const auto restored = TFD::CompanionRestore::RestoreNow();
				if (restored > 0) {
					TFD::ActorScan::Rescan(radius, false);

					const auto retryCount = TFD::ActorScan::GetCount();
					for (int i = 0; i < retryCount; ++i) {
						auto entry = TFD::ActorScan::GetEntry(i);
						auto* actor = TFD::ActorScan::GetActor(i);
						const float score = scoreActor(actor, entry);
						if (score > bestScore) {
							bestScore = score;
							best = actor;
						}
					}
				}
			}

			return best;
		}


		static RE::Actor* PickExactDefeatedCreatureTargetSameCellLoaded(float radius)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return nullptr;
			}

			auto scoreActor = [&](RE::Actor* actor, const TFD::ActorScan::Entry& entry) -> float {
				if (!actor) {
					return -1.0e30f;
				}
				if (actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
					return -1.0e30f;
				}
				if (!TFD::DefeatMonitor::IsCreatureDefeatedEnemy(actor)) {
					return -1.0e30f;
				}
				if (TFD::DefeatMonitor::GetDefeatedEnemyRemainingSeconds(actor) <= 0.0) {
					return -1.0e30f;
				}
				if (entry.dist > radius) {
					return -1.0e30f;
				}

				const float frontDot = GetActorFrontDot2D(actor, player);
				if (frontDot < 0.80f) {
					return -1.0e30f;
				}

				float score = (frontDot * 100000.0f) - entry.dist;
				if (frontDot >= 0.98f) {
					score += 6000.0f;
				}
				else if (frontDot >= 0.94f) {
					score += 3500.0f;
				}
				else if (frontDot >= 0.90f) {
					score += 1500.0f;
				}
				return score;
				};

			TFD::ActorScan::Rescan(radius, false);

			RE::Actor* best = nullptr;
			float bestScore = -1.0e30f;

			const auto count = TFD::ActorScan::GetCount();
			for (int i = 0; i < count; ++i) {
				auto entry = TFD::ActorScan::GetEntry(i);
				auto* actor = TFD::ActorScan::GetActor(i);
				const float score = scoreActor(actor, entry);
				if (score > bestScore) {
					bestScore = score;
					best = actor;
				}
			}

			return best;
		}

		static float ScoreTruceCandidate(
			RE::Actor* actor,
			RE::PlayerCharacter* player,
			const TFD::ActorScan::Entry& entry,
			HotkeyPickMode* outMode)
		{
			if (outMode) {
				*outMode = HotkeyPickMode::None;
			}

			if (!actor || !player) {
				return -1.0e30f;
			}
			if (TFD::DefeatMonitor::IsDefeatedEnemyKnocked(actor)) {
				return -1.0e30f;
			}

			const bool front = IsActorCloseAndFront(actor, player, 1400.0f);
			const bool inCombat = actor->IsInCombat();
			const bool weaponDrawn = actor->IsWeaponDrawn();

			const auto classify = TFD::TargetClassifier::ClassifyForHotkey(
				player,
				actor,
				false,
				inCombat,
				entry.dist);

			if (!classify.valid ||
				classify.intent != TFD::TargetClassifier::InteractionIntent::Truce) {
				return -1.0e30f;
			}

			if (!inCombat &&
				front &&
				entry.dist <= 1150.0f) {
				if (outMode) {
					*outMode = HotkeyPickMode::TrucePreCombat;
				}

				float score = 50000.0f;
				score -= entry.dist;
				if (weaponDrawn) {
					score += 900.0f;
				}
				if (entry.hostile) {
					score += 350.0f;
				}
				if (classify.allowDialogue) {
					score += 250.0f;
				}
				return score;
			}

			if (inCombat &&
				entry.dist <= 1400.0f) {
				if (outMode) {
					*outMode = HotkeyPickMode::TruceInCombat;
				}

				float score = 20000.0f;
				score -= entry.dist;
				if (front) {
					score += 500.0f;
				}
				if (entry.hostile) {
					score += 150.0f;
				}
				if (classify.allowDialogue) {
					score += 250.0f;
				}
				return score;
			}

			return -1.0e30f;
		}

		static float ScoreTameCandidate(
			RE::Actor* actor,
			RE::PlayerCharacter* player,
			const TFD::ActorScan::Entry& entry,
			HotkeyPickMode* outMode)
		{
			if (outMode) {
				*outMode = HotkeyPickMode::None;
			}

			if (!actor || !player) {
				return -1.0e30f;
			}
			if (TFD::DefeatMonitor::IsDefeatedEnemyKnocked(actor)) {
				return -1.0e30f;
			}

			if (IsNonHostileActiveTameFollower(actor, entry)) {
				return -1.0e30f;
			}

			const bool front = IsActorCloseAndFront(actor, player, 1400.0f);
			const bool inCombat = actor->IsInCombat() || entry.inCombat;
			const bool weaponDrawn = actor->IsWeaponDrawn();

			const auto classify = TFD::TargetClassifier::ClassifyForHotkey(
				player,
				actor,
				false,
				inCombat,
				entry.dist);

			if (!classify.valid ||
				classify.intent != TFD::TargetClassifier::InteractionIntent::Tame) {
				return -1.0e30f;
			}

			if (entry.dist > 768.0f) {
				return -1.0e30f;
			}

			const bool combatRelevant = entry.hostile || inCombat;
			if (!combatRelevant) {
				return -1.0e30f;
			}

			if (outMode) {
				*outMode = HotkeyPickMode::Tame;
			}

			float score = inCombat ? 32000.0f : 30000.0f;
			score -= entry.dist;
			if (front) {
				score += 300.0f;
			}
			if (entry.hostile) {
				score += 250.0f;
			}
			if (weaponDrawn) {
				score += 350.0f;
			}
			return score;
		}

		static RE::Actor* PickBestHotkeyCandidateForMode(
			RE::PlayerCharacter* player,
			HotkeyPickMode desiredMode,
			HotkeyPickMode* outMode)
		{
			if (outMode) {
				*outMode = HotkeyPickMode::None;
			}

			if (!player) {
				return nullptr;
			}

			RE::Actor* best = nullptr;
			HotkeyPickMode bestMode = HotkeyPickMode::None;
			float bestScore = -1.0e30f;

			const auto count = TFD::ActorScan::GetCount();
			for (int i = 0; i < count; ++i) {
				auto entry = TFD::ActorScan::GetEntry(i);
				auto* actor = TFD::ActorScan::GetActor(i);
				if (!actor) {
					continue;
				}
				if (actor->IsDead() || actor->IsDisabled()) {
					continue;
				}
				if (!actor->Is3DLoaded()) {
					continue;
				}

				HotkeyPickMode mode = HotkeyPickMode::None;
				float score = -1.0e30f;

				if (desiredMode == HotkeyPickMode::Tame) {
					score = ScoreTameCandidate(actor, player, entry, &mode);
				}
				else {
					score = ScoreTruceCandidate(actor, player, entry, &mode);
				}

				if (desiredMode != HotkeyPickMode::None && mode != desiredMode) {
					continue;
				}

				if (score > bestScore) {
					bestScore = score;
					best = actor;
					bestMode = mode;
				}
			}

			if (outMode) {
				*outMode = bestMode;
			}

			return best;
		}

		static RE::Actor* PickPreCombatTargetSameCellLoaded(float radius, HotkeyPickMode* outMode)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (outMode) {
				*outMode = HotkeyPickMode::None;
			}
			if (!player) {
				return nullptr;
			}

			TFD::ActorScan::Rescan(radius, false);

			HotkeyPickMode truceMode = HotkeyPickMode::None;
			if (auto* truceTarget = PickBestHotkeyCandidateForMode(player, HotkeyPickMode::TrucePreCombat, &truceMode)) {
				if (truceMode == HotkeyPickMode::TrucePreCombat ||
					truceMode == HotkeyPickMode::TruceInCombat) {
					if (outMode) {
						*outMode = truceMode;
					}
					return truceTarget;
				}
			}

			HotkeyPickMode tameMode = HotkeyPickMode::None;
			if (auto* tameTarget = PickBestHotkeyCandidateForMode(player, HotkeyPickMode::Tame, &tameMode)) {
				if (outMode) {
					*outMode = tameMode;
				}
				return tameTarget;
			}

			return nullptr;
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

		static void RenderInlineFeedChoices(RE::Actor* actor, const TFD::Pacify::ActiveTameSnapshot& snap)
		{
			if (!actor) {
				ImGuiMCP::Text("Feed unavailable: creature not loaded.");
				return;
			}

			const bool downed = TFD::DefeatMonitor::IsThresholdDownedActor(actor);
			if (downed) {
				ImGuiMCP::Text("This creature is downed. Feeding will revive and heal it.");
			}

			auto renderFeedGroup = [&](const char* title, TFD::Pacify::FeedAction action) {
				auto options = TFD::Pacify::GetActiveTameFeedOptions(actor, action);
				ImGuiMCP::Text("%s", title);
				if (options.empty()) {
					ImGuiMCP::Text("  No valid bait.");
					return;
				}
				for (const auto& opt : options) {
					char buttonLabel[256];
					std::snprintf(buttonLabel, sizeof(buttonLabel), "%s##feed_%08X_%u_%08X", opt.label.c_str(), snap.actorId, snap.sessionId, opt.itemId);
					if (ImGuiMCP::Button(buttonLabel)) {
						if (TFD::Pacify::ApplyActiveTameFeed(actor, opt.itemId, action)) {
							if (action == TFD::Pacify::FeedAction::Teammate) {
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
			if (snap.disposition == TFD::Pacify::TameDisposition::Companion) {
				renderFeedGroup("Teammate Feed", TFD::Pacify::FeedAction::Teammate);
			}
			else {
				renderFeedGroup("Calm Feed", TFD::Pacify::FeedAction::Calm);
				ImGuiMCP::Separator();
				renderFeedGroup("Teammate Feed", TFD::Pacify::FeedAction::Teammate);
			}
			if (ImGuiMCP::Button((std::string("Close Feed##") + std::to_string(snap.sessionId)).c_str())) {
				gCreatureTeammateFeedSessionId = 0;
			}
			ImGuiMCP::Unindent();
		}

		static void RenderInlineReleaseConfirm(RE::Actor* actor, const TFD::Pacify::ActiveTameSnapshot& snap)
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
					released = TFD::Pacify::ReleaseActiveTameActor(actor, TFD::Pacify::ReleaseReason::Generic);
				}
				else {
					TFD::Pacify::ReleaseSession(snap.sessionId, TFD::Pacify::ReleaseReason::Generic);
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
			const auto snap = TFD::Flow::Controller::GetSingleton().GetSnapshot();
			ImGuiMCP::Text("Root: %s", TFD::Flow::Controller::ToString(snap.root));
			ImGuiMCP::Text("Context Root: %s", TFD::Flow::Controller::ToString(snap.contextRoot));
			ImGuiMCP::Text("Gate: %s", TFD::Flow::Controller::ToString(snap.gate));
			ImGuiMCP::Text("SubFlow: %s", TFD::Flow::Controller::ToString(snap.sub));
			ImGuiMCP::Text("Captive Mode: %s", TFD::Flow::Controller::ToString(snap.captiveMode));
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
				TFD::AntiAggro::ScheduleWaves(TFD::Settings::GetSweepRadius(), true, 6, 180);
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
							if (player && !TFD::DefeatMonitor::IsCaptivePhase()) {
								if (auto* defeatedTalkTarget = PickExactDialogueDefeatedTargetSameCellLoaded(220.0f)) {
									spdlog::info("[TFD][Menu] activate intercepted for defeated dialogue target={:08X}", defeatedTalkTarget->GetFormID());
									RE::DebugNotification("TFD: Defeated Dialogue");
									return RE::BSEventNotifyControl::kStop;
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
					const int captiveStateRaw = GetGlobalValueInt(gCaptiveState);
					const int defeatStateRaw = GetGlobalValueInt(gDefeatState);
					const int preCombatStateRaw = GetGlobalValueInt(gPreCombatState);
					const int inCombatStateRaw = GetGlobalValueInt(gInCombatState);
					const int recoveryStateRaw = GetGlobalValueInt(gRecoveryState);
					const int leftForDeadStateRaw = GetGlobalValueInt(gLeftForDeadState);
					const int rescueStateRaw = GetGlobalValueInt(gRescueState);
					const int pleasureStateRaw = GetGlobalValueInt(gPleasureState);

					if (dialogueStateRaw == 1) {
						RE::DebugNotification("TFD: Dialogue Busy");
						continue;
					}

					if (recoveryStateRaw != 0 || leftForDeadStateRaw != 0 || rescueStateRaw != 0 || pleasureStateRaw != 0) {
						RE::DebugNotification("TFD: Busy");
						continue;
					}

					if (captiveStateRaw == 2) {
						SetInteractionStateValue(6);
						RE::DebugNotification("TFD: Escape");
						continue;
					}

					if (defeatStateRaw == 2) {
						SetInteractionStateValue(5);
						if (TFD::DefeatMonitor::HandleBleedoutHotkey()) {
							RE::DebugNotification("TFD: BleedOut Truce");
						}
						else {
							RE::DebugNotification("TFD: No Response");
							ClearInteractionStateValue();
						}
						continue;
					}

					if (captiveStateRaw == 1) {
						auto* captor = PickCaptorSameCellLoaded(12288.0f);
						if (!captor) {
							RE::DebugNotification("TFD: No Response");
							ClearInteractionStateValue();
							continue;
						}

						SetInteractionStateValue(4);
						ApplyCellHotkeyCalmBubble(player, captor, 12288.0f);
						SendBridgeEvent("TFDCaptiveClearAll");
						SendBridgeAssignActor("TFDCaptiveAssign", captor);

						RE::DebugNotification("TFD: Calling Captor");
						continue;
					}

					if (shiftDown) {
						if (auto* defeatedCreature = PickExactDefeatedCreatureTargetSameCellLoaded(1400.0f)) {
							if (TFD::DefeatMonitor::RecruitDefeatedCreatureAsTeammate(defeatedCreature, NowSec())) {
								RE::DebugNotification("TFD: Defeated Creature Recruited");
							}
							else {
								RE::DebugNotification("TFD: Defeated Recruit Failed");
							}
							continue;
						}

						auto* tameTarget = PickExactActiveTameTargetSameCellLoaded(1400.0f);
						if (!tameTarget) {
							RE::DebugNotification("TFD: No Exact Tame Target");
							continue;
						}

						SetInteractionStateValue(3);
						if (!TFD::FeedPopup::Open(tameTarget)) {
							RE::DebugNotification("TFD: No Available Tame Commands");
							ClearInteractionStateValue();
						}
						continue;
					}

					HotkeyPickMode pickMode = HotkeyPickMode::None;
					RE::Actor* target = nullptr;

					if (preCombatStateRaw == 1) {
						TFD::ActorScan::Rescan(3500.0f, false);
						target = PickBestHotkeyCandidateForMode(player, HotkeyPickMode::TrucePreCombat, &pickMode);
					}
					else if (inCombatStateRaw == 1) {
						TFD::ActorScan::Rescan(3500.0f, false);
						target = PickBestHotkeyCandidateForMode(player, HotkeyPickMode::TruceInCombat, &pickMode);
					}
					else {
						target = PickPreCombatTargetSameCellLoaded(3500.0f, &pickMode);
					}

					if (!target || pickMode == HotkeyPickMode::None) {
						RE::DebugNotification("TFD: No Valid Target");
						ClearInteractionStateValue();
						continue;
					}

					if (pickMode == HotkeyPickMode::Tame) {
						SetInteractionStateValue(3);
						const auto exec = TFD::InteractionRouter::HandleHotkeyPress(
							player,
							target,
							false,
							NowSec());

						if (!exec.executed) {
							if (exec.failReason == TFD::InteractionRouter::FailReason::TameAlreadyActive) {
								RE::DebugNotification("TFD: Already Tamed. Use Shift+H to Feed");
							}
							else if (exec.failReason == TFD::InteractionRouter::FailReason::NoValidBait) {
								RE::DebugNotification("TFD: No Valid Bait");
							}
							else if (exec.action == TFD::InteractionRouter::Action::Tame &&
								exec.failReason == TFD::InteractionRouter::FailReason::SessionBeginFailed) {
								RE::DebugNotification("TFD: Pack Tame Failed");
							}
							else {
								RE::DebugNotification("TFD: Interaction Failed");
							}
							ClearInteractionStateValue();
							continue;
						}

						switch (exec.action) {
						case TFD::InteractionRouter::Action::Tame:
							RE::DebugNotification("TFD: Tame");
							break;
						case TFD::InteractionRouter::Action::None:
						default:
							RE::DebugNotification("TFD: Tame Started");
							break;
						}
						continue;
					}

					TFD::InteractionRouter::Action truceAction = TFD::InteractionRouter::Action::None;
					SetInteractionStateValue(InteractionStateForPickMode(pickMode));
					const bool truceStarted = TFD::PreCombatGreet::BeginForActor(target, &truceAction);
					if (!truceStarted) {
						RE::DebugNotification("TFD: Truce Failed");
						ClearInteractionStateValue();
						continue;
					}

					switch (truceAction) {
					case TFD::InteractionRouter::Action::TrucePreCombat:
						SetInteractionStateValue(1);
						RE::DebugNotification("TFD: PreCombat Truce");
						break;
					case TFD::InteractionRouter::Action::TruceInCombat:
						SetInteractionStateValue(2);
						RE::DebugNotification("TFD: InCombat Truce");
						break;
					case TFD::InteractionRouter::Action::Tame:
						SetInteractionStateValue(3);
						RE::DebugNotification("TFD: Tame");
						break;
					case TFD::InteractionRouter::Action::None:
					default:
						if (pickMode == HotkeyPickMode::TruceInCombat) {
							SetInteractionStateValue(2);
							RE::DebugNotification("TFD: InCombat Truce");
						}
						else {
							SetInteractionStateValue(1);
							RE::DebugNotification("TFD: PreCombat Truce");
						}
						break;
					}
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

		TFD::Settings::InitProfileIniPersistence();

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
