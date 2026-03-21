#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cfloat>
#include <cstdint>
#include <mutex>

#include <spdlog/spdlog.h>
#include "TFDForceGreet.h"
#include "TFDSettings.h"
#include "TFDPacify.h"

namespace TFD::ForceGreet
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		struct Job
		{
			std::uint32_t speakerHandle{ 0 };
			Mode mode{ Mode::None };

			double endTimeSec{ 0.0 };
			double nextTrySec{ 0.0 };
			double pendingOpenUntilSec{ 0.0 };
			bool pendingOpen{ false };

			bool active{ false };
			bool success{ false };
		};

		std::mutex lock;
		Job job;

		std::atomic_bool installed{ false };
		std::atomic_bool inputSinkAdded{ false };
		std::atomic_bool menuSinkAdded{ false };

		Clock::time_point t0 = Clock::now();

		std::uint32_t stickyHandle{ 0 };
		Mode stickyMode{ Mode::None };
		bool stickyArmed{ false };
		double dialogueOpenedAtSec{ 0.0 };
		double suppressUntilSec{ 0.0 };

		constexpr float kCaptiveApproachDistance = 220.0f;
		constexpr float kCaptiveApproachDistanceSq = kCaptiveApproachDistance * kCaptiveApproachDistance;
		constexpr float kBleedoutApproachDistance = 220.0f;
		constexpr float kBleedoutApproachDistanceSq = kBleedoutApproachDistance * kBleedoutApproachDistance;
		constexpr float kInCombatApproachDistance = 1400.0f;
		constexpr float kInCombatApproachDistanceSq = kInCombatApproachDistance * kInCombatApproachDistance;
		constexpr double kCaptiveSuppressSeconds = 6.0;
		constexpr double kNormalSuppressSeconds = 1.5;
		constexpr double kBleedoutPendingOpenRetrySeconds = 1.20;
		constexpr double kInCombatPendingOpenRetrySeconds = 0.85;
		constexpr double kPreCombatPendingOpenRetrySeconds = 0.85;

		enum class DialogueGateFail
		{
			None = 0,
			NoPlayer,
			NoSpeaker,
			DeadSpeaker,
			NotLoaded,
			DifferentCell,
			TooFar,
			NoLOS
		};

		double NowSec()
		{
			const auto now = Clock::now();
			return std::chrono::duration<double>(now - t0).count();
		}

		bool IsDialogueOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
		}

		bool IsBlockingMenuOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return false;
			}

			if (ui->IsMenuOpen(RE::Console::MENU_NAME)) {
				return true;
			}
			if (ui->IsMenuOpen(RE::LockpickingMenu::MENU_NAME)) {
				return true;
			}
			if (ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME)) {
				return true;
			}
			if (ui->IsMenuOpen(RE::JournalMenu::MENU_NAME)) {
				return true;
			}

			return false;
		}

		RE::Actor* ResolveSpeaker(std::uint32_t handle)
		{
			if (!handle) {
				return nullptr;
			}

			auto sp = RE::Actor::LookupByHandle(handle);
			return sp.get();
		}

		bool IsSpeakerUsable(RE::Actor* speaker)
		{
			if (!speaker) {
				return false;
			}

			if (speaker->IsDead()) {
				return false;
			}

			return true;
		}

		void SendModEvent(const char* eventName, RE::Actor* sender)
		{
			if (!eventName) {
				return;
			}

			auto* src = SKSE::GetModCallbackEventSource();
			if (!src) {
				return;
			}

			SKSE::ModCallbackEvent e(eventName, "", 0.0f, sender);
			src->SendEvent(&e);
		}

		void ClearCaptiveAliases(RE::Actor* speaker, const char* reason)
		{
			SendModEvent("TFDCaptiveClearAll", speaker);

			if (speaker) {
				spdlog::info(
					"[TFD][ForceGreet] CaptiveClearAll reason={} speaker={:08X}",
					reason ? reason : "unknown",
					speaker->GetFormID());
			}
			else {
				spdlog::info(
					"[TFD][ForceGreet] CaptiveClearAll reason={} speaker=<none>",
					reason ? reason : "unknown");
			}
		}

		float DistanceSq3D(RE::Actor* a, RE::Actor* b)
		{
			if (!a || !b) {
				return FLT_MAX;
			}

			const auto pa = a->GetPosition();
			const auto pb = b->GetPosition();

			const float dx = pa.x - pb.x;
			const float dy = pa.y - pb.y;
			const float dz = pa.z - pb.z;

			return (dx * dx) + (dy * dy) + (dz * dz);
		}

		DialogueGateFail CanStartDialogueNow(RE::Actor* speaker, Mode mode, float& outDist)
		{
			outDist = 99999.0f;

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return DialogueGateFail::NoPlayer;
			}
			if (!speaker) {
				return DialogueGateFail::NoSpeaker;
			}
			if (speaker->IsDead()) {
				return DialogueGateFail::DeadSpeaker;
			}

			if (!speaker->Is3DLoaded() || !player->Is3DLoaded()) {
				return DialogueGateFail::NotLoaded;
			}

			const float distSq = DistanceSq3D(speaker, player);

			const bool sameCell = speaker->GetParentCell() == player->GetParentCell();
			if (!sameCell) {
				const bool permissiveWorldspace =
					(mode == Mode::InCombatTruce || mode == Mode::PreCombatTruce) &&
					speaker->GetWorldspace() &&
					player->GetWorldspace() &&
					speaker->GetWorldspace() == player->GetWorldspace();

				if (!permissiveWorldspace) {
					return DialogueGateFail::DifferentCell;
				}
			}
			outDist = (distSq > 0.0f) ? std::sqrt(distSq) : 0.0f;

			float maxDistSq = kBleedoutApproachDistanceSq;
			if (mode == Mode::CaptiveMarker) {
				maxDistSq = kCaptiveApproachDistanceSq;
			} else if (mode == Mode::InCombatTruce) {
				maxDistSq = kInCombatApproachDistanceSq;
			} else if (mode == Mode::PreCombatTruce) {
				maxDistSq = kCaptiveApproachDistanceSq;
			}

			if (distSq > maxDistSq) {
				return DialogueGateFail::TooFar;
			}

			// Truce forcegreet is intentionally more permissive:
			// close-enough is sufficient even if LOS/cell boundaries are flaky.
			if (mode == Mode::InCombatTruce || mode == Mode::PreCombatTruce) {
				return DialogueGateFail::None;
			}

			bool los = false;
			if (!speaker->HasLineOfSight(player, los)) {
				return DialogueGateFail::NoLOS;
			}

			return DialogueGateFail::None;
		}

		void NudgeApproach(RE::Actor* speaker)
		{
			if (!speaker) {
				return;
			}

			if (speaker->IsInCombat()) {
				speaker->StopCombat();
			}
			speaker->DrawWeaponMagicHands(false);
			speaker->EvaluatePackage(true, false);
		}

		void ClearStickyGreet(RE::Actor* speaker, Mode mode)
		{
			if (!speaker) {
				return;
			}

			const bool isTruce = (mode == Mode::InCombatTruce || mode == Mode::PreCombatTruce);
			if (!isTruce) {
				if (speaker->IsInCombat()) {
					speaker->StopCombat();
				}
				speaker->DrawWeaponMagicHands(false);
			}

			speaker->AllowPCDialogue(false);
			speaker->SetDialogueWithPlayer(false, false, nullptr);

			if (isTruce) {
				speaker->EvaluatePackage(false, true);
				speaker->EvaluatePackage(true, true);
			} else if (mode == Mode::Bleedout) {
				speaker->EvaluatePackage(true, false);
			}
		}

		double PendingOpenRetrySeconds(Mode mode)
		{
			switch (mode) {
			case Mode::Bleedout:
				return kBleedoutPendingOpenRetrySeconds;
			case Mode::InCombatTruce:
				return kInCombatPendingOpenRetrySeconds;
			case Mode::PreCombatTruce:
				return kPreCombatPendingOpenRetrySeconds;
			default:
				return 0.0;
			}
		}

		void ArmPendingOpenRetry(RE::Actor* speaker, Mode mode, double now)
		{
			const double delay = PendingOpenRetrySeconds(mode);
			if (delay <= 0.0) {
				return;
			}

			std::scoped_lock lk(lock);
			if (job.active && speaker && job.speakerHandle == speaker->GetHandle().native_handle() && job.mode == mode) {
				job.pendingOpen = true;
				job.pendingOpenUntilSec = now + delay;
				job.nextTrySec = now + delay;
			}
		}

		RE::Actor* ResolveCurrentCombatTarget(RE::Actor* actor)
		{
			if (!actor) {
				return nullptr;
			}

			auto targetSp = actor->GetActorRuntimeData().currentCombatTarget.get();
			return targetSp.get();
		}

		bool IsDialogueEnemyStillValid(RE::Actor* speaker, Mode mode)
		{
			if (!speaker || (mode != Mode::InCombatTruce && mode != Mode::PreCombatTruce)) {
				return true;
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return false;
			}

			if (TFD::Pacify::IsPacified(speaker) && TFD::Pacify::CanOpenDialogue(speaker)) {
				return true;
			}

			if (speaker->IsHostileToActor(player)) {
				return true;
			}

			auto* combatTarget = ResolveCurrentCombatTarget(speaker);
			if (combatTarget && combatTarget->GetFormID() == player->GetFormID()) {
				return true;
			}

			return false;
		}

		bool TryStartDialogue(RE::Actor* speaker, Mode mode)
		{
			if (!speaker) {
				return false;
			}

			if (IsBlockingMenuOpen()) {
				return false;
			}

			if (NowSec() < suppressUntilSec) {
				return false;
			}

			if (speaker->IsDead()) {
				return false;
			}

			if (!IsDialogueEnemyStillValid(speaker, mode)) {
				spdlog::info(
					"[TFD][ForceGreet] TryStartDialogue blocked speaker={:08X} mode={} reason=not_enemy_to_player",
					speaker->GetFormID(),
					static_cast<int>(mode));
				return false;
			}

			float dist = 99999.0f;
			const auto gate = CanStartDialogueNow(speaker, mode, dist);
			if (gate != DialogueGateFail::None) {
				spdlog::info(
					"[TFD][ForceGreet] TryStartDialogue blocked speaker={:08X} mode={} reason={} dist={:.1f}",
					speaker->GetFormID(),
					static_cast<int>(mode),
					static_cast<int>(gate),
					dist);
				return false;
			}

			if (speaker->IsInCombat()) {
				speaker->StopCombat();
			}
			speaker->DrawWeaponMagicHands(false);

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (player && (mode == Mode::Bleedout || mode == Mode::CaptiveMarker) && player->IsWeaponDrawn()) {
				player->DrawWeaponMagicHands(false);
			}

			const bool ok = speaker->SetDialogueWithPlayer(true, true, nullptr);

			if (mode == Mode::Bleedout || mode == Mode::InCombatTruce) {
				speaker->EvaluatePackage(true, false);
			}

			spdlog::info(
				"[TFD][ForceGreet] TryStartDialogue speaker={:08X} mode={} ok={} dist={:.1f}",
				speaker->GetFormID(),
				static_cast<int>(mode),
				ok ? "true" : "false",
				dist);

			return ok;
		}

		void BeginInternal(RE::Actor* speaker, Mode mode, int windowSeconds, bool immediateStart)
		{
			if (!speaker) {
				return;
			}

			if (windowSeconds <= 0) {
				windowSeconds = 8;
			}

			const double now = NowSec();

			{
				std::scoped_lock lk(lock);

				job.speakerHandle = speaker->GetHandle().native_handle();
				job.mode = mode;
				job.endTimeSec = now + static_cast<double>(windowSeconds);
				job.nextTrySec = now;
				job.pendingOpenUntilSec = 0.0;
				job.pendingOpen = false;
				job.active = true;
				job.success = false;
			}

			spdlog::info(
				"[TFD][ForceGreet] Begin speaker={:08X} mode={} window={}s immediate={}",
				speaker->GetFormID(),
				static_cast<int>(mode),
				windowSeconds,
				immediateStart ? "true" : "false");

			if (immediateStart) {
				const bool started = TryStartDialogue(speaker, mode);
				if (started) {
					ArmPendingOpenRetry(speaker, mode, now);
				}
			}
		}

		void SetSuccessLocked()
		{
			job.success = true;
			job.active = false;
			job.mode = Mode::None;
			job.pendingOpen = false;
			job.pendingOpenUntilSec = 0.0;
		}

		class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(
				const RE::MenuOpenCloseEvent* e,
				RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!e) {
					return RE::BSEventNotifyControl::kContinue;
				}

				if (e->menuName == RE::DialogueMenu::MENU_NAME && e->opening) {
					std::scoped_lock lk(lock);

					if (job.active) {
						stickyHandle = job.speakerHandle;
						stickyMode = job.mode;
						stickyArmed = (stickyHandle != 0);
						dialogueOpenedAtSec = NowSec();

						spdlog::info("[TFD][ForceGreet] DialogueMenu opened -> success (sticky armed)");
						SetSuccessLocked();
					}
					return RE::BSEventNotifyControl::kContinue;
				}

				if (e->menuName == RE::DialogueMenu::MENU_NAME && !e->opening) {
					bool shouldClearCaptive = false;
					RE::Actor* speakerForClear = nullptr;

					if (stickyArmed) {
						const double now = NowSec();

						if ((now - dialogueOpenedAtSec) < 120.0) {
							auto* speaker = ResolveSpeaker(stickyHandle);

							ClearStickyGreet(speaker, stickyMode);

							if (stickyMode == Mode::CaptiveMarker) {
								suppressUntilSec = now + kCaptiveSuppressSeconds;
								shouldClearCaptive = true;
								speakerForClear = speaker;

								spdlog::info("[TFD][ForceGreet] Captive dialogue closed -> suppress + clearall");
							}
							else {
								suppressUntilSec = now + kNormalSuppressSeconds;
								spdlog::info("[TFD][ForceGreet] Dialogue closed -> suppress");
								if ((stickyMode == Mode::InCombatTruce || stickyMode == Mode::PreCombatTruce) && speaker) {
									const bool onlyIfStillHostile = (stickyMode == Mode::InCombatTruce);
									const bool released = TFD::Pacify::ReleaseActiveTruceSessionForActor(
										speaker,
										TFD::Pacify::ReleaseReason::DialogueClosed,
										onlyIfStillHostile);
									spdlog::info("[TFD][ForceGreet] Truce dialogue closed -> release={} mode={} onlyIfStillHostile={}",
										released ? 1 : 0,
										static_cast<int>(stickyMode),
										onlyIfStillHostile ? 1 : 0);
								}
							}
						}

						stickyArmed = false;
						stickyHandle = 0;
						stickyMode = Mode::None;
					}

					if (shouldClearCaptive) {
						ClearCaptiveAliases(speakerForClear, "dialogue_closed");
					}

					return RE::BSEventNotifyControl::kContinue;
				}

				return RE::BSEventNotifyControl::kContinue;
			}
		};

		static MenuSink menuSink;

		class InputSink : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(
				RE::InputEvent* const*,
				RE::BSTEventSource<RE::InputEvent*>*) override
			{
				TFD::ForceGreet::Tick();
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		static InputSink inputSink;

		void OnSkseMessage(SKSE::MessagingInterface::Message* msg)
		{
			if (!msg) {
				return;
			}

			if (msg->type == SKSE::MessagingInterface::kInputLoaded) {
				if (inputSinkAdded.exchange(true)) {
					return;
				}

				auto* input = RE::BSInputDeviceManager::GetSingleton();
				if (input) {
					input->AddEventSink(&inputSink);
					spdlog::info("[TFD][ForceGreet] InputSink added");
				}
				else {
					spdlog::warn("[TFD][ForceGreet] BSInputDeviceManager null");
					inputSinkAdded.store(false);
				}
			}
		}
	}

	void Install()
	{
		if (installed.exchange(true)) {
			return;
		}

		if (auto* messaging = SKSE::GetMessagingInterface()) {
			messaging->RegisterListener(OnSkseMessage);
		}
		else {
			spdlog::warn("[TFD][ForceGreet] MessagingInterface null");
		}

		if (!menuSinkAdded.exchange(true)) {
			if (auto* ui = RE::UI::GetSingleton()) {
				ui->AddEventSink<RE::MenuOpenCloseEvent>(&menuSink);
				spdlog::info("[TFD][ForceGreet] MenuSink added");
			}
			else {
				spdlog::warn("[TFD][ForceGreet] UI singleton null (MenuSink not added)");
				menuSinkAdded.store(false);
			}
		}
	}

	void BeginBleedout(RE::Actor* speaker)
	{
		const int window = TFD::Settings::GetBleedWindowSeconds();
		BeginInternal(speaker, Mode::Bleedout, window, true);
	}

	void BeginCaptiveMarker(RE::Actor* speaker)
	{
		const int window = TFD::Settings::GetBleedWindowSeconds();
		BeginInternal(speaker, Mode::CaptiveMarker, window, false);
		Tick();
	}

	void BeginInCombatTruce(RE::Actor* speaker)
	{
		BeginInternal(speaker, Mode::InCombatTruce, 20, true);
		Tick();
	}

	void BeginPreCombatTruce(RE::Actor* speaker)
	{
		BeginInternal(speaker, Mode::PreCombatTruce, 20, true);
		Tick();
	}

	void Tick()
	{
		Job snap;
		{
			std::scoped_lock lk(lock);
			snap = job;
		}

		if (!snap.active) {
			return;
		}

		if (IsDialogueOpen()) {
			std::scoped_lock lk(lock);
			SetSuccessLocked();
			return;
		}

		const double now = NowSec();

		if (now >= snap.endTimeSec) {
			RE::Actor* speaker = ResolveSpeaker(snap.speakerHandle);
			const bool wasCaptive = (snap.mode == Mode::CaptiveMarker);

			{
				std::scoped_lock lk(lock);
				job.active = false;
				job.mode = Mode::None;
				job.speakerHandle = 0;
				job.pendingOpen = false;
				job.pendingOpenUntilSec = 0.0;
			}

			spdlog::info("[TFD][ForceGreet] Timed out mode={}", static_cast<int>(snap.mode));

			if (wasCaptive) {
				ClearCaptiveAliases(speaker, "timeout");
			}
			return;
		}

		if (snap.pendingOpen) {
			if (now < snap.pendingOpenUntilSec) {
				return;
			}
			std::scoped_lock lk(lock);
			if (job.active && job.speakerHandle == snap.speakerHandle && job.mode == snap.mode) {
				job.pendingOpen = false;
				if (job.nextTrySec < now) {
					job.nextTrySec = now;
				}
			}
		}

		if (now < snap.nextTrySec) {
			return;
		}

		{
			std::scoped_lock lk(lock);
			job.nextTrySec = now + 0.20;
		}

		auto* speaker = ResolveSpeaker(snap.speakerHandle);
		if (!IsSpeakerUsable(speaker)) {
			const bool wasCaptive = (snap.mode == Mode::CaptiveMarker);

			{
				std::scoped_lock lk(lock);
				job.active = false;
				job.mode = Mode::None;
				job.speakerHandle = 0;
				job.pendingOpen = false;
				job.pendingOpenUntilSec = 0.0;
			}

			spdlog::info("[TFD][ForceGreet] Speaker lost -> cancel");

			if (wasCaptive) {
				ClearCaptiveAliases(nullptr, "speaker_lost");
			}
			return;
		}

		if (!IsDialogueEnemyStillValid(speaker, snap.mode)) {
			{
				std::scoped_lock lk(lock);
				job.active = false;
				job.mode = Mode::None;
				job.speakerHandle = 0;
				job.pendingOpen = false;
				job.pendingOpenUntilSec = 0.0;
			}

			spdlog::info(
				"[TFD][ForceGreet] Speaker no longer enemy to player -> cancel speaker={:08X} mode={}",
				speaker->GetFormID(),
				static_cast<int>(snap.mode));
			return;
		}

		float dist = 99999.0f;
		const auto gate = CanStartDialogueNow(speaker, snap.mode, dist);

		if (gate != DialogueGateFail::None) {
			if ((snap.mode == Mode::CaptiveMarker || snap.mode == Mode::InCombatTruce || snap.mode == Mode::PreCombatTruce) &&
				(gate == DialogueGateFail::DifferentCell || gate == DialogueGateFail::TooFar || gate == DialogueGateFail::NoLOS)) {
				NudgeApproach(speaker);
			}

			spdlog::info(
				"[TFD][ForceGreet] Waiting speaker={:08X} mode={} reason={} dist={:.1f}",
				speaker->GetFormID(),
				static_cast<int>(snap.mode),
				static_cast<int>(gate),
				dist);
			return;
		}

		if (snap.mode == Mode::CaptiveMarker) {
			spdlog::info(
				"[TFD][ForceGreet] Captive close enough -> start dialogue dist={:.1f} speaker={:08X}",
				dist,
				speaker->GetFormID());
		} else if (snap.mode == Mode::InCombatTruce) {
			spdlog::info(
				"[TFD][ForceGreet] InCombat close enough -> start dialogue dist={:.1f} speaker={:08X}",
				dist,
				speaker->GetFormID());
		} else if (snap.mode == Mode::PreCombatTruce) {
			spdlog::info(
				"[TFD][ForceGreet] PreCombat close enough -> start dialogue dist={:.1f} speaker={:08X}",
				dist,
				speaker->GetFormID());
		}

		const bool started = TryStartDialogue(speaker, snap.mode);
		if (started) {
			ArmPendingOpenRetry(speaker, snap.mode, now);
		}
	}

	void Cancel()
	{
		RE::Actor* stickySpeaker = nullptr;
		Mode oldStickyMode = Mode::None;
		Mode oldJobMode = Mode::None;
		RE::Actor* jobSpeaker = nullptr;

		{
			std::scoped_lock lk(lock);

			if (stickyArmed) {
				stickySpeaker = ResolveSpeaker(stickyHandle);
				oldStickyMode = stickyMode;
			}

			oldJobMode = job.mode;
			jobSpeaker = ResolveSpeaker(job.speakerHandle);

			job.active = false;
			job.success = false;
			job.mode = Mode::None;
			job.speakerHandle = 0;
			job.endTimeSec = 0.0;
			job.nextTrySec = 0.0;

			stickyArmed = false;
			stickyHandle = 0;
			stickyMode = Mode::None;
			suppressUntilSec = 0.0;
			dialogueOpenedAtSec = 0.0;
		}

		if (stickySpeaker) {
			ClearStickyGreet(stickySpeaker, oldStickyMode);
		}

		if (oldStickyMode == Mode::CaptiveMarker || oldJobMode == Mode::CaptiveMarker) {
			ClearCaptiveAliases(jobSpeaker ? jobSpeaker : stickySpeaker, "cancel");
		}
	}

	bool IsActive()
	{
		std::scoped_lock lk(lock);
		return job.active;
	}

	bool DidSucceed()
	{
		std::scoped_lock lk(lock);
		return job.success;
	}

	Mode GetMode()
	{
		std::scoped_lock lk(lock);
		return job.mode;
	}
}
