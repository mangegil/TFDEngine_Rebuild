#include "TFDDefeatMonitor.h"
#include "TFDActor.h"
#include "TFDVictory.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <string>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <tuple>
#include <array>
#include <random>
#include <limits>
#include <utility>

#include <RE/Skyrim.h>
#include <type_traits>
#include <RE/A/ActorValues.h>
#include <RE/L/LockpickingMenu.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include "TFDSettings.h"
#include "TFDLocation.h"
#include "TFDCaptive.h"
#include "TFDCaptiveGreet.h"
#include "TFDRescueGreet.h"
#include "TFDRescue.h"
#include "TFDPostDefeatState.h"
#include "TFDDefeatBridge.h"
#include "TFDDefeatRuntimeProviders.h"
#include "TFDDefeatLifecycleBootstrap.h"
#include "TFDCaptiveDoorController.h"
#include "TFDHostilityController.h"

#include "TFDPreCombatGreet.h"
#include "TFDInCombat.h"
#include "TFDInCombatGreet.h"
#include "TFDTame.h"
#include "TFDFlowController.h"
#include "TFDInteractionRouter.h"
#include "TFDTransition.h"
#include "TFDBleedout.h"
#include "TFDBleedoutGreet.h"
#include "TFDPleasureRuntime.h"
#include "TFDPlayerDamageGuard.h"
#include "TFDPlayerOverkillDamageHook.h"
#include "TFDPlayerDownRouter.h"
#include "TFDBleedLockState.h"
#include "TFDDialogueLifecycle.h"
#include "TFDDefeatRuntimeActions.h"
#include "TFDPlayerDownRouterWiring.h"
#include "TFDBleedLockRuntime.h"
#include "TFDBleedoutDialogueRuntime.h"
#include "TFDTeammateManager.h"
#include "TFDCaptiveRecaptureRecovery.h"
#include "TFDCaptiveRecaptureRecoveryWiring.h"
#include "EditorIdCache.h"
#include "RE/B/BGSRefAlias.h"
#include "RE/T/TESQuest.h"

namespace TFD::DefeatMonitor
{
	static RE::Actor* ResolveCurrentPassivePrimaryActor();
	static bool IsActorCoveredByCurrentPassiveContext(RE::Actor* actor);

	namespace
	{
		static bool ActorHasLineOfSightToPlayer(RE::Actor* actor, RE::Actor* player);
		static float Distance3D(const RE::NiPoint3& a, const RE::NiPoint3& b);
		using CaptivePhaseValue = TFD::Captive::PhaseValue;


		constexpr const char* kBleedoutPrimeSpeakerEvent = "TFDBleedoutPrimeSpeaker";
		constexpr const char* kBleedoutCancelStickyReopenEvent = "TFDBleedoutCancelStickyReopen";
		constexpr const char* kBleedoutOutcomePayEvent = "TFDBleedoutOutcomePay";
		constexpr const char* kBleedoutOutcomePleasureEvent = "TFDBleedoutOutcomePleasure";
		constexpr const char* kBleedoutOutcomeCaptiveEvent = "TFDBleedoutOutcomeCaptive";
		constexpr const char* kBleedoutOutcomeReleaseEvent = "TFDBleedoutOutcomeRelease";
		constexpr const char* kBleedoutOutcomeResetEvent = "TFDBleedoutOutcomeReset";
		constexpr const char* kBleedoutOutcomeDoNothingEvent = "TFDBleedoutOutcomeDoNothing";
		constexpr const char* kInCombatOutcomeReleaseEvent = "TFDInCombatOutcomeRelease";
		constexpr const char* kInCombatOutcomeFollowEvent = "TFDInCombatOutcomeFollow";
		constexpr const char* kInCombatOutcomePayEvent = "TFDInCombatOutcomePay";
		constexpr const char* kInCombatOutcomePleasureEvent = "TFDInCombatOutcomePleasure";
		constexpr const char* kInCombatOutcomeCaptiveEvent = "TFDInCombatOutcomeCaptive";
		constexpr const char* kInCombatOutcomeResetEvent = "TFDInCombatOutcomeReset";
		constexpr const char* kPleasureOutcomeReleaseEvent = "TFDPleasureOutcomeRelease";
		constexpr const char* kAfterPleasureEnterEvent = "TFDAfterPleasureEnter";
		constexpr const char* kBleedoutGreetConfirmedEvent = "TFDBleedoutGreetConfirmed";

		static std::uint32_t ResolveBleedFlowActorFormID();
		static void MaintainBleedPrimaryCaptorBinding();
		static bool IsStandingAllyThresholdActor(RE::Actor* actor);
		static TFD::Transition::RuntimeHandlers BuildTransitionRuntimeHandlers();
		static bool IsCombatSupportedAggressor(RE::Actor* actor);
		static bool IsObserverAlly(RE::Actor* actor);
		static bool HasPlayerBleedLock();
		static void ClampHealth(RE::Actor* actor, float minHp);
		static TFD::PlayerDamageGuard::Config BuildPlayerDamageGuardConfig(RE::Actor* actor, float thresholdPct, float protectedHp, float safeFloorHp);
		static void NoteEnemyTargetingPlayerInternal(RE::Actor* actor);
		static bool HasRecentEnemyTargetingPlayerInternal(double maxAgeSec);
		static TFD::PlayerDownRouterWiring::DispatchContext BuildPlayerDownRouterDispatchContext();
		static TFD::PlayerDownRouterWiring::NoThreatContext BuildPlayerDownRouterNoThreatContext();
		static TFD::PlayerDownRouterWiring::PendingContext BuildPlayerDownRouterPendingContext();
		static TFD::BleedLockRuntime::Context BuildBleedLockRuntimeContext();
		static TFD::BleedoutDialogueRuntime::Context BuildBleedoutDialogueRuntimeContext();
		static void ResetBleedRuntimeState(bool preserveCaptive);

		using BleedDialogueOutcome = TFD::Bleedout::DialogueOutcome;
		using BleedTerminalCommit = TFD::Bleedout::TerminalCommit;

		std::atomic_bool g_installed{ false };
		std::atomic_bool g_running{ false };
		std::atomic_bool g_loadTransition{ false };
		std::atomic_flag g_tickPending = ATOMIC_FLAG_INIT;
		std::thread g_worker{};

		static bool g_lastRouterCombatContextActive = false;


		std::atomic_bool g_inBleedState{ false };
		float g_minHp{ 0.0f };

		// P11: pre-death shield lifecycle is owned by TFDPlayerOverkillDamageHook.
		std::chrono::steady_clock::time_point g_playerBleedTargetFirewallLast{};


		bool g_playerBleedImmuneForced = false;
		bool g_playerWasEssential = false;
		bool g_playerWasInvulnerable = false;
		bool g_playerWasNoBleedoutRecovery = false;
		bool g_playerWasBaseInvulnerable = false;

		// P10: killmove guard lifecycle is owned by TFDPlayerOverkillDamageHook.

		static std::chrono::steady_clock::time_point Now()
		{
			return std::chrono::steady_clock::now();
		}

		std::chrono::steady_clock::time_point g_bleedStart{};
		int g_bleedLastSeconds = -1;
		bool g_bleedPaused = false;
		std::chrono::steady_clock::time_point g_bleedPauseStarted{};
		std::chrono::steady_clock::time_point g_bleedLastCalmPulse{};

		bool g_bleedStickyReopenGraceActive = false;
		std::chrono::steady_clock::time_point g_bleedStickyReopenGraceUntil{};
		std::uint32_t g_bleedStickyReopenGraceSpeakerID = 0;
		float g_bleedStickyReopenGraceDistance = 99999.0f;

		bool g_postTerminalBleedoutLockoutActive = false;
		std::chrono::steady_clock::time_point g_postTerminalBleedoutLockoutUntil{};
		std::chrono::steady_clock::time_point g_postTerminalBleedoutLockoutLastLog{};
		std::string g_postTerminalBleedoutLockoutReason{};
		float g_postTerminalBleedoutLockoutArmHpPct = -1.0f;

		std::atomic_bool g_grace{ false };
		std::chrono::steady_clock::time_point g_graceUntil{};


		static bool IsGraceActive()
		{
			if (!g_grace.load(std::memory_order_acquire)) {
				return false;
			}
			if (Now() >= g_graceUntil) {
				g_grace.store(false, std::memory_order_release);
				g_graceUntil = {};
				return false;
			}
			return true;
		}

		static RE::PlayerCharacter* Player();
		static bool IsCaptiveSupportedAggressor(RE::Actor* actor);
		static bool IsStandingEnemyThresholdActor(RE::Actor* actor);
		static bool IsBleedSpaceCompatible(RE::Actor* actor, RE::Actor* player);
		static RE::Actor* ResolveCurrentCombatTarget(RE::Actor* actor);
		static bool IsActiveFollowerActor(RE::Actor* actor);
		static bool IsActorCloseAndFront(RE::Actor* actor, RE::Actor* player, float maxDist);

		RE::ActorHandle g_lastAggressor{};

		static void SetGraceSeconds(int seconds)
		{
			if (seconds <= 0) {
				g_grace.store(false, std::memory_order_release);
				g_graceUntil = {};
				return;
			}
			g_graceUntil = Now() + std::chrono::seconds(seconds);
			g_grace.store(true, std::memory_order_release);
		}

		static bool IsBleedoutTerminalOutcomeEvent(const char* rawName)
		{
			if (!rawName || !rawName[0]) {
				return false;
			}
			return std::strcmp(rawName, kBleedoutOutcomePayEvent) == 0 ||
				std::strcmp(rawName, kBleedoutOutcomePleasureEvent) == 0 ||
				std::strcmp(rawName, kBleedoutOutcomeCaptiveEvent) == 0 ||
				std::strcmp(rawName, kBleedoutOutcomeReleaseEvent) == 0 ||
				std::strcmp(rawName, kBleedoutOutcomeResetEvent) == 0 ||
				std::strcmp(rawName, kBleedoutOutcomeDoNothingEvent) == 0;
		}

		static double ResolvePostTerminalBleedoutLockoutSeconds(const char* rawName, float numArg)
		{
			if (!rawName) {
				return 6.0;
			}

			if (_stricmp(rawName, "TFDBleedoutOutcomePay") == 0) {
				const double requested = numArg > 0.0f ? static_cast<double>(numArg) : 30.0;
				return std::clamp(requested, 20.0, 40.0);
			}

			if (_stricmp(rawName, "TFDBleedoutOutcomeRelease") == 0) {
				const double requested = numArg > 0.0f ? static_cast<double>(numArg) : 30.0;
				return std::clamp(requested, 20.0, 40.0);
			}

			if (_stricmp(rawName, "TFDBleedoutOutcomeDoNothing") == 0) {
				return 12.0;
			}

			if (_stricmp(rawName, "TFDBleedoutOutcomeCaptive") == 0 ||
				_stricmp(rawName, "TFDBleedoutOutcomePleasure") == 0) {
				return 6.0;
			}

			return 6.0;
		}

		static void ArmPostTerminalBleedoutLockout(const char* eventName, double seconds, const char* reason)
		{
			const double duration = std::clamp(seconds, 1.0, 45.0);
			g_postTerminalBleedoutLockoutActive = true;
			g_postTerminalBleedoutLockoutUntil = Now() + std::chrono::milliseconds(static_cast<int>(duration * 1000.0));
			g_postTerminalBleedoutLockoutLastLog = {};
			g_postTerminalBleedoutLockoutReason = reason && reason[0] ? reason : (eventName && eventName[0] ? eventName : "terminal_outcome");
			g_postTerminalBleedoutLockoutArmHpPct = -1.0f;
			if (auto* player = Player()) {
				const float hpNow = player->GetActorValue(RE::ActorValue::kHealth);
				const float hpMax = (std::max)(1.0f, player->GetPermanentActorValue(RE::ActorValue::kHealth));
				g_postTerminalBleedoutLockoutArmHpPct = (hpNow / hpMax) * 100.0f;
			}
			spdlog::info("[TFD][Defeat][R216A] post-terminal bleedout threshold lockout armed event={} seconds={:.1f} reason={} armHpPct={:.1f}",
				eventName && eventName[0] ? eventName : "<none>",
				duration,
				g_postTerminalBleedoutLockoutReason,
				g_postTerminalBleedoutLockoutArmHpPct);
		}

		static bool IsPostTerminalBleedoutLockoutActive(const char* checkReason, float hpPct, float thresholdPct)
		{
			if (!g_postTerminalBleedoutLockoutActive) {
				return false;
			}
			const auto now = Now();
			if (now >= g_postTerminalBleedoutLockoutUntil) {
				// R272A: terminal Release/Pay can leave the player exactly on the
				// defeat threshold floor.  When the temporary pacify owner expires,
				// hostile projection can return without any new damage and immediately
				// reopen Bleedout.  Hold the lockout while the player has not taken
				// fresh post-terminal damage; if HP drops below the armed floor, allow
				// normal re-bleed because that is a real new defeat.
				const bool stillAtThresholdFloor = hpPct <= (thresholdPct + 0.25f);
				const bool noFreshPostTerminalDamage = g_postTerminalBleedoutLockoutArmHpPct >= 0.0f &&
					hpPct >= (g_postTerminalBleedoutLockoutArmHpPct - 0.50f);
				if (stillAtThresholdFloor && noFreshPostTerminalDamage) {
					g_postTerminalBleedoutLockoutUntil = now + std::chrono::milliseconds(1500);
					if (g_postTerminalBleedoutLockoutLastLog.time_since_epoch().count() == 0 ||
						(now - g_postTerminalBleedoutLockoutLastLog) >= std::chrono::milliseconds(900)) {
						g_postTerminalBleedoutLockoutLastLog = now;
						spdlog::info("[TFD][Defeat][R272A] post-terminal lockout held until player recovery check={} hpPct={:.1f} armHpPct={:.1f} threshold={:.1f} reason={}",
							checkReason && checkReason[0] ? checkReason : "threshold",
							hpPct,
							g_postTerminalBleedoutLockoutArmHpPct,
							thresholdPct,
							g_postTerminalBleedoutLockoutReason.empty() ? "unknown" : g_postTerminalBleedoutLockoutReason.c_str());
					}
					return true;
				}

				g_postTerminalBleedoutLockoutActive = false;
				g_postTerminalBleedoutLockoutUntil = {};
				g_postTerminalBleedoutLockoutLastLog = {};
				spdlog::info("[TFD][Defeat][R216A] post-terminal bleedout threshold lockout expired reason={} hpPct={:.1f} armHpPct={:.1f} threshold={:.1f}",
					g_postTerminalBleedoutLockoutReason.empty() ? "unknown" : g_postTerminalBleedoutLockoutReason.c_str(),
					hpPct,
					g_postTerminalBleedoutLockoutArmHpPct,
					thresholdPct);
				g_postTerminalBleedoutLockoutReason.clear();
				g_postTerminalBleedoutLockoutArmHpPct = -1.0f;
				return false;
			}
			if (g_postTerminalBleedoutLockoutLastLog.time_since_epoch().count() == 0 ||
				(now - g_postTerminalBleedoutLockoutLastLog) >= std::chrono::milliseconds(900)) {
				g_postTerminalBleedoutLockoutLastLog = now;
				const auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(g_postTerminalBleedoutLockoutUntil - now).count();
				spdlog::info("[TFD][Defeat][R216A] player threshold suppressed during post-terminal lockout check={} hpPct={:.1f} threshold={:.1f} remainingMs={} reason={}",
					checkReason && checkReason[0] ? checkReason : "threshold",
					hpPct,
					thresholdPct,
					static_cast<long long>(remainingMs),
					g_postTerminalBleedoutLockoutReason.empty() ? "unknown" : g_postTerminalBleedoutLockoutReason.c_str());
			}
			return true;
		}

		static std::uint32_t ResolveBleedFlowActorFormID()
		{
			const auto speakerId = TFD::Bleedout::GetBleedSpeakerID();
			if (speakerId != 0) {
				return speakerId;
			}
			return TFD::FlowController::Controller::GetSingleton().GetSnapshot().primaryActorFormID;
		}
		RE::ActorHandle g_lastEnemyTargetingPlayer{};
		RE::FormID g_lastEnemyTargetingPlayerFormID = 0;
		std::chrono::steady_clock::time_point g_lastEnemyTargetingPlayerSeen{};
		bool g_bleedPendingCaptiveOutcome = false;
		bool g_bleedPendingNonCaptiveOutcome = false;

		struct BleedBattleObserverState
		{
			bool hadValidObservedEnemy = false;
			std::unordered_set<RE::FormID> allyIds{};
			std::unordered_set<RE::FormID> enemyIds{};
		};

		bool g_bleedBattleObservePending = false;
		std::chrono::steady_clock::time_point g_bleedBattleObservePendingUntil{};
		std::chrono::steady_clock::time_point g_bleedBattleObservePendingLastRedirect{};
		int g_bleedBattleObservePendingEmptyEnemyTicks = 0;
		int g_bleedBattleObservePendingEmptyAllyTicks = 0;
		bool g_bleedBattleObserveActive = false;
		std::chrono::steady_clock::time_point g_bleedBattleObserveSince{};
		std::chrono::steady_clock::time_point g_bleedBattleObserveLastRedirect{};
		int g_bleedBattleObserveActiveEmptyEnemyTicks = 0;
		int g_bleedBattleObserveActiveEmptyAllyTicks = 0;
		RE::ActorHandle g_bleedBattlePreferredEnemy{};
		BleedBattleObserverState g_bleedBattleObserver{};
		thread_local std::uint32_t g_observedCombatCommitDepth = 0;

		struct ObservedCombatCommitScope
		{
			ObservedCombatCommitScope()
			{
				++g_observedCombatCommitDepth;
			}

			~ObservedCombatCommitScope()
			{
				if (g_observedCombatCommitDepth > 0) {
					--g_observedCombatCommitDepth;
				}
			}
		};

		using BleedLockKind = TFD::BleedLockState::Kind;
		using BleedLockEntry = TFD::BleedLockState::Entry;

		static const char* ReferenceProbeKindName(BleedLockKind kind)
		{
			return kind == BleedLockKind::Player ? "Player" : "Ally";
		}

		static const char* ReferenceProbeLifeStateName(RE::ACTOR_LIFE_STATE state)
		{
			switch (state) {
			case RE::ACTOR_LIFE_STATE::kAlive:
				return "Alive";
			case RE::ACTOR_LIFE_STATE::kDying:
				return "Dying";
			case RE::ACTOR_LIFE_STATE::kDead:
				return "Dead";
			case RE::ACTOR_LIFE_STATE::kUnconcious:
				return "Unconscious";
			case RE::ACTOR_LIFE_STATE::kReanimate:
				return "Reanimate";
			case RE::ACTOR_LIFE_STATE::kRecycle:
				return "Recycle";
			case RE::ACTOR_LIFE_STATE::kRestrained:
				return "Restrained";
			case RE::ACTOR_LIFE_STATE::kEssentialDown:
				return "EssentialDown";
			case RE::ACTOR_LIFE_STATE::kBleedout:
				return "Bleedout";
			default:
				return "Unknown";
			}
		}

