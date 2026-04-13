#include "TFDBleedout.h"
#include "TFDActor.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <array>
#include <limits>
#include <atomic>
#include <string>
#include <unordered_set>
#include <vector>
#include <utility>
#include <thread>

#include "TFDFlowController.h"
#include "TFDSettings.h"
#include "TFDHostilityController.h"
#include "TFDTame.h"
#include "TFDBleedoutGreet.h"
#include "RE/B/BGSRefAlias.h"
#include "RE/T/TESQuest.h"

namespace TFD::Bleedout
{
	bool ActorHasAllowListFaction(RE::Actor* actor);

	namespace
	{
		using Clock = std::chrono::steady_clock;

		std::uint32_t ActorFormID(RE::Actor* actor)
		{
			return actor ? actor->GetFormID() : 0u;
		}

		constexpr const char* kPrimeSpeakerEvent = "TFDBleedoutPrimeSpeaker";

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

		std::atomic_bool g_inBleedState{ false };
		float g_minHp{ 0.0f };
		Clock::time_point g_bleedStart{};
		int g_bleedLastSeconds = -1;
		bool g_bleedPaused = false;
		Clock::time_point g_bleedPauseStarted{};
		Clock::time_point g_bleedLastCalmPulse{};
		Clock::time_point g_bleedLastCrowdAssign{};
		std::vector<std::uint32_t> g_bleedCrowdAssigned{};
		Clock::time_point g_bleedNoSpeakerTameLastAttempt{};
		std::uint32_t g_bleedSpeakerId = 0;
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
		bool g_bleedBattleObserveActive = false;
		Clock::time_point g_bleedBattleObserveSince{};
		Clock::time_point g_bleedBattleObserveLastRedirect{};
		int g_bleedBattleObserveActiveEmptyEnemyTicks = 0;
		std::uint32_t g_truceSessionId = 0;
		std::uint32_t g_noSpeakerTameSessionId = 0;
		std::uint32_t g_noSpeakerTamePrimaryId = 0;

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

			for (auto* baseAlias : g_bleedoutQuestRegistry.quest->aliases) {
				auto* refAlias = skyrim_cast<RE::BGSRefAlias*>(baseAlias);
				if (!refAlias) {
					continue;
				}
				const auto aliasName = std::string(refAlias->aliasName.c_str());
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

		auto* player = handlers.getPlayer ? handlers.getPlayer() : nullptr;
		if (!player) {
			return {};
		}

		const float scanRadius = (std::max)(radius, 2000.0f);
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
				if (!actor || actor->IsDead() || actor->IsDisabled()) continue;
				if (!actor->Is3DLoaded()) continue;
				if (actor->GetFormID() == player->GetFormID()) continue;
				if (!handlers.isBleedSpaceCompatible || !handlers.isBleedSpaceCompatible(actor, player)) continue;
				if (!handlers.isBleedCrowdSupportedAggressor || !handlers.isBleedCrowdSupportedAggressor(actor)) continue;
				const float dist = Distance3D(actor, player);
				if (dist > scanRadius) continue;
				const bool preserved = preserveAssigned && handlers.isPreservedAssigned && handlers.isPreservedAssigned(actor);
				float score = dist;
				if (preserved) score -= 90.0f;
				if (handlers.isActorCloseAndFront && handlers.isActorCloseAndFront(actor, player, 320.0f)) score -= 120.0f;
				scored.emplace_back(score, actor);
			}
		}

