#include "TFDPleasureRuntime.h"
#include "TFDFlowController.h"
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
			bool redoPending{ false };
			AfterChoice pendingChoice{ AfterChoice::None };

			std::uint32_t queuedPreCombatSpeakerFormID{ 0 };
			std::uint32_t queuedPreCombatConsumedFormID{ 0 };
			SourceContext queuedPreCombatSource{ SourceContext::None };
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

			std::vector<std::uint32_t> deferredInCombatRecruitActorIds{};
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
		constexpr const char* kOStimSceneEndedEvent = "TFDOStimSceneEnded";

		constexpr const char* kAfterPleasureEnterEvent = "TFDAfterPleasureEnter";
		constexpr const char* kAfterPleasureLoopEnterEvent = "TFDAfterPleasureLoopEnter";

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
			case SourceContext::Victory:
				return "Victory";
			case SourceContext::Teammate:
				return "Teammate";
			case SourceContext::InCombat:
				return "InCombat";
			default:
				return "Unknown";
			}
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
			case static_cast<int>(SourceContext::Victory):
				return SourceContext::Victory;
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
			if (actor->IsWeaponDrawn()) {
				actor->DrawWeaponMagicHands(false);
			}
			actor->EvaluatePackage(false, true);
			actor->EvaluatePackage(true, true);

			spdlog::info(
				"[TFD][PleasureRuntime][R109] after pleasure package actor prepared actor={:08X} factionApplied={} reason={}",
				actor->GetFormID(),
				faction ? 1 : 0,
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}

		void ReleaseAfterPleasurePackageActor(RE::Actor* actor, std::string_view reason)
		{
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				return;
			}

			RemoveAfterPleasureFaction(actor, reason.empty() ? "after_pleasure_terminal_cleanup" : reason);
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
			}
			g_state.pleasureSpeakerFormID = 0;
			g_state.afterPleasureSpeakerFormID = 0;
		}

		void ClearBridgeStateLocked()
		{
			g_state.ostimThreadId = static_cast<std::uint32_t>(-1);
			g_state.bridgeState = 0;
			g_state.afterPleasureCommitted = false;
			g_state.afterPleasureDialogueExpireSec = 0.0;
			g_state.pleasureStartPendingAtSec = 0.0;
			g_state.pleasureActiveStartedAtSec = 0.0;
		}

		void ClearHoldStateLocked()
		{
			if (g_state.holdActive || g_state.passiveLockActive) {
				TFD::HostilityController::ClearAggressionClamp();
			}
			g_state.holdActive = false;
			g_state.passiveLockActive = false;
			g_state.blocking = false;
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
			}
			else if (next == Phase::PleasureActive) {
				g_state.pleasureActiveStartedAtSec = NowSec();
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
			}
			++g_state.sessionCycleId;
			g_state.source = source;
			g_state.flowOwnerToken = g_state.sessionCycleId;
			g_state.redoPending = false;
			g_state.pendingChoice = AfterChoice::None;
			g_state.afterPleasureCommitted = false;
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
			ClearQueuedPreCombatCycleLocked("new_cycle");
			ClearQueuedTerminalNeutralFinalizeLocked("new_cycle");

			spdlog::info(
				"[TFD][PleasureRuntime] BeginCycle cycle={} source={} speaker={:08X} reason={}",
				g_state.sessionCycleId,
				ToString(source),
				g_state.pleasureSpeakerFormID,
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
				eventName == kOStimSceneEndedEvent ||
				eventName == kAfterPleasureEnterEvent ||
				eventName == kAfterPleasureLoopEnterEvent ||
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

		void PrepareActorForScenePassiveLocked(RE::Actor* actor, std::string_view reason)
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
			if (actor->IsWeaponDrawn()) {
				actor->DrawWeaponMagicHands(false);
			}
			actor->EvaluatePackage(true, false);
			TFD::HostilityController::ScheduleStopCombatWaves(2400.0f, false, 5, 90);

			spdlog::info(
				"[TFD][PleasureRuntime] scene passive lock actor={:08X} source={} phase={} cycle={} reason={}",
				actor->GetFormID(),
				ToString(g_state.source),
				ToString(g_state.phase),
				g_state.sessionCycleId,
				reasonText);
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

			return nullptr;
		}

		RE::Actor* SelectNextPreCombatCycleActor(RE::Actor* currentActor, unsigned& scannedCount)
		{
			return SelectNextCycleActorForSource(currentActor, g_state.source, scannedCount);
		}

		void QueuePreCombatCycleLocked(RE::Actor* nextActor, RE::Actor* currentActor, unsigned scannedCount, std::string_view reason)
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
			g_state.queuedPreCombatNextTrySec = now + kPreCombatCycleInitialDelaySec;
			g_state.queuedPreCombatExpireSec = now + kPreCombatCycleExpireSec;
			g_state.queuedPreCombatAttempts = 0;

			spdlog::info(
				"[TFD][PleasureRuntime] after pleasure cycle queued current={:08X} next={:08X} scanned={} source={} delay={:.2f}s expire={:.2f}s reason={}",
				currentActor ? currentActor->GetFormID() : 0u,
				nextActor->GetFormID(),
				scannedCount,
				ToString(g_state.source),
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
			auto* nextActor = SelectNextPreCombatCycleActor(recruitedActor, scannedCount);
			QueuePreCombatCycleLocked(nextActor, recruitedActor, scannedCount, "after_pleasure_recruit_cycle");
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
			RE::Actor* predictedNextActor = deferredChainSource ? SelectNextPreCombatCycleActor(actor, preCommitScannedCount) : nullptr;
			const bool deferredChainWillContinue = deferredChainSource && predictedNextActor != nullptr;

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
			options.evaluatePackage = !deferredChainWillContinue;
			options.detailedLog = true;
			options.throttleObserve = false;
			options.ensurePacifyAlliance = true;
			options.applyRuntimeProfile = !deferredChainWillContinue;

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
			bool aliasRegistered = false;

			bool queuedNextCycle = false;
			if (clean) {
				queuedNextCycle = QueueNextPreCombatCycleIfNeededLocked(actor, true);
				if ((g_state.source == SourceContext::InCombat || g_state.source == SourceContext::Bleedout) && queuedNextCycle) {
					DeferInCombatRecruitAliasLocked(actor, g_state.source == SourceContext::Bleedout ? "after_pleasure_recruit_bleedout_chain_active" : "after_pleasure_recruit_chain_active");
					(void)TFD::HostilityController::DemoteTruceActorForCycleHold(actor, g_state.source == SourceContext::Bleedout ? "after_pleasure_recruit_bleedout_chain_defer" : "after_pleasure_recruit_chain_defer");
					// R130: Bleedout now follows the same deferred recruit/crowd-cycle pattern as InCombat.
					// Do not evaluate or register the converted speaker until the next crowd speaker is armed.
				}
				else {
					aliasRegistered = TFD::TeammateManager::RegisterOrRefreshTeammateNow(actor, "after_pleasure_recruit_commit");
					TFD::TeammateManager::RefreshRecruitCapacityGlobals("after_pleasure_recruit_commit");
				}
			}

			if (clean) {
				spdlog::info(
					"[TFD][PleasureRuntime] after pleasure recruit commit actor={:08X} cycle={} source={} eventFlow={} attempted={} skipped={} clean={} rawAfter={} hostileAfter={} aliasRegistered={} reason={}",
					actorFormID,
					g_state.sessionCycleId,
					ToString(g_state.source),
					info.sourceFlow,
					result.attempted ? 1 : 0,
					result.skipped ? 1 : 0,
					1,
					result.rawHostileAfter ? 1 : 0,
					result.hostileFactionMatchesAfter,
					aliasRegistered ? 1 : 0,
					"after_pleasure_recruit_commit");
			}
			else {
				spdlog::warn(
					"[TFD][PleasureRuntime] after pleasure recruit commit actor={:08X} cycle={} source={} eventFlow={} attempted={} skipped={} clean={} rawAfter={} hostileAfter={} aliasRegistered={} reason={}",
					actorFormID,
					g_state.sessionCycleId,
					ToString(g_state.source),
					info.sourceFlow,
					result.attempted ? 1 : 0,
					result.skipped ? 1 : 0,
					0,
					result.rawHostileAfter ? 1 : 0,
					result.hostileFactionMatchesAfter,
					aliasRegistered ? 1 : 0,
					"commit_not_clean_no_alias");
			}

			if (!clean) {
				(void)QueueNextPreCombatCycleIfNeededLocked(actor, false);
			}

			return clean;
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

			TFD::Recruit::CommitOptions options{};
			options.sourceFlow = TFD::Recruit::SourceFlow::Pleasure;
			options.reason = "incombat_consumed_recruit_cycle_finalize";
			options.quarantineHostileFactions = true;
			options.clearCombat = true;
			options.evaluatePackage = true;
			options.detailedLog = true;
			options.throttleObserve = false;
			options.ensurePacifyAlliance = true;
			options.applyRuntimeProfile = true;

			const auto result = TFD::Recruit::CommitRecruit(consumedActor, options);
			const bool recruitLikeClean =
				TFD::Recruit::IsRecruitLike(consumedActor) &&
				!TFD::Recruit::IsRawHostileToPlayer(consumedActor) &&
				!TFD::Recruit::HasKnownHostileSourceFaction(consumedActor);
			const bool settledClean = result.attempted && !result.rawHostileAfter && result.hostileFactionMatchesAfter == 0;
			const bool finalizeOk = settledClean || recruitLikeClean;

			bool truceReleased = false;
			bool aliasRegistered = false;
			if (finalizeOk) {
				truceReleased = TFD::HostilityController::ReleaseSingleTruceActorForCycle(
					consumedActor,
					TFD::HostilityController::ReleaseReason::FlowHandoff,
					"incombat_consumed_recruit_cycle_finalize");
				aliasRegistered = TFD::TeammateManager::RegisterOrRefreshTeammateNowImmediatePackage(
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
			if (eventName == kPleasureStartPendingEvent || eventName == kOStimSceneStartPendingEvent) {
				if (g_state.phase == Phase::Idle || g_state.phase == Phase::Closed) {
					const auto fallbackSource = g_state.source == SourceContext::None ? SourceContext::PreCombat : g_state.source;
					auto eventSource = SourceFromFlowValue(info.sourceFlow, fallbackSource);
					if (eventSource == SourceContext::None) {
						eventSource = SourceContext::PreCombat;
					}
					BeginNewCycleLocked(info.actor, eventSource, eventName);
					AdvancePhaseLocked(Phase::PleasureStartPending, eventName);
					g_state.blocking = true;
					g_state.passiveLockActive = true;
					g_state.holdActive = true;
					PrepareActorForScenePassiveLocked(info.actor ? info.actor : LookupActor(info.actorFormID), "scene_start_pending_passive_lock");
					SuppressActorDialogueForSceneLocked(info.actor ? info.actor : LookupActor(info.actorFormID), "scene_start_pending_suppress_dialogue");
					LogEventAcceptedLocked(eventName, info, "new_cycle");
					return;
				}
				if (g_state.phase == Phase::PleasureStartPending) {
					LogEventIgnoredLocked(eventName, "already_pending", info);
					return;
				}
				LogEventIgnoredLocked(eventName, "wrong_phase", info);
				return;
			}

			if (eventName == kPleasureStartedEvent || eventName == kOStimSceneStartedEvent) {
				if (g_state.phase == Phase::PleasureStartPending) {
					AdvancePhaseLocked(Phase::PleasureActive, eventName);
					g_state.holdActive = true;
					g_state.passiveLockActive = true;
					PrepareActorForScenePassiveLocked(info.actor ? info.actor : LookupActor(info.actorFormID), "scene_started_passive_lock");
					SuppressActorDialogueForSceneLocked(info.actor ? info.actor : LookupActor(info.actorFormID), "scene_started_suppress_dialogue");
					LogEventAcceptedLocked(eventName, info, "scene_active");
					return;
				}
				LogEventIgnoredLocked(eventName, "wrong_phase", info);
				return;
			}

			if (eventName == kPleasureEndedEvent || eventName == kOStimSceneEndedEvent) {
				if (g_state.phase == Phase::PleasureActive) {
					auto* eventActor = ResolveEventOrTrackedActorLocked(info);
					const double activeDuration = GetActiveSceneDurationLocked();
					const bool shortScene = activeDuration < kMinimumSceneActiveSec;
					const bool combatUnsafe = IsSceneCombatUnsafe(eventActor);

					if (shortScene || combatUnsafe) {
						spdlog::warn(
							"[TFD][PleasureRuntime] scene end rejected actor={:08X} cycle={} source={} duration={:.2f}s min={:.2f}s combatUnsafe={} reason={} no_afterpleasure=1",
							eventActor ? eventActor->GetFormID() : info.actorFormID,
							g_state.sessionCycleId,
							ToString(g_state.source),
							activeDuration,
							kMinimumSceneActiveSec,
							combatUnsafe ? 1 : 0,
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

					const auto afterSpeakerFormID = info.actorFormID ? info.actorFormID : g_state.pleasureSpeakerFormID;

					AdvancePhaseLocked(Phase::PleasureEnding, eventName);

					g_state.afterPleasureSpeakerFormID = afterSpeakerFormID;
					g_state.afterPleasureCommitted = false;
					g_state.pendingChoice = AfterChoice::None;
					g_state.redoPending = false;
					g_state.blocking = true;

					if (auto* afterActor = LookupActor(afterSpeakerFormID)) {
						PrepareAfterPleasurePackageActor(afterActor, "scene_end_await_after_dialogue");
					}

					AdvancePhaseLocked(Phase::AfterPleasureAwaitQuest, eventName);
					LogEventAcceptedLocked(eventName, info, "await_after_dialogue_package_owned");
					return;
				}
				LogEventIgnoredLocked(eventName, "wrong_phase", info);
				return;
			}

			if (eventName == kAfterPleasureEnterEvent) {
				if (g_state.phase == Phase::AfterPleasureAwaitQuest) {
					AdvancePhaseLocked(Phase::AfterPleasureDialogue, eventName);
					g_state.afterPleasureCommitted = true;
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
				if (g_state.phase == Phase::PleasureStartPending || g_state.phase == Phase::PleasureActive || g_state.phase == Phase::PleasureEnding || g_state.phase == Phase::RedoPending || g_state.phase == Phase::AfterPleasureDialogue) {
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
				if (g_state.phase != Phase::AfterPleasureDialogue) {
					LogEventIgnoredLocked(eventName, "wrong_phase", info);
					return;
				}

				const auto choice = MapAfterPleasureChoice(eventName);
				g_state.afterPleasureDialogueExpireSec = 0.0;
				g_state.pendingChoice = choice;

				if (choice == AfterChoice::Redo) {
					auto* redoActor = ResolveEventOrTrackedActorLocked(info);
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

				{
					auto* terminalActor = ResolveEventOrTrackedActorLocked(info);
					const bool sourceIsCaptive =
						g_state.source == SourceContext::Captive ||
						info.sourceFlow == static_cast<int>(SourceContext::Captive);

					if (sourceIsCaptive) {
						// Captive returns to CaptiveIdle after AfterPleasure.  Do not leave
						// TFDAfterPleasureFaction / dialogue suppression on the captor,
						// otherwise that actor is still treated as temporarily suppressed
						// after lockpick escape while the rest of the camp goes hostile.
						ReleaseAfterPleasurePackageActor(terminalActor, eventName);
					} else {
						SuppressActorDialogueForSceneLocked(
							terminalActor,
							"after_pleasure_terminal_suppress_teammate_dialogue");
					}
				}

				ClearBridgeStateLocked();
				ClearHoldStateLocked();
				g_state.pendingChoice = AfterChoice::None;
				LogEventAcceptedLocked(eventName, info, "finalize_close");
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
			QueueTruceAliasPromoteSpeaker(actor, attempt.source == SourceContext::Bleedout ? "after_pleasure_cycle_promote_next_bleedout_speaker" : "after_pleasure_cycle_promote_next_speaker");

			if (attempt.source == SourceContext::InCombat) {
				completeFlow = TFD::FlowController::Controller::GetSingleton().RequestCompleteAfterPleasure("after_pleasure_cycle_next_incombat");
				success = TFD::InCombatGreet::BeginForPleasureCycleActor(actor, &action);
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
			"[TFD][PleasureRuntime][R94B] after pleasure cycle attempt source={} actor={:08X} consumed={:08X} attempt={} ok={} terminal={} completeFlow={} action={} reason={}",
			ToString(attempt.source),
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

		BeginNewCycleLocked(speaker, source, reason);
		if ((source == SourceContext::PreCombat || source == SourceContext::Captive) && speaker) {
			if (source == SourceContext::Captive) {
				TFD::HostilityController::TickCaptiveSuppression();
			}

			TFD::HostilityController::ApplyAggressionClamp(speaker);
			if (auto* process = RE::ProcessLists::GetSingleton()) {
				const bool oldRunDetection = process->runDetection;
				process->runDetection = false;
				process->ClearCachedFactionFightReactions();
				process->StopCombatAndAlarmOnActor(speaker, false);
				process->runDetection = oldRunDetection;
			}
			speaker->StopCombat();
			if (speaker->IsWeaponDrawn()) {
				speaker->DrawWeaponMagicHands(false);
			}
			speaker->EvaluatePackage(true, false);

			const float waveRadius = source == SourceContext::Captive ? 2400.0f : 1600.0f;
			TFD::HostilityController::ScheduleStopCombatWaves(waveRadius, source == SourceContext::Captive, 4, 85);
			spdlog::info(
				"[TFD][PleasureRuntime] {} hard passive lock actor={:08X} reason={}",
				source == SourceContext::Captive ? "captive" : "precombat",
				speaker->GetFormID(),
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}
		AdvancePhaseLocked(Phase::PleasureStartPending, reason);
		g_state.blocking = true;
		g_state.passiveLockActive = true;
		g_state.holdActive = true;
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

			TFD::Recruit::CommitOptions options{};
			options.sourceFlow = TFD::Recruit::SourceFlow::Pleasure;
			options.reason = "incombat_deferred_recruit_finalize";
			options.quarantineHostileFactions = true;
			options.clearCombat = true;
			options.evaluatePackage = true;
			options.detailedLog = true;
			options.throttleObserve = false;
			options.ensurePacifyAlliance = true;
			options.applyRuntimeProfile = true;

			const auto result = TFD::Recruit::CommitRecruit(actor, options);
			const bool settledClean = result.attempted && !result.rawHostileAfter && result.hostileFactionMatchesAfter == 0;
			const bool recruitLikeClean =
				TFD::Recruit::IsRecruitLike(actor) &&
				!TFD::Recruit::IsRawHostileToPlayer(actor) &&
				!TFD::Recruit::HasKnownHostileSourceFaction(actor);
			const bool clean = settledClean || recruitLikeClean;
			bool truceReleased = false;
			if (clean) {
				// R95B/R96D: the actor was kept inside the TruceInCombat session while the
				// crowd chain continued. Remove it as a FlowHandoff before the final
				// session cleanup, otherwise PlayerArmed/FightChoice can rehostile it
				// and the original bandit patrol package can win over teammate follow.
				truceReleased = TFD::HostilityController::ReleaseSingleTruceActorForCycle(
					actor,
					TFD::HostilityController::ReleaseReason::FlowHandoff,
					"incombat_deferred_recruit_finalize");
			}
			const bool aliasRegistered = clean && TFD::TeammateManager::RegisterOrRefreshTeammateNowImmediatePackage(actor, "incombat_deferred_recruit_finalize");
			if (aliasRegistered) {
				++registered;
			}

			spdlog::info(
				"[TFD][PleasureRuntime][R96D] deferred recruit flush actor={:08X} attempted={} skipped={} settledClean={} recruitLikeClean={} clean={} rawAfter={} hostileAfter={} truceReleased={} aliasRegistered={} reason={}",
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
				reason.empty() ? std::string{ "-" } : std::string{ reason });
		}

		if (!ids.empty()) {
			TFD::TeammateManager::RefreshRecruitCapacityGlobals("incombat_deferred_recruit_flush");
		}

		return registered;
	}
}
