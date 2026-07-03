#include "TFDBleedout.h"
#include "TFDActor.h"
#include "TFDCombatBehavior.h"
#include "TFDTransition.h"
#include "TFDVictory.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <array>
#include <limits>
#include <atomic>
#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <utility>
#include <thread>
#include <mutex>

#include "TFDFlowController.h"
#include "TFDForceGreetState.h"
#include "TFDDefeatMonitor.h"
#include "TFDSettings.h"
#include "TFDHostilityController.h"
#include "TFDTame.h"
#include "TFDTeammateManager.h"
#include "TFDBleedoutGreet.h"
#include "TFDInteractionRouter.h"
#include "TFDPleasureRuntime.h"
#include "TFDPayModel.h"
#include "TFDRecruit.h"
#include "TFDCaptive.h"
#include "RE/B/BGSKeyword.h"
#include "RE/B/BGSRefAlias.h"
#include "RE/T/TESQuest.h"

namespace TFD::Bleedout
{
	bool ActorHasAllowListFaction(RE::Actor* actor);

	namespace
	{
		using Clock = std::chrono::steady_clock;

		struct QueuedAfterPleasureCrowdContinuation
		{
			std::uint32_t nextSpeakerFormID{ 0 };
			std::uint32_t consumedActorFormID{ 0 };
			bool recruitChoice{ false };
			bool releaseSingle{ false };
			bool demoteHold{ false };
			bool flowComplete{ false };
			std::string reason{};
		};

		struct BattleObserveCombatBehaviorFactionCache
		{
			RE::TESFaction* retarget{ nullptr };
			RE::TESFaction* threat{ nullptr };
			bool tried{ false };
		};

		struct BattleObserveLogThrottleState
		{
			std::unordered_map<std::uint32_t, Clock::time_point> ignoredActorLogs{};
			Clock::time_point lastFilterLog{};
			std::size_t lastFilterInput{ 0 };
			std::size_t lastFilterOutput{ 0 };
			std::size_t lastFilterFollowers{ 0 };
		};

		std::mutex g_afterPleasureCrowdContinuationLock;
		QueuedAfterPleasureCrowdContinuation g_afterPleasureCrowdContinuation{};
		BattleObserveCombatBehaviorFactionCache g_battleObserveCombatBehaviorFactionCache{};
		std::mutex g_battleObserveLogThrottleLock;
		BattleObserveLogThrottleState g_battleObserveLogThrottle{};

		void QueueAfterPleasureCrowdContinuation(
			RE::Actor* consumedActor,
			RE::Actor* nextSpeaker,
			const char* reason,
			bool recruitChoice,
			bool releaseSingle,
			bool demoteHold,
			bool flowComplete)
		{
			std::scoped_lock lk(g_afterPleasureCrowdContinuationLock);
			g_afterPleasureCrowdContinuation.nextSpeakerFormID = nextSpeaker ? nextSpeaker->GetFormID() : 0;
			g_afterPleasureCrowdContinuation.consumedActorFormID = consumedActor ? consumedActor->GetFormID() : 0;
			g_afterPleasureCrowdContinuation.recruitChoice = recruitChoice;
			g_afterPleasureCrowdContinuation.releaseSingle = releaseSingle;
			g_afterPleasureCrowdContinuation.demoteHold = demoteHold;
			g_afterPleasureCrowdContinuation.flowComplete = flowComplete;
			g_afterPleasureCrowdContinuation.reason = reason && reason[0] ? reason : "bleedout_after_pleasure_cycle_next";
		}

		std::uint32_t ActorFormID(RE::Actor* actor)
		{
			return actor ? actor->GetFormID() : 0u;
		}

		bool IsCaptiveEscapeRebleedFlow()
		{
			if (TFD::Captive::IsEscapeBleedoutActive()) {
				return true;
			}
			const auto snapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
			return snapshot.root == TFD::FlowController::RootFlow::Captive &&
				snapshot.gate == TFD::FlowController::DecisionGate::PlayerBleedout &&
				(snapshot.sub == TFD::FlowController::SubFlow::EscapeAttempt ||
					snapshot.sub == TFD::FlowController::SubFlow::EscapeFailed ||
					snapshot.sub == TFD::FlowController::SubFlow::Recapture);
		}

		bool IsCaptiveEscapeBleedoutRetryAllowed()
		{
			if (TFD::Captive::IsEscapeBleedoutActive()) {
				return true;
			}
			const auto snapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
			return snapshot.root == TFD::FlowController::RootFlow::Captive &&
				snapshot.gate == TFD::FlowController::DecisionGate::PlayerBleedout &&
				(snapshot.sub == TFD::FlowController::SubFlow::EscapeFailed ||
					snapshot.sub == TFD::FlowController::SubFlow::Recapture);
		}

		bool IsCaptiveRecaptureGuardActive()
		{
			return TFD::Captive::IsRecaptureCommitActive() || TFD::Captive::IsRecaptureRecentlyCommitted();
		}

		bool ActorHasKeywordByEditorID(RE::Actor* actor, const char* editorID)
		{
			if (!actor || !editorID || !editorID[0]) {
				return false;
			}
			auto* kw = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(editorID);
			return kw && actor->HasKeyword(kw);
		}

