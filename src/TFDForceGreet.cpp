#include <RE/Skyrim.h>
#include "TFDForceGreet.h"

#include <chrono>
#include <mutex>

#include <spdlog/spdlog.h>

namespace TFD::ForceGreet
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr auto kInitialDelay = std::chrono::milliseconds(90);
		constexpr auto kRetryDelay = std::chrono::milliseconds(180);
		constexpr auto kPackageRefreshDelay = std::chrono::milliseconds(350);
		constexpr auto kHardResetDelay = std::chrono::milliseconds(650);
		constexpr auto kDefaultTimeout = std::chrono::milliseconds(1500);
		constexpr auto kBleedoutTimeout = std::chrono::milliseconds(4000);
		constexpr auto kCommitQuietWindow = std::chrono::milliseconds(900);

		struct PendingState
		{
			std::mutex lock{};
			RE::ActorHandle speaker{};
			Mode mode = Mode::None;
			bool active = false;
			bool succeeded = false;
			bool requestIssued = false;
			std::uint32_t attempts = 0;
			Clock::time_point started{};
			Clock::time_point nextAttempt{};
			Clock::time_point deadline{};
			Clock::time_point quietUntil{};
			Clock::time_point lastPackageRefresh{};
			Clock::time_point lastHardReset{};
		};

		PendingState g_pending{};
		RE::TESGlobal* g_dialogueStateGlobal = nullptr;
		bool g_loggedDialogueStateMissing = false;

		const char* ModeName(Mode mode)
		{
			switch (mode) {
			case Mode::Bleedout:
				return "Bleedout";
			case Mode::CaptiveMarker:
				return "CaptiveMarker";
			case Mode::InCombatTruce:
				return "InCombatTruce";
			case Mode::PreCombatTruce:
				return "PreCombatTruce";
			case Mode::AfterPleasure:
				return "AfterPleasure";
			default:
				return "None";
			}
		}

		void ResolveDialogueStateGlobal()
		{
			if (g_dialogueStateGlobal) {
				return;
			}
			g_dialogueStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDDialogueState");
			if (!g_dialogueStateGlobal && !g_loggedDialogueStateMissing) {
				g_loggedDialogueStateMissing = true;
				spdlog::warn("[TFD][ForceGreet] global TFDDialogueState not found");
			}
		}

		bool IsDialogueOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
		}

		void SetDialogueStateValue(int value)
		{
			ResolveDialogueStateGlobal();
			if (!g_dialogueStateGlobal) {
				return;
			}
			const float desired = static_cast<float>(value);
			if (g_dialogueStateGlobal->value != desired) {
				g_dialogueStateGlobal->value = desired;
			}
		}

		void SyncDialogueStateLocked(bool dialogueOpen)
		{
			SetDialogueStateValue((g_pending.active || dialogueOpen) ? 1 : 0);
		}

		bool CanAttemptOpen(RE::PlayerCharacter* player, RE::Actor* speaker)
		{
			return player && speaker && speaker != player && !speaker->IsDead() && !speaker->IsDisabled();
		}

		std::uint32_t PendingSpeakerFormID()
		{
			auto sp = RE::Actor::LookupByHandle(g_pending.speaker.native_handle());
			auto* actor = sp.get();
			return actor ? actor->GetFormID() : 0u;
		}

		void ResetLocked()
		{
			g_pending.speaker = {};
			g_pending.mode = Mode::None;
			g_pending.active = false;
			g_pending.succeeded = false;
			g_pending.requestIssued = false;
			g_pending.attempts = 0;
			g_pending.started = {};
			g_pending.nextAttempt = {};
			g_pending.deadline = {};
			g_pending.quietUntil = {};
			g_pending.lastPackageRefresh = {};
			g_pending.lastHardReset = {};
		}

		void CancelLocked(const char* reason)
		{
			if (g_pending.active || g_pending.mode != Mode::None) {
				spdlog::info(
					"[TFD][ForceGreet] cancel mode={} reason={} speaker={:08X} attempts={} requestIssued={} succeeded={}",
					ModeName(g_pending.mode),
					reason ? reason : "unknown",
					PendingSpeakerFormID(),
					g_pending.attempts,
					g_pending.requestIssued ? 1 : 0,
					g_pending.succeeded ? 1 : 0);
			}
			ResetLocked();
			SyncDialogueStateLocked(IsDialogueOpen());
		}

		void PrepareSpeakerForDialogue(RE::PlayerCharacter* player, RE::Actor* speaker, bool hardReset)
		{
			if (!player || !speaker) {
				return;
			}

			if (!speaker->IsAIEnabled()) {
				speaker->EnableAI(true);
			}

			speaker->AllowPCDialogue(true);

			if (hardReset) {
				speaker->SetDialogueWithPlayer(false, false, nullptr);
			}

			speaker->EvaluatePackage(false, true);
			speaker->EvaluatePackage(true, true);
		}

		void BeginCommon(RE::Actor* speaker, Mode mode, const char* reason)
		{
			std::scoped_lock lk(g_pending.lock);
			ResetLocked();

			if (!speaker || speaker->IsDead() || speaker->IsDisabled()) {
				SyncDialogueStateLocked(IsDialogueOpen());
				spdlog::warn(
					"[TFD][ForceGreet] begin rejected mode={} reason={} speaker={:08X}",
					ModeName(mode),
					reason ? reason : "unknown",
					speaker ? speaker->GetFormID() : 0u);
				return;
			}

			const auto now = Clock::now();
			const auto timeout = mode == Mode::Bleedout ? kBleedoutTimeout : (mode == Mode::AfterPleasure ? std::chrono::milliseconds(2500) : kDefaultTimeout);
			g_pending.speaker = speaker->GetHandle();
			g_pending.mode = mode;
			g_pending.active = true;
			g_pending.succeeded = false;
			g_pending.requestIssued = false;
			g_pending.attempts = 0;
			g_pending.started = now;
			g_pending.nextAttempt = now + kInitialDelay;
			g_pending.deadline = now + timeout;
			g_pending.quietUntil = {};
			g_pending.lastPackageRefresh = {};
			g_pending.lastHardReset = {};
			SyncDialogueStateLocked(IsDialogueOpen());

			spdlog::info(
				"[TFD][ForceGreet] begin mode={} reason={} speaker={:08X} delayMs={} timeoutMs={}",
				ModeName(mode),
				reason ? reason : "unknown",
				speaker->GetFormID(),
				static_cast<int>(kInitialDelay.count()),
				static_cast<int>(timeout.count()));
		}
	}

	void Install()
	{
		std::scoped_lock lk(g_pending.lock);
		ResetLocked();
		ResolveDialogueStateGlobal();
		SyncDialogueStateLocked(IsDialogueOpen());
		spdlog::info("[TFD][ForceGreet] Install active (native open pending)");
	}

	void BeginBleedout(RE::Actor* speaker)
	{
		BeginCommon(speaker, Mode::Bleedout, "bleedout");
	}

	void BeginCaptiveMarker(RE::Actor* speaker)
	{
		BeginCommon(speaker, Mode::CaptiveMarker, "captive_marker");
	}

	void BeginInCombatTruce(RE::Actor* speaker)
	{
		BeginCommon(speaker, Mode::InCombatTruce, "incombat_truce");
	}

	void BeginPreCombatTruce(RE::Actor* speaker)
	{
		BeginCommon(speaker, Mode::PreCombatTruce, "precombat_truce");
	}

	void BeginAfterPleasure(RE::Actor* speaker)
	{
		BeginCommon(speaker, Mode::AfterPleasure, "after_pleasure");
	}

	void Tick()
	{
		std::scoped_lock lk(g_pending.lock);
		const bool dialogueOpen = IsDialogueOpen();
		SyncDialogueStateLocked(dialogueOpen);
		if (!g_pending.active) {
			return;
		}

		if (dialogueOpen) {
			const auto completedMode = g_pending.mode;
			g_pending.succeeded = true;
			g_pending.active = false;
			g_pending.mode = Mode::None;
			SyncDialogueStateLocked(true);
			spdlog::info(
				"[TFD][ForceGreet] success mode={} speaker={:08X} attempts={} requestIssued={}",
				ModeName(completedMode),
				PendingSpeakerFormID(),
				g_pending.attempts,
				g_pending.requestIssued ? 1 : 0);
			return;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		auto speakerSp = RE::Actor::LookupByHandle(g_pending.speaker.native_handle());
		auto* speaker = speakerSp.get();
		const auto now = Clock::now();

		if (!CanAttemptOpen(player, speaker)) {
			CancelLocked("invalid_target");
			return;
		}

		if (now >= g_pending.deadline) {
			spdlog::warn(
				"[TFD][ForceGreet] timeout mode={} speaker={:08X} attempts={} requestIssued={}",
				ModeName(g_pending.mode),
				speaker->GetFormID(),
				g_pending.attempts,
				g_pending.requestIssued ? 1 : 0);
			ResetLocked();
			SyncDialogueStateLocked(IsDialogueOpen());
			return;
		}

		if (g_pending.requestIssued && g_pending.quietUntil.time_since_epoch().count() != 0 && now < g_pending.quietUntil) {
			return;
		}

		const bool shouldHardReset =
			!g_pending.requestIssued &&
			(g_pending.lastHardReset.time_since_epoch().count() == 0 ||
			 g_pending.attempts == 0 ||
			 (now - g_pending.lastHardReset) >= kHardResetDelay);

		if (shouldHardReset) {
			PrepareSpeakerForDialogue(player, speaker, true);
			g_pending.lastHardReset = now;
			spdlog::info(
				"[TFD][ForceGreet] handshake reset mode={} speaker={:08X} attempts={} requestIssued={}",
				ModeName(g_pending.mode),
				speaker->GetFormID(),
				g_pending.attempts,
				g_pending.requestIssued ? 1 : 0);
		}
		else if (!g_pending.requestIssued &&
			(g_pending.lastPackageRefresh.time_since_epoch().count() == 0 ||
			 (now - g_pending.lastPackageRefresh) >= kPackageRefreshDelay)) {
			PrepareSpeakerForDialogue(player, speaker, false);
			g_pending.lastPackageRefresh = now;
		}

		if (now < g_pending.nextAttempt) {
			return;
		}

		const bool ok = speaker->SetDialogueWithPlayer(true, false, nullptr);
		++g_pending.attempts;
		const bool firstIssued = ok && !g_pending.requestIssued;
		g_pending.requestIssued = g_pending.requestIssued || ok;
		if (g_pending.requestIssued) {
			g_pending.quietUntil = now + kCommitQuietWindow;
			g_pending.nextAttempt = g_pending.quietUntil;
		} else {
			g_pending.nextAttempt = now + kRetryDelay;
		}
		SyncDialogueStateLocked(IsDialogueOpen());

		spdlog::info(
			"[TFD][ForceGreet] try mode={} speaker={:08X} attempt={} ok={} requestIssued={}",
			ModeName(g_pending.mode),
			speaker->GetFormID(),
			g_pending.attempts,
			ok ? 1 : 0,
			g_pending.requestIssued ? 1 : 0);

		if (firstIssued) {
			spdlog::info(
				"[TFD][ForceGreet] quiet window mode={} speaker={:08X} holdMs={} attempt={}",
				ModeName(g_pending.mode),
				speaker->GetFormID(),
				static_cast<int>(kCommitQuietWindow.count()),
				g_pending.attempts);
		}
	}

	void Cancel()
	{
		std::scoped_lock lk(g_pending.lock);
		CancelLocked("api_cancel");
	}

	bool IsActive()
	{
		std::scoped_lock lk(g_pending.lock);
		return g_pending.active;
	}

	bool DidSucceed()
	{
		std::scoped_lock lk(g_pending.lock);
		const bool result = g_pending.succeeded;
		g_pending.succeeded = false;
		return result;
	}

	Mode GetMode()
	{
		std::scoped_lock lk(g_pending.lock);
		return g_pending.active ? g_pending.mode : Mode::None;
	}
}
