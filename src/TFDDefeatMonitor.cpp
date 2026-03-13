#include "TFDDefeatMonitor.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <string>
#include <cstring>
#include <limits>

#include <RE/Skyrim.h>
#include <RE/L/LockpickingMenu.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include "TFDSettings.h"
#include "TFDLocation.h"
#include "TFDCaptiveDoorController.h"
#include "TFDAntiAggro.h"
#include "TFDFactionMask.h"
#include "TFDForceGreet.h"
#include "TFDActorScan.h"
#include "TFDAggressionClamp.h"

namespace TFD::DefeatMonitor
{
	namespace
	{
		enum class CaptivePhaseValue : int
		{
			None = 0,
			Captive = 1,
			Escape = 2,
			ReleasedWork = 3,
			Scene = 4
		};

		std::atomic_bool g_installed{ false };
		std::atomic_bool g_running{ false };
		std::atomic_bool g_loadTransition{ false };
		std::atomic_flag g_tickPending = ATOMIC_FLAG_INIT;
		std::thread g_worker{};

		static RE::TESGlobal* g_captiveStateGlobal = nullptr;
		static RE::TESGlobal* g_captivePhaseGlobal = nullptr;
		static RE::TESGlobal* g_preCombatStateGlobal = nullptr;
		static RE::TESGlobal* g_transitionPendingGlobal = nullptr;
		static RE::TESGlobal* g_transitionBusyGlobal = nullptr;
		static RE::TESGlobal* g_transitionReasonGlobal = nullptr;
		static RE::TESGlobal* g_transitionResultGlobal = nullptr;
		static bool g_loggedCaptiveStateGlobal = false;
		static bool g_loggedCaptivePhaseGlobal = false;
		static bool g_loggedPreCombatStateGlobal = false;
		static bool g_loggedTransitionPendingGlobal = false;
		static bool g_loggedTransitionBusyGlobal = false;
		static bool g_loggedTransitionReasonGlobal = false;
		static bool g_loggedTransitionResultGlobal = false;

		static inline std::chrono::steady_clock::time_point Now()
		{
			return std::chrono::steady_clock::now();
		}

		std::atomic_bool g_inBleedState{ false };
		float g_minHp{ 0.0f };

		std::chrono::steady_clock::time_point g_bleedStart{};
		int g_bleedLastSeconds = -1;
		bool g_bleedPaused = false;
		std::chrono::steady_clock::time_point g_bleedPauseStarted{};
		bool g_pendingBleedoutChoice = false;
		std::string g_pendingBleedoutChoiceReason{};
		std::chrono::steady_clock::time_point g_pendingBleedoutChoiceAt{};
		static constexpr int kBleedoutChoiceSettleMs = 1100;

		std::atomic_bool g_grace{ false };
		std::chrono::steady_clock::time_point g_graceUntil{};

		bool g_leftForDeadActive = false;
		std::chrono::steady_clock::time_point g_leftForDeadUntil{};
		std::chrono::steady_clock::time_point g_leftForDeadNextPulse{};
		std::chrono::steady_clock::time_point g_lastRescueContextPoll{};
		RE::FormID g_lastKnownRescueLocationId = 0;
		bool g_pendingRescueNearBed = false;
		bool g_pendingRescueNearBedHideFader = false;
		RE::ObjectRefHandle g_pendingRescueNearBedRef{};
		RE::ObjectRefHandle g_pendingRescueNearDestRef{};
		std::chrono::steady_clock::time_point g_pendingRescueNearBedNotBefore{};
		std::chrono::steady_clock::time_point g_pendingRescueNearBedExpire{};

		RE::ActorHandle g_lastAggressor{};
		bool g_bleedSawDialogue = false;

		bool g_captiveState = false;
		CaptivePhaseValue g_captivePhase = CaptivePhaseValue::None;
		bool g_prevDialogueOpen = false;
		bool g_prevLockpickOpen = false;

		RE::ObjectRefHandle g_lockpickDoorCandidate{};
		bool g_lockpickDoorWasLocked = false;
		TFD::CaptiveDoorController g_captiveDoor{};
		RE::ObjectRefHandle g_captiveMarker{};
		RE::FormID g_captiveCellFormID = 0;
		RE::FormID g_captiveLocationFormID = 0;
		bool g_escapeRadiusActive = false;
		std::chrono::steady_clock::time_point g_escapeRadiusSince{};
		RE::ObjectRefHandle g_boundEscapeDoor{};

		static constexpr double kCaptiveEscapeDoorRadius = 512.0;

		bool g_hasQueuedProgressState = false;
		bool g_queuedCaptiveState = false;
		CaptivePhaseValue g_queuedCaptivePhase = CaptivePhaseValue::None;

		static bool SnapPlayerNearBed(RE::Actor* player, RE::TESObjectREFR* bedRef, RE::TESObjectREFR* destRef);

		static RE::PlayerCharacter* Player()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

		static RE::BGSKeyword* LookupKeyword(const char* editorID)
		{
			if (!editorID || !editorID[0]) {
				return nullptr;
			}
			return RE::TESForm::LookupByEditorID<RE::BGSKeyword>(editorID);
		}

		static bool ActorHasKeywordByEditorID(RE::Actor* actor, const char* editorID)
		{
			if (!actor) {
				return false;
			}
			auto* kw = LookupKeyword(editorID);
			return kw && actor->HasKeyword(kw);
		}

		static bool IsCaptiveSupportedAggressor(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}

			if (ActorHasKeywordByEditorID(actor, "ActorTypeNPC")) {
				return true;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeCreature")) {
				return false;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeAnimal")) {
				return false;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeDragon")) {
				return false;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeDaedra")) {
				return false;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeGhost")) {
				return false;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeUndead")) {
				return false;
			}

			return false;
		}

		static void FinishLeftForDeadRecovery()
		{
			TFD::AggressionClamp::Clear();
			TFD::FactionMask::Clear();
			g_leftForDeadActive = false;
			g_leftForDeadUntil = {};
			g_leftForDeadNextPulse = {};
			spdlog::info("[TFD][Defeat] Transition recovery finished");
		}

		static bool IsLeftForDeadCooldownActive()
		{
			if (!g_leftForDeadActive) {
				return false;
			}
			const auto now = Now();
			if (now >= g_leftForDeadUntil) {
				FinishLeftForDeadRecovery();
				return false;
			}
			return true;
		}

		static void ClearLeftForDeadCooldown()
		{
			if (g_leftForDeadActive) {
				FinishLeftForDeadRecovery();
				return;
			}
			g_leftForDeadActive = false;
			g_leftForDeadUntil = {};
			g_leftForDeadNextPulse = {};
		}

		static void BeginLeftForDeadCooldown(int seconds)
		{
			if (seconds <= 0) {
				ClearLeftForDeadCooldown();
				return;
			}
			const auto now = Now();
			g_leftForDeadActive = true;
			g_leftForDeadUntil = now + std::chrono::seconds(seconds);
			g_leftForDeadNextPulse = now;
		}

		static void ShowBlackoutFader()
		{
			auto* queue = RE::UIMessageQueue::GetSingleton();
			auto* strings = RE::InterfaceStrings::GetSingleton();
			if (!queue || !strings) {
				return;
			}
			queue->AddMessage(strings->faderMenu, RE::UI_MESSAGE_TYPE::kShow, nullptr);
			queue->ProcessCommands();
		}

		static void HideBlackoutFader()
		{
			auto* queue = RE::UIMessageQueue::GetSingleton();
			auto* strings = RE::InterfaceStrings::GetSingleton();
			if (!queue || !strings) {
				return;
			}
			queue->AddMessage(strings->faderMenu, RE::UI_MESSAGE_TYPE::kHide, nullptr);
			queue->ProcessCommands();
		}

		static void SendBridgeEvent(const char* eventName, RE::TESForm* sender = nullptr, float numArg = 0.0f, const char* strArg = "")
		{
			if (!eventName) {
				return;
			}

			auto* task = SKSE::GetTaskInterface();
			if (!task) {
				return;
			}

			const std::string name{ eventName };
			const std::string text{ strArg ? strArg : "" };
			const RE::FormID senderId = sender ? sender->GetFormID() : 0;

			task->AddTask([name, text, numArg, senderId]() {
				auto* src = SKSE::GetModCallbackEventSource();
				if (!src) {
					return;
				}

				RE::TESForm* outSender = nullptr;
				if (senderId != 0) {
					outSender = RE::TESForm::LookupByID(senderId);
				}

				SKSE::ModCallbackEvent ev{ name.c_str(), text.c_str(), numArg, outSender };
				src->SendEvent(&ev);
				});
		}