		bool IsDialogueCreatureKeyword(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			return ActorHasKeywordByEditorID(actor, "ActorTypeCreature") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeAnimal") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeDragon") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeDaedra") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeGhost") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeUndead");
		}

		bool IsBleedoutDialogueCrowdActor(RE::Actor* actor, const RuntimeHostHandlers* handlers = nullptr)
		{
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				return false;
			}
			if (TFD::TeammateManager::IsActiveFollowerActor(actor) || TFD::TeammateManager::IsPlayerSideTeammateActor(actor)) {
				return false;
			}
			if (TFD::Recruit::IsRecruitLike(actor)) {
				return false;
			}
			if (IsDialogueCreatureKeyword(actor)) {
				return false;
			}
			if (handlers && handlers->isBleedCrowdSupportedAggressor && !handlers->isBleedCrowdSupportedAggressor(actor)) {
				return false;
			}
			return ActorHasKeywordByEditorID(actor, "ActorTypeNPC");
		}

		constexpr const char* kPrimeSpeakerEvent = "TFDBleedoutPrimeSpeaker";
		constexpr double kBleedoutReleaseGraceSeconds = 30.0;  // R216A: terminal Pay/Release needs a real released-safe window, not a 4-10s threshold race.

		void CancelPendingBleedoutDialogueOpen(const char* reason)
		{
			if (!TFD::InteractionRouter::DialogueOpen::IsActive()) {
				return;
			}

			const auto mode = TFD::InteractionRouter::DialogueOpen::GetMode();
			if (mode != TFD::InteractionRouter::DialogueOpen::Mode::Bleedout) {
				return;
			}

			TFD::InteractionRouter::DialogueOpen::Cancel();
			spdlog::info("[TFD][Bleedout][R112] canceled stale Bleedout DialogueOpen reason={}", reason ? reason : "unknown");
		}

		void ScrubTerminalReleaseGlobals(const char* reason)
		{
			const char* why = reason && reason[0] ? reason : "bleedout_terminal_release";
			auto setNeutralGlobal = [](const char* editorID) {
				if (auto* global = RE::TESForm::LookupByEditorID<RE::TESGlobal>(editorID)) {
					global->value = 0.0f;
				}
			};
			setNeutralGlobal("TFDPreCombatState");
			setNeutralGlobal("TFDInCombatState");
			setNeutralGlobal("TFDDefeatState");
			setNeutralGlobal("TFDDialogueState");
			spdlog::info("[TFD][Bleedout][R216A] terminal release globals scrubbed reason={}", why);
		}

		bool ReasonIs(const char* reason, const char* expected)
		{
			return reason && expected && std::strcmp(reason, expected) == 0;
		}

		bool IsBleedoutSetupClearReason(const char* reason)
		{
			return ReasonIs(reason, "start_bleed_window") ||
				ReasonIs(reason, "bleed_auto_forcegreet") ||
				ReasonIs(reason, "dialogue_closed_sticky_reopen") ||
				ReasonIs(reason, "bleed_dialogue_overdrive") ||
				ReasonIs(reason, "speaker_prime_retry") ||
				ReasonIs(reason, "bleed_retry") ||
				ReasonIs(reason, "mod_event_pleasure") ||
				ReasonIs(reason, "bleed_pleasure_handoff") ||
				ReasonIs(reason, "bleedout_pleasure");
		}

		struct BleedoutQuestRegistryCache
		{
			RE::TESQuest* quest{ nullptr };
			std::array<RE::BGSRefAlias*, 10> captorAliases{};
			bool resolved{ false };
		};

		BleedoutQuestRegistryCache g_bleedoutQuestRegistry{};
		std::uint32_t g_activeCaptorFormID = 0;
		RE::ActorHandle g_captorFactionHandle{};
		Clock::time_point g_captorBindLast{};
		std::vector<std::uint32_t> g_bleedoutDialogueFactionActorIds{};

		std::atomic_bool g_inBleedState{ false };
		float g_minHp{ 0.0f };
		Clock::time_point g_bleedStart{};
		int g_bleedLastSeconds = -1;
		bool g_bleedPaused = false;
		Clock::time_point g_bleedPauseStarted{};
		Clock::time_point g_bleedLastCalmPulse{};
		Clock::time_point g_bleedLastCrowdAssign{};
		std::vector<std::uint32_t> g_bleedCrowdAssigned{};
		std::vector<std::uint32_t> g_bleedPleasureCrowdSnapshot{};
		Clock::time_point g_bleedNoSpeakerTameLastAttempt{};
		std::uint32_t g_bleedSpeakerId = 0;

		std::uint32_t ResolveCaptiveRecaptureActorID()
		{
			const auto snapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
			if (snapshot.primaryActorFormID != 0) {
				return snapshot.primaryActorFormID;
			}
			if (g_bleedSpeakerId != 0) {
				return g_bleedSpeakerId;
			}
			if (g_activeCaptorFormID != 0) {
				return g_activeCaptorFormID;
			}
			return 0;
		}

		void ReassertCaptiveIdleFlowAfterBlackout(std::uint32_t actorFormID, const char* reason)
		{
			if (actorFormID == 0) {
				actorFormID = ResolveCaptiveRecaptureActorID();
			}
			if (actorFormID == 0) {
				spdlog::warn("[TFD][Bleedout] recapture flow reassert skipped: no actor reason={}", reason ? reason : "unknown");
				return;
			}

			auto& flow = TFD::FlowController::Controller::GetSingleton();
			const bool ok = flow.RequestCaptive(actorFormID, TFD::FlowController::CaptiveMode::Kidnapped, reason ? reason : "blackout_recapture_reassert");
			spdlog::info("[TFD][Bleedout] recapture flow reassert actor={:08X} ok={} reason={}",
				actorFormID,
				ok ? 1 : 0,
				reason ? reason : "unknown");
		}
		Clock::time_point g_bleedSpeakerKickLast{};
		int g_bleedSpeakerKickCount = 0;
		std::unordered_set<std::uint32_t> g_bleedRejectedSpeakerIds{};
		int g_bleedDialogueRetryCount = 0;
		bool g_bleedPendingCaptiveOutcome = false;
		bool g_bleedPendingNonCaptiveOutcome = false;
		bool g_bleedBattleObservePending = false;
		Clock::time_point g_bleedBattleObservePendingUntil{};
		Clock::time_point g_bleedBattleObservePendingLastRedirect{};
		int g_bleedBattleObservePendingEmptyEnemyTicks = 0;
		int g_bleedBattleObservePendingEmptyAllyTicks = 0;
		bool g_bleedBattleObserveActive = false;
		Clock::time_point g_bleedBattleObserveSince{};
		Clock::time_point g_bleedBattleObserveLastRedirect{};
		int g_bleedBattleObserveActiveEmptyEnemyTicks = 0;
		int g_bleedBattleObserveActiveEmptyAllyTicks = 0;
		std::uint32_t g_truceSessionId = 0;
		std::uint32_t g_noSpeakerTameSessionId = 0;
		std::uint32_t g_noSpeakerTamePrimaryId = 0;

	bool ShouldGateDestructiveBleedoutCleanup(const char* reason)
	{
		if (TFD::Bleedout::HasTerminalCommit() || IsCaptiveRecaptureGuardActive()) {
			return false;
		}

		const auto snapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
		const bool flowStillOwnsBleedout =
			OwnsCurrentFlow() ||
			snapshot.root == TFD::FlowController::RootFlow::Bleedout ||
			snapshot.contextRoot == TFD::FlowController::RootFlow::Bleedout ||
			snapshot.gate == TFD::FlowController::DecisionGate::PlayerBleedout;
		const bool bleedSessionStillOwned = g_truceSessionId != 0 || g_bleedSpeakerId != 0;
		const bool dialogueResidueStillOwned =
			TFD::BleedoutGreet::IsActive() ||
			TFD::BleedoutGreet::HasStickyReopenPending() ||
			TFD::BleedoutGreet::HasSeenDialogue() ||
			TFD::BleedoutGreet::HasFlowGreetConfirmed();
		const bool pleasureRuntimeStillOwnsHandoff =
			TFD::PleasureRuntime::IsActive() ||
			TFD::PleasureRuntime::IsBlocking();

		const bool block =
			flowStillOwnsBleedout ||
			bleedSessionStillOwned ||
			dialogueResidueStillOwned ||
			pleasureRuntimeStillOwnsHandoff;

		if (block) {
			spdlog::info(
				"[TFD][Bleedout][R484A] destructive cleanup gated reason={} flowOwns={} sessionOwned={} dialogueOwned={} pleasureOwned={} root={} gate={} sub={} speaker={:08X} session={}",
				reason && reason[0] ? reason : "unknown",
				flowStillOwnsBleedout ? 1 : 0,
				bleedSessionStillOwned ? 1 : 0,
				dialogueResidueStillOwned ? 1 : 0,
				pleasureRuntimeStillOwnsHandoff ? 1 : 0,
				TFD::FlowController::Controller::ToString(snapshot.root),
				TFD::FlowController::Controller::ToString(snapshot.gate),
				TFD::FlowController::Controller::ToString(snapshot.sub),
				g_bleedSpeakerId,
				g_truceSessionId);
		}

		return block;
	}

	void ReArmBleedoutTimeoutAfterGatedCleanup(const char* reason)
	{
		g_bleedStart = Clock::now();
		g_bleedLastSeconds = -1;
		TFD::BleedoutGreet::MarkStickyReopenPending(true, reason && reason[0] ? reason : "r484a_destructive_cleanup_gated");
	}



		void WriteQuestRefAlias(RE::TESQuest* quest, RE::BGSRefAlias* alias, RE::TESObjectREFR* ref)
		{
			if (!quest || !alias) {
				return;
			}

			RE::ObjectRefHandle handle{};
			if (ref) {
				handle = ref->CreateRefHandle();
			}

			RE::BSWriteLockGuard lock(quest->aliasAccessLock);
			auto it = quest->refAliasMap.find(alias->aliasID);
			if (ref) {
				if (it != quest->refAliasMap.end()) {
					it->second = handle;
				}
				else {
					quest->refAliasMap.insert({ alias->aliasID, handle });
				}
			}
			else if (it != quest->refAliasMap.end()) {
				quest->refAliasMap.erase(it);
			}
		}

		bool SendBridgeModEvent(const char* eventName, RE::TESForm* sender = nullptr, const char* strArg = "", float numArg = 0.0f)
		{
			if (!eventName || !eventName[0]) {
				return false;
			}

			auto* task = SKSE::GetTaskInterface();
			if (!task) {
				spdlog::warn("[TFD][BleedBridge] SendBridgeModEvent failed: no task interface event={}", eventName);
				return false;
			}

			const std::string name{ eventName };
			const std::string sarg{ strArg ? strArg : "" };
			const float narg = numArg;

			std::uint32_t actorHandle = 0;
			RE::FormID senderFormID = 0;

			if (sender) {
				senderFormID = sender->GetFormID();
				if (auto* actor = sender->As<RE::Actor>()) {
					actorHandle = actor->GetHandle().native_handle();
				}
			}

			task->AddTask([name, sarg, narg, actorHandle, senderFormID]() {
				RE::TESForm* outSender = nullptr;

				if (actorHandle != 0) {
					auto actorSp = RE::Actor::LookupByHandle(actorHandle);
					outSender = actorSp.get();
					if (!outSender && senderFormID != 0) {
						outSender = RE::TESForm::LookupByID(senderFormID);
					}
				}
				else if (senderFormID != 0) {
					outSender = RE::TESForm::LookupByID(senderFormID);
				}

				auto* src = SKSE::GetModCallbackEventSource();
				if (!src) {
					spdlog::warn("[TFD][BleedBridge] Dispatch skipped: no callback source event={} sender={:08X}", name, senderFormID);
					return;
				}

				SKSE::ModCallbackEvent ev{ name.c_str(), sarg.c_str(), narg, outSender };
				src->SendEvent(&ev);

				spdlog::info("[TFD][BleedBridge] Dispatch event={} sender={:08X} resolved={:08X}",
					name,
					senderFormID,
					outSender ? outSender->GetFormID() : 0u);
			});

			return true;
		}

		RE::Actor* ChooseNoSpeakerTamePrimary(const std::vector<RE::Actor*>& actors, RE::Actor* player, const RuntimeHostHandlers& handlers)
		{
			if (!player) {
				return nullptr;
			}

			RE::Actor* best = nullptr;
			float bestScore = std::numeric_limits<float>::max();
			for (auto* actor : actors) {
				if (!actor || actor->IsDead() || actor->IsDisabled()) {
					continue;
				}
				if (handlers.isCaptiveSupportedAggressor && handlers.isCaptiveSupportedAggressor(actor)) {
					continue;
				}
				const auto pp = player->GetPosition();
				const auto ap = actor->GetPosition();
				const float dx = ap.x - pp.x;
				const float dy = ap.y - pp.y;
				const float dz = ap.z - pp.z;
				const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
				float score = dist;
				if (actor->IsHostileToActor(player)) score -= 120.0f;
				if (actor->IsInCombat()) score -= 80.0f;
				if (score < bestScore) {
					bestScore = score;
					best = actor;
				}
			}

			return best;
		}

		RE::BGSListForm* ResolveBleedoutAllowList()
		{
			static RE::BGSListForm* s_allowList = nullptr;
			static bool s_tried = false;
			if (!s_allowList && !s_tried) {
				s_tried = true;
				s_allowList = RE::TESForm::LookupByEditorID<RE::BGSListForm>(TFD::Actor::Ops::kAllowListEditorId);
			}
			return s_allowList;
		}

		void ResolveBleedoutQuestRegistry()
		{
			if (g_bleedoutQuestRegistry.resolved) {
				return;
			}
			g_bleedoutQuestRegistry.resolved = true;
			g_bleedoutQuestRegistry.quest = nullptr;
			g_bleedoutQuestRegistry.captorAliases.fill(nullptr);

			constexpr std::array<const char*, 2> kBleedoutQuestEditorIds{
				"TFDBleedoutQuest",
				"TFDBleedOutQuest"
			};
			const char* matchedEditorId = nullptr;
			for (auto* editorId : kBleedoutQuestEditorIds) {
				auto* quest = RE::TESForm::LookupByEditorID<RE::TESQuest>(editorId);
				if (quest) {
					g_bleedoutQuestRegistry.quest = quest;
					matchedEditorId = editorId;
					break;
				}
			}
			if (!g_bleedoutQuestRegistry.quest) {
				spdlog::warn("[TFD][BleedQuest] bleedout quest not found editorIds=TFDBleedoutQuest|TFDBleedOutQuest");
				return;
			}

			RE::BGSRefAlias* speakerAlias = nullptr;
			for (auto* baseAlias : g_bleedoutQuestRegistry.quest->aliases) {
				auto* refAlias = skyrim_cast<RE::BGSRefAlias*>(baseAlias);
				if (!refAlias) {
					continue;
				}
				const auto aliasName = std::string(refAlias->aliasName.c_str());
				if (aliasName == "Speaker" || aliasName == "BleedoutSpeaker" || aliasName == "TFDBleedoutSpeaker") {
					speakerAlias = refAlias;
				}

				if (aliasName.rfind("Captor", 0) != 0 || aliasName.size() <= 6) {
					continue;
				}
				try {
					int slot = std::stoi(aliasName.substr(6));
					if (slot >= 1 && slot <= static_cast<int>(g_bleedoutQuestRegistry.captorAliases.size())) {
						g_bleedoutQuestRegistry.captorAliases[static_cast<std::size_t>(slot) - 1] = refAlias;
					}
				}
				catch (...) {}
			}

			// Current TFDBleedoutQuest uses a single Speaker alias rather than the older
			// Captor01..Captor10 alias set. Treat Speaker as the primary captor alias
			// so native faction/alias maintenance does not remove the Bleedout dialogue
			// faction during Captive EscapeFailed forcegreet.
			if (!g_bleedoutQuestRegistry.captorAliases[0] && speakerAlias) {
				g_bleedoutQuestRegistry.captorAliases[0] = speakerAlias;
			}

			std::size_t found = 0;
			for (auto* alias : g_bleedoutQuestRegistry.captorAliases) {
				if (alias) {
					++found;
				}
			}
			spdlog::info("[TFD][BleedQuest] bleedout quest resolved editorId={} quest={:08X} captorAliases={}",
				matchedEditorId ? matchedEditorId : "unknown",
				g_bleedoutQuestRegistry.quest ? g_bleedoutQuestRegistry.quest->GetFormID() : 0u,
				found);
		}

		RE::TESFaction* ResolveBleedoutCaptorFaction()
		{
			static RE::TESFaction* cached = nullptr;
			static bool tried = false;
			if (!tried) {
				tried = true;
				cached = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDBleedOutFaction");
				if (!cached) {
					cached = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDBleedoutFaction");
				}
				if (!cached) {
					spdlog::warn("[TFD][BleedQuest] bleedout captor faction not found editorIds=TFDBleedOutFaction|TFDBleedoutFaction");
				}
			}
			return cached;
		}


		RE::TESFaction* ResolveBleedoutDialogueFaction()
		{
			static RE::TESFaction* cached = nullptr;
			static bool tried = false;
			if (!tried) {
				tried = true;
				cached = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDBleedOutFaction");
				if (!cached) {
					cached = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDBleedoutFaction");
				}
				if (!cached) {
					spdlog::warn("[TFD][Bleedout][R114] bleedout dialogue faction not found editorIds=TFDBleedOutFaction|TFDBleedoutFaction");
				}
			}
			return cached;
		}

		RE::TESFaction* ResolveBleedoutPacifyFaction()
		{
			static RE::TESFaction* cached = nullptr;
			static bool tried = false;
			if (!tried) {
				tried = true;
				cached = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDPacifyFaction");
				if (!cached) {
					spdlog::warn("[TFD][Bleedout][R114] bleedout pacify faction not found editorId=TFDPacifyFaction");
				}
			}
			return cached;
		}

		void TrackBleedoutDialogueFactionActor(RE::Actor* actor)
		{
			if (!actor) {
				return;
			}
			const auto actorId = actor->GetFormID();
			if (actorId == 0) {
				return;
			}
			if (std::find(g_bleedoutDialogueFactionActorIds.begin(), g_bleedoutDialogueFactionActorIds.end(), actorId) == g_bleedoutDialogueFactionActorIds.end()) {
				g_bleedoutDialogueFactionActorIds.push_back(actorId);
			}
		}

		bool HasAnyBleedoutDialogueFaction(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			auto* bleedoutFaction = ResolveBleedoutDialogueFaction();
			auto* pacifyFaction = ResolveBleedoutPacifyFaction();
			return (bleedoutFaction && actor->IsInFaction(bleedoutFaction)) ||
				(pacifyFaction && actor->IsInFaction(pacifyFaction));
		}

		bool ShouldPreservePacifyFactionOnClear(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}

			if (TFD::HostilityController::IsPayDialoguePassiveGuardActive(actor)) {
				return true;
			}

			constexpr const char* kPreserveEditorIds[] = {
				"TFDTeammateFaction",
				"TFDTemporaryTeammateFaction",
				// TFDCaptiveFaction is not a pacify-preserve faction.
				// Escape must be able to remove TFDPacifyFaction while keeping
				// TFDCaptiveFaction for dialogue conditions.
				"TFDWorkingCaptiveFaction",
				"TFDSaviorFaction"
			};

			for (auto* editorId : kPreserveEditorIds) {
				auto* faction = RE::TESForm::LookupByEditorID<RE::TESFaction>(editorId);
				if (faction && actor->IsInFaction(faction)) {
					return true;
				}
			}
			return actor->IsPlayerTeammate();
		}

		void ApplyBleedoutDialogueFactionsInternal(RE::Actor* actor, const char* role, const char* reason)
		{
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				return;
			}

			auto* bleedoutFaction = ResolveBleedoutDialogueFaction();
			auto* pacifyFaction = ResolveBleedoutPacifyFaction();
			if (!bleedoutFaction && !pacifyFaction) {
				return;
			}

			bool addedBleedout = false;
			bool addedPacify = false;
			if (bleedoutFaction && !actor->IsInFaction(bleedoutFaction)) {
				actor->AddToFaction(bleedoutFaction, 0);
				addedBleedout = true;
			}
			if (pacifyFaction && !actor->IsInFaction(pacifyFaction)) {
				actor->AddToFaction(pacifyFaction, 0);
				addedPacify = true;
			}

			TrackBleedoutDialogueFactionActor(actor);

			if (addedBleedout || addedPacify) {
				if (auto* process = RE::ProcessLists::GetSingleton()) {
					process->ClearCachedFactionFightReactions();
				}
				actor->EvaluatePackage(false, true);
				actor->EvaluatePackage(true, true);
			}

			spdlog::info(
				"[TFD][Bleedout][R114] dialogue factions ensured actor={:08X} role={} bleedoutFaction={} pacifyFaction={} addedBleedout={} addedPacify={} reason={}",
				actor->GetFormID(),
				role ? role : "unknown",
				bleedoutFaction ? 1 : 0,
				pacifyFaction ? 1 : 0,
				addedBleedout ? 1 : 0,
				addedPacify ? 1 : 0,
				reason ? reason : "unknown");
		}

		void RemoveBleedoutDialogueFactionsInternal(RE::Actor* actor, const char* reason)
		{
			if (!actor) {
				return;
			}

			auto* bleedoutFaction = ResolveBleedoutDialogueFaction();
			auto* pacifyFaction = ResolveBleedoutPacifyFaction();
			bool removedBleedout = false;
			bool removedPacify = false;

			if (bleedoutFaction && actor->IsInFaction(bleedoutFaction)) {
				actor->RemoveFromFaction(bleedoutFaction);
				removedBleedout = true;
			}
			const bool preservePacify = ShouldPreservePacifyFactionOnClear(actor);
			if (pacifyFaction && actor->IsInFaction(pacifyFaction) && !preservePacify) {
				actor->RemoveFromFaction(pacifyFaction);
				removedPacify = true;
			}

			if (removedBleedout || removedPacify) {
				if (auto* process = RE::ProcessLists::GetSingleton()) {
					process->ClearCachedFactionFightReactions();
				}
				actor->EvaluatePackage(false, true);
				actor->EvaluatePackage(true, true);
				spdlog::info(
					"[TFD][Bleedout][R114] dialogue factions removed actor={:08X} removedBleedout={} removedPacify={} preservePacify={} reason={}",
					actor->GetFormID(),
					removedBleedout ? 1 : 0,
					removedPacify ? 1 : 0,
					preservePacify ? 1 : 0,
					reason ? reason : "unknown");
			}
		}


		void ClearBleedoutDialogueFactionForActorInternal(RE::Actor* actor, const char* reason)
		{
			if (!actor) {
				return;
			}
			auto* bleedoutFaction = ResolveBleedoutDialogueFaction();
			bool removedBleedout = false;
			if (bleedoutFaction && actor->IsInFaction(bleedoutFaction)) {
				actor->RemoveFromFaction(bleedoutFaction);
				removedBleedout = true;
			}
			const auto actorId = actor->GetFormID();
			g_bleedoutDialogueFactionActorIds.erase(
				std::remove(g_bleedoutDialogueFactionActorIds.begin(), g_bleedoutDialogueFactionActorIds.end(), actorId),
				g_bleedoutDialogueFactionActorIds.end());
			if (removedBleedout) {
				if (auto* process = RE::ProcessLists::GetSingleton()) {
					process->ClearCachedFactionFightReactions();
				}
				actor->EvaluatePackage(false, true);
				actor->EvaluatePackage(true, true);
				spdlog::info(
					"[TFD][Bleedout][W48] dialogue faction removed actor={:08X} preservePacify=1 reason={}",
					actorId,
					reason ? reason : "unknown");
			}
		}

		void ClearBleedoutDialogueFactionsInternal(const char* reason)
		{
			if (g_bleedoutDialogueFactionActorIds.empty()) {
				return;
			}

			const std::vector<std::uint32_t> ids = g_bleedoutDialogueFactionActorIds;
			g_bleedoutDialogueFactionActorIds.clear();
			std::uint32_t removedCount = 0;
			for (const auto actorId : ids) {
				auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorId);
				if (!actor) {
					continue;
				}
				const bool hadFaction = HasAnyBleedoutDialogueFaction(actor);
				RemoveBleedoutDialogueFactionsInternal(actor, reason ? reason : "clear_bleedout_dialogue_factions");
				if (hadFaction) {
					++removedCount;
				}
			}

			spdlog::info(
				"[TFD][Bleedout][R114] dialogue faction pack cleared tracked={} affected={} reason={}",
				static_cast<unsigned int>(ids.size()),
				removedCount,
				reason ? reason : "unknown");
		}

		void ClearBleedoutDialogueFactionsPreservePacifyInternal(const char* reason)
		{
			if (g_bleedoutDialogueFactionActorIds.empty()) {
				return;
			}

			const std::vector<std::uint32_t> ids = g_bleedoutDialogueFactionActorIds;
			g_bleedoutDialogueFactionActorIds.clear();
			std::uint32_t removedBleedoutCount = 0;
			for (const auto actorId : ids) {
				auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorId);
				if (!actor) {
					continue;
				}
				const bool hadBleedout = ResolveBleedoutDialogueFaction() && actor->IsInFaction(ResolveBleedoutDialogueFaction());
				ClearBleedoutDialogueFactionForActorInternal(actor, reason ? reason : "clear_bleedout_dialogue_factions_preserve_pacify");
				if (hadBleedout) {
					++removedBleedoutCount;
				}
			}

			spdlog::info(
				"[TFD][Bleedout][R471A] dialogue faction pack cleared preserve-pacify tracked={} removedBleedout={} reason={}",
				static_cast<unsigned int>(ids.size()),
				removedBleedoutCount,
				reason ? reason : "unknown");
		}

		void RemoveBleedoutCaptorFaction(RE::Actor* actor, const char* reason)
		{
			auto* faction = ResolveBleedoutCaptorFaction();
			if (!actor || !faction) {
				if (actor && g_captorFactionHandle) {
					auto tracked = g_captorFactionHandle.get().get();
					if (tracked == actor) {
						g_captorFactionHandle.reset();
					}
				}
				return;
			}

			if (actor->IsInFaction(faction)) {
				actor->RemoveFromFaction(faction);
				spdlog::info("[TFD][BleedQuest] captor faction removed actor={:08X} faction={:08X} reason={}",
					actor->GetFormID(),
					faction->GetFormID(),
					reason ? reason : "unknown");
			}

			if (g_captorFactionHandle) {
				auto tracked = g_captorFactionHandle.get().get();
				if (tracked == actor) {
					g_captorFactionHandle.reset();
				}
			}
		}

		void ApplyBleedoutCaptorFaction(RE::Actor* actor, const char* reason)
		{
			auto* faction = ResolveBleedoutCaptorFaction();
			if (!actor || !faction) {
				return;
			}

			if (g_captorFactionHandle) {
				auto tracked = g_captorFactionHandle.get().get();
				if (tracked && tracked != actor) {
					RemoveBleedoutCaptorFaction(tracked, "rebind_stale");
				}
			}

			if (!actor->IsInFaction(faction)) {
				actor->AddToFaction(faction, 0);
				spdlog::info("[TFD][BleedQuest] captor faction applied actor={:08X} faction={:08X} reason={}",
					actor->GetFormID(),
					faction->GetFormID(),
					reason ? reason : "unknown");
			}

			g_captorFactionHandle = actor->GetHandle();
		}

		DialogueOutcome g_dialogueOutcome = DialogueOutcome::None;
		std::atomic<std::uint8_t> g_terminalCommit{ static_cast<std::uint8_t>(TerminalCommit::None) };
		bool g_awaitingSystemEventOutcome = false;
		Clock::time_point g_systemEventUntil{};
		Clock::time_point g_systemEventLastDeferredLog{};

		constexpr std::size_t kBleedCrowdMaxActors = 10;

		float Distance3D(RE::Actor* a, RE::Actor* b)
		{
			if (!a || !b) {
				return 99999.0f;
			}
			const auto ap = a->GetPosition();
			const auto bp = b->GetPosition();
			const float dx = ap.x - bp.x;
			const float dy = ap.y - bp.y;
			const float dz = ap.z - bp.z;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		}

		float Distance3D(const RE::NiPoint3& a, const RE::NiPoint3& b)
		{
			const float dx = a.x - b.x;
			const float dy = a.y - b.y;
			const float dz = a.z - b.z;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		}

		bool IsReasonableSpeakerImpl(RE::Actor* actor, RE::Actor* player, float maxDist, float* outDistance, const SpeakerLogicHandlers& handlers)
		{
			if (outDistance) {
				*outDistance = 99999.0f;
			}
			if (!actor || !player) {
				return false;
			}
			if (!handlers.isStandingEnemyThresholdActor || !handlers.isStandingEnemyThresholdActor(actor) || !actor->Is3DLoaded()) {
				return false;
			}
			if (actor->GetFormID() == player->GetFormID()) {
				return false;
			}
			if (!handlers.isCaptiveSupportedAggressor || !handlers.isCaptiveSupportedAggressor(actor)) {
				return false;
			}
			if (!ActorHasAllowListFaction(actor)) {
				return false;
			}
			if (!handlers.isBleedSpaceCompatible || !handlers.isBleedSpaceCompatible(actor, player)) {
				return false;
			}
			if (!handlers.hasLineOfSightToPlayer || !handlers.hasLineOfSightToPlayer(actor, player)) {
				return false;
			}
			const float dist = Distance3D(actor, player);
			if (outDistance) {
				*outDistance = dist;
			}
			return dist <= maxDist;
		}

		bool IsReasonableHotkeySpeakerImpl(RE::Actor* actor, RE::Actor* player, float maxDist, const SpeakerLogicHandlers& handlers, float* outDistance)
		{
			if (outDistance) {
				*outDistance = 99999.0f;
			}
			if (!actor || !player) {
				return false;
			}
			if (actor == player || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
				return false;
			}
			if (!handlers.isStandingEnemyThresholdActor || !handlers.isStandingEnemyThresholdActor(actor)) {
				return false;
			}
			if (!handlers.isCaptiveSupportedAggressor || !handlers.isCaptiveSupportedAggressor(actor)) {
				return false;
			}
			if (!handlers.isBleedSpaceCompatible || !handlers.isBleedSpaceCompatible(actor, player)) {
				return false;
			}
			const float dist = Distance3D(actor, player);
			if (outDistance) {
				*outDistance = dist;
			}
			if (dist > maxDist) {
				return false;
			}
			auto* currentTarget = handlers.resolveCurrentCombatTarget ? handlers.resolveCurrentCombatTarget(actor) : nullptr;
			if (currentTarget == player) {
				return true;
			}
			if (currentTarget && handlers.isActiveFollowerActor && handlers.isActiveFollowerActor(currentTarget)) {
				return true;
			}
			if (actor->IsHostileToActor(player) || actor->IsInCombat()) {
				return true;
			}
			if (handlers.resolveLastAggressor) {
				if (auto* last = handlers.resolveLastAggressor(); last && last == actor) {
					return true;
				}
			}
			return ActorHasAllowListFaction(actor);
		}

		TFD::Actor::Snapshot BuildCoalitionSnapshot(float radius, const SpeakerLogicHandlers& handlers)
		{
			auto* player = handlers.getPlayer ? handlers.getPlayer() : nullptr;
			if (!player) {
				return {};
			}
			TFD::Actor::ScanOptions options{};
			options.radius = (std::max)(radius, 2000.0f);
			options.npcOnly = false;
			return TFD::Actor::BuildSnapshot(player, options);
		}

		RE::Actor* ResolveCoalitionSpeakerCandidate(const TFD::Actor::Snapshot& snapshot, RE::Actor* preferred,
			const std::function<bool(RE::Actor*, float&)>& validator)
		{
			auto tryActor = [&](RE::Actor* actor) -> RE::Actor* {
				float dist = 99999.0f;
				if (actor && validator && validator(actor, dist)) {
					return actor;
				}
				return nullptr;
			};

			if (preferred) {
				if (auto* info = TFD::Actor::FindActorInfo(snapshot, preferred); info && info->coalitionID >= 0) {
					if (auto* actor = tryActor(TFD::Actor::ResolveSpeakerCandidate(snapshot, info->coalitionID))) {
						return actor;
					}
				}
			}

			if (snapshot.winningCoalitionCandidateID >= 0) {
				if (auto* actor = tryActor(TFD::Actor::ResolveSpeakerCandidate(snapshot, snapshot.winningCoalitionCandidateID))) {
					return actor;
				}
			}

			return nullptr;
		}

		RE::Actor* FindBestHotkeySpeakerImpl(float radius, float maxDist, RE::Actor* preferred, const SpeakerLogicHandlers& handlers)
		{
			auto* player = handlers.getPlayer ? handlers.getPlayer() : nullptr;
			if (!player) {
				return nullptr;
			}

			float preferredDist = 99999.0f;
			if (preferred && IsReasonableHotkeySpeakerImpl(preferred, player, maxDist, handlers, &preferredDist)) {
				return preferred;
			}

			auto snapshot = BuildCoalitionSnapshot(radius, handlers);
			auto validator = [&](RE::Actor* actor, float& outDist) {
				return IsReasonableHotkeySpeakerImpl(actor, player, maxDist, handlers, &outDist);
			};
			if (auto* coalitionSpeaker = ResolveCoalitionSpeakerCandidate(snapshot, preferred, validator)) {
				return coalitionSpeaker;
			}

			RE::Actor* best = nullptr;
			float bestScore = std::numeric_limits<float>::max();
			auto* lastAggressor = handlers.resolveLastAggressor ? handlers.resolveLastAggressor() : nullptr;
			for (const auto& info : snapshot.actors) {
				auto* actor = info.get();
				float dist = 99999.0f;
				if (!IsReasonableHotkeySpeakerImpl(actor, player, maxDist, handlers, &dist)) {
					continue;
				}

				auto* currentTarget = handlers.resolveCurrentCombatTarget ? handlers.resolveCurrentCombatTarget(actor) : nullptr;
				const bool targetsPlayer = currentTarget == player;
				const bool targetsFollower = currentTarget && handlers.isActiveFollowerActor && handlers.isActiveFollowerActor(currentTarget);
				const bool hostile = info.hostileToPlayer || actor->IsHostileToActor(player);
				const bool inCombat = info.inCombat || actor->IsInCombat();
				const bool los = handlers.hasLineOfSightToPlayer && handlers.hasLineOfSightToPlayer(actor, player);
				float score = dist;
				if (targetsPlayer) score -= 900.0f;
				if (targetsFollower) score -= 650.0f;
				if (hostile) score -= 240.0f;
				if (inCombat) score -= 180.0f;
				if (los) score -= 90.0f;
				if (actor == preferred) score -= 400.0f;
				if (lastAggressor && lastAggressor == actor) score -= 300.0f;
				if (score < bestScore) {
					bestScore = score;
					best = actor;
				}
			}
			return best;
		}

	}

	std::vector<RE::Actor*> CollectCrowd(float radius, RE::Actor* preferred, bool preserveAssigned, const SpeakerLogicHandlers& handlers)
	{
		std::vector<std::pair<float, RE::Actor*>> scored;
		std::unordered_set<std::uint32_t> scoredIDs;

		auto* player = handlers.getPlayer ? handlers.getPlayer() : nullptr;
		if (!player) {
			return {};
		}

		const float scanRadius = (std::max)(radius, 12000.0f);
		auto addCandidate = [&](RE::Actor* actor, float dist, bool hostileHint, bool inCombatHint, bool requireCombatContext) {
			if (!actor || actor->IsDead() || actor->IsDisabled()) return;
			if (!actor->Is3DLoaded()) return;
			if (actor->GetFormID() == player->GetFormID()) return;
			if (dist > scanRadius) return;
			if (!handlers.isBleedSpaceCompatible || !handlers.isBleedSpaceCompatible(actor, player)) return;
			if (!handlers.isBleedCrowdSupportedAggressor || !handlers.isBleedCrowdSupportedAggressor(actor)) return;

			const auto actorID = actor->GetFormID();
			if (scoredIDs.find(actorID) != scoredIDs.end()) return;

			auto* currentTarget = handlers.resolveCurrentCombatTarget ? handlers.resolveCurrentCombatTarget(actor) : nullptr;
			const bool targetsPlayer = currentTarget == player;
			const bool targetsFollower = currentTarget && handlers.isActiveFollowerActor && handlers.isActiveFollowerActor(currentTarget);
			const bool hostile = hostileHint || actor->IsHostileToActor(player);
			const bool inCombat = inCombatHint || actor->IsInCombat();
			const bool preserved = preserveAssigned && handlers.isPreservedAssigned && handlers.isPreservedAssigned(actor);
			const bool combatRelevant = targetsPlayer || targetsFollower || hostile || inCombat || preserved || actor == preferred;
			if (requireCombatContext && !combatRelevant) return;

			float score = dist;
			if (actor == preferred) score -= 1000.0f;
			if (targetsPlayer) score -= 320.0f;
			if (targetsFollower) score -= 240.0f;
			if (hostile) score -= 160.0f;
			if (inCombat) score -= 110.0f;
			if (preserved) score -= 90.0f;
			if (handlers.isActorCloseAndFront && handlers.isActorCloseAndFront(actor, player, 320.0f)) score -= 120.0f;

			scoredIDs.insert(actorID);
			scored.emplace_back(score, actor);
		};

		// R104: do not let the coalition resolver become the crowd cap.  Bleedout
		// crowd must behave like PreCombat/InCombat: all hostile/in-combat actors
		// inside the 12000 bubble are eligible, without LoS and without requiring
		// the same coalition as the selected speaker.
		auto snapshot = BuildCoalitionSnapshot(scanRadius, handlers);
		std::int32_t coalitionID = -1;
		if (preferred) {
			if (auto* info = TFD::Actor::FindActorInfo(snapshot, preferred)) {
				coalitionID = info->coalitionID;
			}
		}
		if (coalitionID < 0) {
			coalitionID = snapshot.winningCoalitionCandidateID;
		}
		if (coalitionID >= 0) {
			for (auto* actor : TFD::Actor::ResolveCrowdCandidates(snapshot, coalitionID)) {
				addCandidate(actor, Distance3D(actor, player), true, actor ? actor->IsInCombat() : false, false);
			}
		}

		auto fallbackSnapshot = TFD::Actor::BuildSnapshot(scanRadius, false);
		for (const auto& info : fallbackSnapshot.actors) {
			auto* actor = info.get();
			addCandidate(actor, info.dist, info.hostileToPlayer, info.inCombat, true);
		}

		std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
			if (a.first != b.first) {
				return a.first < b.first;
			}
			if (!a.second || !b.second) {
				return a.second != nullptr;
			}
			return a.second->GetFormID() < b.second->GetFormID();
		});

		std::vector<RE::Actor*> result;
		result.reserve((std::min)(scored.size(), kBleedCrowdMaxActors));
		for (const auto& [score, actor] : scored) {
			(void)score;
			if (!actor) {
				continue;
			}
			const auto id = actor->GetFormID();
			bool seen = false;
			for (auto* existing : result) {
				if (existing && existing->GetFormID() == id) {
					seen = true;
					break;
				}
			}
			if (seen) {
				continue;
			}
			result.push_back(actor);
			if (result.size() >= kBleedCrowdMaxActors) {
				break;
			}
		}

		if (preferred) {
			const auto preferredId = preferred->GetFormID();
			auto it = std::find_if(result.begin(), result.end(), [preferredId](RE::Actor* actor) {
				return actor && actor->GetFormID() == preferredId;
			});
			if (it == result.end()) {
				if (result.size() >= kBleedCrowdMaxActors) {
					result.pop_back();
				}
				result.insert(result.begin(), preferred);
			} else if (it != result.begin()) {
				std::rotate(result.begin(), it, it + 1);
			}
		}

		spdlog::info("[TFD][Bleedout][R104] collect crowd radius={:.0f} preferred={:08X} scored={} result={}",
			scanRadius,
			preferred ? preferred->GetFormID() : 0u,
			scored.size(),
			result.size());

		return result;
	}

	RE::Actor* ResolveCombatTargetForBleedoutCrowd(RE::Actor* actor)
	{
		if (!actor) {
			return nullptr;
		}
		auto sp = actor->GetActorRuntimeData().currentCombatTarget.get();
		return sp.get();
	}

	bool IsBleedoutCrowdPlayerSideTarget(RE::Actor* actor, RE::Actor* player)
	{
		if (!actor || !player) {
			return false;
		}
		if (actor->GetFormID() == player->GetFormID()) {
			return true;
		}
		return TFD::TeammateManager::IsActiveFollowerActor(actor) ||
			TFD::TeammateManager::IsPlayerSideTeammateActor(actor) ||
			TFD::Tame::IsCompanion(actor);
	}

	bool IsHostileToBleedoutEncounter(RE::Actor* actor, RE::Actor* player, RE::Actor* speaker)
	{
		if (!actor) {
			return false;
		}
		if (player && (actor->IsHostileToActor(player) || player->IsHostileToActor(actor))) {
			return true;
		}
		if (speaker && (actor->IsHostileToActor(speaker) || speaker->IsHostileToActor(actor))) {
			return true;
		}
		return false;
	}

	bool IsNearAcceptedBleedoutCrowd(RE::Actor* actor, const std::vector<RE::Actor*>& accepted, float maxDistance)
	{
		if (!actor) {
			return false;
		}
		for (auto* acceptedActor : accepted) {
			if (!acceptedActor || acceptedActor == actor) {
				continue;
			}
			if (actor->GetPosition().GetDistance(acceptedActor->GetPosition()) <= maxDistance) {
				return true;
			}
		}
		return false;
	}
	bool IsBleedoutCrowdEngagedForEncounter(
		RE::Actor* actor,
		RE::Actor* player,
		RE::Actor* speaker,
		const RuntimeHostHandlers& handlers,
		bool observedEnemy,
		const std::vector<RE::Actor*>& acceptedCrowd,
		const char** outReason)
	{
		if (outReason) {
			*outReason = "none";
		}
		if (!actor || !player) {
			return false;
		}
		if (handlers.isObserverAlly && handlers.isObserverAlly(actor)) {
			return false;
		}

		auto* currentTarget = ResolveCombatTargetForBleedoutCrowd(actor);
		if (currentTarget && currentTarget->GetFormID() == player->GetFormID()) {
			if (outReason) *outReason = "direct_target_player";
			return true;
		}
		if (speaker && currentTarget && currentTarget->GetFormID() == speaker->GetFormID()) {
			if (outReason) *outReason = "direct_target_speaker";
			return true;
		}
		if (currentTarget && IsBleedoutCrowdPlayerSideTarget(currentTarget, player)) {
			if (outReason) *outReason = "direct_target_player_side";
			return true;
		}

		const bool combatPosture = actor->IsInCombat() || actor->IsWeaponDrawn();
		const bool hostileToEncounter = IsHostileToBleedoutEncounter(actor, player, speaker);
		if (observedEnemy && combatPosture) {
			if (outReason) *outReason = actor->IsInCombat() ? "observed_in_combat" : "observed_weapon_drawn";
			return true;
		}

		const float distToPlayer = actor->GetPosition().GetDistance(player->GetPosition());
		const float distToSpeaker = speaker ? actor->GetPosition().GetDistance(speaker->GetPosition()) : 99999.0f;
		const bool localToEncounter = distToPlayer <= 3000.0f || distToSpeaker <= 3000.0f;

		// CB07: no hard LoS requirement. LoS is only a nearby bonus proof, never a
		// required proof and never sufficient for distant actors. This keeps actors
		// behind pillars eligible via combat/weapon/target proof while preventing a
		// far scripted actor from joining only because it is inside the 12000 scan.
		if (localToEncounter) {
			if (hostileToEncounter && actor->IsInCombat()) {
				if (outReason) *outReason = "local_hostile_in_combat";
				return true;
			}
			if (hostileToEncounter && actor->IsWeaponDrawn()) {
				if (outReason) *outReason = "local_hostile_weapon_drawn";
				return true;
			}
			if (hostileToEncounter && currentTarget) {
				if (outReason) *outReason = "local_hostile_has_target";
				return true;
			}
		}

		if (hostileToEncounter && IsNearAcceptedBleedoutCrowd(actor, acceptedCrowd, 1500.0f)) {
			if (outReason) *outReason = "near_accepted_pack_link";
			return true;
		}

		return false;
	}

	std::vector<RE::Actor*> CollectBleedoutCrowdUsingInCombatPattern(
		RE::Actor* player,
		RE::Actor* speaker,
		float radius,
		bool preserveAssigned,
		const RuntimeHostHandlers& handlers)
	{
		std::vector<RE::Actor*> result;
		if (!player) {
			return result;
		}

		const float scanRadius = (std::max)(radius, 12000.0f);
		if (speaker) {
			result = TFD::HostilityController::CollectInCombatStyleTruceActors(player, speaker, scanRadius);
			spdlog::info("[TFD][Bleedout][R105B] crowd source=incombat_pattern speaker={:08X} radius={:.0f} actors={}",
				speaker->GetFormID(),
				scanRadius,
				static_cast<unsigned int>(result.size()));
		}

		if (result.empty() && handlers.collectBleedoutCrowd) {
			result = handlers.collectBleedoutCrowd(scanRadius, speaker, preserveAssigned);
			spdlog::warn("[TFD][Bleedout][R105B] incombat-pattern crowd empty, fallback bleedout collector speaker={:08X} actors={}",
				speaker ? speaker->GetFormID() : 0u,
				static_cast<unsigned int>(result.size()));
		}

		std::unordered_set<std::uint32_t> observedEnemyIDs;
		if (handlers.collectStandingEnemiesFromSnapshot) {
			for (auto* observedEnemy : handlers.collectStandingEnemiesFromSnapshot()) {
				const auto observedID = ActorFormID(observedEnemy);
				if (observedID != 0) {
					observedEnemyIDs.insert(observedID);
				}
			}
		}

		std::vector<RE::Actor*> filtered;
		filtered.reserve(result.size());
		std::unordered_set<std::uint32_t> seen;
		const auto speakerID = ActorFormID(speaker);
		for (auto* actor : result) {
			if (!actor) {
				continue;
			}
			const auto actorID = actor->GetFormID();
			if (actorID == 0 || actorID == speakerID) {
				continue;
			}
			if (!seen.insert(actorID).second) {
				continue;
			}
			if (!IsBleedoutDialogueCrowdActor(actor, &handlers)) {
				spdlog::info("[TFD][Bleedout][R129] reject non-dialogue bleed crowd actor={:08X} source=incombat_pattern", actorID);
				continue;
			}
			if (handlers.isBleedSpaceCompatible && !handlers.isBleedSpaceCompatible(actor, player)) {
				continue;
			}
			const char* engagementReason = "none";
			const bool observed = observedEnemyIDs.find(actorID) != observedEnemyIDs.end();
			if (!IsBleedoutCrowdEngagedForEncounter(actor, player, speaker, handlers, observed, filtered, &engagementReason)) {
				spdlog::info("[TFD][Bleedout][CB07] reject disengaged bleed crowd actor={:08X} speaker={:08X} distPlayer={:.1f} distSpeaker={:.1f} inCombat={} weaponDrawn={} target={:08X} observed={} hostile={} reason=no_engagement_proof",
					actorID,
					speakerID,
					actor->GetPosition().GetDistance(player->GetPosition()),
					speaker ? actor->GetPosition().GetDistance(speaker->GetPosition()) : 99999.0f,
					actor->IsInCombat() ? 1 : 0,
					actor->IsWeaponDrawn() ? 1 : 0,
					ResolveCombatTargetForBleedoutCrowd(actor) ? ResolveCombatTargetForBleedoutCrowd(actor)->GetFormID() : 0u,
					observed ? 1 : 0,
					IsHostileToBleedoutEncounter(actor, player, speaker) ? 1 : 0);
				continue;
			}
			spdlog::info("[TFD][Bleedout][CB07] accept engaged bleed crowd actor={:08X} speaker={:08X} reason={}",
				actorID,
				speakerID,
				engagementReason ? engagementReason : "unknown");
			filtered.push_back(actor);
		}
		spdlog::info("[TFD][Bleedout][CB07] filtered bleed crowd speaker={:08X} input={} output={} observedSnapshot={}",
			speaker ? speaker->GetFormID() : 0u,
			static_cast<unsigned int>(result.size()),
			static_cast<unsigned int>(filtered.size()),
			static_cast<unsigned int>(observedEnemyIDs.size()));
		return filtered;
	}

	std::size_t ReleaseUnacceptedBleedoutTruceActors(RE::Actor* speaker, const std::vector<RE::Actor*>& acceptedCrowd, const char* reason)
	{
		if (!speaker) {
			return 0;
		}

		std::unordered_set<std::uint32_t> keepIds{};
		keepIds.insert(speaker->GetFormID());
		for (auto* actor : acceptedCrowd) {
			const auto id = ActorFormID(actor);
			if (id != 0) {
				keepIds.insert(id);
			}
		}
		for (const auto id : g_bleedCrowdAssigned) {
			if (id != 0) {
				keepIds.insert(id);
			}
		}

		std::vector<RE::Actor*> releaseActors;
		auto activeActors = TFD::HostilityController::CollectActiveTruceActors(speaker);
		for (auto* actor : activeActors) {
			const auto id = ActorFormID(actor);
			if (id == 0 || keepIds.find(id) != keepIds.end()) {
				continue;
			}
			releaseActors.push_back(actor);
		}

		const auto released = TFD::HostilityController::ReleaseDialogueTruceActors(
			speaker,
			releaseActors,
			TFD::HostilityController::ReleaseReason::FlowHandoff);
		if (released > 0 || !releaseActors.empty()) {
			spdlog::info(
				"[TFD][Bleedout][CB07] truce session restricted speaker={:08X} keep={} candidates={} released={} reason={}",
				speaker->GetFormID(),
				static_cast<unsigned int>(keepIds.size()),
				static_cast<unsigned int>(releaseActors.size()),
				static_cast<unsigned int>(released),
				reason ? reason : "unknown");
		}
		return released;
	}

	std::size_t HardStopActorCombatAndAlarm(RE::Actor* actor, const char* reason)
	{
		if (!actor || actor->IsDead() || actor->IsDisabled()) {
			return 0;
		}

		auto* process = RE::ProcessLists::GetSingleton();
		bool oldRunDetection = false;
		bool changedRunDetection = false;
		if (process) {
			oldRunDetection = process->runDetection;
			process->runDetection = false;
			process->ClearCachedFactionFightReactions();
			process->StopCombatAndAlarmOnActor(actor, false);
			changedRunDetection = true;
		}

		actor->StopAlarmOnActor();
		actor->StopCombat();
		// R250A: do not force weapon stance here.  If pacify / combat alarm
		// cleanup is correct, Skyrim will sheathe naturally.  Forcing weapon state
		// can break the animation graph (idle posture with weapon still in hand).
		actor->EvaluatePackage(false, true);
		actor->EvaluatePackage(true, true);

		if (process && changedRunDetection) {
			process->runDetection = oldRunDetection;
		}

		spdlog::info("[TFD][Bleedout][R107] hard stop combat/alarm actor={:08X} reason={}",
			actor->GetFormID(),
			reason ? reason : "unknown");
		return 1;
	}

	void HardStopBleedoutCombatPack(RE::Actor* player, RE::Actor* speaker, const std::vector<RE::Actor*>& crowd, const char* reason, bool scheduleWaves)
	{
		const char* why = reason ? reason : "bleedout_hard_stop";
		std::unordered_set<std::uint32_t> seen{};
		std::size_t directCount = 0;
		auto stopUnique = [&](RE::Actor* actor) {
			if (!actor) {
				return;
			}
			const auto id = actor->GetFormID();
			if (id == 0 || seen.find(id) != seen.end()) {
				return;
			}
			seen.insert(id);
			directCount += HardStopActorCombatAndAlarm(actor, why);
		};

		stopUnique(player);
		stopUnique(speaker);
		for (auto* actor : crowd) {
			stopUnique(actor);
		}

		// CB07: do not run a broad 12000 StopCombat sweep here. The crowd collector may
		// scan up to 12000 for awareness, but only the player, selected speaker, and
		// accepted engaged crowd are allowed to be pacified. A broad sweep can freeze
		// distant scripted actors that were never part of this bleedout encounter.
		const float scanCeiling = (std::max)(12000.0f, TFD::Settings::GetSweepRadius());
		const bool broadSweep = false;
		(void)scheduleWaves;

		spdlog::info("[TFD][Bleedout][CB07] hard stop pack direct={} crowd={} scanCeiling={:.0f} broadSweep={} waves=0 reason={}",
			static_cast<unsigned int>(directCount),
			static_cast<unsigned int>(crowd.size()),
			scanCeiling,
			broadSweep ? 1 : 0,
			why);
	}

	bool IsReasonableSpeaker(RE::Actor* actor, float maxDist, float* outDistance, const SpeakerLogicHandlers& handlers)
	{
		auto* player = handlers.getPlayer ? handlers.getPlayer() : nullptr;
		return IsReasonableSpeakerImpl(actor, player, maxDist, outDistance, handlers);
	}

	RE::Actor* ChooseStrictSpeaker(float radius, float maxDist, RE::Actor* preferred, const SpeakerLogicHandlers& handlers)
	{
		auto* player = handlers.getPlayer ? handlers.getPlayer() : nullptr;
		if (!player) {
			return nullptr;
		}

		auto isStrongPreferred = [&](RE::Actor* actor, float& outDist) -> bool {
			if (!IsReasonableSpeakerImpl(actor, player, maxDist, &outDist, handlers)) {
				return false;
			}
			auto* currentTarget = handlers.resolveCurrentCombatTarget ? handlers.resolveCurrentCombatTarget(actor) : nullptr;
			if (currentTarget == player) {
				return true;
			}
			if (currentTarget && handlers.isActiveFollowerActor && handlers.isActiveFollowerActor(currentTarget)) {
				return true;
			}
			if (actor->IsInCombat() && actor->IsHostileToActor(player) && handlers.isActorCloseAndFront && handlers.isActorCloseAndFront(actor, player, 384.0f)) {
				return true;
			}
			return false;
		};

		float preferredDist = 99999.0f;
		if (preferred && isStrongPreferred(preferred, preferredDist)) {
			return preferred;
		}

		auto snapshot = BuildCoalitionSnapshot(radius, handlers);
		auto validator = [&](RE::Actor* actor, float& outDist) {
			return isStrongPreferred(actor, outDist);
		};
		if (auto* coalitionSpeaker = ResolveCoalitionSpeakerCandidate(snapshot, preferred, validator)) {
			return coalitionSpeaker;
		}

		RE::Actor* best = nullptr;
		float bestScore = std::numeric_limits<float>::max();
		for (const auto& info : snapshot.actors) {
			auto* actor = info.get();
			float dist = 99999.0f;
			if (!IsReasonableSpeakerImpl(actor, player, maxDist, &dist, handlers)) {
				continue;
			}

			auto* currentTarget = handlers.resolveCurrentCombatTarget ? handlers.resolveCurrentCombatTarget(actor) : nullptr;
			const bool targetsPlayer = currentTarget == player;
			const bool targetsFollower = currentTarget && handlers.isActiveFollowerActor && handlers.isActiveFollowerActor(currentTarget);
			const bool hostile = info.hostileToPlayer || actor->IsHostileToActor(player);
			const bool inCombat = info.inCombat || actor->IsInCombat();
			const bool front = handlers.isActorCloseAndFront && handlers.isActorCloseAndFront(actor, player, 448.0f);
			if (!hostile && !inCombat && !targetsPlayer && !targetsFollower) {
				continue;
			}

			float score = dist;
			if (targetsPlayer) score -= 900.0f;
			if (targetsFollower) score -= 650.0f;
			if (hostile) score -= 260.0f;
			if (inCombat) score -= 180.0f;
			if (front) score -= 220.0f;
			if (actor == preferred) score -= 120.0f;
			if (score < bestScore) {
				bestScore = score;
				best = actor;
			}
		}

		if (best) {
			return best;
		}

		float anyPreferredDist = 99999.0f;
		if (preferred && IsReasonableSpeakerImpl(preferred, player, maxDist, &anyPreferredDist, handlers)) {
			return preferred;
		}

		return nullptr;
	}

	RE::Actor* FindBestSpeaker(float radius, float maxDist, RE::Actor* preferred, const SpeakerLogicHandlers& handlers)
	{
		if (auto* best = ChooseStrictSpeaker(radius, maxDist, preferred, handlers)) {
			return best;
		}
		auto* player = handlers.getPlayer ? handlers.getPlayer() : nullptr;
		if (!player) {
			return nullptr;
		}
		float preferredDist = 99999.0f;
		if (preferred && IsReasonableSpeakerImpl(preferred, player, maxDist, &preferredDist, handlers)) {
			return preferred;
		}
		return nullptr;
	}

	bool CanUseSpeakerForGreet(RE::Actor* aggressor, float maxDist, float& outDistance, const SpeakerLogicHandlers& handlers)
	{
		outDistance = 99999.0f;
		auto* player = handlers.getPlayer ? handlers.getPlayer() : nullptr;
		if (!player || !aggressor) {
			return false;
		}
		if (!handlers.isStandingEnemyThresholdActor || !handlers.isStandingEnemyThresholdActor(aggressor) || !aggressor->Is3DLoaded()) {
			return false;
		}
		if (!handlers.isBleedSpaceCompatible || !handlers.isBleedSpaceCompatible(aggressor, player)) {
			return false;
		}
		outDistance = Distance3D(aggressor, player);
		return outDistance <= maxDist;
	}

	bool BeginDialogueHotkey(float radius, float maxSpeakerDist, const DialogueHotkeyHandlers& handlers)
	{
		if (!handlers.isBleedoutActive || !handlers.isBleedoutActive()) {
			return false;
		}
		if (handlers.isCaptiveEscapePhase && handlers.isCaptiveEscapePhase()) {
			const bool allowCaptiveBleedoutRetry = IsCaptiveEscapeBleedoutRetryAllowed();
			if (!allowCaptiveBleedoutRetry) {
				spdlog::info("[TFD][Bleedout][P32S] bleed hotkey blocked by active captive escape phase");
				return false;
			}
			spdlog::info("[TFD][Bleedout][P32S] bleed hotkey allowed during captive escape bleedout retry");
		}
		if (handlers.isDialogueOpen && handlers.isDialogueOpen()) {
			return false;
		}

		auto* player = handlers.speaker.getPlayer ? handlers.speaker.getPlayer() : nullptr;
		if (!player) {
			return false;
		}

		RE::Actor* aggressor = handlers.resolveSpeakerFromRuntime ? handlers.resolveSpeakerFromRuntime() : nullptr;
		if (!aggressor && handlers.speaker.resolveLastAggressor) {
			aggressor = handlers.speaker.resolveLastAggressor();
		}
		if (!aggressor && handlers.resolveAggressor) {
			aggressor = handlers.resolveAggressor();
		}
		if (!aggressor && handlers.findBestAggressor) {
			aggressor = handlers.findBestAggressor(radius);
		}
		aggressor = FindBestHotkeySpeakerImpl(radius, maxSpeakerDist, aggressor, handlers.speaker);
		if (!aggressor) {
			spdlog::info("[TFD][Bleedout] bleed hotkey -> no valid speaker");
			return false;
		}

		if (handlers.releaseNoSpeakerTameSession) {
			handlers.releaseNoSpeakerTameSession("bleed_hotkey_begin");
		}
		if (handlers.releaseTruceSession) {
			handlers.releaseTruceSession();
			handlers.releaseTruceSession();
		}
		if (!handlers.startTruceSessionForSpeaker || !handlers.startTruceSessionForSpeaker(player, aggressor, "bleed_hotkey")) {
			spdlog::warn("[TFD][Bleedout] bleed hotkey truce session failed speaker={:08X}", aggressor->GetFormID());
			return false;
		}
		(void)ReleaseUnacceptedBleedoutTruceActors(aggressor, {}, "bleed_hotkey_restrict_truce");
		if (handlers.resetSpeakerKick) {
			handlers.resetSpeakerKick();
		}
		if (handlers.resetGreetRuntime) {
			handlers.resetGreetRuntime("bleed_hotkey");
		}
		if (handlers.beginGreet) {
			handlers.beginGreet(aggressor, "bleed_hotkey");
		}
		return true;
	}

	void ResetRuntimeState(bool preserveCaptive, const char* reason, const RuntimeResetHandlers& handlers)
	{
		const char* why = reason ? reason : "reset_bleed_runtime";
		ClearBleedoutDialogueFactionsInternal(why);
		if (handlers.releasePlayerBleedLock) {
			handlers.releasePlayerBleedLock(why);
		}
		if (handlers.releaseBleedTruceSession) {
			handlers.releaseBleedTruceSession();
		}
		if (handlers.releaseNoSpeakerTameSession) {
			handlers.releaseNoSpeakerTameSession(why);
		}
		if (handlers.clearBridgeAliases) {
			handlers.clearBridgeAliases(why);
		}
		if (handlers.setBleedActive) {
			handlers.setBleedActive(false, why);
		}
		if (handlers.resetGreetRuntime) {
			handlers.resetGreetRuntime("bleed_reset");
		}
		if (handlers.resetSystemEventState) {
			handlers.resetSystemEventState(why);
		}
		if (handlers.clearCaptorAliases) {
			handlers.clearCaptorAliases(why);
		}
		if (handlers.resetDialogueRuntimeState) {
			handlers.resetDialogueRuntimeState();
		}
		if (handlers.resetBattleObserveState) {
			handlers.resetBattleObserveState();
		}
		if (handlers.clearEscapeBreakState) {
			handlers.clearEscapeBreakState();
		}
		if (handlers.clearLastEnemyTargetingPlayer) {
			handlers.clearLastEnemyTargetingPlayer();
		}
		if (handlers.clearOutcomeWindow) {
			handlers.clearOutcomeWindow(why);
		}
		if (!preserveCaptive && handlers.resetPleasureRuntime) {
			handlers.resetPleasureRuntime(why);
		}
		spdlog::info("[TFD][Bleedout] runtime reset preserveCaptive={} reason={}",
			preserveCaptive ? 1 : 0,
			why);
	}

	void TransitionRuntimeToPleasureCommit(const char* reason, std::uint32_t speakerId, bool preserveSession, std::uint32_t captorId, const RuntimePleasureCommitHandlers& handlers)
	{
		const char* why = reason ? reason : "bleed_pleasure_commit";

		// R112: Bleedout Pleasure is a terminal handoff.  Logs showed the
		// pending Bleedout forcegreet opener survived this commit with
		// attempts=0/requestIssued=0, then opened TFDDialogueBleedoutGreet
		// during the OStim/AfterPleasure lifecycle.  That stale Bleedout
		// dialogue poisoned the next AfterPleasure menu and caused the root
		// topic to flash closed without a terminal choice.  Cancel only the
		// still-pending Bleedout opener here; do not close an already-open
		// DialogueMenu and do not touch AfterPleasure/InCombat/PreCombat
		// openers.
		CancelPendingBleedoutDialogueOpen(why);

		// R248A: the Pleasure runtime owns the source metadata after commit.
		// Do not preserve the Bleedout forcegreet/dialogue owner just because a
		// truce/suppress session existed.  Preserving it allowed initial handoff
		// retry and flow-greet-confirm callbacks to reopen/confirm Bleedout after
		// the player had already committed Pleasure, keeping HUD/conditions in
		// Defeat and leaking passive suppression into the OStim handoff.
		if (handlers.releaseNoSpeakerTameSession) {
			handlers.releaseNoSpeakerTameSession(why);
		}
		if (handlers.clearBridgeAliases) {
			handlers.clearBridgeAliases(why);
		}
		if (handlers.setBleedActive) {
			handlers.setBleedActive(false, why);
		}
		if (handlers.resetGreetRuntime) {
			handlers.resetGreetRuntime("bleed_pleasure_commit");
		}
		if (handlers.resetDialogueRuntimeState) {
			handlers.resetDialogueRuntimeState();
		}
		if (handlers.resetBattleObserveState) {
			handlers.resetBattleObserveState();
		}
		if (handlers.clearEscapeBreakState) {
			handlers.clearEscapeBreakState();
		}
		if (handlers.clearLastEnemyTargetingPlayer) {
			handlers.clearLastEnemyTargetingPlayer();
		}
		spdlog::info("[TFD][Bleedout][R248A] bleedout dialogue owner cleared during pleasure commit speaker={:08X} captor={:08X} preservedSourceMetadata=1 reason={}",
			speakerId,
			captorId,
			why);
		if (handlers.clearOutcomeWindow) {
			handlers.clearOutcomeWindow(why);
		}
		spdlog::info("[TFD][Bleedout] runtime transitioned to pleasure commit reason={} preserveSpeaker={:08X} preserveSession={} preserveCaptor={:08X}",
			why,
			speakerId,
			preserveSession ? 1 : 0,
			captorId);
	}

	void Install()
	{
		spdlog::info("[TFD][Bleedout] Install");
	}

	void ResetForLoad()
	{
		ClearBleedoutDialogueFactionsInternal("reset_for_load");
		ClearTerminalCommit("reset_for_load");
		g_bleedoutQuestRegistry.resolved = false;
		g_bleedoutQuestRegistry.quest = nullptr;
		g_bleedoutQuestRegistry.captorAliases.fill(nullptr);
		g_activeCaptorFormID = 0;
		g_captorFactionHandle.reset();
		g_captorBindLast = {};
		spdlog::info("[TFD][Bleedout] ResetForLoad");
	}

	std::atomic_bool& InBleedStateRef() { return g_inBleedState; }
	float& MinHpRef() { return g_minHp; }
	std::chrono::steady_clock::time_point& BleedStartRef() { return g_bleedStart; }
	int& BleedLastSecondsRef() { return g_bleedLastSeconds; }
	bool& BleedPausedRef() { return g_bleedPaused; }
	std::chrono::steady_clock::time_point& BleedPauseStartedRef() { return g_bleedPauseStarted; }
	std::chrono::steady_clock::time_point& BleedLastCalmPulseRef() { return g_bleedLastCalmPulse; }
	std::chrono::steady_clock::time_point& BleedLastCrowdAssignRef() { return g_bleedLastCrowdAssign; }
	std::vector<std::uint32_t>& BleedCrowdAssignedRef() { return g_bleedCrowdAssigned; }
	std::chrono::steady_clock::time_point& BleedNoSpeakerTameLastAttemptRef() { return g_bleedNoSpeakerTameLastAttempt; }
	std::uint32_t& BleedSpeakerIDRef() { return g_bleedSpeakerId; }
	std::chrono::steady_clock::time_point& BleedSpeakerKickLastRef() { return g_bleedSpeakerKickLast; }
	int& BleedSpeakerKickCountRef() { return g_bleedSpeakerKickCount; }
	int& BleedDialogueRetryCountRef() { return g_bleedDialogueRetryCount; }
	std::unordered_set<std::uint32_t>& BleedRejectedSpeakerIdsRef() { return g_bleedRejectedSpeakerIds; }
	bool& BleedPendingCaptiveOutcomeRef() { return g_bleedPendingCaptiveOutcome; }
	bool& BleedPendingNonCaptiveOutcomeRef() { return g_bleedPendingNonCaptiveOutcome; }
	bool& BleedBattleObservePendingRef() { return g_bleedBattleObservePending; }
	std::chrono::steady_clock::time_point& BleedBattleObservePendingUntilRef() { return g_bleedBattleObservePendingUntil; }
	std::chrono::steady_clock::time_point& BleedBattleObservePendingLastRedirectRef() { return g_bleedBattleObservePendingLastRedirect; }
	int& BleedBattleObservePendingEmptyEnemyTicksRef() { return g_bleedBattleObservePendingEmptyEnemyTicks; }
	int& BleedBattleObservePendingEmptyAllyTicksRef() { return g_bleedBattleObservePendingEmptyAllyTicks; }
	bool& BleedBattleObserveActiveRef() { return g_bleedBattleObserveActive; }
	std::chrono::steady_clock::time_point& BleedBattleObserveSinceRef() { return g_bleedBattleObserveSince; }
	std::chrono::steady_clock::time_point& BleedBattleObserveLastRedirectRef() { return g_bleedBattleObserveLastRedirect; }
	int& BleedBattleObserveActiveEmptyEnemyTicksRef() { return g_bleedBattleObserveActiveEmptyEnemyTicks; }
	int& BleedBattleObserveActiveEmptyAllyTicksRef() { return g_bleedBattleObserveActiveEmptyAllyTicks; }

	void ClearBridgeAliases(RE::TESForm* sender, const char* reason)
	{
		const char* why = reason ? reason : "unknown";
		const bool setupClear = IsBleedoutSetupClearReason(reason);
		if (!setupClear) {
			ClearBleedoutDialogueFactionsInternal(why);
		}
		else {
			spdlog::info("[TFD][Bleedout][R114] dialogue faction clear skipped during setup/handoff reason={}", why);
		}

		const bool bleedQueued = SendBridgeModEvent("TFDBleedoutClearAll", sender, why);
		const bool truceQueued = setupClear ? false : SendBridgeModEvent("TFDTruceClearAll", sender, why);
		spdlog::info("[TFD][BleedBridge][P14OWN] ClearAll reason={} bleedQueued={} truceQueued={} owner=bleedout setupClear={}",
			why,
			bleedQueued ? 1 : 0,
			truceQueued ? 1 : 0,
			setupClear ? 1 : 0);
	}


	void ApplyBleedoutDialogueFactions(RE::Actor* actor, const char* role, const char* reason)
	{
		ApplyBleedoutDialogueFactionsInternal(actor, role ? role : "actor", reason ? reason : "external_apply");
	}

	void ClearBleedoutDialogueFactions(const char* reason)
	{
		ClearBleedoutDialogueFactionsInternal(reason ? reason : "external_clear");
	}

	void ClearBleedoutDialogueFactionForActor(RE::Actor* actor, const char* reason)
	{
		ClearBleedoutDialogueFactionForActorInternal(actor, reason ? reason : "external_clear_actor");
	}

	void AssignBridgeActor(RE::Actor* actor)
	{
		if (!actor) {
			return;
		}

		ApplyBleedoutDialogueFactionsInternal(actor, "speaker", "bleedout_assign");
		const bool queued = SendBridgeModEvent("TFDBleedoutAssign", actor);
		spdlog::info("[TFD][BleedBridge] Assign actor={:08X} queued={}", actor->GetFormID(), queued);
	}

	void PrimeBridgeActor(RE::Actor* actor, const char* reason)
	{
		if (!actor) {
			return;
		}

		ApplyBleedoutDialogueFactionsInternal(actor, "speaker", reason ? reason : "bleedout_prime_speaker");
		const bool queued = SendBridgeModEvent(kPrimeSpeakerEvent, actor);
		spdlog::info("[TFD][BleedBridge][P14OWN] Prime speaker actor={:08X} trucePromote=0 queued={} reason={}",
			actor->GetFormID(),
			queued ? 1 : 0,
			reason ? reason : "unknown");
	}

	void PrimeBridgeCrowdActor(RE::Actor* actor, const char* reason)
	{
		if (!actor) {
			return;
		}

		ApplyBleedoutDialogueFactionsInternal(actor, "crowd", reason ? reason : "bleedout_prime_crowd");
		const bool truceQueued = SendBridgeModEvent("TFDTruceAssign", actor, "bleedout_crowd");
		spdlog::info("[TFD][BleedBridge][P14OWN] Prime crowd actor={:08X} truceAssign={} reason={}",
			actor->GetFormID(),
			truceQueued ? 1 : 0,
			reason ? reason : "unknown");
	}


	bool ActorHasAllowListFaction(RE::Actor* actor)
	{
		if (!actor) {
			return false;
		}
		auto* allowList = ResolveBleedoutAllowList();
		if (!allowList) {
			return false;
		}
		for (auto* form : allowList->forms) {
			auto* faction = form ? form->As<RE::TESFaction>() : nullptr;
			if (faction && actor->IsInFaction(faction)) {
				return true;
			}
		}
		return false;
	}

	void ClearCaptorAliases(const char* reason)
	{
		std::vector<RE::Actor*> staleActors{};
		ResolveBleedoutQuestRegistry();
		if (g_bleedoutQuestRegistry.quest) {
			for (auto* alias : g_bleedoutQuestRegistry.captorAliases) {
				if (!alias) {
					continue;
				}
				if (auto* current = alias->GetActorReference()) {
					const auto currentId = current->GetFormID();
					bool seen = false;
					for (auto* existing : staleActors) {
						if (existing && existing->GetFormID() == currentId) {
							seen = true;
							break;
						}
					}
					if (!seen) {
						staleActors.push_back(current);
					}
				}
				WriteQuestRefAlias(g_bleedoutQuestRegistry.quest, alias, nullptr);
			}
		}

		if (g_captorFactionHandle) {
			auto tracked = g_captorFactionHandle.get().get();
			if (tracked) {
				const auto trackedId = tracked->GetFormID();
				bool seen = false;
				for (auto* existing : staleActors) {
					if (existing && existing->GetFormID() == trackedId) {
						seen = true;
						break;
					}
				}
				if (!seen) {
					staleActors.push_back(tracked);
				}
			}
		}

		for (auto* stale : staleActors) {
			RemoveBleedoutCaptorFaction(stale, reason ? reason : "unknown");
		}
		g_captorFactionHandle.reset();
		g_activeCaptorFormID = 0;
		g_captorBindLast = {};
		spdlog::info("[TFD][BleedQuest] captor aliases cleared reason={}", reason ? reason : "unknown");
	}

	bool IsCaptorAliasPrimary(RE::Actor* actor)
	{
		ResolveBleedoutQuestRegistry();
		if (!actor || !g_bleedoutQuestRegistry.quest || g_bleedoutQuestRegistry.captorAliases.empty()) {
			return false;
		}
		auto* primaryAlias = g_bleedoutQuestRegistry.captorAliases[0];
		if (!primaryAlias) {
			return false;
		}
		auto* primaryRef = primaryAlias->GetActorReference();
		return primaryRef && primaryRef == actor;
	}

	bool BindCaptorAliases(RE::Actor* actor, const char* reason)
	{
		if (!actor || actor->IsDead() || actor->IsDisabled()) {
			return false;
		}
		ResolveBleedoutQuestRegistry();
		if (!g_bleedoutQuestRegistry.quest) {
			return false;
		}

		std::vector<RE::Actor*> staleActors{};
		std::size_t primarySlot = 0;
		bool bound = false;
		bool hasNativeAlias = false;
		for (auto* alias : g_bleedoutQuestRegistry.captorAliases) {
			if (alias) {
				hasNativeAlias = true;
				break;
			}
		}
		if (!hasNativeAlias) {
			ApplyBleedoutCaptorFaction(actor, reason ? reason : "no_native_alias_fallback");
			g_activeCaptorFormID = actor->GetFormID();
			g_captorBindLast = Clock::now();
			spdlog::info("[TFD][BleedQuest] captor bind fallback actor={:08X} bound=1 reason={} noNativeAlias=1",
				actor->GetFormID(),
				reason ? reason : "unknown");
			return true;
		}

		for (std::size_t i = 0; i < g_bleedoutQuestRegistry.captorAliases.size(); ++i) {
			auto* alias = g_bleedoutQuestRegistry.captorAliases[i];
			if (!alias) {
				continue;
			}

			auto* current = alias->GetActorReference();
			if (current && current != actor) {
				const auto currentId = current->GetFormID();
				bool seen = false;
				for (auto* existing : staleActors) {
					if (existing && existing->GetFormID() == currentId) {
						seen = true;
						break;
					}
				}
				if (!seen) {
					staleActors.push_back(current);
				}
			}

			if (!bound) {
				WriteQuestRefAlias(g_bleedoutQuestRegistry.quest, alias, actor);
				auto* boundActor = alias->GetActorReference();
				if (boundActor && boundActor == actor) {
					bound = true;
					primarySlot = i + 1;
					continue;
				}
			}

			WriteQuestRefAlias(g_bleedoutQuestRegistry.quest, alias, nullptr);
		}

		for (auto* stale : staleActors) {
			RemoveBleedoutCaptorFaction(stale, "rebind_stale");
		}

		if (bound) {
			ApplyBleedoutCaptorFaction(actor, reason ? reason : "unknown");
			g_activeCaptorFormID = actor->GetFormID();
		}
		else {
			RemoveBleedoutCaptorFaction(actor, "bind_failed");
			g_activeCaptorFormID = 0;
		}
		g_captorBindLast = Clock::now();
		spdlog::info("[TFD][BleedQuest] captor bind actor={:08X} primarySlot={} bound={} reason={}",
			actor->GetFormID(),
			primarySlot,
			bound ? 1 : 0,
			reason ? reason : "unknown");
		return bound;
	}

	std::uint32_t GetActiveCaptorFormID()
	{
		return g_activeCaptorFormID;
	}

	bool WasCaptorRecentlyBound(std::chrono::steady_clock::time_point now, std::chrono::milliseconds window)
	{
		return g_captorBindLast.time_since_epoch().count() != 0 &&
			(now - g_captorBindLast) < window;
	}

	bool ShouldUseBleedoutScopedSupportClear(const char* reason)
	{
		if (!reason || !*reason) {
			return false;
		}

		std::string_view why{ reason };
		if (why.find("bleed") != std::string_view::npos) {
			return true;
		}
		if (why.find("post_dialogue_system_event") != std::string_view::npos) {
			return true;
		}
		if (why.find("dialogue_closed_pay_release") != std::string_view::npos) {
			return true;
		}
		if (why.find("pay_release") != std::string_view::npos) {
			return true;
		}
		if (why == "reset_bleed_runtime") {
			return true;
		}
		return false;
	}

	void ClearSupportBridgeAliases(const char* reason, const SupportBridgeHandlers& handlers)
	{
		const bool bleedoutScoped = ShouldUseBleedoutScopedSupportClear(reason);
		bool preCombatQueued = false;
		bool truceQueued = false;
		bool inCombatQueued = false;
		bool preCombatCancelled = false;

		if (!bleedoutScoped) {
			preCombatQueued = handlers.queuePreCombatClearAll ? handlers.queuePreCombatClearAll() : false;
			truceQueued = handlers.queueTruceClearAll ? handlers.queueTruceClearAll() : false;
			inCombatQueued = handlers.queueInCombatClearAll ? handlers.queueInCombatClearAll() : false;
			if (handlers.cancelAllPreCombat) {
				handlers.cancelAllPreCombat();
				preCombatCancelled = true;
			}
		}

		spdlog::info("[TFD][BleedBridge][P18OWN] ClearSupport reason={} bleedoutScoped={} preCombatQueued={} truceQueued={} inCombatQueued={} preCombatCancel={}",
			reason ? reason : "unknown",
			bleedoutScoped ? 1 : 0,
			preCombatQueued ? 1 : 0,
			truceQueued ? 1 : 0,
			inCombatQueued ? 1 : 0,
			preCombatCancelled ? 1 : 0);
	}

	bool StartTruceSessionForSpeaker(RE::Actor* player, RE::Actor* speaker, const char* reason, RuntimeHostStateRefs state, const RuntimeHostHandlers& handlers)
	{
		if (!player || !speaker || speaker->IsDead() || speaker->IsDisabled()) {
			return false;
		}
		const auto speakerID = speaker->GetFormID();
		const bool sameActiveCaptor = GetActiveCaptorFormID() != 0 && GetActiveCaptorFormID() == speakerID;
		const bool setupClearReason = IsBleedoutSetupClearReason(reason);
		if (handlers.clearBleedSupportBridgeAliases && !setupClearReason) {
			handlers.clearBleedSupportBridgeAliases(reason ? reason : "bleed_retry");
		}
		else if (handlers.clearBleedSupportBridgeAliases && setupClearReason) {
			spdlog::info("[TFD][BleedBridge][R101] ClearSupport skipped during bleedout setup reason={} speaker={:08X}",
				reason ? reason : "unknown",
				speakerID);
		}
		if (state.bleedSpeakerId) {
			*state.bleedSpeakerId = speakerID;
		}
		if (handlers.setLastAggressor) {
			handlers.setLastAggressor(speaker);
		}
		if (!sameActiveCaptor) {
			ClearBridgeAliases(speaker, reason ? reason : "bleed_retry");
		}
		else {
			spdlog::info("[TFD][BleedBridge] ClearAll skipped reason={} actor={:08X} sameActiveCaptor=1",
				reason ? reason : "bleed_retry",
				speakerID);
		}
		AssignBridgeActor(speaker);
		BindCaptorAliases(speaker, reason ? reason : "bleed_retry");
		PrimeBridgeActor(speaker, reason ? reason : "bleed_retry");
		if (!speaker->IsAIEnabled()) {
			speaker->EnableAI(true);
		}
		speaker->AllowPCDialogue(true);
		speaker->SetDialogueWithPlayer(false, false, nullptr);
		ApplyBleedoutDialogueFactionsInternal(speaker, "speaker", reason ? reason : "bleed_truce_session_speaker");
		HardStopActorCombatAndAlarm(speaker, reason ? reason : "bleed_truce_session_speaker");
		// P14OWN: Bleedout owns passive suppression directly. Do not borrow
		// TruceInCombat here; that leaks truce_spent, ignoreSpent retry,
		// TFDInCombatClear/Assign, and generic InCombat release semantics into
		// Bleedout.
		auto sessionId = TFD::HostilityController::BeginBleedoutSuppressSession(
			player,
			speaker,
			0.0,
			0.0,
			false,
			0.0f,
			reason ? reason : "bleed_speaker_suppress");
		if (!sessionId.has_value()) {
			spdlog::warn("[TFD][Bleedout][P14OWN] speaker suppress rejected actor={:08X} reason={}",
				speaker->GetFormID(),
				reason ? reason : "unknown");
			return false;
		}
		g_truceSessionId = *sessionId;
		spdlog::info(
			"[TFD][Bleedout][P14OWN] bleedout suppress armed actor={:08X} session={} reason={}",
			speaker->GetFormID(),
			g_truceSessionId,
			reason ? reason : "unknown");
		ApplyDialogueOverdrive(player, speaker, reason ? reason : "bleed_retry", false, state, handlers);
		speaker->EvaluatePackage(false, true);
		speaker->EvaluatePackage(true, true);
		spdlog::info("[TFD][BleedoutDialogueRuntime][R24] bleed speaker restart id={} actor={:08X} reason={}",
			g_truceSessionId,
			speaker->GetFormID(),
			reason ? reason : "unknown");
		return true;
	}

	void ApplyDialogueOverdrive(RE::Actor* player, RE::Actor* speaker, const char* reason, bool restartDialogue, RuntimeHostStateRefs state, const RuntimeHostHandlers& handlers)
	{
		if (!player || !speaker || speaker->IsDead() || speaker->IsDisabled()) {
			return;
		}

		if (!speaker->IsAIEnabled()) {
			speaker->EnableAI(true);
		}
		speaker->AllowPCDialogue(true);
		speaker->SetDialogueWithPlayer(false, false, nullptr);

		HardStopActorCombatAndAlarm(player, reason ? reason : "bleed_dialogue_overdrive_player");
		HardStopActorCombatAndAlarm(speaker, reason ? reason : "bleed_dialogue_overdrive_speaker");

		if (!IsCaptorAliasPrimary(speaker)) {
			BindCaptorAliases(speaker, reason ? reason : "bleed_dialogue_overdrive");
		}
		PrimeBridgeActor(speaker, reason ? reason : "bleed_dialogue_overdrive");

		const auto preservedCrowdSize = state.bleedCrowdAssigned ? state.bleedCrowdAssigned->size() : 0u;
		if (state.bleedLastCrowdAssign && preservedCrowdSize > 0u) {
			*state.bleedLastCrowdAssign = Clock::now();
		}

		std::optional<RE::FormID> burstId;
		const bool preserveDialogueSession = state.inBleedState && state.inBleedState->load(std::memory_order_relaxed);
		if (!preserveDialogueSession) {
			burstId = TFD::HostilityController::BeginBleedoutSuppressBurst(
				player,
				speaker,
				0.0,
				2.5,
				12000.0f,
				reason ? reason : "bleed_dialogue_overdrive");
		}
		else {
			spdlog::info("[TFD][BleedoutDialogueRuntime][R24] bleed dialogue overdrive preserve dialogue session speaker={:08X} reason={}",
				speaker->GetFormID(),
				reason ? reason : "unknown");
		}

		player->DrawWeaponMagicHands(false);
		player->EvaluatePackage(false, true);
		player->EvaluatePackage(true, true);
		speaker->EvaluatePackage(false, true);
		speaker->EvaluatePackage(true, true);

		if (restartDialogue && handlers.resetGreetRuntime) {
			handlers.resetGreetRuntime(reason ? reason : "bleed_dialogue_overdrive");
		}
		if (restartDialogue) {
			TFD::BleedoutGreet::Begin(speaker, reason ? reason : "bleed_dialogue_overdrive");
		}

		spdlog::info("[TFD][BleedoutDialogueRuntime][R24] bleed dialogue overdrive speaker={:08X} crowdSize={} burst={} restartDialogue={} reason={}",
			speaker->GetFormID(),
			static_cast<unsigned int>(preservedCrowdSize),
			burstId.has_value() ? 1 : 0,
			restartDialogue ? 1 : 0,
			reason ? reason : "unknown");
	}

	void ReleaseTruceSession(TFD::Tame::ReleaseReason reason, std::uint32_t* bleedSpeakerId)
	{
		ClearBleedoutDialogueFactionsInternal(TFD::HostilityController::ToString(reason));
		if (g_truceSessionId != 0) {
			const bool released = TFD::HostilityController::ReleaseBleedoutSuppressSession(
				g_truceSessionId,
				reason,
				"bleedout_release_suppress_session");
			spdlog::info("[TFD][Bleedout][P14OWN] bleed suppress session released id={} released={} reason={}",
				g_truceSessionId,
				released ? 1 : 0,
				TFD::HostilityController::ToString(reason));
			g_truceSessionId = 0;
		}
		g_bleedSpeakerId = 0;
		ResetBleedSpeakerKick();
		if (bleedSpeakerId) {
			*bleedSpeakerId = 0;
		}
	}

	void ReleaseNoSpeakerTameSession(const char* reason)
	{
		if (g_noSpeakerTameSessionId != 0) {
			const bool released = TFD::HostilityController::ReleaseBleedoutSuppressSession(
				g_noSpeakerTameSessionId,
				TFD::HostilityController::ReleaseReason::Generic,
				reason ? reason : "bleed_no_speaker_suppress_release");
			spdlog::info("[TFD][Bleedout][P14OWN] bleed no-speaker suppress released id={} primary={:08X} released={} reason={}",
				g_noSpeakerTameSessionId,
				g_noSpeakerTamePrimaryId,
				released ? 1 : 0,
				reason ? reason : "unknown");
			g_noSpeakerTameSessionId = 0;
		}
		g_noSpeakerTamePrimaryId = 0;
		g_bleedNoSpeakerTameLastAttempt = {};
	}

	bool TryEnsureNoSpeakerTameSession(const std::vector<RE::Actor*>& actors, RE::Actor* player, const char* reason, const RuntimeHostHandlers& handlers)
	{
		if (!player) {
			return false;
		}
		bool hasAny = false;
		for (auto* actor : actors) {
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				continue;
			}
			hasAny = true;
			if (handlers.isCaptiveSupportedAggressor && handlers.isCaptiveSupportedAggressor(actor)) {
				return false;
			}
		}
		if (!hasAny) {
			return false;
		}
		auto* primary = ChooseNoSpeakerTamePrimary(actors, player, handlers);
		if (!primary) {
			return false;
		}
		if (g_noSpeakerTamePrimaryId == primary->GetFormID() &&
			TFD::HostilityController::IsBleedoutSuppressed(primary)) {
			return true;
		}
		if (g_noSpeakerTameSessionId != 0) {
			ReleaseNoSpeakerTameSession("restart");
		}
		player->DrawWeaponMagicHands(false);
		auto sessionId = TFD::HostilityController::BeginBleedoutSuppressSession(
			player,
			primary,
			0.0,
			0.0,
			false,
			0.0f,
			reason ? reason : "bleed_no_speaker_suppress");
		if (!sessionId.has_value()) {
			spdlog::info("[TFD][Bleedout][P14OWN] bleed no-speaker suppress rejected primary={:08X} reason={}",
				primary->GetFormID(),
				reason ? reason : "unknown");
			return false;
		}
		g_noSpeakerTameSessionId = *sessionId;
		g_noSpeakerTamePrimaryId = primary->GetFormID();
		spdlog::info("[TFD][Bleedout][P14OWN] bleed no-speaker suppress session id={} primary={:08X} crowdSize={} reason={}",
			g_noSpeakerTameSessionId,
			g_noSpeakerTamePrimaryId,
			actors.size(),
			reason ? reason : "unknown");
		return true;
	}

	bool PromoteNextSpeakerFromTruceQueue(RE::TESQuest* truceQuest, const std::array<RE::BGSRefAlias*, 10>& truceAliases, RE::Actor* player, const char* reason, bool rejectCurrent, RuntimeHostStateRefs state, const RuntimeHostHandlers& handlers)
	{
		if (!player || !truceQuest) {
			return false;
		}
		if (rejectCurrent && state.bleedSpeakerId && *state.bleedSpeakerId != 0 && state.bleedRejectedSpeakerIds) {
			state.bleedRejectedSpeakerIds->insert(*state.bleedSpeakerId);
		}
		for (auto* alias : truceAliases) {
			if (!alias) continue;
			auto* actor = alias->GetActorReference();
			if (!actor || actor == player || actor->IsDead() || actor->IsDisabled()) continue;
			const auto actorID = actor->GetFormID();
			if (state.bleedRejectedSpeakerIds && state.bleedRejectedSpeakerIds->find(actorID) != state.bleedRejectedSpeakerIds->end()) continue;
			if (handlers.isBleedCrowdSupportedAggressor && !handlers.isBleedCrowdSupportedAggressor(actor)) continue;
			if (handlers.isBleedSpaceCompatible && !handlers.isBleedSpaceCompatible(actor, player)) continue;
			if (StartTruceSessionForSpeaker(player, actor, reason ? reason : "truce_queue_promote", state, handlers)) {
				if (handlers.resetGreetRuntime) handlers.resetGreetRuntime("bleed_reset");
				if (handlers.setPrevDialogueOpen) handlers.setPrevDialogueOpen(false);
				if (state.bleedPaused) *state.bleedPaused = false;
				if (state.bleedPauseStarted) *state.bleedPauseStarted = {};
				if (state.bleedSpeakerKickLast) *state.bleedSpeakerKickLast = {};
				if (state.bleedSpeakerKickCount) *state.bleedSpeakerKickCount = 0;
				if (state.bleedLastSeconds) *state.bleedLastSeconds = -1;
				if (state.bleedStart) *state.bleedStart = Clock::now();
				spdlog::info("[TFD][BleedoutDialogueRuntime][R24] promoted next bleed speaker actor={:08X} reason={}", actorID, reason ? reason : "unknown");
				return true;
			}
			if (state.bleedRejectedSpeakerIds) state.bleedRejectedSpeakerIds->insert(actorID);
		}
		return false;
	}

	void MaintainPrimaryCaptorBinding(bool dialogueOpen, RuntimeHostStateRefs state)
	{
		if (!state.inBleedState || !state.inBleedState->load(std::memory_order_acquire) || !state.bleedSpeakerId || *state.bleedSpeakerId == 0 || g_truceSessionId == 0) {
			return;
		}
		if (dialogueOpen) {
			return;
		}
		auto* actor = RE::TESForm::LookupByID<RE::Actor>(*state.bleedSpeakerId);
		if (!actor || actor->IsDead() || actor->IsDisabled()) {
			return;
		}
		if (IsCaptorAliasPrimary(actor)) {
			return;
		}
		const auto now = Clock::now();
		if (WasCaptorRecentlyBound(now, std::chrono::milliseconds(1500))) {
			return;
		}
		BindCaptorAliases(actor, "maintain_primary_missing");
	}

	std::uint32_t GetTruceSessionID()
	{
		return g_truceSessionId;
	}

	std::uint32_t GetNoSpeakerTameSessionID()
	{
		return g_noSpeakerTameSessionId;
	}

	std::uint32_t GetNoSpeakerTamePrimaryFormID()
	{
		return g_noSpeakerTamePrimaryId;
	}

	std::chrono::steady_clock::time_point GetNoSpeakerTameLastAttempt()
	{
		return g_bleedNoSpeakerTameLastAttempt;
	}

	void SetNoSpeakerTameLastAttempt(std::chrono::steady_clock::time_point when)
	{
		g_bleedNoSpeakerTameLastAttempt = when;
	}

	std::uint32_t GetBleedSpeakerID()
	{
		return g_bleedSpeakerId;
	}

	RE::Actor* GetBleedSpeakerActor()
	{
		return g_bleedSpeakerId != 0 ? RE::TESForm::LookupByID<RE::Actor>(g_bleedSpeakerId) : nullptr;
	}

	void ResetBleedSpeakerKick()
	{
		g_bleedSpeakerKickLast = {};
		g_bleedSpeakerKickCount = 0;
	}

	int GetBleedDialogueRetryCount()
	{
		return g_bleedDialogueRetryCount;
	}

	void SetBleedDialogueRetryCount(int count)
	{
		g_bleedDialogueRetryCount = count;
	}

	std::vector<std::uint32_t> GetBleedCrowdAssignedIDs()
	{
		return g_bleedCrowdAssigned;
	}

	bool HasBleedCrowdAssignedID(std::uint32_t actorID)
	{
		return actorID != 0 && std::find(g_bleedCrowdAssigned.begin(), g_bleedCrowdAssigned.end(), actorID) != g_bleedCrowdAssigned.end();
	}

	void ClearBleedCrowdAssigned()
	{
		g_bleedCrowdAssigned.clear();
	}

	namespace
	{
		void CapturePleasureCrowdSnapshotFromAssigned(const char* reason)
		{
			if (g_bleedCrowdAssigned.empty()) {
				return;
			}

			g_bleedPleasureCrowdSnapshot.clear();
			g_bleedPleasureCrowdSnapshot.reserve(g_bleedCrowdAssigned.size());
			for (const auto actorID : g_bleedCrowdAssigned) {
				if (actorID != 0 && std::find(g_bleedPleasureCrowdSnapshot.begin(), g_bleedPleasureCrowdSnapshot.end(), actorID) == g_bleedPleasureCrowdSnapshot.end()) {
					g_bleedPleasureCrowdSnapshot.push_back(actorID);
				}
			}

			spdlog::info(
				"[TFD][Bleedout][R457A] captured pleasure crowd snapshot count={} assigned={} reason={}",
				static_cast<unsigned int>(g_bleedPleasureCrowdSnapshot.size()),
				static_cast<unsigned int>(g_bleedCrowdAssigned.size()),
				reason ? reason : "unknown");
		}
	}

	std::vector<std::uint32_t> GetBleedPleasureCrowdSnapshotIDs()
	{
		return g_bleedPleasureCrowdSnapshot;
	}

	void ClearBleedPleasureCrowdSnapshot(const char* reason)
	{
		if (!g_bleedPleasureCrowdSnapshot.empty()) {
			spdlog::info(
				"[TFD][Bleedout][R457A] cleared pleasure crowd snapshot count={} reason={}",
				static_cast<unsigned int>(g_bleedPleasureCrowdSnapshot.size()),
				reason ? reason : "unknown");
		}
		g_bleedPleasureCrowdSnapshot.clear();
	}

	void ClearBleedRejectedSpeakerIds()
	{
		g_bleedRejectedSpeakerIds.clear();
	}

	bool HasBleedRejectedSpeakerID(std::uint32_t actorID)
	{
		return g_bleedRejectedSpeakerIds.find(actorID) != g_bleedRejectedSpeakerIds.end();
	}

	void AddBleedRejectedSpeakerID(std::uint32_t actorID)
	{
		if (actorID != 0) {
			g_bleedRejectedSpeakerIds.insert(actorID);
		}
	}

	const char* GetTerminalCommitName(TerminalCommit kind)
	{
		switch (kind) {
		case TerminalCommit::PayRelease:
			return "pay_release";
		case TerminalCommit::Pleasure:
			return "pleasure";
		case TerminalCommit::Captive:
			return "captive";
		case TerminalCommit::NonCaptiveFallback:
			return "noncaptive_fallback";
		default:
			return "none";
		}
	}

	TerminalCommit GetTerminalCommit()
	{
		return static_cast<TerminalCommit>(g_terminalCommit.load(std::memory_order_acquire));
	}

	bool HasTerminalCommit()
	{
		return GetTerminalCommit() != TerminalCommit::None;
	}

	bool TryBeginTerminalCommit(TerminalCommit kind, const char* reason)
	{
		std::uint8_t expected = static_cast<std::uint8_t>(TerminalCommit::None);
		const auto desired = static_cast<std::uint8_t>(kind);
		if (g_terminalCommit.compare_exchange_strong(expected, desired, std::memory_order_acq_rel)) {
			spdlog::info("[TFD][Bleedout] terminal commit claimed kind={} reason={}",
				GetTerminalCommitName(kind),
				reason ? reason : "unknown");
			return true;
		}

		auto current = static_cast<TerminalCommit>(expected);
		spdlog::info("[TFD][Bleedout] terminal commit blocked requested={} active={} reason={}",
			GetTerminalCommitName(kind),
			GetTerminalCommitName(current),
			reason ? reason : "unknown");
		return false;
	}

	void ClearTerminalCommit(const char* reason)
	{
		const auto prev = static_cast<TerminalCommit>(g_terminalCommit.exchange(static_cast<std::uint8_t>(TerminalCommit::None), std::memory_order_acq_rel));
		if (prev != TerminalCommit::None) {
			spdlog::info("[TFD][Bleedout] terminal commit cleared previous={} reason={}",
				GetTerminalCommitName(prev),
				reason ? reason : "unknown");
		}
	}

	const char* GetDialogueOutcomeName(DialogueOutcome outcome)
	{
		switch (outcome) {
		case DialogueOutcome::PayRelease:
			return "pay_release";
		case DialogueOutcome::Pleasure:
			return "pleasure";
		case DialogueOutcome::Captive:
			return "captive";
		default:
			return "none";
		}
	}

	void SetDialogueOutcome(DialogueOutcome outcome, const char* reason)
	{
		g_dialogueOutcome = outcome;
		spdlog::info("[TFD][Bleedout] dialogue outcome set={} reason={}",
			GetDialogueOutcomeName(outcome),
			reason ? reason : "unknown");
	}

	void ClearDialogueOutcome(const char* reason)
	{
		if (g_dialogueOutcome != DialogueOutcome::None) {
			spdlog::info("[TFD][Bleedout] dialogue outcome cleared={} reason={}",
				GetDialogueOutcomeName(g_dialogueOutcome),
				reason ? reason : "unknown");
		}
		g_dialogueOutcome = DialogueOutcome::None;
	}

	DialogueOutcome GetDialogueOutcome()
	{
		return g_dialogueOutcome;
	}

	void ArmSystemEventOutcomeWindow(const char* reason, double seconds)
	{
		const auto now = Clock::now();
		const auto clamped = (std::max)(0.25, seconds);
		const auto until = now + std::chrono::milliseconds(static_cast<int>(clamped * 1000.0));
		if (!g_awaitingSystemEventOutcome || until > g_systemEventUntil) {
			g_systemEventUntil = until;
		}
		g_awaitingSystemEventOutcome = true;
		spdlog::info("[TFD][Bleedout] arm system-event outcome window seconds={:.2f} reason={}",
			clamped,
			reason ? reason : "unknown");
	}

	void ClearSystemEventOutcomeWindow(const char* reason)
	{
		if (g_awaitingSystemEventOutcome) {
			spdlog::info("[TFD][Bleedout] clear system-event outcome window reason={}", reason ? reason : "unknown");
		}
		g_awaitingSystemEventOutcome = false;
		g_systemEventUntil = {};
		g_systemEventLastDeferredLog = {};
	}

	void ForceStopBleedRuntimeForCaptiveRecapture(const char* reason)
	{
		const char* why = reason ? reason : "captivity_recapture_stop";
		g_inBleedState.store(false, std::memory_order_release);
		g_minHp = 0.0f;
		g_bleedStart = {};
		g_bleedLastSeconds = -1;
		g_bleedPaused = false;
		g_bleedPauseStarted = {};
		g_bleedLastCalmPulse = {};
		g_bleedLastCrowdAssign = {};
		g_bleedNoSpeakerTameLastAttempt = {};
		g_bleedSpeakerId = 0;
		g_bleedPendingCaptiveOutcome = false;
		g_bleedPendingNonCaptiveOutcome = false;
		ResetBleedSpeakerKick();
		ClearBleedCrowdAssigned();
		ClearBleedRejectedSpeakerIds();
		ClearDialogueOutcome(why);
		ClearSystemEventOutcomeWindow(why);
		ClearTerminalCommit(why);
		TFD::BleedoutGreet::ResetRuntime(why);
		spdlog::info("[TFD][Bleedout] captive recapture bleed runtime force-stopped reason={}", why);
	}

	bool IsSystemEventOutcomeWindowActive()
	{
		if (!g_awaitingSystemEventOutcome) {
			return false;
		}
		if (Clock::now() >= g_systemEventUntil) {
			g_awaitingSystemEventOutcome = false;
			g_systemEventUntil = {};
			g_systemEventLastDeferredLog = {};
			return false;
		}
		return true;
	}

	bool IsAwaitingSystemEventOutcome()
	{
		return g_awaitingSystemEventOutcome;
	}

	bool IsSystemEventPendingForFallback(const char** outReason)
	{
		const bool active = IsSystemEventOutcomeWindowActive();
		if (outReason) {
			*outReason = active ? "system_event_outcome_window" : nullptr;
		}
		return active;
	}

	bool ShouldLogDeferredSystemEvent(std::chrono::steady_clock::time_point now)
	{
		return g_systemEventLastDeferredLog.time_since_epoch().count() == 0 ||
			(now - g_systemEventLastDeferredLog) >= std::chrono::milliseconds(750);
	}

	void NoteDeferredSystemEventLog(std::chrono::steady_clock::time_point now)
	{
		g_systemEventLastDeferredLog = now;
	}

	void ResetSystemEventState(const char* reason)
	{
		ClearDialogueOutcome(reason ? reason : "reset");
		ClearSystemEventOutcomeWindow(reason ? reason : "reset");
		ClearTerminalCommit(reason ? reason : "reset");
	}

	bool ShouldDropSystemEventBecauseFallback(const char* eventName, bool terminalCommitActive, const char* terminalCommitName)
	{
		if (!terminalCommitActive) {
			return false;
		}
		spdlog::info("[TFD][Bleedout] drop system event={} activeTerminalCommit={}",
			eventName ? eventName : "unknown",
			terminalCommitName ? terminalCommitName : "unknown");
		return true;
	}

	
	bool HandleOutcomePayEvent(const OutcomeEventContext& context, const OutcomeEventHandlers& handlers)
	{
		if (handlers.shouldDropBecauseFallback &&
			handlers.shouldDropBecauseFallback(context.rawEventName ? context.rawEventName : context.eventName)) {
			return true;
		}
		if (handlers.armOutcomeWindow) {
			handlers.armOutcomeWindow("mod_event_pay", 3.0);
		}
		if (context.inBleedState) {
			if (handlers.setDialogueOutcome) {
				handlers.setDialogueOutcome(DialogueOutcome::PayRelease, "mod_event_pay");
			}
			(void)CommitDialogueOutcome(DialogueOutcome::PayRelease, context.actor, "mod_event_pay");
		}
		return true;
	}

	bool HandleOutcomePleasureEvent(const OutcomeEventContext& context, const OutcomeEventHandlers& handlers)
	{
		if (handlers.shouldDropBecauseFallback &&
			handlers.shouldDropBecauseFallback(context.rawEventName ? context.rawEventName : context.eventName)) {
			return true;
		}
		if (handlers.armOutcomeWindow) {
			handlers.armOutcomeWindow("mod_event_pleasure", 12.0);
		}
		if (!context.inBleedState) {
			return true;
		}

		if (context.preserveCaptive) {
			if (handlers.beginCaptivePleasureFlow &&
				!handlers.beginCaptivePleasureFlow(context.actorFormID, "mod_event_pleasure")) {
				spdlog::warn("[TFD][Bleedout] ignore mod_event_pleasure reason=captive_flow_reject actor={:08X}", context.actorFormID);
				return true;
			}
			if (handlers.setDialogueOutcome) {
				handlers.setDialogueOutcome(DialogueOutcome::Pleasure, "mod_event_pleasure");
			}
			if (handlers.prepareCaptivePleasureScene) {
				handlers.prepareCaptivePleasureScene("mod_event_pleasure");
			}
			if (handlers.completeCaptivePleasureHandoff) {
				handlers.completeCaptivePleasureHandoff("mod_event_pleasure");
			}
			return true;
		}

		if (!CommitDialogueOutcome(DialogueOutcome::Pleasure, context.actor, "mod_event_pleasure")) {
			spdlog::warn("[TFD][Bleedout] ignore mod_event_pleasure reason=flow_reject actor={:08X}", context.actorFormID);
			return true;
		}
		if (handlers.setDialogueOutcome) {
			handlers.setDialogueOutcome(DialogueOutcome::Pleasure, "mod_event_pleasure");
		}
		if (handlers.prepareBleedoutPleasureScene) {
			handlers.prepareBleedoutPleasureScene("mod_event_pleasure");
		}
		if (handlers.completeBleedPleasureHandoff) {
			handlers.completeBleedPleasureHandoff("mod_event_pleasure");
		}
		return true;
	}

	bool HandleOutcomeCaptiveEvent(const OutcomeEventContext& context, const OutcomeEventHandlers& handlers)
	{
		if (handlers.shouldDropBecauseFallback &&
			handlers.shouldDropBecauseFallback(context.rawEventName ? context.rawEventName : context.eventName)) {
			return true;
		}
		if (handlers.armOutcomeWindow) {
			handlers.armOutcomeWindow("mod_event_captive", 3.0);
		}
		if (context.inBleedState) {
			if (!CommitDialogueOutcome(DialogueOutcome::Captive, context.actor, "mod_event_captive")) {
				spdlog::warn("[TFD][Bleedout] ignore mod_event_captive reason=flow_reject actor={:08X}", context.actorFormID);
				return true;
			}
			if (handlers.setDialogueOutcome) {
				handlers.setDialogueOutcome(DialogueOutcome::Captive, "mod_event_captive");
			}
			}
		return true;
	}

	bool HandleOutcomeResetEvent(const OutcomeEventContext&, const OutcomeEventHandlers& handlers)
	{
		if (handlers.clearDialogueOutcome) {
			handlers.clearDialogueOutcome("mod_event_reset");
		}
		if (handlers.clearOutcomeWindow) {
			handlers.clearOutcomeWindow("mod_event_reset");
		}
		return true;
	}

	static float ResolveCalmRadius(const CompletionHandlers& handlers)
	{
		const float sweep = handlers.getSweepRadius ? handlers.getSweepRadius() : 0.0f;
		return (std::max)(2200.0f, sweep);
	}

	bool CompleteCaptivePleasureHandoff(const char* reason, const CompletionHandlers& handlers)
	{
		const char* why = reason ? reason : "captive_pleasure_handoff";
		RE::Actor* preservedSpeaker = handlers.resolveRuntimeSpeaker ? handlers.resolveRuntimeSpeaker() : nullptr;
		const auto preservedSpeakerID = preservedSpeaker ? preservedSpeaker->GetFormID() : 0u;
		const bool preserveReleasedWork = TFD::Captive::IsReleasedWorkActive();

		if (handlers.clearOutcomeWindow) {
			handlers.clearOutcomeWindow(why);
		}
		if (handlers.tryBeginTerminalCommit && !handlers.tryBeginTerminalCommit(TerminalCommit::Pleasure, why)) {
			return false;
		}
		if (handlers.clearPendingCinematicFadeIn) {
			handlers.clearPendingCinematicFadeIn();
		}
		if (handlers.clearBridgeAliases) {
			handlers.clearBridgeAliases(why);
		}

		if (preserveReleasedWork) {
			// R234A: Captive Work escape-break -> Bleedout Pleasure must not be
			// downgraded into normal Kidnapped/Captive.  Keep ReleasedWork (state 3),
			// keep work aliases/resource context, and do not clear the bleed runtime in
			// a way that releases suppression/restores aggression before OStim owns the
			// speaker.
			if (handlers.setGraceActive) {
				handlers.setGraceActive(false);
			}
			if (handlers.clearLastAggressor) {
				handlers.clearLastAggressor();
			}
			if (handlers.setPrevDialogueOpen) {
				handlers.setPrevDialogueOpen(false);
			}
			if (handlers.setPrevLockpickOpen) {
				handlers.setPrevLockpickOpen(false);
			}
			if (handlers.syncPlayerCaptiveAlias) {
				handlers.syncPlayerCaptiveAlias(why);
			}
			if (handlers.setPlayerBleedImmune) {
				handlers.setPlayerBleedImmune(false);
			}
			if (handlers.recoverPlayerForTransition) {
				handlers.recoverPlayerForTransition();
			}
			if (handlers.applyCalmBubble) {
				handlers.applyCalmBubble(ResolveCalmRadius(handlers));
			}
			if (handlers.updatePreCombatState) {
				handlers.updatePreCombatState();
			}
			if (handlers.beginPleasure) {
				handlers.beginPleasure(preservedSpeaker, true, why);
			}
			spdlog::info("[TFD][Bleedout][R234A] captive work pleasure handoff preserved work context reason={} speaker={:08X}", why, preservedSpeakerID);
			return true;
		}

		if (handlers.clearEscapeContext) {
			handlers.clearEscapeContext();
		}
		if (handlers.resetLockpickWatch) {
			handlers.resetLockpickWatch();
		}
		if (handlers.setGraceActive) {
			handlers.setGraceActive(false);
		}
		if (handlers.clearLastAggressor) {
			handlers.clearLastAggressor();
		}
		if (handlers.resetBleedRuntimeState) {
			handlers.resetBleedRuntimeState(true);
		}
		if (handlers.setPrevDialogueOpen) {
			handlers.setPrevDialogueOpen(false);
		}
		if (handlers.setPrevLockpickOpen) {
			handlers.setPrevLockpickOpen(false);
		}
		if (handlers.setCaptiveRuntime) {
			handlers.setCaptiveRuntime(true);
		}
		if (handlers.syncPlayerCaptiveAlias) {
			handlers.syncPlayerCaptiveAlias(why);
		}
		if (handlers.setPlayerBleedImmune) {
			handlers.setPlayerBleedImmune(false);
		}
		if (handlers.recoverPlayerForTransition) {
			handlers.recoverPlayerForTransition();
		}
		// R250A: Bleedout-source Pleasure must not keep the transition calm window
		// alive after the Pleasure commit.  That window repeatedly StopCombat /
		// EvaluatePackage-s nearby actors and can pull a drawn speaker out of stance
		// before PleasureFailed -> Fight.
		if (handlers.applyCalmBubble) {
			spdlog::info("[TFD][Bleedout][R250A] calm bubble skipped during bleedout pleasure handoff reason={}", why);
		}
		if (handlers.updatePreCombatState) {
			handlers.updatePreCombatState();
		}
		if (handlers.beginPleasure) {
			handlers.beginPleasure(preservedSpeaker, true, why);
		}
		spdlog::info("[TFD][Bleedout] captive pleasure handoff complete reason={} speaker={:08X}", why, preservedSpeakerID);
		return true;
	}

	bool CompletePayRelease(const char* reason, const CompletionHandlers& handlers)
	{
		const char* why = reason ? reason : "bleed_pay_release";
		RE::Actor* paySpeaker = handlers.resolveRuntimeSpeaker ? handlers.resolveRuntimeSpeaker() : nullptr;
		const auto speakerId = paySpeaker ? paySpeaker->GetFormID() : 0u;

		if (handlers.clearOutcomeWindow) {
			handlers.clearOutcomeWindow(why);
		}
		if (handlers.tryBeginTerminalCommit && !handlers.tryBeginTerminalCommit(TerminalCommit::PayRelease, why)) {
			spdlog::info(
				"[TFD][Bleedout][P15BOWN] terminal PayRelease rejected before ownership change actor={:08X} reason={}",
				speakerId,
				why);
			return false;
		}

		// P15BOWN: Bleedout Pay is a terminal PayRelease topic.  The Pay guard is
		// only a bridge while old dialogue/runtime ownership is cleared.  It must
		// not be the final owner and it must not expire into restore aggression.
		if (paySpeaker) {
			TFD::HostilityController::ArmPayDialoguePassiveGuard(
				paySpeaker,
				static_cast<double>(kBleedoutReleaseGraceSeconds) + 4.0,
				"bleedout_pay_terminal_cleanup_bridge_guard");
		}

		if (g_truceSessionId != 0) {
			const bool released = TFD::HostilityController::ReleaseBleedoutSuppressSession(
				g_truceSessionId,
				TFD::HostilityController::ReleaseReason::FlowHandoff,
				"bleedout_pay_terminal_dialogue_suppress_end");
			spdlog::info(
				"[TFD][Bleedout][P15BOWN] dialogue suppress ended before PayRelease final owner session={} released={} actor={:08X} reason={}",
				g_truceSessionId,
				released ? 1 : 0,
				speakerId,
				why);
			g_truceSessionId = 0;
		}

		if (handlers.clearPendingCinematicFadeIn) {
			handlers.clearPendingCinematicFadeIn();
		}
		if (handlers.clearBridgeAliases) {
			handlers.clearBridgeAliases(why);
		}
		if (handlers.clearFactionState) {
			handlers.clearFactionState();
		}
		if (paySpeaker) {
			TFD::HostilityController::MarkPayDialoguePassiveGuardRemovePacifyOnRelease(
				paySpeaker,
				"bleedout_pay_terminal_bridge_owns_pacify_until_final_owner");
		}
		if (handlers.clearEscapeContext) {
			handlers.clearEscapeContext();
		}
		if (handlers.resetLockpickWatch) {
			handlers.resetLockpickWatch();
		}
		if (handlers.setGraceActive) {
			handlers.setGraceActive(false);
		}
		if (handlers.clearLastAggressor) {
			handlers.clearLastAggressor();
		}
		if (handlers.resetBleedRuntimeState) {
			handlers.resetBleedRuntimeState(false);
		}

		std::optional<RE::FormID> finalSessionId;
		if (paySpeaker) {
			auto* player = RE::PlayerCharacter::GetSingleton();
			finalSessionId = TFD::HostilityController::BeginBleedoutPayReleaseSuppressSession(
				player,
				paySpeaker,
				0.0,
				static_cast<double>(kBleedoutReleaseGraceSeconds),
				"bleedout_pay_terminal_final_owner");
		}

		if (handlers.setPrevDialogueOpen) {
			handlers.setPrevDialogueOpen(false);
		}
		if (handlers.setPrevLockpickOpen) {
			handlers.setPrevLockpickOpen(false);
		}
		if (handlers.setCaptiveRuntime) {
			handlers.setCaptiveRuntime(false);
		}
		if (handlers.setPlayerBleedImmune) {
			handlers.setPlayerBleedImmune(false);
		}
		if (handlers.recoverPlayerForTransition) {
			handlers.recoverPlayerForTransition();
		}
		if (handlers.applyCalmBubble) {
			handlers.applyCalmBubble(ResolveCalmRadius(handlers));
		}
		if (handlers.beginLeftForDeadCooldown) {
			handlers.beginLeftForDeadCooldown(static_cast<int>(kBleedoutReleaseGraceSeconds));
		}
		if (handlers.setGraceSeconds) {
			handlers.setGraceSeconds(static_cast<int>(kBleedoutReleaseGraceSeconds));
		}
		if (handlers.updatePreCombatState) {
			handlers.updatePreCombatState();
		}
		ScrubTerminalReleaseGlobals(why);

		const bool payGuardActiveAfter = paySpeaker && TFD::HostilityController::IsPayDialoguePassiveGuardActive(paySpeaker);
		const bool finalOwned = paySpeaker && TFD::HostilityController::IsBleedoutPayReleaseSuppressed(paySpeaker);
		// P16OWN: Bleedout Pay is terminal. Gold must be cleared by the
		// Bleedout PayRelease owner, not preserved by cross-flow clear guards.
		TFD::PayModel::ClearSharedGold("bleedout_pay_release_complete");
		spdlog::info(
			"[TFD][Bleedout][R216A] terminal PayRelease safe recovery complete actor={:08X} finalSession={} payGuardActiveAfter={} finalOwned={} duration={:.1f} reason={} payGoldCleared=1",
			speakerId,
			finalSessionId.value_or(0),
			payGuardActiveAfter ? 1 : 0,
			finalOwned ? 1 : 0,
			static_cast<double>(kBleedoutReleaseGraceSeconds),
			why);
		return true;
	}

	bool CompleteBleedPleasureHandoff(const char* reason, const CompletionHandlers& handlers)
	{
		const char* why = reason ? reason : "bleed_pleasure_handoff";
		if (handlers.clearOutcomeWindow) {
			handlers.clearOutcomeWindow(why);
		}
		if (handlers.tryBeginTerminalCommit && !handlers.tryBeginTerminalCommit(TerminalCommit::Pleasure, why)) {
			return false;
		}
		if (handlers.clearPendingCinematicFadeIn) {
			handlers.clearPendingCinematicFadeIn();
		}
		if (handlers.clearBridgeAliases) {
			// W53: Bleedout is only the source of the OStim scene now.  Do not keep
			// TFDBleedoutQuest / bridge forcegreet ownership alive through Pleasure,
			// because it blocks PleasureFailed terminal choices and can reopen/stick
			// the Bleedout dialogue after the player has already stood up.
			handlers.clearBridgeAliases(why);
			spdlog::info("[TFD][Bleedout][W53] cleared bridge aliases during pleasure handoff reason={}", why);
		}
		if (handlers.clearEscapeContext) {
			handlers.clearEscapeContext();
		}
		if (handlers.resetLockpickWatch) {
			handlers.resetLockpickWatch();
		}
		if (handlers.setGraceActive) {
			handlers.setGraceActive(false);
		}
		if (handlers.transitionBleedRuntimeToPleasureCommit) {
			handlers.transitionBleedRuntimeToPleasureCommit(why);
		}
		if (handlers.setPrevDialogueOpen) {
			handlers.setPrevDialogueOpen(false);
		}
		if (handlers.setPrevLockpickOpen) {
			handlers.setPrevLockpickOpen(false);
		}
		if (handlers.setCaptiveRuntime) {
			handlers.setCaptiveRuntime(false);
		}
		if (handlers.setPlayerBleedImmune) {
			handlers.setPlayerBleedImmune(false);
		}
		if (handlers.recoverPlayerForTransition) {
			handlers.recoverPlayerForTransition();
		}
		if (handlers.applyCalmBubble) {
			handlers.applyCalmBubble(ResolveCalmRadius(handlers));
		}
		if (handlers.beginLeftForDeadCooldown) {
			handlers.beginLeftForDeadCooldown(static_cast<int>(kBleedoutReleaseGraceSeconds));
		}
		if (handlers.setGraceSeconds) {
			handlers.setGraceSeconds(static_cast<int>(kBleedoutReleaseGraceSeconds));
		}
		if (handlers.refreshPostDefeatGlobals) {
			handlers.refreshPostDefeatGlobals();
		}
		if (handlers.updatePreCombatState) {
			handlers.updatePreCombatState();
		}
		if (handlers.beginPleasure) {
			handlers.beginPleasure(handlers.resolveRuntimeSpeaker ? handlers.resolveRuntimeSpeaker() : nullptr, false, why);
		}
		spdlog::info("[TFD][Bleedout] pleasure handoff complete reason={}", why);
		return true;
	}

	bool TryHandlePayReleaseDialogueClosed(bool seenDialogue, bool prevDialogueOpen, const DialogueCloseHandlers& handlers)
	{
		if (!seenDialogue || !prevDialogueOpen || GetDialogueOutcome() != DialogueOutcome::PayRelease) {
			return false;
		}
		if (handlers.clearDialogueOutcome) {
			handlers.clearDialogueOutcome("dialogue_closed_pay_release");
		}
		if (handlers.completePayRelease) {
			handlers.completePayRelease("dialogue_closed_pay_release");
		}
		return true;
	}

	bool TryResolvePostDialogueSystemEvent(const char* reason, const PendingSystemEventHandlers& handlers)
	{
		if (!IsAwaitingSystemEventOutcome()) {
			return false;
		}

		switch (GetDialogueOutcome()) {
		case DialogueOutcome::PayRelease:
			if (handlers.clearDialogueOutcome) {
				handlers.clearDialogueOutcome(reason ? reason : "system_event_pay");
			}
			if (handlers.completePayRelease) {
				handlers.completePayRelease(reason ? reason : "system_event_pay");
			}
			return true;

		case DialogueOutcome::Captive:
			if (handlers.clearOutcomeWindow) {
				handlers.clearOutcomeWindow(reason ? reason : "system_event_captive");
			}
			if (handlers.clearDialogueOutcome) {
				handlers.clearDialogueOutcome(reason ? reason : "system_event_captive");
			}
			if (IsCaptiveEscapeRebleedFlow()) {
				auto* speaker = GetBleedSpeakerActor();
				spdlog::info("[TFD][Bleedout] post-dialogue system event committed captive -> captive recapture FSM speaker={:08X}",
					speaker ? speaker->GetFormID() : 0u);
				if (TFD::Captive::CommitRecapture(speaker, reason ? reason : "system_event_captive_recapture")) {
					ForceStopBleedRuntimeForCaptiveRecapture("system_event_captive_recapture");
				}
				return true;
			}
			if (handlers.releaseFlowHandoff) {
				handlers.releaseFlowHandoff();
			}
			if (handlers.resolveCaptiveMarker && handlers.resolveCaptiveMarker()) {
				spdlog::info("[TFD][Bleedout] post-dialogue system event committed captive -> captive marker found");
				if (handlers.doBlackoutTeleport) {
					handlers.doBlackoutTeleport();
				}
				if (handlers.setGraceSeconds) {
					handlers.setGraceSeconds(4);
				}
			} else {
				spdlog::info("[TFD][Bleedout] post-dialogue system event committed captive -> no marker -> resolve fallback");
				if (handlers.releaseNoSpeakerTameSession) {
					handlers.releaseNoSpeakerTameSession("system_event_captive_no_marker");
				}
				if (handlers.exitBleedState) {
					handlers.exitBleedState();
				}
				if (handlers.enterNonCaptiveChoice) {
					handlers.enterNonCaptiveChoice("system_event_captive_no_marker");
				}
			}
			return true;

		default:
			break;
		}

		return false;
	}

	bool TryHandlePendingSystemEventFallback(const PendingSystemEventContext& context, const char* reason, const PendingSystemEventHandlers& handlers)
	{
		const char* pendingReason = nullptr;
		if (IsSystemEventPendingForFallback(&pendingReason)) {
			const auto now = Clock::now();
			if (ShouldLogDeferredSystemEvent(now)) {
				NoteDeferredSystemEventLog(now);
				spdlog::info("[TFD][Bleedout] fallback deferred pendingSystemEvent={} outcome={} runtimePhase={} runtimeActive={} runtimeBlocking={}",
					pendingReason ? pendingReason : "unknown",
					GetDialogueOutcomeName(GetDialogueOutcome()),
					context.runtimePhaseName ? context.runtimePhaseName : "unknown",
					context.runtimeActive ? 1 : 0,
					context.runtimeBlocking ? 1 : 0);
			}
			return true;
		}

		ClearSystemEventOutcomeWindow(reason ? reason : "system_event_window_expired");
		constexpr int kMaxBleedDialogueNoCommitRetries = 1;
		int retryCount = handlers.getRetryCount ? handlers.getRetryCount() : 0;
		if (retryCount < kMaxBleedDialogueNoCommitRetries && handlers.promoteNextSpeaker &&
			handlers.promoteNextSpeaker(reason ? reason : "system_event_window_expired")) {
			++retryCount;
			if (handlers.setRetryCount) {
				handlers.setRetryCount(retryCount);
			}
			spdlog::info("[TFD][Bleedout] system-event window expired -> retry next truce candidate attempt={}/{}",
				retryCount,
				kMaxBleedDialogueNoCommitRetries);
			return true;
		}
		if (retryCount >= kMaxBleedDialogueNoCommitRetries) {
			spdlog::info("[TFD][Bleedout] system-event window expired -> retry cap reached, fallback to captive resolution");
		}
		if (handlers.setRetryCount) {
			handlers.setRetryCount(0);
		}
		if (handlers.resolveCaptiveMarker && handlers.resolveCaptiveMarker()) {
			if (IsCaptiveEscapeRebleedFlow()) {
				spdlog::info("[TFD][Bleedout] system-event window expired -> captive recapture FSM fallback");
				if (TFD::Captive::CommitRecapture(GetBleedSpeakerActor(), reason ? reason : "system_event_window_expired_recapture")) {
					ForceStopBleedRuntimeForCaptiveRecapture("system_event_window_expired_recapture");
				}
				return true;
			}
			spdlog::info("[TFD][Bleedout] system-event window expired -> fallback captive marker found");
			if (handlers.doBlackoutTeleport) {
				handlers.doBlackoutTeleport();
			}
			if (handlers.setGraceSeconds) {
				handlers.setGraceSeconds(4);
			}
		} else {
			spdlog::info("[TFD][Bleedout] system-event window expired -> fallback captive marker missing -> resolve fallback");
			if (handlers.releaseNoSpeakerTameSession) {
				handlers.releaseNoSpeakerTameSession("system_event_window_expired_no_marker");
			}
			if (handlers.exitBleedState) {
				handlers.exitBleedState();
			}
			if (handlers.enterNonCaptiveChoice) {
				handlers.enterNonCaptiveChoice("system_event_window_expired_no_marker");
			}
		}
		return true;
	}

	bool HandleBleedTimeout(const TimeoutContext& context, const char* reason, const TimeoutHandlers& handlers)
	{
		const char* why = reason ? reason : "bleed_timeout";
		if (IsCaptiveRecaptureGuardActive()) {
			if (handlers.resetBleedRuntimeState) {
				handlers.resetBleedRuntimeState();
			}
			ForceStopBleedRuntimeForCaptiveRecapture("late_bleed_timeout_after_recapture");
			spdlog::info("[TFD][Bleedout] bleed timeout suppressed by captive recapture guard reason={}", why);
			return true;
		}
		const auto dialogueMode = TFD::InteractionRouter::DialogueOpen::GetMode();
		const bool nativeBleedDialoguePending =
			TFD::InteractionRouter::DialogueOpen::IsActive() &&
			(dialogueMode == TFD::InteractionRouter::DialogueOpen::Mode::Bleedout ||
				dialogueMode == TFD::InteractionRouter::DialogueOpen::Mode::AfterPleasure ||
				dialogueMode == TFD::InteractionRouter::DialogueOpen::Mode::PleasureFailed);
		auto* ui = RE::UI::GetSingleton();
		const bool dialogueMenuOpen = ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
		const bool greetActive = TFD::BleedoutGreet::IsActive();
		const bool greetSeen = TFD::BleedoutGreet::HasSeenDialogue();
		const bool greetAck = TFD::BleedoutGreet::HasFlowGreetConfirmed();
		const bool stickyPending = TFD::BleedoutGreet::HasStickyReopenPending();
		const bool staleSeenAckOnly =
			!nativeBleedDialoguePending &&
			!dialogueMenuOpen &&
			!greetActive &&
			!stickyPending &&
			(greetSeen || greetAck);
		const bool bleedoutDialogueOwned =
			nativeBleedDialoguePending ||
			(OwnsCurrentFlow() && dialogueMenuOpen) ||
			greetActive ||
			stickyPending ||
			(!staleSeenAckOnly && (greetAck || greetSeen));
		if (!context.hasTerminalCommit && bleedoutDialogueOwned) {
			g_bleedStart = Clock::now();
			g_bleedLastSeconds = -1;
			spdlog::info(
				"[TFD][Bleedout][R470A] bleed timeout suppressed by live dialogue ownership reason={} nativePending={} menuOpen={} greetActive={} sticky={} seen={} ack={}",
				why,
				nativeBleedDialoguePending ? 1 : 0,
				dialogueMenuOpen ? 1 : 0,
				greetActive ? 1 : 0,
				stickyPending ? 1 : 0,
				greetSeen ? 1 : 0,
				greetAck ? 1 : 0);
			return true;
		}
		if (!context.hasTerminalCommit && staleSeenAckOnly) {
			spdlog::info(
				"[TFD][Bleedout][R470A] stale dialogue seen/ack ignored for timeout reason={} nativePending=0 menuOpen=0 greetActive=0 sticky=0 seen={} ack={}",
				why,
				greetSeen ? 1 : 0,
				greetAck ? 1 : 0);
		}
		if (context.hasTerminalCommit) {
			spdlog::info("[TFD][Bleedout] bleed timeout -> suppressed activeTerminalCommit={}",
				context.terminalCommitName ? context.terminalCommitName : "unknown");
			return true;
		}
		if (context.pendingSystemReason) {
			spdlog::info("[TFD][Bleedout] bleed timeout -> suppressed pendingSystemEvent={}",
				context.pendingSystemReason);
			return true;
		}

		const bool captiveMarkerResolved =
			context.pendingCaptive &&
			handlers.resolveCaptiveMarker &&
			handlers.resolveCaptiveMarker();

		if (!captiveMarkerResolved && ShouldGateDestructiveBleedoutCleanup(why)) {
			ReArmBleedoutTimeoutAfterGatedCleanup("r484a_bleed_timeout_destructive_cleanup_blocked");
			spdlog::info("[TFD][Bleedout][R484A] bleed timeout held; no destructive Generic fallback without terminal outcome reason={}", why);
			return true;
		}

		if (captiveMarkerResolved && handlers.releaseTruceGeneric) {
			handlers.releaseTruceGeneric();
		}
		if (captiveMarkerResolved) {
			if (IsCaptiveEscapeRebleedFlow() || TFD::Captive::IsEscapeBleedoutActive()) {
				spdlog::info("[TFD][Bleedout] bleed timeout -> captive recapture FSM");
				if (TFD::Captive::CommitRecapture(GetBleedSpeakerActor(), why)) {
					ForceStopBleedRuntimeForCaptiveRecapture("bleed_timeout_recapture");
				}
				return true;
			}
			if (handlers.resetBleedRuntimeState) {
				handlers.resetBleedRuntimeState();
			}
			spdlog::info("[TFD][Bleedout] bleed timeout -> captive blackout");
			if (handlers.doBlackoutTeleport) {
				handlers.doBlackoutTeleport();
			}
			if (handlers.setGraceSeconds) {
				handlers.setGraceSeconds(4);
			}
		} else {
			spdlog::info("[TFD][Bleedout] bleed timeout -> resolve no-marker fallback");
			if (handlers.releaseTruceGeneric) {
				handlers.releaseTruceGeneric();
			}
			if (handlers.enterNonCaptiveChoice) {
				handlers.enterNonCaptiveChoice(why);
			}
		}
		return true;
	}

	bool EnterNonCaptiveChoice(const char* reason, const NonCaptiveChoiceHandlers& handlers)
	{
		const char* why = reason ? reason : "unknown";
		if (handlers.hasBlockingCommit && handlers.hasBlockingCommit()) {
			spdlog::info("[TFD][Bleedout] non-captive choice suppressed activeTerminalCommit={} reason={}",
				handlers.getBlockingCommitName ? handlers.getBlockingCommitName() : "unknown",
				why);
			return false;
		}
		const bool timeoutFallback =
			std::strstr(why, "bleed_timeout") != nullptr ||
			std::strstr(why, "system_event_window_expired_no_marker") != nullptr;
		if (timeoutFallback && ShouldGateDestructiveBleedoutCleanup(why)) {
			ReArmBleedoutTimeoutAfterGatedCleanup("r484a_non_captive_fallback_blocked");
			spdlog::info("[TFD][Bleedout][R484A] non-captive fallback blocked until explicit terminal outcome reason={}", why);
			return false;
		}
		if (handlers.beginResolvedNoMarkerFallback && handlers.beginResolvedNoMarkerFallback(why)) {
			if (handlers.resetFlowRuntime) {
				handlers.resetFlowRuntime(why);
			}
			return true;
		}

		if (handlers.clearBridgeAliases) {
			handlers.clearBridgeAliases(why);
		}
		if (handlers.clearPendingCinematicFadeIn) {
			handlers.clearPendingCinematicFadeIn();
		}
		if (handlers.clearFactionState) {
			handlers.clearFactionState();
		}
		if (handlers.clearEscapeContext) {
			handlers.clearEscapeContext();
		}
		if (handlers.resetLockpickWatch) {
			handlers.resetLockpickWatch();
		}
		if (handlers.setGraceActive) {
			handlers.setGraceActive(false);
		}
		if (handlers.clearLastAggressor) {
			handlers.clearLastAggressor();
		}
		if (handlers.resetBleedRuntimeState) {
			handlers.resetBleedRuntimeState();
		}
		if (handlers.setPrevDialogueOpen) {
			handlers.setPrevDialogueOpen(false);
		}
		if (handlers.setPrevLockpickOpen) {
			handlers.setPrevLockpickOpen(false);
		}
		if (handlers.setCaptiveRuntime) {
			handlers.setCaptiveRuntime(false);
		}
		if (handlers.setPlayerBleedImmune) {
			handlers.setPlayerBleedImmune(false);
		}
		if (handlers.queueLegacyRequest) {
			handlers.queueLegacyRequest(why);
		}
		spdlog::warn("[TFD][Bleedout] fallback to legacy non-captive choice reason={}", why);
		return true;
	}

	bool DoBlackoutTeleport(const char* reason, const BlackoutHandlers& handlers)
	{
		const char* why = reason ? reason : "blackout_teleport";
		if (IsCaptiveRecaptureGuardActive()) {
			ForceStopBleedRuntimeForCaptiveRecapture("blackout_recapture_guard");
			spdlog::info("[TFD][Bleedout] blackout teleport suppressed by captive recapture guard reason={}", why);
			return true;
		}
		if (IsCaptiveEscapeRebleedFlow()) {
			spdlog::info("[TFD][Bleedout] blackout teleport redirected to captive recapture FSM reason={}", why);
			const bool committed = TFD::Captive::CommitRecapture(GetBleedSpeakerActor(), why);
			if (committed) {
				ForceStopBleedRuntimeForCaptiveRecapture("blackout_recapture_redirect");
			}
			return committed;
		}
		const auto recaptureActorID = ResolveCaptiveRecaptureActorID();

		auto directCaptiveBlackout = [&]() -> bool {
			auto runtimeHandlers = TFD::Transition::DefeatGlue::BuildTransitionRuntimeHandlers();
			auto captiveHandlers = TFD::Transition::DefeatGlue::BuildTransitionCaptiveHandlers();

			if (!TFD::Transition::ResolveCaptiveMarkerForOutcome(runtimeHandlers)) {
				spdlog::warn("[TFD][Bleedout] direct captive blackout fallback failed marker resolution reason={}", why);
				return false;
			}

			if (handlers.resetBleedRuntimeState) {
				handlers.resetBleedRuntimeState();
			}
			if (handlers.clearBridgeAliases) {
				handlers.clearBridgeAliases(why);
			}
			if (handlers.clearLastAggressor) {
				handlers.clearLastAggressor();
			}
			ClearDialogueOutcome(why);
			ClearSystemEventOutcomeWindow(why);

			const bool completed = TFD::Transition::CompleteCaptiveTransitionNow(why, runtimeHandlers, captiveHandlers);
			if (completed) {
				ReassertCaptiveIdleFlowAfterBlackout(recaptureActorID, why);
			}
			spdlog::info("[TFD][Bleedout] direct captive blackout fallback completed={} reason={}", completed ? 1 : 0, why);
			return completed;
		};

		if (handlers.hasBlockingCommit && handlers.hasBlockingCommit()) {
			spdlog::info("[TFD][Bleedout] blackout teleport suppressed activeTerminalCommit={}",
				handlers.getBlockingCommitName ? handlers.getBlockingCommitName() : "unknown");
			return false;
		}

		if (handlers.resolveCaptiveMarker && !handlers.resolveCaptiveMarker()) {
			if (directCaptiveBlackout()) {
				return true;
			}
			if (handlers.enterNonCaptiveChoice) {
				handlers.enterNonCaptiveChoice("marker_not_found");
			}
			return false;
		}

		if (handlers.tryBeginCaptiveCommit && !handlers.tryBeginCaptiveCommit(why)) {
			return false;
		}

		if (!handlers.queueCaptiveFadeTransition && !handlers.completeCaptiveTransitionNow) {
			return directCaptiveBlackout();
		}
		if (handlers.resetBleedRuntimeState) {
			handlers.resetBleedRuntimeState();
		}
		if (handlers.clearBridgeAliases) {
			handlers.clearBridgeAliases(why);
		}
		if (handlers.clearLastAggressor) {
			handlers.clearLastAggressor();
		}
		if (handlers.beginCaptiveFlow) {
			handlers.beginCaptiveFlow();
		}
		if (handlers.clearPendingCinematicFadeIn) {
			handlers.clearPendingCinematicFadeIn();
		}
		if (handlers.queueCaptiveFadeTransition && handlers.queueCaptiveFadeTransition()) {
			return true;
		}
		if (handlers.showBlackoutFader) {
			handlers.showBlackoutFader();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(300));

		bool completed = false;
		if (handlers.completeCaptiveTransitionNow) {
			handlers.completeCaptiveTransitionNow("captive_blackout_fallback");
			completed = true;
		}
		else {
			completed = directCaptiveBlackout();
		}

		if (completed) {
			ReassertCaptiveIdleFlowAfterBlackout(recaptureActorID, why);
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		if (handlers.hideBlackoutFader) {
			handlers.hideBlackoutFader();
		}
		return completed;
	}

	bool BeginWindow(RE::Actor* speaker, const char* reason)
	{
		const auto speakerFormID = ActorFormID(speaker);
		const char* why = reason ? reason : "bleed_window_start";
		TFD::ForceGreetState::ResetBleedout();
		spdlog::info("[TFD][Bleedout][P32L] BleedoutFGState reset for new window actor={:08X} reason={}", speakerFormID, why);

		auto& flow = TFD::FlowController::Controller::GetSingleton();
		const bool ok = flow.BeginPlayerBleedoutDecision(speakerFormID, why);
		spdlog::info("[TFD][Bleedout] BeginWindow actor={:08X} ok={} reason={}", speakerFormID, ok ? 1 : 0, why);
		return ok;
	}

	bool ResolvePay(RE::Actor* actor, const char* reason)
	{
		auto& flow = TFD::FlowController::Controller::GetSingleton();
		const auto actorFormID = ActorFormID(actor);
		if (!flow.ResolveBleedoutOutcome(TFD::FlowController::BleedoutOutcome::Pay, actorFormID, reason ? reason : "bleedout_pay")) {
			return false;
		}
		return flow.CompleteTerminalContext(reason ? reason : "bleedout_pay");
	}

	bool ResolvePleasure(RE::Actor* actor, const char* reason)
	{
		auto& flow = TFD::FlowController::Controller::GetSingleton();
		return flow.ResolveBleedoutOutcome(TFD::FlowController::BleedoutOutcome::Pleasure, ActorFormID(actor), reason ? reason : "bleedout_pleasure");
	}

	bool ResolveCaptive(RE::Actor* actor, const char* reason)
	{
		if (IsCaptiveEscapeRebleedFlow() || TFD::Captive::IsEscapeBleedoutActive()) {
			const bool committed = TFD::Captive::CommitRecapture(actor, reason ? reason : "bleedout_captive_recapture");
			if (committed) {
				ForceStopBleedRuntimeForCaptiveRecapture("bleedout_captive_recapture");
			}
			return committed;
		}
		auto& flow = TFD::FlowController::Controller::GetSingleton();
		return flow.ResolveBleedoutOutcome(TFD::FlowController::BleedoutOutcome::Captive, ActorFormID(actor), reason ? reason : "bleedout_captive");
	}

	bool CommitDialogueOutcome(DialogueOutcome outcome, RE::Actor* actor, const char* reason)
	{
		switch (outcome) {
		case DialogueOutcome::PayRelease:
			return ResolvePay(actor, reason ? reason : "bleedout_pay");
		case DialogueOutcome::Pleasure:
			return ResolvePleasure(actor, reason ? reason : "bleedout_pleasure");
		case DialogueOutcome::Captive:
			return ResolveCaptive(actor, reason ? reason : "bleedout_captive");
		default:
			break;
		}
		return false;
	}

	bool BeginAfterPleasure(RE::Actor* actor, const char* reason)
	{
		ApplyBleedoutDialogueFactionsInternal(actor, "afterpleasure_speaker", reason ? reason : "bleedout_after_pleasure");
		auto& flow = TFD::FlowController::Controller::GetSingleton();
		return flow.BeginAfterPleasure(ActorFormID(actor), reason ? reason : "bleedout_after_pleasure");
	}

	bool CompleteAfterPleasure(const char* reason)
	{
		auto& flow = TFD::FlowController::Controller::GetSingleton();
		const bool ok = flow.CompleteAfterPleasure(reason ? reason : "bleedout_after_pleasure_complete");
		ClearBleedoutDialogueFactionsInternal(reason ? reason : "bleedout_after_pleasure_complete");
		return ok;
	}

	void FinalizePleasureBridgeToInCombat(const char* reason)
	{
		const char* why = reason && reason[0] ? reason : "bleedout_pleasure_bridge_to_incombat_finalize";
		ClearBleedoutDialogueFactionsPreservePacifyInternal(why);

		bool released = false;
		const auto oldSessionId = g_truceSessionId;
		if (g_truceSessionId != 0) {
			released = TFD::HostilityController::ReleaseBleedoutSuppressSession(
				g_truceSessionId,
				TFD::HostilityController::ReleaseReason::FlowHandoff,
				why);
			g_truceSessionId = 0;
		}

		g_bleedSpeakerId = 0;
		g_bleedDialogueRetryCount = 0;
		g_bleedPendingCaptiveOutcome = false;
		g_bleedPendingNonCaptiveOutcome = false;
		g_bleedBattleObservePending = false;
		g_bleedBattleObserveActive = false;
		ResetBleedSpeakerKick();
		ClearBleedCrowdAssigned();
		ClearBleedRejectedSpeakerIds();

		spdlog::info(
			"[TFD][Bleedout][R471A] pleasure bridge finalized to InCombat oldSession={} released={} reason={}",
			oldSessionId,
			released ? 1 : 0,
			why);
	}

	bool IsPleasureCycleDialogueCandidate(RE::Actor* actor, RE::Actor* currentActor)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !actor || actor == player || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
			return false;
		}

		const auto actorID = actor->GetFormID();
		if (currentActor && actorID == currentActor->GetFormID()) {
			return false;
		}
		if (!HasBleedCrowdAssignedID(actorID)) {
			spdlog::info("[TFD][Bleedout][CB07] pleasure cycle candidate rejected actor={:08X} reason=not_locked_bleed_crowd", actorID);
			return false;
		}

		auto handlers = TFD::Bleedout::RuntimeHost::BuildHandlers();
		if (!IsBleedoutDialogueCrowdActor(actor, &handlers)) {
			return false;
		}

		if (handlers.isBleedSpaceCompatible && !handlers.isBleedSpaceCompatible(actor, player)) {
			return false;
		}

		return true;
	}

	bool CompletePleasureCycleChainNeutral(const char* reason)
	{
		const char* why = reason && reason[0] ? reason : "bleedout_pleasure_cycle_chain_neutral";

		auto& flow = TFD::FlowController::Controller::GetSingleton();
		const auto before = flow.GetSnapshot();
		const bool afterComplete = flow.RequestCompleteAfterPleasure(why);
		if (!afterComplete) {
			flow.ResetRuntime(why);
		}

		TFD::InteractionRouter::DialogueOpen::Cancel();
		TFD::InteractionRouter::ClearInteractionStateValue();
		TFD::BleedoutGreet::ResetRuntime(why);
		ClearBleedoutDialogueFactionsInternal(why);
		ClearBridgeAliases(nullptr, why);

		if (g_truceSessionId != 0) {
			const bool released = TFD::HostilityController::ReleaseBleedoutSuppressSession(
				g_truceSessionId,
				TFD::HostilityController::ReleaseReason::FlowHandoff,
				why);
			spdlog::info(
				"[TFD][Bleedout][P14OWN] bleed suppress released at chain neutral id={} released={} reason={}",
				g_truceSessionId,
				released ? 1 : 0,
				why);
			g_truceSessionId = 0;
		}

		g_bleedSpeakerId = 0;
		g_bleedDialogueRetryCount = 0;
		g_bleedPendingCaptiveOutcome = false;
		g_bleedPendingNonCaptiveOutcome = false;
		g_bleedBattleObservePending = false;
		g_bleedBattleObservePendingEmptyEnemyTicks = 0;
		g_bleedBattleObservePendingEmptyAllyTicks = 0;
		g_bleedBattleObserveActive = false;
		g_bleedBattleObserveActiveEmptyEnemyTicks = 0;
		g_bleedBattleObserveActiveEmptyAllyTicks = 0;
		g_inBleedState.store(false, std::memory_order_release);
		ResetBleedSpeakerKick();
		ClearBleedCrowdAssigned();
		ClearBleedRejectedSpeakerIds();

		auto setNeutralGlobal = [](const char* editorID) {
			if (auto* global = RE::TESForm::LookupByEditorID<RE::TESGlobal>(editorID)) {
				global->value = 0.0f;
			}
		};
		setNeutralGlobal("TFDPreCombatState");
		setNeutralGlobal("TFDInCombatState");
		setNeutralGlobal("TFDDefeatState");
		setNeutralGlobal("TFDPleasureState");
		setNeutralGlobal("TFDCaptiveState");
		
		spdlog::info(
			"[TFD][Bleedout][R133] pleasure cycle chain neutralized reason={} completeAfter={} forcedReset={} oldRoot={} oldCtx={} oldGate={} oldSub={} oldTerminal={} oldPrimary={:08X}",
			why,
			afterComplete ? 1 : 0,
			afterComplete ? 0 : 1,
			TFD::FlowController::Controller::ToString(before.root),
			TFD::FlowController::Controller::ToString(before.contextRoot),
			TFD::FlowController::Controller::ToString(before.gate),
			TFD::FlowController::Controller::ToString(before.sub),
			before.terminalResolved ? 1 : 0,
			before.primaryActorFormID);

		return true;
	}

	bool BeginForPleasureCycleActor(RE::Actor* actor, TFD::InteractionRouter::Action* outAction)
	{
		if (outAction) {
			*outAction = TFD::InteractionRouter::Action::None;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !actor || actor == player || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
			spdlog::info(
				"[TFD][Bleedout][R132] BeginForPleasureCycleActor blocked actor={:08X} reason=invalid_candidate",
				actor ? actor->GetFormID() : 0u);
			return false;
		}

		auto handlers = TFD::Bleedout::RuntimeHost::BuildHandlers();
		if (!IsBleedoutDialogueCrowdActor(actor, &handlers)) {
			spdlog::info(
				"[TFD][Bleedout][R132] BeginForPleasureCycleActor blocked actor={:08X} reason=not_dialogue_crowd",
				actor->GetFormID());
			return false;
		}
		if (handlers.isBleedSpaceCompatible && !handlers.isBleedSpaceCompatible(actor, player)) {
			spdlog::info(
				"[TFD][Bleedout][R132] BeginForPleasureCycleActor blocked actor={:08X} reason=space_incompatible",
				actor->GetFormID());
			return false;
		}

		// R132: this is a Bleedout-source dialogue cycle, not a new physical
		// defeat. The player has already recovered for OStim/AfterPleasure.
		// Do not call HostilityController::PromoteTruceActorForCycle here: that
		// sends TFDInCombatAssign and starts TFDInCombatSession, producing the
		// mixed state seen in logs: root=Bleedout but globals/HUD=Incombat.
		// Also do not call StartWindow(), because it restarts the full physical
		// bleedout runtime/animation. Arm only the Bleedout dialogue owner and
		// open the Bleedout root natively.
		TFD::InteractionRouter::DialogueOpen::Cancel();
		TFD::BleedoutGreet::ResetRuntime("pleasure_cycle_bleedout_dialogue_reset");

		const bool flowAccepted = BeginWindow(actor, "pleasure_cycle_bleedout_dialogue_begin");
		if (!flowAccepted) {
			spdlog::info(
				"[TFD][Bleedout][R132] BeginForPleasureCycleActor blocked actor={:08X} reason=flow_rejected",
				actor->GetFormID());
			return false;
		}

		if (player->IsInCombat()) {
			player->StopCombat();
		}
		if (actor->IsInCombat()) {
			actor->StopCombat();
		}
		// R250A: no forced weapon stance reset for Bleedout-source pleasure cycles.

		AssignBridgeActor(actor);
		PrimeBridgeActor(actor, "pleasure_cycle_bleedout_dialogue_begin");
		const bool dialogueRequested = TFD::BleedoutGreet::Begin(actor, "pleasure_cycle_bleedout_dialogue_begin");
		if (!dialogueRequested) {
			spdlog::info(
				"[TFD][Bleedout][R132] BeginForPleasureCycleActor blocked actor={:08X} reason=dialogue_begin_failed",
				actor->GetFormID());
			return false;
		}

		if (outAction) {
			*outAction = TFD::InteractionRouter::Action::None;
		}

		spdlog::info(
			"[TFD][Bleedout][R132] BeginForPleasureCycleActor actor={:08X} action=BleedoutDialogueOnly flowAccepted={} dialogueRequested=1 noInCombatAssign=1 noPhysicalBleedWindow=1",
			actor->GetFormID(),
			flowAccepted ? 1 : 0);
		return true;
	}


	bool TryContinueAfterPleasureCrowd(RE::Actor* consumedActor, const char* reason, bool recruitChoice)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !consumedActor) {
			return false;
		}

		auto handlers = TFD::Bleedout::RuntimeHost::BuildHandlers();
		std::vector<RE::Actor*> candidates = TFD::HostilityController::CollectDialogueTruceActors(consumedActor);
		if (candidates.empty()) {
			candidates = TFD::HostilityController::CollectActiveTruceActors(consumedActor);
		}

		RE::Actor* nextSpeaker = nullptr;
		std::unordered_set<std::uint32_t> seen;
		const auto consumedID = consumedActor->GetFormID();
		for (auto* candidate : candidates) {
			if (!candidate || candidate == player) {
				continue;
			}
			const auto candidateID = candidate->GetFormID();
			if (candidateID == 0 || candidateID == consumedID || !seen.insert(candidateID).second) {
				continue;
			}
			if (!IsBleedoutDialogueCrowdActor(candidate, &handlers)) {
				spdlog::info("[TFD][Bleedout][R129] skip afterpleasure cycle candidate actor={:08X} reason=not_dialogue_crowd", candidateID);
				continue;
			}
			if (handlers.isBleedSpaceCompatible && !handlers.isBleedSpaceCompatible(candidate, player)) {
				spdlog::info("[TFD][Bleedout][R129] skip afterpleasure cycle candidate actor={:08X} reason=space_incompatible", candidateID);
				continue;
			}
			nextSpeaker = candidate;
			break;
		}

		if (!nextSpeaker) {
			spdlog::info("[TFD][Bleedout][R129] no bleed crowd continuation candidate consumed={:08X} candidates={} reason={}",
				consumedID,
				static_cast<unsigned int>(candidates.size()),
				reason ? reason : "unknown");
			return false;
		}

		bool releasedSingle = false;
		bool demoteHold = false;
		if (recruitChoice) {
			demoteHold = TFD::HostilityController::DemoteTruceActorForCycleHold(
				consumedActor,
				"bleedout_after_pleasure_recruit_cycle_hold");
		}
		else {
			releasedSingle = TFD::HostilityController::ReleaseSingleTruceActorForCycle(
				consumedActor,
				TFD::HostilityController::ReleaseReason::FlowHandoff,
				reason ? reason : "bleedout_after_pleasure_cycle_next");
		}

		auto& flow = TFD::FlowController::Controller::GetSingleton();
		const bool oldFlowComplete = flow.RequestCompleteAfterPleasure("bleedout_after_pleasure_cycle_preserve_crowd");
		TFD::InteractionRouter::ClearInteractionStateValue();
		QueueAfterPleasureCrowdContinuation(
			consumedActor,
			nextSpeaker,
			reason ? reason : "unknown",
			recruitChoice,
			releasedSingle,
			demoteHold,
			oldFlowComplete);
		spdlog::info("[TFD][Bleedout][R129] queued afterpleasure next bleed crowd consumed={:08X} next={:08X} oldFlowComplete={} recruitChoice={} releasedSingle={} demoteHold={} reason={}",
			consumedID,
			nextSpeaker->GetFormID(),
			oldFlowComplete ? 1 : 0,
			recruitChoice ? 1 : 0,
			releasedSingle ? 1 : 0,
			demoteHold ? 1 : 0,
			reason ? reason : "unknown");
		return true;
	}

	bool TickQueuedAfterPleasureCrowdContinuation()
	{
		QueuedAfterPleasureCrowdContinuation queued{};
		{
			std::scoped_lock lk(g_afterPleasureCrowdContinuationLock);
			if (g_afterPleasureCrowdContinuation.nextSpeakerFormID == 0) {
				return false;
			}
			queued = g_afterPleasureCrowdContinuation;
			g_afterPleasureCrowdContinuation = {};
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* nextSpeaker = RE::TESForm::LookupByID<RE::Actor>(queued.nextSpeakerFormID);
		if (!player || !nextSpeaker || nextSpeaker->IsDead() || nextSpeaker->IsDisabled()) {
			spdlog::info("[TFD][Bleedout][R129] queued afterpleasure crowd continuation dropped next={:08X} player={} valid=0 reason={}",
				queued.nextSpeakerFormID,
				player ? 1 : 0,
				queued.reason.empty() ? std::string{ "unknown" } : queued.reason);
			return false;
		}

		TFD::InteractionRouter::Action action = TFD::InteractionRouter::Action::None;
		const bool began = BeginForPleasureCycleActor(nextSpeaker, &action);
		spdlog::info("[TFD][Bleedout][R132] continued afterpleasure to next bleed crowd consumed={:08X} next={:08X} oldFlowComplete={} recruitChoice={} releasedSingle={} demoteHold={} began={} action={} reason={}",
			queued.consumedActorFormID,
			queued.nextSpeakerFormID,
			queued.flowComplete ? 1 : 0,
			queued.recruitChoice ? 1 : 0,
			queued.releaseSingle ? 1 : 0,
			queued.demoteHold ? 1 : 0,
			began ? 1 : 0,
			TFD::InteractionRouter::ToString(action),
			queued.reason.empty() ? std::string{ "unknown" } : queued.reason);
		return began;
	}

	bool HandleAfterPleasureEnter(RE::Actor* actor, const char* reason)
	{
		return BeginAfterPleasure(actor, reason ? reason : "after_pleasure_enter");
	}

	void StartRuntimeWindow(RuntimeHostStateRefs state, RE::Actor* player, RE::Actor* aggressor, const RuntimeHostHandlers& handlers)
	{
		if (!player) {
			return;
		}

		const bool observeWindowQueued = SendBridgeModEvent("TFDBleedoutBattleObserveState", aggressor, "window_started", 0.0f);
		spdlog::info("[TFD][Bleedout][R33] battle observe dialogue guard cleared reason=start_bleed_window speaker={:08X} queued={}",
			aggressor ? aggressor->GetFormID() : 0u,
			observeWindowQueued ? 1 : 0);

		if (handlers.clearTerminalCommit) handlers.clearTerminalCommit("start_bleed_window");
		ClearBleedPleasureCrowdSnapshot("start_bleed_window");
		ClearBleedoutDialogueFactionsInternal("start_bleed_window_reset");
		TFD::Actor::Ops::ClearAggressorFactionContext();
		if (handlers.clearBridgeAliasesForActor) handlers.clearBridgeAliasesForActor(aggressor, "start_bleed_window");
		if (handlers.clearNoMarkerFallbackState) handlers.clearNoMarkerFallbackState();
		if (handlers.releaseNoSpeakerTameSession) handlers.releaseNoSpeakerTameSession("start_bleed_window");

		if (state.inBleedState) state.inBleedState->store(true, std::memory_order_release);
		if (handlers.resetGreetRuntime) handlers.resetGreetRuntime("bleed_reset");
		if (handlers.clearDialogueOutcome) handlers.clearDialogueOutcome("start_bleed_window");
		if (state.bleedPendingCaptiveOutcome) *state.bleedPendingCaptiveOutcome = false;
		if (state.bleedPendingNonCaptiveOutcome) *state.bleedPendingNonCaptiveOutcome = false;
		if (state.bleedStart) *state.bleedStart = Clock::now();
		if (state.bleedLastSeconds) *state.bleedLastSeconds = -1;
		if (state.bleedPaused) *state.bleedPaused = false;
		if (state.bleedPauseStarted) *state.bleedPauseStarted = {};
		if (state.bleedSpeakerKickLast) *state.bleedSpeakerKickLast = {};
		if (state.bleedSpeakerKickCount) *state.bleedSpeakerKickCount = 0;
		if (state.bleedDialogueRetryCount) *state.bleedDialogueRetryCount = 0;
		if (state.bleedLastCalmPulse) *state.bleedLastCalmPulse = {};
		if (state.bleedLastCrowdAssign) *state.bleedLastCrowdAssign = {};
		if (state.bleedCrowdAssigned) state.bleedCrowdAssigned->clear();
		if (handlers.resetBattleObserveTracking) handlers.resetBattleObserveTracking();
		if (state.bleedRejectedSpeakerIds) state.bleedRejectedSpeakerIds->clear();
		if (handlers.clearCaptorAliases) handlers.clearCaptorAliases("start_bleed_window_reset");
		if (handlers.resetBattleObserveTracking) handlers.resetBattleObserveTracking();
		if (state.bleedBattleObservePending) *state.bleedBattleObservePending = false;
		if (state.bleedBattleObservePendingUntil) *state.bleedBattleObservePendingUntil = {};
		if (state.bleedBattleObservePendingLastRedirect) *state.bleedBattleObservePendingLastRedirect = {};
		if (state.bleedBattleObservePendingEmptyEnemyTicks) *state.bleedBattleObservePendingEmptyEnemyTicks = 0;
		if (state.bleedBattleObservePendingEmptyAllyTicks) *state.bleedBattleObservePendingEmptyAllyTicks = 0;
		if (state.bleedBattleObserveActive) *state.bleedBattleObserveActive = false;
		if (state.bleedBattleObserveSince) *state.bleedBattleObserveSince = {};
		if (state.bleedBattleObserveLastRedirect) *state.bleedBattleObserveLastRedirect = {};
		if (state.bleedBattleObserveActiveEmptyEnemyTicks) *state.bleedBattleObserveActiveEmptyEnemyTicks = 0;
		if (state.bleedBattleObserveActiveEmptyAllyTicks) *state.bleedBattleObserveActiveEmptyAllyTicks = 0;

		const float maxHp = player->GetPermanentActorValue(RE::ActorValue::kHealth);
		const float minHp = (std::max)(1.0f, maxHp * 0.02f);
		if (state.minHp) *state.minHp = minHp;
		if (handlers.setPlayerBleedImmune) handlers.setPlayerBleedImmune(true);
		if (handlers.clampHealth) handlers.clampHealth(player, minHp);
		player->NotifyAnimationGraph("BleedoutStart");

		const float radius = (std::max)(12000.0f, TFD::Settings::GetSweepRadius());
		const float maxSpeakerDist = radius;
		aggressor = handlers.findBestSpeaker ? handlers.findBestSpeaker(radius, maxSpeakerDist, aggressor) : aggressor;
		if (aggressor) {
			float chosenDist = 99999.0f;
			if (handlers.isReasonableSpeaker) {
				handlers.isReasonableSpeaker(aggressor, player, maxSpeakerDist, &chosenDist);
			}
			spdlog::info("[TFD][Bleedout] bleed speaker locked {:08X} dist={:.1f}", aggressor->GetFormID(), chosenDist);
		}
		else {
			spdlog::info("[TFD][Bleedout] no local bleed speaker within {:.0f} -> hold without greet", maxSpeakerDist);
		}
		(void)BeginWindow(aggressor, "bleed_window_start");

		auto initialCrowd = CollectBleedoutCrowdUsingInCombatPattern(player, aggressor, radius, false, handlers);
		if (aggressor) {
			if (handlers.setLastAggressor) handlers.setLastAggressor(aggressor);
			if (state.bleedSpeakerId) *state.bleedSpeakerId = aggressor->GetFormID();

			bool autoStarted = false;
			if (handlers.startTruceSessionForSpeaker) {
				autoStarted = handlers.startTruceSessionForSpeaker(player, aggressor, "bleed_auto_forcegreet");
			}
			std::uint32_t primedCrowdCount = 0;
			if (state.bleedCrowdAssigned) {
				state.bleedCrowdAssigned->clear();
				const auto speakerID = aggressor->GetFormID();
				for (auto* crowdActor : initialCrowd) {
					if (!crowdActor || crowdActor->GetFormID() == speakerID) {
						continue;
					}
					state.bleedCrowdAssigned->push_back(crowdActor->GetFormID());
					PrimeBridgeCrowdActor(crowdActor, "bleed_auto_forcegreet_crowd");
					++primedCrowdCount;
				}
			}
			CapturePleasureCrowdSnapshotFromAssigned("bleed_auto_forcegreet");
			if (state.bleedLastCrowdAssign) *state.bleedLastCrowdAssign = Clock::now();
			if (autoStarted) {
				(void)ReleaseUnacceptedBleedoutTruceActors(aggressor, initialCrowd, "bleed_auto_forcegreet_restrict_truce");
			}
			HardStopBleedoutCombatPack(player, aggressor, initialCrowd, "bleed_auto_forcegreet_hard_stop", true);
			if (autoStarted) {
				if (handlers.resetGreetRuntime) handlers.resetGreetRuntime("bleed_auto_forcegreet");
				TFD::BleedoutGreet::Begin(aggressor, "bleed_auto_forcegreet");
			}
			else {
				spdlog::warn("[TFD][Bleedout][R100A] bleed auto forcegreet failed speaker={:08X} crowdCandidates={}",
					aggressor->GetFormID(),
					initialCrowd.size());
			}
			spdlog::info("[TFD][Bleedout][R105B] bleed speaker staged {:08X} crowdCandidates={} autoStarted={} crowdAssigned={} crowdPrimed={}",
				aggressor->GetFormID(),
				initialCrowd.size(),
				autoStarted ? 1 : 0,
				state.bleedCrowdAssigned ? state.bleedCrowdAssigned->size() : 0,
				primedCrowdCount);

			if (!handlers.isCaptiveSupportedAggressor || !handlers.isCaptiveSupportedAggressor(aggressor)) {
				if (state.bleedPendingCaptiveOutcome) *state.bleedPendingCaptiveOutcome = false;
				if (state.bleedPendingNonCaptiveOutcome) *state.bleedPendingNonCaptiveOutcome = true;
				spdlog::info("[TFD][Bleedout] aggressor {:08X} not captive-supported -> pending=noncaptive", aggressor->GetFormID());
			}
			else {
				const bool allowlistSupported = handlers.applyAllowedFactionFromAggressor ? handlers.applyAllowedFactionFromAggressor(aggressor) : false;
				TFD::Actor::Ops::ClearAggressorFactionContext();
				spdlog::info("[TFD][Bleedout][R103] JoinEnemy display cleared after captive suitability check actor={:08X} allowlistSupported={}",
					aggressor->GetFormID(),
					allowlistSupported ? 1 : 0);
				const bool hasCaptiveOutcome = handlers.resolveCaptiveMarkerForOutcome ? handlers.resolveCaptiveMarkerForOutcome() : false;
				float fallbackDistance = 99999.0f;
				const bool fallbackSupported = !allowlistSupported && handlers.canUseCaptiveFallbackHeuristic &&
					handlers.canUseCaptiveFallbackHeuristic(player, aggressor, hasCaptiveOutcome, &fallbackDistance);

				if (!allowlistSupported && !fallbackSupported) {
					if (state.bleedPendingCaptiveOutcome) *state.bleedPendingCaptiveOutcome = false;
					if (state.bleedPendingNonCaptiveOutcome) *state.bleedPendingNonCaptiveOutcome = true;
					spdlog::info("[TFD][Bleedout] aggressor {:08X} fallback rejected (marker={} dist={:.1f}) -> pending=noncaptive",
						aggressor->GetFormID(), hasCaptiveOutcome ? 1 : 0, fallbackDistance);
				}
				else {
					if (state.bleedPendingCaptiveOutcome) *state.bleedPendingCaptiveOutcome = hasCaptiveOutcome;
					if (state.bleedPendingNonCaptiveOutcome) *state.bleedPendingNonCaptiveOutcome = !hasCaptiveOutcome;
					float greetDistance = 99999.0f;
					const bool greetableNow = handlers.canUseAggressorForBleedoutGreet &&
						handlers.canUseAggressorForBleedoutGreet(player, aggressor, &greetDistance);
					if (!greetableNow) {
						spdlog::info("[TFD][Bleedout] aggressor {:08X} not greetable now dist={:.1f}", aggressor->GetFormID(), greetDistance);
					}
				}
			}
		}
		else {
			const bool hasCaptiveOutcome = handlers.resolveCaptiveMarkerForOutcome ? handlers.resolveCaptiveMarkerForOutcome() : false;
			if (state.bleedPendingCaptiveOutcome) *state.bleedPendingCaptiveOutcome = hasCaptiveOutcome;
			if (state.bleedPendingNonCaptiveOutcome) *state.bleedPendingNonCaptiveOutcome = !hasCaptiveOutcome;
			const bool tameHeld = handlers.tryEnsureNoSpeakerTameSession ? handlers.tryEnsureNoSpeakerTameSession(initialCrowd, "start_bleed_window_no_speaker") : false;
			spdlog::info("[TFD][Bleedout] no speaker -> pending={} tameHeld={}", hasCaptiveOutcome ? "captive" : "noncaptive", tameHeld ? 1 : 0);
		}

		const int bleedSeconds = TFD::Settings::GetBleedWindowSeconds();
		if (IsCaptiveEscapeRebleedFlow()) {
			constexpr int kCaptiveRecaptureFallbackSeconds = 6;
			const int elapsedOffset = (std::max)(0, bleedSeconds - kCaptiveRecaptureFallbackSeconds);
			if (state.bleedStart) {
				*state.bleedStart = Clock::now() - std::chrono::seconds(elapsedOffset);
			}
			if (state.bleedLastSeconds) {
				*state.bleedLastSeconds = -1;
			}
			spdlog::info("[TFD][Bleedout] captive escape-failed recapture window started fallbackSeconds={} baseSeconds={} notification=0",
				kCaptiveRecaptureFallbackSeconds,
				bleedSeconds);
		}
		else {
			char msg[96]{};
			std::snprintf(msg, sizeof(msg), "TFDEngine: Bleeding... (%ds)", bleedSeconds);
			if (handlers.debugNotification) handlers.debugNotification(msg);
			spdlog::info("[TFD][Bleedout] bleed window started ({}s)", bleedSeconds);
		}
	}

	bool StartRuntimeBattleObservePending(RuntimeHostStateRefs state, RE::Actor* player, const RuntimeHostHandlers& handlers)
	{
		if (!player) {
			return false;
		}
		if (handlers.clearTerminalCommit) handlers.clearTerminalCommit("start_bleed_observe_pending");
		ClearBleedoutDialogueFactionsInternal("start_bleed_observe_pending_reset");
		TFD::Actor::Ops::ClearAggressorFactionContext();
		if (handlers.clearBridgeAliasesForActor) handlers.clearBridgeAliasesForActor(nullptr, "start_bleed_observe_pending");
		if (handlers.clearNoMarkerFallbackState) handlers.clearNoMarkerFallbackState();
		if (handlers.releaseTruceSession) handlers.releaseTruceSession();
		if (handlers.releaseNoSpeakerTameSession) handlers.releaseNoSpeakerTameSession("start_bleed_observe_pending");
		if (handlers.clearBleedSupportBridgeAliases) handlers.clearBleedSupportBridgeAliases("start_bleed_observe_pending");

		if (state.inBleedState) state.inBleedState->store(true, std::memory_order_release);
		if (handlers.resetGreetRuntime) handlers.resetGreetRuntime("bleed_reset");
		if (state.bleedPendingCaptiveOutcome) *state.bleedPendingCaptiveOutcome = false;
		if (state.bleedPendingNonCaptiveOutcome) *state.bleedPendingNonCaptiveOutcome = false;
		if (state.bleedStart) *state.bleedStart = Clock::now();
		if (state.bleedLastSeconds) *state.bleedLastSeconds = -1;
		if (state.bleedPaused) *state.bleedPaused = false;
		if (state.bleedPauseStarted) *state.bleedPauseStarted = {};
		if (state.bleedSpeakerKickLast) *state.bleedSpeakerKickLast = {};
		if (state.bleedSpeakerKickCount) *state.bleedSpeakerKickCount = 0;
		if (state.bleedDialogueRetryCount) *state.bleedDialogueRetryCount = 0;
		if (state.bleedLastCalmPulse) *state.bleedLastCalmPulse = {};
		if (state.bleedLastCrowdAssign) *state.bleedLastCrowdAssign = {};
		if (state.bleedCrowdAssigned) state.bleedCrowdAssigned->clear();
		if (state.bleedBattleObservePending) *state.bleedBattleObservePending = true;
		if (state.bleedBattleObservePendingUntil) *state.bleedBattleObservePendingUntil = Clock::now() + std::chrono::milliseconds(1800);
		const bool observePendingQueued = SendBridgeModEvent("TFDBleedoutBattleObserveState", nullptr, "pending", 1.0f);
		spdlog::info("[TFD][Bleedout][R33] battle observe dialogue guard armed reason=start_bleed_observe_pending queued={}",
			observePendingQueued ? 1 : 0);
		if (state.bleedBattleObservePendingLastRedirect) *state.bleedBattleObservePendingLastRedirect = {};
		if (state.bleedBattleObservePendingEmptyEnemyTicks) *state.bleedBattleObservePendingEmptyEnemyTicks = 0;
		if (state.bleedBattleObservePendingEmptyAllyTicks) *state.bleedBattleObservePendingEmptyAllyTicks = 0;
		if (state.bleedBattleObserveActive) *state.bleedBattleObserveActive = false;
		if (state.bleedBattleObserveSince) *state.bleedBattleObserveSince = {};
		if (state.bleedBattleObserveLastRedirect) *state.bleedBattleObserveLastRedirect = {};
		if (state.bleedBattleObserveActiveEmptyEnemyTicks) *state.bleedBattleObserveActiveEmptyEnemyTicks = 0;
		if (state.bleedBattleObserveActiveEmptyAllyTicks) *state.bleedBattleObserveActiveEmptyAllyTicks = 0;

		const float maxHp = player->GetPermanentActorValue(RE::ActorValue::kHealth);
		const float minHp = (std::max)(1.0f, maxHp * 0.02f);
		if (state.minHp) *state.minHp = minHp;
		if (handlers.setPlayerBleedImmune) handlers.setPlayerBleedImmune(true);
		if (handlers.clampHealth) handlers.clampHealth(player, minHp);
		player->NotifyAnimationGraph("BleedoutStart");

		const float immediateRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 600.0f);
		auto immediateFollowers = handlers.collectBleedStandingFollowers ? handlers.collectBleedStandingFollowers(immediateRadius) : std::vector<RE::Actor*>{};
		const float immediateEnemyScanRadius = handlers.computeBleedBattleEnemyScanRadius ? handlers.computeBleedBattleEnemyScanRadius(player, immediateFollowers, immediateRadius) : immediateRadius;
		auto* immediatePreferredEnemy = handlers.resolveAggressor ? handlers.resolveAggressor() : nullptr;
		if (!immediatePreferredEnemy && handlers.resolveLastEnemyTargetingPlayer) {
			immediatePreferredEnemy = handlers.resolveLastEnemyTargetingPlayer(immediateEnemyScanRadius, 15.0);
		}
		if (!immediatePreferredEnemy && handlers.findBestAggressor) {
			immediatePreferredEnemy = handlers.findBestAggressor(immediateEnemyScanRadius);
		}
		if (immediatePreferredEnemy && handlers.isObserverAlly && handlers.isObserverAlly(immediatePreferredEnemy)) {
			immediatePreferredEnemy = nullptr;
		}
		auto immediateEnemies = handlers.collectCurrentObservedEnemies ? handlers.collectCurrentObservedEnemies(player, immediateRadius, immediatePreferredEnemy, immediateFollowers) : std::vector<RE::Actor*>{};
		if (handlers.updateObserverRoster) handlers.updateObserverRoster(player, immediateFollowers, immediateEnemies, immediatePreferredEnemy);
		auto immediateRosterEnemies = handlers.collectStandingEnemiesFromSnapshot ? handlers.collectStandingEnemiesFromSnapshot() : std::vector<RE::Actor*>{};
		auto& immediateResolvedEnemies = immediateRosterEnemies.empty() ? immediateEnemies : immediateRosterEnemies;
		if (!immediateFollowers.empty() && !immediateResolvedEnemies.empty() && state.bleedBattleObservePendingLastRedirect) {
			*state.bleedBattleObservePendingLastRedirect = Clock::now();
		}

		const int bleedSeconds = TFD::Settings::GetBleedWindowSeconds();
		char msg[96]{};
		std::snprintf(msg, sizeof(msg), "TFDEngine: Bleeding... allies fighting (%ds)", bleedSeconds);
		if (handlers.debugNotification) handlers.debugNotification(msg);
		spdlog::info("[TFD][Bleedout] battle observe pending started");
		return true;
	}


	RE::TESFaction* LookupBattleObserveFactionByEditorID(const char* editorId)
	{
		if (!editorId || !editorId[0]) {
			return nullptr;
		}
		return RE::TESForm::LookupByEditorID<RE::TESFaction>(editorId);
	}

	void ResolveBattleObserveCombatBehaviorFactions()
	{
		if (g_battleObserveCombatBehaviorFactionCache.tried) {
			return;
		}

		g_battleObserveCombatBehaviorFactionCache.tried = true;
		g_battleObserveCombatBehaviorFactionCache.retarget = LookupBattleObserveFactionByEditorID("TFDRetargetFaction");
		g_battleObserveCombatBehaviorFactionCache.threat = LookupBattleObserveFactionByEditorID("TFDCombatThreatFaction");

		spdlog::info(
			"[TFD][Bleedout][R466A] battle observe combat-behavior factions retarget={} threat={}",
			g_battleObserveCombatBehaviorFactionCache.retarget ? 1 : 0,
			g_battleObserveCombatBehaviorFactionCache.threat ? 1 : 0);
	}

	bool IsCombatBehaviorMarkedBleedoutThreat(RE::Actor* actor)
	{
		if (!actor) {
			return false;
		}

		if (TFD::CombatBehavior::IsActiveCombatThreat(actor)) {
			return true;
		}

		ResolveBattleObserveCombatBehaviorFactions();
		if (g_battleObserveCombatBehaviorFactionCache.retarget && actor->IsInFaction(g_battleObserveCombatBehaviorFactionCache.retarget)) {
			return true;
		}
		if (g_battleObserveCombatBehaviorFactionCache.threat && actor->IsInFaction(g_battleObserveCombatBehaviorFactionCache.threat)) {
			return true;
		}

		return false;
	}

	float RuntimeActorHealthPct(RE::Actor* actor)
	{
		if (!actor) {
			return 0.0f;
		}
		const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
		return (actor->GetActorValue(RE::ActorValue::kHealth) / hpMax) * 100.0f;
	}

	bool IsRuntimeAboveDefeatThreshold(RE::Actor* actor)
	{
		if (!actor) {
			return false;
		}
		if (actor->GetActorValue(RE::ActorValue::kHealth) <= 0.0f) {
			return false;
		}
		const float thresholdPct = std::clamp(TFD::Settings::GetDefeatThresholdPct(), 1.0f, 95.0f);
		return RuntimeActorHealthPct(actor) > thresholdPct;
	}

	bool IsRuntimeBattleObserveStandingActor(RE::Actor* actor, RE::Actor* player)
	{
		if (!actor || actor == player || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
			return false;
		}
		auto* actorState = actor->AsActorState();
		if (actorState && actorState->IsBleedingOut()) {
			return false;
		}
		if (TFD::DefeatMonitor::IsThresholdDownedActor(actor)) {
			return false;
		}
		return IsRuntimeAboveDefeatThreshold(actor);
	}

	bool IsRuntimeBattleObserveEnemyThreat(RE::Actor* actor, RE::Actor* player, const RuntimeHostHandlers& handlers)
	{
		if (!IsRuntimeBattleObserveStandingActor(actor, player)) {
			return false;
		}
		if (handlers.isObserverAlly && handlers.isObserverAlly(actor)) {
			return false;
		}
		if (handlers.isStandingEnemyThresholdActor && !handlers.isStandingEnemyThresholdActor(actor)) {
			return false;
		}
		if (handlers.isBleedSpaceCompatible && player && !handlers.isBleedSpaceCompatible(actor, player)) {
			return false;
		}
		return true;
	}

	float RuntimeBattleObserveMinSideDistSq(RE::Actor* actor, RE::Actor* player, const std::vector<RE::Actor*>& followers);

	bool IsRuntimeBattleObserveBlockingEnemy(
		RE::Actor* actor,
		RE::Actor* player,
		const std::vector<RE::Actor*>& followers,
		RE::Actor* preferredEnemy,
		const RuntimeHostHandlers& handlers)
	{
		if (!IsRuntimeBattleObserveStandingActor(actor, player)) {
			return false;
		}
		if (handlers.isObserverAlly && handlers.isObserverAlly(actor)) {
			return false;
		}
		if (handlers.isStandingEnemyThresholdActor && !handlers.isStandingEnemyThresholdActor(actor)) {
			return false;
		}

		// R15/P33L: CombatBehavior threat marks are hard proof after the player side is gone,
		// and they are also a temporary win-blocker while standing followers remain.
		// Otherwise TFD can suppress an enemy off the downed player, erase its target/combat
		// posture, then immediately misread that same standing enemy as defeated and teleport
		// Rescue while combatants are still upright. The mark is still TTL-owned by
		// CombatBehavior; once it clears, this guard naturally stops blocking.
		const bool combatBehaviorThreat = IsCombatBehaviorMarkedBleedoutThreat(actor);
		if (combatBehaviorThreat && followers.empty()) {
			return true;
		}

		auto* target = TFD::Actor::GetCurrentTarget(actor);
		if (target == player) {
			return true;
		}

		bool hostileToPlayerSide = player && actor->IsHostileToActor(player);
		bool targetsPlayerSide = false;
		for (auto* follower : followers) {
			if (!IsRuntimeBattleObserveStandingActor(follower, player)) {
				continue;
			}
			if (target == follower) {
				targetsPlayerSide = true;
			}
			if (actor->IsHostileToActor(follower)) {
				hostileToPlayerSide = true;
			}
		}

		if (targetsPlayerSide) {
			return true;
		}

		constexpr float kCombatBehaviorWinBlockRadius = 5200.0f;
		if (combatBehaviorThreat && !followers.empty() &&
			RuntimeBattleObserveMinSideDistSq(actor, player, followers) <= kCombatBehaviorWinBlockRadius) {
			return true;
		}

		const bool combatPosture = actor->IsInCombat() || actor->IsWeaponDrawn();
		if (hostileToPlayerSide && combatPosture) {
			return true;
		}

		// CB08: preferred/roster enemies are not allowed to block recovery by merely
		// existing in the snapshot. They still need active combat posture or a target.
		if (actor == preferredEnemy && combatPosture) {
			return true;
		}

		return false;
	}

	bool ShouldLogRuntimeBattleObserveIgnored(RE::Actor* actor)
	{
		if (!actor) {
			return false;
		}

		const auto actorId = actor->GetFormID();
		const auto now = Clock::now();
		std::scoped_lock lk(g_battleObserveLogThrottleLock);

		if (g_battleObserveLogThrottle.ignoredActorLogs.size() > 96) {
			g_battleObserveLogThrottle.ignoredActorLogs.clear();
		}

		auto& last = g_battleObserveLogThrottle.ignoredActorLogs[actorId];
		if (last.time_since_epoch().count() != 0 && now - last < std::chrono::milliseconds(2200)) {
			return false;
		}
		last = now;
		return true;
	}

	bool ShouldLogRuntimeBattleObserveFilter(std::size_t before, std::size_t after, std::size_t followers)
	{
		const auto now = Clock::now();
		std::scoped_lock lk(g_battleObserveLogThrottleLock);

		const bool changed = before != g_battleObserveLogThrottle.lastFilterInput ||
			after != g_battleObserveLogThrottle.lastFilterOutput ||
			followers != g_battleObserveLogThrottle.lastFilterFollowers;
		const bool intervalElapsed = g_battleObserveLogThrottle.lastFilterLog.time_since_epoch().count() == 0 ||
			now - g_battleObserveLogThrottle.lastFilterLog >= std::chrono::milliseconds(1800);

		if (!changed && !intervalElapsed) {
			return false;
		}

		g_battleObserveLogThrottle.lastFilterLog = now;
		g_battleObserveLogThrottle.lastFilterInput = before;
		g_battleObserveLogThrottle.lastFilterOutput = after;
		g_battleObserveLogThrottle.lastFilterFollowers = followers;
		return true;
	}

	void ResetRuntimeBattleObserveLogThrottle()
	{
		std::scoped_lock lk(g_battleObserveLogThrottleLock);
		g_battleObserveLogThrottle = BattleObserveLogThrottleState{};
	}

	void FilterRuntimeBattleObserveEnemies(
		std::vector<RE::Actor*>& enemies,
		RE::Actor* player,
		const std::vector<RE::Actor*>& followers,
		RE::Actor* preferredEnemy,
		const RuntimeHostHandlers& handlers,
		const char* reason)
	{
		const auto before = enemies.size();
		enemies.erase(
			std::remove_if(enemies.begin(), enemies.end(), [&](RE::Actor* actor) {
				const bool keep = IsRuntimeBattleObserveBlockingEnemy(actor, player, followers, preferredEnemy, handlers);
				if (!keep && actor && ShouldLogRuntimeBattleObserveIgnored(actor)) {
					const bool cbThreat = IsCombatBehaviorMarkedBleedoutThreat(actor);
					spdlog::info(
						"[TFD][Bleedout][CB08][R16] observed enemy ignored for battle resolution actor={:08X} reason={} inCombat={} weaponDrawn={} target={:08X} cbThreat={} throttled=1",
						actor->GetFormID(),
						reason ? reason : "observe_resolution",
						actor->IsInCombat() ? 1 : 0,
						actor->IsWeaponDrawn() ? 1 : 0,
						TFD::Actor::GetCurrentTarget(actor) ? TFD::Actor::GetCurrentTarget(actor)->GetFormID() : 0u,
						cbThreat ? 1 : 0);
				}
				return !keep;
			}),
			enemies.end());
		if (before != enemies.size() && ShouldLogRuntimeBattleObserveFilter(before, enemies.size(), followers.size())) {
			spdlog::info(
				"[TFD][Bleedout][CB08][R16] observed enemy filter reason={} input={} output={} followers={} throttled=1",
				reason ? reason : "observe_resolution",
				static_cast<unsigned int>(before),
				static_cast<unsigned int>(enemies.size()),
				static_cast<unsigned int>(followers.size()));
		}

	}

	bool RuntimeActorVectorContains(const std::vector<RE::Actor*>& actors, RE::Actor* actor)
	{
		const auto id = actor ? actor->GetFormID() : 0u;
		if (id == 0) {
			return false;
		}
		return std::any_of(actors.begin(), actors.end(), [id](RE::Actor* candidate) {
			return candidate && candidate->GetFormID() == id;
		});
	}

	void AddRuntimeBattleObserveActor(std::vector<RE::Actor*>& out, RE::Actor* actor, RE::Actor* player)
	{
		if (!IsRuntimeBattleObserveStandingActor(actor, player) || RuntimeActorVectorContains(out, actor)) {
			return;
		}
		out.push_back(actor);
	}

	void MergeRuntimeBattleObserveActors(std::vector<RE::Actor*>& out, const std::vector<RE::Actor*>& in, RE::Actor* player)
	{
		for (auto* actor : in) {
			AddRuntimeBattleObserveActor(out, actor, player);
		}
	}

	bool IsRuntimeBattleObservePlayerSideTarget(RE::Actor* target, RE::Actor* player, const std::vector<RE::Actor*>& followers)
	{
		if (!target) {
			return false;
		}
		if (target == player) {
			return true;
		}
		return RuntimeActorVectorContains(followers, target);
	}

	bool IsRuntimeBattleObserveHostileToSide(RE::Actor* actor, RE::Actor* player, const std::vector<RE::Actor*>& followers)
	{
		if (!actor) {
			return false;
		}
		if (player && (actor->IsHostileToActor(player) || player->IsHostileToActor(actor))) {
			return true;
		}
		for (auto* follower : followers) {
			if (follower && (actor->IsHostileToActor(follower) || follower->IsHostileToActor(actor))) {
				return true;
			}
		}
		return false;
	}

	float RuntimeBattleObserveMinSideDistSq(RE::Actor* actor, RE::Actor* player, const std::vector<RE::Actor*>& followers)
	{
		float best = std::numeric_limits<float>::max();
		if (!actor) {
			return best;
		}
		if (player) {
			best = (std::min)(best, Distance3D(actor, player));
		}
		for (auto* follower : followers) {
			if (follower) {
				best = (std::min)(best, Distance3D(actor, follower));
			}
		}
		return best;
	}

	bool IsRuntimeBattleObserveDirectThreatSeed(
		RE::Actor* actor,
		RE::Actor* player,
		const std::vector<RE::Actor*>& followers,
		RE::Actor* preferredEnemy,
		const RuntimeHostHandlers& handlers)
	{
		(void)preferredEnemy;
		// R32: this is the hard proof gate used before BattleObserve is allowed
		// to resolve player-side win.  It intentionally does not require the actor
		// to survive the older observed-enemy roster path; Flow can see activeHostile
		// from a direct current target even when the roster preferred enemy is stale.
		if (!IsRuntimeBattleObserveEnemyThreat(actor, player, handlers)) {
			return false;
		}

		auto* target = ResolveCombatTargetForBleedoutCrowd(actor);
		if (IsRuntimeBattleObservePlayerSideTarget(target, player, followers)) {
			return true;
		}

		const bool hostileToSide = IsRuntimeBattleObserveHostileToSide(actor, player, followers);
		if (!hostileToSide) {
			return false;
		}

		constexpr float kDirectThreatProofRadius = 5200.0f;
		const float sideDistSq = RuntimeBattleObserveMinSideDistSq(actor, player, followers);
		const bool combatBehaviorThreat = IsCombatBehaviorMarkedBleedoutThreat(actor);
		if (combatBehaviorThreat && sideDistSq <= kDirectThreatProofRadius) {
			return true;
		}

		const bool combatPosture = actor->IsInCombat() || actor->IsWeaponDrawn();
		if (!combatPosture) {
			return false;
		}

		return sideDistSq <= kDirectThreatProofRadius;
	}

	std::uint32_t MergeRuntimeBattleObserveDirectThreatsFromSnapshot(
		std::vector<RE::Actor*>& enemies,
		RE::Actor* player,
		float radius,
		RE::Actor* preferredEnemy,
		const std::vector<RE::Actor*>& followers,
		const RuntimeHostHandlers& handlers,
		const char* reason)
	{
		if (!player) {
			return 0;
		}

		const float scanRadius = handlers.computeBleedBattleEnemyScanRadius ?
			handlers.computeBleedBattleEnemyScanRadius(player, followers, radius) :
			radius;
		TFD::Actor::ScanOptions options{};
		options.radius = (std::max)(scanRadius, 2400.0f);
		options.npcOnly = false;
		auto snapshot = TFD::Actor::BuildSnapshot(player, options);

		std::uint32_t merged = 0;
		for (const auto& info : snapshot.actors) {
			auto* actor = info.get();
			if (!actor || RuntimeActorVectorContains(enemies, actor)) {
				continue;
			}
			if (!IsRuntimeBattleObserveDirectThreatSeed(actor, player, followers, preferredEnemy, handlers)) {
				continue;
			}
			AddRuntimeBattleObserveActor(enemies, actor, player);
			++merged;
		}

		if (merged > 0) {
			spdlog::info(
				"[TFD][Bleedout][R32] direct battle-observe threat merged merged={} total={} followers={} reason={}",
				merged,
				static_cast<unsigned int>(enemies.size()),
				static_cast<unsigned int>(followers.size()),
				reason ? reason : "unknown");
		}
		return merged;
	}

	RE::Actor* ResolveRuntimeBattleObserveDirectBlockingThreat(
		RE::Actor* player,
		float radius,
		RE::Actor* preferredEnemy,
		const std::vector<RE::Actor*>& followers,
		const RuntimeHostHandlers& handlers,
		const char* reason)
	{
		std::vector<RE::Actor*> directThreats{};
		(void)MergeRuntimeBattleObserveDirectThreatsFromSnapshot(
			directThreats,
			player,
			radius,
			preferredEnemy,
			followers,
			handlers,
			reason);
		return directThreats.empty() ? nullptr : directThreats.front();
	}

	bool IsRuntimeBattleObserveActiveThreatActor(RE::Actor* actor, RE::Actor* player, const std::vector<RE::Actor*>& followers)
	{
		if (!IsRuntimeBattleObserveStandingActor(actor, player)) {
			return false;
		}
		// R15/P33L: the marker alone remains hard proof with no allies, and while
		// allies are standing it must still block immediate player-side win if the
		// marked enemy is still local. TFD just suppressed this target off the downed
		// player, so target/combat posture may be empty by design, not because the
		// enemy has been defeated.
		const bool combatBehaviorThreat = IsCombatBehaviorMarkedBleedoutThreat(actor);
		if (combatBehaviorThreat && followers.empty()) {
			return true;
		}
		constexpr float kCombatBehaviorActiveThreatRadius = 5200.0f;
		if (combatBehaviorThreat && !followers.empty() &&
			RuntimeBattleObserveMinSideDistSq(actor, player, followers) <= kCombatBehaviorActiveThreatRadius) {
			return true;
		}

		auto* target = ResolveCombatTargetForBleedoutCrowd(actor);
		if (IsRuntimeBattleObservePlayerSideTarget(target, player, followers)) {
			return true;
		}

		const bool hostileToSide = IsRuntimeBattleObserveHostileToSide(actor, player, followers);
		const bool inCombat = actor->IsInCombat();
		const bool weaponDrawn = actor->IsWeaponDrawn();
		if (inCombat && (hostileToSide || weaponDrawn)) {
			return true;
		}

		constexpr float kLocalProofRadius = 3000.0f;
		if (weaponDrawn && hostileToSide && RuntimeBattleObserveMinSideDistSq(actor, player, followers) <= kLocalProofRadius) {
			return true;
		}
		return false;
	}

	std::vector<RE::Actor*> FilterRuntimeBattleObserveActiveEnemies(
		const std::vector<RE::Actor*>& enemies,
		RE::Actor* player,
		const std::vector<RE::Actor*>& followers,
		const char* phase)
	{
		std::vector<RE::Actor*> active{};
		active.reserve(enemies.size());
		for (auto* enemy : enemies) {
			if (IsRuntimeBattleObserveActiveThreatActor(enemy, player, followers)) {
				AddRuntimeBattleObserveActor(active, enemy, player);
			}
		}

		if (active.size() != enemies.size()) {
			spdlog::info(
				"[TFD][Bleedout][CB08] battle observe active enemy filter phase={} enemies={} active={} followers={}",
				phase ? phase : "unknown",
				static_cast<unsigned int>(enemies.size()),
				static_cast<unsigned int>(active.size()),
				static_cast<unsigned int>(followers.size()));
		}
		return active;
	}

	std::vector<RE::Actor*> CollectRuntimeBattleObserveFollowers(float radius, RE::Actor* player, const RuntimeHostHandlers& handlers)
	{
		std::vector<RE::Actor*> followers{};
		if (handlers.collectStandingFollowersFromSnapshot) {
			MergeRuntimeBattleObserveActors(followers, handlers.collectStandingFollowersFromSnapshot(), player);
		}
		if (handlers.collectBleedStandingFollowers) {
			MergeRuntimeBattleObserveActors(followers, handlers.collectBleedStandingFollowers(radius), player);
		}
		if (followers.empty() && player) {
			auto snapshot = TFD::Actor::BuildSnapshot(player, TFD::Actor::ScanOptions{ radius, false });
			MergeRuntimeBattleObserveActors(followers, TFD::Actor::ResolveStandingPlayerSideActors(snapshot, false), player);
		}
		return followers;
	}

	std::vector<RE::Actor*> CollectRuntimeBattleObserveEnemies(
		RE::Actor* player,
		float radius,
		RE::Actor* preferredEnemy,
		const std::vector<RE::Actor*>& followers,
		const RuntimeHostHandlers& handlers,
		bool updateRoster)
	{
		std::vector<RE::Actor*> enemies{};
		if (handlers.collectCurrentObservedEnemies) {
			auto currentEnemies = handlers.collectCurrentObservedEnemies(player, radius, preferredEnemy, followers);
			if (updateRoster && handlers.updateObserverRoster) {
				handlers.updateObserverRoster(player, followers, currentEnemies, preferredEnemy);
			}
			MergeRuntimeBattleObserveActors(enemies, currentEnemies, player);
		}
		else if (updateRoster && handlers.updateObserverRoster) {
			handlers.updateObserverRoster(player, followers, std::vector<RE::Actor*>{}, preferredEnemy);
		}
		if (handlers.collectStandingEnemiesFromSnapshot) {
			MergeRuntimeBattleObserveActors(enemies, handlers.collectStandingEnemiesFromSnapshot(), player);
		}
		if (preferredEnemy) {
			AddRuntimeBattleObserveActor(enemies, preferredEnemy, player);
		}
		const std::uint32_t directMerged = MergeRuntimeBattleObserveDirectThreatsFromSnapshot(
			enemies,
			player,
			radius,
			preferredEnemy,
			followers,
			handlers,
			"collect_runtime_observe_enemies");
		if (updateRoster && directMerged > 0 && handlers.updateObserverRoster) {
			handlers.updateObserverRoster(player, followers, enemies, preferredEnemy);
		}
		FilterRuntimeBattleObserveEnemies(enemies, player, followers, preferredEnemy, handlers, "collect_runtime_observe_enemies");
		return enemies;
	}

	RE::Actor* ResolveRuntimeBattleObservePreferredEnemy(
		RE::Actor* player,
		float radius,
		const std::vector<RE::Actor*>& followers,
		const RuntimeHostHandlers& handlers)
	{
		const float enemyScanRadius = handlers.computeBleedBattleEnemyScanRadius ? handlers.computeBleedBattleEnemyScanRadius(player, followers, radius) : radius;
		auto* preferredEnemy = handlers.resolveAggressor ? handlers.resolveAggressor() : nullptr;
		if (!preferredEnemy && handlers.resolveLastEnemyTargetingPlayer) {
			preferredEnemy = handlers.resolveLastEnemyTargetingPlayer(enemyScanRadius, 15.0);
		}
		if (!preferredEnemy && handlers.findBestAggressor) {
			preferredEnemy = handlers.findBestAggressor(enemyScanRadius);
		}
		if (preferredEnemy && handlers.isObserverAlly && handlers.isObserverAlly(preferredEnemy)) {
			preferredEnemy = nullptr;
		}
		return preferredEnemy;
	}

	void ExtendRuntimeBattleObserveHold(RuntimeHostStateRefs state, Clock::time_point now, std::chrono::milliseconds duration)
	{
		if (state.bleedBattleObservePendingUntil) {
			*state.bleedBattleObservePendingUntil = now + duration;
		}
	}


	bool IsRuntimeBattleObserveSpeakerCandidate(RE::Actor* actor, RE::Actor* player, const RuntimeHostHandlers& handlers)
	{
		if (!actor || actor == player || actor->IsDead() || actor->IsDisabled()) {
			return false;
		}
		auto* actorState = actor->AsActorState();
		if (actorState && actorState->IsBleedingOut()) {
			return false;
		}
		if (actor->GetActorValue(RE::ActorValue::kHealth) <= 0.0f) {
			return false;
		}
		if (handlers.isObserverAlly && handlers.isObserverAlly(actor)) {
			return false;
		}
		if (handlers.isStandingEnemyThresholdActor && !handlers.isStandingEnemyThresholdActor(actor)) {
			return false;
		}
		if (handlers.isBleedSpaceCompatible && player && !handlers.isBleedSpaceCompatible(actor, player)) {
			return false;
		}
		return true;
	}

	void ClearRuntimeBattleObserveFlags(RuntimeHostStateRefs state)
	{
		if (state.bleedBattleObservePending) *state.bleedBattleObservePending = false;
		if (state.bleedBattleObservePendingUntil) *state.bleedBattleObservePendingUntil = {};
		if (state.bleedBattleObservePendingLastRedirect) *state.bleedBattleObservePendingLastRedirect = {};
		if (state.bleedBattleObservePendingEmptyEnemyTicks) *state.bleedBattleObservePendingEmptyEnemyTicks = 0;
		if (state.bleedBattleObservePendingEmptyAllyTicks) *state.bleedBattleObservePendingEmptyAllyTicks = 0;
		if (state.bleedBattleObserveActive) *state.bleedBattleObserveActive = false;
		if (state.bleedBattleObserveSince) *state.bleedBattleObserveSince = {};
		if (state.bleedBattleObserveLastRedirect) *state.bleedBattleObserveLastRedirect = {};
		if (state.bleedBattleObserveActiveEmptyEnemyTicks) *state.bleedBattleObserveActiveEmptyEnemyTicks = 0;
		if (state.bleedBattleObserveActiveEmptyAllyTicks) *state.bleedBattleObserveActiveEmptyAllyTicks = 0;
		ResetRuntimeBattleObserveLogThrottle();
	}

	RE::Actor* ResolveRuntimeBattleObserveLossSpeaker(
		RuntimeHostStateRefs state,
		RE::Actor* player,
		const std::vector<RE::Actor*>& followers,
		const std::vector<RE::Actor*>& knownEnemies,
		float radius,
		const RuntimeHostHandlers& handlers)
	{
		(void)state;
		if (!player) {
			return nullptr;
		}

		const float enemyScanRadius = handlers.computeBleedBattleEnemyScanRadius ? handlers.computeBleedBattleEnemyScanRadius(player, followers, radius) : radius;
		auto* preferredEnemy = handlers.resolveAggressor ? handlers.resolveAggressor() : nullptr;
		if (!preferredEnemy && handlers.resolveLastEnemyTargetingPlayer) {
			preferredEnemy = handlers.resolveLastEnemyTargetingPlayer(enemyScanRadius, 15.0);
		}
		if (!preferredEnemy && handlers.findBestAggressor) {
			preferredEnemy = handlers.findBestAggressor(enemyScanRadius);
		}
		if (!IsRuntimeBattleObserveSpeakerCandidate(preferredEnemy, player, handlers)) {
			preferredEnemy = nullptr;
		}

		auto enemies = handlers.collectCurrentObservedEnemies ? handlers.collectCurrentObservedEnemies(player, radius, preferredEnemy, followers) : std::vector<RE::Actor*>{};
		if (handlers.updateObserverRoster) {
			handlers.updateObserverRoster(player, followers, enemies, preferredEnemy);
		}
		auto rosterEnemies = handlers.collectStandingEnemiesFromSnapshot ? handlers.collectStandingEnemiesFromSnapshot() : std::vector<RE::Actor*>{};
		for (auto* known : knownEnemies) {
			if (known) {
				rosterEnemies.push_back(known);
			}
		}
		for (auto* observed : enemies) {
			if (observed) {
				rosterEnemies.push_back(observed);
			}
		}

		auto* speaker = handlers.findBestSpeaker ? handlers.findBestSpeaker(radius, 12000.0f, preferredEnemy) : preferredEnemy;
		if (IsRuntimeBattleObserveSpeakerCandidate(speaker, player, handlers)) {
			return speaker;
		}

		for (auto* candidate : rosterEnemies) {
			if (IsRuntimeBattleObserveSpeakerCandidate(candidate, player, handlers)) {
				return candidate;
			}
		}
		return nullptr;
	}

	bool StartRuntimeWindowFromBattleObserveLoss(
		RuntimeHostStateRefs state,
		RE::Actor* player,
		const std::vector<RE::Actor*>& followers,
		const std::vector<RE::Actor*>& knownEnemies,
		float radius,
		const RuntimeHostHandlers& handlers,
		const char* reason)
	{
		auto* speaker = ResolveRuntimeBattleObserveLossSpeaker(state, player, followers, knownEnemies, radius, handlers);
		if (!speaker) {
			return false;
		}
		if (handlers.setLastAggressor) {
			handlers.setLastAggressor(speaker);
		}
		const bool observeLossQueued = SendBridgeModEvent("TFDBleedoutBattleObserveState", speaker, "loss", 0.0f);
		spdlog::info("[TFD][Bleedout][R33] battle observe dialogue guard cleared reason=loss speaker={:08X} queued={}",
			speaker->GetFormID(),
			observeLossQueued ? 1 : 0);
		ClearRuntimeBattleObserveFlags(state);
		spdlog::info("[TFD][Bleedout][CB05] battle observe loss -> bleedout forcegreet speaker={:08X} reason={}",
			speaker->GetFormID(),
			reason ? reason : "battle_observe_loss");
		StartRuntimeWindow(state, player, speaker, handlers);
		return true;
	}

	void TickRuntimeBattleObservePending(RuntimeHostStateRefs state, RE::Actor* player, const RuntimeHostHandlers& handlers)
	{
		if (!player) {
			if (state.bleedBattleObservePending) *state.bleedBattleObservePending = false;
			return;
		}
		if (state.minHp && *state.minHp > 0.0f) {
			if (handlers.setPlayerBleedImmune) handlers.setPlayerBleedImmune(true);
			if (handlers.clampHealth) handlers.clampHealth(player, *state.minHp);
		}

		const auto now = Clock::now();
		const float radius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 600.0f);
		auto followers = CollectRuntimeBattleObserveFollowers(radius, player, handlers);
		auto* preferredEnemy = ResolveRuntimeBattleObservePreferredEnemy(player, radius, followers, handlers);
		auto enemies = CollectRuntimeBattleObserveEnemies(player, radius, preferredEnemy, followers, handlers, true);
		auto activeEnemies = FilterRuntimeBattleObserveActiveEnemies(enemies, player, followers, "pending");

		if (followers.empty()) {
			int emptyAllyTicks = 1;
			if (state.bleedBattleObservePendingEmptyAllyTicks) {
				++(*state.bleedBattleObservePendingEmptyAllyTicks);
				emptyAllyTicks = *state.bleedBattleObservePendingEmptyAllyTicks;
			}
			if (state.bleedBattleObservePendingEmptyEnemyTicks) *state.bleedBattleObservePendingEmptyEnemyTicks = 0;

			if (emptyAllyTicks < 4) {
				ExtendRuntimeBattleObserveHold(state, now, std::chrono::milliseconds(250));
				if (emptyAllyTicks == 1 || emptyAllyTicks == 3) {
					spdlog::info(
						"[TFD][Bleedout][CB03] battle observe hold: waiting ally loss confirm ticks={} enemies={} reason=ally_scan_empty",
						emptyAllyTicks,
						static_cast<unsigned int>(enemies.size()));
				}
				return;
			}

			if (!enemies.empty()) {
				if (StartRuntimeWindowFromBattleObserveLoss(state, player, followers, enemies, radius, handlers, "battle_observe_pending_enemy_win_confirmed")) {
					return;
				}
				ExtendRuntimeBattleObserveHold(state, now, std::chrono::milliseconds(750));
				spdlog::info(
					"[TFD][Bleedout][CB03] battle observe enemy win held: enemies={} no valid speaker yet",
					static_cast<unsigned int>(enemies.size()));
				return;
			}

			const bool observeEmptyQueued = SendBridgeModEvent("TFDBleedoutBattleObserveState", nullptr, "win_empty", 0.0f);
			spdlog::info("[TFD][Bleedout][R33] battle observe dialogue guard cleared reason=win_empty queued={}", observeEmptyQueued ? 1 : 0);
			ClearRuntimeBattleObserveFlags(state);
			spdlog::info("[TFD][Bleedout][CB03] battle observe pending resolved without allies/enemies -> noncaptive recovery fallback");
			if (handlers.enterObservedBattleWin) handlers.enterObservedBattleWin();
			return;
		}

		if (state.bleedBattleObservePendingEmptyAllyTicks) *state.bleedBattleObservePendingEmptyAllyTicks = 0;
		if (!activeEnemies.empty()) {
			if (state.bleedBattleObservePendingEmptyEnemyTicks) *state.bleedBattleObservePendingEmptyEnemyTicks = 0;
			ExtendRuntimeBattleObserveHold(state, now, std::chrono::milliseconds(750));
			return;
		}

		if (state.bleedBattleObservePendingUntil && now < *state.bleedBattleObservePendingUntil) {
			return;
		}
		if (state.bleedBattleObservePendingEmptyEnemyTicks) {
			++(*state.bleedBattleObservePendingEmptyEnemyTicks);
			if (*state.bleedBattleObservePendingEmptyEnemyTicks < 4) {
				ExtendRuntimeBattleObserveHold(state, now, std::chrono::milliseconds(350));
				return;
			}
		}

		if (const auto victoryVisualPending = TFD::Victory::FindPendingEnemyVisualEntry(player, radius); victoryVisualPending != 0) {
			if (state.bleedBattleObservePendingEmptyEnemyTicks) *state.bleedBattleObservePendingEmptyEnemyTicks = 0;
			ExtendRuntimeBattleObserveHold(state, now, std::chrono::milliseconds(550));
			spdlog::info(
				"[TFD][Bleedout][P33M] pending player-side win held reason=victory_visual_pending actor={:08X} followers={} enemies={} activeEnemies=0",
				victoryVisualPending,
				static_cast<unsigned int>(followers.size()),
				static_cast<unsigned int>(enemies.size()));
			return;
		}

		if (auto* blockingThreat = ResolveRuntimeBattleObserveDirectBlockingThreat(
				player,
				radius,
				preferredEnemy,
				followers,
				handlers,
				"pending_win_guard")) {
			if (state.bleedBattleObservePendingEmptyEnemyTicks) *state.bleedBattleObservePendingEmptyEnemyTicks = 0;
			ExtendRuntimeBattleObserveHold(state, now, std::chrono::milliseconds(750));
			spdlog::info(
				"[TFD][Bleedout][R32] pending player-side win held threat={:08X} followers={} enemies={} reason=direct_blocking_threat",
				blockingThreat->GetFormID(),
				static_cast<unsigned int>(followers.size()),
				static_cast<unsigned int>(enemies.size()));
			return;
		}

		const bool observeWinQueued = SendBridgeModEvent("TFDBleedoutBattleObserveState", nullptr, "win", 0.0f);
		spdlog::info("[TFD][Bleedout][R33] battle observe dialogue guard cleared reason=win queued={}", observeWinQueued ? 1 : 0);
		ClearRuntimeBattleObserveFlags(state);
		spdlog::info(
			"[TFD][Bleedout][CB08][R16] battle observe pending resolved as player-side win followers={} enemies={} activeEnemies=0",
			static_cast<unsigned int>(followers.size()),
			static_cast<unsigned int>(enemies.size()));
		if (handlers.enterObservedBattleWin) handlers.enterObservedBattleWin();
	}


	void TickRuntimeBattleObserve(RuntimeHostStateRefs state, RE::Actor* player, const RuntimeHostHandlers& handlers)
	{
		if (!player) return;
		if (handlers.setPlayerBleedImmune) handlers.setPlayerBleedImmune(true);
		if (state.minHp && *state.minHp > 0.0f && handlers.clampHealth) handlers.clampHealth(player, *state.minHp);

		const auto now = Clock::now();
		const float radius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 600.0f);
		auto followers = CollectRuntimeBattleObserveFollowers(radius, player, handlers);
		auto* preferredEnemy = ResolveRuntimeBattleObservePreferredEnemy(player, radius, followers, handlers);
		auto enemies = CollectRuntimeBattleObserveEnemies(player, radius, preferredEnemy, followers, handlers, true);
		auto activeEnemies = FilterRuntimeBattleObserveActiveEnemies(enemies, player, followers, "active");

		if (!followers.empty()) {
			if (state.bleedBattleObserveActiveEmptyAllyTicks) *state.bleedBattleObserveActiveEmptyAllyTicks = 0;
		}
		if (!activeEnemies.empty()) {
			if (state.bleedBattleObserveActiveEmptyEnemyTicks) *state.bleedBattleObserveActiveEmptyEnemyTicks = 0;
		}

		if (!followers.empty() && !activeEnemies.empty()) {
			return;
		}

		if (followers.empty()) {
			int emptyAllyTicks = 1;
			if (state.bleedBattleObserveActiveEmptyAllyTicks) {
				++(*state.bleedBattleObserveActiveEmptyAllyTicks);
				emptyAllyTicks = *state.bleedBattleObserveActiveEmptyAllyTicks;
			}
			if (emptyAllyTicks < 4) {
				if (state.bleedBattleObserveSince) *state.bleedBattleObserveSince = now;
				return;
			}

			if (!enemies.empty()) {
				if (StartRuntimeWindowFromBattleObserveLoss(state, player, followers, enemies, radius, handlers, "battle_observe_enemy_win_confirmed")) {
					return;
				}
				spdlog::info(
					"[TFD][Bleedout][CB03] battle observe active enemy win held: enemies={} no valid speaker yet",
					static_cast<unsigned int>(enemies.size()));
				return;
			}

			const bool observeActiveEmptyQueued = SendBridgeModEvent("TFDBleedoutBattleObserveState", nullptr, "active_win_empty", 0.0f);
			spdlog::info("[TFD][Bleedout][R33] battle observe dialogue guard cleared reason=active_win_empty queued={}", observeActiveEmptyQueued ? 1 : 0);
			ClearRuntimeBattleObserveFlags(state);
			spdlog::info("[TFD][Bleedout][CB03] battle observe active resolved without allies/enemies -> noncaptive recovery fallback");
			if (handlers.enterObservedBattleWin) handlers.enterObservedBattleWin();
			return;
		}

		if (activeEnemies.empty()) {
			int emptyEnemyTicks = 1;
			if (state.bleedBattleObserveActiveEmptyEnemyTicks) {
				++(*state.bleedBattleObserveActiveEmptyEnemyTicks);
				emptyEnemyTicks = *state.bleedBattleObserveActiveEmptyEnemyTicks;
			}
			if (emptyEnemyTicks < 4) {
				return;
			}
			if (const auto victoryVisualPending = TFD::Victory::FindPendingEnemyVisualEntry(player, radius); victoryVisualPending != 0) {
				if (state.bleedBattleObserveActiveEmptyEnemyTicks) *state.bleedBattleObserveActiveEmptyEnemyTicks = 0;
				spdlog::info(
					"[TFD][Bleedout][P33M] active player-side win held reason=victory_visual_pending actor={:08X} followers={} enemies={} activeEnemies=0",
					victoryVisualPending,
					static_cast<unsigned int>(followers.size()),
					static_cast<unsigned int>(enemies.size()));
				return;
			}

			if (auto* blockingThreat = ResolveRuntimeBattleObserveDirectBlockingThreat(
					player,
					radius,
					preferredEnemy,
					followers,
					handlers,
					"active_win_guard")) {
				if (state.bleedBattleObserveActiveEmptyEnemyTicks) *state.bleedBattleObserveActiveEmptyEnemyTicks = 0;
				spdlog::info(
					"[TFD][Bleedout][R32] active player-side win held threat={:08X} followers={} enemies={} reason=direct_blocking_threat",
					blockingThreat->GetFormID(),
					static_cast<unsigned int>(followers.size()),
					static_cast<unsigned int>(enemies.size()));
				return;
			}
			const bool observeActiveWinQueued = SendBridgeModEvent("TFDBleedoutBattleObserveState", nullptr, "active_win", 0.0f);
			spdlog::info("[TFD][Bleedout][R33] battle observe dialogue guard cleared reason=active_win queued={}", observeActiveWinQueued ? 1 : 0);
			ClearRuntimeBattleObserveFlags(state);
			spdlog::info(
				"[TFD][Bleedout][CB08][R16] battle observe active resolved as player-side win followers={} enemies={} activeEnemies=0",
				static_cast<unsigned int>(followers.size()),
				static_cast<unsigned int>(enemies.size()));
			if (handlers.enterObservedBattleWin) handlers.enterObservedBattleWin();
			return;
		}
	}


	bool HandleRuntimePendingEscapeBreak(RuntimeHostStateRefs state, RE::Actor* player, const RuntimeHostHandlers& handlers)
	{
		auto isPending = [&]() -> bool {
			return state.isEscapeBreakBleedPending ? state.isEscapeBreakBleedPending() : false;
		};
		auto setPending = [&](bool pending) {
			if (state.setEscapeBreakBleedPending) {
				state.setEscapeBreakBleedPending(pending);
			}
		};

		if (!isPending()) return false;
		if (!player || player->IsDead() || player->IsDisabled()) {
			setPending(false);
			return false;
		}
		const float hpNow = player->GetActorValue(RE::ActorValue::kHealth);
		const float hpMax = (std::max)(1.0f, player->GetPermanentActorValue(RE::ActorValue::kHealth));
		const float pct = (hpNow / hpMax) * 100.0f;
		const float thresh = TFD::Settings::GetDefeatThresholdPct();
		const bool captiveEscapeRebleed = IsCaptiveEscapeRebleedFlow() || TFD::Captive::IsEscapeBleedoutActive();
		if (pct > thresh && !captiveEscapeRebleed) {
			setPending(false);
			if (handlers.setPlayerBleedImmune) {
				handlers.setPlayerBleedImmune(false);
			}
			spdlog::info("[TFD][Bleedout][P32N] pending escape-break rebleed canceled pct={:.1f} thresh={:.1f} captiveEscapeRebleed=0", pct, thresh);
			return false;
		}
		if (pct > thresh && captiveEscapeRebleed) {
			spdlog::warn("[TFD][Bleedout][P32N] pending escape-break rebleed forced despite high HP pct={:.1f} thresh={:.1f} reason=captive_escape_killmove_or_overkill_veto", pct, thresh);
		}
		const float scanRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius());
		if (handlers.clearEnemyTargetsToPlayerForDefeat) handlers.clearEnemyTargetsToPlayerForDefeat(player, scanRadius, "escape_break_rebleed");
		auto* aggressor = handlers.resolveEscapeBreakPreferredAggressor ? handlers.resolveEscapeBreakPreferredAggressor(scanRadius) : nullptr;
		auto standingFollowers = handlers.collectBleedStandingFollowers ? handlers.collectBleedStandingFollowers(scanRadius) : std::vector<RE::Actor*>{};
		if (!standingFollowers.empty()) {
			if (aggressor && (!handlers.isObserverAlly || !handlers.isObserverAlly(aggressor)) && handlers.setLastAggressor) {
				handlers.setLastAggressor(aggressor);
			}
			if (StartRuntimeBattleObservePending(state, player, handlers)) {
				setPending(false);
				return true;
			}
		}
		if (aggressor && handlers.isObserverAlly && handlers.isObserverAlly(aggressor)) aggressor = nullptr;
		aggressor = handlers.findBestSpeaker ? handlers.findBestSpeaker(scanRadius, 768.0f, aggressor) : aggressor;
		if (aggressor && handlers.setLastAggressor) handlers.setLastAggressor(aggressor);
		setPending(false);
		StartRuntimeWindow(state, player, aggressor, handlers);
		return true;
	}

	bool ShouldLogSpeakerPrimeRetrySkip(std::uint32_t speakerFormID, std::string_view reason)
	{
		static std::mutex s_lock;
		static std::uint32_t s_lastSpeakerFormID = 0;
		static std::string s_lastReason{};
		static Clock::time_point s_lastLog{};

		const auto now = Clock::now();
		const std::string reasonText(reason);
		std::scoped_lock lk(s_lock);
		const bool sameContext =
			s_lastSpeakerFormID == speakerFormID &&
			s_lastReason == reasonText;
		if (sameContext && s_lastLog.time_since_epoch().count() != 0 &&
			std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastLog).count() < 5000) {
			return false;
		}

		s_lastSpeakerFormID = speakerFormID;
		s_lastReason = reasonText;
		s_lastLog = now;
		return true;
	}

	void MaintainRuntimeSpeakerKick(RuntimeHostStateRefs state, RE::Actor* player, const RuntimeHostHandlers& handlers)
	{
		if (!state.inBleedState || !state.inBleedState->load(std::memory_order_acquire)) return;
		if (!state.bleedSpeakerId || *state.bleedSpeakerId == 0) return;
		if (!state.bleedPaused || *state.bleedPaused) return;
		if (!state.bleedStart || state.bleedStart->time_since_epoch().count() == 0) return;
		const bool nativeOpenSucceeded = TFD::InteractionRouter::DialogueOpen::WasLastSuccess(
			TFD::InteractionRouter::DialogueOpen::Mode::Bleedout,
			*state.bleedSpeakerId);
		if (TFD::BleedoutGreet::HasSeenDialogue()) {
			if (ShouldLogSpeakerPrimeRetrySkip(*state.bleedSpeakerId, "dialogue_menu_seen")) {
				spdlog::info("[TFD][Bleedout][P32I] speaker prime retry skipped reason=dialogue_menu_seen speaker={:08X}", *state.bleedSpeakerId);
			}
			return;
		}
		if (nativeOpenSucceeded) {
			// CB07: once the native forcegreet handoff reports success, stop overdrive
			// retries. The Papyrus ack can arrive late, and reopening here causes the
			// visible double-forcegreet/reopen loop. P32I only throttles this skip log;
			// it does not change cancel-vs-commit reopen semantics.
			if (ShouldLogSpeakerPrimeRetrySkip(*state.bleedSpeakerId, "native_open_succeeded_awaiting_ack")) {
				spdlog::info("[TFD][Bleedout][P32I] speaker prime retry skipped reason=native_open_succeeded_awaiting_ack speaker={:08X}", *state.bleedSpeakerId);
			}
			return;
		}
		if (TFD::InteractionRouter::DialogueOpen::IsActive() &&
			TFD::InteractionRouter::DialogueOpen::GetMode() == TFD::InteractionRouter::DialogueOpen::Mode::Bleedout) {
			if (ShouldLogSpeakerPrimeRetrySkip(*state.bleedSpeakerId, "native_open_pending")) {
				spdlog::info("[TFD][Bleedout][P32I] speaker prime retry skipped reason=native_open_pending speaker={:08X}", *state.bleedSpeakerId);
			}
			return;
		}
		if (TFD::BleedoutGreet::HasFlowGreetConfirmed()) {
			spdlog::info("[TFD][Bleedout][R432A] speaker prime retry allowed to continue reason=fragment_ack_without_dialogue_menu speaker={:08X}", *state.bleedSpeakerId);
		}
		const auto now = Clock::now();
		if (state.bleedSpeakerKickCount && *state.bleedSpeakerKickCount >= 3) return;
		if (state.bleedSpeakerKickCount && *state.bleedSpeakerKickCount == 0) {
			if (std::chrono::duration_cast<std::chrono::milliseconds>(now - *state.bleedStart).count() < 850) return;
		}
		else if (state.bleedSpeakerKickLast && state.bleedSpeakerKickLast->time_since_epoch().count() != 0) {
			if (std::chrono::duration_cast<std::chrono::milliseconds>(now - *state.bleedSpeakerKickLast).count() < 1100) return;
		}
		auto* actor = RE::TESForm::LookupByID<RE::Actor>(*state.bleedSpeakerId);
		float dist = 99999.0f;
		if (!player || !handlers.isReasonableSpeaker || !handlers.isReasonableSpeaker(actor, player, 1400.0f, &dist)) return;
		if (handlers.applyDialogueOverdrive) handlers.applyDialogueOverdrive(player, actor, "speaker_prime_retry_cb07", true);
		if (state.bleedSpeakerKickLast) *state.bleedSpeakerKickLast = now;
		if (state.bleedSpeakerKickCount) ++(*state.bleedSpeakerKickCount);
		spdlog::info("[TFD][Bleedout][CB07] speaker prime retry issued speaker={:08X} retry={} dist={:.1f}",
			actor ? actor->GetFormID() : 0u,
			state.bleedSpeakerKickCount ? *state.bleedSpeakerKickCount : 0u,
			dist);
	}

	bool IsActive()
	{
		return TFD::FlowController::Controller::GetSingleton().IsBleedDecisionActive();
	}

	bool OwnsCurrentFlow()
	{
		const auto snapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
		const bool captiveEscapeRebleed =
			snapshot.root == TFD::FlowController::RootFlow::Captive &&
			(snapshot.sub == TFD::FlowController::SubFlow::EscapeFailed ||
				snapshot.sub == TFD::FlowController::SubFlow::Recapture);
		return snapshot.root == TFD::FlowController::RootFlow::Bleedout ||
			snapshot.contextRoot == TFD::FlowController::RootFlow::Bleedout ||
			captiveEscapeRebleed ||
			snapshot.sub == TFD::FlowController::SubFlow::BleedoutPleasure ||
			snapshot.sub == TFD::FlowController::SubFlow::BleedoutAfterPleasure;
	}

	std::uint32_t ResolveActorFormID(RE::Actor* actor)
	{
		return ActorFormID(actor);
	}
}