		static const char* ReferenceProbeKnockStateName(RE::KNOCK_STATE_ENUM state)
		{
			switch (state) {
			case RE::KNOCK_STATE_ENUM::kNormal:
				return "Normal";
			case RE::KNOCK_STATE_ENUM::kExplode:
				return "Explode";
			case RE::KNOCK_STATE_ENUM::kExplodeLeadIn:
				return "ExplodeLeadIn";
			case RE::KNOCK_STATE_ENUM::kOut:
				return "Out";
			case RE::KNOCK_STATE_ENUM::kOutLeadIn:
				return "OutLeadIn";
			case RE::KNOCK_STATE_ENUM::kQueued:
				return "Queued";
			case RE::KNOCK_STATE_ENUM::kGetUp:
				return "GetUp";
			case RE::KNOCK_STATE_ENUM::kDown:
				return "Down";
			case RE::KNOCK_STATE_ENUM::kWaitForTaskQueue:
				return "WaitForTaskQueue";
			default:
				return "Unknown";
			}
		}

		static void LogReferenceBleedState(
			RE::Actor* actor,
			const BleedLockEntry& entry,
			const char* point,
			const char* note)
		{
			if (!actor) {
				spdlog::info(
					"[TFD][Defeat][R398A][ReferenceProbe] point={} actor=00000000 kind={} actorMissing=1 pulseCount={} probeOnly=1 mutation=0 note={}",
					point ? point : "unknown",
					ReferenceProbeKindName(entry.kind),
					entry.referenceProbePulseCount,
					note ? note : "-");
				return;
			}

			const auto* state = actor->AsActorState();
			const auto lifeState = state ? state->GetLifeState() : RE::ACTOR_LIFE_STATE::kAlive;
			const auto knockState = state ? state->GetKnockState() : RE::KNOCK_STATE_ENUM::kNormal;
			const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
			const auto& flags = actor->GetActorRuntimeData().boolFlags;

			spdlog::info(
				"[TFD][Defeat][R398A][ReferenceProbe] point={} actor={:08X} kind={} hp={:.2f} hpPct={:.1f} threshold={:.1f} life={}({}) knock={}({}) bleedingOut={} unconscious={} inBleedoutAnimation={} noBleedoutRecovery={} canSpeakEssentialDown={} essential={} protected={} loaded3D={} inCombat={} pulseCount={} probeOnly=1 mutation=0 note={}",
				point ? point : "unknown",
				actor->GetFormID(),
				ReferenceProbeKindName(entry.kind),
				hpNow,
				(hpNow / hpMax) * 100.0f,
				entry.thresholdPct,
				state ? ReferenceProbeLifeStateName(lifeState) : "NoActorState",
				state ? static_cast<std::int32_t>(lifeState) : -1,
				state ? ReferenceProbeKnockStateName(knockState) : "NoActorState",
				state ? static_cast<std::int32_t>(knockState) : -1,
				state && state->IsBleedingOut() ? 1 : 0,
				state && state->IsUnconscious() ? 1 : 0,
				flags.all(RE::Actor::BOOL_FLAGS::kInBleedoutAnimation) ? 1 : 0,
				flags.all(RE::Actor::BOOL_FLAGS::kNoBleedoutRecovery) ? 1 : 0,
				flags.all(RE::Actor::BOOL_FLAGS::kCanSpeakToEssentialDown) ? 1 : 0,
				flags.all(RE::Actor::BOOL_FLAGS::kEssential) ? 1 : 0,
				flags.all(RE::Actor::BOOL_FLAGS::kProtected) ? 1 : 0,
				actor->Is3DLoaded() ? 1 : 0,
				actor->IsInCombat() ? 1 : 0,
				entry.referenceProbePulseCount,
				note ? note : "-");
		}

		static void MaybeLogReferenceBleedSamples(
			RE::Actor* actor,
			BleedLockEntry& entry,
			std::chrono::steady_clock::time_point now)
		{
			if (!actor || entry.referenceProbeStartedAt.time_since_epoch().count() == 0) {
				return;
			}

			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				now - entry.referenceProbeStartedAt);
			const auto emit = [&](std::uint8_t bit, std::chrono::milliseconds delay, const char* point) {
				if ((entry.referenceProbeSampleMask & bit) == 0 && elapsed >= delay) {
					entry.referenceProbeSampleMask = static_cast<std::uint8_t>(entry.referenceProbeSampleMask | bit);
					LogReferenceBleedState(actor, entry, point, "scheduled_reference_sample");
				}
			};

			emit(0x01, std::chrono::milliseconds(250), "reference_250ms");
			emit(0x02, std::chrono::milliseconds(1000), "reference_1000ms");
			emit(0x04, std::chrono::milliseconds(3000), "reference_3000ms");
			emit(0x08, std::chrono::milliseconds(7500), "reference_7500ms");
		}

		auto& g_bleedLocks = TFD::BleedLockState::Entries();
		std::chrono::steady_clock::time_point g_enemyThresholdScanLast{};
		std::chrono::steady_clock::time_point g_thresholdScanImmediateDispatchLast{};



		static constexpr std::size_t kBleedBridgeMaxActors = 10;

		bool g_hasQueuedProgressState = false;
		bool g_queuedBleedOutState = false;

		static RE::PlayerCharacter* Player()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

		static void SetPlayerBleedImmune(bool enable)
		{
			auto* player = Player();
			if (!player) {
				return;
			}

			// R19: hard player damage immunity is owned by TFDPlayerDamageGuard.
			// DefeatMonitor may request the guard during threshold/bleed detection,
			// but it no longer owns or restores player essential/protected flags.
			TFD::PlayerDamageGuard::SetHardImmunity(
				player,
				enable,
				enable ? "defeat_monitor_request_enable" : "defeat_monitor_request_release");
		}

		// P10: killmove guard lifecycle moved to TFDPlayerOverkillDamageHook.


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