		static void AdvanceGameHoursSoft(float hours)
		{
			if (hours <= 0.0f) {
				return;
			}
			auto* calendar = RE::Calendar::GetSingleton();
			if (!calendar) {
				return;
			}
			const float dayDelta = hours / 24.0f;
			calendar->rawDaysPassed += dayDelta;
			if (calendar->gameDaysPassed) {
				calendar->gameDaysPassed->value = calendar->rawDaysPassed;
			}
			if (calendar->gameHour) {
				float hour = std::fmod(calendar->gameHour->value + hours, 24.0f);
				if (hour < 0.0f) {
					hour += 24.0f;
				}
				calendar->gameHour->value = hour;
			}
		}

		static void BlackoutAndAdvanceHours(float hours, int holdMs, const char* reason)
		{
			ShowBlackoutFader();
			std::this_thread::sleep_for(std::chrono::milliseconds(350));
			AdvanceGameHoursSoft(hours);
			if (holdMs > 0) {
				std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
			}
			HideBlackoutFader();
			spdlog::info("[TFD][Defeat] blackout advance reason={} hours={:.2f}", reason ? reason : "unknown", hours);
		}

		static CaptivePhaseValue PhaseFromRaw(std::uint32_t raw)
		{
			switch (raw) {
			case 1: return CaptivePhaseValue::Captive;
			case 2: return CaptivePhaseValue::Escape;
			case 3: return CaptivePhaseValue::ReleasedWork;
			case 4: return CaptivePhaseValue::Scene;
			default: return CaptivePhaseValue::None;
			}
		}


		static RE::Actor* ResolveAggressor();
		static RE::Actor* FindBestAggressor(float radius);
		static void SetGraceSeconds(int seconds);
		static void RecoverPlayerForTransition();
		static void UpdatePreCombatState();
		static bool IsActorCloseAndFront(RE::Actor* actor, RE::Actor* player, float maxDist);
		static bool CanUseAggressorForBleedoutGreet(RE::Actor* player, RE::Actor* aggressor, float& outDistance);
		static RE::TESObjectREFR* LookupRefByFormID(std::uint32_t formID);
		static void ClearPendingRescueNearBed(const char* reason, bool hideNow);
		static void QueuePendingRescueNearBed(RE::TESObjectREFR* bedRef, RE::TESObjectREFR* destRef, int minDelayMs);
		static void ProcessPendingRescueNearBed();
		static bool BeginRescueTransition(const char* reason);
		static void BeginRecoverTransition(const char* reason);

		static void ResolveGlobals()
		{
			if (!g_captiveStateGlobal) {
				g_captiveStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDCaptiveState");
				if (g_captiveStateGlobal && !g_loggedCaptiveStateGlobal) {
					g_loggedCaptiveStateGlobal = true;
					spdlog::info("[TFD][Defeat] TFDCaptiveState resolved {:08X}", g_captiveStateGlobal->GetFormID());
				}
			}
			if (!g_captivePhaseGlobal) {
				g_captivePhaseGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDCaptivePhase");
				if (g_captivePhaseGlobal && !g_loggedCaptivePhaseGlobal) {
					g_loggedCaptivePhaseGlobal = true;
					spdlog::info("[TFD][Defeat] TFDCaptivePhase resolved {:08X}", g_captivePhaseGlobal->GetFormID());
				}
			}
			if (!g_preCombatStateGlobal) {
				g_preCombatStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDPreCombatState");
				if (g_preCombatStateGlobal && !g_loggedPreCombatStateGlobal) {
					g_loggedPreCombatStateGlobal = true;
					spdlog::info("[TFD][Defeat] TFDPreCombatState resolved {:08X}", g_preCombatStateGlobal->GetFormID());
				}
			}
			if (!g_transitionPendingGlobal) {
				g_transitionPendingGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDTransitionPending");
				if (g_transitionPendingGlobal && !g_loggedTransitionPendingGlobal) {
					g_loggedTransitionPendingGlobal = true;
					spdlog::info("[TFD][Transition] TFDTransitionPending resolved {:08X}", g_transitionPendingGlobal->GetFormID());
				}
			}
			if (!g_transitionBusyGlobal) {
				g_transitionBusyGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDTransitionBusy");
				if (g_transitionBusyGlobal && !g_loggedTransitionBusyGlobal) {
					g_loggedTransitionBusyGlobal = true;
					spdlog::info("[TFD][Transition] TFDTransitionBusy resolved {:08X}", g_transitionBusyGlobal->GetFormID());
				}
			}
			if (!g_transitionReasonGlobal) {
				g_transitionReasonGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDTransitionReason");
				if (g_transitionReasonGlobal && !g_loggedTransitionReasonGlobal) {
					g_loggedTransitionReasonGlobal = true;
					spdlog::info("[TFD][Transition] TFDTransitionReason resolved {:08X}", g_transitionReasonGlobal->GetFormID());
				}
			}
			if (!g_transitionResultGlobal) {
				g_transitionResultGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDTransitionResult");
				if (g_transitionResultGlobal && !g_loggedTransitionResultGlobal) {
					g_loggedTransitionResultGlobal = true;
					spdlog::info("[TFD][Transition] TFDTransitionResult resolved {:08X}", g_transitionResultGlobal->GetFormID());
				}
			}
		}

		static bool ResolveCaptiveMarkerForOutcome()
		{
			auto* aggressor = ResolveAggressor();
			bool resolved = false;
			if (aggressor) {
				resolved = TFD::Location::RescanCaptiveMarkerWithAggressor(aggressor, true);
				if (!resolved) {
					resolved = TFD::Location::RescanCaptiveMarkerWithAggressor(aggressor, false);
				}
			}
			if (!resolved) {
				resolved = TFD::Location::RescanCaptiveMarker();
			}
			return resolved && TFD::Location::GetCachedCaptiveMarker();
		}

		static bool TeleportPlayerToCachedMarkerNow()
		{
			auto* player = Player();
			auto* marker = TFD::Location::GetCachedCaptiveMarker();
			if (!player || !marker) {
				return false;
			}
			player->MoveTo(marker);
			spdlog::info("[TFD][Location] Direct MoveTo cached marker {:08X}", marker->GetFormID());
			return true;
		}

		static int ResolveNonCaptiveChoiceReason(const char* reason)
		{
			if (!reason || !reason[0]) {
				return 4;
			}
			if (std::strcmp(reason, "bleed_timeout") == 0) {
				return 2;
			}
			if (std::strcmp(reason, "dialogue_closed_no_marker") == 0) {
				return 3;
			}
			if (std::strcmp(reason, "no_valid_npc") == 0 ||
				std::strcmp(reason, "unsupported_aggressor_nonhumanoid") == 0 ||
				std::strcmp(reason, "unsupported_aggressor_allowlist") == 0) {
				return 1;
			}
			return 4;
		}

		static void QueueNonCaptiveChoiceRequest(const char* reason)
		{
			ResolveGlobals();
			if (!g_transitionPendingGlobal) {
				spdlog::warn("[TFD][Transition] non-captive choice skipped (globals missing) reason={}", reason ? reason : "unknown");
				return;
			}
			if (g_transitionBusyGlobal && g_transitionBusyGlobal->value >= 0.5f) {
				spdlog::info("[TFD][Transition] non-captive choice skipped (busy) reason={}", reason ? reason : "unknown");
				return;
			}
			if (g_transitionPendingGlobal->value >= 0.5f) {
				spdlog::info("[TFD][Transition] non-captive choice skipped (already pending={}) reason={}", g_transitionPendingGlobal->value, reason ? reason : "unknown");
				return;
			}
			const int mappedReason = ResolveNonCaptiveChoiceReason(reason);
			if (g_transitionReasonGlobal) {
				g_transitionReasonGlobal->value = static_cast<float>(mappedReason);
			}
			if (g_transitionResultGlobal) {
				g_transitionResultGlobal->value = 0.0f;
			}
			g_transitionPendingGlobal->value = 1.0f;
			spdlog::info("[TFD][Transition] queued non-captive choice reason={} source={}", mappedReason, reason ? reason : "unknown");
		}

		static int ConsumeTransitionResult()
		{
			ResolveGlobals();
			if (!g_transitionResultGlobal) {
				return 0;
			}
			const int result = static_cast<int>(std::lround(g_transitionResultGlobal->value));
			if (result != 0) {
				g_transitionResultGlobal->value = 0.0f;
			}
			return result;
		}

		static bool IsTransitionAwaiting()
		{
			ResolveGlobals();
			const bool pending = g_transitionPendingGlobal && g_transitionPendingGlobal->value >= 0.5f;
			const bool busy = g_transitionBusyGlobal && g_transitionBusyGlobal->value >= 0.5f;
			return pending || busy;
		}