// Consolidated from former TFDBleedoutBuilders / TFDBleedoutRuntimeHost staging modules
namespace TFD::Bleedout::Builders
{
    namespace
    {
        PendingSystemEventProvider g_pending{};
        DialogueCloseProvider g_dialogueClose{};
        TimeoutProvider g_timeout{};
        BaseCompletionProvider g_completionBase{};
        CaptivePleasureCompletionExtras g_captiveCompletion{};
        PayReleaseCompletionExtras g_payReleaseCompletion{};
        BleedPleasureCompletionExtras g_bleedPleasureCompletion{};
        NonCaptiveChoiceProvider g_nonCaptiveChoice{};
        BlackoutProvider g_blackout{};
        TransitionRuntimeProvider g_transitionRuntime{};
        TransitionCaptiveProvider g_transitionCaptive{};

        CompletionHandlers BuildBaseCompletionHandlers()
        {
            CompletionHandlers handlers{};
            handlers.clearOutcomeWindow = g_completionBase.clearOutcomeWindow;
            handlers.tryBeginTerminalCommit = g_completionBase.tryBeginTerminalCommit;
            handlers.clearPendingCinematicFadeIn = g_completionBase.clearPendingCinematicFadeIn;
            handlers.clearBridgeAliases = g_completionBase.clearBridgeAliases;
            handlers.clearEscapeContext = g_completionBase.clearEscapeContext;
            handlers.resetLockpickWatch = g_completionBase.resetLockpickWatch;
            handlers.setGraceActive = g_completionBase.setGraceActive;
            handlers.setCaptiveRuntime = g_completionBase.setCaptiveRuntime;
            handlers.setPlayerBleedImmune = g_completionBase.setPlayerBleedImmune;
            handlers.recoverPlayerForTransition = g_completionBase.recoverPlayerForTransition;
            handlers.getSweepRadius = g_completionBase.getSweepRadius;
            handlers.applyCalmBubble = g_completionBase.applyCalmBubble;
            handlers.updatePreCombatState = g_completionBase.updatePreCombatState;
            handlers.resolveRuntimeSpeaker = g_completionBase.resolveRuntimeSpeaker;
            handlers.beginPleasure = g_completionBase.beginPleasure;
            handlers.setPrevDialogueOpen = g_completionBase.setPrevDialogueOpen;
            handlers.setPrevLockpickOpen = g_completionBase.setPrevLockpickOpen;
            return handlers;
        }
    }