		static bool ActorHasLineOfSightToPlayer(RE::Actor* actor, RE::Actor* player)
		{
			if (!actor || !player) {
				return false;
			}
			bool hasLOSData = false;
			return actor->HasLineOfSight(player, hasLOSData);
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

		static bool IsActiveFollowerActor(RE::Actor* actor);
		static bool IsStandingEnemyThresholdActor(RE::Actor* actor);

		static bool IsCombatSupportedAggressor(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			if (actor == Player()) {
				return false;
			}
			if (actor->IsDead() || actor->IsDisabled()) {
				return false;
			}
			if (IsActiveFollowerActor(actor) || TFD::Tame::IsCompanion(actor)) {
				return false;
			}
			return true;
		}

		static bool IsReasonableCombatAggressor(RE::Actor* actor, RE::Actor* player, float maxDist, float* outDistance = nullptr)
		{
			if (outDistance) {
				*outDistance = -1.0f;
			}
			if (!actor || !player) {
				return false;
			}
			if (!IsCombatSupportedAggressor(actor)) {
				return false;
			}
			if (!IsStandingEnemyThresholdActor(actor) || !actor->Is3DLoaded()) {
				return false;
			}
			auto* playerCell = player->GetParentCell();
			auto* actorCell = actor->GetParentCell();
			if (playerCell && actorCell != playerCell) {
				return false;
			}
			auto* playerWs = player->GetWorldspace();
			if (playerWs && actor->GetWorldspace() != playerWs) {
				return false;
			}
			const auto pp = player->GetPosition();
			const auto ap = actor->GetPosition();
			const float dx = ap.x - pp.x;
			const float dy = ap.y - pp.y;
			const float dz = ap.z - pp.z;
			const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (outDistance) {
				*outDistance = dist;
			}
			if (maxDist > 0.0f && dist > maxDist) {
				return false;
			}
			if (actor->IsHostileToActor(player) || actor->IsInCombat()) {
				return true;
			}
			auto targetSp = actor->GetActorRuntimeData().currentCombatTarget.get();
			auto* currentTarget = targetSp.get();
			if (currentTarget == player) {
				return true;
			}
			if (currentTarget && IsActiveFollowerActor(currentTarget)) {
				return true;
			}
			return false;
		}

		static bool IsBleedCrowdSupportedAggressor(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			if (IsActiveFollowerActor(actor)) {
				return false;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeCreature") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeAnimal") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeDragon") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeDaedra") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeGhost") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeUndead")) {
				return false;
			}
			return ActorHasKeywordByEditorID(actor, "ActorTypeNPC");
		}

		static RE::Actor* ResolveAggressor();
		static RE::Actor* ResolveLastEnemyTargetingPlayerInternal(float radius = 0.0f, double maxAgeSec = 15.0);
		static void NoteEnemyTargetingPlayerInternal(RE::Actor* actor);
		static void ClearLastEnemyTargetingPlayerInternal();
		static RE::Actor* FindBestAggressor(float radius);
		static bool IsTrackedDefeatedEnemyHook(RE::Actor* actor)
		{
			return actor && g_bleedBattleObserver.enemyIds.find(actor->GetFormID()) != g_bleedBattleObserver.enemyIds.end();
		}

		static bool IsLastAggressorHook(RE::Actor* actor)
		{
			if (!actor || !g_lastAggressor) {
				return false;
			}
			auto sp = RE::Actor::LookupByHandle(g_lastAggressor.native_handle());
			return sp.get() == actor;
		}

		static std::vector<RE::Actor*> CollectBleedStandingFollowers(float radius);
		static bool IsObserverAlly(RE::Actor* actor);
		static bool IsValidBleedBattleEnemyRosterActor(RE::Actor* actor, RE::Actor* player);
		static bool IsActorBleedingOut(RE::Actor* actor);
		static float GetActorHealthPct(RE::Actor* actor);
		static void RestoreActorHealthToSafePct(RE::Actor* actor, float thresholdPct, float bonusPct, float minSafePct, float maxSafePct, float minAbsHp, const char* reason);
		static RE::Actor* ResolveCurrentCombatTarget(RE::Actor* actor);
		static void ClampHealth(RE::Actor* actor, float minHp);
		static bool ComputePlayerBleedOutState(RE::Actor* player);

		static bool ComputePlayerBleedOutState(RE::Actor* player)
		{
			if (!player || player->IsDead() || player->IsDisabled()) {
				return false;
			}

			const float hpNow = player->GetActorValue(RE::ActorValue::kHealth);
			const float hpMax = (std::max)(1.0f, player->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float pct = (hpNow / hpMax) * 100.0f;
			const float thresh = TFD::Settings::GetDefeatThresholdPct();
			return pct <= thresh;
		}

		static void RefreshPostDefeatGlobals();
		static void SetRescueStateValue(int value);

		static void SetRescueStateValue(int value)
		{
			TFD::Rescue::SetStateValue(value);
		}

		static bool IsActorCloseAndFront(RE::Actor* actor, RE::Actor* player, float maxDist);
		static bool IsBleedSpaceCompatible(RE::Actor* actor, RE::Actor* player);

		static bool IsBleedSpaceCompatible(RE::Actor* actor, RE::Actor* player)
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

		static TFD::Bleedout::SpeakerLogicHandlers BuildBleedoutSpeakerHandlers(bool preserveAssigned = false)
		{
			TFD::Bleedout::SpeakerLogicHandlers handlers{};
			handlers.getPlayer = []() -> RE::Actor* { return Player(); };
			handlers.isStandingEnemyThresholdActor = [](RE::Actor* actor) { return IsStandingEnemyThresholdActor(actor); };
			handlers.isCaptiveSupportedAggressor = [](RE::Actor* actor) { return IsCaptiveSupportedAggressor(actor); };
			handlers.isBleedCrowdSupportedAggressor = [](RE::Actor* actor) { return IsBleedCrowdSupportedAggressor(actor); };
			handlers.isBleedSpaceCompatible = [](RE::Actor* actor, RE::Actor* player) { return IsBleedSpaceCompatible(actor, player); };
			handlers.hasLineOfSightToPlayer = [](RE::Actor* actor, RE::Actor* player) { return ActorHasLineOfSightToPlayer(actor, player); };
			handlers.isActorCloseAndFront = [](RE::Actor* actor, RE::Actor* player, float maxDist) { return IsActorCloseAndFront(actor, player, maxDist); };
			handlers.resolveCurrentCombatTarget = [](RE::Actor* actor) -> RE::Actor* { return ResolveCurrentCombatTarget(actor); };
			handlers.isActiveFollowerActor = [](RE::Actor* actor) { return IsActiveFollowerActor(actor); };
			handlers.resolveLastAggressor = []() -> RE::Actor* {
				if (!g_lastAggressor) {
					return nullptr;
				}
				auto sp = RE::Actor::LookupByHandle(g_lastAggressor.native_handle());
				return sp.get();
				};
			handlers.isPreservedAssigned = [preserveAssigned](RE::Actor* actor) {
				if (!preserveAssigned || !actor) {
					return false;
				}
				return TFD::Bleedout::HasBleedCrowdAssignedID(actor->GetFormID());
				};
			return handlers;
		}


		static TFD::BleedoutDialogueRuntime::Context BuildBleedoutDialogueRuntimeContext()
		{
			TFD::BleedoutDialogueRuntime::Context context{};
			context.getPlayer = []() -> RE::Actor* { return Player(); };
			context.buildSpeakerHandlers = [](bool preserveAssigned) { return BuildBleedoutSpeakerHandlers(preserveAssigned); };
			context.buildRuntimeStateRefs = []() { return TFD::DefeatRuntimeActions::BuildBleedRuntimeHostStateRefs(); };
			context.buildRuntimeHostHandlers = []() { return TFD::Bleedout::RuntimeHost::BuildHandlers(); };
			context.currentSpeakerID = []() { return TFD::Bleedout::GetBleedSpeakerID(); };
			context.ownsCurrentFlow = []() { return TFD::Bleedout::OwnsCurrentFlow(); };
			return context;
		}

		static std::vector<RE::Actor*> CollectBleedoutCrowd(float radius, RE::Actor* preferred, bool preserveAssigned = false)
		{
			return TFD::Bleedout::CollectCrowd(radius, preferred, preserveAssigned, BuildBleedoutSpeakerHandlers(preserveAssigned));
		}


		static void ResetBleedRuntimeState(bool preserveCaptive = false)
		{
			TFD::Bleedout::RuntimeResetHandlers handlers{};
			handlers.releasePlayerBleedLock = [&](const char* reason) { TFD::BleedLockRuntime::ReleasePlayer(BuildBleedLockRuntimeContext(), reason, false); };
			handlers.releaseBleedTruceSession = [&]() { TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::Generic); };
			handlers.releaseNoSpeakerTameSession = [&](const char* reason) { TFD::Tame::ReleaseBleedNoSpeakerTameSession(reason); };
			handlers.clearBridgeAliases = [&](const char* reason) { TFD::DefeatBridge::ClearBleedSupportAliases(reason); };
			handlers.setBleedActive = [&](bool active, const char*) {
				g_inBleedState.store(active, std::memory_order_release);
				if (!active) {
					g_minHp = 0.0f;
				}
				};
			handlers.resetGreetRuntime = [&](const char* reason) { TFD::BleedoutGreet::ResetRuntime(reason); };
			handlers.resetSystemEventState = [&](const char* reason) { TFD::Bleedout::ResetSystemEventState(reason); };
			handlers.clearCaptorAliases = [&](const char* reason) { TFD::Bleedout::ClearCaptorAliases(reason); };
			handlers.clearDialogueOutcome = [&](const char* reason) { TFD::Bleedout::ClearDialogueOutcome(reason); };
			handlers.resetBattleObserveTracking = [&]() {
				g_bleedBattlePreferredEnemy.reset();
				g_bleedBattleObserver = {};
				};
			handlers.resetDialogueRuntimeState = [&]() {
				g_bleedPendingCaptiveOutcome = false;
				g_bleedPendingNonCaptiveOutcome = false;
				g_bleedPaused = false;
				g_bleedPauseStarted = {};
				TFD::Bleedout::ResetBleedSpeakerKick();
				TFD::Bleedout::SetBleedDialogueRetryCount(0);
				g_bleedLastCalmPulse = {};
				TFD::Bleedout::BleedLastCrowdAssignRef() = {};
				TFD::Bleedout::ClearBleedCrowdAssigned();
				TFD::Bleedout::ClearBleedRejectedSpeakerIds();
				g_bleedStart = Now();
				g_bleedLastSeconds = -1;
				};
			handlers.resetBattleObserveState = [&]() {
				g_bleedBattleObservePending = false;
				g_bleedBattleObservePendingUntil = {};
				g_bleedBattleObservePendingLastRedirect = {};
				g_bleedBattleObservePendingEmptyEnemyTicks = 0;
				g_bleedBattleObservePendingEmptyAllyTicks = 0;
				g_bleedBattleObserveActive = false;
				g_bleedBattleObserveSince = {};
				g_bleedBattleObserveLastRedirect = {};
				g_bleedBattleObserveActiveEmptyEnemyTicks = 0;
				g_bleedBattleObserveActiveEmptyAllyTicks = 0;
				};
			handlers.clearEscapeBreakState = [&]() { TFD::Captive::ClearEscapeBreakRebleed(); };
			handlers.clearLastEnemyTargetingPlayer = [&]() { ClearLastEnemyTargetingPlayerInternal(); };
			handlers.clearOutcomeWindow = [&](const char* reason) { TFD::Bleedout::ClearSystemEventOutcomeWindow(reason); };
			handlers.resetPleasureRuntime = [&](const char* reason) { TFD::PleasureRuntime::ResetRuntime(reason); };

			TFD::Bleedout::ResetRuntimeState(preserveCaptive, "reset_bleed_runtime", handlers);
			TFD::PlayerOverkillDamageHook::SetKillmoveGuard(false, "reset_bleed_runtime");
		}

		static void TransitionBleedRuntimeToPleasureCommit(const char* reason)
		{
			TFD::Bleedout::TransitionRuntimeToPleasureCommit(
				reason,
				static_cast<std::uint32_t>(TFD::Bleedout::GetBleedSpeakerID()),
				TFD::Bleedout::GetTruceSessionID() != 0,
				static_cast<std::uint32_t>(TFD::Bleedout::GetActiveCaptorFormID()),
				TFD::Bleedout::RuntimePleasureCommitHandlers{
					[&](const char* why) { TFD::Tame::ReleaseBleedNoSpeakerTameSession(why); },
					[&](const char* why) { TFD::DefeatBridge::ClearBleedSupportAliases(why); },
					[&](bool active, const char*) {
						g_inBleedState.store(active, std::memory_order_release);
						if (!active) {
							g_minHp = 0.0f;
						}
					},
					[&](const char* why) { TFD::BleedoutGreet::ResetRuntime(why); },
					[&]() {
						g_bleedPendingCaptiveOutcome = false;
						g_bleedPendingNonCaptiveOutcome = false;
						g_bleedPaused = false;
						g_bleedPauseStarted = {};
						TFD::Bleedout::ResetBleedSpeakerKick();
						TFD::Bleedout::SetBleedDialogueRetryCount(0);
						g_bleedLastCalmPulse = {};
						TFD::Bleedout::BleedLastCrowdAssignRef() = {};
						TFD::Bleedout::ClearBleedCrowdAssigned();
						TFD::Bleedout::ClearBleedRejectedSpeakerIds();
						g_bleedStart = Now();
						g_bleedLastSeconds = -1;
					},
					[&]() {
						g_bleedBattleObservePending = false;
						g_bleedBattleObservePendingUntil = {};
						g_bleedBattleObservePendingLastRedirect = {};
						g_bleedBattleObservePendingEmptyEnemyTicks = 0;
						g_bleedBattleObservePendingEmptyAllyTicks = 0;
						g_bleedBattleObserveActive = false;
						g_bleedBattleObserveSince = {};
						g_bleedBattleObserveLastRedirect = {};
						g_bleedBattleObserveActiveEmptyEnemyTicks = 0;
						g_bleedBattleObserveActiveEmptyAllyTicks = 0;
						g_bleedBattlePreferredEnemy.reset();
						g_bleedBattleObserver = {};
					},
					[&]() { TFD::Captive::ClearEscapeBreakRebleed(); },
					[&]() { ClearLastEnemyTargetingPlayerInternal(); },
					[&](const char* why) { TFD::Bleedout::ClearSystemEventOutcomeWindow(why); }
				});
		}

		static CaptivePhaseValue PhaseFromRaw(std::uint32_t raw)
		{
			return TFD::Captive::PhaseFromRaw(raw);
		}

		static RE::Actor* ResolveAggressor();
		static RE::Actor* FindBestAggressor(float radius);
		static void SetGraceSeconds(int seconds);
		static bool IsGraceActive();
		static bool IsBleedoutTerminalOutcomeEvent(const char* rawName);
		static double ResolvePostTerminalBleedoutLockoutSeconds(const char* rawName, float numArg);
		static void ArmPostTerminalBleedoutLockout(const char* eventName, double seconds, const char* reason);
		static bool IsPostTerminalBleedoutLockoutActive(const char* checkReason, float hpPct, float thresholdPct);
		static void ClearBleedStickyReopenGrace(const char* reason);
		static bool TickBleedStickyReopenGrace(RE::Actor* player, std::chrono::steady_clock::time_point now);
		static void UpdatePreCombatState();
		static bool IsActorCloseAndFront(RE::Actor* actor, RE::Actor* player, float maxDist);
		static void StartBleedWindow(RE::Actor* player, RE::Actor* aggressor);

		static void StartBleedWindow(RE::Actor* player, RE::Actor* aggressor)
		{
			ClearBleedStickyReopenGrace("start_bleed_window");
			TFD::Bleedout::RuntimeHost::StartWindow(player, aggressor);
		}

		static TFD::Transition::RuntimeHandlers BuildTransitionRuntimeHandlers()
		{
			TFD::Transition::RuntimeHandlers handlers{};
			handlers.getPlayer = []() -> RE::Actor* { return Player(); };
			handlers.resolveAggressor = []() -> RE::Actor* { return ResolveAggressor(); };
			handlers.findBestAggressor = [](float radius) -> RE::Actor* { return FindBestAggressor(radius); };
			handlers.isCombatSupportedAggressor = [](RE::Actor* actor) { return IsCombatSupportedAggressor(actor); };
			handlers.isActiveFollowerActor = [](RE::Actor* actor) { return TFD::TeammateManager::IsActiveFollowerActor(actor); };
			handlers.isStandingAllyThresholdActor = [](RE::Actor* actor) { return IsStandingAllyThresholdActor(actor); };
			handlers.collectRegisteredTeammates = []() { return TFD::TeammateManager::CollectRegisteredTeammates(); };
			handlers.collectBleedoutCrowd = [](float radius, RE::Actor* preferred, bool preserveAssigned) {
				return CollectBleedoutCrowd(radius, preferred, preserveAssigned);
			};
			handlers.getBleedCrowdAssigned = []() { return TFD::Bleedout::GetBleedCrowdAssignedIDs(); };
			handlers.releasePlayerBleedLock = [](const char* reason, bool playGetUp) { TFD::BleedLockRuntime::ReleasePlayer(BuildBleedLockRuntimeContext(), reason, playGetUp); };
			handlers.setGraceSeconds = [](int seconds) { SetGraceSeconds(seconds); };
			handlers.setRescueStateValue = [](int value) { TFD::Rescue::SetStateValue(value); };
			handlers.refreshPostDefeatGlobals = []() { RefreshPostDefeatGlobals(); };
			handlers.updatePreCombatState = []() { UpdatePreCombatState(); };
			return handlers;
		}

		static TFD::DefeatRuntimeActions::NoMarkerFallbackContext BuildNoMarkerFallbackContext()
		{
			TFD::DefeatRuntimeActions::NoMarkerFallbackContext context{};
			context.buildTransitionRuntimeHandlers = []() { return BuildTransitionRuntimeHandlers(); };
			context.clearCaptiveOrchestrationResidue = []() { TFD::Bleedout::DefeatGlue::ClearCaptiveOrchestrationResidue(true); };
			context.getPlayer = []() -> RE::Actor* { return Player(); };
			context.clearBridgeAliases = [](const char* reason) { TFD::Bleedout::ClearBridgeAliases(nullptr, reason); };
			context.setPlayerBleedImmune = [](bool immune) { SetPlayerBleedImmune(immune); };
			context.resetBleedRuntimeState = []() { ResetBleedRuntimeState(); };
			context.clearLastAggressor = []() { g_lastAggressor.reset(); };
			context.updatePreCombatState = []() { UpdatePreCombatState(); };
			return context;
		}

		static bool IsObserverAlly(RE::Actor* actor)
		{
			return TFD::Bleedout::DefeatGlue::IsObserverAlly(actor);
		}

		static bool IsValidBleedBattleEnemyRosterActor(RE::Actor* actor, RE::Actor* player)
		{
			if (!player || !actor || actor == player) {
				return false;
			}
			if (actor->IsPlayerTeammate() ||
				TFD::TeammateManager::IsActiveFollowerActor(actor) ||
				TFD::TeammateManager::IsPlayerSideTeammateActor(actor) ||
				TFD::Tame::IsCompanion(actor)) {
				return false;
			}
			if (!IsStandingEnemyThresholdActor(actor) || IsObserverAlly(actor) || !actor->Is3DLoaded()) {
				return false;
			}
			auto* pCell = player->GetParentCell();
			if (pCell && actor->GetParentCell() != pCell) {
				return false;
			}
			return true;
		}

		static void NoteEnemyTargetingPlayerInternal(RE::Actor* actor)
		{
			if (!actor || !IsCombatSupportedAggressor(actor)) {
				return;
			}
			g_lastEnemyTargetingPlayer = actor->GetHandle();
			g_lastEnemyTargetingPlayerFormID = actor->GetFormID();
			g_lastEnemyTargetingPlayerSeen = Now();
		}

		static void ClearLastEnemyTargetingPlayerInternal()
		{
			g_lastEnemyTargetingPlayer = {};
			g_lastEnemyTargetingPlayerFormID = 0;
			g_lastEnemyTargetingPlayerSeen = {};
		}

		static bool HasRecentEnemyTargetingPlayerInternal(double maxAgeSec)
		{
			if (g_lastEnemyTargetingPlayerFormID == 0 ||
				g_lastEnemyTargetingPlayerSeen == std::chrono::steady_clock::time_point{}) {
				return false;
			}

			const auto age = std::chrono::duration<double>(Now() - g_lastEnemyTargetingPlayerSeen).count();
			return age >= 0.0 && age <= maxAgeSec;
		}

		static RE::Actor* ResolveLastEnemyTargetingPlayerInternal(float radius, double maxAgeSec)
		{
			const auto now = Now();
			if (g_lastEnemyTargetingPlayerFormID != 0 && g_lastEnemyTargetingPlayerSeen != std::chrono::steady_clock::time_point{}) {
				const auto age = std::chrono::duration<double>(now - g_lastEnemyTargetingPlayerSeen).count();
				if (age <= maxAgeSec) {
					auto resolveCached = [&](RE::Actor* actor) -> RE::Actor* {
						float dist = -1.0f;
						if (IsReasonableCombatAggressor(actor, Player(), radius, &dist)) {
							return actor;
						}
						return nullptr;
						};
					if (g_lastEnemyTargetingPlayer) {
						if (auto actor = g_lastEnemyTargetingPlayer.get().get()) {
							if (auto* resolved = resolveCached(actor->As<RE::Actor>()); resolved) {
								return resolved;
							}
						}
					}
					if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(g_lastEnemyTargetingPlayerFormID)) {
						if (auto* resolved = resolveCached(actor); resolved) {
							g_lastEnemyTargetingPlayer = resolved->GetHandle();
							return resolved;
						}
					}
				}
			}

			auto* recent = TFD::PreCombatGreet::ResolveRecentAggressor(radius, maxAgeSec);
			if (recent && IsCombatSupportedAggressor(recent)) {
				NoteEnemyTargetingPlayerInternal(recent);
				return recent;
			}
			return nullptr;
		}

		static RE::Actor* FindBestAggressor(float radius)
		{
			return TFD::Actor::FindBestAggressor(radius, Player());
		}

		static RE::Actor* ResolveAggressor()
		{
			const float radius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 400.0f);
			return TFD::Actor::ResolveAggressor(radius, Player());
		}

		static bool IsActorCloseAndFront(RE::Actor* actor, RE::Actor* player, float maxDist)
		{
			if (!actor || !player) {
				return false;
			}

			const auto playerPos = player->GetPosition();
			const auto actorPos = actor->GetPosition();
			const float dx = actorPos.x - playerPos.x;
			const float dy = actorPos.y - playerPos.y;
			const float d2 = dx * dx + dy * dy;
			if (d2 > (maxDist * maxDist)) {
				return false;
			}
			if (d2 <= 1.0f) {
				return true;
			}

			const float len = std::sqrt(d2);
			const float ang = player->GetAngleZ();
			const float fx = std::sin(ang);
			const float fy = std::cos(ang);
			const float nx = dx / len;
			const float ny = dy / len;
			return (nx * fx + ny * fy) >= 0.20f;
		}

		static bool IsActorBleedingOut(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			if (auto* state = actor->AsActorState()) {
				return state->IsBleedingOut();
			}
			return false;
		}

		static float GetActorHealthPct(RE::Actor* actor)
		{
			if (!actor) {
				return 0.0f;
			}
			const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float hpNow = (std::max)(0.0f, actor->GetActorValue(RE::ActorValue::kHealth));
			return (hpNow / hpMax) * 100.0f;
		}

		static bool HasActiveBleedLock(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			return g_bleedLocks.find(actor->GetFormID()) != g_bleedLocks.end();
		}

		static bool IsActorDownByThreshold(RE::Actor* actor, float thresholdPct)
		{
			if (!actor || actor->IsDisabled() || actor->IsDead() || IsActorBleedingOut(actor)) {
				return true;
			}
			if (HasActiveBleedLock(actor) || TFD::Actor::Ops::IsDefeatedEnemyKnocked(actor)) {
				return true;
			}
			return GetActorHealthPct(actor) <= std::clamp(thresholdPct, 2.0f, 95.0f);
		}

		static bool IsStandingAllyThresholdActor(RE::Actor* actor)
		{
			if (!actor || actor == Player()) {
				return false;
			}
			return !IsActorDownByThreshold(actor, TFD::Settings::GetAllyDownedThresholdPct());
		}

		static bool IsStandingEnemyThresholdActor(RE::Actor* actor)
		{
			if (!actor || actor == Player()) {
				return false;
			}
			return !IsActorDownByThreshold(actor, TFD::Settings::GetEnemyDownedThresholdPct());
		}

		static void MaintainBleedPrimaryCaptorBinding()
		{
			TFD::Bleedout::MaintainPrimaryCaptorBinding(TFD::DialogueLifecycle::IsDialogueOpen(), TFD::DefeatRuntimeActions::BuildBleedRuntimeHostStateRefs());
		}

		// Shared combat-target and player-side actor helpers

		static bool IsActiveFollowerActor(RE::Actor* actor)
		{
			return TFD::TeammateManager::IsActiveFollowerActor(actor);
		}

		static RE::Actor* ResolveCurrentCombatTarget(RE::Actor* actor)
		{
			return TFD::Actor::GetCurrentTarget(actor);
		}

		static bool IsPlayerSideActorForRouter(RE::Actor* actor, RE::Actor* player)
		{
			if (!actor || !player) {
				return false;
			}
			if (actor == player) {
				return true;
			}
			return IsActiveFollowerActor(actor) ||
				TFD::TeammateManager::IsPlayerSideTeammateActor(actor) ||
				TFD::Tame::IsCompanion(actor);
		}

		static bool IsActorActivelyTargetingPlayerSideForRouter(RE::Actor* actor, RE::Actor* player)
		{
			if (!actor || !player) {
				return false;
			}
			auto* currentTarget = ResolveCurrentCombatTarget(actor);
			return IsPlayerSideActorForRouter(currentTarget, player);
		}

		static std::vector<RE::Actor*> CollectLiveStandingObservedEnemies(RE::Actor* player, float radius, RE::Actor* preferredEnemy, const std::vector<RE::Actor*>& allies)
		{
			std::vector<RE::Actor*> out;
			auto enemies = TFD::Bleedout::DefeatGlue::CollectCurrentObservedEnemies(player, radius, preferredEnemy, allies);
			out.reserve(enemies.size());
			for (auto* enemy : enemies) {
				if (enemy && IsValidBleedBattleEnemyRosterActor(enemy, player)) {
					out.push_back(enemy);
				}
			}
			return out;
		}

		// R17: DefeatMonitor is a HP/threshold sensor only.
		// Combat target mutation is owned by TFDCombatBehavior.
		// Keep these legacy hooks as no-op providers because FlowController/
		// Bleedout runtime still pass the function pointer through existing
		// provider structs.
		struct FollowerResolution
		{
			RE::Actor* standing{ nullptr };
			RE::Actor* downed{ nullptr };
		};

		using PlayerThresholdOutcomeScan = TFD::PlayerDownRouter::ThresholdScan;
		using PlayerThresholdOutcomeClassification = TFD::PlayerDownRouter::ThresholdClassification;

		static void RememberAggressorForOutcome(RE::Actor* actor)
		{
			if (actor && !IsObserverAlly(actor)) {
				g_lastAggressor = actor->GetHandle();
			}
		}

		static bool ContainsActorByFormID(const std::vector<RE::Actor*>& actors, RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			const auto formID = actor->GetFormID();
			for (auto* candidate : actors) {
				if (candidate && candidate->GetFormID() == formID) {
					return true;
				}
			}
			return false;
		}

		static bool IsConcreteStandingPlayerSideAlly(RE::Actor* actor, RE::Actor* player)
		{
			if (!actor || !player || actor == player || actor->IsDead() || actor->IsDisabled()) {
				return false;
			}
			if (TFD::Actor::IsDownByHealthThreshold(actor, TFD::Settings::GetAllyDownedThresholdPct())) {
				return false;
			}
			return
				actor->IsPlayerTeammate() ||
				TFD::TeammateManager::IsActiveFollowerActor(actor) ||
				TFD::TeammateManager::IsPlayerSideTeammateActor(actor) ||
				TFD::Tame::IsCompanion(actor);
		}

		static std::vector<RE::Actor*> CollectConcreteStandingPlayerSideAlliesFromSnapshot(
			const TFD::Actor::Snapshot& snapshot,
			RE::Actor* player)
		{
			std::vector<RE::Actor*> out;
			for (auto* actor : TFD::Actor::ResolveStandingPlayerSideActors(snapshot, false)) {
				if (!IsConcreteStandingPlayerSideAlly(actor, player)) {
					continue;
				}
				if (ContainsActorByFormID(out, actor)) {
					continue;
				}
				out.push_back(actor);
			}
			return out;
		}

		static PlayerThresholdOutcomeScan ScanPlayerThresholdOutcome(RE::Actor* player)
		{
			PlayerThresholdOutcomeScan scan{};
			scan.scanRadius = (std::max)(12000.0f, TFD::Settings::GetSweepRadius());
			scan.initialAggressor = ResolveAggressor();
			if (!scan.initialAggressor) {
				scan.initialAggressor = FindBestAggressor(scan.scanRadius);
			}
			scan.coalitionSnapshot = TFD::Actor::BuildSnapshot(scan.scanRadius, false);
			scan.unresolvedBattle = !TFD::Actor::IsConflictResolved(scan.coalitionSnapshot);
			scan.playerSideStanding = TFD::Actor::HasStandingTeammateOnPlayerSide(scan.coalitionSnapshot);
			scan.hostileCoalitionStanding = TFD::Actor::HasStandingHostileCoalition(scan.coalitionSnapshot);
			scan.standingFollowers = CollectBleedStandingFollowers(scan.scanRadius);

			// R10: TeammateManager aliases can be stale/empty at the exact player overkill
			// hook, especially with vanilla followers. The combat snapshot already knows
			// whether a concrete non-player player-side actor is standing. Merge that
			// snapshot ally into the follower guard so enemy Bleedout ForceGreet cannot
			// preempt an active teammate-vs-enemy fight.
			auto snapshotAllies = CollectConcreteStandingPlayerSideAlliesFromSnapshot(scan.coalitionSnapshot, player);
			std::uint32_t merged = 0;
			for (auto* ally : snapshotAllies) {
				if (ContainsActorByFormID(scan.standingFollowers, ally)) {
					continue;
				}
				scan.standingFollowers.push_back(ally);
				++merged;
			}
			if (merged > 0) {
				spdlog::info(
					"[TFD][Defeat][R10] merged concrete player-side allies into bleedout follower guard merged={} total={} playerSideStanding={} hostileStanding={}",
					merged,
					static_cast<unsigned int>(scan.standingFollowers.size()),
					scan.playerSideStanding ? 1 : 0,
					scan.hostileCoalitionStanding ? 1 : 0);
			}
			return scan;
		}

		static PlayerThresholdOutcomeClassification ClassifyPlayerThresholdOutcome(const PlayerThresholdOutcomeScan& scan)
		{
			auto classification = TFD::PlayerDownRouter::ClassifyThresholdOutcome(scan);
			classification.rememberedAggressor =
				(scan.initialAggressor && !IsObserverAlly(scan.initialAggressor)) ? scan.initialAggressor : nullptr;
			return classification;
		}

		static bool TryBeginThresholdNoThreatRescueFallback(
			RE::Actor* player,
			const PlayerThresholdOutcomeScan& scan,
			float threshold,
			const char* reason)
		{
			return TFD::PlayerDownRouter::TryBeginNoThreatRescueFallback(
				player,
				scan,
				threshold,
				reason,
				TFD::PlayerDownRouterWiring::BuildNoThreatHandlers(BuildPlayerDownRouterNoThreatContext()));
		}

		static bool DispatchPlayerThresholdOutcome(
			RE::Actor* player,
			const PlayerThresholdOutcomeScan& scan,
			const PlayerThresholdOutcomeClassification& classification)
		{
			return TFD::PlayerDownRouter::DispatchThresholdOutcome(
				player,
				scan,
				classification,
				TFD::PlayerDownRouterWiring::BuildDispatchHandlers(BuildPlayerDownRouterDispatchContext()));
		}

		static bool DispatchThresholdScanImmediateBleedout(
			RE::Actor* player,
			float playerHpPct,
			float playerThreshold,
			const char* reason)
		{
			if (!player) {
				return false;
			}

			const auto now = Now();
			if (g_thresholdScanImmediateDispatchLast.time_since_epoch().count() != 0 &&
				(now - g_thresholdScanImmediateDispatchLast) < std::chrono::milliseconds(900)) {
				spdlog::info(
					"[TFD][Defeat][R270A] threshold scan immediate bleedout skipped reason=recent_dispatch hpPct={:.1f} threshold={:.1f}",
					playerHpPct,
					std::clamp(playerThreshold, 2.0f, 95.0f));
				return false;
			}

			auto& flow = TFD::FlowController::Controller::GetSingleton();
			const bool flowDecisionActive = flow.IsBleedDecisionActive();
			const auto flowSnapshot = flow.GetSnapshot();
			const bool staleEscapeBreakBleedRuntime =
				g_inBleedState.load(std::memory_order_acquire) &&
				!flowDecisionActive &&
				flowSnapshot.root == TFD::FlowController::RootFlow::InCombat &&
				flowSnapshot.contextRoot == TFD::FlowController::RootFlow::Captive &&
				flowSnapshot.sub == TFD::FlowController::SubFlow::InCombatEscapeBreak;

			if (flowDecisionActive) {
				spdlog::info(
					"[TFD][Defeat][R270A] threshold scan immediate bleedout skipped reason=bleed_decision_active hpPct={:.1f} threshold={:.1f}",
					playerHpPct,
					std::clamp(playerThreshold, 2.0f, 95.0f));
				return false;
			}

			if (g_inBleedState.load(std::memory_order_acquire) && !staleEscapeBreakBleedRuntime) {
				spdlog::info(
					"[TFD][Defeat][R270A] threshold scan immediate bleedout skipped reason=bleed_runtime_active root={} ctx={} sub={} hpPct={:.1f} threshold={:.1f}",
					TFD::FlowController::Controller::ToString(flowSnapshot.root),
					TFD::FlowController::Controller::ToString(flowSnapshot.contextRoot),
					TFD::FlowController::Controller::ToString(flowSnapshot.sub),
					playerHpPct,
					std::clamp(playerThreshold, 2.0f, 95.0f));
				return false;
			}

			if (staleEscapeBreakBleedRuntime) {
				g_inBleedState.store(false, std::memory_order_release);
				g_minHp = 0.0f;
				TFD::BleedoutGreet::ResetRuntime("r270_stale_escape_break_rebleed");
				TFD::Bleedout::ClearDialogueOutcome("r270_stale_escape_break_rebleed");
				spdlog::info(
					"[TFD][Defeat][R270A] stale escape-break bleed runtime released root={} ctx={} sub={} primary={:08X}",
					TFD::FlowController::Controller::ToString(flowSnapshot.root),
					TFD::FlowController::Controller::ToString(flowSnapshot.contextRoot),
					TFD::FlowController::Controller::ToString(flowSnapshot.sub),
					flowSnapshot.primaryActorFormID);
			}

			auto thresholdScan = ScanPlayerThresholdOutcome(player);
			const bool immediateThreat = player->IsInCombat() ||
				thresholdScan.initialAggressor != nullptr ||
				thresholdScan.hostileCoalitionStanding;
			if (!immediateThreat) {
				if (TryBeginThresholdNoThreatRescueFallback(player, thresholdScan, playerThreshold, reason ? reason : "threshold_scan_no_threat")) {
					g_thresholdScanImmediateDispatchLast = now;
					return true;
				}
				spdlog::info(
					"[TFD][Defeat][R270A] threshold scan immediate bleedout skipped reason=no_immediate_threat hpPct={:.1f} threshold={:.1f}",
					playerHpPct,
					std::clamp(playerThreshold, 2.0f, 95.0f));
				return false;
			}

			auto thresholdClassification = ClassifyPlayerThresholdOutcome(thresholdScan);
			const bool dispatched = DispatchPlayerThresholdOutcome(player, thresholdScan, thresholdClassification);
			spdlog::info(
				"[TFD][Defeat][R270A] threshold scan immediate bleedout dispatch dispatched={} staleEscapeBreak={} hpPct={:.1f} threshold={:.1f} root={} ctx={} sub={} speaker={:08X}",
				dispatched ? 1 : 0,
				staleEscapeBreakBleedRuntime ? 1 : 0,
				playerHpPct,
				std::clamp(playerThreshold, 2.0f, 95.0f),
				TFD::FlowController::Controller::ToString(flowSnapshot.root),
				TFD::FlowController::Controller::ToString(flowSnapshot.contextRoot),
				TFD::FlowController::Controller::ToString(flowSnapshot.sub),
				thresholdClassification.rememberedAggressor ? thresholdClassification.rememberedAggressor->GetFormID() : 0u);
			if (dispatched) {
				g_thresholdScanImmediateDispatchLast = now;
			}
			return dispatched;
		}

		static FollowerResolution ResolveFollowerCandidates(float radius)
		{
			auto external = TFD::TeammateManager::ResolveFollowerCandidates(radius);
			FollowerResolution result{};
			result.standing = external.standing;
			result.downed = external.downed;
			return result;
		}

		static std::vector<RE::Actor*> CollectBleedStandingFollowers(float radius)
		{
			return TFD::TeammateManager::CollectStandingFollowers(radius);
		}

		static bool IsObservedEnemyTarget(RE::Actor* target, const std::vector<RE::Actor*>& enemies)
		{
			if (!target) {
				return false;
			}

			for (auto* enemy : enemies) {
				if (enemy && enemy == target) {
					return true;
				}
			}
			return false;
		}

		static bool HasRealCombatProof(RE::Actor* player, const std::vector<RE::Actor*>& enemies)
		{
			if (!player) {
				return false;
			}

			auto& flow = TFD::FlowController::Controller::GetSingleton();
			if (flow.IsCombatOrBleedRootActive()) {
				return true;
			}
			if (flow.IsCaptiveEscapeContextActive()) {
				return true;
			}

			// Real combat proof must come from actual combat state or a live combat target.
			// Observed enemies alone are only threat/precombat data and must not flip
			// dialogue-facing combat/defeat globals to 1.
			if (player->IsInCombat()) {
				return true;
			}

			if (IsObservedEnemyTarget(ResolveCurrentCombatTarget(player), enemies)) {
				return true;
			}

			for (auto* enemy : enemies) {
				if (!enemy) {
					continue;
				}

				if (IsActorActivelyTargetingPlayerSideForRouter(enemy, player)) {
					return true;
				}
			}

			return false;
		}


		static bool IsDefeatCombatContextActive(RE::Actor* player, const std::vector<RE::Actor*>& enemies)
		{
			if (HasRealCombatProof(player, enemies)) {
				return true;
			}
			if (g_inBleedState.load(std::memory_order_acquire)) {
				return true;
			}
			if (g_bleedBattleObservePending || g_bleedBattleObserveActive) {
				return true;
			}
			return false;
		}

		static bool IsRouterCombatContextActive(RE::Actor* player, const std::vector<RE::Actor*>& enemies)
		{
			if (!player) {
				return false;
			}

			for (auto* enemy : enemies) {
				if (IsActorActivelyTargetingPlayerSideForRouter(enemy, player)) {
					return true;
				}
			}

			return false;
		}

		static std::vector<RE::Actor*> FilterTemporarilySuppressedEnemiesForDialogueGlobals(
			const std::vector<RE::Actor*>& enemies,
			bool* onlySuppressedEnemies,
			std::size_t* suppressedCount)
		{
			std::vector<RE::Actor*> filtered;
			filtered.reserve(enemies.size());

			std::size_t localSuppressedCount = 0;
			for (auto* enemy : enemies) {
				if (!enemy) {
					continue;
				}
				if (TFD::HostilityController::IsActorTemporarilySuppressed(enemy)) {
					++localSuppressedCount;
					continue;
				}
				filtered.push_back(enemy);
			}

			if (onlySuppressedEnemies) {
				*onlySuppressedEnemies = !enemies.empty() && filtered.empty() && localSuppressedCount > 0;
			}
			if (suppressedCount) {
				*suppressedCount = localSuppressedCount;
			}
			return filtered;
		}

		static void RefreshPostDefeatGlobals()
		{
			auto* player = Player();
			const float radius = (std::max)(2200.0f, TFD::Settings::GetSweepRadius() + 200.0f);
			auto followers = CollectBleedStandingFollowers(radius);
			auto observedEnemies = CollectLiveStandingObservedEnemies(player, radius, nullptr, followers);
			bool onlySuppressedDialogueEnemies = false;
			std::size_t suppressedEnemyCount = 0;
			auto enemies = FilterTemporarilySuppressedEnemiesForDialogueGlobals(
				observedEnemies,
				&onlySuppressedDialogueEnemies,
				&suppressedEnemyCount);
			const bool defeatContext = IsDefeatCombatContextActive(player, enemies);
			const bool routerCombatContext = IsRouterCombatContextActive(player, enemies);
			const bool pleasurePassiveLock = TFD::PleasureRuntime::IsPassiveLockActive();
			const bool battleObserveHold = g_bleedBattleObservePending || g_bleedBattleObserveActive;
			const auto flowSnapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
			const bool playerBleedLockActive = [&]() {
				if (!player) {
					return false;
				}
				auto it = g_bleedLocks.find(player->GetFormID());
				return it != g_bleedLocks.end() && it->second.kind == BleedLockKind::Player;
			}();
			const bool flowOwnsPlayerBleedout =
				flowSnapshot.root == TFD::FlowController::RootFlow::Bleedout ||
				flowSnapshot.gate == TFD::FlowController::DecisionGate::PlayerBleedout ||
				g_inBleedState.load(std::memory_order_acquire) ||
				playerBleedLockActive;
			auto refreshResult = TFD::PostDefeatState::Refresh(TFD::PostDefeatState::RefreshInput{
				.player = player,
				.enemies = std::move(enemies),
				.defeatContext = defeatContext,
				.routerCombatContext = routerCombatContext,
				.onlySuppressedDialogueEnemies = onlySuppressedDialogueEnemies,
				.suppressedEnemyCount = suppressedEnemyCount,
				.pleasurePassiveLock = pleasurePassiveLock,
				.battleObserveHold = battleObserveHold,
				.forcePlayerBleedout = flowOwnsPlayerBleedout,
				.playerBleedLockActive = playerBleedLockActive,
				.playerBleedRuntimeActive = g_inBleedState.load(std::memory_order_acquire),
				.ownerRootName = TFD::FlowController::Controller::ToString(flowSnapshot.root),
				.ownerGateName = TFD::FlowController::Controller::ToString(flowSnapshot.gate)
				});
			g_lastRouterCombatContextActive = refreshResult.routerCombatContextActive;
		}

		static void UpdatePreCombatState()
		{
			// ownership moved to TFDFlowController.cpp
		}


		static RE::Actor* ResolveAggressor();
		static RE::Actor* FindBestAggressor(float radius);

		static TFD::Captive::EscapeTickHandlers BuildCaptiveEscapeTickHandlers()
		{
			TFD::Captive::EscapeTickHandlers handlers{};
			handlers.updatePreCombatState = []() { UpdatePreCombatState(); };
			handlers.setPrevDialogueOpen = [](bool open) { TFD::DialogueLifecycle::SetDialogueOpenObserved(open); };
			handlers.clearLastAggressor = []() { g_lastAggressor.reset(); };
			handlers.setLastAggressor = [](RE::Actor* actor) { g_lastAggressor = actor ? actor->GetHandle() : RE::ActorHandle{}; };
			handlers.resolveAggressor = []() -> RE::Actor* { return ResolveAggressor(); };
			handlers.findBestAggressor = [](float radius) -> RE::Actor* { return FindBestAggressor(radius); };
			handlers.setGraceActive = [](bool active) { g_grace.store(active, std::memory_order_release); };
			return handlers;
		}

		static TFD::Captive::RuntimeTickHandlers BuildCaptiveRuntimeTickHandlers()
		{
			TFD::Captive::RuntimeTickHandlers handlers{};
			handlers.isDialogueOpen = []() -> bool { return TFD::DialogueLifecycle::IsDialogueOpen(); };
			handlers.getPrevDialogueOpen = []() -> bool { return TFD::DialogueLifecycle::WasDialogueOpen(); };
			handlers.setPrevDialogueOpen = [](bool open) { TFD::DialogueLifecycle::SetDialogueOpenObserved(open); };
			handlers.escape = BuildCaptiveEscapeTickHandlers();
			return handlers;
		}

		static void ClampHealth(RE::Actor* actor, float minHp)
		{
			if (!actor) return;
			const float hp = actor->GetActorValue(RE::ActorValue::kHealth);
			if (hp < minHp) {
				actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, (minHp - hp));
			}
		}

		static void ClampHealthCeiling(RE::Actor* actor, float maxHp)
		{
			if (!actor) {
				return;
			}
			const float hp = actor->GetActorValue(RE::ActorValue::kHealth);
			if (hp > maxHp + 0.001f) {
				actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, -(hp - maxHp));
			}
		}

		static float ResolveActorHealthForPct(RE::Actor* actor, float pct)
		{
			if (!actor) {
				return 0.0f;
			}
			const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
			return hpMax * std::clamp(pct / 100.0f, 0.0f, 1.0f);
		}

		static bool HasPlayerBleedLock()
		{
			return TFD::BleedLockRuntime::HasPlayer(BuildBleedLockRuntimeContext());
		}

		static float ResolvePlayerBleedRuntimeSafeHealth(RE::Actor* actor, float thresholdPct)
		{
			if (!actor) {
				return 1.0f;
			}
			const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
			// Keep enough real HP under the hard-invuln flags to absorb oversized hits.
			// TFD state, not low raw HP, owns player defeat once the bleed runtime starts.
			const float safePct = std::clamp((std::max)(70.0f, thresholdPct + 45.0f), 35.0f, 95.0f);
			return (std::max)(1.0f, hpMax * (safePct / 100.0f));
		}


		static TFD::PlayerDownRouterWiring::DispatchContext BuildPlayerDownRouterDispatchContext()
		{
			TFD::PlayerDownRouterWiring::DispatchContext context{};
			context.rememberAggressor = [](RE::Actor* actor) { RememberAggressorForOutcome(actor); };
			context.findBleedoutSpeaker = [](float scanRadius, float maxDistance, RE::Actor* preferred) -> RE::Actor* {
				return TFD::BleedoutDialogueRuntime::FindBestSpeaker(scanRadius, maxDistance, preferred, BuildBleedoutDialogueRuntimeContext());
			};
			context.startBleedWindow = [](RE::Actor* player, RE::Actor* speaker) { StartBleedWindow(player, speaker); };
			context.startBattleObservePending = [](RE::Actor* player) -> bool { return TFD::Bleedout::RuntimeHost::StartBattleObservePending(player); };
			context.setGraceSeconds = [](int seconds) { SetGraceSeconds(seconds); };
			return context;
		}

		static TFD::PlayerDownRouterWiring::NoThreatContext BuildPlayerDownRouterNoThreatContext()
		{
			TFD::PlayerDownRouterWiring::NoThreatContext context{};
			context.hasTerminalCommit = []() -> bool { return TFD::Bleedout::HasTerminalCommit(); };
			context.hasCachedRescueDestination = []() -> bool { return TFD::Location::ResolveMostRecentCachedRescueDestination(true) != nullptr; };
			context.hasPlayerBleedLock = []() -> bool { return HasPlayerBleedLock(); };
			context.enterPlayerBleedLock = [](RE::Actor* player, float thresholdPct, const char* reason) { TFD::BleedLockRuntime::Enter(BuildBleedLockRuntimeContext(), player, BleedLockKind::Player, thresholdPct, reason); };
			context.releasePlayerBleedLock = [](const char* reason, bool playGetUp) { TFD::BleedLockRuntime::ReleasePlayer(BuildBleedLockRuntimeContext(), reason, playGetUp); };
			context.setPlayerBleedImmune = [](bool immune) { SetPlayerBleedImmune(immune); };
			context.preparePlayer = [](RE::Actor* player) {
				if (player) {
					player->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
				}
			};
			context.executeNoMarkerFallback = [](const char* reason) -> bool {
				return TFD::DefeatRuntimeActions::ExecuteResolvedNoMarkerFallback(reason ? reason : "threshold_no_threat_rescue", BuildNoMarkerFallbackContext());
			};
			return context;
		}

		static TFD::PlayerDownRouterWiring::PendingContext BuildPlayerDownRouterPendingContext()
		{
			TFD::PlayerDownRouterWiring::PendingContext context{};
			context.setPlayerBleedImmune = [](bool immune) { SetPlayerBleedImmune(immune); };
			context.preparePlayer = [](RE::Actor* player) {
				if (!player) {
					return;
				}
				player->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
				if (player->IsDead(false)) {
					player->Resurrect(false, true);
				}
			};
			context.clampHealth = [](RE::Actor* player, float minHealth) { ClampHealth(player, minHealth); };
			context.resolveSafeFloorHealth = [](RE::Actor* player, float thresholdPct) -> float { return TFD::PlayerOverkillDamageHook::ResolveSafeFloorHealth(player, thresholdPct); };
			context.hasPlayerBleedOwner = []() -> bool { return HasPlayerBleedLock() || g_inBleedState.load(std::memory_order_acquire); };
			context.enterPlayerBleedLock = [](RE::Actor* player, float thresholdPct, const char* reason) { TFD::BleedLockRuntime::Enter(BuildBleedLockRuntimeContext(), player, BleedLockKind::Player, thresholdPct, reason); };
			context.isBleedDecisionActive = []() -> bool { return TFD::FlowController::Controller::GetSingleton().IsBleedDecisionActive(); };
			context.scanThresholdOutcome = [](RE::Actor* player) -> TFD::PlayerDownRouter::ThresholdScan { return ScanPlayerThresholdOutcome(player); };
			context.isObserverAlly = [](RE::Actor* actor) -> bool { return IsObserverAlly(actor); };
			context.rememberAggressor = [](RE::Actor* actor) { RememberAggressorForOutcome(actor); };
			context.getHealthPct = [](RE::Actor* actor) -> float { return GetActorHealthPct(actor); };
			context.dispatchContext = BuildPlayerDownRouterDispatchContext();
			return context;
		}

		static RE::Actor* ResolveCachedPlayerOverkillAttacker()
		{
			auto* player = Player();
			auto valid = [player](RE::Actor* actor) -> bool {
				return actor && actor != player && !actor->IsDead() && !actor->IsDisabled() && !IsObserverAlly(actor);
				};

			if (g_lastAggressor) {
				auto sp = RE::Actor::LookupByHandle(g_lastAggressor.native_handle());
				auto* actor = sp.get();
				if (valid(actor)) {
					return actor;
				}
			}

			if (g_lastEnemyTargetingPlayerFormID != 0) {
				if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(g_lastEnemyTargetingPlayerFormID); valid(actor)) {
					return actor;
				}
			}

			return nullptr;
		}

		static void TickPlayerOverkillBlockPending()

		{
			if (!TFD::PlayerDownRouter::HasPendingOverkillRoute()) {
				return;
			}
			(void)TFD::PlayerDownRouter::TickPendingOverkillRoute(Player(), TFD::PlayerDownRouterWiring::BuildPendingHandlers(BuildPlayerDownRouterPendingContext()));
		}


		static void ForcePlayerBleedAlive(RE::Actor* actor, BleedLockEntry& entry, const char* reason)
		{
			if (!actor) {
				return;
			}

			SetPlayerBleedImmune(true);
			auto& boolFlags = actor->GetActorRuntimeData().boolFlags;
			const bool wasDead = actor->IsDead(false);
			const bool wasKillMove = boolFlags.all(RE::Actor::BOOL_FLAGS::kIsInKillMove);

			if (wasDead) {
				actor->Resurrect(false, true);
			}

			// R19: hard essential/protected/no-bleedout flags are owned by PlayerDamageGuard.
			// DefeatMonitor only clears the killmove flag after requesting the guard.
			boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);

			const float hardFloorHp = (std::max)(1.0f, entry.minHp);
			ClampHealth(actor, hardFloorHp);
			float hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
			if (hpNow + 0.001f < hardFloorHp) {
				actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, hardFloorHp - hpNow);
				hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
			}
			entry.protectedHealth = (std::max)(entry.protectedHealth, hpNow);
			entry.lastHealthSample = hpNow;

			actor->NotifyAnimationGraph("BleedoutStart");
			// R19: package evaluation is not owned by DefeatMonitor.

			spdlog::warn("[TFD][Defeat] player bleed anti-death reassert actor={:08X} reason={} resurrected={} killmove={}",
				actor->GetFormID(),
				reason ? reason : "unknown",
				wasDead ? 1 : 0,
				wasKillMove ? 1 : 0);
		}

		static void EnforcePlayerBleedInvulnerability(RE::Actor* actor, BleedLockEntry& entry)
		{
			if (!actor) {
				return;
			}

			if (actor->IsDead(false) || actor->GetActorRuntimeData().boolFlags.all(RE::Actor::BOOL_FLAGS::kIsInKillMove)) {
				ForcePlayerBleedAlive(actor, entry, actor->IsDead(false) ? "dead_state" : "killmove_state");
			}

			SetPlayerBleedImmune(true);

			entry.minHp = (std::max)(entry.minHp, (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth)) * 0.05f);
			const auto guardTick = TFD::PlayerDamageGuard::Tick(
				actor,
				BuildPlayerDamageGuardConfig(actor, entry.thresholdPct, entry.protectedHealth, entry.minHp));
			if (guardTick.active) {
				entry.protectedHealth = (std::max)(entry.protectedHealth, guardTick.protectedHp);
				entry.lastHealthSample = (std::max)(entry.lastHealthSample, guardTick.actualHpAfter);
			}
			entry.protectedHealth = (std::max)(entry.protectedHealth, ResolvePlayerBleedRuntimeSafeHealth(actor, entry.thresholdPct));
			g_minHp = (std::max)(g_minHp, entry.protectedHealth);

			const float hardFloorHp = (std::max)(1.0f, entry.minHp);
			const float protectedHp = (std::max)(entry.protectedHealth, hardFloorHp);
			ClampHealth(actor, protectedHp);

			float hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
			if (hpNow + 0.001f < protectedHp) {
				const float delta = protectedHp - hpNow;
				actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, delta);
				const auto now = Now();
				if (entry.lastDamageLog.time_since_epoch().count() == 0 || (now - entry.lastDamageLog) >= std::chrono::milliseconds(150)) {
					spdlog::info("[TFD][Defeat] player bleed zeroed health damage actor={:08X} from={:.2f} restore={:.2f} target={:.2f}",
						actor->GetFormID(),
						hpNow,
						delta,
						protectedHp);
					entry.lastDamageLog = now;
				}
				hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
			}

			if (hpNow > entry.protectedHealth + 0.001f) {
				entry.protectedHealth = hpNow;
			}

			entry.lastHealthSample = hpNow;
		}

		static void ApplyBleedRegenOverride(RE::Actor* actor, BleedLockEntry& entry)
		{
			auto* avo = actor ? actor->AsActorValueOwner() : nullptr;
			if (!avo) {
				return;
			}

			if (!entry.regenOverridden) {
				entry.savedHealRate = avo->GetActorValue(RE::ActorValue::kHealRate);
				entry.savedHealRateMult = avo->GetActorValue(RE::ActorValue::kHealRateMult);
				entry.savedCombatHealRateMult = avo->GetActorValue(RE::ActorValue::kCombatHealthRegenMultiply);
				entry.regenOverridden = true;
			}

			avo->SetActorValue(RE::ActorValue::kHealRate, 0.0f);
			avo->SetActorValue(RE::ActorValue::kHealRateMult, 0.0f);
			avo->SetActorValue(RE::ActorValue::kCombatHealthRegenMultiply, 0.0f);
		}

		static void RestoreBleedRegenOverride(RE::Actor* actor, const BleedLockEntry& entry)
		{
			if (!entry.regenOverridden) {
				return;
			}

			auto* avo = actor ? actor->AsActorValueOwner() : nullptr;
			if (!avo) {
				return;
			}

			avo->SetActorValue(RE::ActorValue::kHealRate, entry.savedHealRate);
			avo->SetActorValue(RE::ActorValue::kHealRateMult, entry.savedHealRateMult);
			avo->SetActorValue(RE::ActorValue::kCombatHealthRegenMultiply, entry.savedCombatHealRateMult);
		}

		static void RestoreActorHealthToSafePct(RE::Actor* actor, float thresholdPct, float bonusPct, float minSafePct, float maxSafePct, float minAbsHp, const char* reason)
		{
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				return;
			}
			const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
			auto pctToUnit = [](float v) { return v > 1.0f ? (v / 100.0f) : v; };
			const float thresh = std::clamp(pctToUnit(thresholdPct), 0.05f, 0.95f);
			const float bonus = std::clamp(pctToUnit(bonusPct), 0.0f, 0.95f);
			const float minSafe = std::clamp(pctToUnit(minSafePct), 0.05f, 1.0f);
			const float maxSafe = std::clamp(pctToUnit(maxSafePct), minSafe, 1.0f);
			const float safePct = std::clamp(thresh + bonus, minSafe, maxSafe);
			const float target = (std::max)(minAbsHp, hpMax * safePct);
			if (hpNow + 0.001f < target) {
				actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, (target - hpNow));
				spdlog::info("[TFD][Defeat] recover actor hp actor={:08X} reason={} from={:.2f} to={:.2f} threshPct={:.1f}",
					actor->GetFormID(),
					reason ? reason : "unknown",
					hpNow,
					target,
					thresholdPct);
			}
		}

		static float ResolveBleedLockThresholdPct(RE::Actor* actor)
		{
			if (!actor) {
				return 0.0f;
			}
			if (actor == Player()) {
				return TFD::Settings::GetDefeatThresholdPct();
			}
			if (IsActiveFollowerActor(actor)) {
				return TFD::Settings::GetAllyDownedThresholdPct();
			}
			return TFD::Settings::GetEnemyDownedThresholdPct();
		}


		static TFD::PlayerDamageGuard::Config BuildPlayerDamageGuardConfig(RE::Actor* actor, float thresholdPct, float protectedHp, float safeFloorHp)
		{
			TFD::PlayerDamageGuard::Config config{};
			config.thresholdPct = std::clamp(thresholdPct, 2.0f, 95.0f);
			config.protectedHp = (std::max)(1.0f, protectedHp);
			config.safeFloorHp = (std::max)(1.0f, safeFloorHp);
			if (actor) {
				const float maxHp = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
				config.protectedHp = std::clamp(config.protectedHp, 1.0f, maxHp);
				config.safeFloorHp = std::clamp(config.safeFloorHp, 1.0f, config.protectedHp);
			}
			return config;
		}

		static bool HasImmediatePlayerSideBleedThreat(RE::Actor* player, const TFD::Actor::Snapshot& snapshot, float radius)
		{
			if (!player) {
				return false;
			}
			if (player->IsInCombat()) {
				return true;
			}
			if (ResolveAggressor()) {
				return true;
			}
			if (FindBestAggressor(radius)) {
				return true;
			}
			return TFD::Actor::HasStandingHostileCoalition(snapshot);
		}

		static bool ShouldEnterPlayerOrAllyBleedLock(RE::Actor* actor, float thresholdPct, bool hasRelevantThreat)
		{
			if (!actor || actor->IsDisabled() || actor->IsDead()) {
				return false;
			}
			if (GetActorHealthPct(actor) > std::clamp(thresholdPct, 2.0f, 95.0f)) {
				return false;
			}
			if (actor == Player() ||
				IsActiveFollowerActor(actor) ||
				TFD::TeammateManager::IsPlayerSideTeammateActor(actor)) {
				return hasRelevantThreat;
			}
			return false;
		}


		static void ScanBleedLockCandidates()
		{
			auto* player = Player();
			if (!player) {
				return;
			}

			const float radius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 600.0f);
			auto snapshot = TFD::Actor::BuildSnapshot(radius, false);
			const bool playerSideThreat = HasImmediatePlayerSideBleedThreat(player, snapshot, radius);

			const float playerThreshold = ResolveBleedLockThresholdPct(player);
			const float playerHpPct = GetActorHealthPct(player);
			const bool terminalLockout = playerHpPct <= std::clamp(playerThreshold, 2.0f, 95.0f) &&
				IsPostTerminalBleedoutLockoutActive("threshold_scan_player", playerHpPct, std::clamp(playerThreshold, 2.0f, 95.0f));

			const float preDeathArmPct = TFD::PlayerOverkillDamageHook::ResolvePreDeathArmPct(playerThreshold);
			if (!terminalLockout && playerSideThreat && playerHpPct <= preDeathArmPct && playerHpPct > std::clamp(playerThreshold, 2.0f, 95.0f)) {
				// R448A: rollback the rejected pseudo damage guard.  Pre-death arm remains
				// the commit point, but no virtual HP module or large pseudo buffer is used.
				// Enter the protected Player Bleedout runtime immediately while the player
				// is still alive, then route the normal threshold outcome.
				TFD::PlayerOverkillDamageHook::SetPreDeathShieldActive(true, player, playerHpPct, playerThreshold, "r448a_predeath_arm_commit");
				SetPlayerBleedImmune(true);
				player->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
				TFD::BleedLockRuntime::Enter(BuildBleedLockRuntimeContext(), player, BleedLockKind::Player, playerThreshold, "r448a_predeath_arm_commit");
				const bool dispatched = DispatchThresholdScanImmediateBleedout(player, playerHpPct, playerThreshold, "r448a_predeath_arm_commit");
				spdlog::warn(
					"[TFD][Defeat][R448A] pre-death arm committed to bleedout dispatched={} hpPctBefore={:.1f} hpPctAfter={:.1f} threshold={:.1f} armPct={:.1f} playerSideThreat={}",
					dispatched ? 1 : 0,
					playerHpPct,
					GetActorHealthPct(player),
					std::clamp(playerThreshold, 2.0f, 95.0f),
					preDeathArmPct,
					playerSideThreat ? 1 : 0);
				return;
			}
			else if (!HasPlayerBleedLock() && !g_inBleedState.load(std::memory_order_acquire) &&
				(TFD::PlayerOverkillDamageHook::IsPreDeathShieldActive() && (terminalLockout || !playerSideThreat || playerHpPct > std::clamp(preDeathArmPct + 8.0f, 6.0f, 99.0f)))) {
				TFD::PlayerOverkillDamageHook::SetPreDeathShieldActive(false, player, playerHpPct, playerThreshold, terminalLockout ? "terminal_lockout" : (!playerSideThreat ? "no_immediate_threat" : "hp_recovered"));
			}

			if (!terminalLockout && ShouldEnterPlayerOrAllyBleedLock(player, playerThreshold, playerSideThreat)) {
				TFD::BleedLockRuntime::Enter(BuildBleedLockRuntimeContext(), player, BleedLockKind::Player, playerThreshold, "threshold_scan_player");
				if (DispatchThresholdScanImmediateBleedout(player, playerHpPct, playerThreshold, "threshold_scan_player_immediate")) {
					return;
				}
			}
			else if (!terminalLockout && playerHpPct <= std::clamp(playerThreshold, 2.0f, 95.0f) && !playerSideThreat) {
				auto thresholdScan = ScanPlayerThresholdOutcome(player);
				if (TryBeginThresholdNoThreatRescueFallback(player, thresholdScan, playerThreshold, "threshold_scan_player_no_threat")) {
					return;
				}
				spdlog::info("[TFD][Defeat] skip bleed lock player reason=no_immediate_threat hpPct={:.1f} threshold={:.1f}",
					playerHpPct,
					std::clamp(playerThreshold, 2.0f, 95.0f));
			}

			for (auto* actor : TFD::TeammateManager::CollectKnownTeammates(radius)) {
				const float threshold = ResolveBleedLockThresholdPct(actor);
				const bool followerThreat = playerSideThreat || (actor && actor->IsInCombat());
				if (ShouldEnterPlayerOrAllyBleedLock(actor, threshold, followerThreat)) {
					TFD::BleedLockRuntime::Enter(BuildBleedLockRuntimeContext(), actor, BleedLockKind::Ally, threshold, "threshold_scan_follower");
				}
			}

			for (const auto& info : snapshot.actors) {
				auto* actor = info.get();
				if (!actor || actor == player || actor->IsDisabled() || actor->IsDead()) {
					continue;
				}
				if (!TFD::TeammateManager::IsPlayerSideTeammateActor(actor) || IsActiveFollowerActor(actor)) {
					continue;
				}
				const float threshold = TFD::Settings::GetAllyDownedThresholdPct();
				const bool followerThreat = playerSideThreat || actor->IsInCombat();
				if (ShouldEnterPlayerOrAllyBleedLock(actor, threshold, followerThreat)) {
					TFD::BleedLockRuntime::Enter(BuildBleedLockRuntimeContext(), actor, BleedLockKind::Ally, threshold, "threshold_scan_follower_loose");
				}
			}
		}

		static void ScanEnemyThresholdCandidatesOnly(const char* reason)
		{
			auto* player = Player();
			if (!player) {
				return;
			}

			const float radius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 600.0f);
			auto snapshot = TFD::Actor::BuildSnapshot(radius, false);
			const float threshold = std::clamp(TFD::Settings::GetEnemyDownedThresholdPct(), 2.0f, 95.0f);

			for (const auto& info : snapshot.actors) {
				auto* actor = info.get();
				if (!actor || actor == player || actor->IsDisabled() || actor->IsDead()) {
					continue;
				}
				if (TFD::TeammateManager::IsPlayerSideTeammateActor(actor) || IsActiveFollowerActor(actor)) {
					continue;
				}
				if (GetActorHealthPct(actor) > threshold) {
					continue;
				}
				// Sensor ownership stops at the HP crossing. Victory performs the
				// hostile/candidate validation and deduplicates repeated notices.
				(void)TFD::Victory::NotifyEnemyBelowThreshold(
					actor,
					threshold,
					reason && reason[0] ? reason : "enemy_threshold_sensor");
			}
		}

		static void TickEnemyThresholdSensor(const char* reason)
		{
			const auto now = Now();
			if (g_enemyThresholdScanLast.time_since_epoch().count() != 0 &&
				(now - g_enemyThresholdScanLast) < std::chrono::milliseconds(250)) {
				return;
			}
			g_enemyThresholdScanLast = now;
			ScanEnemyThresholdCandidatesOnly(reason && reason[0] ? reason : "enemy_threshold_sensor");
		}


		static TFD::BleedLockRuntime::Context BuildBleedLockRuntimeContext()
		{
			TFD::BleedLockRuntime::Context context{};
			context.getPlayer = []() -> RE::Actor* { return Player(); };
			context.resolveAggressor = []() -> RE::Actor* { return ResolveAggressor(); };
			context.setPlayerBleedImmune = [](bool enable) { SetPlayerBleedImmune(enable); };
			context.getMinHp = []() -> float { return g_minHp; };
			context.setMinHp = [](float value) { g_minHp = value; };
			context.maxMinHp = [](float value) { g_minHp = (std::max)(g_minHp, value); };
			context.resetPreDeathShield = []() { TFD::PlayerOverkillDamageHook::ClearPreDeathShield("bleed_lock_reset", false); };
			context.isInBleedState = []() -> bool { return g_inBleedState.load(std::memory_order_acquire); };
			context.resolvePlayerBleedRuntimeSafeHealth = [](RE::Actor* actor, float thresholdPct) -> float {
				return ResolvePlayerBleedRuntimeSafeHealth(actor, thresholdPct);
			};
			context.buildPlayerDamageGuardConfig = [](RE::Actor* actor, float thresholdPct, float protectedHp, float safeFloorHp) {
				return BuildPlayerDamageGuardConfig(actor, thresholdPct, protectedHp, safeFloorHp);
			};
			context.clampHealth = [](RE::Actor* actor, float minHp) { ClampHealth(actor, minHp); };
			context.clampHealthCeiling = [](RE::Actor* actor, float maxHp) { ClampHealthCeiling(actor, maxHp); };
			context.resolveActorHealthForPct = [](RE::Actor* actor, float pct) -> float { return ResolveActorHealthForPct(actor, pct); };
			context.getActorHealthPct = [](RE::Actor* actor) -> float { return GetActorHealthPct(actor); };
			context.isActorBleedingOut = [](RE::Actor* actor) -> bool { return IsActorBleedingOut(actor); };
			context.applyBleedRegenOverride = [](RE::Actor* actor, BleedLockEntry& entry) { ApplyBleedRegenOverride(actor, entry); };
			context.restoreBleedRegenOverride = [](RE::Actor* actor, const BleedLockEntry& entry) { RestoreBleedRegenOverride(actor, entry); };
			context.logReferenceBleedState = [](RE::Actor* actor, const BleedLockEntry& entry, const char* point, const char* note) {
				LogReferenceBleedState(actor, entry, point, note);
			};
			context.maybeLogReferenceBleedSamples = [](RE::Actor* actor, BleedLockEntry& entry, std::chrono::steady_clock::time_point now) {
				MaybeLogReferenceBleedSamples(actor, entry, now);
			};
			context.enforcePlayerBleedInvulnerability = [](RE::Actor* actor, BleedLockEntry& entry) { EnforcePlayerBleedInvulnerability(actor, entry); };
			context.forcePlayerBleedAlive = [](RE::Actor* actor, BleedLockEntry& entry, const char* reason) { ForcePlayerBleedAlive(actor, entry, reason); };
			context.tickPlayerKillmoveSuppression = []() { TFD::PlayerOverkillDamageHook::TickKillmoveSuppression(); };
			context.tickPlayerOverkillBlockPending = []() { TickPlayerOverkillBlockPending(); };
			context.tickPlayerPreDeathShield = []() { TFD::PlayerOverkillDamageHook::TickPreDeathShield(); };
			context.scanBleedLockCandidates = []() { ScanBleedLockCandidates(); };
			context.isDownedTeammateRecoveryDialogueHoldActor = [](RE::Actor* actor) -> bool {
				return TFD::TeammateManager::IsDownedTeammateRecoveryDialogueHoldActor(actor);
			};
			context.isPlayerBattleObserveActive = []() -> bool {
				return TFD::Bleedout::BleedBattleObservePendingRef() || TFD::Bleedout::BleedBattleObserveActiveRef();
			};
			context.resetPlayerDamageGuard = [](const char* reason) { TFD::PlayerDamageGuard::Reset(reason ? reason : "bleed_lock_runtime_clear"); };
			context.clearPendingOverkillRoute = [](const char* reason) { TFD::PlayerDownRouter::ClearPendingOverkillRoute(reason ? reason : "bleed_lock_runtime_clear"); };
			context.clearPreDeathShield = []() { TFD::PlayerOverkillDamageHook::ClearPreDeathShield("bleed_lock_clear", true); };
			context.resetEnemyThresholdScanTimer = []() { g_enemyThresholdScanLast = {}; };
			return context;
		}



		// Temporary split module: bleed dialogue/runtime orchestration

		static void ClearBleedStickyReopenGrace(const char* reason)
		{
			if (!g_bleedStickyReopenGraceActive) {
				return;
			}
			spdlog::info("[TFD][Defeat][R113] bleed sticky reopen grace cleared reason={} speaker={:08X}",
				reason ? reason : "unknown",
				g_bleedStickyReopenGraceSpeakerID);
			g_bleedStickyReopenGraceActive = false;
			g_bleedStickyReopenGraceUntil = {};
			g_bleedStickyReopenGraceSpeakerID = 0;
			g_bleedStickyReopenGraceDistance = 99999.0f;
		}

		static void CancelBleedStickyReopenAfterTerminalOutcome(const char* eventName, const char* reason)
		{
			const bool wasActive = g_bleedStickyReopenGraceActive;
			const std::uint32_t speakerID = g_bleedStickyReopenGraceSpeakerID;
			const char* why = reason && reason[0] ? reason : "terminal_outcome";

			ClearBleedStickyReopenGrace(why);
			TFD::BleedoutGreet::MarkStickyReopenPending(false, why);

			if (wasActive) {
				spdlog::info("[TFD][Defeat][R300C] bleed sticky reopen cancelled by terminal outcome event={} speaker={:08X} reason={}",
					eventName && eventName[0] ? eventName : "unknown",
					speakerID,
					why);
			}
		}

		static bool TickBleedStickyReopenGrace(RE::Actor* player, std::chrono::steady_clock::time_point now)
		{
			(void)player;
			(void)now;
			if (!g_bleedStickyReopenGraceActive) {
				return false;
			}

			// R470A: DefeatMonitor is an HP/death guard only.  Legacy sticky
			// reopen grace used to let this monitor reopen Bleedout forcegreet
			// after another owner had already moved the route.  That overlap caused
			// stale speaker ownership and AfterPleasure flicker.
			spdlog::info("[TFD][Defeat][R470A] bleed sticky reopen grace ignored monitor-only speaker={:08X}", g_bleedStickyReopenGraceSpeakerID);
			ClearBleedStickyReopenGrace("r470a_monitor_only_no_reopen");
			TFD::BleedoutGreet::MarkStickyReopenPending(false, "r470a_monitor_only_no_reopen");
			return false;
		}

		static void TickUI()
		{
			struct Guard {
				~Guard() { g_tickPending.clear(std::memory_order_release); }
			} guard;
			if (!TFD::Settings::GetEnabled()) return;
			if (g_loadTransition.load(std::memory_order_acquire)) return;
			auto* ui = RE::UI::GetSingleton();
			TFD::Transition::PollResult();
			TFD::Transition::ProcessPendingFadeIn();
			if (TFD::Transition::IsAwaiting() || TFD::Transition::HasPendingFadeIn()) {
				TFD::Transition::MaintainCalmWindow(BuildTransitionRuntimeHandlers());
				UpdatePreCombatState();
				return;
			}
			RefreshPostDefeatGlobals();

			// R393A: DefeatMonitor is the Enemy HP sensor only. Victory owns
			// registry, passive state, countdown, and auto-death.
			TickEnemyThresholdSensor("pre_owner_gate");

			const bool inCombatDialogueOpen = TFD::DialogueLifecycle::IsDialogueOpen();
			if (!inCombatDialogueOpen &&
				!TFD::InteractionRouter::DialogueOpen::IsActive() &&
				TFD::Bleedout::TickQueuedAfterPleasureCrowdContinuation()) {
				return;
			}
			TFD::CaptiveRecaptureRecoveryWiring::TickPulse();
			const bool captiveBleedOverlay = TFD::Captive::HasEscapeBreakRebleedPending() || g_inBleedState.load(std::memory_order_acquire);
			const auto dialogueSnapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
			auto dialogueTick = TFD::DialogueLifecycle::TickOpenEdges(TFD::DialogueLifecycle::TickInput{
				captiveBleedOverlay,
				TFD::InCombat::IsActive(),
				TFD::Rescue::IsActive(),
				dialogueSnapshot.sub == TFD::FlowController::SubFlow::BleedoutAfterPleasure,
				TFD::PleasureRuntime::IsBlocking()
			});
			if (dialogueTick.handledBleedAfterPleasurePause) {
				return;
			}

			if (!TFD::Captive::TickRuntime(Player(), captiveBleedOverlay, BuildCaptiveRuntimeTickHandlers())) {
				return;
			}

			if (ui && ui->GameIsPaused()) return;
			auto* player = Player();
			if (!player) {
				RefreshPostDefeatGlobals();
				return;
			}
			TFD::FlowController::TickRuntime();
			TFD::Location::UpdateAmbientKidnapAvailability(false);
			if (TFD::Bleedout::DefeatGlue::HandlePendingEscapeBreak()) {
				return;
			}

			// R215A: Pay/Release and Left-For-Dead terminal outcomes arm the
			// post-defeat recovery cooldown before the next threshold scan.  The
			// old order scanned TickBleedLocks() first, so a still-low player HP
			// could immediately create a new Bleedout window before recovery had
			// a chance to own the state.
			if (TFD::Transition::IsLeftForDeadCooldownActive(BuildTransitionRuntimeHandlers())) {
				TFD::Transition::TickLeftForDeadCooldown(BuildTransitionRuntimeHandlers());
				return;
			}

			TFD::BleedLockRuntime::Tick(BuildBleedLockRuntimeContext());
			UpdatePreCombatState();
			if (IsGraceActive()) return;
			if (!captiveBleedOverlay && !g_inBleedState.load(std::memory_order_acquire) && TFD::InCombat::IsActive()) {
				// R470A: DefeatMonitor must not own InCombat dialogue lifecycle.
				// InCombatGreet / FlowController own sticky reopen, close, complete,
				// cancel, and AfterPleasure handoff.  DefeatMonitor only observed the
				// DialogueMenu open edge above for compatibility with existing state.
			}


			if (g_inBleedState.load(std::memory_order_acquire)) {
				if (g_bleedBattleObservePending) {
					TFD::Bleedout::RuntimeHost::TickBattleObservePending();
					return;
				}
				if (g_bleedBattleObserveActive) {
					TFD::Bleedout::RuntimeHost::TickBattleObserve();
					return;
				}
				if (TickBleedStickyReopenGrace(player, Now())) {
					return;
				}
				{
					const float runtimeSafeHp = ResolvePlayerBleedRuntimeSafeHealth(player, TFD::Settings::GetDefeatThresholdPct());
					g_minHp = (std::max)(g_minHp, runtimeSafeHp);
					SetPlayerBleedImmune(true);
					ClampHealth(player, g_minHp);
				}
				MaintainBleedPrimaryCaptorBinding();
				TFD::Bleedout::DefeatGlue::MaintainSpeakerKick();
				const bool dOpen = TFD::DialogueLifecycle::IsDialogueOpen();
				const int bleedSeconds = TFD::Settings::GetBleedWindowSeconds();
				const bool pleasureCommitted = TFD::Bleedout::GetDialogueOutcome() == BleedDialogueOutcome::Pleasure;
				const bool ostimBridgeBlocking = TFD::PleasureRuntime::IsBlocking();
				auto holdDecision = TFD::BleedoutGreet::EvaluateHold(dOpen, pleasureCommitted, ostimBridgeBlocking);
				if (holdDecision.hold) {
					const auto nowBleedHold = Now();
					if (dOpen) {
						TFD::DialogueLifecycle::ObserveBleedoutDialogueOpened("bleed_hold");
					}
					if (!g_bleedPaused) {
						g_bleedPaused = true;
						g_bleedPauseStarted = nowBleedHold;
						spdlog::info("[TFD][Defeat][R470A] bleed countdown paused monitor-only by {}", TFD::BleedoutGreet::GetHoldReasonName(holdDecision.reason));
					}
					TFD::DialogueLifecycle::SetDialogueOpenObserved(dOpen);
					return;
				}
				// R130: no destructive Bleedout forcegreet timeout/rearm.
				// Long-distance or stuck speakers should be handled by approach assist / MoveTo,
				// not by cancelling and restarting the dialogue lifecycle.
				if (g_bleedPaused) {
					g_bleedStart += (Now() - g_bleedPauseStarted);
					g_bleedPaused = false;
					g_bleedPauseStarted = {};
					g_bleedLastSeconds = -1;
					spdlog::info("[TFD][Defeat] bleed countdown resumed after dialogue");
				}

				// R470A: no DefeatMonitor-owned Bleedout forcegreet reopen.
				// Initial handoff retry and speaker primer belong to TFDBleedout/TFDBleedoutGreet.
				// This keeps DefeatMonitor as HP/death guard instead of a dialogue owner.

				if (TFD::Bleedout::GetBleedSpeakerID() == 0) {
					const auto nowBleed = Now();
					const auto lastNoSpeakerAttempt = TFD::Tame::GetBleedNoSpeakerTameLastAttempt();
					if (lastNoSpeakerAttempt.time_since_epoch().count() == 0 || (nowBleed - lastNoSpeakerAttempt) >= std::chrono::milliseconds(900)) {
						TFD::Tame::SetBleedNoSpeakerTameLastAttempt(nowBleed);
						const float bleedRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius());
						auto crowd = CollectBleedoutCrowd(bleedRadius, nullptr, false);
						const bool tameHeld = TFD::Tame::TryEnsureBleedNoSpeakerTameSession(crowd, "bleed_tick_no_speaker");
						if (tameHeld) {
							spdlog::info("[TFD][Defeat] bleed no-speaker tick tameHeld=1 crowdSize={}", crowd.size());
						}
					}
				}
				// R470A: DefeatMonitor is no longer a Bleedout dialogue decision owner.
				// It may observe the close edge to clear local monitor bookkeeping, but it
				// must not resolve Pay/Release, captive fallback, post-dialogue system
				// events, or sticky reopen.  Those are route-owner responsibilities
				// (Bleedout/PleasureRuntime/FlowController/Papyrus fragments).
				const bool bleedDialogueSeen = TFD::BleedoutGreet::HasSeenDialogue();
				const bool bleedTerminalCommit = TFD::Bleedout::HasTerminalCommit();
				const bool bleedPleasureBlocking = TFD::PleasureRuntime::IsBlocking();
				const bool bleedCaptiveOutcome = TFD::Bleedout::GetDialogueOutcome() == BleedDialogueOutcome::Captive;
				if (bleedDialogueSeen && TFD::DialogueLifecycle::WasDialogueOpen() && !dOpen) {
					TFD::DialogueLifecycle::SetDialogueOpenObserved(false);
					g_bleedLastSeconds = -1;
					ClearBleedStickyReopenGrace("r470a_monitor_only_dialogue_closed");
					TFD::BleedoutGreet::MarkStickyReopenPending(false, "r470a_monitor_only_dialogue_closed");
					spdlog::info(
						"[TFD][Defeat][R470A] bleed dialogue closed observed monitor-only terminal={} pleasureBlocking={} captiveOutcome={} speaker={:08X}",
						bleedTerminalCommit ? 1 : 0,
						bleedPleasureBlocking ? 1 : 0,
						bleedCaptiveOutcome ? 1 : 0,
						TFD::Bleedout::GetBleedSpeakerID());

					if (bleedTerminalCommit || bleedPleasureBlocking || bleedCaptiveOutcome) {
						return;
					}
				}

				if (TFD::Bleedout::IsAwaitingSystemEventOutcome() && !TFD::DialogueLifecycle::WasDialogueOpen()) {
					const char* pendingSystemReason = nullptr;
					(void)TFD::Bleedout::IsSystemEventPendingForFallback(&pendingSystemReason);
					if (pendingSystemReason && pendingSystemReason[0]) {
						spdlog::info(
							"[TFD][Defeat][R470A] pending bleed system event observed monitor-only reason={} terminal={} speaker={:08X}",
							pendingSystemReason,
							TFD::Bleedout::HasTerminalCommit() ? 1 : 0,
							TFD::Bleedout::GetBleedSpeakerID());
						return;
					}
				}

				const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(Now() - g_bleedStart).count();
				const int remain = bleedSeconds - static_cast<int>(elapsed);
				if (remain != g_bleedLastSeconds) {
					g_bleedLastSeconds = remain;
					const bool suppressCaptiveRecaptureNotice = TFD::Captive::IsEscapeBleedoutActive() || TFD::Captive::IsRecaptureCommitActive() || TFD::Captive::IsRecaptureRecentlyCommitted();
					if (remain > 0 && !suppressCaptiveRecaptureNotice) {
						char msg[96]{};
						std::snprintf(msg, sizeof(msg), "TFDEngine: Bleeding... (%ds)", remain);
						RE::DebugNotification(msg);
					}
				}
				if (remain <= 0) {
					g_bleedLastSeconds = -1;
					const char* pendingSystemReason = nullptr;
					(void)TFD::Bleedout::IsSystemEventPendingForFallback(&pendingSystemReason);
					TFD::Bleedout::TimeoutContext timeoutContext{
						TFD::Bleedout::HasTerminalCommit(),
						TFD::Bleedout::GetTerminalCommitName(TFD::Bleedout::GetTerminalCommit()),
						pendingSystemReason,
						g_bleedPendingCaptiveOutcome
					};
					auto timeoutHandlers = TFD::Bleedout::Builders::BuildTimeoutHandlers();
					if (TFD::Bleedout::HandleBleedTimeout(timeoutContext, "bleed_timeout", timeoutHandlers)) {
						return;
					}
				}
				return;
			}
			const float hpNow = player->GetActorValue(RE::ActorValue::kHealth);
			const float hpMax = (std::max)(1.0f, player->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float pct = (hpNow / hpMax) * 100.0f;
			const float thresh = TFD::Settings::GetDefeatThresholdPct();
			if (pct <= thresh) {
				if (IsPostTerminalBleedoutLockoutActive("player_threshold", pct, thresh)) {
					return;
				}
				auto thresholdScan = ScanPlayerThresholdOutcome(player);
				const bool immediateThreat = player->IsInCombat() || thresholdScan.initialAggressor != nullptr || thresholdScan.hostileCoalitionStanding;
				if (!immediateThreat) {
					if (TryBeginThresholdNoThreatRescueFallback(player, thresholdScan, thresh, "player_threshold_no_threat")) {
						return;
					}
					spdlog::info("[TFD][Defeat] skip player threshold outcome reason=no_immediate_threat hpPct={:.1f} threshold={:.1f}", pct, thresh);
					return;
				}
				TFD::BleedLockRuntime::Enter(BuildBleedLockRuntimeContext(), player, BleedLockKind::Player, thresh, "player_threshold");
				auto thresholdClassification = ClassifyPlayerThresholdOutcome(thresholdScan);
				if (DispatchPlayerThresholdOutcome(player, thresholdScan, thresholdClassification)) {
					return;
				}
			}
		}

		class BleedOutcomeEventSink final : public RE::BSTEventSink<SKSE::ModCallbackEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* ev, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
			{
				if (!ev) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto* rawName = ev->eventName.c_str();
				if (!rawName || !rawName[0]) {
					return RE::BSEventNotifyControl::kContinue;
				}


				if (std::strcmp(rawName, kBleedoutGreetConfirmedEvent) == 0) {
					auto* speaker = ev->sender ? ev->sender->As<RE::Actor>() : nullptr;
					TFD::BleedoutGreet::NotifyFlowGreetConfirmed(speaker, ev->strArg.c_str());
					return RE::BSEventNotifyControl::kContinue;
				}

				if (std::strcmp(rawName, kBleedoutCancelStickyReopenEvent) == 0) {
					auto* speaker = ev->sender ? ev->sender->As<RE::Actor>() : nullptr;
					const auto speakerID = speaker ? speaker->GetFormID() : 0u;
					const bool wasGraceActive = g_bleedStickyReopenGraceActive;
					const auto graceSpeakerID = g_bleedStickyReopenGraceSpeakerID;
					const char* why = ev->strArg.empty() ? "r441a_reject_cycle_handoff" : ev->strArg.c_str();

					ClearBleedStickyReopenGrace(why);
					TFD::Bleedout::ClearSystemEventOutcomeWindow(why);
					TFD::BleedoutGreet::ClearFlowGreetConfirmed(why);
					TFD::BleedoutGreet::MarkStickyReopenPending(false, why);
					TFD::BleedoutGreet::ResetRuntime(why);
					TFD::DialogueLifecycle::SetDialogueOpenObserved(false);
					g_bleedLastSeconds = -1;
					spdlog::info(
						"[TFD][Defeat][R441A] bleed sticky reopen cancelled by reject cycle sender={:08X} graceSpeaker={:08X} wasActive={} reason={}",
						speakerID,
						graceSpeakerID,
						wasGraceActive ? 1 : 0,
						why);
					return RE::BSEventNotifyControl::kContinue;
				}

				if (TFD::FlowController::HandleOutcomeModEvent(rawName, ev->strArg.c_str(), ev->numArg, ev->sender)) {
					if (IsBleedoutTerminalOutcomeEvent(rawName)) {
						CancelBleedStickyReopenAfterTerminalOutcome(rawName, "mod_event_terminal_outcome");
						ArmPostTerminalBleedoutLockout(rawName, ResolvePostTerminalBleedoutLockoutSeconds(rawName, ev->numArg), "mod_event_terminal_outcome");
					}
					return RE::BSEventNotifyControl::kContinue;
				}

				return RE::BSEventNotifyControl::kContinue;
			}
		};

		BleedOutcomeEventSink g_bleedOutcomeEventSink{};

		class PassiveBreakEventSink final : public RE::BSTEventSink<RE::TESHitEvent>, public RE::BSTEventSink<SKSE::ModCallbackEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* ev, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				if (!ev) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* causeActor = ev->cause ? ev->cause.get()->As<RE::Actor>() : nullptr;
				auto* targetActor = ev->target ? ev->target.get()->As<RE::Actor>() : nullptr;
				(void)TFD::FlowController::HandlePassiveBreakHitEvent(causeActor, targetActor);
				if (targetActor && targetActor == Player() &&
					causeActor && causeActor != targetActor &&
					IsCombatSupportedAggressor(causeActor) &&
					!IsObserverAlly(causeActor)) {
					NoteEnemyTargetingPlayerInternal(causeActor);
					TFD::PlayerOverkillDamageHook::SetKillmoveGuard(true, "tes_hit_event_player_target", std::chrono::milliseconds(2500));
					(void)TFD::PlayerOverkillDamageHook::TryQueueKillmoveBlockedBleedout(targetActor, causeActor, "tes_hit_event_player_target");
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* ev, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
			{
				if (!ev) {
					return RE::BSEventNotifyControl::kContinue;
				}

				(void)TFD::FlowController::HandlePassiveBreakModEvent(ev->eventName.c_str(), ev->strArg.c_str());
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		PassiveBreakEventSink g_passiveBreakEventSink{};

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

	static TFD::DefeatRuntimeProviders::InstallHooks BuildDefeatRuntimeProviderHooks()
	{
		TFD::DefeatRuntimeProviders::InstallHooks hooks{};
		hooks.teammateHasAllyBleedLock = [](RE::Actor* actor) {
			auto it = g_bleedLocks.find(actor ? actor->GetFormID() : 0u);
			return actor && it != g_bleedLocks.end() && it->second.kind == BleedLockKind::Ally;
			};
		hooks.teammateIsBleedingOutActor = [](RE::Actor* actor) { return IsActorBleedingOut(actor); };
		hooks.teammateReviveDownedAlly = [](RE::Actor* actor, float targetHealthPct) { return TFD::TeammateManager::ReviveDownedAlly(actor, targetHealthPct); };
		hooks.hostilityStartBleedTruceSessionForSpeaker = [](RE::Actor* player, RE::Actor* speaker, const char* reason) {
			return TFD::Bleedout::StartTruceSessionForSpeaker(player, speaker, reason, TFD::Bleedout::RuntimeHost::BuildStateRefs(), TFD::Bleedout::RuntimeHost::BuildHandlers());
			};
		hooks.hostilityReleaseBleedTruceSession = [](TFD::Tame::ReleaseReason reason) { TFD::Bleedout::ReleaseTruceSession(reason); };
		hooks.tameIsCreatureDefeatedEnemy = [](RE::Actor* actor) { return TFD::Actor::Ops::IsCreatureDefeatedEnemy(actor); };
		hooks.tameGetDefeatedEnemyRemainingSeconds = [](RE::Actor* actor) { return TFD::Actor::Ops::GetDefeatedEnemyRemainingSeconds(actor); };
		hooks.tameSuppressDefeatedReentry = [](RE::Actor* actor, double seconds, const char* reason) { TFD::Actor::Ops::SuppressDefeatedEnemyReentry(actor, seconds, reason); };
		hooks.tameReleaseBleedLock = [](RE::Actor* actor, const char* reason, bool playGetUp) { (void)TFD::Victory::ReleaseManagedEnemy(actor, reason ? reason : "tame_release", playGetUp); };
		hooks.tameRestoreActorHealthToSafePct = [](RE::Actor* actor, float thresholdPct, float bonusPct, float minSafePct, float maxSafePct, float minAbsHp, const char* reason) { TFD::Victory::RestoreActorHealthToSafePct(actor, thresholdPct, bonusPct, minSafePct, maxSafePct, minAbsHp, reason ? reason : "tame_recover"); };
		hooks.tameReleaseBleedNoSpeakerTameSession = [](const char* reason) { TFD::Bleedout::ReleaseNoSpeakerTameSession(reason); };
		hooks.tameTryEnsureBleedNoSpeakerTameSession = [](const std::vector<RE::Actor*>& actors, const char* reason) {
			auto* player = Player();
			if (!player) {
				return false;
			}
			return TFD::Bleedout::TryEnsureNoSpeakerTameSession(actors, player, reason, TFD::Bleedout::RuntimeHost::BuildHandlers());
			};
		return hooks;
	}


	static TFD::Bleedout::DefeatLifecycleProviders BuildBleedoutDefeatLifecycleProviders()
	{
		return TFD::Bleedout::DefeatLifecycleProviders{
		TFD::Bleedout::Builders::PendingSystemEventProvider{
			[](const char* r) { TFD::Bleedout::ClearDialogueOutcome(r); },
			[](const char* r) { TFD::Bleedout::ClearSystemEventOutcomeWindow(r); },
			[](const char* r) { (void)TFD::DefeatRuntimeActions::CompletePayRelease(r); },
			[]() { TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::FlowHandoff); },
			[]() -> bool { return TFD::Transition::ResolveCaptiveMarkerForOutcome(BuildTransitionRuntimeHandlers()); },
			[]() {
				const bool teleported = TFD::Bleedout::DoBlackoutTeleport("blackout_teleport", TFD::Bleedout::Builders::BuildBlackoutHandlers());
				if (teleported) {
					TFD::CaptiveRecaptureRecoveryWiring::ArmPulse("pending_system_event_blackout");
				}
			},
			[](int seconds) { SetGraceSeconds(seconds); },
			[](const char* r) { TFD::Tame::ReleaseBleedNoSpeakerTameSession(r); },
			[]() { TFD::Bleedout::DefeatGlue::ExitSystemEventRuntime(); },
			[](const char* r) { (void)TFD::Bleedout::EnterNonCaptiveChoice(r, TFD::Bleedout::Builders::BuildNonCaptiveChoiceHandlers()); }
		},
		TFD::Bleedout::Builders::DialogueCloseProvider{
			[](const char* r) { TFD::Bleedout::ClearDialogueOutcome(r); },
			[](const char* r) { (void)TFD::DefeatRuntimeActions::CompletePayRelease(r); }
		},
		TFD::Bleedout::Builders::TimeoutProvider{
			[]() { TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::Generic); },
			[]() -> bool { return TFD::Transition::ResolveCaptiveMarkerForOutcome(BuildTransitionRuntimeHandlers()); },
			[]() { ResetBleedRuntimeState(); },
			[]() {
				const bool teleported = TFD::Bleedout::DoBlackoutTeleport("blackout_teleport", TFD::Bleedout::Builders::BuildBlackoutHandlers());
				if (teleported) {
					TFD::CaptiveRecaptureRecoveryWiring::ArmPulse("timeout_blackout");
				}
			},
			[](int seconds) { SetGraceSeconds(seconds); },
			[](const char* r) { (void)TFD::Bleedout::EnterNonCaptiveChoice(r, TFD::Bleedout::Builders::BuildNonCaptiveChoiceHandlers()); }
		},
		TFD::Bleedout::Builders::BaseCompletionProvider{
			[](const char* r) { TFD::Bleedout::ClearSystemEventOutcomeWindow(r); },
			[](BleedTerminalCommit kind, const char* r) { return TFD::Bleedout::TryBeginTerminalCommit(kind, r); },
			[]() { TFD::Transition::ClearPendingFadeIn(); },
			[](const char* r) { TFD::Bleedout::ClearBridgeAliases(nullptr, r); },
			[]() { TFD::Captive::ClearEscapeContext(); },
			[]() { TFD::Captive::ResetLockpickWatch(); },
			[](bool active) { g_grace.store(active, std::memory_order_release); },
			[](bool captive) { TFD::Captive::SetRuntimeState(captive, captive ? CaptivePhaseValue::Captive : CaptivePhaseValue::None); },
			[](bool immune) { SetPlayerBleedImmune(immune); },
			[]() { TFD::Transition::RecoverPlayerForTransition(BuildTransitionRuntimeHandlers()); },
			[]() { return TFD::Settings::GetSweepRadius(); },
			[](float radius) { TFD::DefeatRuntimeActions::ApplyTerminalCalmBubble(Player(), ResolveAggressor(), radius, TFD::Settings::GetSweepRadius(), "defeat_monitor_legacy_calm_callback"); },
			[]() { UpdatePreCombatState(); },
			[]() -> RE::Actor* { return TFD::Bleedout::DefeatGlue::ResolveBleedRuntimeSpeaker(); },
			[](RE::Actor* speaker, bool captive, const char* why) { TFD::Bleedout::DefeatGlue::BeginBleedPleasureRuntime(speaker, captive, why); },
			[](bool v) { TFD::DialogueLifecycle::SetDialogueOpenObserved(v); },
			[](bool v) { TFD::Captive::SetPrevLockpickOpen(v); }
		},
		TFD::Bleedout::Builders::CaptivePleasureCompletionExtras{
			[]() { g_lastAggressor.reset(); },
			[](bool preserve) { ResetBleedRuntimeState(preserve); },
			[](const char* r) { TFD::Captive::SyncPlayerAlias(Player(), r); }
		},
		TFD::Bleedout::Builders::PayReleaseCompletionExtras{
			[]() { TFD::Actor::Ops::ClearAggressorFactionContext(); },
			[]() { g_lastAggressor.reset(); },
			[](bool preserve) { if (preserve) { ResetBleedRuntimeState(true); } else { ResetBleedRuntimeState(); } },
			[](int secs) { TFD::Transition::BeginLeftForDeadCooldown(secs); },
			[](int secs) { SetGraceSeconds(secs); }
		},
		TFD::Bleedout::Builders::BleedPleasureCompletionExtras{
			[](const char* r) { TransitionBleedRuntimeToPleasureCommit(r); g_lastRouterCombatContextActive = false; },
			[](int secs) { TFD::Transition::BeginLeftForDeadCooldown(secs); },
			[](int secs) { SetGraceSeconds(secs); },
			[]() { RefreshPostDefeatGlobals(); }
		},
		TFD::Bleedout::RuntimeHost::Provider{
			&g_inBleedState,
			&g_minHp,
			&g_bleedStart,
			&g_bleedLastSeconds,
			&g_bleedPaused,
			&g_bleedPauseStarted,
			&g_bleedLastCalmPulse,
			[]() { return TFD::Captive::HasEscapeBreakRebleedPending(); },
			[](bool pending) { TFD::Captive::SetEscapeBreakRebleedPending(pending); },
			&g_bleedPendingCaptiveOutcome,
			&g_bleedPendingNonCaptiveOutcome,
			&g_bleedBattleObservePending,
			&g_bleedBattleObservePendingUntil,
			&g_bleedBattleObservePendingLastRedirect,
			&g_bleedBattleObservePendingEmptyEnemyTicks,
			&g_bleedBattleObservePendingEmptyAllyTicks,
			&g_bleedBattleObserveActive,
			&g_bleedBattleObserveSince,
			&g_bleedBattleObserveLastRedirect,
			&g_bleedBattleObserveActiveEmptyEnemyTicks,
			&g_bleedBattleObserveActiveEmptyAllyTicks,
			[]() { return TFD::Bleedout::DefeatGlue::BuildLocalRuntimeHostHandlers(); },
			[]() { return TFD::Bleedout::DefeatGlue::BuildDialogueHotkeyHandlers(); },
			[]() -> RE::Actor* { return Player(); },
			[]() { return (std::max)(2400.0f, TFD::Settings::GetSweepRadius()); },
			[]() { return 1800.0f; }
		},
		TFD::Bleedout::DefeatGlue::Provider{
			&g_grace,
			&g_graceUntil,
			TFD::DialogueLifecycle::PreviousOpenStorage(),
			[]() { TFD::Transition::ClearPendingFadeIn(); },
			[]() { TFD::Captive::ClearEscapeContext(); },
			[]() { TFD::Captive::ResetLockpickWatch(); },
			[](bool open) { TFD::Captive::SetPrevLockpickOpen(open); },
			[]() { TFD::Captive::SetRuntimeState(false, CaptivePhaseValue::None); },
			[]() -> RE::Actor* { return Player(); },
			[](const char* reason, bool playGetUp) { TFD::BleedLockRuntime::ReleasePlayer(BuildBleedLockRuntimeContext(), reason, playGetUp); },
			[]() { return TFD::Bleedout::GetBleedSpeakerID(); },
			[]() -> RE::Actor* { return TFD::Bleedout::GetBleedSpeakerActor(); },
			[]() { return BuildBleedoutSpeakerHandlers(); },
			[]() { return TFD::DialogueLifecycle::IsDialogueOpen(); },
			[]() -> RE::Actor* { return ResolveAggressor(); },
			[](float radius) -> RE::Actor* { return FindBestAggressor(radius); },
			[](const char* reason) { TFD::Tame::ReleaseBleedNoSpeakerTameSession(reason); },
			[]() { TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::Generic); },
			[](RE::Actor* player, RE::Actor* speaker, const char* reason) { return TFD::HostilityController::StartBleedTruceSessionForSpeaker(player, speaker, reason); },
			[]() { TFD::Bleedout::ResetBleedSpeakerKick(); },
			[](const char* reason) { TFD::BleedoutGreet::ResetRuntime(reason); },
			[](RE::Actor* actor, const char* reason) { (void)TFD::BleedoutGreet::Begin(actor, reason); },
			[](const char* reason) { TFD::DefeatBridge::ClearBleedSupportAliases(reason); },
			[]() { g_bleedBattlePreferredEnemy.reset(); g_bleedBattleObserver = {}; },
			[]() { return g_observedCombatCommitDepth > 0; },
			[](RE::Actor* actor) { NoteEnemyTargetingPlayerInternal(actor); },
			[](bool immune) { SetPlayerBleedImmune(immune); },
			[](RE::Actor* actor, float minHp) { ClampHealth(actor, minHp); },
			[](float radius) { return CollectBleedStandingFollowers(radius); },
			[](RE::Actor* player, const std::vector<RE::Actor*>& allies, float baseRadius) { return TFD::Bleedout::DefeatGlue::ComputeObservedEnemyScanRadius(player, allies, baseRadius); },
			[](float radius, double maxAgeSec) -> RE::Actor* { return ResolveLastEnemyTargetingPlayerInternal(radius, maxAgeSec); },
			[](RE::Actor* actor) { return IsObserverAlly(actor); },
			[](RE::Actor* player, float radius, RE::Actor* preferred, const std::vector<RE::Actor*>& allies) { return TFD::Bleedout::DefeatGlue::CollectCurrentObservedEnemies(player, radius, preferred, allies); },
			[](RE::Actor* player, const std::vector<RE::Actor*>& followers, const std::vector<RE::Actor*>& enemies, RE::Actor* preferred) { TFD::Bleedout::DefeatGlue::UpdateObservedBattleRoster(player, followers, enemies, preferred); },
			[]() { return TFD::Bleedout::DefeatGlue::CollectStandingFollowersFromSnapshot(); },
			[]() { return TFD::Bleedout::DefeatGlue::CollectStandingEnemiesFromSnapshot(); },
			[]() { return TFD::Bleedout::DefeatGlue::HadValidObservedEnemy(); },
			[]() { return ResolveBleedFlowActorFormID(); },
			[](const TFD::FlowController::ObservedDefeatInput& input, const char* reason) {
				return TFD::FlowController::ApplyObservedDefeatResolution(input, reason ? reason : "battle_observe_resolution");
			},
			[]() { TFD::Bleedout::DefeatGlue::HandleObservedBattleWin(); },
			[](const char* reason) { TFD::Bleedout::DefeatGlue::HandleObservedLeftForDead(reason); },
			[](float radius, float maxDist, RE::Actor* preferred) { return TFD::BleedoutDialogueRuntime::FindBestSpeaker(radius, maxDist, preferred, BuildBleedoutDialogueRuntimeContext()); },
			[](RE::Actor* actor, RE::Actor* player, float maxDist, float* outDistance) { return TFD::BleedoutDialogueRuntime::IsReasonableSpeaker(actor, player, maxDist, outDistance, BuildBleedoutDialogueRuntimeContext()); },
			[](float radius, RE::Actor* preferred, bool preserveAssigned) { return CollectBleedoutCrowd(radius, preferred, preserveAssigned); },
			[](RE::Actor* actor) { return IsCaptiveSupportedAggressor(actor); },
			[](RE::Actor* actor) { return TFD::Actor::Ops::ApplyAggressorFactionContext(actor); },
			[]() { return TFD::Transition::ResolveCaptiveMarkerForOutcome(BuildTransitionRuntimeHandlers()); },
			[](RE::Actor* player, RE::Actor* aggressor, bool hasCaptiveOutcome, float* outDistance) { const float maxDist = hasCaptiveOutcome ? 1200.0f : 900.0f; return player && aggressor && IsCaptiveSupportedAggressor(aggressor) && IsReasonableCombatAggressor(aggressor, player, maxDist, outDistance); },
			[](const std::vector<RE::Actor*>& actors, const char* reason) { auto* player = Player(); return player && TFD::Bleedout::TryEnsureNoSpeakerTameSession(actors, player, reason, TFD::Bleedout::RuntimeHost::BuildHandlers()); },
			[](RE::Actor* actor) { g_lastAggressor = actor ? actor->GetHandle() : RE::ActorHandle{}; },
			[](RE::Actor* actor) { return IsBleedCrowdSupportedAggressor(actor); },
			[](RE::Actor* actor, RE::Actor* player) { return IsBleedSpaceCompatible(actor, player); },
			[](const char* msg) { if (msg && msg[0]) RE::DebugNotification(msg); },
			[](RE::Actor*, float, const char*) { /* R19: CombatBehavior owns player-target detach/retarget. */ },
			[](float radius) -> RE::Actor* { return TFD::Captive::ResolveEscapeBreakPreferredAggressor(radius, [](float fallbackRadius) { return FindBestAggressor(fallbackRadius); }); },
			[](RE::Actor* player, RE::Actor* aggressor, float* outDistance) { if (!player || !aggressor) { if (outDistance) *outDistance = -1.0f; return false; } float dist = -1.0f; const bool ok = TFD::BleedoutDialogueRuntime::CanUseAggressorForGreet(player, aggressor, dist, BuildBleedoutDialogueRuntimeContext()); if (outDistance) *outDistance = dist; return ok; },
			[](RE::Actor* player, RE::Actor* speaker, const char* reason, bool restartDialogue) { TFD::BleedoutDialogueRuntime::ApplyOverdrive(player, speaker, reason, restartDialogue, BuildBleedoutDialogueRuntimeContext()); },
			[](RE::Actor* actor) { return IsStandingAllyThresholdActor(actor); },
			[](RE::Actor* actor) { return IsStandingEnemyThresholdActor(actor); }
		}
		};
	}

	static TFD::FlowController::DefeatLifecycleProviders BuildFlowControllerDefeatLifecycleProviders()
	{
		return TFD::FlowController::DefeatLifecycleProviders{
		TFD::FlowController::PassiveRuntimeProviders{
			[]() { return g_inBleedState.load(std::memory_order_acquire); },
			[]() { return TFD::Actor::Ops::HasAnyReleaseFollowGrace(); },
			[]() -> RE::Actor* { return ResolveCurrentPassivePrimaryActor(); },
			[](RE::Actor* actor) { return IsActorCoveredByCurrentPassiveContext(actor); },
			[](RE::Actor* actor, const char* reason) { TFD::Actor::Ops::CancelReleaseFollowGraceFromPlayerAggression(actor, reason); },
			[](const char* reason) { TFD::Actor::Ops::ClearAllReleaseFollowGrace(reason); },
			[]() { TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::PlayerAggression); },
			[](const char* reason) { TFD::BleedLockRuntime::ClearAll(BuildBleedLockRuntimeContext(), reason); },
			[](RE::Actor* actor, const char* reason) { TFD::Bleedout::ClearBridgeAliases(actor, reason); }
		},
		TFD::FlowController::OutcomeRuntimeProviders{
			[](RE::Actor* actor, double seconds, const char* reason) { TFD::Actor::Ops::ApplyReleaseFollowGraceToSpeakerAndCrowd(actor, seconds, reason); },
			[](RE::Actor* actor, const char* reason) { TFD::Actor::Ops::RemoveReleaseFollowGraceFromSpeakerAndCrowd(actor, reason); },
			[]() { return ResolveBleedFlowActorFormID(); },
			[]() { return g_inBleedState.load(std::memory_order_acquire); },
			[](const char* reason) { (void)TFD::DefeatRuntimeActions::CompletePayRelease(reason ? reason : "mod_event_pay_immediate_terminal"); },
			[](const char* reason) { TFD::Bleedout::DefeatGlue::PreparePlayerForCaptivePleasureScene(reason); },
			[](const char* reason) { auto completion = TFD::Bleedout::Builders::BuildCaptivePleasureCompletionHandlers(); (void)TFD::Bleedout::CompleteCaptivePleasureHandoff(reason, completion); },
			[](const char* reason) { TFD::Bleedout::DefeatGlue::PreparePlayerForBleedoutPleasureScene(reason); },
			[](const char* reason) { auto completion = TFD::Bleedout::Builders::BuildBleedPleasureCompletionHandlers(); (void)TFD::Bleedout::CompleteBleedPleasureHandoff(reason ? reason : "bleed_pleasure_handoff", completion); }
		},
		TFD::FlowController::BattleObserverRuntimeProviders{
			[](const char* reason) { TFD::Bleedout::ClearBridgeAliases(nullptr, reason); },
			[]() { TFD::Bleedout::DefeatGlue::ClearCaptiveOrchestrationResidue(true); },
			[]() { TFD::Actor::Ops::ClearAggressorFactionContext(); },
			[]() { ResetBleedRuntimeState(); },
			[](bool immune) { SetPlayerBleedImmune(immune); },
			[](const char* reason) {
				return TFD::DefeatRuntimeActions::ExecuteResolvedNoMarkerFallback(reason ? reason : "battle_observe_win", BuildNoMarkerFallbackContext());
			},
			[](const char* reason) { TFD::DefeatBridge::QueueNonCaptiveChoiceRequest(reason); },
			[]() -> RE::Actor* { const float followerRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 400.0f); auto followers = ResolveFollowerCandidates(followerRadius); return followers.downed; },
			[](RE::Actor* follower) { TFD::Transition::ArmObservedLeftForDeadFallback(follower, BuildTransitionRuntimeHandlers()); },
			[](const char* reason) { TFD::Transition::BeginRecoverTransition(reason ? reason : "battle_observe_loss", BuildTransitionRuntimeHandlers()); },
			[]() -> const char* { return TFD::Transition::GetCurrentFallbackBranchName(); }
		}
		};
	}

	static TFD::DefeatLifecycleBootstrap::InstallHooks BuildDefeatLifecycleBootstrapHooks()
	{
		TFD::DefeatLifecycleBootstrap::InstallHooks hooks{};
		hooks.buildBleedoutProviders = []() { return BuildBleedoutDefeatLifecycleProviders(); };
		hooks.buildFlowControllerProviders = []() { return BuildFlowControllerDefeatLifecycleProviders(); };
		return hooks;
	}

	// Defeat lifecycle / install / shutdown / queued progress / load state
	void Install()
	{
		if (g_installed.exchange(true, std::memory_order_acq_rel)) return;
		g_running.store(true, std::memory_order_release);
		g_loadTransition.store(false, std::memory_order_release);
		TFD::Captive::ResetForLoad();
		TFD::Bleedout::DefeatGlue::ClearCaptiveOrchestrationResidue(false);
		SetPlayerBleedImmune(false);
		TFD::PlayerDamageGuard::Reset("defeat_monitor_reset");
		ResetBleedRuntimeState();
		TFD::CaptiveRecaptureRecoveryWiring::Dependencies captiveRecaptureRecoveryDependencies{};
		captiveRecaptureRecoveryDependencies.getPlayer = []() -> RE::Actor* { return Player(); };
		captiveRecaptureRecoveryDependencies.releasePlayerBleedLock = [](const char* reason, bool playGetUp) {
			TFD::BleedLockRuntime::ReleasePlayer(BuildBleedLockRuntimeContext(), reason, playGetUp);
			};
		captiveRecaptureRecoveryDependencies.restoreActorHealthToSafePct = [](RE::Actor* actor, float thresholdPct, float bonusPct, float minSafePct, float maxSafePct, float minAbsHp, const char* reason) {
			RestoreActorHealthToSafePct(actor, thresholdPct, bonusPct, minSafePct, maxSafePct, minAbsHp, reason);
			};
		captiveRecaptureRecoveryDependencies.resetBleedRuntimeState = [](bool preserveCaptive) { ResetBleedRuntimeState(preserveCaptive); };
		captiveRecaptureRecoveryDependencies.forceStopBleedRuntimeForCaptiveRecapture = [](const char* reason) { TFD::Bleedout::ForceStopBleedRuntimeForCaptiveRecapture(reason); };
		captiveRecaptureRecoveryDependencies.setPlayerBleedImmune = [](bool immune) { SetPlayerBleedImmune(immune); };
		captiveRecaptureRecoveryDependencies.playPlayerGetUp = [](RE::Actor* actor) {
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				return;
			}
			actor->NotifyAnimationGraph("BleedoutStop");
			actor->NotifyAnimationGraph("GetUpStart");
			};
		captiveRecaptureRecoveryDependencies.refreshPostDefeatGlobals = []() { RefreshPostDefeatGlobals(); };
		captiveRecaptureRecoveryDependencies.getActorHealthPct = [](RE::Actor* actor) -> float { return GetActorHealthPct(actor); };
		captiveRecaptureRecoveryDependencies.getDefeatThresholdPct = []() -> float { return TFD::Settings::GetDefeatThresholdPct(); };
		captiveRecaptureRecoveryDependencies.isActorBleedingOut = [](RE::Actor* actor) -> bool { return IsActorBleedingOut(actor); };
		TFD::CaptiveRecaptureRecoveryWiring::InstallProvider(captiveRecaptureRecoveryDependencies);
		// R451A/R460A: true overkill block plus lifecycle-gated player killmove suppression.
		// Clamp player Health damage in CheckClampDamageModifier before it can drop
		// below the safe floor, and keep paired killmoves from entering death state without
		// re-arming the guard from non-combat Health writes after terminal cleanup.
		TFD::PlayerOverkillDamageHook::Context playerOverkillHookContext{};
		playerOverkillHookContext.getPlayer = []() -> RE::Actor* { return Player(); };
		playerOverkillHookContext.resolveCachedAttacker = []() -> RE::Actor* { return ResolveCachedPlayerOverkillAttacker(); };
		playerOverkillHookContext.hasPendingOverkillRoute = []() -> bool { return TFD::PlayerDownRouter::HasPendingOverkillRoute(); };
		playerOverkillHookContext.hasPlayerBleedLock = []() -> bool { return HasPlayerBleedLock(); };
		playerOverkillHookContext.isInBleedState = []() -> bool { return g_inBleedState.load(std::memory_order_acquire); };
		playerOverkillHookContext.hasRecentEnemyTargetingPlayer = [](double maxAgeSec) -> bool { return HasRecentEnemyTargetingPlayerInternal(maxAgeSec); };
		playerOverkillHookContext.isObserverAlly = [](RE::Actor* actor) -> bool { return IsObserverAlly(actor); };
		playerOverkillHookContext.setPlayerBleedImmune = [](bool enable) { SetPlayerBleedImmune(enable); };
		playerOverkillHookContext.clampHealth = [](RE::Actor* actor, float minHp) { ClampHealth(actor, minHp); };
		playerOverkillHookContext.noteEnemyTargetingPlayer = [](RE::Actor* actor) { NoteEnemyTargetingPlayerInternal(actor); };
		playerOverkillHookContext.isPreDeathShieldActive = []() -> bool { return TFD::PlayerOverkillDamageHook::IsPreDeathShieldActive(); };
		playerOverkillHookContext.resolveBleedRuntimeSafeHealth = [](RE::Actor* actor, float thresholdPct) -> float {
			return ResolvePlayerBleedRuntimeSafeHealth(actor, thresholdPct);
			};
		playerOverkillHookContext.rememberAggressor = [](RE::Actor* actor) { RememberAggressorForOutcome(actor); };
		playerOverkillHookContext.getDefeatThresholdPct = []() -> float { return TFD::Settings::GetDefeatThresholdPct(); };
		playerOverkillHookContext.getActorHealthPct = [](RE::Actor* actor) -> float { return GetActorHealthPct(actor); };
		playerOverkillHookContext.resolveAggressor = []() -> RE::Actor* { return ResolveAggressor(); };
		playerOverkillHookContext.resolveLastEnemyTargetingPlayer = [](float radius, double maxAgeSec) -> RE::Actor* { return ResolveLastEnemyTargetingPlayerInternal(radius, maxAgeSec); };
		playerOverkillHookContext.enterPlayerBleedLock = [](RE::Actor* player, float thresholdPct, const char* reason) {
			TFD::BleedLockRuntime::Enter(BuildBleedLockRuntimeContext(), player, BleedLockKind::Player, thresholdPct, reason);
		};
		playerOverkillHookContext.dispatchThresholdScanImmediateBleedout = [](RE::Actor* player, float hpPct, float thresholdPct, const char* reason) -> bool {
			return DispatchThresholdScanImmediateBleedout(player, hpPct, thresholdPct, reason);
		};
		playerOverkillHookContext.tryBeginThresholdNoThreatRescueFallback = [](RE::Actor* player, float thresholdPct, const char* reason) -> bool {
			auto thresholdScan = ScanPlayerThresholdOutcome(player);
			return TryBeginThresholdNoThreatRescueFallback(player, thresholdScan, thresholdPct, reason);
		};
		TFD::PlayerOverkillDamageHook::SetContext(std::move(playerOverkillHookContext));
		TFD::PlayerOverkillDamageHook::Install();
		spdlog::info("[TFD][Defeat][R460A] player killmove guard lifecycle gated mode=combat_health_damage_or_hit_event");
		TFD::FlowController::Controller::GetSingleton().ResetRuntime("defeat_install");
		TFD::DefeatLifecycleBootstrap::Install(BuildDefeatLifecycleBootstrapHooks());

		TFD::DefeatRuntimeProviders::Install(BuildDefeatRuntimeProviderHooks());
		TFD::PleasureRuntime::Install();
		g_lastRouterCombatContextActive = false;
		TFD::BleedLockRuntime::ClearAll(BuildBleedLockRuntimeContext(), "install");
		TFD::Bleedout::ClearBridgeAliases(nullptr, "install");
		TFD::Actor::Ops::Initialize();
		TFD::Actor::Ops::InstallDefeatedEnemyQueryHooks(TFD::Actor::Ops::DefeatedEnemyQueryHooks{
			&IsTrackedDefeatedEnemyHook,
			&IsLastAggressorHook
			});
		TFD::Location::Initialize();
		if (auto* scripts = RE::ScriptEventSourceHolder::GetSingleton()) {
			scripts->AddEventSink<RE::TESHitEvent>(&g_passiveBreakEventSink);
		}
		if (auto* src = SKSE::GetModCallbackEventSource()) {
			src->AddEventSink(&g_bleedOutcomeEventSink);
			src->AddEventSink(&g_passiveBreakEventSink);
		}
		g_worker = std::thread([]() { WorkerLoop(); });
		TFD::DefeatMonitor::ApplyQueuedProgressState();
		SetRescueStateValue(0);
		RefreshPostDefeatGlobals();
		spdlog::info("[TFD][Defeat] monitor installed");
	}

	void Shutdown()
	{
		if (!g_installed.exchange(false, std::memory_order_acq_rel)) return;
		g_running.store(false, std::memory_order_release);
		if (g_worker.joinable()) g_worker.join();
		TFD::CaptiveRecaptureRecoveryWiring::ShutdownProvider();
		TFD::PlayerOverkillDamageHook::ClearContext();
		TFD::Bleedout::DefeatGlue::ClearCaptiveOrchestrationResidue(false);
		g_hasQueuedProgressState = false;
		TFD::Captive::ClearQueuedLoadedState();
		g_queuedBleedOutState = false;
		SetPlayerBleedImmune(false);
		ResetBleedRuntimeState();
		TFD::FlowController::Controller::GetSingleton().ResetRuntime("defeat_shutdown");
		TFD::FlowController::ResetDefeatLifecycleProviders();
		TFD::TeammateManager::ResetRuntimeProviders();
		TFD::Tame::ResetRuntimeProviders();
		TFD::HostilityController::ResetBleedTruceRuntimeProviders();
		g_lastRouterCombatContextActive = false;
		TFD::BleedLockRuntime::ClearAll(BuildBleedLockRuntimeContext(), "shutdown");
		TFD::Bleedout::ClearBridgeAliases(nullptr, "shutdown");
		g_loadTransition.store(false, std::memory_order_release);
		TFD::Transition::ClearLeftForDeadCooldown(BuildTransitionRuntimeHandlers());
		if (auto* scripts = RE::ScriptEventSourceHolder::GetSingleton()) {
			scripts->RemoveEventSink<RE::TESHitEvent>(&g_passiveBreakEventSink);
		}
		if (auto* src = SKSE::GetModCallbackEventSource()) {
			src->RemoveEventSink(&g_bleedOutcomeEventSink);
			src->RemoveEventSink(&g_passiveBreakEventSink);
		}
		TFD::PleasureRuntime::Shutdown();
		SetRescueStateValue(0);
		RefreshPostDefeatGlobals();
		TFD::Bleedout::ResetDefeatLifecycleProviders();
		TFD::Transition::DefeatGlue::Reset();
		spdlog::info("[TFD][Defeat] monitor shutdown");
	}

	void ResetGrace()
	{
		g_grace.store(false, std::memory_order_release);
		TFD::Transition::ClearLeftForDeadCooldown(BuildTransitionRuntimeHandlers());
		TFD::Actor::Ops::ClearAllReleaseFollowGrace("reset_grace");
	}

	bool HandlePassiveInvalidationAgainstActor(RE::Actor* actor, const char* reason)
	{
		return TFD::FlowController::HandlePassiveInvalidationAgainstActor(actor, reason, false);
	}

	bool GetCaptiveStateForSave()
	{
		return TFD::Captive::GetStateFlag();
	}

	std::uint32_t GetCaptivePhaseForSave()
	{
		return TFD::Captive::GetPhaseRaw();
	}

	bool GetBleedOutStateForSave()
	{
		if (auto* player = Player()) {
			return ComputePlayerBleedOutState(player);
		}
		return g_queuedBleedOutState;
	}

	void QueueLoadedBleedOutState(bool active)
	{
		g_queuedBleedOutState = active;
		spdlog::info("[TFD][Defeat] QueueLoadedBleedOutState state={}", active ? 1 : 0);
	}

	void QueueLoadedProgressState(bool stateActive, std::uint32_t phaseRaw)
	{
		g_hasQueuedProgressState = true;
		CaptivePhaseValue phase = stateActive ? PhaseFromRaw(phaseRaw) : CaptivePhaseValue::None;
		if (stateActive && phase == CaptivePhaseValue::None) phase = CaptivePhaseValue::Escape;
		TFD::Captive::QueueLoadedState(stateActive, phase);
		spdlog::info("[TFD][Defeat] QueueLoadedProgressState state={} phase={} normalized={}", stateActive ? 1 : 0, phaseRaw, static_cast<int>(TFD::Captive::GetQueuedPhase()));
	}

	void QueueDefaultProgressState()
	{
		g_hasQueuedProgressState = true;
		TFD::Captive::ClearQueuedLoadedState();
		g_queuedBleedOutState = false;
		spdlog::info("[TFD][Defeat] QueueDefaultProgressState");
	}

	bool HasQueuedProgressState()
	{
		return g_hasQueuedProgressState;
	}

	void ApplyQueuedProgressState()
	{
		if (!g_hasQueuedProgressState) QueueDefaultProgressState();
		TFD::Transition::ClearLeftForDeadCooldown(BuildTransitionRuntimeHandlers());
		TFD::Captive::ApplyQueuedDefeatProgressState(TFD::Captive::ApplyQueuedDefeatProgressHandlers{
			[]() -> RE::Actor* { return Player(); },
			[]() { return TFD::DialogueLifecycle::IsDialogueOpen(); },
			[](bool open) { TFD::DialogueLifecycle::SetDialogueOpenObserved(open); }
			});
		SetPlayerBleedImmune(false);
		TFD::PlayerDamageGuard::Reset("apply_queued_state");
		TFD::Bleedout::ClearBridgeAliases(nullptr, "apply_queued_state");
		SetRescueStateValue(0);
		RefreshPostDefeatGlobals();
		UpdatePreCombatState();
		spdlog::info("[TFD][Defeat] ApplyQueuedProgressState state={} phase={} bleed={}", TFD::Captive::GetQueuedStateFlag() ? 1 : 0, static_cast<int>(TFD::Captive::GetQueuedPhase()), g_queuedBleedOutState ? 1 : 0);
	}



	void ResetForLoad()
	{
		g_grace.store(false, std::memory_order_release);
		TFD::CaptiveRecaptureRecoveryWiring::ResetPulse();
		SetPlayerBleedImmune(false);
		ResetBleedRuntimeState();
		TFD::BleedLockRuntime::ClearAll(BuildBleedLockRuntimeContext(), "reset_for_load");
		TFD::Bleedout::ClearBridgeAliases(nullptr, "reset_for_load");
		g_lastAggressor = RE::ActorHandle{};
		ClearLastEnemyTargetingPlayerInternal();
		TFD::Bleedout::DefeatGlue::ClearCaptiveOrchestrationResidue(false);
		TFD::Actor::Ops::ClearAggressorFactionContext();
		TFD::HostilityController::ClearAggressionClamp();
		TFD::Transition::ClearLeftForDeadCooldown(BuildTransitionRuntimeHandlers());
		SetRescueStateValue(0);
		g_lastRouterCombatContextActive = false;
		TFD::PleasureRuntime::ResetForLoad("defeat_reset_for_load");
		RefreshPostDefeatGlobals();
		spdlog::info("[TFD][Defeat] ResetForLoad -> runtime only");
	}

	void SetLoadTransition(bool active)
	{
		g_loadTransition.store(active, std::memory_order_release);
		if (active) {
			SetPlayerBleedImmune(false);
			TFD::BleedLockRuntime::ClearAll(BuildBleedLockRuntimeContext(), "set_load_transition");
			TFD::Bleedout::ClearBridgeAliases(nullptr, "set_load_transition");
			TFD::Captive::ResetLockpickWatch();
			TFD::PleasureRuntime::ResetForLoad("defeat_set_load_transition");
			spdlog::info("[TFD][Defeat] SetLoadTransition(true)");
		}
		else {
			spdlog::info("[TFD][Defeat] SetLoadTransition(false)");
		}
	}

	bool IsThresholdDownedActor(RE::Actor* actor)
	{
		if (!actor) {
			return true;
		}

		float thresholdPct = TFD::Settings::GetEnemyDownedThresholdPct();
		if (actor == Player()) {
			thresholdPct = TFD::Settings::GetDefeatThresholdPct();
		}
		else if (IsActiveFollowerActor(actor)) {
			thresholdPct = TFD::Settings::GetAllyDownedThresholdPct();
		}

		return IsActorDownByThreshold(actor, thresholdPct);
	}

	bool IsThresholdCombatTargetValid(RE::Actor* actor)
	{
		if (!actor) {
			return false;
		}
		if (actor->IsDisabled() || actor->IsDead()) {
			return false;
		}
		return !IsThresholdDownedActor(actor);
	}

	RE::Actor* ResolveCurrentPassivePrimaryActor()
	{
		return nullptr;
	}

	bool IsActorCoveredByCurrentPassiveContext(RE::Actor*)
	{
		return false;
	}

}