		static void MaintainTransitionCalmWindow()
		{
			auto* player = Player();
			if (!player) {
				return;
			}
			if (player->IsInCombat()) {
				player->StopCombat();
			}
			player->DrawWeaponMagicHands(false);
			const float radius = (std::max)(3200.0f, TFD::Settings::GetSweepRadius() + 1200.0f);
			TFD::AntiAggro::SweepOnce(radius, false);
			TFD::ActorScan::Rescan(radius, false);
			const auto count = TFD::ActorScan::GetCount();
			for (int i = 0; i < count; ++i) {
				auto entry = TFD::ActorScan::GetEntry(i);
				auto actorSP = entry.actor.get();
				auto* actor = actorSP.get();
				if (!actor || actor->IsDead() || actor->IsDisabled()) {
					continue;
				}
				TFD::AggressionClamp::Apply(actor);
				actor->StopCombat();
				actor->EvaluatePackage(true, false);
			}
		}

		static RE::TESObjectREFR* LookupRefByFormID(std::uint32_t formID)
		{
			if (formID == 0) {
				return nullptr;
			}
			return RE::TESForm::LookupByID<RE::TESObjectREFR>(formID);
		}

		static void UpdateRescueContextCache(RE::PlayerCharacter* player)
		{
			if (!player) {
				return;
			}

			const auto now = Now();
			if (g_lastRescueContextPoll.time_since_epoch().count() != 0 && now < g_lastRescueContextPoll + std::chrono::seconds(2)) {
				return;
			}
			g_lastRescueContextPoll = now;

			auto* startLoc = TFD::Location::GetLocationFromRef(player);
			auto* safeLoc = TFD::Location::ResolveRescueTargetLocation(startLoc);
			if (safeLoc) {
				g_lastKnownRescueLocationId = safeLoc->GetFormID();
			}
		}

		static bool SharesParentCell(RE::TESObjectREFR* a, RE::TESObjectREFR* b)
		{
			if (!a || !b) {
				return false;
			}
			auto* aCell = a->GetParentCell();
			auto* bCell = b->GetParentCell();
			if (!aCell || !bCell) {
				return false;
			}
			return aCell == bCell || aCell->GetFormID() == bCell->GetFormID();
		}

		static RE::TESObjectREFR* ResolveApprovedBedRefForSafeLocation(RE::BGSLocation* safeLoc)
		{
			if (!safeLoc) {
				return nullptr;
			}

			TFD::Location::ApprovedBed bed{};
			if (!TFD::Location::GetBestApprovedBedForLocation(safeLoc, bed)) {
				return nullptr;
			}

			return LookupRefByFormID(bed.bedRefId);
		}

		static RE::TESObjectREFR* ResolveBestRescueMarker(RE::BGSLocation* safeLoc, RE::TESObjectREFR* preferredBed)
		{
			if (!safeLoc) {
				return nullptr;
			}

			const auto preferredCellId = preferredBed && preferredBed->GetParentCell() ? preferredBed->GetParentCell()->GetFormID() : 0;

			TFD::Location::SafeCheckpoint cp{};
			if (TFD::Location::GetLastSafeCheckpointForLocation(safeLoc, cp)) {
				if (preferredCellId != 0) {
					if (auto* ref = LookupRefByFormID(cp.insideEntranceRefId); ref && ref->GetParentCell() && ref->GetParentCell()->GetFormID() == preferredCellId) {
						return ref;
					}
					if (auto* ref = LookupRefByFormID(cp.centerMarkerRefId); ref && ref->GetParentCell() && ref->GetParentCell()->GetFormID() == preferredCellId) {
						return ref;
					}
					if (auto* ref = LookupRefByFormID(cp.entryDoorRefId); ref && ref->GetParentCell() && ref->GetParentCell()->GetFormID() == preferredCellId) {
						return ref;
					}
				}

				if (auto* ref = LookupRefByFormID(cp.insideEntranceRefId)) {
					return ref;
				}
				if (auto* ref = LookupRefByFormID(cp.centerMarkerRefId)) {
					return ref;
				}
				if (auto* ref = LookupRefByFormID(cp.entryDoorRefId)) {
					return ref;
				}
			}

			if (auto* ref = TFD::Location::ResolvePreferredRescueDestination(safeLoc, true)) {
				return ref;
			}

			return nullptr;
		}


		static void ClearPendingRescueNearBed(const char* reason, bool hideNow)
		{
			const bool shouldHide = hideNow || g_pendingRescueNearBedHideFader;
			const bool hadPending = g_pendingRescueNearBed;

			g_pendingRescueNearBed = false;
			g_pendingRescueNearBedHideFader = false;
			g_pendingRescueNearBedRef = RE::ObjectRefHandle{};
			g_pendingRescueNearDestRef = RE::ObjectRefHandle{};
			g_pendingRescueNearBedNotBefore = {};
			g_pendingRescueNearBedExpire = {};

			if (reason && reason[0]) {
				spdlog::info("[TFD][Transition] rescue direct-bed pending cleared reason={}", reason);
			}

			if (shouldHide && (hadPending || hideNow)) {
				HideBlackoutFader();
			}
		}

		static void QueuePendingRescueNearBed(RE::TESObjectREFR* bedRef, RE::TESObjectREFR* destRef, int minDelayMs)
		{
			if (!bedRef || !destRef) {
				return;
			}

			g_pendingRescueNearBed = true;
			g_pendingRescueNearBedHideFader = true;
			g_pendingRescueNearBedRef = bedRef->GetHandle();
			g_pendingRescueNearDestRef = destRef->GetHandle();
			g_pendingRescueNearBedNotBefore = Now() + std::chrono::milliseconds((std::max)(minDelayMs, 0));
			g_pendingRescueNearBedExpire = Now() + std::chrono::seconds(8);

			spdlog::info(
				"[TFD][Transition] rescue direct-bed deferred queued bed={:08X} dest={:08X} minDelayMs={}",
				bedRef->GetFormID(),
				destRef->GetFormID(),
				(std::max)(minDelayMs, 0));
		}

		static void ProcessPendingRescueNearBed()
		{
			if (!g_pendingRescueNearBed) {
				return;
			}

			auto* ui = RE::UI::GetSingleton();
			if (ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
				return;
			}

			const auto now = Now();
			if (now < g_pendingRescueNearBedNotBefore) {
				return;
			}

			auto* player = Player();
			if (!player) {
				if (now >= g_pendingRescueNearBedExpire) {
					spdlog::info("[TFD][Transition] rescue direct-bed deferred timeout cause=no_player");
					ClearPendingRescueNearBed("timeout_no_player", true);
				}
				return;
			}

			auto bedSP = g_pendingRescueNearBedRef.get();
			auto destSP = g_pendingRescueNearDestRef.get();
			auto* bedRef = bedSP.get();
			auto* destRef = destSP.get();

			if (!bedRef || !destRef) {
				spdlog::info(
					"[TFD][Transition] rescue direct-bed deferred cancelled cause=ref_invalid bed={:08X} dest={:08X}",
					bedRef ? bedRef->GetFormID() : 0,
					destRef ? destRef->GetFormID() : 0);
				ClearPendingRescueNearBed("ref_invalid", true);
				return;
			}

			if (!SharesParentCell(destRef, bedRef)) {
				spdlog::info(
					"[TFD][Transition] rescue direct-bed deferred cancelled cause=cell_mismatch bed={:08X} dest={:08X}",
					bedRef->GetFormID(),
					destRef->GetFormID());
				ClearPendingRescueNearBed("cell_mismatch", true);
				return;
			}

			auto* playerCell = player->GetParentCell();
			auto* bedCell = bedRef->GetParentCell();
			auto* destCell = destRef->GetParentCell();

			if (!playerCell || !bedCell || !destCell) {
				if (now >= g_pendingRescueNearBedExpire) {
					spdlog::info(
						"[TFD][Transition] rescue direct-bed deferred timeout cause=null_cell playerCell={:08X} bedCell={:08X} destCell={:08X}",
						playerCell ? playerCell->GetFormID() : 0,
						bedCell ? bedCell->GetFormID() : 0,
						destCell ? destCell->GetFormID() : 0);
					ClearPendingRescueNearBed("timeout_null_cell", true);
				}
				return;
			}

			const auto playerCellId = playerCell->GetFormID();
			const auto bedCellId = bedCell->GetFormID();
			const auto destCellId = destCell->GetFormID();
			if (playerCellId != bedCellId || playerCellId != destCellId) {
				if (now >= g_pendingRescueNearBedExpire) {
					spdlog::info(
						"[TFD][Transition] rescue direct-bed deferred timeout cause=player_not_in_dest_cell playerCell={:08X} bedCell={:08X} destCell={:08X}",
						playerCellId,
						bedCellId,
						destCellId);
					ClearPendingRescueNearBed("timeout_player_not_in_dest_cell", true);
				}
				return;
			}

			if (SnapPlayerNearBed(player, bedRef, destRef)) {
				spdlog::info(
					"[TFD][Transition] rescue direct-bed deferred applied bed={:08X} dest={:08X} playerCell={:08X}",
					bedRef->GetFormID(),
					destRef->GetFormID(),
					playerCellId);
				ClearPendingRescueNearBed("applied", true);
			}
			else {
				spdlog::info(
					"[TFD][Transition] rescue direct-bed deferred failed bed={:08X} dest={:08X}",
					bedRef->GetFormID(),
					destRef->GetFormID());
				ClearPendingRescueNearBed("failed", true);
			}
		}

