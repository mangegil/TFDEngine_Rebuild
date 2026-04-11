#include "TFDTransitionRuntime.h"

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

#include <functional>
#include <thread>

namespace TFD::TransitionRuntime
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		bool g_pendingFadeIn = false;
		Kind g_pendingFadeInKind = Kind::None;
		Clock::time_point g_pendingFadeInNotBefore{};
		bool g_pendingFadeInSawLoadingMenu = false;
	}

	const char* GetKindName(Kind kind)
	{
		switch (kind) {
		case Kind::Captive:
			return "captive";
		case Kind::Rescue:
			return "rescue";
		case Kind::Recover:
			return "recover";
		default:
			return "none";
		}
	}

	bool QueueRequest(Kind kind, bool fadeIn, const char* reason)
	{
		spdlog::info("[TFD][Transition] cinematic request skipped (globals removed) kind={} phase={} reason={}",
			GetKindName(kind),
			fadeIn ? "fadein" : "fadeout",
			reason ? reason : "unknown");
		return false;
	}

	void ClearPendingFadeIn()
	{
		g_pendingFadeIn = false;
		g_pendingFadeInKind = Kind::None;
		g_pendingFadeInNotBefore = {};
		g_pendingFadeInSawLoadingMenu = false;
	}

	void ArmPendingFadeIn(Kind kind, std::chrono::steady_clock::time_point notBefore, bool sawLoadingMenu)
	{
		g_pendingFadeIn = kind != Kind::None;
		g_pendingFadeInKind = kind;
		g_pendingFadeInNotBefore = notBefore;
		g_pendingFadeInSawLoadingMenu = sawLoadingMenu;
		spdlog::info("[TFD][Transition] arm pending fadein kind={} sawLoading={} notBeforeSet={}",
			GetKindName(kind),
			sawLoadingMenu ? 1 : 0,
			g_pendingFadeIn ? 1 : 0);
	}

	bool HasPendingFadeIn()
	{
		return g_pendingFadeIn && g_pendingFadeInKind != Kind::None;
	}

	void ProcessPendingFadeIn()
	{
		if (!HasPendingFadeIn()) {
			return;
		}

		auto* ui = RE::UI::GetSingleton();
		const bool loadingOpen = ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
		if (loadingOpen) {
			g_pendingFadeInSawLoadingMenu = true;
			return;
		}

		if (Clock::now() < g_pendingFadeInNotBefore) {
			return;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->GetParentCell() : nullptr;
		if (!player || !cell) {
			return;
		}

		const auto kind = g_pendingFadeInKind;
		const bool sawLoading = g_pendingFadeInSawLoadingMenu;
		ClearPendingFadeIn();

		spdlog::info("[TFD][Transition] fadein ready kind={} cell={:08X} path={}",
			GetKindName(kind),
			cell->GetFormID(),
			sawLoading ? "after_loading_close" : "after_settle");

		if (!QueueRequest(kind, true, sawLoading ? "fadein_after_loading_close" : "fadein_after_settle")) {
			HideBlackoutFader();
		}
	}

	bool IsAwaiting()
	{
		return false;
	}

	void PollResult()
	{
		// Foundation only.
		// Legacy TFDTransition* globals have already been removed from the ESP side,
		// so there is no external transition result channel to poll yet.
	}

	bool BeginImmediate(Kind kind, const char* reason, const std::function<bool(const char*)>& completeNow)
	{
		ClearPendingFadeIn();
		if (QueueRequest(kind, false, reason)) {
			return true;
		}
		ShowBlackoutFader();
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		const bool ok = completeNow ? completeNow(reason) : false;
		std::this_thread::sleep_for(std::chrono::milliseconds(400));
		HideBlackoutFader();
		return ok;
	}

	void BeginImmediateVoid(Kind kind, const char* reason, const std::function<void(const char*)>& completeNow)
	{
		ClearPendingFadeIn();
		if (QueueRequest(kind, false, reason)) {
			return;
		}
		ShowBlackoutFader();
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		if (completeNow) {
			completeNow(reason);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(400));
		HideBlackoutFader();
	}

	void ShowBlackoutFader()
	{
		auto* queue = RE::UIMessageQueue::GetSingleton();
		auto* strings = RE::InterfaceStrings::GetSingleton();
		if (!queue || !strings) {
			return;
		}
		queue->AddMessage(strings->faderMenu, RE::UI_MESSAGE_TYPE::kShow, nullptr);
		queue->ProcessCommands();
	}

	void HideBlackoutFader()
	{
		auto* queue = RE::UIMessageQueue::GetSingleton();
		auto* strings = RE::InterfaceStrings::GetSingleton();
		if (!queue || !strings) {
			return;
		}
		queue->AddMessage(strings->faderMenu, RE::UI_MESSAGE_TYPE::kHide, nullptr);
		queue->ProcessCommands();
	}
}
