#include "TFDDefeatMonitor.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstdio>
#include <cstdint>

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
		static bool g_loggedCaptiveStateGlobal = false;
		static bool g_loggedCaptivePhaseGlobal = false;
		static bool g_loggedPreCombatStateGlobal = false;

		static inline std::chrono::steady_clock::time_point Now()
		{
			return std::chrono::steady_clock::now();
		}

		std::atomic_bool g_inBleedState{ false };
		float g_minHp{ 0.0f };

		std::chrono::steady_clock::time_point g_bleedStart{};
		int g_bleedLastSeconds = -1;

		std::atomic_bool g_grace{ false };
		std::chrono::steady_clock::time_point g_graceUntil{};

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

		bool g_hasQueuedProgressState = false;
		bool g_queuedCaptiveState = false;
		CaptivePhaseValue g_queuedCaptivePhase = CaptivePhaseValue::None;


		static CaptivePhaseValue PhaseFromRaw(std::uint32_t raw)
		{
			switch (raw) {
			case 1:
				return CaptivePhaseValue::Captive;
			case 2:
				return CaptivePhaseValue::Escape;
			case 3:
				return CaptivePhaseValue::ReleasedWork;
			case 4:
				return CaptivePhaseValue::Scene;
			default:
				return CaptivePhaseValue::None;
			}
		}

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

		static RE::PlayerCharacter* Player()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

		static void UpdatePreCombatState()
		{
			auto* player = Player();

			bool preCombat = false;
			if (player) {
				preCombat = true;
				if (g_loadTransition.load(std::memory_order_acquire)) {
					preCombat = false;
				}
				if (g_inBleedState.load(std::memory_order_acquire)) {
					preCombat = false;
				}
				if (g_captiveState) {
					preCombat = false;
				}
				if (player->IsInCombat()) {
					preCombat = false;
				}

				auto* st = player->AsActorState();
				if (st && st->IsBleedingOut()) {
					preCombat = false;
				}
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
			if (!ref) {
				return false;
			}

			auto* base = ref->GetBaseObject();
			if (!base) {
				return false;
			}

			return base->GetFormType() == RE::FormType::Door;
		}

		static bool IsRefLocked(RE::TESObjectREFR* ref)
		{
			if (!ref) {
				return false;
			}
			auto* lock = ref->GetLock();
			return lock && lock->IsLocked();
		}
		static RE::TESObjectCELL* GetParentCell(RE::TESObjectREFR* ref)
		{
			return ref ? ref->GetParentCell() : nullptr;
		}

		static RE::BGSLocation* GetLocationFromRef(RE::TESObjectREFR* ref)
		{
			auto* cell = GetParentCell(ref);
			return cell ? cell->GetLocation() : nullptr;
		}

		static RE::TESObjectREFR* FindNearestDoorNearCaptiveMarker()
		{
			RE::TESObjectREFR* marker = nullptr;
			if (g_captiveMarker) {
				auto ptr = g_captiveMarker.get();
				marker = ptr.get();
			}
			if (!marker) {
				marker = TFD::Location::GetCachedCaptiveMarker();
			}
			if (!marker) {
				return nullptr;
			}

			auto* cell = marker->GetParentCell();
			if (!cell) {
				return nullptr;
			}

			const auto mp = marker->GetPosition();
			RE::TESObjectREFR* best = nullptr;
			double bestDistSq = 384.0 * 384.0;

			cell->ForEachReferenceInRange(mp, 384.0f,
				[&](RE::TESObjectREFR* candidate) -> RE::BSContainer::ForEachResult {
					if (!candidate || candidate == marker) {
						return RE::BSContainer::ForEachResult::kContinue;
					}
					if (!IsDoorRef(candidate)) {
						return RE::BSContainer::ForEachResult::kContinue;
					}

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
			g_captiveDoor.Reset();
		}

		static void ArmEscapeContextFromCurrentState()
		{
			auto* player = Player();
			if (!player) {
				return;
			}

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
					g_captiveDoor.Bind(door);
					spdlog::info("[TFD][Captive] Bound nearest captive door {:08X} on captive enter", door->GetFormID());
				}
			}

			spdlog::info("[TFD][Captive] Escape context armed marker={:08X} cell={:08X} loc={:08X}",
				marker ? marker->GetFormID() : 0,
				g_captiveCellFormID,
				g_captiveLocationFormID);
		}

		static bool IsDoorNearCaptiveMarker(RE::TESObjectREFR* door)
		{
			if (!door || !IsDoorRef(door)) {
				return false;
			}
			RE::TESObjectREFR* marker = nullptr;
			if (g_captiveMarker) {
				auto ptr = g_captiveMarker.get();
				marker = ptr.get();
			}
			if (!marker) {
				marker = TFD::Location::GetCachedCaptiveMarker();
			}
			if (!marker) {
				return true;
			}
			const auto dp = door->GetPosition();
			const auto mp = marker->GetPosition();
			const double dx = static_cast<double>(dp.x - mp.x);
			const double dy = static_cast<double>(dp.y - mp.y);
			const double dz = static_cast<double>(dp.z - mp.z);
			const double distSq = dx * dx + dy * dy + dz * dz;
			return distSq <= (384.0 * 384.0);
		}

		static void EnterEscapeCommit(const char* reason, RE::TESObjectREFR* door)
		{
			if (!g_captiveState || g_captivePhase != CaptivePhaseValue::Captive) {
				return;
			}

			TFD::ForceGreet::Cancel();
			TFD::FactionMask::Clear();
			TFD::AggressionClamp::Clear();
			g_grace.store(false, std::memory_order_release);

			SetCaptiveRuntime(true, CaptivePhaseValue::Escape);
			UpdatePreCombatState();
			ResetLockpickWatch();
			g_prevDialogueOpen = false;

			if (door) {
				g_captiveDoor.Bind(door);
			}

			spdlog::info("[TFD][Captive] EscapeCommit reason={} door={:08X}",
				reason ? reason : "unknown",
				door ? door->GetFormID() : 0);
		}

		static void TryCommitEscapeByRadius()
		{
			if (!g_captiveState || g_captivePhase != CaptivePhaseValue::Captive) {
				g_escapeRadiusActive = false;
				return;
			}

			auto* player = Player();
			if (!player) {
				return;
			}

			RE::TESObjectREFR* marker = nullptr;
			if (g_captiveMarker) {
				auto ptr = g_captiveMarker.get();
				marker = ptr.get();
			}
			if (!marker) {
				marker = TFD::Location::GetCachedCaptiveMarker();
			}
			if (!marker) {
				return;
			}

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
			if (!g_captiveState || g_captivePhase != CaptivePhaseValue::Escape) {
				return;
			}

			auto* player = Player();
			if (!player) {
				return;
			}

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
			if (!actor) {
				return;
			}

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
			if (!g_grace.load(std::memory_order_acquire)) {
				return false;
			}
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

		static RE::Actor* FindBestAggressor(float radius)
		{
			auto* player = Player();
			if (!player) {
				return nullptr;
			}

			TFD::ActorScan::Rescan(radius, true);

			RE::Actor* best = nullptr;
			float bestDist = 1.0e30f;

			const auto n = TFD::ActorScan::GetCount();
			for (int i = 0; i < n; ++i) {
				auto e = TFD::ActorScan::GetEntry(i);
				auto sp = e.actor.get();
				auto* a = sp.get();
				if (!a || a->IsDead()) {
					continue;
				}
				if (!e.inCombat && !e.hostile) {
					continue;
				}
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
				if (!a || a->IsDead()) {
					continue;
				}
				TFD::AggressionClamp::Apply(a);
			}
		}

		static void RecoverPlayerAfterTeleport()
		{
			auto* p = Player();
			if (!p) {
				return;
			}

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

			if (p->IsInCombat()) {
				p->StopCombat();
			}
			p->DrawWeaponMagicHands(false);
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

			const float maxHp = player->GetPermanentActorValue(RE::ActorValue::kHealth);
			const float minHp = (std::max)(1.0f, maxHp * 0.02f);
			g_minHp = minHp;

			ClampHealth(player, g_minHp);
			player->NotifyAnimationGraph("BleedoutStart");

			const float radius = (std::max)(2000.0f, TFD::Settings::GetSweepRadius());
			ApplyCalmBubble(radius);

			if (aggressor) {
				g_lastAggressor = aggressor->GetHandle();
				TFD::FactionMask::ApplyFromAggressor(aggressor);
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

		static void DoBlackoutTeleport()
		{
			auto* aggressor = ResolveAggressor();

			g_inBleedState.store(false, std::memory_order_release);
			g_minHp = 0.0f;
			g_bleedSawDialogue = false;

			g_bleedStart = Now();
			g_bleedLastSeconds = -1;

			RE::DebugNotification("TFDEngine: Blackout -> Captive");

			if (aggressor) {
				TFD::Location::RescanCaptiveMarkerWithAggressor(aggressor, true);
			} else {
				TFD::Location::RescanCaptiveMarker();
			}

			TFD::Location::TeleportToCaptiveMarker();
			RecoverPlayerAfterTeleport();

			if (g_captiveDoor.HasDoor()) {
				g_captiveDoor.SealToInitial(true);
			}

			SetGraceSeconds(5);
			SetCaptiveRuntime(true, CaptivePhaseValue::Captive);
			g_prevDialogueOpen = IsDialogueOpen();
			g_prevLockpickOpen = IsLockpickingOpen();
			ResetLockpickWatch();
			ArmEscapeContextFromCurrentState();

			ApplyCalmBubble((std::max)(2000.0f, TFD::Settings::GetSweepRadius()));

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
				auto* target = RE::LockpickingMenu::GetTargetReference();
				if (target && IsDoorRef(target) && IsDoorNearCaptiveMarker(target)) {
					g_lockpickDoorCandidate = target->GetHandle();
					g_lockpickDoorWasLocked = IsRefLocked(target);
					if (g_lockpickDoorWasLocked) {
						g_captiveDoor.Bind(target);
					}
					spdlog::info("[TFD][Captive] lockpick opened on door {:08X} wasLocked={} nearMarker=1",
						target->GetFormID(), g_lockpickDoorWasLocked ? 1 : 0);
				} else {
					g_lockpickDoorCandidate.reset();
					g_lockpickDoorWasLocked = false;
					if (target) {
						spdlog::info("[TFD][Captive] lockpick target {:08X} ignored (door={} nearMarker={})",
							target->GetFormID(),
							IsDoorRef(target) ? 1 : 0,
							IsDoorNearCaptiveMarker(target) ? 1 : 0);
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

				if (door && IsDoorRef(door) && g_lockpickDoorWasLocked && !IsRefLocked(door)) {
					EnterEscapeFromLockpick(door);
				} else {
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

			if (!TFD::Settings::GetEnabled()) {
				return;
			}

			if (g_loadTransition.load(std::memory_order_acquire)) {
				return;
			}

			auto* ui = RE::UI::GetSingleton();
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
			} else if (g_captiveState && g_captivePhase == CaptivePhaseValue::Escape) {
				TryResolveEscapeByLocation();
			}

			if (ui && ui->GameIsPaused()) {
				return;
			}

			auto* player = Player();
			if (!player) {
				SyncPreCombatGlobal(false);
				return;
			}

			UpdatePreCombatState();

			if (IsGraceActive()) {
				return;
			}

			if (g_inBleedState.load(std::memory_order_acquire)) {
				if (g_minHp > 0.0f) {
					ClampHealth(player, g_minHp);
				}

				const int bleedSeconds = TFD::Settings::GetBleedWindowSeconds();
				const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(Now() - g_bleedStart).count();
				const int remain = bleedSeconds - static_cast<int>(elapsed);

				const bool dOpen = IsDialogueOpen();
				if (dOpen) {
					g_bleedSawDialogue = true;
				}
				if (g_bleedSawDialogue && !dOpen) {
					g_bleedStart = Now();
					g_bleedLastSeconds = -1;
					spdlog::info("[TFD][Defeat] bleedout dialogue closed -> early teleport");
					DoBlackoutTeleport();
					return;
				}

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
					DoBlackoutTeleport();
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
					if (task) {
						task->AddTask([]() { TickUI(); });
					}
				}

				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
		}
	}

	void Install()
	{
		if (g_installed.exchange(true, std::memory_order_acq_rel)) {
			return;
		}

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

		TFD::FactionMask::Initialize();
		TFD::Location::Initialize();
		TFD::ForceGreet::Install();

		g_worker = std::thread([]() { WorkerLoop(); });

		TFD::DefeatMonitor::ApplyQueuedProgressState();

		spdlog::info("[TFD][Defeat] monitor installed");
	}

	void Shutdown()
	{
		if (!g_installed.exchange(false, std::memory_order_acq_rel)) {
			return;
		}

		g_running.store(false, std::memory_order_release);

		if (g_worker.joinable()) {
			g_worker.join();
		}

		SetCaptiveRuntime(false, CaptivePhaseValue::None);
		g_hasQueuedProgressState = false;
		g_queuedCaptiveState = false;
		g_queuedCaptivePhase = CaptivePhaseValue::None;
		g_inBleedState.store(false, std::memory_order_release);
		g_loadTransition.store(false, std::memory_order_release);
		ResetLockpickWatch();
		ClearEscapeContext();

		spdlog::info("[TFD][Defeat] monitor shutdown");
	}

	void ResetGrace()
	{
		g_grace.store(false, std::memory_order_release);
	}

	bool GetCaptiveStateForSave()
	{
		return g_captiveState;
	}

	std::uint32_t GetCaptivePhaseForSave()
	{
		if (g_captiveState && g_captivePhase == CaptivePhaseValue::None) {
			return 2u;
		}

		return static_cast<std::uint32_t>(static_cast<int>(g_captivePhase));
	}

	void QueueLoadedProgressState(bool stateActive, std::uint32_t phaseRaw)
	{
		g_hasQueuedProgressState = true;
		g_queuedCaptiveState = stateActive;

		CaptivePhaseValue phase = stateActive ? PhaseFromRaw(phaseRaw) : CaptivePhaseValue::None;
		if (stateActive && phase == CaptivePhaseValue::None) {
			phase = CaptivePhaseValue::Escape;
		}

		g_queuedCaptivePhase = phase;

		spdlog::info("[TFD][Defeat] QueueLoadedProgressState state={} phase={} normalized={}",
			stateActive ? 1 : 0,
			phaseRaw,
			static_cast<int>(g_queuedCaptivePhase));
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
		if (!g_hasQueuedProgressState) {
			QueueDefaultProgressState();
		}

		SetCaptiveRuntime(g_queuedCaptiveState, g_queuedCaptivePhase);
		g_prevDialogueOpen = IsDialogueOpen();
		g_prevLockpickOpen = IsLockpickingOpen();

		if (g_queuedCaptiveState && g_queuedCaptivePhase == CaptivePhaseValue::Captive) {
			TFD::Location::RescanCaptiveMarker();
			ArmEscapeContextFromCurrentState();
		} else {
			ResetLockpickWatch();
			ClearEscapeContext();
		}

		UpdatePreCombatState();

		spdlog::info("[TFD][Defeat] ApplyQueuedProgressState state={} phase={}",
			g_queuedCaptiveState ? 1 : 0,
			static_cast<int>(g_queuedCaptivePhase));
	}

	void ResetForLoad()
	{
		g_grace.store(false, std::memory_order_release);
		g_inBleedState.store(false, std::memory_order_release);
		g_minHp = 0.0f;
		g_bleedSawDialogue = false;

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

		spdlog::info("[TFD][Defeat] ResetForLoad -> runtime only");
	}

	void SetLoadTransition(bool active)
	{
		g_loadTransition.store(active, std::memory_order_release);
		if (active) {
			TFD::ForceGreet::Cancel();
			ResetLockpickWatch();
			spdlog::info("[TFD][Defeat] SetLoadTransition(true)");
		} else {
			spdlog::info("[TFD][Defeat] SetLoadTransition(false)");
		}
	}

	bool IsCaptivePhase()
	{
		return g_captiveState && g_captivePhase == CaptivePhaseValue::Captive;
	}
}
