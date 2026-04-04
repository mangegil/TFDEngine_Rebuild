#include <RE/Skyrim.h>
#include "TFDForceGreet.h"

#include <chrono>
#include <cmath>
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
		constexpr float kDefaultOpenDistance = 192.0f;
		constexpr float kBleedoutOpenDistance = 256.0f;

		struct PendingState
		{
			std::mutex lock{};
			RE::ActorHandle speaker{};
			Mode mode = Mode::None;
			bool active = false;
			bool succeeded = false;
			bool requestIssued = false;
			std::uint32_t attempts = 0;
			float maxDistance = kDefaultOpenDistance;
			Clock::time_point started{};
			Clock::time_point nextAttempt{};
			Clock::time_point deadline{};
			Clock::time_point lastPackageRefresh{};
			Clock::time_point lastHardReset{};
		};

		PendingState g_pending{};

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
			default:
				return "None";
			}
		}

		bool IsDialogueOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
		}

		float Distance3D(const RE::NiPoint3& a, const RE::NiPoint3& b)
		{
			const auto dx = a.x - b.x;
			const auto dy = a.y - b.y;
			const auto dz = a.z - b.z;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		}

		bool IsSpaceCompatible(RE::Actor* speaker, RE::PlayerCharacter* player)
		{
			if (!speaker || !player) {
				return false;
			}

			auto* speakerCell = speaker->GetParentCell();
			auto* playerCell = player->GetParentCell();
			if (!speakerCell || !playerCell) {
				return false;
			}

			const bool speakerInterior = speakerCell->IsInteriorCell();
			const bool playerInterior = playerCell->IsInteriorCell();
			if (speakerInterior != playerInterior) {
				return false;
			}

			if (playerInterior) {
				return speakerCell == playerCell;
			}

			auto* speakerWs = speaker->GetWorldspace();
			auto* playerWs = player->GetWorldspace();
			return speakerWs && playerWs && speakerWs == playerWs;
		}

		bool CanAttemptOpen(RE::PlayerCharacter* player, RE::Actor* speaker, float maxDistance, float* outDistance)
		{
			if (outDistance) {
				*outDistance = 99999.0f;
			}
			if (!player || !speaker || speaker == player) {
				return false;
			}
			if (speaker->IsDead() || speaker->IsDisabled() || !speaker->Is3DLoaded()) {
				return false;
			}
			if (!IsSpaceCompatible(speaker, player)) {
				return false;
			}

			const float dist = Distance3D(speaker->GetPosition(), player->GetPosition());
			if (outDistance) {
				*outDistance = dist;
			}
			return dist <= maxDistance;
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
			g_pending.maxDistance = kDefaultOpenDistance;
			g_pending.started = {};
			g_pending.nextAttempt = {};
			g_pending.deadline = {};
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

			if (speaker->IsInCombat()) {
				speaker->StopCombat();
			}
			if (auto* process = RE::ProcessLists::GetSingleton()) {
				process->StopCombatAndAlarmOnActor(speaker, false);
			}

			if (player->IsWeaponDrawn()) {
				player->DrawWeaponMagicHands(false);
			}
			if (speaker->IsWeaponDrawn()) {
				speaker->DrawWeaponMagicHands(false);
			}

			player->EvaluatePackage(false, true);
			player->EvaluatePackage(true, true);
			speaker->EvaluatePackage(false, true);
			speaker->EvaluatePackage(true, true);
		}

		void BeginCommon(RE::Actor* speaker, Mode mode, const char* reason)
		{
			std::scoped_lock lk(g_pending.lock);
			ResetLocked();

			if (!speaker || speaker->IsDead() || speaker->IsDisabled()) {
				spdlog::warn(
					"[TFD][ForceGreet] begin rejected mode={} reason={} speaker={:08X}",
					ModeName(mode),
					reason ? reason : "unknown",
					speaker ? speaker->GetFormID() : 0u);
				return;
			}

			const auto now = Clock::now();
			const auto timeout = mode == Mode::Bleedout ? kBleedoutTimeout : kDefaultTimeout;
			g_pending.speaker = speaker->GetHandle();
			g_pending.mode = mode;
			g_pending.active = true;
			g_pending.succeeded = false;
			g_pending.requestIssued = false;
			g_pending.attempts = 0;
			g_pending.maxDistance = mode == Mode::Bleedout ? kBleedoutOpenDistance : kDefaultOpenDistance;
			g_pending.started = now;
			g_pending.nextAttempt = now + kInitialDelay;
			g_pending.deadline = now + timeout;
			g_pending.lastPackageRefresh = {};
			g_pending.lastHardReset = {};

			spdlog::info(
				"[TFD][ForceGreet] begin mode={} reason={} speaker={:08X} delayMs={} timeoutMs={} maxDist={:.1f}",
				ModeName(mode),
				reason ? reason : "unknown",
				speaker->GetFormID(),
				static_cast<int>(kInitialDelay.count()),
				static_cast<int>(timeout.count()),
				g_pending.maxDistance);
		}
	}

	void Install()
	{
		std::scoped_lock lk(g_pending.lock);
		ResetLocked();
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

	void Tick()
	{
		std::scoped_lock lk(g_pending.lock);
		if (!g_pending.active) {
			return;
		}

		if (IsDialogueOpen()) {
			g_pending.succeeded = true;
			spdlog::info(
				"[TFD][ForceGreet] success mode={} speaker={:08X} attempts={} requestIssued={}",
				ModeName(g_pending.mode),
				PendingSpeakerFormID(),
				g_pending.attempts,
				g_pending.requestIssued ? 1 : 0);
			g_pending.active = false;
			g_pending.mode = Mode::None;
			return;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		auto speakerSp = RE::Actor::LookupByHandle(g_pending.speaker.native_handle());
		auto* speaker = speakerSp.get();
		const auto now = Clock::now();

		if (!player || !speaker || speaker->IsDead() || speaker->IsDisabled()) {
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
			return;
		}

		float dist = 99999.0f;
		const bool canAttempt = CanAttemptOpen(player, speaker, g_pending.maxDistance, &dist);
		if (!canAttempt) {
			if (g_pending.lastPackageRefresh.time_since_epoch().count() == 0 ||
				(now - g_pending.lastPackageRefresh) >= kPackageRefreshDelay) {
				PrepareSpeakerForDialogue(player, speaker, false);
				g_pending.lastPackageRefresh = now;
				spdlog::info(
					"[TFD][ForceGreet] wait mode={} speaker={:08X} dist={:.1f} attempts={} requestIssued={} -> refresh package",
					ModeName(g_pending.mode),
					speaker->GetFormID(),
					dist,
					g_pending.attempts,
					g_pending.requestIssued ? 1 : 0);
			}
			return;
		}

		const bool shouldHardReset =
			g_pending.lastHardReset.time_since_epoch().count() == 0 ||
			(!g_pending.requestIssued && g_pending.attempts == 0) ||
			(now - g_pending.lastHardReset) >= kHardResetDelay ||
			(g_pending.requestIssued && (g_pending.attempts >= 3) && ((g_pending.attempts % 4) == 0));

		if (shouldHardReset) {
			PrepareSpeakerForDialogue(player, speaker, true);
			g_pending.lastHardReset = now;
			spdlog::info(
				"[TFD][ForceGreet] handshake reset mode={} speaker={:08X} dist={:.1f} attempts={} requestIssued={}",
				ModeName(g_pending.mode),
				speaker->GetFormID(),
				dist,
				g_pending.attempts,
				g_pending.requestIssued ? 1 : 0);
		}
		else if (g_pending.lastPackageRefresh.time_since_epoch().count() == 0 ||
			(now - g_pending.lastPackageRefresh) >= kPackageRefreshDelay) {
			PrepareSpeakerForDialogue(player, speaker, false);
			g_pending.lastPackageRefresh = now;
		}

		if (now < g_pending.nextAttempt) {
			return;
		}

		const bool ok = speaker->SetDialogueWithPlayer(true, false, nullptr);
		++g_pending.attempts;
		g_pending.requestIssued = g_pending.requestIssued || ok;
		g_pending.nextAttempt = now + kRetryDelay;

		spdlog::info(
			"[TFD][ForceGreet] try mode={} speaker={:08X} dist={:.1f} attempt={} ok={} requestIssued={}",
			ModeName(g_pending.mode),
			speaker->GetFormID(),
			dist,
			g_pending.attempts,
			ok ? 1 : 0,
			g_pending.requestIssued ? 1 : 0);
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
		return g_pending.succeeded;
	}

	Mode GetMode()
	{
		std::scoped_lock lk(g_pending.lock);
		return g_pending.active ? g_pending.mode : Mode::None;
	}
}