    void InstallPendingSystemEventProvider(PendingSystemEventProvider provider) { g_pending = std::move(provider); }
    void InstallDialogueCloseProvider(DialogueCloseProvider provider) { g_dialogueClose = std::move(provider); }
    void InstallTimeoutProvider(TimeoutProvider provider) { g_timeout = std::move(provider); }
    void InstallBaseCompletionProvider(BaseCompletionProvider provider) { g_completionBase = std::move(provider); }
    void InstallCaptivePleasureCompletionExtras(CaptivePleasureCompletionExtras provider) { g_captiveCompletion = std::move(provider); }
    void InstallPayReleaseCompletionExtras(PayReleaseCompletionExtras provider) { g_payReleaseCompletion = std::move(provider); }
    void InstallBleedPleasureCompletionExtras(BleedPleasureCompletionExtras provider) { g_bleedPleasureCompletion = std::move(provider); }
    void InstallNonCaptiveChoiceProvider(NonCaptiveChoiceProvider provider) { g_nonCaptiveChoice = std::move(provider); }
    void InstallBlackoutProvider(BlackoutProvider provider) { g_blackout = std::move(provider); }
    void InstallTransitionRuntimeProvider(TransitionRuntimeProvider provider) { g_transitionRuntime = std::move(provider); }
    void InstallTransitionCaptiveProvider(TransitionCaptiveProvider provider) { g_transitionCaptive = std::move(provider); }