		static bool SnapPlayerNearBed(RE::Actor* player, RE::TESObjectREFR* bedRef, RE::TESObjectREFR* destRef)
		{
			if (!player || !bedRef) {
				return false;
			}

			if (!destRef || !SharesParentCell(destRef, bedRef)) {
				return false;
			}

			const auto bedPos = bedRef->GetPosition();
			player->MoveTo(bedRef);
			player->StopMoving(0.0f);

			spdlog::info(
				"[TFD][Transition] rescue direct-bed move success bed={:08X} x={:.1f} y={:.1f} z={:.1f}",
				bedRef->GetFormID(),
				bedPos.x,
				bedPos.y,
				bedPos.z);

			return true;
		}

		static bool BeginRescueTransition(const char* reason)
		{
			auto* player = Player();
			if (!player) {
				return false;
			}

			auto* safeLoc = TFD::Location::ResolveRescueTargetLocationFromRef(player);
			if (!safeLoc && g_lastKnownRescueLocationId != 0) {
				safeLoc = RE::TESForm::LookupByID<RE::BGSLocation>(g_lastKnownRescueLocationId);
				if (safeLoc) {
					spdlog::info("[TFD][Transition] rescue using cached safeLoc={:08X}", safeLoc->GetFormID());
				}
			}
			if (!safeLoc) {
				spdlog::info("[TFD][Transition] rescue unavailable reason={} cause=no_safe_location", reason ? reason : "unknown");
				return false;
			}

			auto* bedRef = ResolveApprovedBedRefForSafeLocation(safeLoc);
			auto* dest = ResolveBestRescueMarker(safeLoc, bedRef);
			if (!dest) {
				spdlog::info("[TFD][Transition] rescue unavailable reason={} safeLoc={:08X} cause=no_destination",
					reason ? reason : "unknown", safeLoc->GetFormID());
				return false;
			}

			const bool canSnapNearBed = bedRef && SharesParentCell(dest, bedRef);
			const char* bedSnapState = "no";

			ClearPendingRescueNearBed(nullptr, false);
			ShowBlackoutFader();
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			player->MoveTo(dest);
			std::this_thread::sleep_for(std::chrono::milliseconds(150));
			RecoverPlayerForTransition();
			if (canSnapNearBed) {
				QueuePendingRescueNearBed(bedRef, dest, 1200);
				bedSnapState = "direct_bed_deferred";
			}
			else if (bedRef) {
				bedSnapState = "cell_mismatch";
				spdlog::info(
					"[TFD][Transition] rescue direct-bed skipped bed={:08X} dest={:08X} cause=cell_mismatch",
					bedRef->GetFormID(),
					dest->GetFormID());
			}
			MaintainTransitionCalmWindow();
			BeginLeftForDeadCooldown(5);
			SetGraceSeconds(4);
			UpdatePreCombatState();
			std::this_thread::sleep_for(std::chrono::milliseconds(400));
			if (!g_pendingRescueNearBed) {
				HideBlackoutFader();
			}
			spdlog::info(
				"[TFD][Transition] rescue complete reason={} safeLoc={:08X} dest={:08X} bed={:08X} bedSnap={}",
				reason ? reason : "unknown",
				safeLoc->GetFormID(),
				dest->GetFormID(),
				bedRef ? bedRef->GetFormID() : 0,
				bedSnapState);
			return true;
		}

		static void BeginRecoverTransition(const char* reason)
		{
			RecoverPlayerForTransition();
			MaintainTransitionCalmWindow();
			BeginLeftForDeadCooldown(3);
			SetGraceSeconds(2);
			UpdatePreCombatState();
			spdlog::info("[TFD][Transition] recover complete reason={} visual=no_blackout", reason ? reason : "unknown");
		}

		static void PollTransitionResult()
		{
			const int result = ConsumeTransitionResult();
			if (result == 0) {
				return;
			}
			if (result == 1) {
				spdlog::info("[TFD][Transition] result=completed");
				return;
			}
			if (result == 2) {
				spdlog::info("[TFD][Transition] result=cancelled");
				return;
			}
			if (result == 3) {
				spdlog::info("[TFD][Transition] result=recover_chosen");
				BeginRecoverTransition("recover_chosen");
				return;
			}
			if (result == 4) {
				spdlog::info("[TFD][Transition] result=rescue_chosen");
				if (!BeginRescueTransition("rescue_chosen")) {
					BeginRecoverTransition("rescue_fallback_recover");
				}
				return;
			}
			spdlog::info("[TFD][Transition] result={} (unknown)", result);
		}

		static void SyncCaptiveGlobals(bool stateActive, CaptivePhaseValue phase)
		{
			ResolveGlobals();
			if (g_captiveStateGlobal) {
				g_captiveStateGlobal->value = stateActive ? 1.0f : 0.0f;
			}
			if (g_captivePhaseGlobal) {
				g_captivePhaseGlobal->value = static_cast<float>(static_cast<int>(phase));
			}
		}

		static void SyncPreCombatGlobal(bool active)
		{
			ResolveGlobals();
			if (g_preCombatStateGlobal) {
				g_preCombatStateGlobal->value = active ? 1.0f : 0.0f;
			}
		}

		static void SetCaptiveRuntimeOnly(bool stateActive, CaptivePhaseValue phase)
		{
			g_captiveState = stateActive;
			g_captivePhase = phase;
		}

		static void SetCaptiveRuntime(bool stateActive, CaptivePhaseValue phase)
		{
			SetCaptiveRuntimeOnly(stateActive, phase);
			SyncCaptiveGlobals(stateActive, phase);
		}

		static void UpdatePreCombatState()
		{
			auto* player = Player();
			bool preCombat = false;
			if (player) {
				preCombat = true;
				if (g_loadTransition.load(std::memory_order_acquire)) preCombat = false;
				if (g_inBleedState.load(std::memory_order_acquire)) preCombat = false;
				if (g_captiveState) preCombat = false;
				if (player->IsInCombat()) preCombat = false;
				if (g_leftForDeadActive) preCombat = false;
				auto* st = player->AsActorState();
				if (st && st->IsBleedingOut()) preCombat = false;
			}
			SyncPreCombatGlobal(preCombat);
		}

