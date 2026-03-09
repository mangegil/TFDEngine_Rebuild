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
#include <thread>
#include <cmath>

#include <spdlog/spdlog.h>

#include "TFDSettings.h"
#include "TFDLocation.h"
#include "TFDActorScan.h"
#include "TFDAntiAggro.h"
#include "TFDForceGreet.h"
#include "TFDPreCombatGreet.h"
#include "TFDDefeatMonitor.h"

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

		// Source of truth captive = native defeat monitor
		static bool IsCaptivePhase()
		{
			return TFD::DefeatMonitor::IsCaptivePhase();
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

		static RE::Actor* PickPreCombatTargetSameCellLoaded(float radius)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return nullptr;
			}

			TFD::ActorScan::Rescan(radius, true);
			return TFD::ActorScan::GetBestPreCombatCandidate();
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
			ImGuiMCP::Text("CaptivePhase(C++): %.0f", IsCaptivePhase() ? 1.0f : 0.0f);
			ImGuiMCP::Text("CaptiveState(Global): %.0f", GetGlobalValue(gCaptiveState) >= 0.5f ? 1.0f : 0.0f);
			ImGuiMCP::Text("PreCombatState: %.0f", IsPreCombatPhase() ? 1.0f : 0.0f);
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

						SendBridgeEvent("TFDCaptiveClearAll");
						SendBridgeAssignActor("TFDCaptiveAssign", captor);
						TFD::ForceGreet::BeginCaptiveMarker(captor);

						RE::DebugNotification("TFD: Calling Captor");
						continue;
					}

					// Precombat
					auto* target = PickPreCombatTargetSameCellLoaded(3500.0f);
					if (!target) {
						RE::DebugNotification("TFD: No PreCombat Target");
						continue;
					}

					if (!TFD::PreCombatGreet::BeginForActor(target)) {
						RE::DebugNotification("TFD: PreCombat Failed");
						continue;
					}

					RE::DebugNotification("TFD: PreCombat Truce");
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