    void Reset()
    {
        g_pending = {};
        g_dialogueClose = {};
        g_timeout = {};
        g_completionBase = {};
        g_captiveCompletion = {};
        g_payReleaseCompletion = {};
        g_bleedPleasureCompletion = {};
        g_nonCaptiveChoice = {};
        g_blackout = {};
        g_transitionRuntime = {};
        g_transitionCaptive = {};
    }

    PendingSystemEventHandlers BuildPendingSystemEventHandlers()
    {
        PendingSystemEventHandlers handlers{};
        handlers.clearDialogueOutcome = g_pending.clearDialogueOutcome;
        handlers.clearOutcomeWindow = g_pending.clearOutcomeWindow;
        handlers.completePayRelease = g_pending.completePayRelease;
        handlers.releaseFlowHandoff = g_pending.releaseFlowHandoff;
        handlers.resolveCaptiveMarker = g_pending.resolveCaptiveMarker;
        handlers.doBlackoutTeleport = g_pending.doBlackoutTeleport;
        handlers.setGraceSeconds = g_pending.setGraceSeconds;
        handlers.releaseNoSpeakerTameSession = g_pending.releaseNoSpeakerTameSession;
        handlers.exitBleedState = g_pending.exitBleedState;
        handlers.enterNonCaptiveChoice = g_pending.enterNonCaptiveChoice;
        return handlers;
    }

