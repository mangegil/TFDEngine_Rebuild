#include "TFDPleasureRuntime.h"
#include "TFDFlowController.h"
#include "TFDForceGreetState.h"
#include "TFDHostilityController.h"
#include "TFDInteractionRouter.h"
#include "TFDPreCombatGreet.h"
#include "TFDInCombatGreet.h"
#include "TFDBleedout.h"
#include "TFDCaptive.h"
#include "TFDRecruit.h"
#include "TFDTeammateManager.h"

#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>

#include <spdlog/spdlog.h>

namespace TFD::PleasureRuntime
{
	namespace
	{
		struct RuntimeState
		{
			Phase phase{ Phase::Idle };
			SourceContext source{ SourceContext::None };

			std::uint32_t sessionCycleId{ 0 };
			std::uint32_t flowOwnerToken{ 0 };

			std::uint32_t pleasureSpeakerFormID{ 0 };
			std::uint32_t afterPleasureSpeakerFormID{ 0 };

			std::uint32_t ostimThreadId{ static_cast<std::uint32_t>(-1) };
			int bridgeState{ 0 };

			bool installed{ false };
			bool active{ false };
			bool blocking{ false };
			bool passiveLockActive{ false };
			bool holdActive{ false };
			bool afterPleasureCommitted{ false };
			bool pleasureFailedDialogueActive{ false };
			bool redoPending{ false };
			AfterChoice pendingChoice{ AfterChoice::None };

			std::uint32_t queuedPreCombatSpeakerFormID{ 0 };
			std::uint32_t queuedPreCombatConsumedFormID{ 0 };
			SourceContext queuedPreCombatSource{ SourceContext::None };
			bool queuedPreCombatBleedoutBridgeToInCombat{ false };
			double queuedPreCombatNextTrySec{ 0.0 };
			double queuedPreCombatExpireSec{ 0.0 };
			unsigned queuedPreCombatAttempts{ 0 };

			bool queuedTerminalNeutralFinalize{ false };
			std::uint32_t queuedTerminalNeutralActorFormID{ 0 };
			std::uint32_t queuedTerminalNeutralCycleId{ 0 };
			double queuedTerminalNeutralDueSec{ 0.0 };
			SourceContext queuedTerminalNeutralSource{ SourceContext::None };

			double afterPleasureDialogueExpireSec{ 0.0 };

			double pleasureStartPendingAtSec{ 0.0 };
			double pleasureActiveStartedAtSec{ 0.0 };
			double abortedFlowCompleteDueSec{ 0.0 };
			std::uint32_t abortedFlowActorFormID{ 0 };
			bool abortedFlowCompleteQueued{ false };

			double scenePassiveHoldNextPulseSec{ 0.0 };
			unsigned scenePassiveHoldPulseCount{ 0 };
			double truceFlowHandoffRefreshNextSec{ 0.0 };

			std::vector<std::uint32_t> deferredInCombatRecruitActorIds{};

			// R505A: A Bleedout recruit chain can legitimately continue as InCombat.
			// In that handoff, HostilityController's live truce list may shrink to
			// only the current speaker while the Bleedout/Papyrus crowd snapshot
			// still owns the remaining actors.  Keep this flag until terminal reset
			// so later InCombat recruit outcomes can fall back to that snapshot
			// instead of finalizing Neutral while crowd actors still exist.
			bool inCombatContinuationFromBleedoutSnapshot{ false };
		};

		// R94H: this runtime may be queried re-entrantly by TeammateManager
		// while handling TFDAfterPleasureChoiceRecruit. A plain std::mutex can
		// throw std::system_error(resource_deadlock_would_occur) on that path.
		std::recursive_mutex g_lock;
		RuntimeState g_state{};

		constexpr const char* kPleasureStartPendingEvent = "TFDPreCombatPleasureStartPending";
		constexpr const char* kPleasureStartedEvent = "TFDPreCombatPleasureStarted";
		constexpr const char* kPleasureFailedEvent = "TFDPreCombatPleasureFailed";
		constexpr const char* kPleasureAbortedEvent = "TFDPleasureAborted";
		constexpr const char* kPleasureEndedEvent = "TFDPreCombatPleasureEnded";

		constexpr const char* kOStimSceneStartPendingEvent = "TFDOStimSceneStartPending";
		constexpr const char* kOStimSceneStartedEvent = "TFDOStimSceneStarted";
		constexpr const char* kOStimSceneSucceededEvent = "TFDOStimSceneSucceeded";
		// R498A compatibility only. Updated Papyrus sends the explicit success event.
		constexpr const char* kOStimSceneEndedEvent = "TFDOStimSceneEnded";

		constexpr const char* kAfterPleasureEnterEvent = "TFDAfterPleasureEnter";
		constexpr const char* kAfterPleasureLoopEnterEvent = "TFDAfterPleasureLoopEnter";
		constexpr const char* kPleasureFailedEnterEvent = "TFDPleasureFailedEnter";

		constexpr const char* kAfterPleasureChoiceFinishEvent = "TFDAfterPleasureChoiceFinish";
		constexpr const char* kAfterPleasureChoiceRecruitEvent = "TFDAfterPleasureChoiceRecruit";
		constexpr const char* kAfterPleasureChoiceJoinEnemyEvent = "TFDAfterPleasureChoiceJoinEnemy";
		constexpr const char* kAfterPleasureChoicePleasureEvent = "TFDAfterPleasureChoicePleasure";
		constexpr const char* kAfterPleasureChoiceReleaseEvent = "TFDAfterPleasureChoiceRelease";
		constexpr const char* kAfterPleasureChoiceWorkEvent = "TFDAfterPleasureChoiceWork";
		constexpr const char* kAfterPleasureChoiceKidnapEvent = "TFDAfterPleasureChoiceKidnap";
		constexpr const char* kPleasureClearEvent = "TFDPleasureClear";
		constexpr const char* kSystemEventClearAfterPleasureEvent = "TFDSystemEventClearAfterPleasure";

		constexpr double kPreCombatCycleInitialDelaySec = 1.75;
		constexpr double kPreCombatCycleRetryDelaySec = 0.65;
		constexpr double kPreCombatCycleExpireSec = 8.0;
		constexpr unsigned kPreCombatCycleMaxAttempts = 12;
		constexpr double kTerminalNeutralFinalizeDelaySec = 0.05;
		constexpr double kAfterPleasureDialogueHardTimeoutSec = 0.0; // R127: AfterPleasure waits for terminal choice; no destructive hard timeout.
		constexpr double kSuppressSceneStartDialogueCooldownSec = 0.75;
		constexpr double kMinimumSceneActiveSec = 1.50;
		constexpr double kAbortedFlowCompleteDelaySec = 0.05;
		constexpr double kScenePassiveHoldPendingPulseSec = 0.45;
		constexpr double kScenePassiveHoldActivePulseSec = 3.50;
		constexpr double kInCombatTruceRefreshIntervalSec = 20.0;
		constexpr double kInCombatTruceRefreshDurationSec = 90.0;
		constexpr double kInCombatTruceRefreshRetrySec = 1.0;

		const char* ToString(Phase value)
		{
			switch (value) {
			case Phase::Idle:
				return "Idle";
			case Phase::PleasureStartPending:
				return "PleasureStartPending";
			case Phase::PleasureActive:
				return "PleasureActive";
			case Phase::PleasureEnding:
				return "PleasureEnding";
			case Phase::AfterPleasureAwaitQuest:
				return "AfterPleasureAwaitQuest";
			case Phase::AfterPleasureDialogue:
				return "AfterPleasureDialogue";
			case Phase::PleasureFailedDialogue:
				return "PleasureFailedDialogue";
			case Phase::RedoPending:
				return "RedoPending";
			case Phase::Finalizing:
				return "Finalizing";
			case Phase::Closed:
				return "Closed";
			default:
				return "Unknown";
			}
		}

		const char* ToString(SourceContext value)
		{
			switch (value) {
			case SourceContext::None:
				return "None";
			case SourceContext::PreCombat:
				return "PreCombat";
			case SourceContext::Bleedout:
				return "Bleedout";
			case SourceContext::Captive:
				return "Captive";
			case SourceContext::Teammate:
				return "Teammate";
			case SourceContext::InCombat:
				return "InCombat";
			default:
				return "Unknown";
			}
		}

		bool IsSupportedSourceContext(SourceContext source)
		{
			return source == SourceContext::PreCombat ||
				source == SourceContext::Bleedout ||
				source == SourceContext::Captive ||
				source == SourceContext::Teammate ||
				source == SourceContext::InCombat;
		}

		SourceContext SourceFromFlowValue(int sourceFlow, SourceContext fallback)
		{
			switch (sourceFlow) {
			case static_cast<int>(SourceContext::PreCombat):
				return SourceContext::PreCombat;
			case static_cast<int>(SourceContext::Bleedout):
				return SourceContext::Bleedout;
			case static_cast<int>(SourceContext::Captive):
				return SourceContext::Captive;
			case static_cast<int>(SourceContext::Teammate):
				return SourceContext::Teammate;
			case static_cast<int>(SourceContext::InCombat):
				return SourceContext::InCombat;
			default:
				return fallback;
			}
		}

		AfterChoice MapAfterPleasureChoice(std::string_view eventName)
		{
			if (eventName == kAfterPleasureChoicePleasureEvent) {
				return AfterChoice::Redo;
			}
			if (eventName == kAfterPleasureChoiceFinishEvent) {
				return AfterChoice::Finish;
			}
			if (eventName == kAfterPleasureChoiceRecruitEvent) {
				return AfterChoice::Recruit;
			}
			if (eventName == kAfterPleasureChoiceJoinEnemyEvent) {
				return AfterChoice::JoinEnemy;
			}
			if (eventName == kAfterPleasureChoiceReleaseEvent) {
				return AfterChoice::Release;
			}
			if (eventName == kAfterPleasureChoiceWorkEvent) {
				return AfterChoice::Work;
			}
			if (eventName == kAfterPleasureChoiceKidnapEvent) {
				return AfterChoice::Captive;
			}
			return AfterChoice::None;
		}

		bool IsValidSceneStartSource(SourceContext source)
		{
			switch (source) {
			case SourceContext::PreCombat:
			case SourceContext::InCombat:
			case SourceContext::Bleedout:
			case SourceContext::Captive:
			case SourceContext::Teammate:
				return true;
			default:
				return false;
			}
		}

		RE::Actor* LookupActor(std::uint32_t formID)
		{
			return formID ? RE::TESForm::LookupByID<RE::Actor>(formID) : nullptr;
		}

		RE::TESFaction* ResolveAfterPleasureFaction()
		{
			static RE::TESFaction* faction = nullptr;
			static bool tried = false;
			if (!tried) {
				tried = true;
				faction = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDAfterPleasureFaction");
				if (!faction) {
					spdlog::warn("[TFD][PleasureRuntime][R109] TFDAfterPleasureFaction unresolved");
				}
			}
			return faction;
		}

		RE::TESFaction* ResolvePleasureFailedFaction()
		{
			static RE::TESFaction* faction = nullptr;
			static bool tried = false;
			if (!tried) {
				tried = true;
				faction = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDPleasureFailedFaction");
				if (!faction) {
					spdlog::warn("[TFD][PleasureRuntime][R203A] TFDPleasureFailedFaction unresolved");
				}
			}
			return faction;
		}

		void RemoveAfterPleasureFaction(RE::Actor* actor, std::string_view reason)
		{
			auto* faction = ResolveAfterPleasureFaction();
			if (!actor || !faction) {
				return;
			}
			if (actor->IsInFaction(faction)) {
				actor->RemoveFromFaction(faction);
				spdlog::info(
					"[TFD][PleasureRuntime][R109] after pleasure faction removed actor={:08X} reason={}",
					actor->GetFormID(),
					reason.empty() ? std::string{ "-" } : std::string{ reason });
			}
		}

		void RemovePleasureFailedFaction(RE::Actor* actor, std::string_view reason)
		{
			auto* faction = ResolvePleasureFailedFaction();
			if (!actor || !faction) {
				return;
			}
			if (actor->IsInFaction(faction)) {
				actor->RemoveFromFaction(faction);
				spdlog::info(
					"[TFD][PleasureRuntime][R203A] pleasure failed faction removed actor={:08X} reason={}",
					actor->GetFormID(),
					reason.empty() ? std::string{ "-" } : std::string{ reason });
			}
		}

		void PrepareAfterPleasurePackageActor(RE::Actor* actor, std::string_view reason)
		{
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				return;
			}

			auto* faction = ResolveAfterPleasureFaction();
			if (faction && !actor->IsInFaction(faction)) {
				actor->AddToFaction(faction, 0);
			}

			if (!actor->IsAIEnabled()) {
				actor->EnableAI(true);
			}
			actor->AllowPCDialogue(true);
			actor->StopCombat();
			if (auto* process = RE::ProcessLists::GetSingleton()) {
				process->StopCombatAndAlarmOnActor(actor, false);
			}
			// R240A: Do not force weapon sheathe here. Once combat/alarm/pacify
			// state is correct, Skyrim will settle the weapon naturally. Forced
			// sheathe can break the animation graph: idle posture with weapon still
			// in hand, then hostile actors cannot attack after Fight.
			actor->EvaluatePackage(false, true);
			actor->EvaluatePackage(true, true);

			spdlog::info(
				"[TFD][PleasureRuntime][R109] after pleasure package actor prepared actor={:08X} factionApplied={} reason={}",
				actor->GetFormID(),
				faction ? 1 : 0,
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}

		void PreparePleasureFailedPackageActor(RE::Actor* actor, std::string_view reason)
		{
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				return;
			}

			// R203A: Pleasure Failed is not generic AfterPleasure.  Give it its
			// own CK faction/package route and explicitly strip the old passive
			// AfterPleasure marker so the speaker does not inherit the sheathe/idle
			// package before the user chooses Fight.
			RemoveAfterPleasureFaction(actor, reason.empty() ? "pleasure_failed_prepare_strip_after" : reason);

			auto* faction = ResolvePleasureFailedFaction();
			if (faction && !actor->IsInFaction(faction)) {
				actor->AddToFaction(faction, 0);
			}

			if (!actor->IsAIEnabled()) {
				actor->EnableAI(true);
			}
			actor->AllowPCDialogue(true);

			// R253A: PleasureFailed is opened by native hard-topic forcegreet, not by
			// an AI package.  BO11/R252A proved the drawn-state predicate was too late:
			// after OStim ends, the actor can already report weaponDrawn=false even
			// though the animation/package graph is still in the fragile handoff that
			// later becomes idle-with-weapon-in-hand.  Do not StopCombatAlarm or
			// EvaluatePackage for any PleasureFailed speaker.  Keep the dialogue marker
			// and let the hard topic open directly; Fight owns the later combat handoff.
			const bool wasInCombat = actor->IsInCombat();
			const bool wasWeaponDrawn = actor->IsWeaponDrawn();
			const bool preserveDrawnPosture = true;
			const bool stoppedCombat = false;
			const bool stoppedAlarm = false;
			const bool evaluatedPackage = false;

			// No forced sheathe/draw, no StopCombatAlarm, and no package settle during
			// PleasureFailed.  The dialogue opener forces the topic directly and Fight
			// removes TFDPleasureFailedFaction before combat refresh.

			spdlog::info(
				"[TFD][PleasureRuntime][R253A] pleasure failed package actor prepared actor={:08X} factionApplied={} wasInCombat={} wasWeaponDrawn={} preserveDrawnPosture={} stoppedCombat={} stoppedAlarm={} packageEval={} weaponDrawnNow={} forcedSheathe=0 reason={}",
				actor->GetFormID(),
				faction ? 1 : 0,
				wasInCombat ? 1 : 0,
				wasWeaponDrawn ? 1 : 0,
				preserveDrawnPosture ? 1 : 0,
				stoppedCombat ? 1 : 0,
				stoppedAlarm ? 1 : 0,
				evaluatedPackage ? 1 : 0,
				actor->IsWeaponDrawn() ? 1 : 0,
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}

		void ReleaseAfterPleasurePackageActor(RE::Actor* actor, std::string_view reason)
		{
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				return;
			}

			RemoveAfterPleasureFaction(actor, reason.empty() ? "after_pleasure_terminal_cleanup" : reason);
			RemovePleasureFailedFaction(actor, reason.empty() ? "after_pleasure_terminal_cleanup" : reason);
			if (!actor->IsAIEnabled()) {
				actor->EnableAI(true);
			}
			actor->AllowPCDialogue(true);
			actor->EvaluatePackage(true, false);

			spdlog::info(
				"[TFD][PleasureRuntime][R109] after pleasure package actor released actor={:08X} reason={}",
				actor->GetFormID(),
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}

		double NowSec()
		{
			using Clock = std::chrono::steady_clock;
			static const auto start = Clock::now();
			return std::chrono::duration<double>(Clock::now() - start).count();
		}

		struct QueuedPreCombatAttempt
		{
			std::uint32_t actorFormID{ 0 };
			std::uint32_t consumedActorFormID{ 0 };
			SourceContext source{ SourceContext::None };
			bool bleedoutBridgeToInCombat{ false };
			unsigned attemptIndex{ 0 };
			bool valid{ false };
		};

		struct QueuedTerminalNeutralFinalize
		{
			std::uint32_t actorFormID{ 0 };
			std::uint32_t cycleId{ 0 };
			SourceContext source{ SourceContext::None };
			bool valid{ false };
		};

		struct QueuedAfterPleasureDialogueTimeout
		{
			std::uint32_t actorFormID{ 0 };
			std::uint32_t cycleId{ 0 };
			SourceContext source{ SourceContext::None };
			bool valid{ false };
		};

		struct QueuedAbortedFlowComplete
		{
			std::uint32_t actorFormID{ 0 };
			std::uint32_t cycleId{ 0 };
			SourceContext source{ SourceContext::None };
			bool valid{ false };
		};

