#include "TFDRescueGreet.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include "TFDDefeatBridge.h"
#include "TFDForceGreetState.h"
#include "TFDInteractionRouter.h"
#include "TFDRescue.h"

namespace TFD::RescueGreet
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		constexpr RE::FormID kRescueGreetInfoLocalFormID = 0x0006FEB0;
		constexpr RE::FormID kSaviorFactionLocalFormID = 0x0006FEB1;
		constexpr std::string_view kPluginName{ "TFDEngine.esp" };
		constexpr float kRescueHardOpenMaxDistance = 180.0f;
		constexpr float kRescueHardOpenPlacementDistance = 96.0f;
		constexpr int kRescueHardOpenMaxAttempts = 12;
		constexpr int kRescueHardOpenRetryDelayMs = 220;
		constexpr int kRescueDialogueCloseRetryDelayMs = 650;
		constexpr int kRescuePostLoadSettleDelayMs = 1400;
		constexpr int kRescueFallbackWorldReadyDelayMs = 2200;

		struct RuntimeState
		{
			std::mutex lock{};
			bool sawDialogue = false;
			bool bridgeAssigned = false;
			bool worldReadySeen = false;
			bool exhausted = false;
			int attempts = 0;
			Clock::time_point armedAt{};
			Clock::time_point worldReadyAt{};
			std::string source{};
			std::string reason{};
		};

		std::atomic_bool g_installed{ false };
		std::atomic<State> g_state{ State::Idle };
		std::atomic<std::uint32_t> g_speakerFormID{ 0 };
		std::atomic<std::uint32_t> g_tokenActorFormID{ 0 };
		RuntimeState g_runtime{};

		void ResetRuntimeLocked()
		{
			g_runtime.sawDialogue = false;
			g_runtime.bridgeAssigned = false;
			g_runtime.worldReadySeen = false;
			g_runtime.exhausted = false;
			g_runtime.attempts = 0;
			g_runtime.armedAt = {};
			g_runtime.worldReadyAt = {};
			g_runtime.source.clear();
			g_runtime.reason.clear();
		}

		bool IsDialogueMenuOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
		}

		bool IsRescueCommitted()
		{
			return TFD::ForceGreetState::IsRescueCommitted();
		}

		void ClearSaviorFactionToken(const char* reason);

		const char* StateName(State state)
		{
			switch (state) {
			case State::PostTeleportPending:
				return "PostTeleportPending";
			case State::Armed:
				return "Armed";
			case State::Running:
				return "Running";
			default:
				return "Idle";
			}
		}

		void StopNativeRetryAfterCommitted(const char* reason)
		{
			const auto formID = g_speakerFormID.exchange(0, std::memory_order_acq_rel);
			const auto state = g_state.exchange(State::Idle, std::memory_order_acq_rel);
			ClearSaviorFactionToken(reason ? reason : "rescue_committed_stop_retry");
			ResetRuntime(reason ? reason : "rescue_committed_stop_retry");

			if (state != State::Idle || formID != 0) {
				spdlog::info(
					"[TFD][RescueGreet][P32X] native retry stopped after committed speaker={:08X} state={} reason={}",
					formID,
					StateName(state),
					reason ? reason : "rescue_committed_stop_retry");
			}
		}

		bool IsLoadingOrMainMenuOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && (ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) || ui->IsMenuOpen(RE::MainMenu::MENU_NAME));
		}

		bool IsWorldReady()
		{
			if (IsLoadingOrMainMenuOpen()) {
				return false;
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			return player && player->GetParentCell();
		}

		bool IsPostTeleportOpenSettleReady(int& outDelayMs)
		{
			outDelayMs = 0;

			if (g_state.load(std::memory_order_acquire) != State::PostTeleportPending) {
				return true;
			}

			const auto now = Clock::now();
			std::chrono::milliseconds remaining{ 0 };
			{
				std::scoped_lock lk(g_runtime.lock);
				if (g_runtime.worldReadySeen && g_runtime.worldReadyAt.time_since_epoch().count() != 0) {
					const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_runtime.worldReadyAt);
					remaining = std::chrono::milliseconds(kRescuePostLoadSettleDelayMs) - elapsed;
				}
				else if (g_runtime.armedAt.time_since_epoch().count() != 0) {
					const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_runtime.armedAt);
					remaining = std::chrono::milliseconds(kRescueFallbackWorldReadyDelayMs) - elapsed;
				}
				else {
					remaining = std::chrono::milliseconds(kRescuePostLoadSettleDelayMs);
				}
			}

			if (remaining.count() > 0) {
				outDelayMs = static_cast<int>(std::clamp<long long>(
					remaining.count(),
					static_cast<long long>(kRescueHardOpenRetryDelayMs),
					static_cast<long long>(kRescueFallbackWorldReadyDelayMs)));
				return false;
			}

			return true;
		}

		bool IsRetryExhausted()
		{
			std::scoped_lock lk(g_runtime.lock);
			return g_runtime.exhausted;
		}

		RE::Actor* ResolveSpeakerByStoredFormID()
		{
			const auto formID = g_speakerFormID.load(std::memory_order_acquire);
			if (!formID) {
				return nullptr;
			}
			return RE::TESForm::LookupByID<RE::Actor>(formID);
		}

		RE::TESTopicInfo* ResolveRescueGreetTopicInfo()
		{
			static RE::TESTopicInfo* info = nullptr;
			static bool attempted = false;

			if (!attempted) {
				attempted = true;
				if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
					info = dataHandler->LookupForm<RE::TESTopicInfo>(kRescueGreetInfoLocalFormID, kPluginName);
				}

				if (info) {
					spdlog::info(
						"[TFD][RescueGreet][R36D] TFDDialogueRescueGreet INFO resolved {:08X} local={:06X}",
						info->GetFormID(),
						kRescueGreetInfoLocalFormID);
				}
				else {
					spdlog::warn(
						"[TFD][RescueGreet][R36D] TFDDialogueRescueGreet INFO {:06X} not found in {}; rescue hard-open will fall back to default topic selection",
						kRescueGreetInfoLocalFormID,
						kPluginName);
				}
			}

			return info;
		}

		RE::TESFaction* ResolveSaviorFaction()
		{
			static RE::TESFaction* faction = nullptr;
			static bool attempted = false;

			if (!attempted) {
				attempted = true;
				faction = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDSaviorFaction");
				if (!faction) {
					if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
						faction = dataHandler->LookupForm<RE::TESFaction>(kSaviorFactionLocalFormID, kPluginName);
					}
				}

				if (faction) {
					spdlog::info(
						"[TFD][RescueGreet][R36D] TFDSaviorFaction resolved {:08X} local={:06X}",
						faction->GetFormID(),
						kSaviorFactionLocalFormID);
				}
				else {
					spdlog::warn(
						"[TFD][RescueGreet][R36D] TFDSaviorFaction not found local={:06X}; Rescue CK faction condition will not pass unless Papyrus property adds it",
						kSaviorFactionLocalFormID);
				}
			}

			return faction;
		}

		void ClearSaviorFactionToken(const char* reason)
		{
			auto* faction = ResolveSaviorFaction();
			const auto oldFormID = g_tokenActorFormID.exchange(0, std::memory_order_acq_rel);
			if (!faction || !oldFormID) {
				return;
			}

			auto* oldActor = RE::TESForm::LookupByID<RE::Actor>(oldFormID);
			if (oldActor && oldActor->IsInFaction(faction)) {
				oldActor->RemoveFromFaction(faction);
				spdlog::info(
					"[TFD][RescueGreet][R36D] TFDSaviorFaction removed actor={:08X} reason={}",
					oldActor->GetFormID(),
					reason ? reason : "clear_savior_token");
			}
		}

		void ApplyExclusiveSaviorFactionToken(RE::Actor* speaker, const char* reason)
		{
			auto* faction = ResolveSaviorFaction();
			if (!faction || !speaker) {
				return;
			}

			const auto newFormID = speaker->GetFormID();
			const auto oldFormID = g_tokenActorFormID.load(std::memory_order_acquire);
			if (oldFormID && oldFormID != newFormID) {
				auto* oldActor = RE::TESForm::LookupByID<RE::Actor>(oldFormID);
				if (oldActor && oldActor->IsInFaction(faction)) {
					oldActor->RemoveFromFaction(faction);
					spdlog::info(
						"[TFD][RescueGreet][R36D] TFDSaviorFaction removed old actor={:08X} new={:08X} reason={}",
						oldActor->GetFormID(),
						newFormID,
						reason ? reason : "apply_savior_token");
				}
			}

			if (!speaker->IsInFaction(faction)) {
				speaker->AddToFaction(faction, 0);
				spdlog::info(
					"[TFD][RescueGreet][R36D] TFDSaviorFaction added actor={:08X} reason={}",
					speaker->GetFormID(),
					reason ? reason : "apply_savior_token");
			}

			g_tokenActorFormID.store(newFormID, std::memory_order_release);
		}

		float DistanceSquared(RE::TESObjectREFR* a, RE::TESObjectREFR* b)
		{
			if (!a || !b) {
				return std::numeric_limits<float>::max();
			}

			const auto ap = a->GetPosition();
			const auto bp = b->GetPosition();
			const float dx = ap.x - bp.x;
			const float dy = ap.y - bp.y;
			const float dz = ap.z - bp.z;
			return dx * dx + dy * dy + dz * dz;
		}

		bool IsValidSavior(RE::Actor* speaker)
		{
			return speaker && !speaker->IsDead() && !speaker->IsDisabled();
		}

		bool PrepareSaviorForOwnedHardOpen(RE::Actor* speaker, const char* reason)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || !IsValidSavior(speaker) || speaker == player) {
				spdlog::warn(
					"[TFD][RescueGreet][R36D] prepare rejected speaker={:08X} reason={} invalid=1 player={:08X}",
					speaker ? speaker->GetFormID() : 0u,
					reason ? reason : "rescue_hard_open",
					player ? player->GetFormID() : 0u);
				return false;
			}

			if (!speaker->IsAIEnabled()) {
				speaker->EnableAI(true);
			}
			speaker->AllowPCDialogue(true);
			if (speaker->IsInCombat()) {
				speaker->StopCombat();
			}
			if (auto* process = RE::ProcessLists::GetSingleton()) {
				process->StopCombatAndAlarmOnActor(speaker, false);
			}
			if (speaker->IsWeaponDrawn()) {
				speaker->DrawWeaponMagicHands(false);
			}

			const bool differentCell = speaker->GetParentCell() && player->GetParentCell() && speaker->GetParentCell() != player->GetParentCell();
			const float beforeDistSq = DistanceSquared(speaker, player);
			bool moved = false;

			if (differentCell || beforeDistSq > kRescueHardOpenMaxDistance * kRescueHardOpenMaxDistance) {
				RE::NiPoint3 pos = player->GetPosition();
				const float yaw = player->GetAngleZ();
				pos.x += static_cast<float>(std::sin(yaw) * kRescueHardOpenPlacementDistance);
				pos.y += static_cast<float>(std::cos(yaw) * kRescueHardOpenPlacementDistance);

				speaker->MoveTo(player);
				speaker->SetPosition(pos, true);
				moved = true;
			}

			speaker->EvaluatePackage(false, true);

			const float afterDistSq = DistanceSquared(speaker, player);
			spdlog::info(
				"[TFD][RescueGreet][R36D] post-load hard-open prep speaker={:08X} moved={} differentCell={} beforeDist={:.1f} afterDist={:.1f} max={:.1f} reason={}",
				speaker->GetFormID(),
				moved ? 1 : 0,
				differentCell ? 1 : 0,
				std::sqrt(beforeDistSq),
				std::sqrt(afterDistSq),
				kRescueHardOpenMaxDistance,
				reason ? reason : "rescue_hard_open");

			return true;
		}

		void QueueRetryTask(const char* reason, int delayMs = 0)
		{
			std::string reasonCopy = reason ? reason : "rescue_retry";
			auto schedule = [reasonCopy]() {
				if (auto* tasks = SKSE::GetTaskInterface()) {
					tasks->AddTask([reasonCopy]() {
						(void)RetryPendingHardOpen(reasonCopy.c_str());
					});
				}
				else {
					spdlog::warn("[TFD][RescueGreet][R36D] retry task dropped reason={} cause=no_task_interface", reasonCopy);
				}
			};

			if (delayMs <= 0) {
				schedule();
				return;
			}

			std::thread([schedule, delayMs]() mutable {
				std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
				schedule();
			}).detach();
		}

		bool TryPostTeleportHardOpen(RE::Actor* speaker, const char* reason)
		{
			if (IsRescueCommitted()) {
				StopNativeRetryAfterCommitted(reason ? reason : "hard_open_committed_guard");
				return false;
			}

			if (!speaker || !IsValidSavior(speaker)) {
				spdlog::warn(
					"[TFD][RescueGreet][R36D] post-load hard-open rejected invalid speaker={:08X} reason={}",
					speaker ? speaker->GetFormID() : 0u,
					reason ? reason : "rescue_hard_open");
				return false;
			}

			if (!OwnsCurrentFlow()) {
				spdlog::warn(
					"[TFD][RescueGreet][R36D] post-load hard-open held flowOwnerMismatch speaker={:08X} reason={}",
					speaker->GetFormID(),
					reason ? reason : "rescue_hard_open");
				return false;
			}

			if (!IsWorldReady()) {
				spdlog::info(
					"[TFD][RescueGreet][R36D] post-load hard-open held worldNotReady speaker={:08X} reason={}",
					speaker->GetFormID(),
					reason ? reason : "rescue_hard_open");
				QueueRetryTask("world_not_ready_retry_r36d", kRescueHardOpenRetryDelayMs);
				return false;
			}

			int postLoadSettleDelayMs = 0;
			if (!IsPostTeleportOpenSettleReady(postLoadSettleDelayMs)) {
				spdlog::info(
					"[TFD][RescueGreet][P33H] post-load hard-open held for world settle speaker={:08X} delayMs={} reason={}",
					speaker->GetFormID(),
					postLoadSettleDelayMs,
					reason ? reason : "rescue_hard_open");
				QueueRetryTask("post_load_world_settle_retry_p33h", postLoadSettleDelayMs);
				return false;
			}

			if (IsDialogueMenuOpen() && g_state.load(std::memory_order_acquire) == State::Running) {
				NotifyDialogueOpened();
				spdlog::info(
					"[TFD][RescueGreet][R36D] retry skipped because DialogueMenu is already open speaker={:08X} reason={}",
					speaker->GetFormID(),
					reason ? reason : "rescue_hard_open");
				return true;
			}

			int attempt = 0;
			bool shouldAssignBridge = false;
			{
				std::scoped_lock lk(g_runtime.lock);
				attempt = ++g_runtime.attempts;
				if (!g_runtime.bridgeAssigned) {
					g_runtime.bridgeAssigned = true;
					shouldAssignBridge = true;
				}
			}

			if (attempt > kRescueHardOpenMaxAttempts) {
				{
					std::scoped_lock lk(g_runtime.lock);
					g_runtime.exhausted = true;
				}
				spdlog::warn(
					"[TFD][RescueGreet][R36D] post-load hard-open attempt limit reached speaker={:08X} attempts={} reason={} action=release_activation_block_but_keep_savior_alias",
					speaker->GetFormID(),
					attempt - 1,
					reason ? reason : "rescue_hard_open");
				return false;
			}

			if (shouldAssignBridge) {
				TFD::DefeatBridge::AssignPlayerSavior(speaker);
				spdlog::info(
					"[TFD][RescueGreet][R36D] world ready -> assign Savior speaker={:08X} reason={}",
					speaker->GetFormID(),
					reason ? reason : "rescue_hard_open");
			}

			if (IsRescueCommitted()) {
				StopNativeRetryAfterCommitted(reason ? reason : "hard_open_late_committed_guard");
				return false;
			}

			ApplyExclusiveSaviorFactionToken(speaker, reason ? reason : "rescue_hard_open");

			const bool openedArmed = TFD::ForceGreetState::SetRescueOpened();
			if (g_state.load(std::memory_order_acquire) == State::PostTeleportPending) {
				g_state.store(State::Armed, std::memory_order_release);
			}

			RE::TESTopicInfo* topicInfo = ResolveRescueGreetTopicInfo();
			if (!PrepareSaviorForOwnedHardOpen(speaker, reason)) {
				return false;
			}

			speaker->SetDialogueWithPlayer(false, false, nullptr);
			const bool ok = speaker->SetDialogueWithPlayer(true, true, topicInfo);
			const bool menuOpen = IsDialogueMenuOpen();

			spdlog::info(
				"[TFD][RescueGreet][R36D] try post-load hard-open speaker={:08X} attempt={} ok={} menuOpen={} force=1 topicInfo={:08X} explicit={} fgStateArmed={} reason={}",
				speaker->GetFormID(),
				attempt,
				ok ? 1 : 0,
				menuOpen ? 1 : 0,
				topicInfo ? topicInfo->GetFormID() : 0u,
				topicInfo ? 1 : 0,
				openedArmed ? 1 : 0,
				reason ? reason : "rescue_hard_open");

			if (menuOpen) {
				NotifyDialogueOpened();
				return true;
			}

			if (attempt < kRescueHardOpenMaxAttempts) {
				QueueRetryTask("post_load_menu_not_open_retry_r36d", kRescueHardOpenRetryDelayMs);
			}
			else {
				std::scoped_lock lk(g_runtime.lock);
				g_runtime.exhausted = true;
			}

			return ok;
		}
	}

	bool OwnsCurrentFlow()
	{
		return TFD::Rescue::IsActive();
	}

	void Install()
	{
		if (g_installed.exchange(true, std::memory_order_acq_rel)) {
			return;
		}

		spdlog::info("[TFD][RescueGreet][R36D] Install post-teleport owned hard-open path + auto retry lock + P33H world-settle gate");
	}

	void ResetRuntime(const char* reason)
	{
		std::scoped_lock lk(g_runtime.lock);
		ResetRuntimeLocked();
		spdlog::info("[TFD][RescueGreet] runtime reset reason={}", reason ? reason : "unknown");
	}

	void Reset()
	{
		Cancel("reset");
		ResetRuntime("reset");
	}

	bool ArmPostTeleportSavior(RE::Actor* speaker, const char* source, const char* reason)
	{
		const auto formID = speaker ? speaker->GetFormID() : 0u;
		if (!OwnsCurrentFlow()) {
			spdlog::warn("[TFD][RescueGreet][R36D] arm ignored speaker={:08X} source={} reason={} flowOwnerMismatch=1", formID, source ? source : "unknown", reason ? reason : "rescue");
			return false;
		}
		if (!speaker || speaker->IsDead() || speaker->IsDisabled()) {
			spdlog::warn("[TFD][RescueGreet][R36D] arm rejected speaker={:08X} source={} reason={} invalidSpeaker=1", formID, source ? source : "unknown", reason ? reason : "rescue");
			return false;
		}

		Cancel("arm_post_teleport_replace");
		{
			std::scoped_lock lk(g_runtime.lock);
			ResetRuntimeLocked();
			g_runtime.armedAt = Clock::now();
			g_runtime.source = source ? source : "unknown";
			g_runtime.reason = reason ? reason : "rescue_post_teleport";
		}

		g_speakerFormID.store(formID, std::memory_order_release);
		g_state.store(State::PostTeleportPending, std::memory_order_release);

		spdlog::info(
			"[TFD][RescueGreet][R36D] post-teleport Savior armed speaker={:08X} source={} reason={} worldReady={} loadingOrMainMenu={}",
			formID,
			source ? source : "unknown",
			reason ? reason : "rescue_post_teleport",
			IsWorldReady() ? 1 : 0,
			IsLoadingOrMainMenuOpen() ? 1 : 0);

		if (IsWorldReady()) {
			QueueRetryTask("arm_post_teleport_world_ready", kRescueHardOpenRetryDelayMs);
		}

		return true;
	}

	bool Begin(RE::Actor* speaker, const char* reason)
	{
		return ArmPostTeleportSavior(speaker, "legacy_begin", reason ? reason : "rescue_legacy_begin");
	}

	void NotifyWorldReady(const char* reason)
	{
		const auto state = g_state.load(std::memory_order_acquire);
		if (state == State::Idle) {
			return;
		}

		{
			std::scoped_lock lk(g_runtime.lock);
			g_runtime.worldReadySeen = true;
			g_runtime.worldReadyAt = Clock::now();
		}

		spdlog::info(
			"[TFD][RescueGreet][R36D] world ready notify state={} speaker={:08X} reason={}",
			GetStateName(),
			g_speakerFormID.load(std::memory_order_acquire),
			reason ? reason : "world_ready");
		QueueRetryTask(reason ? reason : "world_ready", kRescueHardOpenRetryDelayMs);
	}

	bool RetryPendingHardOpen(const char* reason)
	{
		const auto state = g_state.load(std::memory_order_acquire);
		if (state == State::Idle) {
			return false;
		}

		if (IsRescueCommitted()) {
			StopNativeRetryAfterCommitted(reason ? reason : "retry_committed_guard");
			return false;
		}

		if (IsRetryExhausted()) {
			spdlog::warn(
				"[TFD][RescueGreet][R36D] retry skipped exhausted speaker={:08X} state={} reason={}",
				g_speakerFormID.load(std::memory_order_acquire),
				GetStateName(),
				reason ? reason : "retry");
			return false;
		}

		auto* speaker = ResolveSpeakerByStoredFormID();
		if (!speaker) {
			spdlog::warn(
				"[TFD][RescueGreet][R36D] retry failed speaker missing form={:08X} state={} reason={}",
				g_speakerFormID.load(std::memory_order_acquire),
				GetStateName(),
				reason ? reason : "retry");
			return false;
		}

		return TryPostTeleportHardOpen(speaker, reason ? reason : "retry_pending_hard_open_r36d");
	}

	void Cancel(const char* reason)
	{
		const auto state = g_state.exchange(State::Idle, std::memory_order_acq_rel);
		const auto formID = g_speakerFormID.exchange(0, std::memory_order_acq_rel);
		ClearSaviorFactionToken(reason ? reason : "cancel");
		ResetRuntime("cancel");

		if (TFD::InteractionRouter::DialogueOpen::IsActive() && TFD::InteractionRouter::DialogueOpen::GetMode() == TFD::InteractionRouter::DialogueOpen::Mode::Rescue) {
			TFD::InteractionRouter::DialogueOpen::Cancel();
		}

		if (state != State::Idle || formID != 0) {
			spdlog::info("[TFD][RescueGreet] Cancel speaker={:08X} reason={}", formID, reason ? reason : "unknown");
		}
	}

	void NotifyDialogueOpened()
	{
		bool firstSeen = false;
		{
			std::scoped_lock lk(g_runtime.lock);
			firstSeen = !g_runtime.sawDialogue;
			g_runtime.sawDialogue = true;
		}
		if (g_state.load(std::memory_order_acquire) != State::Idle) {
			g_state.store(State::Running, std::memory_order_release);
		}
		if (firstSeen) {
			spdlog::info("[TFD][RescueGreet][R36D] dialogue opened speaker={:08X}", g_speakerFormID.load(std::memory_order_acquire));
		}
	}

	void NotifyDialogueMenuStateChanged(bool opening)
	{
		const auto state = g_state.load(std::memory_order_acquire);
		if (state == State::Idle) {
			return;
		}

		if (IsRescueCommitted()) {
			StopNativeRetryAfterCommitted(opening ? "dialogue_open_event_committed_guard" : "dialogue_closed_committed_guard");
			return;
		}

		if (opening) {
			NotifyDialogueOpened();
			return;
		}

		if (!OwnsCurrentFlow()) {
			spdlog::info(
				"[TFD][RescueGreet][R36D] DialogueMenu closed -> no retry because Rescue no longer owns flow speaker={:08X} state={} action=clear_native_savior_token",
				g_speakerFormID.load(std::memory_order_acquire),
				GetStateName());
			ClearSaviorFactionToken("dialogue_closed_flow_lost");
			g_speakerFormID.store(0, std::memory_order_release);
			g_state.store(State::Idle, std::memory_order_release);
			ResetRuntime("dialogue_closed_flow_lost");
			return;
		}

		if (IsRetryExhausted()) {
			spdlog::warn(
				"[TFD][RescueGreet][R36D] DialogueMenu closed -> retry suppressed exhausted speaker={:08X} state={}",
				g_speakerFormID.load(std::memory_order_acquire),
				GetStateName());
			return;
		}

		if (state == State::Running) {
			g_state.store(State::Armed, std::memory_order_release);
		}

		spdlog::info(
			"[TFD][RescueGreet][R36D] DialogueMenu closed while Rescue pending -> auto retry armed speaker={:08X} state={} delayMs={}",
			g_speakerFormID.load(std::memory_order_acquire),
			GetStateName(),
			kRescueDialogueCloseRetryDelayMs);
		QueueRetryTask("dialogue_closed_auto_retry_r36d", kRescueDialogueCloseRetryDelayMs);
	}

	bool IsActivationBlocked()
	{
		const auto state = g_state.load(std::memory_order_acquire);
		if (state == State::Idle) {
			return false;
		}
		if (!OwnsCurrentFlow()) {
			return false;
		}
		return !IsRetryExhausted();
	}

	bool HasSeenDialogue()
	{
		std::scoped_lock lk(g_runtime.lock);
		return g_runtime.sawDialogue;
	}

	bool IsActive()
	{
		return g_state.load(std::memory_order_acquire) != State::Idle;
	}

	bool IsPendingOrActive()
	{
		const auto state = g_state.load(std::memory_order_acquire);
		return OwnsCurrentFlow() && (state == State::PostTeleportPending || state == State::Armed);
	}

	bool IsPendingForSpeaker(RE::Actor* speaker)
	{
		if (!speaker || !IsPendingOrActive()) {
			return false;
		}
		return speaker->GetFormID() == g_speakerFormID.load(std::memory_order_acquire);
	}

	State GetState()
	{
		return g_state.load(std::memory_order_acquire);
	}

	const char* GetStateName()
	{
		switch (GetState()) {
		case State::PostTeleportPending:
			return "PostTeleportPending";
		case State::Armed:
			return "Armed";
		case State::Running:
			return "Running";
		default:
			return "Idle";
		}
	}

	std::uint32_t GetSpeakerFormID()
	{
		return g_speakerFormID.load(std::memory_order_acquire);
	}
}