		if (scored.empty()) {
			auto fallbackSnapshot = TFD::Actor::BuildSnapshot(scanRadius, false);
			for (const auto& info : fallbackSnapshot.actors) {
				auto* actor = info.get();
				if (!actor || actor->IsDead() || actor->IsDisabled()) continue;
				if (!actor->Is3DLoaded()) continue;
				if (actor->GetFormID() == player->GetFormID()) continue;
				if (!handlers.isBleedSpaceCompatible || !handlers.isBleedSpaceCompatible(actor, player)) continue;
				if (!handlers.isBleedCrowdSupportedAggressor || !handlers.isBleedCrowdSupportedAggressor(actor)) continue;
				if (info.dist > scanRadius) continue;

				const bool targetingPlayer = info.hostileToPlayer || info.inCombat || actor->IsInCombat() || actor->IsHostileToActor(player);
				const bool preserved = preserveAssigned && handlers.isPreservedAssigned && handlers.isPreservedAssigned(actor);
				if (!targetingPlayer && !preserved && actor != preferred) continue;

				float score = info.dist;
				if (actor == preferred) score -= 1000.0f;
				if (targetingPlayer) score -= 140.0f;
				if (info.hostileToPlayer) score -= 80.0f;
				if (info.inCombat || actor->IsInCombat()) score -= 60.0f;
				if (preserved) score -= 90.0f;
				if (handlers.isActorCloseAndFront && handlers.isActorCloseAndFront(actor, player, 320.0f)) score -= 120.0f;
				scored.emplace_back(score, actor);
			}
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

		return result;
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
			return false;
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
	bool& BleedBattleObserveActiveRef() { return g_bleedBattleObserveActive; }
	std::chrono::steady_clock::time_point& BleedBattleObserveSinceRef() { return g_bleedBattleObserveSince; }
	std::chrono::steady_clock::time_point& BleedBattleObserveLastRedirectRef() { return g_bleedBattleObserveLastRedirect; }
	int& BleedBattleObserveActiveEmptyEnemyTicksRef() { return g_bleedBattleObserveActiveEmptyEnemyTicks; }

	void ClearBridgeAliases(RE::TESForm* sender, const char* reason)
	{
		const bool queued = SendBridgeModEvent("TFDBleedoutClearAll", sender);
		spdlog::info("[TFD][BleedBridge] ClearAll reason={} queued={}", reason ? reason : "unknown", queued);
	}

	void AssignBridgeActor(RE::Actor* actor)
	{
		if (!actor) {
			return;
		}

		const bool queued = SendBridgeModEvent("TFDBleedoutAssign", actor);
		spdlog::info("[TFD][BleedBridge] Assign actor={:08X} queued={}", actor->GetFormID(), queued);
	}

	void PrimeBridgeActor(RE::Actor* actor, const char* reason)
	{
		if (!actor) {
			return;
		}
		const bool queued = SendBridgeModEvent(kPrimeSpeakerEvent, actor);
		spdlog::info("[TFD][BleedBridge] Prime speaker actor={:08X} queued={} reason={}",
			actor->GetFormID(),
			queued,
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

	void ClearSupportBridgeAliases(const char* reason, const SupportBridgeHandlers& handlers)
	{
		const bool preCombatQueued = handlers.queuePreCombatClearAll ? handlers.queuePreCombatClearAll() : false;
		const bool truceQueued = handlers.queueTruceClearAll ? handlers.queueTruceClearAll() : false;
		const bool inCombatQueued = handlers.queueInCombatClearAll ? handlers.queueInCombatClearAll() : false;
		if (handlers.cancelAllPreCombat) {
			handlers.cancelAllPreCombat();
		}
		spdlog::info("[TFD][BleedBridge] ClearSupport reason={} preCombatQueued={} truceQueued={} inCombatQueued={}",
			reason ? reason : "unknown",
			preCombatQueued ? 1 : 0,
			truceQueued ? 1 : 0,
			inCombatQueued ? 1 : 0);
	}

	bool StartTruceSessionForSpeaker(RE::Actor* player, RE::Actor* speaker, const char* reason, RuntimeHostStateRefs state, const RuntimeHostHandlers& handlers)
	{
		if (!player || !speaker || speaker->IsDead() || speaker->IsDisabled()) {
			return false;
		}
		const auto speakerID = speaker->GetFormID();
		const bool sameActiveCaptor = GetActiveCaptorFormID() != 0 && GetActiveCaptorFormID() == speakerID;
		if (handlers.clearBleedSupportBridgeAliases) {
			handlers.clearBleedSupportBridgeAliases(reason ? reason : "bleed_retry");
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
		if (speaker->IsInCombat()) {
			speaker->StopCombat();
		}
		if (auto* process = RE::ProcessLists::GetSingleton()) {
			process->StopCombatAndAlarmOnActor(speaker, false);
		}
		if (speaker->IsWeaponDrawn()) {
			speaker->DrawWeaponMagicHands(false);
		}
		auto sessionId = TFD::HostilityController::BeginTruceInCombatSession(player, speaker, 0.0, true, false, true);
		const bool preserveDialogueSession = state.inBleedState && state.inBleedState->load(std::memory_order_relaxed);
		if (!sessionId.has_value() && preserveDialogueSession) {
			spdlog::info("[TFD][Defeat] bleed speaker restart retry ignoreSpent actor={:08X} reason={}",
				speaker->GetFormID(),
				reason ? reason : "unknown");
			sessionId = TFD::HostilityController::BeginTruceInCombatSession(player, speaker, 0.0, true, true, true);
		}
		if (!sessionId.has_value()) {
			spdlog::warn("[TFD][Defeat] bleed speaker restart rejected actor={:08X} reason={}",
				speaker->GetFormID(),
				reason ? reason : "unknown");
			return false;
		}
		g_truceSessionId = *sessionId;
		ApplyDialogueOverdrive(player, speaker, reason ? reason : "bleed_retry", false, state, handlers);
		speaker->EvaluatePackage(false, true);
		speaker->EvaluatePackage(true, true);
		spdlog::info("[TFD][Defeat] bleed speaker restart id={} actor={:08X} reason={}",
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

		if (speaker->IsInCombat()) {
			speaker->StopCombat();
		}
		if (auto* process = RE::ProcessLists::GetSingleton()) {
			process->StopCombatAndAlarmOnActor(speaker, false);
		}
		if (speaker->IsWeaponDrawn()) {
			speaker->DrawWeaponMagicHands(false);
		}

		if (!IsCaptorAliasPrimary(speaker)) {
			BindCaptorAliases(speaker, reason ? reason : "bleed_dialogue_overdrive");
		}
		PrimeBridgeActor(speaker, reason ? reason : "bleed_dialogue_overdrive");

		if (state.bleedCrowdAssigned) state.bleedCrowdAssigned->clear();
		if (state.bleedLastCrowdAssign) *state.bleedLastCrowdAssign = Clock::now();

		std::optional<RE::FormID> burstId;
		const bool preserveDialogueSession = state.inBleedState && state.inBleedState->load(std::memory_order_relaxed);
		if (!preserveDialogueSession) {
			burstId = TFD::HostilityController::BeginCellTruceBurst(player, speaker, 0.0, 2.5, 12000.0f);
		}
		else {
			spdlog::info("[TFD][Defeat] bleed dialogue overdrive preserve dialogue session speaker={:08X} reason={}",
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

		spdlog::info("[TFD][Defeat] bleed dialogue overdrive speaker={:08X} crowdSize=1 burst={} restartDialogue={} reason={}",
			speaker->GetFormID(),
			burstId.has_value() ? 1 : 0,
			restartDialogue ? 1 : 0,
			reason ? reason : "unknown");
	}

	void ReleaseTruceSession(TFD::Tame::ReleaseReason reason, std::uint32_t* bleedSpeakerId)
	{
		if (g_truceSessionId != 0) {
			TFD::HostilityController::ReleaseSession(g_truceSessionId, reason);
			spdlog::info("[TFD][Defeat] bleed truce session released id={} reason={}",
				g_truceSessionId,
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
			TFD::HostilityController::ReleaseSession(g_noSpeakerTameSessionId, TFD::Tame::ReleaseReason::Generic);
			spdlog::info("[TFD][Defeat] bleed no-speaker tame session released id={} primary={:08X} reason={}",
				g_noSpeakerTameSessionId,
				g_noSpeakerTamePrimaryId,
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
			TFD::HostilityController::IsSuppressed(primary) &&
			TFD::HostilityController::GetMode(primary) == TFD::HostilityController::Mode::Tame) {
			return true;
		}
		if (g_noSpeakerTameSessionId != 0) {
			ReleaseNoSpeakerTameSession("restart");
		}
		player->DrawWeaponMagicHands(false);
		auto sessionId = TFD::Tame::BeginSession(player, primary, 0.0, false);
		if (!sessionId.has_value()) {
			spdlog::info("[TFD][Defeat] bleed no-speaker tame session rejected primary={:08X} reason={}",
				primary->GetFormID(),
				reason ? reason : "unknown");
			return false;
		}
		g_noSpeakerTameSessionId = *sessionId;
		g_noSpeakerTamePrimaryId = primary->GetFormID();
		spdlog::info("[TFD][Defeat] bleed no-speaker tame session id={} primary={:08X} crowdSize={} reason={}",
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
				spdlog::info("[TFD][Defeat] promoted next bleed speaker actor={:08X} reason={}", actorID, reason ? reason : "unknown");
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
		if (handlers.applyCalmBubble) {
			handlers.applyCalmBubble(ResolveCalmRadius(handlers));
		}
		if (handlers.updatePreCombatState) {
			handlers.updatePreCombatState();
		}
		if (handlers.beginPleasure) {
			handlers.beginPleasure(handlers.resolveRuntimeSpeaker ? handlers.resolveRuntimeSpeaker() : nullptr, true, why);
		}
		spdlog::info("[TFD][Bleedout] captive pleasure handoff complete reason={}", why);
		return true;
	}

	bool CompletePayRelease(const char* reason, const CompletionHandlers& handlers)
	{
		const char* why = reason ? reason : "bleed_pay_release";
		if (handlers.clearOutcomeWindow) {
			handlers.clearOutcomeWindow(why);
		}
		if (handlers.tryBeginTerminalCommit && !handlers.tryBeginTerminalCommit(TerminalCommit::PayRelease, why)) {
			return false;
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
			handlers.beginLeftForDeadCooldown(8);
		}
		if (handlers.setGraceSeconds) {
			handlers.setGraceSeconds(8);
		}
		if (handlers.updatePreCombatState) {
			handlers.updatePreCombatState();
		}
		spdlog::info("[TFD][Bleedout] pay release complete reason={}", why);
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
			handlers.clearBridgeAliases(why);
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
			handlers.beginLeftForDeadCooldown(8);
		}
		if (handlers.setGraceSeconds) {
			handlers.setGraceSeconds(8);
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
		if (handlers.releaseTruceGeneric) {
			handlers.releaseTruceGeneric();
		}
		if (context.pendingCaptive && handlers.resolveCaptiveMarker && handlers.resolveCaptiveMarker()) {
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
		if (handlers.hasBlockingCommit && handlers.hasBlockingCommit()) {
			spdlog::info("[TFD][Bleedout] blackout teleport suppressed activeTerminalCommit={}",
				handlers.getBlockingCommitName ? handlers.getBlockingCommitName() : "unknown");
			return false;
		}
		if (handlers.resolveCaptiveMarker && !handlers.resolveCaptiveMarker()) {
			if (handlers.enterNonCaptiveChoice) {
				handlers.enterNonCaptiveChoice("marker_not_found");
			}
			return false;
		}
		if (handlers.tryBeginCaptiveCommit && !handlers.tryBeginCaptiveCommit(why)) {
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
		if (handlers.completeCaptiveTransitionNow) {
			handlers.completeCaptiveTransitionNow("captive_blackout_fallback");
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		if (handlers.hideBlackoutFader) {
			handlers.hideBlackoutFader();
		}
		return true;
	}

	bool BeginWindow(RE::Actor* speaker, const char* reason)
	{
		auto& flow = TFD::FlowController::Controller::GetSingleton();
		const bool ok = flow.BeginPlayerBleedoutDecision(ActorFormID(speaker), reason ? reason : "bleed_window_start");
		spdlog::info("[TFD][Bleedout] BeginWindow actor={:08X} ok={} reason={}", ActorFormID(speaker), ok ? 1 : 0, reason ? reason : "bleed_window_start");
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
		auto& flow = TFD::FlowController::Controller::GetSingleton();
		return flow.BeginAfterPleasure(ActorFormID(actor), reason ? reason : "bleedout_after_pleasure");
	}

	bool CompleteAfterPleasure(const char* reason)
	{
		auto& flow = TFD::FlowController::Controller::GetSingleton();
		return flow.CompleteAfterPleasure(reason ? reason : "bleedout_after_pleasure_complete");
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

		if (handlers.clearTerminalCommit) handlers.clearTerminalCommit("start_bleed_window");
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
		if (state.bleedBattleObserveActive) *state.bleedBattleObserveActive = false;
		if (state.bleedBattleObserveSince) *state.bleedBattleObserveSince = {};
		if (state.bleedBattleObserveLastRedirect) *state.bleedBattleObserveLastRedirect = {};
		if (state.bleedBattleObserveActiveEmptyEnemyTicks) *state.bleedBattleObserveActiveEmptyEnemyTicks = 0;

		const float maxHp = player->GetPermanentActorValue(RE::ActorValue::kHealth);
		const float minHp = (std::max)(1.0f, maxHp * 0.02f);
		if (state.minHp) *state.minHp = minHp;
		if (handlers.setPlayerBleedImmune) handlers.setPlayerBleedImmune(true);
		if (handlers.clampHealth) handlers.clampHealth(player, minHp);
		player->NotifyAnimationGraph("BleedoutStart");

		const float radius = (std::max)(12000.0f, TFD::Settings::GetSweepRadius());
		const float maxSpeakerDist = 768.0f;
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

		auto initialCrowd = handlers.collectBleedoutCrowd ? handlers.collectBleedoutCrowd(radius, aggressor, false) : std::vector<RE::Actor*>{};
		if (aggressor) {
			if (handlers.setLastAggressor) handlers.setLastAggressor(aggressor);
			if (state.bleedSpeakerId) *state.bleedSpeakerId = aggressor->GetFormID();
			spdlog::info("[TFD][Bleedout] bleed speaker staged {:08X} crowdCandidates={} (awaiting hotkey)", aggressor->GetFormID(), initialCrowd.size());

			if (!handlers.isCaptiveSupportedAggressor || !handlers.isCaptiveSupportedAggressor(aggressor)) {
				if (state.bleedPendingCaptiveOutcome) *state.bleedPendingCaptiveOutcome = false;
				if (state.bleedPendingNonCaptiveOutcome) *state.bleedPendingNonCaptiveOutcome = true;
				spdlog::info("[TFD][Bleedout] aggressor {:08X} not captive-supported -> pending=noncaptive", aggressor->GetFormID());
			}
			else {
				const bool allowlistSupported = handlers.applyAllowedFactionFromAggressor ? handlers.applyAllowedFactionFromAggressor(aggressor) : false;
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
		char msg[96]{};
		std::snprintf(msg, sizeof(msg), "TFDEngine: Bleeding... (%ds)", bleedSeconds);
		if (handlers.debugNotification) handlers.debugNotification(msg);
		spdlog::info("[TFD][Bleedout] bleed window started ({}s)", bleedSeconds);
	}

	bool StartRuntimeBattleObservePending(RuntimeHostStateRefs state, RE::Actor* player, const RuntimeHostHandlers& handlers)
	{
		if (!player) {
			return false;
		}
		if (handlers.clearTerminalCommit) handlers.clearTerminalCommit("start_bleed_observe_pending");
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
		if (state.bleedBattleObservePendingLastRedirect) *state.bleedBattleObservePendingLastRedirect = {};
		if (state.bleedBattleObservePendingEmptyEnemyTicks) *state.bleedBattleObservePendingEmptyEnemyTicks = 0;
		if (state.bleedBattleObserveActive) *state.bleedBattleObserveActive = false;
		if (state.bleedBattleObserveSince) *state.bleedBattleObserveSince = {};
		if (state.bleedBattleObserveLastRedirect) *state.bleedBattleObserveLastRedirect = {};
		if (state.bleedBattleObserveActiveEmptyEnemyTicks) *state.bleedBattleObserveActiveEmptyEnemyTicks = 0;

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
		const float radius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 600.0f);
		auto followers = handlers.collectBleedStandingFollowers ? handlers.collectBleedStandingFollowers(radius) : std::vector<RE::Actor*>{};
		if (followers.empty()) {
			if (state.bleedBattleObservePending) *state.bleedBattleObservePending = false;
			if (handlers.enterObservedLeftForDead) handlers.enterObservedLeftForDead("battle_observe_pending_no_followers");
			return;
		}
		const float enemyScanRadius = handlers.computeBleedBattleEnemyScanRadius ? handlers.computeBleedBattleEnemyScanRadius(player, followers, radius) : radius;
		auto* preferredEnemy = handlers.resolveAggressor ? handlers.resolveAggressor() : nullptr;
		if (!preferredEnemy && handlers.resolveLastEnemyTargetingPlayer) preferredEnemy = handlers.resolveLastEnemyTargetingPlayer(enemyScanRadius, 15.0);
		if (!preferredEnemy && handlers.findBestAggressor) preferredEnemy = handlers.findBestAggressor(enemyScanRadius);
		if (preferredEnemy && handlers.isObserverAlly && handlers.isObserverAlly(preferredEnemy)) preferredEnemy = nullptr;
		auto enemies = handlers.collectCurrentObservedEnemies ? handlers.collectCurrentObservedEnemies(player, radius, preferredEnemy, followers) : std::vector<RE::Actor*>{};
		if (handlers.updateObserverRoster) handlers.updateObserverRoster(player, followers, enemies, preferredEnemy);
		auto rosterEnemies = handlers.collectStandingEnemiesFromSnapshot ? handlers.collectStandingEnemiesFromSnapshot() : std::vector<RE::Actor*>{};
		const auto now = Clock::now();
		if (!rosterEnemies.empty()) {
			if (state.bleedBattleObservePendingEmptyEnemyTicks) *state.bleedBattleObservePendingEmptyEnemyTicks = 0;
			if (state.bleedBattleObservePendingUntil) *state.bleedBattleObservePendingUntil = now + std::chrono::milliseconds(750);
			return;
		}
		if (state.bleedBattleObservePendingUntil && now < *state.bleedBattleObservePendingUntil) {
			return;
		}
		if (state.bleedBattleObservePendingEmptyEnemyTicks) {
			++(*state.bleedBattleObservePendingEmptyEnemyTicks);
			if (*state.bleedBattleObservePendingEmptyEnemyTicks < 8) {
				if (state.bleedBattleObservePendingUntil) *state.bleedBattleObservePendingUntil = now + std::chrono::milliseconds(500);
				return;
			}
		}
		if (state.bleedBattleObservePending) *state.bleedBattleObservePending = false;
		if (handlers.hadValidObservedEnemy && handlers.hadValidObservedEnemy() && !followers.empty()) {
			if (handlers.enterObservedBattleWin) handlers.enterObservedBattleWin();
		}
		else {
			if (handlers.enterObservedLeftForDead) handlers.enterObservedLeftForDead("battle_observe_no_survivor");
		}
	}

	void TickRuntimeBattleObserve(RuntimeHostStateRefs state, RE::Actor* player, const RuntimeHostHandlers& handlers)
	{
		if (!player) return;
		if (handlers.setPlayerBleedImmune) handlers.setPlayerBleedImmune(true);
		if (state.minHp && *state.minHp > 0.0f && handlers.clampHealth) handlers.clampHealth(player, *state.minHp);
		auto followers = handlers.collectStandingFollowersFromSnapshot ? handlers.collectStandingFollowersFromSnapshot() : std::vector<RE::Actor*>{};
		auto enemies = handlers.collectStandingEnemiesFromSnapshot ? handlers.collectStandingEnemiesFromSnapshot() : std::vector<RE::Actor*>{};
		if (followers.empty()) {
			if (handlers.enterObservedLeftForDead) handlers.enterObservedLeftForDead("battle_observe_loss");
			return;
		}
		if (enemies.empty()) {
			if (state.bleedBattleObserveActiveEmptyEnemyTicks) ++(*state.bleedBattleObserveActiveEmptyEnemyTicks);
			if (state.bleedBattleObserveActiveEmptyEnemyTicks && *state.bleedBattleObserveActiveEmptyEnemyTicks < 8) return;
			if (handlers.hadValidObservedEnemy && handlers.hadValidObservedEnemy() && !followers.empty()) {
				if (handlers.enterObservedBattleWin) handlers.enterObservedBattleWin();
			}
			else {
				if (handlers.enterObservedLeftForDead) handlers.enterObservedLeftForDead("battle_observe_no_survivor");
			}
			return;
		}
		if (state.bleedBattleObserveActiveEmptyEnemyTicks) *state.bleedBattleObserveActiveEmptyEnemyTicks = 0;
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
		if (pct > thresh) {
			setPending(false);
			spdlog::info("[TFD][Bleedout] pending escape-break rebleed canceled pct={:.1f} thresh={:.1f}", pct, thresh);
			return false;
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

	void MaintainRuntimeSpeakerKick(RuntimeHostStateRefs state, RE::Actor* player, const RuntimeHostHandlers& handlers)
	{
		if (!state.inBleedState || !state.inBleedState->load(std::memory_order_acquire)) return;
		if (!state.bleedSpeakerId || *state.bleedSpeakerId == 0) return;
		if (!state.bleedPaused || *state.bleedPaused) return;
		if (!state.bleedStart || state.bleedStart->time_since_epoch().count() == 0) return;
		const auto now = Clock::now();
		if (state.bleedSpeakerKickCount && *state.bleedSpeakerKickCount >= 2) return;
		if (state.bleedSpeakerKickCount && *state.bleedSpeakerKickCount == 0) {
			if (std::chrono::duration_cast<std::chrono::milliseconds>(now - *state.bleedStart).count() < 350) return;
		}
		else if (state.bleedSpeakerKickLast && state.bleedSpeakerKickLast->time_since_epoch().count() != 0) {
			if (std::chrono::duration_cast<std::chrono::milliseconds>(now - *state.bleedSpeakerKickLast).count() < 900) return;
		}
		auto* actor = RE::TESForm::LookupByID<RE::Actor>(*state.bleedSpeakerId);
		float dist = 99999.0f;
		if (!player || !handlers.isReasonableSpeaker || !handlers.isReasonableSpeaker(actor, player, 1400.0f, &dist)) return;
		if (handlers.applyDialogueOverdrive) handlers.applyDialogueOverdrive(player, actor, "speaker_prime_retry", true);
		if (state.bleedSpeakerKickLast) *state.bleedSpeakerKickLast = now;
		if (state.bleedSpeakerKickCount) ++(*state.bleedSpeakerKickCount);
	}

	bool IsActive()
	{
		return TFD::FlowController::Controller::GetSingleton().IsBleedDecisionActive();
	}

	bool OwnsCurrentFlow()
	{
		const auto snapshot = TFD::FlowController::Controller::GetSingleton().GetSnapshot();
		return snapshot.root == TFD::FlowController::RootFlow::Bleedout ||
			snapshot.contextRoot == TFD::FlowController::RootFlow::Bleedout ||
			snapshot.sub == TFD::FlowController::SubFlow::BleedoutPleasure ||
			snapshot.sub == TFD::FlowController::SubFlow::BleedoutAfterPleasure;
	}

	std::uint32_t ResolveActorFormID(RE::Actor* actor)
	{
		return ActorFormID(actor);
	}
}