		void ClearQueuedPreCombatCycleLocked(std::string_view reason)
		{
			const auto oldActor = g_state.queuedPreCombatSpeakerFormID;
			const auto oldConsumed = g_state.queuedPreCombatConsumedFormID;
			const auto oldAttempts = g_state.queuedPreCombatAttempts;

			g_state.queuedPreCombatSpeakerFormID = 0;
			g_state.queuedPreCombatConsumedFormID = 0;
			g_state.queuedPreCombatSource = SourceContext::None;
			g_state.queuedPreCombatBleedoutBridgeToInCombat = false;
			g_state.queuedPreCombatNextTrySec = 0.0;
			g_state.queuedPreCombatExpireSec = 0.0;
			g_state.queuedPreCombatAttempts = 0;

			if (oldActor != 0 || oldConsumed != 0) {
				spdlog::info(
					"[TFD][PleasureRuntime] after pleasure cycle queue cleared actor={:08X} consumed={:08X} attempts={} reason={}",
					oldActor,
					oldConsumed,
					oldAttempts,
					reason.empty() ? std::string{ "-" } : std::string{ reason });
			}
		}

		void ClearQueuedTerminalNeutralFinalizeLocked(std::string_view reason)
		{
			const auto oldActor = g_state.queuedTerminalNeutralActorFormID;
			const auto oldCycle = g_state.queuedTerminalNeutralCycleId;

			g_state.queuedTerminalNeutralFinalize = false;
			g_state.queuedTerminalNeutralActorFormID = 0;
			g_state.queuedTerminalNeutralCycleId = 0;
			g_state.queuedTerminalNeutralDueSec = 0.0;
			g_state.queuedTerminalNeutralSource = SourceContext::None;

			if (oldActor != 0 || oldCycle != 0) {
				spdlog::info(
					"[TFD][PleasureRuntime] terminal neutral finalize queue cleared actor={:08X} cycle={} reason={}",
					oldActor,
					oldCycle,
					reason.empty() ? std::string{ "-" } : std::string{ reason });
			}
		}