		static bool IsDialogueOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
		}

		static bool IsLockpickingOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && ui->IsMenuOpen(RE::LockpickingMenu::MENU_NAME);
		}

		static void ResetLockpickWatch()
		{
			g_lockpickDoorCandidate.reset();
			g_lockpickDoorWasLocked = false;
			g_prevLockpickOpen = false;
		}

		static bool IsDoorRef(RE::TESObjectREFR* ref)
		{
			if (!ref) return false;
			auto* base = ref->GetBaseObject();
			if (!base) return false;
			return base->GetFormType() == RE::FormType::Door;
		}

		static bool IsRefLocked(RE::TESObjectREFR* ref)
		{
			if (!ref) return false;
			auto* lock = ref->GetLock();
			return lock && lock->IsLocked();
		}

		static RE::TESObjectREFR* ResolveBoundEscapeDoor()
		{
			if (!g_boundEscapeDoor) return nullptr;
			auto ptr = g_boundEscapeDoor.get();
			return ptr.get();
		}

		static void BindCaptiveDoor(RE::TESObjectREFR* door)
		{
			if (!door) return;
			g_captiveDoor.Bind(door);
			g_boundEscapeDoor = door->GetHandle();
		}

		static RE::TESObjectCELL* GetParentCell(RE::TESObjectREFR* ref)
		{
			return ref ? ref->GetParentCell() : nullptr;
		}

		static RE::BGSLocation* GetLocationFromRef(RE::TESObjectREFR* ref)
		{
			return TFD::Location::GetLocationFromRef(ref);
		}

		static RE::TESObjectREFR* FindNearestDoorNearCaptiveMarker()
		{
			RE::TESObjectREFR* marker = nullptr;
			if (g_captiveMarker) {
				auto ptr = g_captiveMarker.get();
				marker = ptr.get();
			}
			if (!marker) marker = TFD::Location::GetCachedCaptiveMarker();
			if (!marker) return nullptr;
			auto* cell = marker->GetParentCell();
			if (!cell) return nullptr;
			const auto mp = marker->GetPosition();
			RE::TESObjectREFR* best = nullptr;
			double bestDistSq = kCaptiveEscapeDoorRadius * kCaptiveEscapeDoorRadius;
			cell->ForEachReferenceInRange(mp, static_cast<float>(kCaptiveEscapeDoorRadius), [&](RE::TESObjectREFR* candidate) -> RE::BSContainer::ForEachResult {
				if (!candidate || candidate == marker) return RE::BSContainer::ForEachResult::kContinue;
				if (!IsDoorRef(candidate)) return RE::BSContainer::ForEachResult::kContinue;
				const auto rp = candidate->GetPosition();
				const double dx = static_cast<double>(rp.x - mp.x);
				const double dy = static_cast<double>(rp.y - mp.y);
				const double dz = static_cast<double>(rp.z - mp.z);
				const double distSq = dx * dx + dy * dy + dz * dz;
				if (distSq <= bestDistSq) {
					bestDistSq = distSq;
					best = candidate;
				}
				return RE::BSContainer::ForEachResult::kContinue;
				});
			return best;
		}

		static void ClearEscapeContext()
		{
			g_captiveMarker.reset();
			g_captiveCellFormID = 0;
			g_captiveLocationFormID = 0;
			g_escapeRadiusActive = false;
			g_escapeRadiusSince = {};
			g_boundEscapeDoor.reset();
			g_captiveDoor.Reset();
		}

		static void ArmEscapeContextFromCurrentState()
		{
			auto* player = Player();
			if (!player) return;
			auto* marker = TFD::Location::GetCachedCaptiveMarker();
			if (!marker) {
				TFD::Location::RescanCaptiveMarker();
				marker = TFD::Location::GetCachedCaptiveMarker();
			}
			g_captiveMarker = marker ? marker->GetHandle() : RE::ObjectRefHandle{};
			auto* cell = player->GetParentCell();
			g_captiveCellFormID = cell ? cell->GetFormID() : 0;
			auto* loc = cell ? cell->GetLocation() : nullptr;
			g_captiveLocationFormID = loc ? loc->GetFormID() : 0;
			g_escapeRadiusActive = false;
			g_escapeRadiusSince = {};
			if (!g_captiveDoor.HasDoor()) {
				if (auto* door = FindNearestDoorNearCaptiveMarker()) {
					BindCaptiveDoor(door);
					spdlog::info("[TFD][Captive] Bound nearest captive door {:08X} on captive enter", door->GetFormID());
				}
			}
			spdlog::info("[TFD][Captive] Escape context armed marker={:08X} cell={:08X} loc={:08X}", marker ? marker->GetFormID() : 0, g_captiveCellFormID, g_captiveLocationFormID);
		}

		static bool IsDoorNearCaptiveMarker(RE::TESObjectREFR* door)
		{
			if (!door || !IsDoorRef(door)) return false;
			RE::TESObjectREFR* marker = nullptr;
			if (g_captiveMarker) {
				auto ptr = g_captiveMarker.get();
				marker = ptr.get();
			}
			if (!marker) marker = TFD::Location::GetCachedCaptiveMarker();
			if (!marker) return true;
			const auto dp = door->GetPosition();
			const auto mp = marker->GetPosition();
			const double dx = static_cast<double>(dp.x - mp.x);
			const double dy = static_cast<double>(dp.y - mp.y);
			const double dz = static_cast<double>(dp.z - mp.z);
			const double distSq = dx * dx + dy * dy + dz * dz;
			return distSq <= (kCaptiveEscapeDoorRadius * kCaptiveEscapeDoorRadius);
		}

		static RE::Actor* ResolveAggressor();
		static RE::Actor* FindBestAggressor(float radius);

		static RE::TESObjectREFR* ResolveLockpickDoorCandidate(RE::TESObjectREFR* target)
		{
			if (target && IsDoorRef(target) && IsDoorNearCaptiveMarker(target)) return target;
			auto* boundDoor = ResolveBoundEscapeDoor();
			if (boundDoor && IsDoorRef(boundDoor) && IsDoorNearCaptiveMarker(boundDoor)) return boundDoor;
			return nullptr;
		}

		static void EnterEscapeCommit(const char* reason, RE::TESObjectREFR* door)
		{
			if (!g_captiveState || g_captivePhase != CaptivePhaseValue::Captive) return;
			TFD::ForceGreet::Cancel();
			TFD::FactionMask::Clear();
			TFD::AggressionClamp::Clear();
			g_grace.store(false, std::memory_order_release);
			SetCaptiveRuntime(true, CaptivePhaseValue::Escape);
			UpdatePreCombatState();
			ResetLockpickWatch();
			g_prevDialogueOpen = false;
			if (door) BindCaptiveDoor(door); else door = ResolveBoundEscapeDoor();
			auto* player = Player();
			if (player) player->EvaluatePackage(true, false);
			RE::Actor* aggressor = ResolveAggressor();
			if (!aggressor) {
				const float radius = (std::max)(1800.0f, TFD::Settings::GetSweepRadius());
				aggressor = FindBestAggressor(radius);
				if (aggressor) g_lastAggressor = aggressor->GetHandle();
			}
			if (aggressor) {
				aggressor->EvaluatePackage(true, false);
				spdlog::info("[TFD][Captive] Escape aggro nudge actor={:08X}", aggressor->GetFormID());
			}
			else {
				spdlog::info("[TFD][Captive] Escape aggro nudge skipped (no aggressor)");
			}
			spdlog::info("[TFD][Captive] EscapeCommit reason={} door={:08X}", reason ? reason : "unknown", door ? door->GetFormID() : 0);
		}

		static void TryCommitEscapeByRadius()
		{
			if (!g_captiveState || g_captivePhase != CaptivePhaseValue::Captive) {
				g_escapeRadiusActive = false;
				return;
			}
			auto* player = Player();
			if (!player) return;
			RE::TESObjectREFR* marker = nullptr;
			if (g_captiveMarker) {
				auto ptr = g_captiveMarker.get();
				marker = ptr.get();
			}
			if (!marker) marker = TFD::Location::GetCachedCaptiveMarker();
			if (!marker) return;
			const auto pp = player->GetPosition();
			const auto mp = marker->GetPosition();
			const double dx = static_cast<double>(pp.x - mp.x);
			const double dy = static_cast<double>(pp.y - mp.y);
			const double dz = static_cast<double>(pp.z - mp.z);
			const double distSq = dx * dx + dy * dy + dz * dz;
			const bool outside = distSq > (512.0 * 512.0);
			if (!outside) {
				g_escapeRadiusActive = false;
				return;
			}
			if (!g_escapeRadiusActive) {
				g_escapeRadiusActive = true;
				g_escapeRadiusSince = Now();
				return;
			}
			const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(Now() - g_escapeRadiusSince).count();
			if (held >= 2000) {
				EnterEscapeCommit("marker_radius", nullptr);
				g_escapeRadiusActive = false;
			}
		}

		static void TryResolveEscapeByLocation()
		{
			if (!g_captiveState || g_captivePhase != CaptivePhaseValue::Escape) return;
			auto* player = Player();
			if (!player) return;
			auto* loc = GetLocationFromRef(player);
			const auto curLoc = loc ? loc->GetFormID() : 0;
			if (g_captiveLocationFormID != 0 && curLoc != 0 && curLoc != g_captiveLocationFormID) {
				SetCaptiveRuntime(false, CaptivePhaseValue::None);
				ClearEscapeContext();
				UpdatePreCombatState();
				spdlog::info("[TFD][Captive] EscapeResolved by location old={:08X} new={:08X}", g_captiveLocationFormID, curLoc);
			}
		}

		static void NormalizeInvalidCaptivePair()
		{
			if (g_captiveState && g_captivePhase == CaptivePhaseValue::None) {
				SetCaptiveRuntime(true, CaptivePhaseValue::Escape);
				spdlog::info("[TFD][Captive] Normalized invalid captive pair -> Escape");
			}
		}

		static void ClampHealth(RE::Actor* actor, float minHp)
		{
			if (!actor) return;
			const float hp = actor->GetActorValue(RE::ActorValue::kHealth);
			if (hp < minHp) {
				actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, (minHp - hp));
			}
		}

		static void SetGraceSeconds(int seconds)
		{
			g_grace.store(true, std::memory_order_release);
			g_graceUntil = Now() + std::chrono::seconds((std::max)(0, seconds));
		}

		static bool IsGraceActive()
		{
			if (!g_grace.load(std::memory_order_acquire)) return false;
			if (Now() >= g_graceUntil) {
				g_grace.store(false, std::memory_order_release);
				return false;
			}
			return true;
		}

		static RE::Actor* ResolveAggressor()
		{
			if (g_lastAggressor) {
				auto sp = RE::Actor::LookupByHandle(g_lastAggressor.native_handle());
				return sp.get();
			}
			return nullptr;
		}

		static bool IsActorCloseAndFront(RE::Actor* actor, RE::Actor* player, float maxDist)
		{
			if (!actor || !player) {
				return false;
			}

			const auto pa = player->GetPosition();
			const auto pb = actor->GetPosition();

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

		static bool CanUseAggressorForBleedoutGreet(RE::Actor* player, RE::Actor* aggressor, float& outDistance)
		{
			outDistance = 99999.0f;
			if (!player || !aggressor) {
				return false;
			}
			if (aggressor->IsDead() || aggressor->IsDisabled() || !aggressor->Is3DLoaded()) {
				return false;
			}
			if (aggressor->GetParentCell() != player->GetParentCell()) {
				return false;
			}

			const auto pa = player->GetPosition();
			const auto pb = aggressor->GetPosition();
			const float dx = pb.x - pa.x;
			const float dy = pb.y - pa.y;
			const float dz = pb.z - pa.z;
			outDistance = std::sqrt(dx * dx + dy * dy + dz * dz);

			if (!IsActorCloseAndFront(aggressor, player, 220.0f)) {
				return false;
			}

			bool hasLOS = false;
			if (!aggressor->HasLineOfSight(player, hasLOS) || !hasLOS) {
				return false;
			}

			return true;
		}

		static RE::Actor* FindBestAggressor(float radius)
		{
			auto* player = Player();
			if (!player) return nullptr;
			auto* pCell = player->GetParentCell();
			if (!pCell) return nullptr;

			TFD::ActorScan::Rescan(radius, true);
			RE::Actor* best = nullptr;
			float bestDist = 1.0e30f;
			const auto n = TFD::ActorScan::GetCount();
			for (int i = 0; i < n; ++i) {
				auto e = TFD::ActorScan::GetEntry(i);
				auto sp = e.actor.get();
				auto* a = sp.get();
				if (!a || a->IsDead() || a->IsDisabled()) continue;
				if (!a->Is3DLoaded()) continue;
				if (a->GetFormID() == player->GetFormID()) continue;
				if (a->GetParentCell() != pCell) continue;
				if (!e.inCombat && !e.hostile) continue;
				if (e.dist < bestDist) {
					bestDist = e.dist;
					best = a;
				}
			}
			return best;
		}

		static void ApplyCalmBubble(float radius)
		{
			TFD::AntiAggro::SweepOnce(radius, true);
			TFD::AntiAggro::ScheduleWaves(radius, true, 10, 140);
			TFD::ActorScan::Rescan(radius, true);
			const auto n = TFD::ActorScan::GetCount();
			for (int i = 0; i < n; ++i) {
				auto e = TFD::ActorScan::GetEntry(i);
				auto sp = e.actor.get();
				auto* a = sp.get();
				if (!a || a->IsDead()) continue;
				TFD::AggressionClamp::Apply(a);
			}
		}

		static void RecoverPlayerAfterTeleport()
		{
			auto* p = Player();
			if (!p) return;
			p->NotifyAnimationGraph("BleedoutStop");
			p->NotifyAnimationGraph("GetUpStart");
			const float hpMax = (std::max)(1.0f, p->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float threshPct = std::clamp(TFD::Settings::GetDefeatThresholdPct() / 100.0f, 0.05f, 0.95f);
			const float safePct = std::clamp(threshPct + 0.15f, 0.35f, 0.85f);
			const float target = (std::max)(25.0f, hpMax * safePct);
			const float hpNow = p->GetActorValue(RE::ActorValue::kHealth);
			if (hpNow < target) {
				p->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, (target - hpNow));
			}
			if (p->IsInCombat()) p->StopCombat();
			p->DrawWeaponMagicHands(false);
		}

		static void RecoverPlayerForTransition()
		{
			auto* p = Player();
			if (!p) return;
			p->NotifyAnimationGraph("BleedoutStop");
			p->NotifyAnimationGraph("GetUpStart");
			auto restoreToPct = [&](RE::ActorValue av, float pct, float minValue) {
				const float maxValue = (std::max)(1.0f, p->GetPermanentActorValue(av));
				const float target = (std::max)(minValue, maxValue * pct);
				const float nowValue = p->GetActorValue(av);
				if (nowValue < target) {
					p->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, av, (target - nowValue));
				}
				};
			const float threshPct = std::clamp(TFD::Settings::GetDefeatThresholdPct() / 100.0f, 0.05f, 0.95f);
			const float safeHealthPct = std::clamp(threshPct + 0.10f, 0.55f, 1.00f);
			restoreToPct(RE::ActorValue::kHealth, safeHealthPct, 40.0f);
			restoreToPct(RE::ActorValue::kStamina, 0.95f, 35.0f);
			restoreToPct(RE::ActorValue::kMagicka, 0.95f, 25.0f);
			if (p->IsInCombat()) p->StopCombat();
			p->DrawWeaponMagicHands(false);
		}

		static void TickLeftForDeadCooldown()
		{
			if (!IsLeftForDeadCooldownActive()) return;
			const auto now = Now();
			if (g_leftForDeadNextPulse.time_since_epoch().count() != 0 && now < g_leftForDeadNextPulse) return;
			g_leftForDeadNextPulse = now + std::chrono::seconds(2);
		}

		static void BeginLeftForDeadBlackout(int, const char*)
		{
			// Disabled on C++ side. Native wait / blackout will be handled by CK/Papyrus.
		}

		static bool TickLeftForDeadBlackout()
		{
			return false;
		}

		static void EnterNonCaptiveChoice(const char* reason);

		static void ClearPendingBleedoutChoice(const char* reason)
		{
			if (g_pendingBleedoutChoice && reason && reason[0]) {
				spdlog::info("[TFD][Transition] non-captive bleedout defer cleared reason={}", reason);
			}
			g_pendingBleedoutChoice = false;
			g_pendingBleedoutChoiceReason.clear();
			g_pendingBleedoutChoiceAt = {};
		}

		static void QueueNonCaptiveChoiceAfterBleedout(const char* reason, int settleMs = kBleedoutChoiceSettleMs)
		{
			g_pendingBleedoutChoice = true;
			g_pendingBleedoutChoiceReason = reason ? reason : "unknown";
			g_pendingBleedoutChoiceAt = Now() + std::chrono::milliseconds((std::max)(0, settleMs));
			spdlog::info(
				"[TFD][Transition] non-captive choice deferred until bleedout settles reason={} delayMs={}",
				g_pendingBleedoutChoiceReason,
				(std::max)(0, settleMs));
		}

		static void StartBleedWindow(RE::Actor* player, RE::Actor* aggressor)
		{
			if (!player) {
				return;
			}

			g_inBleedState.store(true, std::memory_order_release);
			g_bleedSawDialogue = false;
			g_bleedStart = Now();
			g_bleedLastSeconds = -1;
			g_bleedPaused = false;
			g_bleedPauseStarted = {};

			const float maxHp = player->GetPermanentActorValue(RE::ActorValue::kHealth);
			const float minHp = (std::max)(1.0f, maxHp * 0.02f);
			g_minHp = minHp;
			ClampHealth(player, g_minHp);
			player->NotifyAnimationGraph("BleedoutStart");

			const float radius = (std::max)(2000.0f, TFD::Settings::GetSweepRadius());
			ApplyCalmBubble(radius);

			if (aggressor) {
				g_lastAggressor = aggressor->GetHandle();

				if (!IsCaptiveSupportedAggressor(aggressor)) {
					spdlog::info("[TFD][Defeat] aggressor {:08X} not captive-supported (non-humanoid) -> LeftForDead", aggressor->GetFormID());
					TFD::ForceGreet::Cancel();
					TFD::FactionMask::Clear();
					QueueNonCaptiveChoiceAfterBleedout("unsupported_aggressor_nonhumanoid");
					return;
				}

				const bool captiveSupported = TFD::FactionMask::ApplyFromAggressor(aggressor);
				if (!captiveSupported) {
					spdlog::info("[TFD][Defeat] aggressor {:08X} has no captive-supported allowlist faction -> LeftForDead", aggressor->GetFormID());
					TFD::ForceGreet::Cancel();
					TFD::FactionMask::Clear();
					QueueNonCaptiveChoiceAfterBleedout("unsupported_aggressor_allowlist");
					return;
				}

				float greetDistance = 99999.0f;
				if (!CanUseAggressorForBleedoutGreet(player, aggressor, greetDistance)) {
					spdlog::info("[TFD][Defeat] aggressor {:08X} not greetable in-place dist={:.1f} -> noncaptive fallback",
						aggressor->GetFormID(), greetDistance);
					TFD::ForceGreet::Cancel();
					TFD::FactionMask::Clear();
					QueueNonCaptiveChoiceAfterBleedout("aggressor_not_greetable");
					return;
				}

				TFD::ForceGreet::BeginBleedout(aggressor);
			}

			const int bleedSeconds = TFD::Settings::GetBleedWindowSeconds();
			char msg[96]{};
			std::snprintf(msg, sizeof(msg), "TFDEngine: Bleeding... (%ds)", bleedSeconds);
			RE::DebugNotification(msg);
			spdlog::info("[TFD][Defeat] bleed window started ({}s)", bleedSeconds);
		}

		static void EnterEscapeFromLockpick(RE::TESObjectREFR* door)
		{
			EnterEscapeCommit("lockpick", door);
		}

		static void EnterNonCaptiveChoice(const char* reason)
		{
			TFD::ForceGreet::Cancel();
			TFD::FactionMask::Clear();
			ClearEscapeContext();
			ResetLockpickWatch();
			g_grace.store(false, std::memory_order_release);
			g_lastAggressor.reset();
			g_bleedPaused = false;
			g_bleedPauseStarted = {};
			g_inBleedState.store(false, std::memory_order_release);
			g_minHp = 0.0f;
			g_bleedSawDialogue = false;
			g_bleedLastSeconds = -1;
			g_prevDialogueOpen = false;
			g_prevLockpickOpen = false;
			ClearPendingBleedoutChoice("enter_non_captive_choice");
			SetCaptiveRuntime(false, CaptivePhaseValue::None);

			auto* player = Player();
			if (player) {
				player->NotifyAnimationGraph("BleedoutStop");
				player->NotifyAnimationGraph("GetUpStart");
				if (player->IsInCombat()) {
					player->StopCombat();
				}
				player->DrawWeaponMagicHands(false);
			}

			const float radius = (std::max)(3200.0f, TFD::Settings::GetSweepRadius() + 1200.0f);
			TFD::AntiAggro::SweepOnce(radius, false);
			TFD::ActorScan::Rescan(radius, false);
			const auto count = TFD::ActorScan::GetCount();
			for (int i = 0; i < count; ++i) {
				auto entry = TFD::ActorScan::GetEntry(i);
				auto actorSP = entry.actor.get();
				auto* actor = actorSP.get();
				if (!actor || actor->IsDead() || actor->IsDisabled()) {
					continue;
				}
				TFD::AggressionClamp::Apply(actor);
				actor->StopCombat();
				actor->EvaluatePackage(true, false);
			}

			QueueNonCaptiveChoiceRequest(reason);
			spdlog::warn("[TFD][Transition] non-captive choice armed reason={}", reason ? reason : "unknown");
		}

		static void DoBlackoutTeleport()
		{
			g_inBleedState.store(false, std::memory_order_release);
			g_minHp = 0.0f;
			g_bleedSawDialogue = false;
			g_bleedPaused = false;
			g_bleedPauseStarted = {};
			g_bleedStart = Now();
			g_bleedLastSeconds = -1;
			TFD::ForceGreet::Cancel();
			RE::DebugNotification("TFDEngine: Blackout -> Captive (1h)");
			if (!ResolveCaptiveMarkerForOutcome()) {
				EnterNonCaptiveChoice("marker_not_found");
				return;
			}
			ShowBlackoutFader();
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			AdvanceGameHoursSoft(1.0f);
			std::this_thread::sleep_for(std::chrono::milliseconds(900));
			if (!TeleportPlayerToCachedMarkerNow()) {
				HideBlackoutFader();
				EnterNonCaptiveChoice("teleport_failed");
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(150));
			RecoverPlayerAfterTeleport();
			SetGraceSeconds(5);
			SetCaptiveRuntime(true, CaptivePhaseValue::Captive);
			g_prevDialogueOpen = IsDialogueOpen();
			g_prevLockpickOpen = IsLockpickingOpen();
			ResetLockpickWatch();
			ArmEscapeContextFromCurrentState();
			if (g_captiveDoor.HasDoor()) {
				g_captiveDoor.SealToInitial(true);
			}
			ApplyCalmBubble((std::max)(2000.0f, TFD::Settings::GetSweepRadius()));
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			HideBlackoutFader();
			spdlog::info("[TFD][Captive] entered captivePhase");
		}

		static void UpdateLockpickEscapeWatch()
		{
			if (!g_captiveState || g_captivePhase != CaptivePhaseValue::Captive) {
				ResetLockpickWatch();
				return;
			}
			const bool lockOpen = IsLockpickingOpen();
			if (lockOpen && !g_prevLockpickOpen) {
				auto* rawTarget = RE::LockpickingMenu::GetTargetReference();
				auto* target = ResolveLockpickDoorCandidate(rawTarget);
				if (target) {
					g_lockpickDoorCandidate = target->GetHandle();
					g_lockpickDoorWasLocked = IsRefLocked(target);
					if (g_lockpickDoorWasLocked) BindCaptiveDoor(target);
					spdlog::info("[TFD][Captive] lockpick opened on door {:08X} wasLocked={} nearMarker=1 rawTarget={:08X}", target->GetFormID(), g_lockpickDoorWasLocked ? 1 : 0, rawTarget ? rawTarget->GetFormID() : 0);
				}
				else {
					g_lockpickDoorCandidate.reset();
					g_lockpickDoorWasLocked = false;
					auto* boundDoor = ResolveBoundEscapeDoor();
					if (rawTarget) {
						spdlog::info("[TFD][Captive] lockpick target {:08X} ignored (door={} nearMarker={} fallbackBoundDoor={:08X})", rawTarget->GetFormID(), IsDoorRef(rawTarget) ? 1 : 0, IsDoorNearCaptiveMarker(rawTarget) ? 1 : 0, boundDoor ? boundDoor->GetFormID() : 0);
					}
					else {
						spdlog::info("[TFD][Captive] lockpick target null (fallbackBoundDoor={:08X})", boundDoor ? boundDoor->GetFormID() : 0);
					}
				}
			}
			if (g_captiveDoor.HasDoor() && g_captiveDoor.UpdateWatcher()) {
				EnterEscapeCommit("door_watch", nullptr);
			}
			if (!lockOpen && g_prevLockpickOpen) {
				RE::TESObjectREFR* door = nullptr;
				if (g_lockpickDoorCandidate) {
					auto ptr = g_lockpickDoorCandidate.get();
					door = ptr.get();
				}
				if (!door) door = ResolveBoundEscapeDoor();
				if (door && IsDoorRef(door) && g_lockpickDoorWasLocked && !IsRefLocked(door)) {
					EnterEscapeFromLockpick(door);
				}
				else {
					ResetLockpickWatch();
				}
			}
			g_prevLockpickOpen = lockOpen;
		}

		static void TickUI()
		{
			struct Guard {
				~Guard() { g_tickPending.clear(std::memory_order_release); }
			} guard;
			if (!TFD::Settings::GetEnabled()) return;
			if (g_loadTransition.load(std::memory_order_acquire)) return;
			auto* ui = RE::UI::GetSingleton();
			PollTransitionResult();
			ProcessPendingRescueNearBed();
			if (IsTransitionAwaiting()) {
				MaintainTransitionCalmWindow();
				UpdatePreCombatState();
				return;
			}
			TFD::ForceGreet::Tick();
			NormalizeInvalidCaptivePair();
			if (g_captiveState && g_captivePhase == CaptivePhaseValue::Captive) {
				const bool dialogOpen = IsDialogueOpen();
				if (!dialogOpen && g_prevDialogueOpen) {
					ApplyCalmBubble((std::max)(1800.0f, TFD::Settings::GetSweepRadius()));
					spdlog::info("[TFD][Captive] Dialogue closed -> calm burst");
				}
				g_prevDialogueOpen = dialogOpen;
				UpdateLockpickEscapeWatch();
				TryCommitEscapeByRadius();
			}
			else if (g_captiveState && g_captivePhase == CaptivePhaseValue::Escape) {
				TryResolveEscapeByLocation();
			}
			if (ui && ui->GameIsPaused()) return;
			auto* player = Player();
			if (!player) {
				SyncPreCombatGlobal(false);
				return;
			}
			UpdateRescueContextCache(player);
			UpdatePreCombatState();
			if (IsLeftForDeadCooldownActive()) {
				TickLeftForDeadCooldown();
				return;
			}
			if (IsGraceActive()) return;
			if (g_inBleedState.load(std::memory_order_acquire)) {
				if (g_minHp > 0.0f) ClampHealth(player, g_minHp);
				const int bleedSeconds = TFD::Settings::GetBleedWindowSeconds();
				const bool dOpen = IsDialogueOpen();
				if (dOpen) {
					g_bleedSawDialogue = true;
					if (!g_bleedPaused) {
						g_bleedPaused = true;
						g_bleedPauseStarted = Now();
						spdlog::info("[TFD][Defeat] bleed countdown paused by dialogue");
					}
					g_prevDialogueOpen = true;
					return;
				}
				if (g_bleedPaused) {
					g_bleedStart += (Now() - g_bleedPauseStarted);
					g_bleedPaused = false;
					g_bleedPauseStarted = {};
					g_bleedLastSeconds = -1;
					spdlog::info("[TFD][Defeat] bleed countdown resumed after dialogue");
				}
				if (g_bleedSawDialogue && g_prevDialogueOpen) {
					g_prevDialogueOpen = false;
					if (ResolveCaptiveMarkerForOutcome()) {
						spdlog::info("[TFD][Defeat] bleedout dialogue closed -> captive marker found");
						DoBlackoutTeleport();
					}
					else {
						spdlog::info("[TFD][Defeat] bleedout dialogue closed -> no marker -> LeftForDead");
						g_inBleedState.store(false, std::memory_order_release);
						g_minHp = 0.0f;
						g_bleedSawDialogue = false;
						g_bleedLastSeconds = -1;
						EnterNonCaptiveChoice("dialogue_closed_no_marker");
					}
					return;
				}
				if (g_pendingBleedoutChoice && Now() >= g_pendingBleedoutChoiceAt) {
					const auto deferredReason = g_pendingBleedoutChoiceReason;
					spdlog::info("[TFD][Transition] non-captive choice released after bleedout settle reason={}", deferredReason);
					ClearPendingBleedoutChoice("released");
					EnterNonCaptiveChoice(deferredReason.c_str());
					return;
				}
				const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(Now() - g_bleedStart).count();
				const int remain = bleedSeconds - static_cast<int>(elapsed);
				if (remain != g_bleedLastSeconds) {
					g_bleedLastSeconds = remain;
					if (remain > 0) {
						char msg[96]{};
						std::snprintf(msg, sizeof(msg), "TFDEngine: Bleeding... (%ds)", remain);
						RE::DebugNotification(msg);
					}
				}
				if (remain <= 0) {
					g_bleedStart = Now();
					g_bleedLastSeconds = -1;
					g_inBleedState.store(false, std::memory_order_release);
					g_minHp = 0.0f;
					g_bleedSawDialogue = false;
					EnterNonCaptiveChoice("bleed_timeout");
				}
				return;
			}
			const float hpNow = player->GetActorValue(RE::ActorValue::kHealth);
			const float hpMax = (std::max)(1.0f, player->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float pct = (hpNow / hpMax) * 100.0f;
			const float thresh = TFD::Settings::GetDefeatThresholdPct();
			if (pct <= thresh) {
				const float scanRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius());
				auto* aggressor = FindBestAggressor(scanRadius);
				if (!aggressor) {
					spdlog::info("[TFD][Defeat] no valid NPC aggressor -> bleedout settle then LeftForDead choice");
					StartBleedWindow(player, nullptr);
					QueueNonCaptiveChoiceAfterBleedout("no_valid_npc");
					SetGraceSeconds(1);
					return;
				}
				StartBleedWindow(player, aggressor);
				SetGraceSeconds(1);
				return;
			}
		}

		static void WorkerLoop()
		{
			while (g_running.load(std::memory_order_acquire)) {
				if (!g_tickPending.test_and_set(std::memory_order_acq_rel)) {
					auto* task = SKSE::GetTaskInterface();
					if (task) task->AddTask([]() { TickUI(); });
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
		}
	}

	void Install()
	{
		if (g_installed.exchange(true, std::memory_order_acq_rel)) return;
		g_running.store(true, std::memory_order_release);
		g_loadTransition.store(false, std::memory_order_release);
		SetCaptiveRuntimeOnly(false, CaptivePhaseValue::None);
		g_prevDialogueOpen = false;
		ResetLockpickWatch();
		ClearEscapeContext();
		g_inBleedState.store(false, std::memory_order_release);
		g_minHp = 0.0f;
		g_bleedStart = Now();
		g_bleedLastSeconds = -1;
		ClearPendingBleedoutChoice("install");
		TFD::FactionMask::Initialize();
		TFD::Location::Initialize();
		TFD::ForceGreet::Install();
		g_worker = std::thread([]() { WorkerLoop(); });
		TFD::DefeatMonitor::ApplyQueuedProgressState();
		spdlog::info("[TFD][Defeat] monitor installed");
	}

	void Shutdown()
	{
		if (!g_installed.exchange(false, std::memory_order_acq_rel)) return;
		g_running.store(false, std::memory_order_release);
		if (g_worker.joinable()) g_worker.join();
		SetCaptiveRuntime(false, CaptivePhaseValue::None);
		g_hasQueuedProgressState = false;
		g_queuedCaptiveState = false;
		g_queuedCaptivePhase = CaptivePhaseValue::None;
		g_inBleedState.store(false, std::memory_order_release);
		g_loadTransition.store(false, std::memory_order_release);
		ClearPendingBleedoutChoice("shutdown");
		ResetLockpickWatch();
		ClearEscapeContext();
		ClearLeftForDeadCooldown();
		spdlog::info("[TFD][Defeat] monitor shutdown");
	}

	void ResetGrace()
	{
		g_grace.store(false, std::memory_order_release);
		ClearLeftForDeadCooldown();
	}

	bool GetCaptiveStateForSave()
	{
		return g_captiveState;
	}

	std::uint32_t GetCaptivePhaseForSave()
	{
		if (g_captiveState && g_captivePhase == CaptivePhaseValue::None) return 2u;
		return static_cast<std::uint32_t>(static_cast<int>(g_captivePhase));
	}

	void QueueLoadedProgressState(bool stateActive, std::uint32_t phaseRaw)
	{
		g_hasQueuedProgressState = true;
		g_queuedCaptiveState = stateActive;
		CaptivePhaseValue phase = stateActive ? PhaseFromRaw(phaseRaw) : CaptivePhaseValue::None;
		if (stateActive && phase == CaptivePhaseValue::None) phase = CaptivePhaseValue::Escape;
		g_queuedCaptivePhase = phase;
		spdlog::info("[TFD][Defeat] QueueLoadedProgressState state={} phase={} normalized={}", stateActive ? 1 : 0, phaseRaw, static_cast<int>(g_queuedCaptivePhase));
	}

	void QueueDefaultProgressState()
	{
		g_hasQueuedProgressState = true;
		g_queuedCaptiveState = false;
		g_queuedCaptivePhase = CaptivePhaseValue::None;
		spdlog::info("[TFD][Defeat] QueueDefaultProgressState");
	}

	bool HasQueuedProgressState()
	{
		return g_hasQueuedProgressState;
	}

	void ApplyQueuedProgressState()
	{
		if (!g_hasQueuedProgressState) QueueDefaultProgressState();
		ClearLeftForDeadCooldown();
		SetCaptiveRuntime(g_queuedCaptiveState, g_queuedCaptivePhase);
		g_prevDialogueOpen = IsDialogueOpen();
		g_prevLockpickOpen = IsLockpickingOpen();
		if (g_queuedCaptiveState && g_queuedCaptivePhase == CaptivePhaseValue::Captive) {
			TFD::Location::RescanCaptiveMarker();
			ArmEscapeContextFromCurrentState();
		}
		else {
			ResetLockpickWatch();
			ClearEscapeContext();
		}
		UpdatePreCombatState();
		spdlog::info("[TFD][Defeat] ApplyQueuedProgressState state={} phase={}", g_queuedCaptiveState ? 1 : 0, static_cast<int>(g_queuedCaptivePhase));
	}

	void ResetForLoad()
	{
		g_grace.store(false, std::memory_order_release);
		g_inBleedState.store(false, std::memory_order_release);
		g_minHp = 0.0f;
		g_bleedSawDialogue = false;
		ClearPendingBleedoutChoice("reset_for_load");
		g_bleedStart = Now();
		g_bleedLastSeconds = -1;
		g_lastAggressor = RE::ActorHandle{};
		SetCaptiveRuntimeOnly(false, CaptivePhaseValue::None);
		g_prevDialogueOpen = false;
		ResetLockpickWatch();
		ClearEscapeContext();
		TFD::FactionMask::Clear();
		TFD::AggressionClamp::Clear();
		TFD::ForceGreet::Cancel();
		ClearLeftForDeadCooldown();
		ClearPendingRescueNearBed("reset_for_load", true);
		spdlog::info("[TFD][Defeat] ResetForLoad -> runtime only");
	}

	void SetLoadTransition(bool active)
	{
		g_loadTransition.store(active, std::memory_order_release);
		if (active) {
			ClearPendingBleedoutChoice("load_transition");
			TFD::ForceGreet::Cancel();
			ResetLockpickWatch();
			ClearPendingRescueNearBed("load_transition", true);
			spdlog::info("[TFD][Defeat] SetLoadTransition(true)");
		}
		else {
			spdlog::info("[TFD][Defeat] SetLoadTransition(false)");
		}
	}

	bool IsLeftForDeadRecoveryActive()
	{
		return g_leftForDeadActive;
	}

	bool IsCaptivePhase()
	{
		return g_captiveState && g_captivePhase == CaptivePhaseValue::Captive;
	}
}