    DialogueCloseHandlers BuildDialogueCloseHandlers()
    {
        return DialogueCloseHandlers{g_dialogueClose.clearDialogueOutcome, g_dialogueClose.completePayRelease};
    }

    TimeoutHandlers BuildTimeoutHandlers()
    {
        TimeoutHandlers handlers{};
        handlers.releaseTruceGeneric = g_timeout.releaseTruceGeneric;
        handlers.resolveCaptiveMarker = g_timeout.resolveCaptiveMarker;
        handlers.resetBleedRuntimeState = g_timeout.resetBleedRuntimeState;
        handlers.doBlackoutTeleport = g_timeout.doBlackoutTeleport;
        handlers.setGraceSeconds = g_timeout.setGraceSeconds;
        handlers.enterNonCaptiveChoice = g_timeout.enterNonCaptiveChoice;
        return handlers;
    }

    CompletionHandlers BuildCaptivePleasureCompletionHandlers()
    {
        auto handlers = BuildBaseCompletionHandlers();
        handlers.clearLastAggressor = g_captiveCompletion.clearLastAggressor;
        handlers.resetBleedRuntimeState = g_captiveCompletion.resetBleedRuntimeState;
        handlers.syncPlayerCaptiveAlias = g_captiveCompletion.syncPlayerCaptiveAlias;
        return handlers;
    }