		void ArmTerminalNeutralFinalizeLocked(RE::Actor* consumedActor, std::string_view reason, SourceContext sourceOverride = SourceContext::None)
		{
			const SourceContext source = sourceOverride != SourceContext::None ? sourceOverride : g_state.source;
			if ((source != SourceContext::PreCombat && source != SourceContext::InCombat && source != SourceContext::Bleedout) || !consumedActor) {
				return;
			}

			g_state.queuedTerminalNeutralFinalize = true;
			g_state.queuedTerminalNeutralActorFormID = consumedActor->GetFormID();
			g_state.queuedTerminalNeutralCycleId = g_state.sessionCycleId;
			g_state.queuedTerminalNeutralDueSec = NowSec() + kTerminalNeutralFinalizeDelaySec;
			g_state.queuedTerminalNeutralSource = source;

			spdlog::info(
				"[TFD][PleasureRuntime] terminal neutral finalize queued actor={:08X} cycle={} source={} delay={:.2f}s reason={}",
				g_state.queuedTerminalNeutralActorFormID,
				g_state.queuedTerminalNeutralCycleId,
				ToString(g_state.queuedTerminalNeutralSource),
				kTerminalNeutralFinalizeDelaySec,
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}

		bool IsDeferredPleasureChainSourceLocked()
		{
			return g_state.source == SourceContext::InCombat ||
				g_state.source == SourceContext::Bleedout ||
				g_state.queuedPreCombatSource == SourceContext::InCombat ||
				g_state.queuedPreCombatSource == SourceContext::Bleedout ||
				g_state.queuedTerminalNeutralSource == SourceContext::InCombat ||
				g_state.queuedTerminalNeutralSource == SourceContext::Bleedout;
		}

		bool IsInCombatPleasureChainActiveLocked()
		{
			return IsDeferredPleasureChainSourceLocked() &&
				(g_state.active ||
					g_state.blocking ||
					g_state.passiveLockActive ||
					g_state.holdActive ||
					g_state.queuedPreCombatSpeakerFormID != 0 ||
					g_state.queuedTerminalNeutralFinalize ||
					g_state.abortedFlowCompleteQueued ||
					!g_state.deferredInCombatRecruitActorIds.empty());
		}

		void DeferInCombatRecruitAliasLocked(RE::Actor* actor, std::string_view reason)
		{
			if (!actor) {
				return;
			}

			const auto actorId = actor->GetFormID();
			if (actorId == 0) {
				return;
			}

			if (std::find(g_state.deferredInCombatRecruitActorIds.begin(), g_state.deferredInCombatRecruitActorIds.end(), actorId) == g_state.deferredInCombatRecruitActorIds.end()) {
				g_state.deferredInCombatRecruitActorIds.push_back(actorId);
			}

			spdlog::info(
				"[TFD][PleasureRuntime][R94G] deferred incombat recruit alias actor={:08X} deferredCount={} cycle={} reason={}",
				actorId,
				static_cast<unsigned int>(g_state.deferredInCombatRecruitActorIds.size()),
				g_state.sessionCycleId,
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}

		bool RemoveDeferredInCombatRecruitIdLocked(std::uint32_t actorId, std::string_view reason)
		{
			if (actorId == 0) {
				return false;
			}

			const auto before = g_state.deferredInCombatRecruitActorIds.size();
			g_state.deferredInCombatRecruitActorIds.erase(
				std::remove(g_state.deferredInCombatRecruitActorIds.begin(), g_state.deferredInCombatRecruitActorIds.end(), actorId),
				g_state.deferredInCombatRecruitActorIds.end());
			const bool removed = g_state.deferredInCombatRecruitActorIds.size() != before;
			if (removed) {
				spdlog::info(
					"[TFD][PleasureRuntime][R96D] deferred incombat recruit removed actor={:08X} remaining={} reason={}",
					actorId,
					static_cast<unsigned int>(g_state.deferredInCombatRecruitActorIds.size()),
					reason.empty() ? std::string{ "-" } : std::string{ reason });
			}
			return removed;
		}

		std::vector<std::uint32_t> TakeDeferredInCombatRecruitIdsLocked(std::string_view reason)
		{
			std::vector<std::uint32_t> ids = std::move(g_state.deferredInCombatRecruitActorIds);
			g_state.deferredInCombatRecruitActorIds.clear();

			if (!ids.empty()) {
				spdlog::info(
					"[TFD][PleasureRuntime][R94G] take deferred incombat recruits count={} reason={}",
					static_cast<unsigned int>(ids.size()),
					reason.empty() ? std::string{ "-" } : std::string{ reason });
			}

			return ids;
		}

		void ClearSpeakerStateLocked()
		{
			if (auto* actor = LookupActor(g_state.afterPleasureSpeakerFormID ? g_state.afterPleasureSpeakerFormID : g_state.pleasureSpeakerFormID)) {
				RemoveAfterPleasureFaction(actor, "clear_speaker_state");
				RemovePleasureFailedFaction(actor, "clear_speaker_state");
			}
			g_state.pleasureSpeakerFormID = 0;
			g_state.afterPleasureSpeakerFormID = 0;
		}

		void ClearBridgeStateLocked()
		{
			g_state.ostimThreadId = static_cast<std::uint32_t>(-1);
			g_state.bridgeState = 0;
			g_state.afterPleasureCommitted = false;
			g_state.pleasureFailedDialogueActive = false;
			g_state.afterPleasureDialogueExpireSec = 0.0;
			g_state.pleasureStartPendingAtSec = 0.0;
			g_state.pleasureActiveStartedAtSec = 0.0;
			g_state.scenePassiveHoldNextPulseSec = 0.0;
			g_state.scenePassiveHoldPulseCount = 0;
			g_state.truceFlowHandoffRefreshNextSec = 0.0;
		}

		void ClearHoldStateLocked(bool preserveAggressionClampForBridge = false)
		{
			if (g_state.holdActive || g_state.passiveLockActive) {
				if (preserveAggressionClampForBridge) {
					spdlog::info(
						"[TFD][PleasureRuntime][R471A] aggression clamp clear deferred for bleedout recruit bridge cycle={} source={} queuedNext={:08X} consumed={:08X}",
						g_state.sessionCycleId,
						ToString(g_state.source),
						g_state.queuedPreCombatSpeakerFormID,
						g_state.queuedPreCombatConsumedFormID);
				}
				else {
					TFD::HostilityController::ClearAggressionClamp();
				}
			}
			g_state.holdActive = false;
			g_state.passiveLockActive = false;
			g_state.blocking = false;
			g_state.scenePassiveHoldNextPulseSec = 0.0;
			g_state.scenePassiveHoldPulseCount = 0;
			g_state.truceFlowHandoffRefreshNextSec = 0.0;
		}

		void ResetStateLocked(std::string_view reason)
		{
			const auto oldPhase = g_state.phase;
			const auto oldCycle = g_state.sessionCycleId;

			g_state.phase = Phase::Idle;
			g_state.source = SourceContext::None;
			g_state.active = false;
			g_state.redoPending = false;
			g_state.flowOwnerToken = 0;
			g_state.pendingChoice = AfterChoice::None;
			g_state.abortedFlowCompleteQueued = false;
			g_state.abortedFlowActorFormID = 0;
			g_state.abortedFlowCompleteDueSec = 0.0;
			g_state.deferredInCombatRecruitActorIds.clear();
			g_state.inCombatContinuationFromBleedoutSnapshot = false;

			ClearSpeakerStateLocked();
			ClearBridgeStateLocked();
			ClearHoldStateLocked();
			ClearQueuedPreCombatCycleLocked(reason.empty() ? "reset" : reason);
			ClearQueuedTerminalNeutralFinalizeLocked(reason.empty() ? "reset" : reason);

			spdlog::info(
				"[TFD][PleasureRuntime] Reset reason={} oldPhase={} oldCycle={}",
				reason.empty() ? std::string{ "-" } : std::string{ reason },
				ToString(oldPhase),
				oldCycle);
		}

		void AdvancePhaseLocked(Phase next, std::string_view reason)
		{
			const auto prev = g_state.phase;
			g_state.phase = next;

			if (next == Phase::PleasureStartPending) {
				g_state.pleasureStartPendingAtSec = NowSec();
				g_state.pleasureActiveStartedAtSec = 0.0;
				g_state.scenePassiveHoldNextPulseSec = 0.0;
				g_state.scenePassiveHoldPulseCount = 0;
				g_state.truceFlowHandoffRefreshNextSec = 0.0;
			}
			else if (next == Phase::PleasureActive) {
				g_state.pleasureActiveStartedAtSec = NowSec();
				g_state.scenePassiveHoldNextPulseSec = 0.0;
				g_state.scenePassiveHoldPulseCount = 0;
			}

			if (next == Phase::Idle || next == Phase::Closed) {
				g_state.active = false;
			}
			else {
				g_state.active = true;
			}

			spdlog::info(
				"[TFD][PleasureRuntime] phase {} -> {} reason={} cycle={} source={}",
				ToString(prev),
				ToString(next),
				reason.empty() ? std::string{ "-" } : std::string{ reason },
				g_state.sessionCycleId,
				ToString(g_state.source));
		}

		void BeginNewCycleLocked(RE::Actor* speaker, SourceContext source, std::string_view reason)
		{
			if (auto* oldActor = LookupActor(g_state.afterPleasureSpeakerFormID ? g_state.afterPleasureSpeakerFormID : g_state.pleasureSpeakerFormID)) {
				RemoveAfterPleasureFaction(oldActor, "begin_new_cycle");
				RemovePleasureFailedFaction(oldActor, "begin_new_cycle");
			}
			++g_state.sessionCycleId;
			g_state.source = source;
			g_state.flowOwnerToken = g_state.sessionCycleId;
			g_state.redoPending = false;
			g_state.pendingChoice = AfterChoice::None;
			g_state.afterPleasureCommitted = false;
			g_state.pleasureFailedDialogueActive = false;
			g_state.ostimThreadId = static_cast<std::uint32_t>(-1);
			g_state.bridgeState = 0;
			g_state.pleasureStartPendingAtSec = 0.0;
			g_state.pleasureActiveStartedAtSec = 0.0;
			g_state.abortedFlowCompleteQueued = false;
			g_state.abortedFlowActorFormID = 0;
			g_state.abortedFlowCompleteDueSec = 0.0;
			g_state.pleasureSpeakerFormID = speaker ? speaker->GetFormID() : 0;
			g_state.afterPleasureSpeakerFormID = 0;
			g_state.active = true;
			g_state.holdActive = false;
			g_state.passiveLockActive = false;
			g_state.blocking = false;
			g_state.truceFlowHandoffRefreshNextSec = 0.0;
			ClearQueuedPreCombatCycleLocked("new_cycle");
			ClearQueuedTerminalNeutralFinalizeLocked("new_cycle");

			spdlog::info(
				"[TFD][PleasureRuntime] BeginCycle cycle={} source={} speaker={:08X} reason={}",
				g_state.sessionCycleId,
				ToString(source),
				g_state.pleasureSpeakerFormID,
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}

		void PrepareRedoSpeakerSwitchLocked(RE::Actor* redoActor, std::uint32_t requestedActorID, std::string_view reason)
		{
			const auto oldPleasureID = g_state.pleasureSpeakerFormID;
			const auto oldAfterID = g_state.afterPleasureSpeakerFormID;
			const auto newActorID = redoActor ? redoActor->GetFormID() : requestedActorID;

			std::vector<std::uint32_t> staleActorIDs{};
			if (oldAfterID != 0 && oldAfterID != newActorID) {
				staleActorIDs.push_back(oldAfterID);
			}
			if (oldPleasureID != 0 && oldPleasureID != newActorID &&
				std::find(staleActorIDs.begin(), staleActorIDs.end(), oldPleasureID) == staleActorIDs.end()) {
				staleActorIDs.push_back(oldPleasureID);
			}

			std::uint32_t released = 0;
			for (auto actorID : staleActorIDs) {
				auto* staleActor = LookupActor(actorID);
				if (!staleActor || staleActor->IsDead() || staleActor->IsDisabled()) {
					continue;
				}
				ReleaseAfterPleasurePackageActor(staleActor, reason.empty() ? "after_pleasure_redo_actor_switch" : reason);
				++released;
			}

			if (newActorID != 0) {
				g_state.pleasureSpeakerFormID = newActorID;
			}
			g_state.afterPleasureSpeakerFormID = 0;
			g_state.ostimThreadId = static_cast<std::uint32_t>(-1);

			spdlog::info(
				"[TFD][PleasureRuntime][C51] redo speaker switch prepared oldPleasure={:08X} oldAfter={:08X} new={:08X} released={} reason={}",
				oldPleasureID,
				oldAfterID,
				newActorID,
				released,
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}

		bool IsAfterPleasureChoiceEvent(std::string_view eventName)
		{
			return eventName == kAfterPleasureChoiceFinishEvent ||
				eventName == kAfterPleasureChoiceRecruitEvent ||
				eventName == kAfterPleasureChoiceJoinEnemyEvent ||
				eventName == kAfterPleasureChoicePleasureEvent ||
				eventName == kAfterPleasureChoiceReleaseEvent ||
				eventName == kAfterPleasureChoiceWorkEvent ||
				eventName == kAfterPleasureChoiceKidnapEvent;
		}

		bool IsRecognizedEvent(std::string_view eventName)
		{
			return eventName == kPleasureStartPendingEvent ||
				eventName == kPleasureStartedEvent ||
				eventName == kPleasureFailedEvent ||
				eventName == kPleasureAbortedEvent ||
				eventName == kPleasureEndedEvent ||
				eventName == kOStimSceneStartPendingEvent ||
				eventName == kOStimSceneStartedEvent ||
				eventName == kOStimSceneSucceededEvent ||
				eventName == kOStimSceneEndedEvent ||
				eventName == kAfterPleasureEnterEvent ||
				eventName == kAfterPleasureLoopEnterEvent ||
				eventName == kPleasureFailedEnterEvent ||
				eventName == kPleasureClearEvent ||
				IsAfterPleasureChoiceEvent(eventName);
		}


		bool IsTrackedActorFormIDLocked(std::uint32_t formID)
		{
			if (formID == 0) {
				return false;
			}

			return formID == g_state.pleasureSpeakerFormID || formID == g_state.afterPleasureSpeakerFormID;
		}

		bool ShouldProtectPendingDialogueLocked(std::uint32_t formID)
		{
			if (g_state.source != SourceContext::PreCombat) {
				return false;
			}

			if (!IsTrackedActorFormIDLocked(formID)) {
				return false;
			}

			switch (g_state.phase) {
			case Phase::PleasureStartPending:
			case Phase::PleasureActive:
			case Phase::PleasureEnding:
			case Phase::AfterPleasureAwaitQuest:
			case Phase::AfterPleasureDialogue:
			case Phase::PleasureFailedDialogue:
			case Phase::RedoPending:
			case Phase::Finalizing:
				return true;
			default:
				return false;
			}
		}

		bool ShouldSuppressTruceDialogueLocked(std::uint32_t formID)
		{
			if (formID == 0) {
				return false;
			}

			if (!g_state.active && !g_state.blocking && !g_state.passiveLockActive && !g_state.holdActive) {
				return false;
			}

			switch (g_state.phase) {
			case Phase::PleasureStartPending:
			case Phase::PleasureActive:
			case Phase::PleasureEnding:
			case Phase::AfterPleasureAwaitQuest:
			case Phase::AfterPleasureDialogue:
			case Phase::PleasureFailedDialogue:
			case Phase::RedoPending:
				return true;
			default:
				return false;
			}
		}

		std::uint32_t ParseActorFormIDToken(const std::string& token, std::uint32_t fallback = 0)
		{
			if (token.empty()) {
				return fallback;
			}

			try {
				if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) {
					return static_cast<std::uint32_t>(std::stoul(token, nullptr, 16));
				}

				for (char ch : token) {
					if ((ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f')) {
						return static_cast<std::uint32_t>(std::stoul(token, nullptr, 16));
					}
				}

				return static_cast<std::uint32_t>(std::stoul(token, nullptr, 10));
			}
			catch (...) {
				return fallback;
			}
		}

		int ParseIntToken(const std::string& token, int fallback = 0)
		{
			if (token.empty()) {
				return fallback;
			}

			try {
				return std::stoi(token);
			}
			catch (...) {
				return fallback;
			}
		}

		EventInfo ParseEventInfo(const RE::BSFixedString& strArg, float numArg, RE::TESForm* sender)
		{
			EventInfo info{};
			info.senderFormID = sender ? sender->GetFormID() : 0;

			if (sender) {
				info.actor = sender->As<RE::Actor>();
				if (info.actor) {
					info.actorFormID = info.actor->GetFormID();
				}
			}

			if (std::isfinite(numArg)) {
				info.threadID = static_cast<int>(std::lround(numArg));
			}

			if (strArg.empty()) {
				return info;
			}

			std::string token = strArg.c_str();
			auto first = token.find('|');
			auto second = token.find('|', first == std::string::npos ? first : first + 1);

			if (first != std::string::npos) {
				const auto a = token.substr(0, first);
				if (!a.empty()) {
					info.actorFormID = ParseActorFormIDToken(a, info.actorFormID);
				}

				if (second != std::string::npos) {
					const auto b = token.substr(first + 1, second - first - 1);
					const auto c = token.substr(second + 1);
					if (!b.empty()) {
						info.sourceFlow = ParseIntToken(b, info.sourceFlow);
					}
					if (!c.empty()) {
						info.threadID = ParseIntToken(c, info.threadID);
					}
				}
				else {
					const auto b = token.substr(first + 1);
					if (!b.empty()) {
						info.sourceFlow = ParseIntToken(b, info.sourceFlow);
					}
				}
			}
			else {
				info.actorFormID = ParseActorFormIDToken(token, info.actorFormID);
			}

			if (!info.actor && info.actorFormID) {
				info.actor = LookupActor(info.actorFormID);
			}

			return info;
		}

		void LogEventIgnoredLocked(std::string_view eventName, std::string_view reason, const EventInfo& info)
		{
			spdlog::info(
				"[TFD][PleasureRuntime] ignored event={} phase={} cycle={} actor={:08X} flow={} thread={} reason={}",
				eventName,
				ToString(g_state.phase),
				g_state.sessionCycleId,
				info.actorFormID,
				info.sourceFlow,
				info.threadID,
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}

		void LogEventAcceptedLocked(std::string_view eventName, const EventInfo& info, std::string_view detail = {})
		{
			spdlog::info(
				"[TFD][PleasureRuntime] accepted event={} phase={} cycle={} actor={:08X} flow={} thread={} detail={}",
				eventName,
				ToString(g_state.phase),
				g_state.sessionCycleId,
				info.actorFormID,
				info.sourceFlow,
				info.threadID,
				detail.empty() ? std::string{ "-" } : std::string{ detail });
		}

		void SuppressActorDialogueForSceneLocked(RE::Actor* actor, std::string_view reason, double cooldownSec = kSuppressSceneStartDialogueCooldownSec)
		{
			if (!actor) {
				return;
			}

			const std::string reasonText = reason.empty() ? "pleasure_scene_dialogue_suppress" : std::string{ reason };

			// Keep PC dialogue disabled after a teammate-driven pleasure transition.
			// Manual teammate interaction explicitly re-enables it in MenuFramework before
			// hard-opening the root topic, while stale engine forcegreet requests do not.
			// This prevents the delayed teammate dialogue reopen caused by the manual
			// SetDialogueWithPlayer(..., forceGreet=true, teammateInfo) path.
			actor->SetDialogueWithPlayer(false, false, nullptr);
			actor->AllowPCDialogue(false);
			TFD::InteractionRouter::DialogueOpen::ArmTemporaryDialogueCooldown(
				actor,
				cooldownSec,
				reasonText.c_str());

			spdlog::info(
				"[TFD][PleasureRuntime] suppress actor dialogue actor={:08X} cooldown={:.2f}s phase={} cycle={} reason={}",
				actor->GetFormID(),
				cooldownSec,
				ToString(g_state.phase),
				g_state.sessionCycleId,
				reasonText);
		}

		void PrepareActorForScenePassiveLocked(RE::Actor* actor, std::string_view reason, bool allowPackageEvaluation = true)
		{
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				return;
			}

			const std::string reasonText = reason.empty() ? "pleasure_scene_passive_lock" : std::string{ reason };
			TFD::HostilityController::ApplyAggressionClamp(actor);
			if (auto* process = RE::ProcessLists::GetSingleton()) {
				const bool oldRunDetection = process->runDetection;
				process->runDetection = false;
				process->ClearCachedFactionFightReactions();
				process->StopCombatAndAlarmOnActor(actor, false);
				process->runDetection = oldRunDetection;
			}
			actor->StopCombat();
			actor->StopAlarmOnActor();
			// R498A: Before QuickStart, package evaluation may settle the combat handoff.
			// Once OStim reports a real scene start, OStim owns actor positioning and
			// animation; evaluating the package at that point can terminate the scene.
			if (allowPackageEvaluation) {
				actor->EvaluatePackage(true, false);
			}
			TFD::HostilityController::ScheduleStopCombatWaves(2400.0f, false, 5, 90);

			spdlog::info(
				"[TFD][PleasureRuntime] scene passive lock actor={:08X} source={} phase={} cycle={} packageEval={} reason={}",
				actor->GetFormID(),
				ToString(g_state.source),
				ToString(g_state.phase),
				g_state.sessionCycleId,
				allowPackageEvaluation ? 1 : 0,
				reasonText);
		}

		bool IsScenePassiveHoldSource(SourceContext source)
		{
			// R454A: Bleedout-source Pleasure must remain pacified until the
			// PleasureFailed dialogue/outcome decides whether combat resumes.
			// InCombat uses the same shared lifecycle but owns an existing truce
			// session that R499A renews until a terminal outcome commits.
			return source == SourceContext::InCombat || source == SourceContext::Bleedout;
		}

		bool IsBleedoutSourceClearPhase(Phase phase)
		{
			switch (phase) {
			case Phase::PleasureStartPending:
			case Phase::PleasureActive:
			case Phase::PleasureEnding:
			case Phase::RedoPending:
			case Phase::Finalizing:
				return true;
			default:
				return false;
			}
		}

		bool IsScenePassiveHoldSourceLocked()
		{
			return IsScenePassiveHoldSource(g_state.source);
		}

		bool IsResultDecisionPhaseLocked()
		{
			return g_state.phase == Phase::AfterPleasureAwaitQuest ||
				g_state.phase == Phase::AfterPleasureDialogue ||
				g_state.phase == Phase::PleasureFailedDialogue;
		}

		bool IsInCombatTruceLifecyclePhaseLocked()
		{
			if (g_state.source != SourceContext::InCombat) {
				return false;
			}

			switch (g_state.phase) {
			case Phase::PleasureStartPending:
			case Phase::PleasureActive:
			case Phase::PleasureEnding:
			case Phase::AfterPleasureAwaitQuest:
			case Phase::AfterPleasureDialogue:
			case Phase::PleasureFailedDialogue:
			case Phase::RedoPending:
				return true;
			default:
				return false;
			}
		}

		RE::Actor* ResolveInCombatTruceLifecycleActorLocked()
		{
			if (IsResultDecisionPhaseLocked() && g_state.afterPleasureSpeakerFormID != 0) {
				if (auto* actor = LookupActor(g_state.afterPleasureSpeakerFormID)) {
					return actor;
				}
			}
			return LookupActor(g_state.pleasureSpeakerFormID);
		}

		void RefreshInCombatTruceLifecycleHoldLocked(double now)
		{
			if (!IsInCombatTruceLifecyclePhaseLocked()) {
				g_state.truceFlowHandoffRefreshNextSec = 0.0;
				return;
			}

			if (g_state.truceFlowHandoffRefreshNextSec > 0.0 && now < g_state.truceFlowHandoffRefreshNextSec) {
				return;
			}

			auto* actor = ResolveInCombatTruceLifecycleActorLocked();
			const bool refreshed = actor && TFD::HostilityController::RefreshTruceSessionForFlowHandoff(
				actor,
				kInCombatTruceRefreshDurationSec,
				"r499a_incombat_pleasure_lifecycle");

			g_state.truceFlowHandoffRefreshNextSec = now +
				(refreshed ? kInCombatTruceRefreshIntervalSec : kInCombatTruceRefreshRetrySec);

			if (refreshed) {
				spdlog::info(
					"[TFD][PleasureRuntime][R499A] InCombat truce lifecycle hold refreshed actor={:08X} phase={} cycle={} next={:.2f}s",
					actor ? actor->GetFormID() : 0u,
					ToString(g_state.phase),
					g_state.sessionCycleId,
					kInCombatTruceRefreshIntervalSec);
			}
		}

		bool ShouldMaintainScenePassiveHoldLocked()
		{
			if (!g_state.holdActive || !IsScenePassiveHoldSourceLocked()) {
				return false;
			}

			// Result dialogue is already the owner. Keep only the lightweight
			// aggression guard even when the old scene passiveLock flag is off.
			if (IsResultDecisionPhaseLocked()) {
				return true;
			}

			if (!g_state.passiveLockActive) {
				return false;
			}

			return g_state.phase == Phase::PleasureStartPending ||
				g_state.phase == Phase::PleasureActive ||
				g_state.phase == Phase::PleasureEnding;
		}

		void MaintainScenePassiveHoldLocked(double now)
		{
			if (!ShouldMaintainScenePassiveHoldLocked()) {
				return;
			}

			if (g_state.scenePassiveHoldNextPulseSec > 0.0 && now < g_state.scenePassiveHoldNextPulseSec) {
				return;
			}

			const bool resultDialogueLightHold = IsResultDecisionPhaseLocked();
			const auto actorFormID = resultDialogueLightHold && g_state.afterPleasureSpeakerFormID != 0 ?
				g_state.afterPleasureSpeakerFormID :
				g_state.pleasureSpeakerFormID;
			auto* actor = LookupActor(actorFormID);
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				g_state.scenePassiveHoldNextPulseSec = now + kScenePassiveHoldPendingPulseSec;
				return;
			}

			if (resultDialogueLightHold) {
				// Native ForceGreet owns the result dialogue. Do not clear fight
				// reactions, stop combat/alarm, evaluate packages, or schedule waves
				// while DialogueMenu is alive. Hooks and the session faction still
				// block reacquisition; this only keeps aggression clamped.
				TFD::HostilityController::ApplyAggressionClamp(actor);
				++g_state.scenePassiveHoldPulseCount;
				g_state.scenePassiveHoldNextPulseSec = now + kScenePassiveHoldActivePulseSec;

				if (g_state.scenePassiveHoldPulseCount == 1 || (g_state.scenePassiveHoldPulseCount % 4) == 0) {
					spdlog::info(
						"[TFD][PleasureRuntime][R499A] result dialogue light hold actor={:08X} source={} phase={} cycle={} count={} policy=aggression_only",
						actor->GetFormID(),
						ToString(g_state.source),
						ToString(g_state.phase),
						g_state.sessionCycleId,
						g_state.scenePassiveHoldPulseCount);
				}
				return;
			}

			TFD::HostilityController::ApplyAggressionClamp(actor);
			if (auto* process = RE::ProcessLists::GetSingleton()) {
				const bool oldRunDetection = process->runDetection;
				process->runDetection = false;
				process->ClearCachedFactionFightReactions();
				process->StopCombatAndAlarmOnActor(actor, false);
				process->runDetection = oldRunDetection;
			}
			actor->StopCombat();
			actor->StopAlarmOnActor();
			// R240A: Do not force weapon sheathe here. Once combat/alarm/pacify
			// state is correct, Skyrim will settle the weapon naturally.
			// R498A: The pending transition performs one deliberate pre-launch
			// package evaluation before QuickStart. Periodic hold pulses must not repeat
			// it while the asynchronous player thread is being created or animated.
			const bool ostimLifecycleOwnsActor =
				g_state.phase == Phase::PleasureStartPending ||
				g_state.phase == Phase::PleasureActive;
			if (!ostimLifecycleOwnsActor) {
				actor->EvaluatePackage(false, true);
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (player) {
				player->StopCombat();
			}

			const float waveRadius = g_state.source == SourceContext::Captive ? 2400.0f : 1800.0f;
			TFD::HostilityController::ScheduleStopCombatWaves(waveRadius, g_state.source == SourceContext::Captive, 3, 75);

			++g_state.scenePassiveHoldPulseCount;
			const double nextDelay = g_state.phase == Phase::PleasureStartPending ? kScenePassiveHoldPendingPulseSec : kScenePassiveHoldActivePulseSec;
			g_state.scenePassiveHoldNextPulseSec = now + nextDelay;

			if (g_state.scenePassiveHoldPulseCount == 1 || (g_state.scenePassiveHoldPulseCount % 4) == 0) {
				spdlog::info(
					"[TFD][PleasureRuntime][R454A] scene combat quarantine pulse actor={:08X} source={} phase={} cycle={} count={} next={:.2f}s",
					actor->GetFormID(),
					ToString(g_state.source),
					ToString(g_state.phase),
					g_state.sessionCycleId,
					g_state.scenePassiveHoldPulseCount,
					nextDelay);
			}
		}

		void HandleAfterPleasureLoopNoiseLocked(const EventInfo& info, std::string_view reason)
		{
			spdlog::info(
				"[TFD][PleasureRuntime] loop-noise ignored phase={} cycle={} actor={:08X} thread={} reason={}",
				ToString(g_state.phase),
				g_state.sessionCycleId,
				info.actorFormID,
				info.threadID,
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}

		bool IsValidPreCombatCycleCandidate(RE::Actor* candidate, RE::Actor* currentActor)
		{
			if (!candidate || candidate->IsDead() || candidate->IsDisabled() || !candidate->Is3DLoaded()) {
				return false;
			}

			if (currentActor && candidate->GetFormID() == currentActor->GetFormID()) {
				return false;
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (player && candidate->GetFormID() == player->GetFormID()) {
				return false;
			}

			if (TFD::Recruit::IsRecruitLike(candidate)) {
				return false;
			}

			return true;
		}

		void QueueTruceAliasUnassign(RE::Actor* actor, std::string_view reason)
		{
			if (!actor) {
				return;
			}

			const bool ok = TFD::FlowController::QueueBridgeModEvent("TFDTruceUnassign", actor);
			spdlog::info(
				"[TFD][PleasureRuntime] after pleasure cycle truce unassign actor={:08X} ok={} reason={}",
				actor->GetFormID(),
				ok ? 1 : 0,
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}

		void QueueTruceAliasPromoteSpeaker(RE::Actor* actor, std::string_view reason)
		{
			if (!actor) {
				return;
			}

			const bool ok = TFD::FlowController::QueueBridgeModEvent("TFDTrucePromoteSpeaker", actor);
			spdlog::info(
				"[TFD][PleasureRuntime] after pleasure cycle truce promote-speaker actor={:08X} ok={} reason={}",
				actor->GetFormID(),
				ok ? 1 : 0,
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}

		void QueuePreCombatAliasClear(RE::Actor* actor, std::string_view reason)
		{
			if (!actor) {
				return;
			}

			const bool ok = TFD::FlowController::QueueBridgeModEvent("TFDPreCombatClear", actor);
			spdlog::info(
				"[TFD][PleasureRuntime] after pleasure cycle precombat clear actor={:08X} ok={} reason={}",
				actor->GetFormID(),
				ok ? 1 : 0,
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}

		void QueueInCombatAliasClear(RE::Actor* actor, std::string_view reason)
		{
			if (!actor) {
				return;
			}

			const bool ok = TFD::FlowController::QueueBridgeModEvent("TFDInCombatClear", actor);
			spdlog::info(
				"[TFD][PleasureRuntime][R94B] after pleasure cycle incombat clear actor={:08X} ok={} reason={}",
				actor->GetFormID(),
				ok ? 1 : 0,
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}

		void AppendUniqueCycleActor(std::vector<RE::Actor*>& actors, RE::Actor* candidate)
		{
			if (!candidate) {
				return;
			}

			const auto candidateID = candidate->GetFormID();
			if (candidateID == 0) {
				return;
			}

			const auto found = std::find_if(
				actors.begin(),
				actors.end(),
				[candidateID](RE::Actor* existing) {
					return existing && existing->GetFormID() == candidateID;
				});
			if (found == actors.end()) {
				actors.push_back(candidate);
			}
		}

		void AppendUniqueCycleActorList(std::vector<RE::Actor*>& actors, const std::vector<RE::Actor*>& candidates)
		{
			for (auto* candidate : candidates) {
				AppendUniqueCycleActor(actors, candidate);
			}
		}

		RE::Actor* SelectNextBleedoutCycleActor(RE::Actor* currentActor, unsigned& scannedCount)
		{
			scannedCount = 0;

			std::vector<RE::Actor*> actors;
			const auto stagedCrowdIds = TFD::Bleedout::GetBleedCrowdAssignedIDs();
			for (const auto actorID : stagedCrowdIds) {
				AppendUniqueCycleActor(actors, RE::TESForm::LookupByID<RE::Actor>(actorID));
			}

			// CB07: Bleedout crowd is locked by the original defeat handoff. Do not
			// append HostilityController dialogue/active truce lists here; those lists can
			// include ambient actors from the native suppression session and can reintroduce
			// far/scripted actors that Bleedout already rejected.
			spdlog::info(
				"[TFD][PleasureRuntime][CB07] bleedout cycle using locked crowd only current={:08X} lockedCrowd={} reason=no_truce_recollect",
				currentActor ? currentActor->GetFormID() : 0u,
				static_cast<unsigned int>(stagedCrowdIds.size()));

			scannedCount = static_cast<unsigned>(actors.size());
			for (auto* candidate : actors) {
				if (IsValidPreCombatCycleCandidate(candidate, currentActor) &&
					TFD::Bleedout::IsPleasureCycleDialogueCandidate(candidate, currentActor)) {
					spdlog::info(
						"[TFD][PleasureRuntime][R133] bleedout cycle candidate selected current={:08X} next={:08X} stagedCrowd={} scanned={}",
						currentActor ? currentActor->GetFormID() : 0u,
						candidate->GetFormID(),
						static_cast<unsigned int>(stagedCrowdIds.size()),
						scannedCount);
					return candidate;
				}

				if (candidate && candidate->GetFormID() != 0) {
					spdlog::info(
						"[TFD][PleasureRuntime][R133] bleedout cycle candidate skipped current={:08X} candidate={:08X} stagedCrowd={} reason=not_dialogue_candidate",
						currentActor ? currentActor->GetFormID() : 0u,
						candidate->GetFormID(),
						static_cast<unsigned int>(stagedCrowdIds.size()));
				}
			}

			spdlog::info(
				"[TFD][PleasureRuntime][R133] bleedout cycle candidate not found current={:08X} stagedCrowd={} scanned={}",
				currentActor ? currentActor->GetFormID() : 0u,
				static_cast<unsigned int>(stagedCrowdIds.size()),
				scannedCount);
			return nullptr;
		}

		RE::Actor* SelectNextInCombatBleedoutSnapshotCycleActor(RE::Actor* currentActor, unsigned& scannedCount)
		{
			scannedCount = 0;

			const auto snapshotIds = TFD::Bleedout::GetBleedPleasureCrowdSnapshotIDs();
			if (snapshotIds.empty()) {
				return nullptr;
			}

			const auto currentID = currentActor ? currentActor->GetFormID() : RE::FormID{ 0 };
			const bool currentBelongsToSnapshot = currentID != 0 &&
				std::find(snapshotIds.begin(), snapshotIds.end(), currentID) != snapshotIds.end();
			if (!currentBelongsToSnapshot) {
				spdlog::info(
					"[TFD][PleasureRuntime][R505A] InCombat Bleedout snapshot fallback skipped current={:08X} snapshot={} reason=current_not_in_snapshot",
					currentID,
					static_cast<unsigned int>(snapshotIds.size()));
				return nullptr;
			}

			std::vector<RE::Actor*> actors;
			actors.reserve(snapshotIds.size());
			for (const auto actorID : snapshotIds) {
				AppendUniqueCycleActor(actors, RE::TESForm::LookupByID<RE::Actor>(actorID));
			}

			scannedCount = static_cast<unsigned>(actors.size());
			for (auto* candidate : actors) {
				if (IsValidPreCombatCycleCandidate(candidate, currentActor)) {
					spdlog::info(
						"[TFD][PleasureRuntime][R505A] InCombat Bleedout snapshot fallback selected current={:08X} next={:08X} scanned={} snapshot={}",
						currentID,
						candidate->GetFormID(),
						scannedCount,
						static_cast<unsigned int>(snapshotIds.size()));
					return candidate;
				}

				if (candidate && candidate->GetFormID() != 0) {
					spdlog::info(
						"[TFD][PleasureRuntime][R505A] InCombat Bleedout snapshot candidate skipped current={:08X} candidate={:08X} reason=invalid_or_recruit_like",
						currentID,
						candidate->GetFormID());
				}
			}

			spdlog::info(
				"[TFD][PleasureRuntime][R505A] InCombat Bleedout snapshot fallback exhausted current={:08X} scanned={} snapshot={}",
				currentID,
				scannedCount,
				static_cast<unsigned int>(snapshotIds.size()));
			return nullptr;
		}

		RE::Actor* SelectNextCycleActorForSource(RE::Actor* currentActor, SourceContext source, unsigned& scannedCount)
		{
			scannedCount = 0;

			if (source == SourceContext::Bleedout) {
				return SelectNextBleedoutCycleActor(currentActor, scannedCount);
			}

			auto actors = TFD::HostilityController::CollectDialogueTruceActors(currentActor);
			if (actors.empty()) {
				actors = TFD::HostilityController::CollectActiveTruceActors(currentActor);
			}

			scannedCount = static_cast<unsigned>(actors.size());
			for (auto* candidate : actors) {
				if (IsValidPreCombatCycleCandidate(candidate, currentActor)) {
					return candidate;
				}
			}

			if (source == SourceContext::InCombat && g_state.inCombatContinuationFromBleedoutSnapshot) {
				unsigned fallbackScanned = 0;
				auto* fallback = SelectNextInCombatBleedoutSnapshotCycleActor(currentActor, fallbackScanned);
				if (fallbackScanned > scannedCount) {
					scannedCount = fallbackScanned;
				}
				if (fallback) {
					return fallback;
				}
			}

			return nullptr;
		}

		RE::Actor* SelectNextBleedoutRecruitBridgeToInCombatActor(RE::Actor* currentActor, unsigned& scannedCount)
		{
			scannedCount = 0;

			// R457A: R456A looked only at live native truce pools. In Bleedout ->
			// Pleasure handoff those pools can already be cleared while the Papyrus
			// bridge aliases still preserve the original crowd.  Do not re-scan wild
			// hostility actors here.  Use the Bleedout-owned snapshot captured when the
			// original Bleedout crowd was primed / when Pleasure was prepared, then fall
			// back to the old bounded truce pools only if the snapshot is empty.
			std::vector<RE::Actor*> actors;
			const auto snapshotIds = TFD::Bleedout::GetBleedPleasureCrowdSnapshotIDs();
			for (const auto actorID : snapshotIds) {
				AppendUniqueCycleActor(actors, RE::TESForm::LookupByID<RE::Actor>(actorID));
			}

			if (!actors.empty()) {
				scannedCount = static_cast<unsigned>(actors.size());
				for (auto* candidate : actors) {
					if (IsValidPreCombatCycleCandidate(candidate, currentActor)) {
						spdlog::info(
							"[TFD][PleasureRuntime][R457A] bleedout recruit bridge selected next InCombat speaker current={:08X} next={:08X} scanned={} pool=bleedout_snapshot snapshot={}",
							currentActor ? currentActor->GetFormID() : 0u,
							candidate->GetFormID(),
							scannedCount,
							static_cast<unsigned int>(snapshotIds.size()));
						return candidate;
					}

					if (candidate && candidate->GetFormID() != 0) {
						spdlog::info(
							"[TFD][PleasureRuntime][R457A] bleedout recruit bridge snapshot candidate skipped current={:08X} candidate={:08X} snapshot={} reason=invalid_or_recruit_like",
							currentActor ? currentActor->GetFormID() : 0u,
							candidate->GetFormID(),
							static_cast<unsigned int>(snapshotIds.size()));
					}
				}

				spdlog::info(
					"[TFD][PleasureRuntime][R457A] bleedout recruit bridge snapshot exhausted current={:08X} scanned={} snapshot={} reason=no_valid_snapshot_candidate",
					currentActor ? currentActor->GetFormID() : 0u,
					scannedCount,
					static_cast<unsigned int>(snapshotIds.size()));
			}

			auto liveActors = TFD::HostilityController::CollectDialogueTruceActors(currentActor);
			const bool usedDialogueList = !liveActors.empty();
			if (liveActors.empty()) {
				liveActors = TFD::HostilityController::CollectActiveTruceActors(currentActor);
			}

			scannedCount = static_cast<unsigned>(liveActors.size());
			for (auto* candidate : liveActors) {
				if (IsValidPreCombatCycleCandidate(candidate, currentActor)) {
					spdlog::info(
						"[TFD][PleasureRuntime][R456A] bleedout recruit bridge selected next InCombat speaker current={:08X} next={:08X} scanned={} pool={}",
						currentActor ? currentActor->GetFormID() : 0u,
						candidate->GetFormID(),
						scannedCount,
						usedDialogueList ? "dialogue_truce" : "active_truce");
					return candidate;
				}

				if (candidate && candidate->GetFormID() != 0) {
					spdlog::info(
						"[TFD][PleasureRuntime][R456A] bleedout recruit bridge candidate skipped current={:08X} candidate={:08X} pool={} reason=invalid_or_recruit_like",
						currentActor ? currentActor->GetFormID() : 0u,
						candidate->GetFormID(),
						usedDialogueList ? "dialogue_truce" : "active_truce");
				}
			}

			spdlog::info(
				"[TFD][PleasureRuntime][R457A] bleedout recruit bridge no InCombat speaker current={:08X} snapshot={} liveScanned={} livePool={}",
				currentActor ? currentActor->GetFormID() : 0u,
				static_cast<unsigned int>(snapshotIds.size()),
				scannedCount,
				usedDialogueList ? "dialogue_truce" : "active_truce");
			return nullptr;
		}

		RE::Actor* SelectNextPreCombatCycleActor(RE::Actor* currentActor, unsigned& scannedCount)
		{
			return SelectNextCycleActorForSource(currentActor, g_state.source, scannedCount);
		}

		RE::Actor* SelectNextRecruitCycleActor(RE::Actor* currentActor, unsigned& scannedCount)
		{
			if (g_state.source == SourceContext::Bleedout) {
				return SelectNextBleedoutRecruitBridgeToInCombatActor(currentActor, scannedCount);
			}

			return SelectNextPreCombatCycleActor(currentActor, scannedCount);
		}

		void QueuePreCombatCycleLocked(RE::Actor* nextActor, RE::Actor* currentActor, unsigned scannedCount, std::string_view reason, bool bleedoutBridgeToInCombat = false)
		{
			if (!nextActor) {
				spdlog::info(
					"[TFD][PleasureRuntime] after pleasure cycle queue skipped current={:08X} scanned={} source={} reason=no_candidate",
					currentActor ? currentActor->GetFormID() : 0u,
					scannedCount,
					ToString(g_state.source));
				ClearQueuedPreCombatCycleLocked("no_candidate");
				ArmTerminalNeutralFinalizeLocked(currentActor, "after_pleasure_cycle_no_candidate");
				return;
			}

			const double now = NowSec();
			g_state.queuedPreCombatSpeakerFormID = nextActor->GetFormID();
			g_state.queuedPreCombatConsumedFormID = currentActor ? currentActor->GetFormID() : 0;
			g_state.queuedPreCombatSource = g_state.source;
			g_state.queuedPreCombatBleedoutBridgeToInCombat = bleedoutBridgeToInCombat;
			g_state.queuedPreCombatNextTrySec = now + kPreCombatCycleInitialDelaySec;
			g_state.queuedPreCombatExpireSec = now + kPreCombatCycleExpireSec;
			g_state.queuedPreCombatAttempts = 0;

			spdlog::info(
				"[TFD][PleasureRuntime] after pleasure cycle queued current={:08X} next={:08X} scanned={} source={} bridgeBleedoutToInCombat={} delay={:.2f}s expire={:.2f}s reason={}",
				currentActor ? currentActor->GetFormID() : 0u,
				nextActor->GetFormID(),
				scannedCount,
				ToString(g_state.source),
				bleedoutBridgeToInCombat ? 1 : 0,
				kPreCombatCycleInitialDelaySec,
				kPreCombatCycleExpireSec,
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}

		bool QueueNextPreCombatCycleIfNeededLocked(RE::Actor* recruitedActor, bool commitClean)
		{
			if (!commitClean || !recruitedActor ||
				(g_state.source != SourceContext::PreCombat && g_state.source != SourceContext::InCombat && g_state.source != SourceContext::Bleedout)) {
				return false;
			}

			unsigned scannedCount = 0;
			auto* nextActor = SelectNextRecruitCycleActor(recruitedActor, scannedCount);

			QueuePreCombatCycleLocked(
				nextActor,
				recruitedActor,
				scannedCount,
				g_state.source == SourceContext::Bleedout ?
					"after_pleasure_recruit_bleedout_bridge_to_incombat_cycle" :
					"after_pleasure_recruit_cycle",
				g_state.source == SourceContext::Bleedout && nextActor != nullptr);
			return nextActor != nullptr;
		}

		QueuedPreCombatAttempt TakeDueQueuedPreCombatAttemptLocked(double nowSec)
		{
			QueuedPreCombatAttempt attempt{};
			if (g_state.queuedPreCombatSpeakerFormID == 0) {
				return attempt;
			}

			if (nowSec < g_state.queuedPreCombatNextTrySec) {
				return attempt;
			}

			if (nowSec >= g_state.queuedPreCombatExpireSec || g_state.queuedPreCombatAttempts >= kPreCombatCycleMaxAttempts) {
				const auto expiredSource = g_state.queuedPreCombatSource;
				const auto consumedFormID = g_state.queuedPreCombatConsumedFormID;
				spdlog::warn(
					"[TFD][PleasureRuntime] after pleasure cycle queue expired actor={:08X} consumed={:08X} source={} attempts={} now={:.2f} expire={:.2f}",
					g_state.queuedPreCombatSpeakerFormID,
					consumedFormID,
					ToString(expiredSource),
					g_state.queuedPreCombatAttempts,
					nowSec,
					g_state.queuedPreCombatExpireSec);

				if (expiredSource == SourceContext::Bleedout) {
					ArmTerminalNeutralFinalizeLocked(
						LookupActor(consumedFormID),
						"after_pleasure_cycle_queue_expired_bleedout",
						expiredSource);
				}

				ClearQueuedPreCombatCycleLocked("expired");
				return attempt;
			}

			++g_state.queuedPreCombatAttempts;
			g_state.queuedPreCombatNextTrySec = nowSec + kPreCombatCycleRetryDelaySec;

			attempt.actorFormID = g_state.queuedPreCombatSpeakerFormID;
			attempt.consumedActorFormID = g_state.queuedPreCombatConsumedFormID;
			attempt.source = g_state.queuedPreCombatSource;
			attempt.bleedoutBridgeToInCombat = g_state.queuedPreCombatBleedoutBridgeToInCombat;
			attempt.attemptIndex = g_state.queuedPreCombatAttempts;
			attempt.valid = true;
			return attempt;
		}

		void FinishQueuedPreCombatAttemptLocked(const QueuedPreCombatAttempt& attempt, bool success, bool terminalFailure, std::string_view reason)
		{
			if (!attempt.valid || g_state.queuedPreCombatSpeakerFormID != attempt.actorFormID) {
				return;
			}

			const double now = NowSec();
			if (success) {
				if (attempt.source == SourceContext::Bleedout && attempt.bleedoutBridgeToInCombat) {
					g_state.inCombatContinuationFromBleedoutSnapshot = true;
					spdlog::info(
						"[TFD][PleasureRuntime][R505A] InCombat continuation adopted Bleedout snapshot actor={:08X} consumed={:08X} attempt={}",
						attempt.actorFormID,
						attempt.consumedActorFormID,
						attempt.attemptIndex);
				}
				ClearQueuedPreCombatCycleLocked("begin_ok");
				return;
			}

			if (terminalFailure || now >= g_state.queuedPreCombatExpireSec || g_state.queuedPreCombatAttempts >= kPreCombatCycleMaxAttempts) {
				spdlog::warn(
					"[TFD][PleasureRuntime] after pleasure cycle queue terminal actor={:08X} consumed={:08X} source={} attempts={} terminal={} reason={}",
					attempt.actorFormID,
					attempt.consumedActorFormID,
					ToString(attempt.source),
					g_state.queuedPreCombatAttempts,
					terminalFailure ? 1 : 0,
					reason.empty() ? std::string{ "-" } : std::string{ reason });

				if (attempt.source == SourceContext::Bleedout) {
					ArmTerminalNeutralFinalizeLocked(
						LookupActor(attempt.consumedActorFormID),
						reason.empty() ? "after_pleasure_cycle_terminal_bleedout" : reason,
						attempt.source);
				}

				ClearQueuedPreCombatCycleLocked(reason.empty() ? "terminal" : reason);
				return;
			}

			spdlog::info(
				"[TFD][PleasureRuntime] after pleasure cycle retry actor={:08X} attempts={} nextDelay={:.2f}s reason={}",
				attempt.actorFormID,
				g_state.queuedPreCombatAttempts,
				kPreCombatCycleRetryDelaySec,
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}

		QueuedTerminalNeutralFinalize TakeDueTerminalNeutralFinalizeLocked(double nowSec)
		{
			QueuedTerminalNeutralFinalize finalize{};
			if (!g_state.queuedTerminalNeutralFinalize) {
				return finalize;
			}

			if (nowSec < g_state.queuedTerminalNeutralDueSec) {
				return finalize;
			}

			finalize.actorFormID = g_state.queuedTerminalNeutralActorFormID;
			finalize.cycleId = g_state.queuedTerminalNeutralCycleId;
			finalize.source = g_state.queuedTerminalNeutralSource;
			finalize.valid = true;

			g_state.queuedTerminalNeutralFinalize = false;
			g_state.queuedTerminalNeutralActorFormID = 0;
			g_state.queuedTerminalNeutralCycleId = 0;
			g_state.queuedTerminalNeutralDueSec = 0.0;

			return finalize;
		}

		QueuedAfterPleasureDialogueTimeout TakeDueAfterPleasureDialogueTimeoutLocked(double nowSec)
		{
			QueuedAfterPleasureDialogueTimeout timeout{};
			if (g_state.phase != Phase::AfterPleasureDialogue || g_state.afterPleasureDialogueExpireSec <= 0.0) {
				return timeout;
			}

			if (nowSec < g_state.afterPleasureDialogueExpireSec) {
				return timeout;
			}

			timeout.actorFormID = g_state.afterPleasureSpeakerFormID ? g_state.afterPleasureSpeakerFormID : g_state.pleasureSpeakerFormID;
			timeout.cycleId = g_state.sessionCycleId;
			timeout.source = g_state.source;
			timeout.valid = true;

			const auto oldPhase = g_state.phase;
			g_state.afterPleasureDialogueExpireSec = 0.0;
			g_state.redoPending = false;
			g_state.pendingChoice = AfterChoice::None;
			AdvancePhaseLocked(Phase::Finalizing, "after_pleasure_dialogue_timeout");
			AdvancePhaseLocked(Phase::Closed, "after_pleasure_dialogue_timeout");
			ClearBridgeStateLocked();
			ClearHoldStateLocked();

			spdlog::warn(
				"[TFD][PleasureRuntime] after pleasure dialogue timeout actor={:08X} cycle={} source={} oldPhase={} timeout={:.2f}s reason=no_terminal_choice",
				timeout.actorFormID,
				timeout.cycleId,
				ToString(timeout.source),
				ToString(oldPhase),
				kAfterPleasureDialogueHardTimeoutSec);

			return timeout;
		}

		void MarkTerminalNeutralFinalizedLocked(const QueuedTerminalNeutralFinalize& finalize, bool completeFlow, std::string_view reason)
		{
			if (!finalize.valid) {
				return;
			}

			if (finalize.cycleId != 0 && finalize.cycleId != g_state.sessionCycleId) {
				spdlog::info(
					"[TFD][PleasureRuntime] terminal neutral finalize ignored actor={:08X} queuedCycle={} currentCycle={} reason=cycle_mismatch",
					finalize.actorFormID,
					finalize.cycleId,
					g_state.sessionCycleId);
				return;
			}

			const auto oldPhase = g_state.phase;
			const auto oldSource = g_state.source;

			g_state.phase = Phase::Idle;
			g_state.source = SourceContext::None;
			g_state.active = false;
			g_state.blocking = false;
			g_state.passiveLockActive = false;
			g_state.holdActive = false;
			g_state.redoPending = false;
			g_state.flowOwnerToken = 0;
			g_state.pendingChoice = AfterChoice::None;
			g_state.inCombatContinuationFromBleedoutSnapshot = false;
			ClearSpeakerStateLocked();
			ClearBridgeStateLocked();

			spdlog::info(
				"[TFD][PleasureRuntime] terminal neutral finalized actor={:08X} cycle={} completeFlow={} oldPhase={} oldSource={} reason={}",
				finalize.actorFormID,
				finalize.cycleId,
				completeFlow ? 1 : 0,
				ToString(oldPhase),
				ToString(oldSource),
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}

		RE::Actor* ResolveEventOrTrackedActorLocked(const EventInfo& info)
		{
			if (info.actor && !info.actor->IsDisabled() && !info.actor->IsDead()) {
				return info.actor;
			}
			if (auto* actor = LookupActor(info.actorFormID)) {
				if (!actor->IsDisabled() && !actor->IsDead()) {
					return actor;
				}
			}
			if (auto* actor = LookupActor(g_state.pleasureSpeakerFormID)) {
				if (!actor->IsDisabled() && !actor->IsDead()) {
					return actor;
				}
			}
			return nullptr;
		}

		bool IsSceneCombatUnsafe(RE::Actor* actor)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (player && player->IsInCombat()) {
				return true;
			}
			if (actor && actor->IsInCombat()) {
				return true;
			}
			return false;
		}

		bool ShouldTolerateCombatFlagAtSceneEndLocked(double activeDuration)
		{
			// R468A: In combat-owned and bleedout-owned pleasure scenes often still
			// report vanilla combat flags at OStim end because nearby enemies are
			// only quarantined, not globally neutralized.  Once OStim has been
			// established for the minimum active duration, that flag is not an
			// abort signal.  The next owner must be AfterPleasure, not generic
			// PleasureFailed/Recovery cleanup.
			if (activeDuration < kMinimumSceneActiveSec) {
				return false;
			}

			return g_state.source == SourceContext::Bleedout ||
				g_state.source == SourceContext::InCombat;
		}

		bool IsBleedoutRecruitSettleReason(std::string_view reason)
		{
			return reason.find("bleedout") != std::string_view::npos;
		}

		void StabilizeBleedoutRecruitSettle(RE::Actor* actor, std::string_view reason)
		{
			if (!actor || actor->IsDisabled() || actor->IsDead()) {
				return;
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* process = RE::ProcessLists::GetSingleton();
			if (process) {
				process->StopCombatAndAlarmOnActor(actor, false);
			}
			actor->GetActorRuntimeData().currentCombatTarget = RE::ActorHandle{};
			actor->StopCombat();
			actor->StopAlarmOnActor();
			actor->SetBeenAttacked(false);

			if (player) {
				auto* target = player->GetActorRuntimeData().currentCombatTarget.get().get();
				if (target && target->GetFormID() == actor->GetFormID()) {
					player->GetActorRuntimeData().currentCombatTarget = RE::ActorHandle{};
				}
			}

			if (auto* owner = actor->AsActorValueOwner()) {
				owner->SetActorValue(RE::ActorValue::kAggression, 0.0f);
				owner->SetActorValue(RE::ActorValue::kConfidence, 1.0f);
				owner->SetActorValue(RE::ActorValue::kAssistance, 0.0f);
			}

			spdlog::info(
				"[TFD][PleasureRuntime][R453A] bleedout recruit settle guard actor={:08X} reason={} policy=no_runtime_profile_no_package_eval_stop_combat",
				actor->GetFormID(),
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}

		double GetActiveSceneDurationLocked()
		{
			if (g_state.pleasureActiveStartedAtSec <= 0.0) {
				return 0.0;
			}
			return NowSec() - g_state.pleasureActiveStartedAtSec;
		}

		void QueueAbortedFlowCompleteLocked(RE::Actor* actor, std::string_view reason)
		{
			g_state.abortedFlowCompleteQueued = true;
			g_state.abortedFlowActorFormID = actor ? actor->GetFormID() : g_state.pleasureSpeakerFormID;
			g_state.abortedFlowCompleteDueSec = NowSec() + kAbortedFlowCompleteDelaySec;

			spdlog::warn(
				"[TFD][PleasureRuntime] aborted flow complete queued actor={:08X} cycle={} source={} delay={:.2f}s reason={}",
				g_state.abortedFlowActorFormID,
				g_state.sessionCycleId,
				ToString(g_state.source),
				kAbortedFlowCompleteDelaySec,
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}

		bool RouteBleedoutAbortToPleasureFailedLocked(const EventInfo& info, std::string_view eventName)
		{
			if (g_state.source != SourceContext::Bleedout) {
				return false;
			}

			if (eventName != kPleasureAbortedEvent && eventName != kPleasureFailedEvent) {
				return false;
			}

			switch (g_state.phase) {
			case Phase::PleasureStartPending:
			case Phase::PleasureActive:
			case Phase::PleasureEnding:
			case Phase::RedoPending:
			case Phase::AfterPleasureAwaitQuest:
			case Phase::AfterPleasureDialogue:
				break;
			case Phase::PleasureFailedDialogue:
				return true;
			default:
				return false;
			}

			auto* failedActor = ResolveEventOrTrackedActorLocked(info);
			if (!failedActor || failedActor->IsDead() || failedActor->IsDisabled()) {
				failedActor = LookupActor(g_state.pleasureSpeakerFormID);
			}

			if (!failedActor || failedActor->IsDead() || failedActor->IsDisabled()) {
				spdlog::warn(
					"[TFD][PleasureRuntime][R461A] bleedout abort failed dialogue rejected invalid actor event={} eventActor={:08X} tracked={:08X} phase={} source={}",
					std::string(eventName),
					info.actorFormID,
					g_state.pleasureSpeakerFormID,
					ToString(g_state.phase),
					ToString(g_state.source));
				return false;
			}

			const auto failedActorFormID = failedActor->GetFormID();
			g_state.afterPleasureSpeakerFormID = failedActorFormID;
			g_state.afterPleasureCommitted = true;
			g_state.pleasureFailedDialogueActive = true;
			g_state.pendingChoice = AfterChoice::None;
			g_state.redoPending = false;
			g_state.blocking = true;
			g_state.holdActive = true;
			g_state.passiveLockActive = false;
			g_state.afterPleasureDialogueExpireSec = 0.0;
			PreparePleasureFailedPackageActor(failedActor, "r461_bleedout_abort_to_pleasure_failed");
			AdvancePhaseLocked(Phase::PleasureFailedDialogue, "r461_bleedout_abort_to_pleasure_failed");

			const std::string failedArg =
				std::to_string(failedActorFormID) + "|" +
				std::to_string(static_cast<int>(SourceContext::Bleedout));
			const bool queued = TFD::FlowController::QueueBridgeModEvent(
				kPleasureFailedEnterEvent,
				failedActor,
				failedArg.c_str(),
				0.0f);

			spdlog::warn(
				"[TFD][PleasureRuntime][R461A] bleedout abort routed to PleasureFailed actor={:08X} cycle={} event={} queued={} arg={} policy=no_neutral_abort",
				failedActorFormID,
				g_state.sessionCycleId,
				std::string(eventName),
				queued ? 1 : 0,
				failedArg);
			return true;
		}

		QueuedAbortedFlowComplete TakeDueAbortedFlowCompleteLocked(double nowSec)
		{
			QueuedAbortedFlowComplete complete{};
			if (!g_state.abortedFlowCompleteQueued || nowSec < g_state.abortedFlowCompleteDueSec) {
				return complete;
			}

			complete.actorFormID = g_state.abortedFlowActorFormID;
			complete.cycleId = g_state.sessionCycleId;
			complete.source = g_state.source;
			complete.valid = true;

			g_state.abortedFlowCompleteQueued = false;
			g_state.abortedFlowActorFormID = 0;
			g_state.abortedFlowCompleteDueSec = 0.0;
			return complete;
		}

		RE::Actor* ResolveAfterPleasureRecruitActorLocked(const EventInfo& info)
		{
			if (info.actor && !info.actor->IsDisabled() && !info.actor->IsDead()) {
				return info.actor;
			}

			if (auto* actor = LookupActor(info.actorFormID)) {
				if (!actor->IsDisabled() && !actor->IsDead()) {
					return actor;
				}
			}

			if (auto* actor = LookupActor(g_state.afterPleasureSpeakerFormID)) {
				if (!actor->IsDisabled() && !actor->IsDead()) {
					return actor;
				}
			}

			if (auto* actor = LookupActor(g_state.pleasureSpeakerFormID)) {
				if (!actor->IsDisabled() && !actor->IsDead()) {
					return actor;
				}
			}

			return nullptr;
		}

		bool CommitAfterPleasureRecruitLocked(const EventInfo& info)
		{
			auto* actor = ResolveAfterPleasureRecruitActorLocked(info);
			if (!actor) {
				spdlog::warn(
					"[TFD][PleasureRuntime] after pleasure recruit rejected actor=00000000 cycle={} source={} reason=no_valid_actor",
					g_state.sessionCycleId,
					ToString(g_state.source));
				return false;
			}

			const auto actorFormID = actor->GetFormID();
			if (!IsTrackedActorFormIDLocked(actorFormID)) {
				spdlog::warn(
					"[TFD][PleasureRuntime] after pleasure recruit rejected actor={:08X} cycle={} source={} reason=actor_not_tracked pleasureSpeaker={:08X} afterSpeaker={:08X}",
					actorFormID,
					g_state.sessionCycleId,
					ToString(g_state.source),
					g_state.pleasureSpeakerFormID,
					g_state.afterPleasureSpeakerFormID);
				return false;
			}

			const auto slotsFree = TFD::TeammateManager::GetRecruitSlotsFree();
			if (slotsFree == 0) {
				spdlog::warn(
					"[TFD][PleasureRuntime] after pleasure recruit rejected actor={:08X} cycle={} source={} reason=no_recruit_slots",
					actorFormID,
					g_state.sessionCycleId,
					ToString(g_state.source));
				QueueNextPreCombatCycleIfNeededLocked(actor, false);
				TFD::TeammateManager::RefreshRecruitCapacityGlobals("after_pleasure_recruit_no_slots");
				return false;
			}

			const bool inCombatSource = g_state.source == SourceContext::InCombat;
			const bool bleedoutSource = g_state.source == SourceContext::Bleedout;
			const bool deferredChainSource = inCombatSource || bleedoutSource;
			unsigned preCommitScannedCount = 0;
			RE::Actor* predictedNextActor = deferredChainSource ? SelectNextRecruitCycleActor(actor, preCommitScannedCount) : nullptr;
			const bool deferredChainWillContinue = deferredChainSource && predictedNextActor != nullptr;
			const bool bleedoutRecruitSettleGuard = bleedoutSource;

			TFD::Recruit::MarkRecruitCommitPending(
				actor,
				6.0,
				TFD::Recruit::SourceFlow::Pleasure,
				"after_pleasure_recruit_pending");

			TFD::Recruit::CommitOptions options{};
			options.sourceFlow = TFD::Recruit::SourceFlow::Pleasure;
			options.reason = deferredChainWillContinue ?
				(bleedoutSource ? "after_pleasure_recruit_commit_deferred_bleedout_chain" : "after_pleasure_recruit_commit_deferred_incombat_chain") :
				"after_pleasure_recruit_commit";
			options.quarantineHostileFactions = true;
			options.clearCombat = true;
			options.evaluatePackage = !(deferredChainWillContinue || bleedoutRecruitSettleGuard);
			options.detailedLog = true;
			options.throttleObserve = false;
			options.ensurePacifyAlliance = true;
			options.applyRuntimeProfile = !(deferredChainWillContinue || bleedoutRecruitSettleGuard);

			if (deferredChainWillContinue) {
				spdlog::info(
					"[TFD][PleasureRuntime][R130] {} recruit chain precommit actor={:08X} predictedNext={:08X} scanned={} policy=no_package_eval_no_runtime_profile",
					ToString(g_state.source),
					actorFormID,
					predictedNextActor ? predictedNextActor->GetFormID() : 0u,
					preCommitScannedCount);
			}

			const auto result = TFD::Recruit::CommitRecruit(actor, options);
			const bool clean = result.attempted && !result.skipped && !result.rawHostileAfter && result.hostileFactionMatchesAfter == 0;
			const bool bleedoutBridgeClean = bleedoutSource &&
				result.attempted &&
				!result.skipped &&
				result.hostileFactionMatchesAfter == 0 &&
				TFD::Recruit::IsRecruitLike(actor);
			const bool chainClean = clean || bleedoutBridgeClean;

			if (bleedoutBridgeClean && !clean) {
				spdlog::info(
					"[TFD][PleasureRuntime][R485A] bleedout recruit bridge accepted stale raw hostility actor={:08X} rawAfter={} hostileAfter={} recruitLike=1 predictedNext={:08X} scanned={} policy=queue_handoff_not_neutralize",
					actorFormID,
					result.rawHostileAfter ? 1 : 0,
					result.hostileFactionMatchesAfter,
					predictedNextActor ? predictedNextActor->GetFormID() : 0u,
					preCommitScannedCount);
			}

			if (chainClean && bleedoutRecruitSettleGuard) {
				StabilizeBleedoutRecruitSettle(actor, "after_pleasure_recruit_bleedout_settle_guard");
			}
			bool aliasRegistered = false;

			bool queuedNextCycle = false;
			if (chainClean) {
				queuedNextCycle = QueueNextPreCombatCycleIfNeededLocked(actor, true);
				if ((g_state.source == SourceContext::InCombat || g_state.source == SourceContext::Bleedout) && (queuedNextCycle || bleedoutRecruitSettleGuard)) {
					const char* deferReason = g_state.source == SourceContext::Bleedout ?
						(queuedNextCycle ? "after_pleasure_recruit_bleedout_chain_active" : "after_pleasure_recruit_bleedout_settle_no_candidate") :
						"after_pleasure_recruit_chain_active";
					DeferInCombatRecruitAliasLocked(actor, deferReason);
					(void)TFD::HostilityController::DemoteTruceActorForCycleHold(actor, g_state.source == SourceContext::Bleedout ? "after_pleasure_recruit_bleedout_settle_defer" : "after_pleasure_recruit_chain_defer");
					// R453A: Bleedout recruits must not receive teammate package/runtime aggression
					// before the next speaker/fallback is stable. Keep them in passive settle hold.
				}
				else {
					aliasRegistered = TFD::TeammateManager::RegisterOrRefreshTeammateNow(actor, "after_pleasure_recruit_commit");
					TFD::TeammateManager::RefreshRecruitCapacityGlobals("after_pleasure_recruit_commit");
				}
			}

			if (chainClean) {
				spdlog::info(
					"[TFD][PleasureRuntime] after pleasure recruit commit actor={:08X} cycle={} source={} eventFlow={} attempted={} skipped={} clean={} bridgeClean={} rawAfter={} hostileAfter={} aliasRegistered={} queuedNext={} reason={}",
					actorFormID,
					g_state.sessionCycleId,
					ToString(g_state.source),
					info.sourceFlow,
					result.attempted ? 1 : 0,
					result.skipped ? 1 : 0,
					clean ? 1 : 0,
					bleedoutBridgeClean ? 1 : 0,
					result.rawHostileAfter ? 1 : 0,
					result.hostileFactionMatchesAfter,
					aliasRegistered ? 1 : 0,
					queuedNextCycle ? 1 : 0,
					"after_pleasure_recruit_commit");
			}
			else {
				spdlog::warn(
					"[TFD][PleasureRuntime] after pleasure recruit commit actor={:08X} cycle={} source={} eventFlow={} attempted={} skipped={} clean={} bridgeClean={} rawAfter={} hostileAfter={} aliasRegistered={} queuedNext={} reason={}",
					actorFormID,
					g_state.sessionCycleId,
					ToString(g_state.source),
					info.sourceFlow,
					result.attempted ? 1 : 0,
					result.skipped ? 1 : 0,
					0,
					0,
					result.rawHostileAfter ? 1 : 0,
					result.hostileFactionMatchesAfter,
					aliasRegistered ? 1 : 0,
					queuedNextCycle ? 1 : 0,
					"commit_not_clean_no_alias");
			}

			if (!chainClean) {
				(void)QueueNextPreCombatCycleIfNeededLocked(actor, false);
			}

			return chainClean;
		}

		bool FinalizeConsumedInCombatRecruitForCycle(RE::Actor* consumedActor, RE::Actor* nextActor, std::string_view reason)
		{
			if (!consumedActor || consumedActor->IsDead() || consumedActor->IsDisabled()) {
				return false;
			}

			const auto consumedId = consumedActor->GetFormID();
			if (consumedId == 0) {
				return false;
			}

			if (!TFD::Recruit::IsRecruitLike(consumedActor)) {
				spdlog::info(
					"[TFD][PleasureRuntime][R96D] consumed recruit finalize skipped actor={:08X} next={:08X} reason={} detail=not_recruit_like",
					consumedId,
					nextActor ? nextActor->GetFormID() : 0u,
					reason.empty() ? std::string{ "-" } : std::string{ reason });
				return false;
			}

			{
				std::scoped_lock lk(g_lock);
				RemoveDeferredInCombatRecruitIdLocked(consumedId, reason.empty() ? "consumed_recruit_cycle_finalize" : reason);
			}

			TFD::Recruit::MarkRecruitCommitPending(
				consumedActor,
				2.0,
				TFD::Recruit::SourceFlow::Pleasure,
				"incombat_consumed_recruit_cycle_finalize_pending");

			const bool bleedoutSettle = IsBleedoutRecruitSettleReason(reason);

			TFD::Recruit::CommitOptions options{};
			options.sourceFlow = TFD::Recruit::SourceFlow::Pleasure;
			options.reason = bleedoutSettle ? "bleedout_consumed_recruit_cycle_finalize" : "incombat_consumed_recruit_cycle_finalize";
			options.quarantineHostileFactions = true;
			options.clearCombat = true;
			options.evaluatePackage = !bleedoutSettle;
			options.detailedLog = true;
			options.throttleObserve = false;
			options.ensurePacifyAlliance = true;
			options.applyRuntimeProfile = !bleedoutSettle;

			const auto result = TFD::Recruit::CommitRecruit(consumedActor, options);
			const bool recruitLikeClean =
				TFD::Recruit::IsRecruitLike(consumedActor) &&
				!TFD::Recruit::IsRawHostileToPlayer(consumedActor) &&
				!TFD::Recruit::HasKnownHostileSourceFaction(consumedActor);
			const bool settledClean = result.attempted && !result.rawHostileAfter && result.hostileFactionMatchesAfter == 0;
			const bool finalizeOk = settledClean || recruitLikeClean;

			if (finalizeOk && bleedoutSettle) {
				StabilizeBleedoutRecruitSettle(consumedActor, reason.empty() ? "bleedout_consumed_recruit_cycle_finalize" : reason);
			}

			bool truceReleased = false;
			bool aliasRegistered = false;
			if (finalizeOk) {
				truceReleased = TFD::HostilityController::ReleaseSingleTruceActorForCycle(
					consumedActor,
					TFD::HostilityController::ReleaseReason::FlowHandoff,
					"incombat_consumed_recruit_cycle_finalize");
				aliasRegistered = bleedoutSettle ?
					TFD::TeammateManager::RegisterOrRefreshTeammateNowDeferredPackage(
						consumedActor,
						"bleedout_consumed_recruit_cycle_finalize") :
					TFD::TeammateManager::RegisterOrRefreshTeammateNowImmediatePackage(
						consumedActor,
						"incombat_consumed_recruit_cycle_finalize");
				TFD::TeammateManager::RefreshRecruitCapacityGlobals("incombat_consumed_recruit_cycle_finalize");
			}

			spdlog::info(
				"[TFD][PleasureRuntime][R96D] consumed recruit finalized actor={:08X} next={:08X} attempted={} skipped={} settledClean={} recruitLikeClean={} rawAfter={} hostileAfter={} truceReleased={} aliasRegistered={} reason={}",
				consumedId,
				nextActor ? nextActor->GetFormID() : 0u,
				result.attempted ? 1 : 0,
				result.skipped ? 1 : 0,
				settledClean ? 1 : 0,
				recruitLikeClean ? 1 : 0,
				result.rawHostileAfter ? 1 : 0,
				result.hostileFactionMatchesAfter,
				truceReleased ? 1 : 0,
				aliasRegistered ? 1 : 0,
				reason.empty() ? std::string{ "-" } : std::string{ reason });

			return aliasRegistered;
		}

		void HandleRecognizedEventLocked(std::string_view eventName, const EventInfo& info)
		{
			if (eventName == kPleasureClearEvent) {
				const auto oldPhase = g_state.phase;
				const auto oldSource = g_state.source;
				const auto oldCycle = g_state.sessionCycleId;
				const auto eventSource = SourceFromFlowValue(info.sourceFlow, oldSource);
				const bool hadBleedoutPleasureOwner =
					eventSource == SourceContext::Bleedout &&
					IsBleedoutSourceClearPhase(oldPhase) &&
					(g_state.active || g_state.blocking || g_state.holdActive || g_state.passiveLockActive);
				auto* clearActor = ResolveEventOrTrackedActorLocked(info);

				ClearQueuedPreCombatCycleLocked(eventName);
				ClearQueuedTerminalNeutralFinalizeLocked(eventName);
				g_state.redoPending = false;
				g_state.pendingChoice = AfterChoice::None;
				g_state.abortedFlowCompleteQueued = false;
				g_state.abortedFlowActorFormID = 0;
				g_state.abortedFlowCompleteDueSec = 0.0;
				g_state.afterPleasureCommitted = false;
				g_state.pleasureFailedDialogueActive = false;
				g_state.active = false;
				g_state.blocking = false;
				g_state.phase = Phase::Closed;
				g_state.source = SourceContext::None;
				g_state.flowOwnerToken = 0;
				ClearBridgeStateLocked();
				ClearHoldStateLocked();
				ClearSpeakerStateLocked();

				bool bleedoutNeutralized = false;
				if (hadBleedoutPleasureOwner) {
					TFD::ForceGreetState::ResetBleedout();
					TFD::ForceGreetState::ResetPleasureFailed();
					TFD::ForceGreetState::ResetAfterPleasure();
					bleedoutNeutralized = TFD::Bleedout::CompletePleasureCycleChainNeutral("r512a_bleedout_source_pleasure_clear");
				}

				spdlog::info(
					"[TFD][PleasureRuntime][W32] clear event accepted event={} actor={:08X} oldPhase={} oldSource={} eventSource={} oldCycle={} bleedoutNeutralized={} reason=papyrus_or_system_clear",
					std::string{ eventName },
					clearActor ? clearActor->GetFormID() : info.actorFormID,
					ToString(oldPhase),
					ToString(oldSource),
					ToString(eventSource),
					oldCycle,
					bleedoutNeutralized ? 1 : 0);
				return;
			}

			if (eventName == kPleasureStartPendingEvent || eventName == kOStimSceneStartPendingEvent) {
				if (g_state.phase == Phase::Idle || g_state.phase == Phase::Closed) {
					const auto eventSource = SourceFromFlowValue(info.sourceFlow, SourceContext::None);
					auto* eventActor = info.actor ? info.actor : LookupActor(info.actorFormID);

					if (!IsValidSceneStartSource(eventSource)) {
						LogEventIgnoredLocked(eventName, "orphan_start_pending_source_none", info);
						return;
					}

					if (!eventActor || eventActor->IsDisabled() || eventActor->IsDead()) {
						LogEventIgnoredLocked(eventName, "orphan_start_pending_no_actor", info);
						return;
					}


					BeginNewCycleLocked(eventActor, eventSource, eventName);
					if (eventName == kOStimSceneStartPendingEvent && info.threadID >= 0) {
						g_state.ostimThreadId = static_cast<std::uint32_t>(info.threadID);
					}
					AdvancePhaseLocked(Phase::PleasureStartPending, eventName);
					g_state.blocking = true;
					g_state.holdActive = true;
					if (IsScenePassiveHoldSource(eventSource)) {
						g_state.passiveLockActive = true;
						PrepareActorForScenePassiveLocked(eventActor, "scene_start_pending_passive_lock");
						SuppressActorDialogueForSceneLocked(eventActor, "scene_start_pending_suppress_dialogue");
					}
					else {
						g_state.passiveLockActive = false;
						g_state.scenePassiveHoldNextPulseSec = 0.0;
						g_state.scenePassiveHoldPulseCount = 0;
						if (eventSource == SourceContext::Bleedout) {
							SuppressActorDialogueForSceneLocked(eventActor, "r249a_bleedout_scene_start_dialogue_cooldown_only");
						}
						spdlog::info(
							"[TFD][PleasureRuntime][R249A] safe scene start pending actor={:08X} source={} cycle={} reason=no_hard_passive_hold",
							eventActor->GetFormID(),
							ToString(eventSource),
							g_state.sessionCycleId);
					}
					LogEventAcceptedLocked(eventName, info, "new_cycle");
					return;
				}
				if (g_state.phase == Phase::PleasureStartPending) {
					if (eventName == kOStimSceneStartPendingEvent && info.threadID >= 0) {
						g_state.ostimThreadId = static_cast<std::uint32_t>(info.threadID);
						g_state.scenePassiveHoldNextPulseSec = 0.0;
						LogEventAcceptedLocked(eventName, info, "r498a_quickstart_thread_armed_no_package_eval");
						return;
					}
					LogEventIgnoredLocked(eventName, "already_pending", info);
					return;
				}
				LogEventIgnoredLocked(eventName, "wrong_phase", info);
				return;
			}

			if (eventName == kPleasureStartedEvent || eventName == kOStimSceneStartedEvent) {
				if (g_state.phase == Phase::PleasureStartPending) {
					if (info.threadID >= 0) {
						g_state.ostimThreadId = static_cast<std::uint32_t>(info.threadID);
					}
					AdvancePhaseLocked(Phase::PleasureActive, eventName);
					g_state.holdActive = true;
					if (IsScenePassiveHoldSource(g_state.source)) {
						g_state.passiveLockActive = true;
						PrepareActorForScenePassiveLocked(
							info.actor ? info.actor : LookupActor(info.actorFormID),
							"r498a_scene_started_passive_lock_no_package_eval",
							false);
						SuppressActorDialogueForSceneLocked(
							info.actor ? info.actor : LookupActor(info.actorFormID),
							"scene_started_suppress_dialogue");
					}
					else {
						g_state.passiveLockActive = false;
						g_state.scenePassiveHoldNextPulseSec = 0.0;
						g_state.scenePassiveHoldPulseCount = 0;
						if (g_state.source == SourceContext::Bleedout) {
							SuppressActorDialogueForSceneLocked(info.actor ? info.actor : LookupActor(info.actorFormID), "r249a_bleedout_scene_started_dialogue_cooldown_only");
						}
						spdlog::info(
							"[TFD][PleasureRuntime][R249A] safe scene active source={} cycle={} reason=no_hard_passive_hold",
							ToString(g_state.source),
							g_state.sessionCycleId);
					}
					LogEventAcceptedLocked(eventName, info, "scene_active");
					return;
				}
				LogEventIgnoredLocked(eventName, "wrong_phase", info);
				return;
			}

			if (eventName == kOStimSceneEndedEvent) {
				// R498A: Generic scene-ended remains a result-neutral compatibility
				// notification for existing Papyrus listeners. It must never decide
				// AfterPleasure versus PleasureFailed.
				spdlog::info(
					"[TFD][PleasureRuntime][R498A] terminal notification accepted actor={:08X} cycle={} source={} phase={} event={}",
					info.actorFormID,
					g_state.sessionCycleId,
					ToString(g_state.source),
					ToString(g_state.phase),
					std::string{ eventName });
				return;
			}

			if (eventName == kPleasureEndedEvent ||
				eventName == kOStimSceneSucceededEvent) {
				if (g_state.phase == Phase::PleasureActive) {
					auto* eventActor = ResolveEventOrTrackedActorLocked(info);
					const double activeDuration = GetActiveSceneDurationLocked();
					const bool shortScene = activeDuration < kMinimumSceneActiveSec;
					const bool combatUnsafe = IsSceneCombatUnsafe(eventActor);
					const bool combatFlagTolerated = combatUnsafe && ShouldTolerateCombatFlagAtSceneEndLocked(activeDuration);
					const bool authoritativeOStimSuccess = eventName == kOStimSceneSucceededEvent;

					// R498A: TFDOStimSceneSucceeded is emitted only after Papyrus has
					// claimed the scene terminal and confirmed the speaker climax. Native
					// must not reinterpret that result from duration or a late combat flag.
					// Keep the old safety policy for the unused legacy
					// TFDPreCombatPleasureEnded route so compatibility behavior does not
					// silently become more permissive.
					if (!authoritativeOStimSuccess &&
						(shortScene || (combatUnsafe && !combatFlagTolerated))) {
						spdlog::warn(
							"[TFD][PleasureRuntime] legacy scene end rejected actor={:08X} cycle={} source={} duration={:.2f}s min={:.2f}s combatUnsafe={} tolerated={} reason={} no_afterpleasure=1",
							eventActor ? eventActor->GetFormID() : info.actorFormID,
							g_state.sessionCycleId,
							ToString(g_state.source),
							activeDuration,
							kMinimumSceneActiveSec,
							combatUnsafe ? 1 : 0,
							combatFlagTolerated ? 1 : 0,
							std::string{ eventName });

						AdvancePhaseLocked(Phase::Finalizing, "scene_rejected_no_afterpleasure");
						AdvancePhaseLocked(Phase::Closed, "scene_rejected_no_afterpleasure");
						ClearBridgeStateLocked();
						ClearHoldStateLocked();
						g_state.redoPending = false;
						g_state.pendingChoice = AfterChoice::None;
						QueueAbortedFlowCompleteLocked(eventActor, "scene_rejected_no_afterpleasure");
						LogEventAcceptedLocked(eventName, info, "scene_rejected_no_afterpleasure");
						return;
					}

					if (authoritativeOStimSuccess && (shortScene || combatUnsafe)) {
						spdlog::warn(
							"[TFD][PleasureRuntime][R498A] authoritative OStim success accepted actor={:08X} cycle={} source={} duration={:.2f}s min={:.2f}s combatUnsafe={} event={}",
							eventActor ? eventActor->GetFormID() : info.actorFormID,
							g_state.sessionCycleId,
							ToString(g_state.source),
							activeDuration,
							kMinimumSceneActiveSec,
							combatUnsafe ? 1 : 0,
							std::string{ eventName });
					}

					if (authoritativeOStimSuccess && combatUnsafe) {
						// Preserve the encounter while the authoritative AfterPleasure
						// result is handed to the quest/dialogue owner.
						g_state.holdActive = true;
						g_state.passiveLockActive = true;
						g_state.scenePassiveHoldNextPulseSec = 0.0;
						TFD::HostilityController::ScheduleStopCombatWaves(2400.0f, false, 6, 75);
					}
					else if (combatFlagTolerated) {
						// Preserve the baseline tolerance for the legacy result event.
						g_state.holdActive = true;
						g_state.passiveLockActive = true;
						g_state.scenePassiveHoldNextPulseSec = 0.0;
						TFD::HostilityController::ScheduleStopCombatWaves(2400.0f, false, 6, 75);
						spdlog::warn(
							"[TFD][PleasureRuntime][R468A] legacy scene end combat flag tolerated actor={:08X} cycle={} source={} duration={:.2f}s reason={} afterpleasure=1",
							eventActor ? eventActor->GetFormID() : info.actorFormID,
							g_state.sessionCycleId,
							ToString(g_state.source),
							activeDuration,
							std::string{ eventName });
					}

					const auto afterSpeakerFormID = info.actorFormID ? info.actorFormID : g_state.pleasureSpeakerFormID;

					AdvancePhaseLocked(Phase::PleasureEnding, eventName);

					g_state.afterPleasureSpeakerFormID = afterSpeakerFormID;
					g_state.afterPleasureCommitted = false;
					g_state.pleasureFailedDialogueActive = false;
					g_state.pendingChoice = AfterChoice::None;
					g_state.redoPending = false;
					g_state.blocking = true;

					if (auto* afterActor = LookupActor(afterSpeakerFormID)) {
						if (IsScenePassiveHoldSourceLocked()) {
							g_state.holdActive = true;
							g_state.passiveLockActive = true;
							g_state.scenePassiveHoldNextPulseSec = 0.0;
							PrepareActorForScenePassiveLocked(afterActor, "r498a_success_afterpleasure_passive_lock");
							TFD::HostilityController::ScheduleStopCombatWaves(2400.0f, false, 8, 60);
						}
						PrepareAfterPleasurePackageActor(afterActor, "r498a_success_await_after_dialogue");
					}

					AdvancePhaseLocked(Phase::AfterPleasureAwaitQuest, eventName);
					LogEventAcceptedLocked(
						eventName,
						info,
						authoritativeOStimSuccess ? "r498a_success_committed" : "r498a_legacy_success_committed");
					return;
				}
				if (g_state.phase == Phase::AfterPleasureAwaitQuest || g_state.phase == Phase::AfterPleasureDialogue) {
					LogEventIgnoredLocked(eventName, "duplicate_success_result", info);
					return;
				}
				if (g_state.phase == Phase::PleasureFailedDialogue) {
					LogEventIgnoredLocked(eventName, "stale_success_after_failure_committed", info);
					return;
				}
				LogEventIgnoredLocked(eventName, "wrong_phase", info);
				return;
			}

			if (eventName == kPleasureFailedEnterEvent) {
				if (g_state.phase == Phase::AfterPleasureAwaitQuest || g_state.phase == Phase::AfterPleasureDialogue) {
					LogEventIgnoredLocked(eventName, "stale_failure_after_success_committed", info);
					return;
				}
				if (g_state.phase == Phase::PleasureFailedDialogue) {
					LogEventIgnoredLocked(eventName, "duplicate_failure_result", info);
					return;
				}
				if (g_state.phase == Phase::PleasureStartPending ||
					g_state.phase == Phase::PleasureActive ||
					g_state.phase == Phase::PleasureEnding) {
					auto* failedActor = ResolveEventOrTrackedActorLocked(info);
					if (!failedActor || failedActor->IsDead() || failedActor->IsDisabled()) {
						spdlog::warn(
							"[TFD][PleasureRuntime][W51] pleasure failed dialogue ignored invalid actor actor={:08X} tracked={:08X} phase={} source={}",
							info.actorFormID,
							g_state.pleasureSpeakerFormID,
							ToString(g_state.phase),
							ToString(g_state.source));
						AdvancePhaseLocked(Phase::Finalizing, "pleasure_failed_invalid_actor");
						AdvancePhaseLocked(Phase::Closed, "pleasure_failed_invalid_actor");
						ClearBridgeStateLocked();
						ClearHoldStateLocked();
						g_state.redoPending = false;
						g_state.pendingChoice = AfterChoice::None;
						LogEventIgnoredLocked(eventName, "invalid_actor", info);
						return;
					}

					const auto failedActorFormID = failedActor->GetFormID();
					g_state.afterPleasureSpeakerFormID = failedActorFormID;
					g_state.afterPleasureCommitted = true;
					g_state.pleasureFailedDialogueActive = true;
					g_state.pendingChoice = AfterChoice::None;
					g_state.redoPending = false;
					g_state.blocking = true;
					g_state.holdActive = true;
					g_state.passiveLockActive = false;
					g_state.afterPleasureDialogueExpireSec = 0.0;
					PreparePleasureFailedPackageActor(failedActor, "r498a_pleasure_failed_committed");
					AdvancePhaseLocked(Phase::PleasureFailedDialogue, eventName);
					LogEventAcceptedLocked(eventName, info, "r498a_failure_committed");
					return;
				}
				LogEventIgnoredLocked(eventName, "wrong_phase", info);
				return;
			}

			if (eventName == kAfterPleasureEnterEvent) {
				if (g_state.phase == Phase::AfterPleasureAwaitQuest) {
					AdvancePhaseLocked(Phase::AfterPleasureDialogue, eventName);
					g_state.afterPleasureCommitted = true;
					g_state.pleasureFailedDialogueActive = false;
					g_state.pendingChoice = AfterChoice::None;
					g_state.blocking = true;
					g_state.afterPleasureDialogueExpireSec = 0.0;
					LogEventAcceptedLocked(eventName, info, "dialogue_open_no_hard_timeout");
					return;
				}
				LogEventIgnoredLocked(eventName, "wrong_phase", info);
				return;
			}

			if (eventName == kAfterPleasureLoopEnterEvent) {
				HandleAfterPleasureLoopNoiseLocked(info, eventName);
				return;
			}

			if (eventName == kPleasureFailedEvent || eventName == kPleasureAbortedEvent) {
				if (g_state.phase == Phase::AfterPleasureAwaitQuest || g_state.phase == Phase::AfterPleasureDialogue) {
					LogEventIgnoredLocked(eventName, "stale_failure_or_abort_after_success_committed", info);
					return;
				}
				if (RouteBleedoutAbortToPleasureFailedLocked(info, eventName)) {
					LogEventAcceptedLocked(eventName, info, "r461_bleedout_abort_to_pleasure_failed");
					return;
				}

				if (g_state.phase == Phase::PleasureStartPending || g_state.phase == Phase::PleasureActive || g_state.phase == Phase::PleasureEnding || g_state.phase == Phase::RedoPending || g_state.phase == Phase::AfterPleasureDialogue || g_state.phase == Phase::PleasureFailedDialogue) {
					auto* eventActor = ResolveEventOrTrackedActorLocked(info);
					AdvancePhaseLocked(Phase::Finalizing, eventName);
					AdvancePhaseLocked(Phase::Closed, eventName);
					ClearBridgeStateLocked();
					ClearHoldStateLocked();
					g_state.redoPending = false;
					g_state.pendingChoice = AfterChoice::None;
					QueueAbortedFlowCompleteLocked(eventActor, eventName);
					LogEventAcceptedLocked(eventName, info, "failed_or_aborted_close");
					return;
				}
				LogEventIgnoredLocked(eventName, "late_or_wrong_phase", info);
				return;
			}

			if (IsAfterPleasureChoiceEvent(eventName)) {
				if (g_state.phase == Phase::AfterPleasureAwaitQuest) {
					// R109: package-owned Bleedout AfterPleasure can receive a branch
					// choice before native sees a separate enter/open event. Promote the
					// runtime to dialogue here instead of dropping the terminal choice.
					AdvancePhaseLocked(Phase::AfterPleasureDialogue, "after_pleasure_choice_promote_from_await");
					g_state.afterPleasureCommitted = true;
					g_state.afterPleasureDialogueExpireSec = 0.0;
				}
				const auto choice = MapAfterPleasureChoice(eventName);
				const bool fromPleasureFailedDialogue =
					g_state.pleasureFailedDialogueActive || g_state.phase == Phase::PleasureFailedDialogue;
				if (g_state.phase != Phase::AfterPleasureDialogue && g_state.phase != Phase::PleasureFailedDialogue) {
					LogEventIgnoredLocked(eventName, "wrong_phase", info);
					return;
				}
				if (fromPleasureFailedDialogue && choice != AfterChoice::Redo) {
					LogEventIgnoredLocked(eventName, "pleasure_failed_blocks_normal_after_choice", info);
					return;
				}

				g_state.pleasureFailedDialogueActive = false;
				g_state.afterPleasureDialogueExpireSec = 0.0;
				g_state.pendingChoice = choice;

				if (choice == AfterChoice::Redo) {
					auto* redoActor = ResolveEventOrTrackedActorLocked(info);
					PrepareRedoSpeakerSwitchLocked(redoActor, info.actorFormID, "after_pleasure_redo_actor_switch");
					const bool combatFlagged = IsSceneCombatUnsafe(redoActor);
					if (combatFlagged) {
						// CB09A: Redo is requested from an already-owned AfterPleasure dialogue.
						// InCombat/Bleedout sources can legitimately still report combat flags while
						// the runtime is holding/pacifying the encounter. Rejecting here closes the
						// pleasure runtime before the Papyrus ArmRedoStart timer can start OStim.
						spdlog::info(
							"[TFD][PleasureRuntime][CB09A] redo allowed despite combat flag actor={:08X} cycle={} source={} phase={} reason=owned_afterpleasure_redo",
							redoActor ? redoActor->GetFormID() : info.actorFormID,
							g_state.sessionCycleId,
							ToString(g_state.source),
							ToString(g_state.phase));
					}

					g_state.redoPending = true;
					AdvancePhaseLocked(Phase::RedoPending, eventName);
					AdvancePhaseLocked(Phase::PleasureStartPending, eventName);
					g_state.afterPleasureCommitted = false;
					g_state.blocking = true;
					g_state.passiveLockActive = true;
					g_state.holdActive = true;
					SuppressActorDialogueForSceneLocked(
						redoActor ? redoActor : (info.actor ? info.actor : LookupActor(info.actorFormID)),
						"after_pleasure_redo_suppress_teammate_dialogue");
					LogEventAcceptedLocked(eventName, info, combatFlagged ? "redo_pending_combat_flag_allowed" : "redo_pending");
					return;
				}

				if (choice == AfterChoice::Recruit) {
					(void)CommitAfterPleasureRecruitLocked(info);
				}

				g_state.redoPending = false;
				AdvancePhaseLocked(Phase::Finalizing, eventName);
				AdvancePhaseLocked(Phase::Closed, eventName);

				auto* terminalActor = ResolveEventOrTrackedActorLocked(info);
				{
					const bool sourceIsCaptive =
						g_state.source == SourceContext::Captive ||
						info.sourceFlow == static_cast<int>(SourceContext::Captive);

					if (sourceIsCaptive) {
						// Captive returns to CaptiveIdle after AfterPleasure.  Do not leave
						// TFDAfterPleasureFaction / dialogue suppression on the captor,
						// otherwise that actor is still treated as temporarily suppressed
						// after lockpick escape while the rest of the camp goes hostile.
						ReleaseAfterPleasurePackageActor(terminalActor, eventName);
					}
					else {
						SuppressActorDialogueForSceneLocked(
							terminalActor,
							"after_pleasure_terminal_suppress_teammate_dialogue");
					}
				}

				const bool preserveBleedoutRecruitBridgeClamp =
					choice == AfterChoice::Recruit &&
					g_state.source == SourceContext::Bleedout &&
					g_state.queuedPreCombatSpeakerFormID != 0 &&
					g_state.queuedPreCombatConsumedFormID == (terminalActor ? terminalActor->GetFormID() : info.actorFormID) &&
					g_state.queuedPreCombatSource == SourceContext::Bleedout &&
					g_state.queuedPreCombatBleedoutBridgeToInCombat;

				ClearBridgeStateLocked();
				ClearHoldStateLocked(preserveBleedoutRecruitBridgeClamp);
				g_state.pendingChoice = AfterChoice::None;
				LogEventAcceptedLocked(eventName, info, preserveBleedoutRecruitBridgeClamp ? "finalize_close_bleedout_bridge_hold_preserved" : "finalize_close");
				return;
			}
		}

	}  // namespace

	void Install()
	{
		std::scoped_lock lk(g_lock);
		if (g_state.installed) {
			spdlog::info("[TFD][PleasureRuntime] Install skipped already_installed=1");
			return;
		}

		g_state.installed = true;
		ResetStateLocked("install");
		spdlog::info("[TFD][PleasureRuntime] Install");
	}

	void Shutdown()
	{
		std::scoped_lock lk(g_lock);
		if (!g_state.installed) {
			return;
		}

		ResetStateLocked("shutdown");
		g_state.installed = false;
		spdlog::info("[TFD][PleasureRuntime] Shutdown");
	}

	void Tick()
	{
		QueuedPreCombatAttempt attempt{};
		QueuedTerminalNeutralFinalize terminalFinalize{};
		QueuedAfterPleasureDialogueTimeout afterDialogueTimeout{};
		QueuedAbortedFlowComplete abortedComplete{};
		{
			std::scoped_lock lk(g_lock);
			if (!g_state.installed) {
				return;
			}

			const double now = NowSec();
			RefreshInCombatTruceLifecycleHoldLocked(now);
			MaintainScenePassiveHoldLocked(now);
			afterDialogueTimeout = TakeDueAfterPleasureDialogueTimeoutLocked(now);
			if (!afterDialogueTimeout.valid) {
				abortedComplete = TakeDueAbortedFlowCompleteLocked(now);
			}
			if (!afterDialogueTimeout.valid && !abortedComplete.valid) {
				attempt = TakeDueQueuedPreCombatAttemptLocked(now);
			}
			if (!afterDialogueTimeout.valid && !abortedComplete.valid && !attempt.valid) {
				terminalFinalize = TakeDueTerminalNeutralFinalizeLocked(now);
			}
		}

		if (afterDialogueTimeout.valid) {
			auto* timeoutActor = LookupActor(afterDialogueTimeout.actorFormID);
			const bool pleasureClearQueued = TFD::FlowController::QueueBridgeModEvent(
				kPleasureClearEvent,
				timeoutActor,
				"after_pleasure_dialogue_timeout",
				0.0f);
			const bool systemClearQueued = TFD::FlowController::QueueBridgeModEvent(
				kSystemEventClearAfterPleasureEvent,
				timeoutActor,
				"after_pleasure_dialogue_timeout",
				0.0f);
			const bool completeFlow = TFD::FlowController::Controller::GetSingleton().RequestCompleteAfterPleasure("after_pleasure_dialogue_timeout");
			spdlog::warn(
				"[TFD][PleasureRuntime] after pleasure dialogue timeout finalized actor={:08X} cycle={} source={} completeFlow={} pleasureClearQueued={} systemClearQueued={}",
				afterDialogueTimeout.actorFormID,
				afterDialogueTimeout.cycleId,
				ToString(afterDialogueTimeout.source),
				completeFlow ? 1 : 0,
				pleasureClearQueued ? 1 : 0,
				systemClearQueued ? 1 : 0);
			return;
		}

		if (abortedComplete.valid) {
			auto* abortActor = LookupActor(abortedComplete.actorFormID);
			const bool pleasureClearQueued = TFD::FlowController::QueueBridgeModEvent(
				kPleasureClearEvent,
				abortActor,
				"pleasure_aborted_no_afterpleasure",
				0.0f);
			const bool systemClearQueued = TFD::FlowController::QueueBridgeModEvent(
				kSystemEventClearAfterPleasureEvent,
				abortActor,
				"pleasure_aborted_no_afterpleasure",
				0.0f);
			const bool completeFlow = TFD::FlowController::Controller::GetSingleton().RequestCompleteAfterPleasure("pleasure_aborted_no_afterpleasure");
			spdlog::warn(
				"[TFD][PleasureRuntime] aborted flow completed actor={:08X} cycle={} source={} completeFlow={} pleasureClearQueued={} systemClearQueued={}",
				abortedComplete.actorFormID,
				abortedComplete.cycleId,
				ToString(abortedComplete.source),
				completeFlow ? 1 : 0,
				pleasureClearQueued ? 1 : 0,
				systemClearQueued ? 1 : 0);
			return;
		}

		if (!attempt.valid && terminalFinalize.valid) {
			auto* terminalActor = LookupActor(terminalFinalize.actorFormID);
			if (terminalActor) {
				if (terminalFinalize.source == SourceContext::InCombat) {
					QueueInCombatAliasClear(terminalActor, "after_pleasure_cycle_terminal_no_candidate");
				}
				else if (terminalFinalize.source == SourceContext::PreCombat) {
					QueuePreCombatAliasClear(terminalActor, "after_pleasure_cycle_terminal_no_candidate");
				}
				QueueTruceAliasUnassign(terminalActor, terminalFinalize.source == SourceContext::Bleedout ? "after_pleasure_cycle_terminal_no_candidate_bleedout" : "after_pleasure_cycle_terminal_no_candidate");
			}

			const bool completeFlow = terminalFinalize.source == SourceContext::Bleedout ?
				TFD::Bleedout::CompletePleasureCycleChainNeutral("after_pleasure_cycle_no_candidate_bleedout") :
				TFD::FlowController::Controller::GetSingleton().RequestCompleteAfterPleasure("after_pleasure_cycle_no_candidate");
			const bool flushDeferredSource = terminalFinalize.source == SourceContext::InCombat || terminalFinalize.source == SourceContext::Bleedout;
			const auto flushedDeferred = flushDeferredSource ?
				TFD::PleasureRuntime::FlushDeferredInCombatRecruits(terminalFinalize.source == SourceContext::Bleedout ? "after_pleasure_cycle_no_candidate_bleedout" : "after_pleasure_cycle_no_candidate") :
				0u;
			spdlog::info(
				"[TFD][PleasureRuntime][R94G] terminal neutral finalize actor={:08X} cycle={} completeFlow={} deferredRegistered={} reason=after_pleasure_cycle_no_candidate",
				terminalFinalize.actorFormID,
				terminalFinalize.cycleId,
				completeFlow ? 1 : 0,
				static_cast<unsigned int>(flushedDeferred));

			{
				std::scoped_lock lk(g_lock);
				MarkTerminalNeutralFinalizedLocked(terminalFinalize, completeFlow, "after_pleasure_cycle_no_candidate");
			}
			return;
		}

		if (!attempt.valid) {
			return;
		}

		auto* actor = LookupActor(attempt.actorFormID);
		TFD::InteractionRouter::Action action = TFD::InteractionRouter::Action::None;
		bool success = false;
		bool terminalFailure = false;
		bool completeFlow = false;
		const char* reason = "begin_failed";

		if (!actor) {
			terminalFailure = true;
			reason = "actor_missing";
		}
		else if (actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
			terminalFailure = true;
			reason = "actor_invalid";
		}
		else if (TFD::Recruit::IsRecruitLike(actor)) {
			terminalFailure = true;
			reason = "actor_already_recruit_like";
		}
		else if (attempt.source == SourceContext::Bleedout &&
			!attempt.bleedoutBridgeToInCombat &&
			!TFD::Bleedout::IsPleasureCycleDialogueCandidate(actor, LookupActor(attempt.consumedActorFormID))) {
			terminalFailure = true;
			reason = "bleedout_candidate_invalid";
		}
		else {
			auto* consumedActor = LookupActor(attempt.consumedActorFormID);
			if (consumedActor && consumedActor->GetFormID() != attempt.actorFormID) {
				if (attempt.source == SourceContext::InCombat || attempt.source == SourceContext::Bleedout) {
					FinalizeConsumedInCombatRecruitForCycle(
						consumedActor,
						actor,
						attempt.source == SourceContext::Bleedout ? "after_pleasure_cycle_consumed_bleedout_recruit" : "after_pleasure_cycle_consumed_recruit");
				}
				QueueTruceAliasUnassign(consumedActor, attempt.source == SourceContext::Bleedout ? "after_pleasure_cycle_consumed_bleedout_recruit" : "after_pleasure_cycle_consumed_recruit");
			}
			QueueTruceAliasPromoteSpeaker(
				actor,
				attempt.bleedoutBridgeToInCombat ?
					"after_pleasure_cycle_promote_next_bleedout_bridge_incombat_speaker" :
					(attempt.source == SourceContext::Bleedout ?
						"after_pleasure_cycle_promote_next_bleedout_speaker" :
						"after_pleasure_cycle_promote_next_speaker"));

			if (attempt.source == SourceContext::InCombat) {
				// R475A: AfterPleasure/InCombat cycle candidates can be pacified by
				// the same truce quarantine that protects the player during the
				// external scene. A selected desire/crowd candidate with no current
				// combat target is still a valid next speaker for this handoff.
				completeFlow = TFD::FlowController::Controller::GetSingleton().RequestCompleteAfterPleasure("after_pleasure_cycle_next_incombat");
				success = TFD::InCombatGreet::BeginForPleasureCycleActor(actor, &action, true);
			}
			else if (attempt.source == SourceContext::Bleedout && attempt.bleedoutBridgeToInCombat) {
				// R456A/R471A: The original Bleedout physical/session owner is intentionally
				// not re-entered after AfterPleasure Recruit.  The next crowd actor continues
				// as an InCombat ForceGreet.  Do not release the Bleedout quarantine before
				// BeginForPleasureCycleActor succeeds; otherwise the crowd can reacquire the
				// player during the 1-2 second bridge delay.
				// R472A: this bridge is allowed to promote a pacified standing threat.
				// The actor may no longer target the player because Bleedout quarantine
				// intentionally stopped combat to protect the downed player.
				completeFlow = TFD::FlowController::Controller::GetSingleton().RequestCompleteAfterPleasure("after_pleasure_cycle_next_bleedout_bridge_incombat");
				success = TFD::InCombatGreet::BeginForPleasureCycleActor(actor, &action, true);
				if (success) {
					TFD::HostilityController::ClearAggressionClampForSettledHandoff("bleedout_recruit_bridge_incombat_settled");
					TFD::Bleedout::FinalizePleasureBridgeToInCombat("after_pleasure_cycle_next_bleedout_bridge_incombat_settled");
				}
			}
			else if (attempt.source == SourceContext::Bleedout) {
				completeFlow = TFD::FlowController::Controller::GetSingleton().RequestCompleteAfterPleasure("after_pleasure_cycle_next_bleedout");
				success = TFD::Bleedout::BeginForPleasureCycleActor(actor, &action);
			}
			else {
				completeFlow = TFD::FlowController::Controller::GetSingleton().RequestCompleteAfterPleasure("after_pleasure_cycle_next_precombat");
				success = TFD::PreCombatGreet::BeginForPleasureCycleActor(actor, &action);
			}
			reason = success ? "begin_ok" : "begin_failed";
		}

		spdlog::info(
			"[TFD][PleasureRuntime][R94B] after pleasure cycle attempt source={} bridgeBleedoutToInCombat={} actor={:08X} consumed={:08X} attempt={} ok={} terminal={} completeFlow={} action={} reason={}",
			ToString(attempt.source),
			attempt.bleedoutBridgeToInCombat ? 1 : 0,
			attempt.actorFormID,
			attempt.consumedActorFormID,
			attempt.attemptIndex,
			success ? 1 : 0,
			terminalFailure ? 1 : 0,
			completeFlow ? 1 : 0,
			TFD::InteractionRouter::ToString(action),
			reason);

		{
			std::scoped_lock lk(g_lock);
			FinishQueuedPreCombatAttemptLocked(attempt, success, terminalFailure, reason);
		}
	}

	void ResetRuntime(std::string_view reason)
	{
		std::scoped_lock lk(g_lock);
		ResetStateLocked(reason);
	}

	void ResetForLoad(std::string_view reason)
	{
		std::scoped_lock lk(g_lock);
		ResetStateLocked(reason);
	}

	bool BeginPleasure(RE::Actor* speaker, SourceContext source, std::string_view reason)
	{
		std::scoped_lock lk(g_lock);

		if (!g_state.installed) {
			spdlog::warn("[TFD][PleasureRuntime] BeginPleasure rejected installed=0");
			return false;
		}
		if (!speaker || !IsSupportedSourceContext(source)) {
			spdlog::warn(
				"[TFD][PleasureRuntime][R392A] BeginPleasure rejected actor={:08X} source={} reason={} policy=unsupported_source_no_mutation",
				speaker ? speaker->GetFormID() : 0u,
				static_cast<int>(source),
				reason.empty() ? std::string{ "-" } : std::string{ reason });
			return false;
		}

		BeginNewCycleLocked(speaker, source, reason);
		if (source == SourceContext::InCombat && speaker) {
			TFD::HostilityController::ApplyAggressionClamp(speaker);
			if (auto* process = RE::ProcessLists::GetSingleton()) {
				const bool oldRunDetection = process->runDetection;
				process->runDetection = false;
				process->ClearCachedFactionFightReactions();
				process->StopCombatAndAlarmOnActor(speaker, false);
				process->runDetection = oldRunDetection;
			}
			speaker->StopCombat();
			// R240A: no forced sheathe at pleasure handoff; pacify/combat state owns this.
			speaker->EvaluatePackage(true, false);

			TFD::HostilityController::ScheduleStopCombatWaves(1800.0f, false, 3, 75);
			spdlog::info(
				"[TFD][PleasureRuntime][R331A] hard passive lock actor={:08X} source={} reason={}",
				speaker->GetFormID(),
				ToString(source),
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}
		else if (source == SourceContext::Bleedout && speaker) {
			PrepareActorForScenePassiveLocked(speaker, "r454a_bleedout_begin_pleasure_combat_quarantine");
			SuppressActorDialogueForSceneLocked(speaker, "r454a_bleedout_begin_pleasure_dialogue_cooldown");
			spdlog::info(
				"[TFD][PleasureRuntime][R454A] bleedout source BeginPleasure combat quarantine actor={:08X} reason={}",
				speaker->GetFormID(),
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}
		else if ((source == SourceContext::PreCombat || source == SourceContext::Captive) && speaker) {
			if (source == SourceContext::Captive) {
				TFD::HostilityController::TickCaptiveSuppression();
			}
			// R240A: no forced sheathe for already-safe sources.
			speaker->EvaluatePackage(false, true);
			spdlog::info(
				"[TFD][PleasureRuntime][W37] safe source no hard passive lock actor={:08X} source={} reason={}",
				speaker->GetFormID(),
				ToString(source),
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}
		AdvancePhaseLocked(Phase::PleasureStartPending, reason);
		g_state.blocking = true;
		g_state.holdActive = true;
		g_state.passiveLockActive = IsScenePassiveHoldSource(source);
		return true;
	}

	bool QueueNextCycleAfterTerminal(RE::Actor* currentActor, SourceContext source, std::string_view reason)
	{
		if (!currentActor || (source != SourceContext::PreCombat && source != SourceContext::InCombat && source != SourceContext::Bleedout)) {
			return false;
		}

		unsigned scannedCount = 0;
		auto* nextActor = SelectNextCycleActorForSource(currentActor, source, scannedCount);
		if (!nextActor) {
			spdlog::info(
				"[TFD][PleasureRuntime][R94C] terminal cycle queue skipped current={:08X} scanned={} source={} reason=no_candidate trigger={}",
				currentActor->GetFormID(),
				scannedCount,
				ToString(source),
				reason.empty() ? std::string{ "-" } : std::string{ reason });
			return false;
		}

		const double now = NowSec();
		const double initialDelay = (source == SourceContext::InCombat || source == SourceContext::Bleedout) ? 0.45 : kPreCombatCycleInitialDelaySec;
		{
			std::scoped_lock lk(g_lock);
			if (!g_state.installed) {
				return false;
			}
			g_state.queuedPreCombatSpeakerFormID = nextActor->GetFormID();
			g_state.queuedPreCombatConsumedFormID = currentActor->GetFormID();
			g_state.queuedPreCombatSource = source;
			g_state.queuedPreCombatBleedoutBridgeToInCombat = false;
			g_state.queuedPreCombatNextTrySec = now + initialDelay;
			g_state.queuedPreCombatExpireSec = now + kPreCombatCycleExpireSec;
			g_state.queuedPreCombatAttempts = 0;
		}

		spdlog::info(
			"[TFD][PleasureRuntime][R94C] terminal cycle queued current={:08X} next={:08X} scanned={} source={} delay={:.2f}s expire={:.2f}s reason={}",
			currentActor->GetFormID(),
			nextActor->GetFormID(),
			scannedCount,
			ToString(source),
			initialDelay,
			kPreCombatCycleExpireSec,
			reason.empty() ? std::string{ "-" } : std::string{ reason });
		return true;
	}

	bool HasQueuedCycleForConsumedActor(RE::Actor* currentActor, SourceContext source)
	{
		return currentActor ? HasQueuedCycleForConsumedActor(currentActor->GetFormID(), source) : false;
	}

	bool HasQueuedCycleForConsumedActor(std::uint32_t currentActorFormID, SourceContext source)
	{
		if (currentActorFormID == 0 || source == SourceContext::None) {
			return false;
		}

		std::scoped_lock lk(g_lock);
		return g_state.queuedPreCombatSpeakerFormID != 0 &&
			g_state.queuedPreCombatConsumedFormID == currentActorFormID &&
			g_state.queuedPreCombatSource == source;
	}

	bool HandleModEvent(const RE::BSFixedString& name, const RE::BSFixedString& strArg, float numArg, RE::TESForm* sender)
	{
		std::scoped_lock lk(g_lock);

		if (!g_state.installed) {
			return false;
		}

		const std::string eventName = name.empty() ? std::string{} : std::string(name.c_str());
		if (!IsRecognizedEvent(eventName)) {
			return false;
		}

		const auto info = ParseEventInfo(strArg, numArg, sender);
		HandleRecognizedEventLocked(eventName, info);
		return true;
	}

	void NoteAfterPleasureDialogueCommitted(std::string_view reason)
	{
		std::scoped_lock lk(g_lock);

		if (!g_state.installed) {
			return;
		}

		spdlog::info(
			"[TFD][PleasureRuntime] deprecated dialogue commit caller ignored phase={} cycle={} reason={}",
			ToString(g_state.phase),
			g_state.sessionCycleId,
			reason.empty() ? std::string{ "-" } : std::string{ reason });
	}

	void Break(std::string_view reason, bool clearHold, bool clearBridge, bool completeFlow)
	{
		std::scoped_lock lk(g_lock);

		if (clearHold) {
			ClearHoldStateLocked();
		}
		if (clearBridge) {
			ClearBridgeStateLocked();
		}

		g_state.redoPending = false;
		g_state.pendingChoice = AfterChoice::None;
		g_state.pleasureFailedDialogueActive = false;
		ClearQueuedTerminalNeutralFinalizeLocked(reason.empty() ? "break" : reason);
		g_state.active = false;
		g_state.blocking = false;
		g_state.phase = Phase::Closed;

		spdlog::info(
			"[TFD][PleasureRuntime] Break reason={} clearHold={} clearBridge={} completeFlow={} cycle={}",
			reason.empty() ? std::string{ "-" } : std::string{ reason },
			clearHold ? 1 : 0,
			clearBridge ? 1 : 0,
			completeFlow ? 1 : 0,
			g_state.sessionCycleId);
	}

	bool IsActive()
	{
		std::scoped_lock lk(g_lock);
		return g_state.active;
	}

	bool IsBlocking()
	{
		std::scoped_lock lk(g_lock);
		return g_state.blocking;
	}

	bool IsPassiveLockActive()
	{
		std::scoped_lock lk(g_lock);
		return g_state.passiveLockActive;
	}

	bool IsPleasureFailedDialogueActive()
	{
		std::scoped_lock lk(g_lock);
		return g_state.pleasureFailedDialogueActive;
	}

	Phase GetPhase()
	{
		std::scoped_lock lk(g_lock);
		return g_state.phase;
	}

	const char* GetPhaseName()
	{
		std::scoped_lock lk(g_lock);
		return ToString(g_state.phase);
	}

	SourceContext GetSourceContext()
	{
		std::scoped_lock lk(g_lock);
		return g_state.source;
	}

	const char* GetSourceContextName()
	{
		std::scoped_lock lk(g_lock);
		return ToString(g_state.source);
	}

	AfterChoice GetPendingAfterChoice()
	{
		std::scoped_lock lk(g_lock);
		return g_state.pendingChoice;
	}

	std::uint32_t GetSessionCycleId()
	{
		std::scoped_lock lk(g_lock);
		return g_state.sessionCycleId;
	}

	RE::Actor* GetPleasureSpeaker()
	{
		std::scoped_lock lk(g_lock);
		return LookupActor(g_state.pleasureSpeakerFormID);
	}

	RE::Actor* GetAfterPleasureSpeaker()
	{
		std::scoped_lock lk(g_lock);
		return LookupActor(g_state.afterPleasureSpeakerFormID);
	}

	RE::Actor* GetPrimarySpeaker()
	{
		std::scoped_lock lk(g_lock);
		if (auto* actor = LookupActor(g_state.afterPleasureSpeakerFormID)) {
			return actor;
		}
		return LookupActor(g_state.pleasureSpeakerFormID);
	}

	bool IsActorTracked(RE::Actor* actor)
	{
		std::scoped_lock lk(g_lock);
		return actor ? IsTrackedActorFormIDLocked(actor->GetFormID()) : false;
	}

	bool ShouldProtectPendingDialogue(RE::Actor* actor)
	{
		std::scoped_lock lk(g_lock);
		return actor ? ShouldProtectPendingDialogueLocked(actor->GetFormID()) : false;
	}

	bool ShouldSuppressTruceDialogue(RE::Actor* actor)
	{
		std::scoped_lock lk(g_lock);
		return actor ? ShouldSuppressTruceDialogueLocked(actor->GetFormID()) : false;
	}

	bool IsInCombatPleasureChainActive()
	{
		std::scoped_lock lk(g_lock);
		return IsInCombatPleasureChainActiveLocked();
	}

	std::size_t FlushDeferredInCombatRecruits(std::string_view reason)
	{
		std::vector<std::uint32_t> ids;
		{
			std::scoped_lock lk(g_lock);
			ids = TakeDeferredInCombatRecruitIdsLocked(reason.empty() ? "flush_deferred_incombat_recruits" : reason);
		}

		std::size_t registered = 0;
		for (auto actorId : ids) {
			auto* actor = LookupActor(actorId);
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				spdlog::warn(
					"[TFD][PleasureRuntime][R94G] deferred recruit flush skipped actor={:08X} reason=invalid flushReason={}",
					actorId,
					reason.empty() ? std::string{ "-" } : std::string{ reason });
				continue;
			}

			TFD::Recruit::MarkRecruitCommitPending(
				actor,
				4.0,
				TFD::Recruit::SourceFlow::Pleasure,
				"incombat_deferred_recruit_finalize_pending");

			const bool bleedoutNoCandidateSettle = IsBleedoutRecruitSettleReason(reason) && reason.find("no_candidate") != std::string_view::npos;

			TFD::Recruit::CommitOptions options{};
			options.sourceFlow = TFD::Recruit::SourceFlow::Pleasure;
			options.reason = bleedoutNoCandidateSettle ? "bleedout_deferred_recruit_settle_no_candidate" : "incombat_deferred_recruit_finalize";
			options.quarantineHostileFactions = true;
			options.clearCombat = true;
			options.evaluatePackage = !bleedoutNoCandidateSettle;
			options.detailedLog = true;
			options.throttleObserve = false;
			options.ensurePacifyAlliance = true;
			options.applyRuntimeProfile = !bleedoutNoCandidateSettle;

			const auto result = TFD::Recruit::CommitRecruit(actor, options);
			const bool settledClean = result.attempted && !result.rawHostileAfter && result.hostileFactionMatchesAfter == 0;
			const bool recruitLikeClean =
				TFD::Recruit::IsRecruitLike(actor) &&
				!TFD::Recruit::IsRawHostileToPlayer(actor) &&
				!TFD::Recruit::HasKnownHostileSourceFaction(actor);
			const bool clean = settledClean || recruitLikeClean;
			bool truceReleased = false;
			if (clean && bleedoutNoCandidateSettle) {
				StabilizeBleedoutRecruitSettle(actor, reason.empty() ? "bleedout_deferred_recruit_settle_no_candidate" : reason);
			}

			if (clean) {
				// R95B/R96D: the actor was kept inside the TruceInCombat session while the
				// crowd chain continued. Remove it as a FlowHandoff before the final
				// session cleanup, otherwise PlayerArmed/FightChoice can rehostile it
				// and the original bandit patrol package can win over teammate follow.
				truceReleased = TFD::HostilityController::ReleaseSingleTruceActorForCycle(
					actor,
					TFD::HostilityController::ReleaseReason::FlowHandoff,
					bleedoutNoCandidateSettle ? "bleedout_deferred_recruit_settle_no_candidate" : "incombat_deferred_recruit_finalize");
			}
			const bool aliasRegistered = clean && (bleedoutNoCandidateSettle ?
				TFD::TeammateManager::RegisterOrRefreshTeammateNowDeferredPackage(actor, "bleedout_deferred_recruit_settle_no_candidate") :
				TFD::TeammateManager::RegisterOrRefreshTeammateNowImmediatePackage(actor, "incombat_deferred_recruit_finalize"));
			if (aliasRegistered) {
				++registered;
			}

			spdlog::info(
				"[TFD][PleasureRuntime][R453A] deferred recruit flush actor={:08X} attempted={} skipped={} settledClean={} recruitLikeClean={} clean={} rawAfter={} hostileAfter={} truceReleased={} aliasRegistered={} bleedoutSettle={} reason={}",
				actorId,
				result.attempted ? 1 : 0,
				result.skipped ? 1 : 0,
				settledClean ? 1 : 0,
				recruitLikeClean ? 1 : 0,
				clean ? 1 : 0,
				result.rawHostileAfter ? 1 : 0,
				result.hostileFactionMatchesAfter,
				truceReleased ? 1 : 0,
				aliasRegistered ? 1 : 0,
				bleedoutNoCandidateSettle ? 1 : 0,
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}

		if (!ids.empty()) {
			TFD::TeammateManager::RefreshRecruitCapacityGlobals("incombat_deferred_recruit_flush");
		}

		return registered;
	}
}
