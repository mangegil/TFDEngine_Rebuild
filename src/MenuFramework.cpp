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

#include <spdlog/spdlog.h>

#include "TFDSettings.h"
#include "TFDLocation.h"
#include "TFDActorScan.h"
#include "TFDAntiAggro.h"
#include "TFDForceGreet.h"
#include "TFDPreCombatGreet.h"
#include "TFDDefeatMonitor.h"
#include "TFDTargetClassifier.h"
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

namespace TFDMenu
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		static bool started = false;
		static bool menuRegistered = false;

		static std::atomic_bool inputSinkAdded{ false };
		static std::atomic_bool inputRetryThreadStarted{ false };

		static bool  uiEnabled = true;
		static float uiThreshold = 30.0f;
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

		static RE::TESGlobal* gCaptiveState = nullptr;
		static RE::TESGlobal* gCaptivePhase = nullptr;
		static RE::TESGlobal* gPreCombatState = nullptr;
		static bool gLoggedCaptiveFound = false;
		static bool gLoggedCaptivePhaseFound = false;
		static bool gLoggedPreCombatFound = false;

		static void ResolveGlobals()
		{
			if (!gCaptiveState) {
				gCaptiveState = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDCaptiveState");
				if (gCaptiveState && !gLoggedCaptiveFound) {
					gLoggedCaptiveFound = true;
					spdlog::info("[TFD][Menu] TFDCaptiveState resolved {:08X}", gCaptiveState->GetFormID());
				}
			}

			if (!gCaptivePhase) {
				gCaptivePhase = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDCaptivePhase");
				if (gCaptivePhase && !gLoggedCaptivePhaseFound) {
					gLoggedCaptivePhaseFound = true;
					spdlog::info("[TFD][Menu] TFDCaptivePhase resolved {:08X}", gCaptivePhase->GetFormID());
				}
			}

			if (!gPreCombatState) {
				gPreCombatState = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDPreCombatState");
				if (gPreCombatState && !gLoggedPreCombatFound) {
					gLoggedPreCombatFound = true;
					spdlog::info("[TFD][Menu] TFDPreCombatState resolved {:08X}", gPreCombatState->GetFormID());
				}
			}
		}

		static float GetGlobalValue(RE::TESGlobal* g)
		{
			return g ? g->value : 0.0f;
		}

		static const char* SafeStr(const char* s)
		{
			return (s && s[0]) ? s : "";
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
			TFD::Settings::SetBleedWindowSeconds(uiBleedSeconds);
			TFD::Settings::SetScanRadius(uiScanRadius);
			TFD::Settings::SetSweepRadius(uiSweepRadius);

			TFD::Settings::SetHotkeyEnabled(uiHotkeyEnabled);
			TFD::Settings::SetHotkeyScanCode(static_cast<std::uint32_t>(std::max(1, uiHotkeyScanCode)));
			TFD::Settings::SetHotkeyCooldownMs(uiHotkeyCooldownMs);
			TFD::Settings::SetHotkeyWave(uiHotkeyWave);
		}

		static RE::Actor* PickCaptorSameCellLoaded(float radius)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return nullptr;
			}

			auto* pCell = player->GetParentCell();
			if (!pCell) {
				return nullptr;
			}

			TFD::ActorScan::Rescan(radius, true);

			RE::Actor* best = nullptr;
			float bestDist = 1.0e30f;

			const auto n = TFD::ActorScan::GetCount();
			for (int i = 0; i < n; ++i) {
				auto e = TFD::ActorScan::GetEntry(i);
				auto* a = TFD::ActorScan::GetActor(i);
				if (!a) continue;
				if (a->IsDead() || a->IsDisabled()) continue;
				if (!a->Is3DLoaded()) continue;
				if (a->GetFormID() == player->GetFormID()) continue;
				if (a->GetParentCell() != pCell) continue;

				if (e.dist < bestDist) {
					bestDist = e.dist;
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

			const float len = std::sqrt((std::max)(1.0f, d2));

			const float ang = player->GetAngleZ();
			const float fx = std::sin(ang);
			const float fy = std::cos(ang);

			const float nx = dx / len;
			const float ny = dy / len;
			const float dot = nx * fx + ny * fy;

			return dot >= 0.20f;
		}

		enum class HotkeyPickMode
		{
			None = 0,
			TrucePreCombat,
			Tame,
			TruceInCombat
		};

		static float ScoreHotkeyCandidate(
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

			const bool front = IsActorCloseAndFront(actor, player, 1400.0f);
			const bool inCombat = actor->IsInCombat();
			const bool negotiable = TFD::TargetClassifier::IsNegotiable(actor);
			const bool creature = TFD::TargetClassifier::IsCreature(actor);
			const bool weaponDrawn = actor->IsWeaponDrawn();

			if (negotiable && !inCombat && front && entry.dist <= 1150.0f) {
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
				return score;
			}

			if (creature && !inCombat && front && entry.dist <= 768.0f) {
				if (outMode) {
					*outMode = HotkeyPickMode::Tame;
				}

				float score = 30000.0f;
				score -= entry.dist;
				if (weaponDrawn) {
					score += 350.0f;
				}
				return score;
			}

			if (negotiable && inCombat && entry.dist <= 1400.0f) {
				if (outMode) {
					*outMode = HotkeyPickMode::TruceInCombat;
				}

				float score = 20000.0f;
				score -= entry.dist;
				if (front) {
					score += 500.0f;
				}
				return score;
			}

			return -1.0e30f;
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
				const float score = ScoreHotkeyCandidate(actor, player, entry, &mode);
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

		static void __stdcall RenderSettings()
		{
			ResolveGlobals();

			ImGuiMCP::Text("TFDEngine");
			ImGuiMCP::Separator();

			if (ImGuiMCP::Checkbox("Enable defeat", &uiEnabled)) {
				ApplyToCore();
			}

			if (ImGuiMCP::SliderFloat("Defeat threshold (%)", &uiThreshold, 2.0f, 95.0f, "%.0f%%")) {
				ApplyToCore();
			}

			if (ImGuiMCP::SliderInt("Bleed window (sec)", &uiBleedSeconds, 10, 30, "%d")) {
				ApplyToCore();
			}

			ImGuiMCP::Separator();
			ImGuiMCP::Text("Hotkey");

			if (ImGuiMCP::Checkbox("Enable hotkey", &uiHotkeyEnabled)) {
				ApplyToCore();
			}
			if (ImGuiMCP::Checkbox("Wave gesture", &uiHotkeyWave)) {
				ApplyToCore();
			}
			if (ImGuiMCP::SliderInt("Hotkey cooldown (ms)", &uiHotkeyCooldownMs, 0, 5000, "%d")) {
				ApplyToCore();
			}

			ImGuiMCP::Text("Bound key: %s", KeyLabel(static_cast<std::uint32_t>(uiHotkeyScanCode)).c_str());

			if (!gCaptureHotkey) {
				if (ImGuiMCP::Button("Rebind hotkey")) {
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

			ImGuiMCP::Separator();
			ImGuiMCP::Text("Debug");

			ImGuiMCP::SliderFloat("Scan radius", &uiScanRadius, 256.0f, 12000.0f, "%.0f");
			ImGuiMCP::SliderFloat("Sweep radius", &uiSweepRadius, 256.0f, 12000.0f, "%.0f");
			ImGuiMCP::Checkbox("NPC only", &uiNpcOnly);

			if (ImGuiMCP::Button("Apply")) {
				ApplyToCore();
			}

			if (ImGuiMCP::Button("Rescan Marker")) {
				TFD::Location::RescanCaptiveMarker();
				TFD::Location::DumpContextToLog();
			}

			if (ImGuiMCP::Button("Teleport -> Marker")) {
				TFD::Location::TeleportToCaptiveMarker();
				TFD::AntiAggro::ScheduleWaves(TFD::Settings::GetSweepRadius(), true, 6, 180);
			}

			if (ImGuiMCP::Button("Rescan Actors")) {
				TFD::ActorScan::Rescan(TFD::Settings::GetScanRadius(), uiNpcOnly);
			}

			if (ImGuiMCP::Button("StopCombat Sweep")) {
				TFD::AntiAggro::SweepOnce(TFD::Settings::GetSweepRadius(), true);
			}

			ImGuiMCP::Separator();
			ImGuiMCP::Text("Marker: 0x%08X", TFD::Location::GetCachedCaptiveMarkerFormID());
			ImGuiMCP::Text("Actors: %d", TFD::ActorScan::GetCount());
			ImGuiMCP::Text("Hotkey: %s", KeyLabel(TFD::Settings::GetHotkeyScanCode()).c_str());
			ImGuiMCP::Text("InputSink: %s", inputSinkAdded.load() ? "READY" : "WAITING");
			ImGuiMCP::Text("CaptivePhase(C++ bool): %.0f", IsCaptivePhase() ? 1.0f : 0.0f);
			ImGuiMCP::Text("CaptivePhaseRaw(C++): %u", GetCaptivePhaseRaw());
			ImGuiMCP::Text("CaptivePhaseName(C++): %s", GetCaptivePhaseName());
			ImGuiMCP::Text("CaptiveFamily(C++): %.0f", IsCaptiveFamily() ? 1.0f : 0.0f);
			ImGuiMCP::Text("CaptiveState(Global): %.0f", GetGlobalValue(gCaptiveState) >= 0.5f ? 1.0f : 0.0f);
			ImGuiMCP::Text("PreCombatState: %.0f", IsPreCombatPhase() ? 1.0f : 0.0f);

			RenderLocationMonitor();
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

			SKSEMenuFramework::SetSection("TFDEngine");
			SKSEMenuFramework::AddSectionItem("Settings", RenderSettings);

			menuRegistered = true;
			spdlog::info("[TFD][SMF] menu registered OK");
			RE::DebugNotification("TFDEngine: SMF menu registered");
		}

		class InputSink : public RE::BSTEventSink<RE::InputEvent*>
		{
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

					if (!TFD::Settings::GetHotkeyEnabled()) {
						continue;
					}

					const auto code = static_cast<std::uint32_t>(btn->GetIDCode());
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

					if (TFD::Settings::GetHotkeyWave()) {
						player->NotifyAnimationGraph("IdleWave");
					}

					// Captive
					if (IsCaptivePhase()) {
						auto* captor = PickCaptorSameCellLoaded(3500.0f);
						if (!captor) {
							RE::DebugNotification("TFD: No Response");
							continue;
						}

						ApplyCellHotkeyCalmBubble(player, captor, 12000.0f);
						SendBridgeEvent("TFDCaptiveClearAll");
						SendBridgeAssignActor("TFDCaptiveAssign", captor);
						TFD::ForceGreet::BeginCaptiveMarker(captor);

						RE::DebugNotification("TFD: Calling Captor");
						continue;
					}

					// Precombat / InCombat / Tame
					HotkeyPickMode pickMode = HotkeyPickMode::None;
					auto* target = PickPreCombatTargetSameCellLoaded(3500.0f, &pickMode);
					if (!target || pickMode == HotkeyPickMode::None) {
						RE::DebugNotification("TFD: No Truce Target");
						continue;
					}

					if (!TFD::PreCombatGreet::BeginForActor(target)) {
						RE::DebugNotification("TFD: Truce Failed");
						continue;
					}

					switch (pickMode) {
					case HotkeyPickMode::TrucePreCombat:
						RE::DebugNotification("TFD: PreCombat Truce");
						break;
					case HotkeyPickMode::Tame:
						RE::DebugNotification("TFD: Tame");
						break;
					case HotkeyPickMode::TruceInCombat:
						RE::DebugNotification("TFD: InCombat Truce");
						break;
					case HotkeyPickMode::None:
					default:
						RE::DebugNotification("TFD: Truce Started");
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
				TFD::PreCombatGreet::OnPreLoadGame();
				TFD::DefeatMonitor::SetLoadTransition(true);
				spdlog::info("[TFD][Menu] PreLoadGame -> prepare only");
			}

			if (m->type == SKSE::MessagingInterface::kPostLoadGame ||
				m->type == SKSE::MessagingInterface::kNewGame) {
				ResolveGlobals();
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

		TFD::ForceGreet::Install();
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