    CompletionHandlers BuildPayReleaseCompletionHandlers()
    {
        auto handlers = BuildBaseCompletionHandlers();
        handlers.clearFactionState = g_payReleaseCompletion.clearFactionState;
        handlers.clearLastAggressor = g_payReleaseCompletion.clearLastAggressor;
        handlers.resetBleedRuntimeState = g_payReleaseCompletion.resetBleedRuntimeState;
        handlers.beginLeftForDeadCooldown = g_payReleaseCompletion.beginLeftForDeadCooldown;
        handlers.setGraceSeconds = g_payReleaseCompletion.setGraceSeconds;
        return handlers;
    }

    CompletionHandlers BuildBleedPleasureCompletionHandlers()
    {
        auto handlers = BuildBaseCompletionHandlers();
        handlers.transitionBleedRuntimeToPleasureCommit = g_bleedPleasureCompletion.transitionBleedRuntimeToPleasureCommit;
        handlers.beginLeftForDeadCooldown = g_bleedPleasureCompletion.beginLeftForDeadCooldown;
        handlers.setGraceSeconds = g_bleedPleasureCompletion.setGraceSeconds;
        handlers.refreshPostDefeatGlobals = g_bleedPleasureCompletion.refreshPostDefeatGlobals;
        return handlers;
    }

    NonCaptiveChoiceHandlers BuildNonCaptiveChoiceHandlers()
    {
        NonCaptiveChoiceHandlers handlers{};
        handlers.hasBlockingCommit = g_nonCaptiveChoice.hasBlockingCommit;
        handlers.getBlockingCommitName = g_nonCaptiveChoice.getBlockingCommitName;
        handlers.beginResolvedNoMarkerFallback = g_nonCaptiveChoice.beginResolvedNoMarkerFallback;
        handlers.clearBridgeAliases = g_nonCaptiveChoice.clearBridgeAliases;
        handlers.clearPendingCinematicFadeIn = g_nonCaptiveChoice.clearPendingCinematicFadeIn;
        handlers.clearFactionState = g_nonCaptiveChoice.clearFactionState;
        handlers.clearEscapeContext = g_nonCaptiveChoice.clearEscapeContext;
        handlers.resetLockpickWatch = g_nonCaptiveChoice.resetLockpickWatch;
        handlers.setGraceActive = g_nonCaptiveChoice.setGraceActive;
        handlers.clearLastAggressor = g_nonCaptiveChoice.clearLastAggressor;
        handlers.resetBleedRuntimeState = g_nonCaptiveChoice.resetBleedRuntimeState;
        handlers.setPrevDialogueOpen = g_nonCaptiveChoice.setPrevDialogueOpen;
        handlers.setPrevLockpickOpen = g_nonCaptiveChoice.setPrevLockpickOpen;
        handlers.setCaptiveRuntime = g_nonCaptiveChoice.setCaptiveRuntime;
        handlers.setPlayerBleedImmune = g_nonCaptiveChoice.setPlayerBleedImmune;
        handlers.queueLegacyRequest = g_nonCaptiveChoice.queueLegacyRequest;
        handlers.resetFlowRuntime = g_nonCaptiveChoice.resetFlowRuntime;
        return handlers;
    }

    BlackoutHandlers BuildBlackoutHandlers()
    {
        BlackoutHandlers handlers{};
        handlers.hasBlockingCommit = g_blackout.hasBlockingCommit;
        handlers.getBlockingCommitName = g_blackout.getBlockingCommitName;
        handlers.resolveCaptiveMarker = g_blackout.resolveCaptiveMarker;
        handlers.enterNonCaptiveChoice = g_blackout.enterNonCaptiveChoice;
        handlers.tryBeginCaptiveCommit = g_blackout.tryBeginCaptiveCommit;
        handlers.resetBleedRuntimeState = g_blackout.resetBleedRuntimeState;
        handlers.clearBridgeAliases = g_blackout.clearBridgeAliases;
        handlers.clearLastAggressor = g_blackout.clearLastAggressor;
        handlers.beginCaptiveFlow = g_blackout.beginCaptiveFlow;
        handlers.clearPendingCinematicFadeIn = g_blackout.clearPendingCinematicFadeIn;
        handlers.queueCaptiveFadeTransition = g_blackout.queueCaptiveFadeTransition;
        handlers.showBlackoutFader = g_blackout.showBlackoutFader;
        handlers.completeCaptiveTransitionNow = g_blackout.completeCaptiveTransitionNow;
        handlers.hideBlackoutFader = g_blackout.hideBlackoutFader;
        return handlers;
    }

    TFD::Transition::RuntimeHandlers BuildTransitionRuntimeHandlers()
    {
        TFD::Transition::RuntimeHandlers handlers{};
        handlers.getPlayer = g_transitionRuntime.getPlayer;
        handlers.resolveAggressor = g_transitionRuntime.resolveAggressor;
        handlers.findBestAggressor = g_transitionRuntime.findBestAggressor;
        handlers.isCombatSupportedAggressor = g_transitionRuntime.isCombatSupportedAggressor;
        handlers.isActiveFollowerActor = g_transitionRuntime.isActiveFollowerActor;
        handlers.isStandingAllyThresholdActor = g_transitionRuntime.isStandingAllyThresholdActor;
        handlers.collectRegisteredTeammates = g_transitionRuntime.collectRegisteredTeammates;
        handlers.collectBleedoutCrowd = g_transitionRuntime.collectBleedoutCrowd;
        handlers.getBleedCrowdAssigned = g_transitionRuntime.getBleedCrowdAssigned;
        handlers.tryAbortPleasureDueToHostileIntrusion = g_transitionRuntime.tryAbortPleasureDueToHostileIntrusion;
        handlers.releasePlayerBleedLock = g_transitionRuntime.releasePlayerBleedLock;
        handlers.setGraceSeconds = g_transitionRuntime.setGraceSeconds;
        handlers.setRescueStateValue = g_transitionRuntime.setRescueStateValue;
        handlers.refreshPostDefeatGlobals = g_transitionRuntime.refreshPostDefeatGlobals;
        handlers.updatePreCombatState = g_transitionRuntime.updatePreCombatState;
        return handlers;
    }

    TFD::Transition::CaptiveHandlers BuildTransitionCaptiveHandlers()
    {
        TFD::Transition::CaptiveHandlers handlers{};
        handlers.resetBleedRuntimeState = g_transitionCaptive.resetBleedRuntimeState;
        handlers.clearBridgeAliases = g_transitionCaptive.clearBridgeAliases;
        handlers.clearLastAggressor = g_transitionCaptive.clearLastAggressor;
        handlers.beginCaptiveFlow = g_transitionCaptive.beginCaptiveFlow;
        handlers.setCaptiveRuntimeCaptive = g_transitionCaptive.setCaptiveRuntimeCaptive;
        handlers.isDialogueOpen = g_transitionCaptive.isDialogueOpen;
        handlers.setPrevDialogueOpen = g_transitionCaptive.setPrevDialogueOpen;
        handlers.captureCurrentLockpickMenuState = g_transitionCaptive.captureCurrentLockpickMenuState;
        handlers.resetLockpickWatch = g_transitionCaptive.resetLockpickWatch;
        handlers.armEscapeContextFromCurrentState = g_transitionCaptive.armEscapeContextFromCurrentState;
        handlers.sealCaptiveDoorIfPresent = g_transitionCaptive.sealCaptiveDoorIfPresent;
        handlers.applyCalmBubble = g_transitionCaptive.applyCalmBubble;
        handlers.queuePendingCaptiveConfiscation = g_transitionCaptive.queuePendingCaptiveConfiscation;
        handlers.syncPlayerCaptiveAlias = g_transitionCaptive.syncPlayerCaptiveAlias;
        return handlers;
    }
}


namespace TFD::Bleedout
{
	void InstallDefeatLifecycleProviders(DefeatLifecycleProviders providers)
	{
		Builders::InstallPendingSystemEventProvider(std::move(providers.pendingSystemEvent));
		Builders::InstallDialogueCloseProvider(std::move(providers.dialogueClose));
		Builders::InstallTimeoutProvider(std::move(providers.timeout));
		Builders::InstallBaseCompletionProvider(std::move(providers.baseCompletion));
		Builders::InstallCaptivePleasureCompletionExtras(std::move(providers.captivePleasureCompletion));
		Builders::InstallPayReleaseCompletionExtras(std::move(providers.payReleaseCompletion));
		Builders::InstallBleedPleasureCompletionExtras(std::move(providers.bleedPleasureCompletion));
		RuntimeHost::InstallProvider(std::move(providers.runtimeHost));
		DefeatGlue::InstallProvider(std::move(providers.defeatGlue));
	}

	void ResetDefeatLifecycleProviders()
	{
		Builders::Reset();
		RuntimeHost::Reset();
		DefeatGlue::Reset();
	}
}


namespace TFD::Bleedout::RuntimeHost
{
	namespace
	{
		Provider g_provider{};
	}

	void InstallProvider(Provider provider)
	{
		g_provider = std::move(provider);
	}

	void Reset()
	{
		g_provider = {};
	}

	TFD::Bleedout::RuntimeHostStateRefs BuildStateRefs()
	{
		TFD::Bleedout::RuntimeHostStateRefs state{};
		state.inBleedState = g_provider.inBleedState;
		state.minHp = g_provider.minHp;
		state.bleedStart = g_provider.bleedStart;
		state.bleedLastSeconds = g_provider.bleedLastSeconds;
		state.bleedPaused = g_provider.bleedPaused;
		state.bleedPauseStarted = g_provider.bleedPauseStarted;
		state.bleedLastCalmPulse = g_provider.bleedLastCalmPulse;
		state.bleedLastCrowdAssign = &TFD::Bleedout::BleedLastCrowdAssignRef();
		state.bleedCrowdAssigned = &TFD::Bleedout::BleedCrowdAssignedRef();
		state.bleedRejectedSpeakerIds = &TFD::Bleedout::BleedRejectedSpeakerIdsRef();
		state.bleedSpeakerId = &TFD::Bleedout::BleedSpeakerIDRef();
		state.bleedSpeakerKickLast = &TFD::Bleedout::BleedSpeakerKickLastRef();
		state.bleedSpeakerKickCount = &TFD::Bleedout::BleedSpeakerKickCountRef();
		state.bleedDialogueRetryCount = &TFD::Bleedout::BleedDialogueRetryCountRef();
		state.isEscapeBreakBleedPending = g_provider.isEscapeBreakBleedPending;
		state.setEscapeBreakBleedPending = g_provider.setEscapeBreakBleedPending;
		state.bleedPendingCaptiveOutcome = g_provider.bleedPendingCaptiveOutcome;
		state.bleedPendingNonCaptiveOutcome = g_provider.bleedPendingNonCaptiveOutcome;
		state.bleedBattleObservePending = g_provider.bleedBattleObservePending;
		state.bleedBattleObservePendingUntil = g_provider.bleedBattleObservePendingUntil;
		state.bleedBattleObservePendingLastRedirect = g_provider.bleedBattleObservePendingLastRedirect;
		state.bleedBattleObservePendingEmptyEnemyTicks = g_provider.bleedBattleObservePendingEmptyEnemyTicks;
		state.bleedBattleObservePendingEmptyAllyTicks = g_provider.bleedBattleObservePendingEmptyAllyTicks;
		state.bleedBattleObserveActive = g_provider.bleedBattleObserveActive;
		state.bleedBattleObserveSince = g_provider.bleedBattleObserveSince;
		state.bleedBattleObserveLastRedirect = g_provider.bleedBattleObserveLastRedirect;
		state.bleedBattleObserveActiveEmptyEnemyTicks = g_provider.bleedBattleObserveActiveEmptyEnemyTicks;
		state.bleedBattleObserveActiveEmptyAllyTicks = g_provider.bleedBattleObserveActiveEmptyAllyTicks;
		return state;
	}

