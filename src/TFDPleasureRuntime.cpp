#include "TFDPleasureRuntime.h"
#include "TFDFlowController.h"

#include <cmath>
#include <mutex>
#include <string>
#include <string_view>

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
		};

		std::mutex g_lock;
		RuntimeState g_state{};

		constexpr const char* kPleasureStartPendingEvent = "TFDPreCombatPleasureStartPending";
		constexpr const char* kPleasureStartedEvent = "TFDPreCombatPleasureStarted";
		constexpr const char* kPleasureFailedEvent = "TFDPreCombatPleasureFailed";
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
			default:
				return "Unknown";
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

		void ClearSpeakerStateLocked()
		{
			g_state.pleasureSpeakerFormID = 0;
			g_state.afterPleasureSpeakerFormID = 0;
		}

		void ClearBridgeStateLocked()
		{
			g_state.ostimThreadId = static_cast<std::uint32_t>(-1);
			g_state.bridgeState = 0;
			g_state.afterPleasureCommitted = false;
		}

		void ClearHoldStateLocked()
		{
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

			ClearSpeakerStateLocked();
			ClearBridgeStateLocked();
			ClearHoldStateLocked();

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
			++g_state.sessionCycleId;
			g_state.source = source;
			g_state.flowOwnerToken = g_state.sessionCycleId;
			g_state.redoPending = false;
			g_state.pendingChoice = AfterChoice::None;
			g_state.afterPleasureCommitted = false;
			g_state.ostimThreadId = static_cast<std::uint32_t>(-1);
			g_state.bridgeState = 0;
			g_state.pleasureSpeakerFormID = speaker ? speaker->GetFormID() : 0;
			g_state.afterPleasureSpeakerFormID = 0;
			g_state.active = true;
			g_state.holdActive = false;
			g_state.passiveLockActive = false;
			g_state.blocking = false;

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

			try {
				if (first != std::string::npos) {
					const auto a = token.substr(0, first);
					if (!a.empty()) {
						info.actorFormID = static_cast<std::uint32_t>(std::stoul(a, nullptr, 16));
					}

					if (second != std::string::npos) {
						const auto b = token.substr(first + 1, second - first - 1);
						const auto c = token.substr(second + 1);
						if (!b.empty()) {
							info.sourceFlow = std::stoi(b);
						}
						if (!c.empty()) {
							info.threadID = std::stoi(c);
						}
					}
					else {
						const auto b = token.substr(first + 1);
						if (!b.empty()) {
							info.sourceFlow = std::stoi(b);
						}
					}
				}
				else {
					info.actorFormID = static_cast<std::uint32_t>(std::stoul(token, nullptr, 16));
				}
			}
			catch (...) {
				// shell awal: gagal parse cukup diam
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

		void HandleRecognizedEventLocked(std::string_view eventName, const EventInfo& info)
		{
			if (eventName == kPleasureStartPendingEvent || eventName == kOStimSceneStartPendingEvent) {
				if (g_state.phase == Phase::Idle || g_state.phase == Phase::Closed) {
					BeginNewCycleLocked(info.actor, g_state.source == SourceContext::None ? SourceContext::PreCombat : g_state.source, eventName);
					AdvancePhaseLocked(Phase::PleasureStartPending, eventName);
					g_state.blocking = true;
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
					LogEventAcceptedLocked(eventName, info, "scene_active");
					return;
				}
				LogEventIgnoredLocked(eventName, "wrong_phase", info);
				return;
			}

			if (eventName == kPleasureEndedEvent || eventName == kOStimSceneEndedEvent) {
				if (g_state.phase == Phase::PleasureActive) {
					const auto afterSpeakerFormID = info.actorFormID ? info.actorFormID : g_state.pleasureSpeakerFormID;

					AdvancePhaseLocked(Phase::PleasureEnding, eventName);

					g_state.afterPleasureSpeakerFormID = afterSpeakerFormID;
					g_state.afterPleasureCommitted = false;
					g_state.pendingChoice = AfterChoice::None;
					g_state.redoPending = false;
					g_state.blocking = true;

					AdvancePhaseLocked(Phase::AfterPleasureAwaitQuest, eventName);
					LogEventAcceptedLocked(eventName, info, "await_after_dialogue");
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
					LogEventAcceptedLocked(eventName, info, "dialogue_open");
					return;
				}
				LogEventIgnoredLocked(eventName, "wrong_phase", info);
				return;
			}

			if (eventName == kAfterPleasureLoopEnterEvent) {
				HandleAfterPleasureLoopNoiseLocked(info, eventName);
				return;
			}

			if (eventName == kPleasureFailedEvent) {
				if (g_state.phase == Phase::PleasureStartPending || g_state.phase == Phase::PleasureActive || g_state.phase == Phase::PleasureEnding) {
					AdvancePhaseLocked(Phase::Finalizing, eventName);
					AdvancePhaseLocked(Phase::Closed, eventName);
					ClearBridgeStateLocked();
					ClearHoldStateLocked();
					LogEventAcceptedLocked(eventName, info, "failed_close");
					return;
				}
				LogEventIgnoredLocked(eventName, "late_or_wrong_phase", info);
				return;
			}

			if (IsAfterPleasureChoiceEvent(eventName)) {
				if (g_state.phase != Phase::AfterPleasureDialogue) {
					LogEventIgnoredLocked(eventName, "wrong_phase", info);
					return;
				}

				const auto choice = MapAfterPleasureChoice(eventName);
				g_state.pendingChoice = choice;

				if (choice == AfterChoice::Redo) {
					g_state.redoPending = true;
					AdvancePhaseLocked(Phase::RedoPending, eventName);
					AdvancePhaseLocked(Phase::PleasureStartPending, eventName);
					g_state.afterPleasureCommitted = false;
					g_state.blocking = true;
					LogEventAcceptedLocked(eventName, info, "redo_pending");
					return;
				}

				g_state.redoPending = false;
				AdvancePhaseLocked(Phase::Finalizing, eventName);
				AdvancePhaseLocked(Phase::Closed, eventName);
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
		std::scoped_lock lk(g_lock);
		if (!g_state.installed) {
			return;
		}
		// shell awal: timeout/lease resolver belum diaktifkan
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
		AdvancePhaseLocked(Phase::PleasureStartPending, reason);
		g_state.blocking = true;
		return true;
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
}