	TFD::Bleedout::RuntimeHostHandlers BuildHandlers()
	{
		if (g_provider.buildRuntimeHostHandlers) {
			return g_provider.buildRuntimeHostHandlers();
		}
		return {};
	}

	void StartWindow(RE::Actor* player, RE::Actor* aggressor)
	{
		TFD::Bleedout::StartRuntimeWindow(BuildStateRefs(), player, aggressor, BuildHandlers());
	}

	bool BeginDialogueHotkey()
	{
		if (!g_provider.buildDialogueHotkeyHandlers) {
			return false;
		}
		const float radius = g_provider.getDialogueHotkeyRadius ? g_provider.getDialogueHotkeyRadius() : 2400.0f;
		const float maxSpeakerDist = g_provider.getDialogueHotkeyMaxSpeakerDist ? g_provider.getDialogueHotkeyMaxSpeakerDist() : 1800.0f;
		return TFD::Bleedout::BeginDialogueHotkey((std::max)(2400.0f, radius), maxSpeakerDist, g_provider.buildDialogueHotkeyHandlers());
	}

	bool StartBattleObservePending(RE::Actor* player)
	{
		return TFD::Bleedout::StartRuntimeBattleObservePending(BuildStateRefs(), player, BuildHandlers());
	}

	void TickBattleObservePending()
	{
		RE::Actor* player = g_provider.getPlayer ? g_provider.getPlayer() : nullptr;
		TFD::Bleedout::TickRuntimeBattleObservePending(BuildStateRefs(), player, BuildHandlers());
	}

	void TickBattleObserve()
	{
		RE::Actor* player = g_provider.getPlayer ? g_provider.getPlayer() : nullptr;
		TFD::Bleedout::TickRuntimeBattleObserve(BuildStateRefs(), player, BuildHandlers());
	}

	bool HandlePendingEscapeBreak()
	{
		RE::Actor* player = g_provider.getPlayer ? g_provider.getPlayer() : nullptr;
		return TFD::Bleedout::HandleRuntimePendingEscapeBreak(BuildStateRefs(), player, BuildHandlers());
	}

	void MaintainSpeakerKick()
	{
		RE::Actor* player = g_provider.getPlayer ? g_provider.getPlayer() : nullptr;
		TFD::Bleedout::MaintainRuntimeSpeakerKick(BuildStateRefs(), player, BuildHandlers());
	}
}



namespace TFD::Bleedout::DefeatGlue
{
	namespace
	{
		Provider g_provider{};
		using Clock = std::chrono::steady_clock;
	}

	void InstallProvider(Provider provider)
	{
		g_provider = std::move(provider);
	}

	struct ObservedBattleState
	{
		bool hadValidObservedEnemy = false;
		std::unordered_set<RE::FormID> allyIds{};
		std::unordered_set<RE::FormID> enemyIds{};
	};

	ObservedBattleState g_observedBattle{};
	RE::ActorHandle g_observedPreferredEnemy{};

	static RE::Actor* PlayerActor()
	{
		return g_provider.getPlayer ? g_provider.getPlayer() : RE::PlayerCharacter::GetSingleton();
	}

	static bool IsActorBleedingOutLocal(RE::Actor* actor)
	{
		if (!actor) {
			return false;
		}
		if (auto* state = actor->AsActorState()) {
			return state->IsBleedingOut();
		}
		return false;
	}

	static bool IsStandingObserverActorInternal(RE::Actor* actor)
	{
		return actor && !actor->IsDisabled() && !actor->IsDead() && !IsActorBleedingOutLocal(actor);
	}

	static bool IsStandingAllyThresholdActorInternal(RE::Actor* actor)
	{
		auto* player = PlayerActor();
		if (!actor || actor == player) {
			return false;
		}
		return !TFD::DefeatMonitor::IsThresholdDownedActor(actor);
	}

	static bool IsStandingEnemyThresholdActorInternal(RE::Actor* actor)
	{
		auto* player = PlayerActor();
		if (!actor || actor == player) {
			return false;
		}
		return !TFD::DefeatMonitor::IsThresholdDownedActor(actor);
	}

	static RE::Actor* ResolveCurrentCombatTargetInternal(RE::Actor* actor)
	{
		if (!actor) {
			return nullptr;
		}
		auto sp = actor->GetActorRuntimeData().currentCombatTarget.get();
		return sp.get();
	}

	static bool IsCombatSupportedAggressorInternal(RE::Actor* actor)
	{
		auto* player = PlayerActor();
		if (!actor || actor == player) {
			return false;
		}
		if (actor->IsDead() || actor->IsDisabled()) {
			return false;
		}
		if (TFD::TeammateManager::IsActiveFollowerActor(actor) || TFD::Tame::IsCompanion(actor)) {
			return false;
		}
		return true;
	}

	static float MinDistanceToObserverSideInternal(RE::Actor* actor, RE::Actor* player, const std::vector<RE::Actor*>& allies)
	{
		if (!actor || !player) {
			return std::numeric_limits<float>::max();
		}
		float best = Distance3D(actor->GetPosition(), player->GetPosition());
		for (auto* ally : allies) {
			if (!IsStandingAllyThresholdActorInternal(ally)) {
				continue;
			}
			best = (std::min)(best, Distance3D(actor->GetPosition(), ally->GetPosition()));
		}
		return best;
	}

	static bool IsObserverEnemyInternal(RE::Actor* actor, RE::Actor* player, const std::vector<RE::Actor*>& allies, bool hostileHint, bool inCombatHint)
	{
		if (!player || !IsStandingEnemyThresholdActorInternal(actor) || actor == player) {
			return false;
		}
		if (IsObserverAlly(actor)) {
			return false;
		}

		auto* target = ResolveCurrentCombatTargetInternal(actor);
		if (target == player) {
			return true;
		}

		bool hostileToSide = actor->IsHostileToActor(player);
		for (auto* ally : allies) {
			if (!IsStandingAllyThresholdActorInternal(ally)) {
				continue;
			}
			if (target == ally) {
				return true;
			}
			if (actor->IsHostileToActor(ally)) {
				hostileToSide = true;
			}
		}

		if (!hostileHint && !inCombatHint && !actor->IsInCombat() && !hostileToSide) {
			return false;
		}
		return hostileToSide || hostileHint || inCombatHint || actor->IsInCombat();
	}

	static bool IsValidBleedBattleEnemyRosterActorInternal(RE::Actor* actor, RE::Actor* player)
	{
		if (!player || !actor || actor == player) {
			return false;
		}
		if (!IsStandingEnemyThresholdActorInternal(actor)) {
			return false;
		}
		if (IsObserverAlly(actor)) {
			return false;
		}
		if (!actor->Is3DLoaded()) {
			return false;
		}
		auto* pCell = player->GetParentCell();
		if (pCell && actor->GetParentCell() != pCell) {
			return false;
		}
		return true;
	}

	static bool IsLikelyObservedEnemySeedInternal(RE::Actor* actor, RE::Actor* player, const std::vector<RE::Actor*>& allies, float seedRadius, bool hostileHint, bool inCombatHint)
	{
		if (!IsValidBleedBattleEnemyRosterActorInternal(actor, player)) {
			return false;
		}
		if (!IsCombatSupportedAggressorInternal(actor)) {
			return false;
		}

		if (IsObserverEnemyInternal(actor, player, allies, hostileHint, inCombatHint)) {
			return true;
		}

		auto* target = ResolveCurrentCombatTargetInternal(actor);
		if (target == player) {
			return true;
		}

		bool targetsSide = false;
		bool hostileToSide = actor->IsHostileToActor(player);
		for (auto* ally : allies) {
			if (!IsStandingAllyThresholdActorInternal(ally)) {
				continue;
			}
			if (target == ally) {
				targetsSide = true;
			}
			if (actor->IsHostileToActor(ally)) {
				hostileToSide = true;
			}
		}

		if (targetsSide || hostileToSide) {
			return true;
		}

		if (!hostileHint && !inCombatHint && !actor->IsInCombat()) {
			return false;
		}

		return MinDistanceToObserverSideInternal(actor, player, allies) <= seedRadius;
	}

	void Reset()
	{
		g_provider = {};
		ResetObservedRuntimeTracking();
	}

	void ResetObservedRuntimeTracking()
	{
		g_observedPreferredEnemy.reset();
		g_observedBattle = {};
	}

	bool IsObserverAlly(RE::Actor* actor)
	{
		if (!IsStandingObserverActorInternal(actor)) {
			return false;
		}
		auto* player = PlayerActor();
		if (actor == player) {
			return false;
		}
		return TFD::TeammateManager::IsActiveFollowerActor(actor) || TFD::Tame::IsCompanion(actor);
	}

	float ComputeObservedEnemyScanRadius(RE::Actor* player, const std::vector<RE::Actor*>& allies, float baseRadius)
	{
		float scanRadius = (std::max)(2400.0f, baseRadius);
		if (!player) {
			return scanRadius;
		}
		for (auto* ally : allies) {
			if (!IsStandingAllyThresholdActorInternal(ally)) {
				continue;
			}
			const float dist = Distance3D(player->GetPosition(), ally->GetPosition());
			scanRadius = (std::max)(scanRadius, dist + 1600.0f);
		}
		return (std::min)(scanRadius, 9000.0f);
	}

	std::vector<RE::Actor*> CollectCurrentObservedEnemies(RE::Actor* player, float radius, RE::Actor* preferredEnemy, const std::vector<RE::Actor*>& allies)
	{
		std::vector<RE::Actor*> out;
		if (!player) {
			return out;
		}
		auto* pCell = player->GetParentCell();
		if (!pCell) {
			return out;
		}
		const float scanRadius = ComputeObservedEnemyScanRadius(player, allies, radius);
		const float seedRadius = (std::max)(1800.0f, scanRadius * 0.55f);
		auto snapshot = TFD::Actor::BuildSnapshot(scanRadius, false);
		for (const auto& info : snapshot.actors) {
			auto* actor = info.get();
			const bool hostile = info.hostileToPlayer;
			const bool inCombat = info.inCombat;
			if (!actor || actor->GetParentCell() != pCell || !actor->Is3DLoaded()) {
				continue;
			}
			if (TFD::Actor::IsActorOutsider(snapshot, actor)) {
				continue;
			}
			if (IsLikelyObservedEnemySeedInternal(actor, player, allies, seedRadius, hostile, inCombat)) {
				out.push_back(actor);
			}
		}
		if (preferredEnemy && IsLikelyObservedEnemySeedInternal(preferredEnemy, player, allies, seedRadius, true, preferredEnemy->IsInCombat())) {
			const auto id = preferredEnemy->GetFormID();
			auto it = std::find_if(out.begin(), out.end(), [id](RE::Actor* a) { return a && a->GetFormID() == id; });
			if (it == out.end()) {
				out.push_back(preferredEnemy);
			}
		} else if (out.empty()) {
			auto* fallbackEnemy = g_provider.resolveLastEnemyTargetingPlayer ? g_provider.resolveLastEnemyTargetingPlayer(scanRadius, 15.0) : nullptr;
			if (!fallbackEnemy && g_provider.findBestAggressor) {
				fallbackEnemy = g_provider.findBestAggressor(scanRadius);
			}
			if (fallbackEnemy && IsLikelyObservedEnemySeedInternal(fallbackEnemy, player, allies, seedRadius, true, fallbackEnemy->IsInCombat())) {
				out.push_back(fallbackEnemy);
			}
		}
		return out;
	}

	void UpdateObservedBattleRoster(RE::Actor* player, const std::vector<RE::Actor*>& followers, const std::vector<RE::Actor*>& enemies, RE::Actor* preferredEnemy)
	{
		if (!player) {
			return;
		}
		for (auto* ally : followers) {
			if (ally && IsStandingAllyThresholdActorInternal(ally)) {
				g_observedBattle.allyIds.insert(ally->GetFormID());
			}
		}
		for (auto* enemy : enemies) {
			if (enemy && IsValidBleedBattleEnemyRosterActorInternal(enemy, player)) {
				g_observedBattle.enemyIds.insert(enemy->GetFormID());
			}
		}
		if (preferredEnemy && IsValidBleedBattleEnemyRosterActorInternal(preferredEnemy, player)) {
			g_observedBattle.enemyIds.insert(preferredEnemy->GetFormID());
			g_observedPreferredEnemy = preferredEnemy->GetHandle();
		} else if (!preferredEnemy) {
			g_observedPreferredEnemy.reset();
		}
		g_observedBattle.hadValidObservedEnemy = g_observedBattle.hadValidObservedEnemy || !g_observedBattle.enemyIds.empty();
	}

	std::vector<RE::Actor*> CollectStandingFollowersFromSnapshot()
	{
		std::vector<RE::Actor*> out;
		for (auto id : g_observedBattle.allyIds) {
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
			if (!IsStandingAllyThresholdActorInternal(actor)) {
				continue;
			}
			out.push_back(actor);
		}
		return out;
	}

	std::vector<RE::Actor*> CollectStandingEnemiesFromSnapshot()
	{
		std::vector<RE::Actor*> out;
		for (auto id : g_observedBattle.enemyIds) {
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
			if (!IsStandingEnemyThresholdActorInternal(actor)) {
				continue;
			}
			out.push_back(actor);
		}
		return out;
	}

	static RE::Actor* PickClosestObservedTargetInternal(RE::Actor* source, const std::vector<RE::Actor*>& candidates)
	{
		if (!source || candidates.empty()) {
			return nullptr;
		}
		RE::Actor* best = nullptr;
		float bestDist = std::numeric_limits<float>::max();
		const auto srcPos = source->GetPosition();
		for (auto* actor : candidates) {
			if (!actor || !IsStandingObserverActorInternal(actor) || actor == source) {
				continue;
			}
			const float dist = Distance3D(srcPos, actor->GetPosition());
			if (dist < bestDist) {
				bestDist = dist;
				best = actor;
			}
		}
		return best;
	}

	static bool CanRedirectBleedEnemyToFollowersInternal(RE::Actor* actor, RE::Actor* player, const std::vector<RE::Actor*>& followers)
	{
		if (!actor || !player || followers.empty()) {
			return false;
		}
		if (!IsCombatSupportedAggressorInternal(actor) || !IsStandingEnemyThresholdActorInternal(actor) || IsObserverAlly(actor)) {
			return false;
		}
		auto* currentTarget = ResolveCurrentCombatTargetInternal(actor);
		if (currentTarget == player) {
			return true;
		}
		for (auto* follower : followers) {
			if (!IsStandingAllyThresholdActorInternal(follower)) {
				continue;
			}
			if (currentTarget == follower) {
				return true;
			}
			if (actor->IsHostileToActor(follower)) {
				return true;
			}
		}
		if (g_observedBattle.enemyIds.find(actor->GetFormID()) != g_observedBattle.enemyIds.end()) {
			return true;
		}
		return actor->IsInCombat() || actor->IsHostileToActor(player);
	}

	RE::Actor* ResolveBleedRedirectTarget(RE::Actor* actor)
	{
		if (!IsPlayerBleedHoldTargetBlocked()) {
			return nullptr;
		}
		auto* player = PlayerActor();
		if (!player || !actor || actor == player) {
			return nullptr;
		}
		auto followers = CollectStandingFollowersFromSnapshot();
		auto state = TFD::Bleedout::RuntimeHost::BuildStateRefs();
		if (followers.empty() && state.bleedBattleObservePending && *state.bleedBattleObservePending) {
			const float radius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 600.0f);
			followers = g_provider.collectBleedStandingFollowers ? g_provider.collectBleedStandingFollowers(radius) : std::vector<RE::Actor*>{};
		}
		if (followers.empty()) {
			return nullptr;
		}
		if (!CanRedirectBleedEnemyToFollowersInternal(actor, player, followers)) {
			return nullptr;
		}
		auto* target = PickClosestObservedTargetInternal(actor, followers);
		if (!target || target == player) {
			return nullptr;
		}
		return target;
	}

	RE::Actor* ResolveBleedFollowerAggroTarget(RE::Actor* actor)
	{
		if (!IsPlayerBleedHoldTargetBlocked()) {
			return nullptr;
		}
		auto* player = PlayerActor();
		if (!player || !actor || actor == player) {
			return nullptr;
		}
		if (!IsStandingAllyThresholdActorInternal(actor)) {
			return nullptr;
		}
		if (!TFD::TeammateManager::IsActiveFollowerActor(actor) && !TFD::Tame::IsCompanion(actor)) {
			return nullptr;
		}
		auto enemies = CollectStandingEnemiesFromSnapshot();
		auto state = TFD::Bleedout::RuntimeHost::BuildStateRefs();
		if (enemies.empty() && state.bleedBattleObservePending && *state.bleedBattleObservePending) {
			const float radius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 600.0f);
			auto followers = g_provider.collectBleedStandingFollowers ? g_provider.collectBleedStandingFollowers(radius) : std::vector<RE::Actor*>{};
			auto* preferredEnemy = g_provider.resolveAggressor ? g_provider.resolveAggressor() : nullptr;
			if (!preferredEnemy && g_provider.resolveLastEnemyTargetingPlayer) {
				preferredEnemy = g_provider.resolveLastEnemyTargetingPlayer(radius, 15.0);
			}
			if (!preferredEnemy && g_provider.findBestAggressor) {
				preferredEnemy = g_provider.findBestAggressor(radius);
			}
			enemies = CollectCurrentObservedEnemies(player, radius, preferredEnemy, followers);
		}
		if (enemies.empty()) {
			return nullptr;
		}
		RE::Actor* best = nullptr;
		float bestScore = std::numeric_limits<float>::max();
		const auto actorPos = actor->GetPosition();
		for (auto* enemy : enemies) {
			if (!enemy || enemy == actor || !IsStandingEnemyThresholdActorInternal(enemy) || IsObserverAlly(enemy)) {
				continue;
			}
			if (!enemy->Is3DLoaded()) {
				continue;
			}
			auto* enemyTarget = ResolveCurrentCombatTargetInternal(enemy);
			const bool targetsActor = enemyTarget == actor;
			const bool hostileToActor = enemy->IsHostileToActor(actor);
			const bool inCombat = enemy->IsInCombat();
			if (!targetsActor && !hostileToActor && !inCombat) {
				continue;
			}
			float score = Distance3D(actorPos, enemy->GetPosition());
			if (targetsActor) score -= 3000.0f;
			else if (enemyTarget && TFD::TeammateManager::IsActiveFollowerActor(enemyTarget)) score -= 1200.0f;
			if (hostileToActor) score -= 400.0f;
			if (inCombat) score -= 150.0f;
			if (g_observedPreferredEnemy) {
				auto preferredSp = RE::Actor::LookupByHandle(g_observedPreferredEnemy.native_handle());
				if (auto* preferred = preferredSp.get(); preferred && preferred == enemy) score -= 200.0f;
			}
			if (score < bestScore) {
				bestScore = score;
				best = enemy;
			}
		}
		return best;
	}

	bool HadValidObservedEnemy()
	{
		return g_observedBattle.hadValidObservedEnemy;
	}

	void HandleObservedBattleWin(const char* reason)
	{
		TFD::FlowController::HandleObservedBattleWin(reason ? reason : "battle_observe_win");
	}

	void HandleObservedLeftForDead(const char* reason)
	{
		TFD::FlowController::HandleObservedLeftForDead(reason ? reason : "battle_observe_loss");
	}

	void ClearCaptiveOrchestrationResidue(bool clearPendingFadeIn)
	{
		if (clearPendingFadeIn && g_provider.clearPendingFadeIn) {
			g_provider.clearPendingFadeIn();
		}
		if (g_provider.clearEscapeContext) {
			g_provider.clearEscapeContext();
		}
		if (g_provider.resetLockpickWatch) {
			g_provider.resetLockpickWatch();
		}
		if (g_provider.graceActive) {
			g_provider.graceActive->store(false, std::memory_order_release);
		}
		if (g_provider.prevDialogueOpen) {
			*g_provider.prevDialogueOpen = false;
		}
		if (g_provider.setPrevLockpickOpen) {
			g_provider.setPrevLockpickOpen(false);
		}
		if (g_provider.clearCaptiveRuntime) {
			g_provider.clearCaptiveRuntime();
		}
	}

	RE::Actor* ResolveBleedRuntimeSpeaker()
	{
		return g_provider.currentBleedSpeaker ? g_provider.currentBleedSpeaker() : nullptr;
	}

	void BeginBleedPleasureRuntime(RE::Actor* speaker, bool captive, const char* reason)
	{
		(void)TFD::PleasureRuntime::BeginPleasure(
			speaker,
			captive ? TFD::PleasureRuntime::SourceContext::Captive : TFD::PleasureRuntime::SourceContext::Bleedout,
			reason);
	}

	void ExitSystemEventRuntime()
	{
		auto state = TFD::Bleedout::RuntimeHost::BuildStateRefs();
		if (state.inBleedState) state.inBleedState->store(false, std::memory_order_release);
		if (state.minHp) *state.minHp = 0.0f;
		if (state.bleedLastSeconds) *state.bleedLastSeconds = -1;
		TFD::BleedoutGreet::ResetRuntime("bleed_reset");
	}

	DialogueHotkeyHandlers BuildDialogueHotkeyHandlers()
	{
		DialogueHotkeyHandlers handlers{};
		handlers.speaker = g_provider.buildSpeakerHandlers ? g_provider.buildSpeakerHandlers() : SpeakerLogicHandlers{};
		handlers.isBleedoutActive = []() { auto state = TFD::Bleedout::RuntimeHost::BuildStateRefs(); return state.inBleedState ? state.inBleedState->load(std::memory_order_acquire) : false; };
		handlers.isCaptiveEscapePhase = []() { return TFD::Captive::IsEscapeActive(); };
		handlers.isDialogueOpen = g_provider.isDialogueOpen;
		handlers.resolveSpeakerFromRuntime = []() -> RE::Actor* {
			if (!g_provider.currentBleedSpeakerId || g_provider.currentBleedSpeakerId() == 0) {
				return nullptr;
			}
			return g_provider.currentBleedSpeaker ? g_provider.currentBleedSpeaker() : nullptr;
		};
		handlers.resolveAggressor = g_provider.resolveAggressor;
		handlers.findBestAggressor = g_provider.findBestAggressor;
		handlers.releaseNoSpeakerTameSession = g_provider.releaseNoSpeakerTameSession;
		handlers.releaseTruceSession = g_provider.releaseTruceSession;
		handlers.startTruceSessionForSpeaker = g_provider.startTruceSessionForSpeaker;
		handlers.resetSpeakerKick = g_provider.resetSpeakerKick;
		handlers.resetGreetRuntime = g_provider.resetGreetRuntime;
		handlers.beginGreet = g_provider.beginGreet;
		return handlers;
	}

	bool BeginDialogueHotkey()
	{
		return TFD::Bleedout::RuntimeHost::BeginDialogueHotkey();
	}

	bool IsBleedoutActive()
	{
		auto state = TFD::Bleedout::RuntimeHost::BuildStateRefs();
		return state.inBleedState && state.inBleedState->load(std::memory_order_acquire);
	}

	bool IsPlayerBleedHoldTargetBlocked()
	{
		auto state = TFD::Bleedout::RuntimeHost::BuildStateRefs();
		if (!state.inBleedState || !state.inBleedState->load(std::memory_order_acquire)) {
			return false;
		}
		const bool pending = state.bleedBattleObservePending && *state.bleedBattleObservePending;
		const bool active = state.bleedBattleObserveActive && *state.bleedBattleObserveActive;
		return pending || active;
	}

	bool IsObservedCombatCommitInProgress()
	{
		return g_provider.isObservedCombatCommitInProgress && g_provider.isObservedCombatCommitInProgress();
	}

	void NoteEnemyTargetingPlayer(RE::Actor* actor)
	{
		if (g_provider.noteEnemyTargetingPlayer) {
			g_provider.noteEnemyTargetingPlayer(actor);
		}
	}

	void PreparePlayerForBleedoutPleasureScene(const char* reason)
	{
		auto* p = g_provider.getPlayer ? g_provider.getPlayer() : nullptr;
		if (!p || p->IsDead() || p->IsDisabled()) {
			return;
		}

		if (g_provider.releasePlayerBleedLock) {
			g_provider.releasePlayerBleedLock(reason ? reason : "bleed_pleasure_prepare", true);
		}
		p->NotifyAnimationGraph("BleedoutStop");
		p->NotifyAnimationGraph("GetUpStart");

		const float hpMax = (std::max)(1.0f, p->GetPermanentActorValue(RE::ActorValue::kHealth));
		const float threshPct = std::clamp(TFD::Settings::GetDefeatThresholdPct() / 100.0f, 0.05f, 0.95f);
		const float safeHealthPct = std::clamp(threshPct + 0.08f, 0.22f, 0.95f);
		const float healthTarget = (std::max)(35.0f, hpMax * safeHealthPct);
		const float hpNow = p->GetActorValue(RE::ActorValue::kHealth);
		if (hpNow < healthTarget) {
			p->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, (healthTarget - hpNow));
		}

		const float staminaMax = (std::max)(1.0f, p->GetPermanentActorValue(RE::ActorValue::kStamina));
		const float staminaTarget = (std::max)(30.0f, staminaMax * 0.45f);
		const float staminaNow = p->GetActorValue(RE::ActorValue::kStamina);
		if (staminaNow < staminaTarget) {
			p->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina, (staminaTarget - staminaNow));
		}

		std::unordered_set<std::uint32_t> stoppedForPleasure{};
		auto stopPleasureParticipant = [&](RE::Actor* actor, const char* stopReason) {
			if (!actor) {
				return;
			}
			const auto id = actor->GetFormID();
			if (id == 0 || !stoppedForPleasure.insert(id).second) {
				return;
			}
			HardStopActorCombatAndAlarm(actor, stopReason);
		};

		stopPleasureParticipant(p, reason ? reason : "bleed_pleasure_prepare_player");

		// R457A: preserve the Bleedout crowd before the OStim handoff clears the
		// physical Bleedout owner.  AfterPleasure Recruit may bridge the next crowd
		// actor into an InCombat ForceGreet, so the candidate pool must survive the
		// pleasure handoff without re-scanning wild hostility actors.
		CapturePleasureCrowdSnapshotFromAssigned(reason ? reason : "bleed_pleasure_prepare");

		// R250A: once the player has committed Bleedout -> Pleasure, Bleedout is
		// only source metadata.  Do not hard-stop the speaker/crowd here; doing so
		// can drop a drawn actor out of combat stance before PleasureFailed / Fight.
		const unsigned int skippedActorStops =
			(ResolveBleedRuntimeSpeaker() ? 1u : 0u) +
			static_cast<unsigned int>(GetBleedCrowdAssignedIDs().size());
		spdlog::info("[TFD][Bleedout][R250A] pleasure prepare actor hard stop skipped count={} reason={}",
			skippedActorStops,
			reason ? reason : "unknown");
		spdlog::info("[TFD][Bleedout][CB07] pleasure prepare participant hard stop count={} broadSweep=0 reason={}",
			static_cast<unsigned int>(stoppedForPleasure.size()),
			reason ? reason : "unknown");
		spdlog::info("[TFD][Defeat] player prepared for bleed pleasure scene reason={} hpTarget={:.2f} hpNow={:.2f}",
			reason ? reason : "unknown",
			healthTarget,
			p->GetActorValue(RE::ActorValue::kHealth));
	}

	void PreparePlayerForCaptivePleasureScene(const char* reason)
	{
		const char* why = reason ? reason : "captive_pleasure_prepare";
		const bool preserveReleasedWork = TFD::Captive::IsReleasedWorkActive();

		PreparePlayerForBleedoutPleasureScene(why);

		if (!preserveReleasedWork) {
			// R234B: Captive/Bleedout -> Pleasure is already a terminal scene handoff.
			// Do not reopen the standard Captive phase here; standard Captive runs
			// marker-radius escape checks and can restore aggression before OStim owns
			// the pair scene. Scene keeps Captive identity without escape ticking.
			TFD::Captive::SetRuntimeState(true, TFD::Captive::PhaseValue::Scene);
		}
		else {
			// R234B: preserve ReleasedWork so CompleteCaptivePleasureHandoff can keep
			// the work/escape-break context instead of clearing work aliases and letting
			// marker_radius convert the handoff into EscapeStarted.
			spdlog::info(
				"[TFD][Bleedout][R234B] captive pleasure prepare preserved ReleasedWork phase reason={}",
				why);
		}

		TFD::Captive::SyncPlayerAlias(g_provider.getPlayer ? g_provider.getPlayer() : nullptr, why);
	}

	RuntimeHostHandlers BuildLocalRuntimeHostHandlers()
	{
		RuntimeHostHandlers handlers{};
		handlers.clearTerminalCommit = [](const char* reason) { TFD::Bleedout::ClearTerminalCommit(reason); };
		handlers.clearBridgeAliasesForActor = [](RE::Actor* actor, const char* reason) { TFD::Bleedout::ClearBridgeAliases(actor, reason); };
		handlers.clearNoMarkerFallbackState = []() { TFD::Transition::ClearNoMarkerFallbackState(); };
		handlers.releaseNoSpeakerTameSession = g_provider.releaseNoSpeakerTameSession;
		handlers.clearBleedSupportBridgeAliases = g_provider.clearBleedSupportBridgeAliases;
		handlers.releaseTruceSession = g_provider.releaseTruceSession;
		handlers.resetGreetRuntime = g_provider.resetGreetRuntime;
		handlers.clearCaptorAliases = [](const char* reason) { TFD::Bleedout::ClearCaptorAliases(reason); };
		handlers.clearDialogueOutcome = [](const char* reason) { TFD::Bleedout::ClearDialogueOutcome(reason); };
		handlers.resetBattleObserveTracking = []() { ResetObservedRuntimeTracking(); };
		handlers.setPlayerBleedImmune = g_provider.setPlayerBleedImmune;
		handlers.clampHealth = g_provider.clampHealth;
		handlers.collectBleedStandingFollowers = g_provider.collectBleedStandingFollowers;
		handlers.computeBleedBattleEnemyScanRadius = [](RE::Actor* player, const std::vector<RE::Actor*>& allies, float baseRadius) { return ComputeObservedEnemyScanRadius(player, allies, baseRadius); };
		handlers.resolveAggressor = g_provider.resolveAggressor;
		handlers.resolveLastEnemyTargetingPlayer = g_provider.resolveLastEnemyTargetingPlayer;
		handlers.findBestAggressor = g_provider.findBestAggressor;
		handlers.isObserverAlly = [](RE::Actor* actor) { return IsObserverAlly(actor); };
		handlers.collectCurrentObservedEnemies = [](RE::Actor* player, float radius, RE::Actor* preferred, const std::vector<RE::Actor*>& allies) { return CollectCurrentObservedEnemies(player, radius, preferred, allies); };
		handlers.updateObserverRoster = [](RE::Actor* player, const std::vector<RE::Actor*>& followers, const std::vector<RE::Actor*>& enemies, RE::Actor* preferred) { UpdateObservedBattleRoster(player, followers, enemies, preferred); };
		handlers.collectStandingFollowersFromSnapshot = []() { return CollectStandingFollowersFromSnapshot(); };
		handlers.collectStandingEnemiesFromSnapshot = []() { return CollectStandingEnemiesFromSnapshot(); };
		handlers.hadValidObservedEnemy = []() { return HadValidObservedEnemy(); };
		handlers.resolveBleedFlowActorFormID = g_provider.resolveBleedFlowActorFormID;
		handlers.applyObservedDefeatResolution = g_provider.applyObservedDefeatResolution;
		handlers.enterObservedBattleWin = []() { TFD::FlowController::HandleObservedBattleWin("battle_observe_win"); };
		handlers.enterObservedLeftForDead = [](const char* reason) { TFD::FlowController::HandleObservedLeftForDead(reason ? reason : "battle_observe_loss"); };
		handlers.findBestSpeaker = g_provider.findBestSpeaker;
		handlers.isReasonableSpeaker = g_provider.isReasonableSpeaker;
		handlers.collectBleedoutCrowd = g_provider.collectBleedoutCrowd;
		handlers.isCaptiveSupportedAggressor = g_provider.isCaptiveSupportedAggressor;
		handlers.applyAllowedFactionFromAggressor = g_provider.applyAllowedFactionFromAggressor;
		handlers.resolveCaptiveMarkerForOutcome = g_provider.resolveCaptiveMarkerForOutcome;
		handlers.canUseCaptiveFallbackHeuristic = g_provider.canUseCaptiveFallbackHeuristic;
		handlers.tryEnsureNoSpeakerTameSession = g_provider.tryEnsureNoSpeakerTameSession;
		handlers.setLastAggressor = g_provider.setLastAggressor;
		handlers.setPrevDialogueOpen = [this_provider = &g_provider](bool v) {
			if (this_provider->prevDialogueOpen) *this_provider->prevDialogueOpen = v;
		};
		handlers.isBleedCrowdSupportedAggressor = g_provider.isBleedCrowdSupportedAggressor;
		handlers.isBleedSpaceCompatible = g_provider.isBleedSpaceCompatible;
		handlers.debugNotification = g_provider.debugNotification;
		handlers.clearEnemyTargetsToPlayerForDefeat = g_provider.clearEnemyTargetsToPlayerForDefeat;
		handlers.resolveEscapeBreakPreferredAggressor = g_provider.resolveEscapeBreakPreferredAggressor;
		handlers.startTruceSessionForSpeaker = g_provider.startTruceSessionForSpeaker;
		handlers.canUseAggressorForBleedoutGreet = g_provider.canUseAggressorForBleedoutGreet;
		handlers.applyDialogueOverdrive = g_provider.applyDialogueOverdrive;
		handlers.isStandingEnemyThresholdActor = g_provider.isStandingEnemyThresholdActor;
		return handlers;
	}

	bool HandlePendingEscapeBreak()
	{
		return TFD::Bleedout::RuntimeHost::HandlePendingEscapeBreak();
	}

	void MaintainSpeakerKick()
	{
		TFD::Bleedout::RuntimeHost::MaintainSpeakerKick();
	}
}
