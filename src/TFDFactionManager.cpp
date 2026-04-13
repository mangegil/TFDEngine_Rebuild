#include "TFDFactionManager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

namespace TFD::FactionManager
{
	namespace
	{
		struct AllowedFactionEntry
		{
			const char* editorID = nullptr;
			RE::TESFaction* faction = nullptr;
		};

		struct TruceQuestRegistryCache
		{
			RE::TESQuest* quest{ nullptr };
			std::array<RE::BGSRefAlias*, 10> truceAliases{};
			bool resolved{ false };
		};

		struct DefeatedEnemyRegistryCache
		{
			RE::TESQuest* quest{ nullptr };
			RE::TESFaction* faction{ nullptr };
			std::array<RE::BGSRefAlias*, 10> enemyAliases{};
			bool resolved{ false };
		};

		struct ReleaseFollowGraceEntry
		{
			RE::ActorHandle actor{};
			std::chrono::steady_clock::time_point expiresAt{};
			std::string source{};
		};

		bool g_initialized = false;
		bool g_active = false;
		int g_matchCount = 0;
		std::uint32_t g_sourceActorFormID = 0;

		std::vector<AllowedFactionEntry> g_allowedFactions{};
		TruceQuestRegistryCache g_truceQuestRegistry{};
		DefeatedEnemyRegistryCache g_defeatedEnemyRegistry{};
		std::unordered_map<RE::FormID, ReleaseFollowGraceEntry> g_releaseFollowGraceEntries{};

		static RE::TESGlobal* g_joinEnemyStateGlobal = nullptr;
		static RE::TESFaction* g_releaseFollowHelperFaction = nullptr;
		static RE::TESFaction* g_dialogueHelperFaction = nullptr;

		static void ResolveJoinEnemyStateGlobal()
		{
			if (!g_joinEnemyStateGlobal) {
				g_joinEnemyStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDJoinEnemyState");
			}
		}

		static void SetJoinEnemyState(int value)
		{
			ResolveJoinEnemyStateGlobal();
			if (g_joinEnemyStateGlobal) {
				g_joinEnemyStateGlobal->value = static_cast<float>(value);
			}
		}

		static constexpr const char* kAllowedFactionEditorIDs[] = {
			"BanditFaction",
			"ForswornFaction",
			"NecromancerFaction",
			"WarlockFaction",
			"WitchFaction",
			"VampireFaction",
			"DLC1VampireFaction",
			"SilverHandFaction",
			"ThalmorFaction",
			"AlikrFaction",
			"BloodHorkerFaction",
			"MS06BanditFaction",
			"WEPlayerEnemyFaction",
			"dunMistwatchBanditFaction",
			"dunCragslaneFaction",
			"dunTrevasBanditFaction",
			"dunFellglowWarlockFaction",
			"dunBrokenOarFaction",
			"dunWhiteRiverFaction",
			"dunValtheimFaction",
			"dunBannermistFaction",
			"dunHaltedStreamFaction"
		};

		static std::int8_t GetExactFactionRank(RE::Actor* actor, RE::TESFaction* faction)
		{
			if (!actor || !faction) {
				return -2;
			}

			return static_cast<std::int8_t>(actor->GetFactionRank(faction, false));
		}

		static bool HasExactFaction(RE::Actor* actor, RE::TESFaction* faction)
		{
			return GetExactFactionRank(actor, faction) > -2;
		}

		static void ResetState(bool resetGlobal)
		{
			g_active = false;
			g_matchCount = 0;
			g_sourceActorFormID = 0;
			if (resetGlobal) {
				SetJoinEnemyState(0);
			}
		}

		static void AddAllowedFaction(const char* editorID)
		{
			if (!editorID || !editorID[0]) {
				return;
			}

			auto* faction = RE::TESForm::LookupByEditorID<RE::TESFaction>(editorID);
			if (!faction) {
				spdlog::warn("[TFD][FactionManager] unresolved allowlist faction '{}'", editorID);
				return;
			}

			const auto it = std::find_if(
				g_allowedFactions.begin(),
				g_allowedFactions.end(),
				[faction](const AllowedFactionEntry& e) { return e.faction == faction; });

			if (it != g_allowedFactions.end()) {
				return;
			}

			g_allowedFactions.push_back({ editorID, faction });
		}
		static auto Now()
		{
			return std::chrono::steady_clock::now();
		}

		static RE::PlayerCharacter* Player()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

		static RE::Actor* ResolveCurrentCombatTarget(RE::Actor* actor)
		{
			if (!actor) {
				return nullptr;
			}
			auto sp = actor->GetActorRuntimeData().currentCombatTarget.get();
			return sp.get();
		}

		static void ResolveReleaseFollowHelperFaction()
		{
			if (!g_releaseFollowHelperFaction) {
				g_releaseFollowHelperFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDTruceTeammateFaction");
				if (!g_releaseFollowHelperFaction) {
					spdlog::warn("[TFD][FactionManager] helper faction not found editorId=TFDTruceTeammateFaction");
				}
			}
			if (!g_dialogueHelperFaction) {
				g_dialogueHelperFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDPacifyFaction");
				if (!g_dialogueHelperFaction) {
					spdlog::warn("[TFD][FactionManager] dialogue helper faction not found editorId=TFDPacifyFaction");
				}
			}
		}

		static bool ActorHasActiveDialoguePhaseFaction(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			constexpr const char* kPhaseFactionEditorIds[] = {
				"TFDPreCombatTruceFaction",
				"TFDInCombatTruceFaction",
				"TFDBleedOutFaction",
				"TFDBleedoutFaction",
				"TFDCaptiveFaction",
				"TFDWorkingCaptiveFaction",
				"TFDAfterPleasureFaction",
				"TFDSaviorFaction"
			};
			for (auto* editorID : kPhaseFactionEditorIds) {
				auto* faction = RE::TESForm::LookupByEditorID<RE::TESFaction>(editorID);
				if (faction && actor->IsInFaction(faction)) {
					return true;
				}
			}
			return false;
		}

		static void ResolveDefeatedEnemyRegistry()
		{
			if (g_defeatedEnemyRegistry.resolved) {
				return;
			}
			g_defeatedEnemyRegistry.resolved = true;
			g_defeatedEnemyRegistry.quest = RE::TESForm::LookupByEditorID<RE::TESQuest>("TFDDefeatedEnemyQuest");
			g_defeatedEnemyRegistry.faction = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDDefeatedFaction");
			if (!g_defeatedEnemyRegistry.quest) {
				spdlog::warn("[TFD][FactionManager] defeated enemy registry quest not found");
				return;
			}

			for (auto* baseAlias : g_defeatedEnemyRegistry.quest->aliases) {
				auto* refAlias = skyrim_cast<RE::BGSRefAlias*>(baseAlias);
				if (!refAlias) {
					continue;
				}
				const auto aliasName = std::string(refAlias->aliasName.c_str());
				if (aliasName.rfind("Enemy", 0) != 0 || aliasName.size() < 6) {
					continue;
				}
				try {
					int slot = std::stoi(aliasName.substr(5));
					if (slot >= 1 && slot <= static_cast<int>(g_defeatedEnemyRegistry.enemyAliases.size())) {
						g_defeatedEnemyRegistry.enemyAliases[static_cast<std::size_t>(slot) - 1] = refAlias;
					}
				} catch (...) {}
			}

			std::size_t found = 0;
			for (auto* alias : g_defeatedEnemyRegistry.enemyAliases) {
				if (alias) {
					++found;
				}
			}
			spdlog::info("[TFD][FactionManager] defeated enemy registry resolved quest={:08X} faction={:08X} aliases={}",
				g_defeatedEnemyRegistry.quest ? g_defeatedEnemyRegistry.quest->GetFormID() : 0u,
				g_defeatedEnemyRegistry.faction ? g_defeatedEnemyRegistry.faction->GetFormID() : 0u,
				found);
		}

		static void WriteDefeatedEnemyAlias(RE::BGSRefAlias* alias, RE::Actor* actor)
		{
			ResolveDefeatedEnemyRegistry();
			if (!g_defeatedEnemyRegistry.quest || !alias) {
				return;
			}

			RE::ObjectRefHandle handle{};
			if (actor) {
				handle = actor->CreateRefHandle();
			}

			RE::BSWriteLockGuard lock(g_defeatedEnemyRegistry.quest->aliasAccessLock);
			auto it = g_defeatedEnemyRegistry.quest->refAliasMap.find(alias->aliasID);
			if (actor) {
				if (it != g_defeatedEnemyRegistry.quest->refAliasMap.end()) {
					it->second = handle;
				} else {
					g_defeatedEnemyRegistry.quest->refAliasMap.insert({ alias->aliasID, handle });
				}
			} else if (it != g_defeatedEnemyRegistry.quest->refAliasMap.end()) {
				g_defeatedEnemyRegistry.quest->refAliasMap.erase(it);
			}
		}

		static int FindDefeatedEnemyAliasSlot(RE::Actor* actor)
		{
			if (!actor) {
				return -1;
			}
			ResolveDefeatedEnemyRegistry();
			for (std::size_t i = 0; i < g_defeatedEnemyRegistry.enemyAliases.size(); ++i) {
				auto* alias = g_defeatedEnemyRegistry.enemyAliases[i];
				if (!alias) {
					continue;
				}
				auto* current = alias->GetActorReference();
				if (current && current->GetFormID() == actor->GetFormID()) {
					return static_cast<int>(i);
				}
			}
			return -1;
		}

		static void SyncDefeatedEnemyMirrorInternal(RE::Actor* actor, int& aliasSlot, bool& factionApplied)
		{
			if (!actor) {
				return;
			}
			ResolveDefeatedEnemyRegistry();
			if (g_defeatedEnemyRegistry.faction) {
				actor->AddToFaction(g_defeatedEnemyRegistry.faction, 0);
				factionApplied = true;
			}

			int slot = FindDefeatedEnemyAliasSlot(actor);
			if (slot >= 0) {
				aliasSlot = slot;
				return;
			}

			for (std::size_t i = 0; i < g_defeatedEnemyRegistry.enemyAliases.size(); ++i) {
				auto* alias = g_defeatedEnemyRegistry.enemyAliases[i];
				if (!alias) {
					continue;
				}
				auto* current = alias->GetActorReference();
				if (current && current != actor) {
					continue;
				}
				WriteDefeatedEnemyAlias(alias, actor);
				aliasSlot = static_cast<int>(i);
				spdlog::info("[TFD][FactionManager] defeated enemy alias fill alias='{}' actor={:08X}",
					alias->aliasName.c_str(), actor->GetFormID());
				return;
			}
		}

		static void ClearDefeatedEnemyMirrorInternal(RE::Actor* actor, int& aliasSlot, bool& factionApplied, const char* reason)
		{
			ResolveDefeatedEnemyRegistry();
			if (aliasSlot >= 0 && aliasSlot < static_cast<int>(g_defeatedEnemyRegistry.enemyAliases.size())) {
				if (auto* alias = g_defeatedEnemyRegistry.enemyAliases[static_cast<std::size_t>(aliasSlot)]) {
					auto* current = alias->GetActorReference();
					if (!actor || !current || current->GetFormID() == actor->GetFormID()) {
						WriteDefeatedEnemyAlias(alias, nullptr);
						spdlog::info("[TFD][FactionManager] defeated enemy alias clear alias='{}' actor={:08X} reason={}",
							alias->aliasName.c_str(), actor ? actor->GetFormID() : 0u, reason ? reason : "unknown");
					}
				}
			}
			aliasSlot = -1;
			if (factionApplied && actor && g_defeatedEnemyRegistry.faction) {
				actor->RemoveFromFaction(g_defeatedEnemyRegistry.faction);
			}
			factionApplied = false;
		}

		static void ClearAllDefeatedEnemyMirrorsInternal(const char* reason)
		{
			ResolveDefeatedEnemyRegistry();
			for (auto* alias : g_defeatedEnemyRegistry.enemyAliases) {
				if (!alias) {
					continue;
				}
				auto* current = alias->GetActorReference();
				if (!current) {
					continue;
				}
				WriteDefeatedEnemyAlias(alias, nullptr);
				spdlog::info("[TFD][FactionManager] defeated enemy alias clear alias='{}' actor={:08X} reason={}",
					alias->aliasName.c_str(), current->GetFormID(), reason ? reason : "unknown");
			}
		}

		static void ResolveTruceQuestRegistry()
		{
			if (g_truceQuestRegistry.resolved) {
				return;
			}
			g_truceQuestRegistry.resolved = true;
			g_truceQuestRegistry.quest = RE::TESForm::LookupByEditorID<RE::TESQuest>("TFDTruceQuest");
			if (!g_truceQuestRegistry.quest) {
				spdlog::warn("[TFD][FactionManager] truce quest not found");
				return;
			}
			for (auto* baseAlias : g_truceQuestRegistry.quest->aliases) {
				auto* refAlias = skyrim_cast<RE::BGSRefAlias*>(baseAlias);
				if (!refAlias) {
					continue;
				}
				const auto aliasName = std::string(refAlias->aliasName.c_str());
				std::size_t prefixLen = 0;
				if (aliasName.rfind("Crowd", 0) == 0 && aliasName.size() > 5) {
					prefixLen = 5;
				} else if (aliasName.rfind("Truce", 0) == 0 && aliasName.size() > 5) {
					prefixLen = 5;
				} else {
					continue;
				}
				try {
					int slot = std::stoi(aliasName.substr(prefixLen));
					if (slot >= 1 && slot <= static_cast<int>(g_truceQuestRegistry.truceAliases.size())) {
						g_truceQuestRegistry.truceAliases[static_cast<std::size_t>(slot) - 1] = refAlias;
					}
				} catch (...) {}
			}
			std::size_t found = 0;
			for (auto* alias : g_truceQuestRegistry.truceAliases) {
				if (alias) {
					++found;
				}
			}
			spdlog::info("[TFD][FactionManager] truce quest resolved quest={:08X} truceAliases={}",
				g_truceQuestRegistry.quest ? g_truceQuestRegistry.quest->GetFormID() : 0u,
				found);
		}

		static std::vector<RE::Actor*> CollectTruceActorsInternal(RE::Actor* speaker)
		{
			ResolveTruceQuestRegistry();
			std::vector<RE::Actor*> actors{};
			auto addUnique = [&](RE::Actor* actor) {
				if (!actor || actor->IsDead() || actor->IsDisabled() || actor == Player()) {
					return;
				}
				for (auto* existing : actors) {
					if (existing == actor) {
						return;
					}
				}
				actors.push_back(actor);
			};
			addUnique(speaker);
			for (auto* alias : g_truceQuestRegistry.truceAliases) {
				if (!alias) {
					continue;
				}
				addUnique(alias->GetActorReference());
			}
			return actors;
		}

		static bool SuppressReleaseFollowTargetingToPlayer(RE::Actor* actor, RE::Actor* player, const char* reason)
		{
			if (!actor || !player || actor == player) {
				return false;
			}
			bool changed = false;
			auto* target = ResolveCurrentCombatTarget(actor);
			if (target == player) {
				actor->GetActorRuntimeData().currentCombatTarget = RE::ActorHandle{};
				changed = true;
			}
			if (actor->IsInCombat() || target == player) {
				actor->StopCombat();
				if (auto* process = RE::ProcessLists::GetSingleton()) {
					process->StopCombatAndAlarmOnActor(actor, false);
				}
				changed = true;
			}
			if (!changed) {
				return false;
			}
			if (auto* process = RE::ProcessLists::GetSingleton()) {
				process->ClearCachedFactionFightReactions();
			}
			actor->EvaluatePackage(false, true);
			actor->EvaluatePackage(true, true);
			actor->UpdateCombat();
			player->UpdateCombat();
			spdlog::info("[TFD][FactionManager] grace suppressed player targeting actor={:08X} player={:08X} inCombat={} reason={}",
				actor->GetFormID(),
				player->GetFormID(),
				actor->IsInCombat() ? 1 : 0,
				reason ? reason : "release_follow");
			return true;
		}

		static void RemoveReleaseFollowGraceFromActor(RE::Actor* actor, const char* reason)
		{
			if (!actor) {
				return;
			}
			ResolveReleaseFollowHelperFaction();
			if (g_releaseFollowHelperFaction && actor->IsInFaction(g_releaseFollowHelperFaction)) {
				actor->RemoveFromFaction(g_releaseFollowHelperFaction);
			}
			if (g_dialogueHelperFaction && actor->IsInFaction(g_dialogueHelperFaction) && !ActorHasActiveDialoguePhaseFaction(actor)) {
				actor->RemoveFromFaction(g_dialogueHelperFaction);
			}
			g_releaseFollowGraceEntries.erase(actor->GetFormID());
			spdlog::info("[TFD][FactionManager] grace removed actor={:08X} reason={}", actor->GetFormID(), reason ? reason : "unknown");
		}

		static void ApplyReleaseFollowGraceToActor(RE::Actor* actor, double durationSeconds, const char* reason)
		{
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				return;
			}
			ResolveReleaseFollowHelperFaction();
			if (!g_releaseFollowHelperFaction && !g_dialogueHelperFaction) {
				return;
			}
			if (durationSeconds <= 0.0) {
				durationSeconds = 20.0;
			}
			if (g_releaseFollowHelperFaction && !actor->IsInFaction(g_releaseFollowHelperFaction)) {
				actor->AddToFaction(g_releaseFollowHelperFaction, 0);
			}
			if (g_dialogueHelperFaction && !actor->IsInFaction(g_dialogueHelperFaction)) {
				actor->AddToFaction(g_dialogueHelperFaction, 0);
			}
			ReleaseFollowGraceEntry entry{};
			entry.actor = actor->GetHandle();
			entry.expiresAt = Now() + std::chrono::milliseconds(static_cast<int>((std::max)(0.0, durationSeconds) * 1000.0));
			entry.source = reason ? reason : "unknown";
			g_releaseFollowGraceEntries[actor->GetFormID()] = std::move(entry);
			if (auto* player = Player()) {
				(void)SuppressReleaseFollowTargetingToPlayer(actor, player, reason ? reason : "grace_apply");
			}
			spdlog::info("[TFD][FactionManager] grace applied actor={:08X} helperFaction={:08X} dialogueFaction={:08X} duration={:.2f} reason={}",
				actor->GetFormID(),
				g_releaseFollowHelperFaction ? g_releaseFollowHelperFaction->GetFormID() : 0u,
				g_dialogueHelperFaction ? g_dialogueHelperFaction->GetFormID() : 0u,
				durationSeconds,
				reason ? reason : "unknown");
		}
	}

	void Initialize()
	{
		if (g_initialized) {
			return;
		}

		g_initialized = true;
		g_allowedFactions.clear();
		ResetState(true);

		for (auto* editorID : kAllowedFactionEditorIDs) {
			AddAllowedFaction(editorID);
		}

		spdlog::info(
			"[TFD][FactionManager] initialized allowlist entries={}",
			g_allowedFactions.size());
	}

	bool ApplyFromAggressor(RE::Actor* aggressor)
	{
		Initialize();
		ResetState(false);

		if (!aggressor) {
			SetJoinEnemyState(0);
			spdlog::info("[TFD][FactionManager] apply skipped: aggressor missing");
			return false;
		}

		int matchedCount = 0;

		for (const auto& entry : g_allowedFactions) {
			auto* faction = entry.faction;
			if (!faction) {
				continue;
			}

			if (!HasExactFaction(aggressor, faction)) {
				continue;
			}

			++matchedCount;
			spdlog::info(
				"[TFD][FactionManager] classifier match actor={:08X} fac={:08X} editorID={}",
				aggressor->GetFormID(),
				faction->GetFormID(),
				entry.editorID ? entry.editorID : "unknown");
		}

		if (matchedCount <= 0) {
			SetJoinEnemyState(0);
			spdlog::info(
				"[TFD][FactionManager] aggressor {:08X} had no matching allowlist faction",
				aggressor->GetFormID());
			return false;
		}

		g_active = true;
		g_matchCount = matchedCount;
		g_sourceActorFormID = aggressor->GetFormID();
		SetJoinEnemyState(matchedCount == 1 ? 1 : 2);

		spdlog::info(
			"[TFD][FactionManager] classified aggressor {:08X} matchedCount={} joinEnemyState={} (no faction copied to player)",
			g_sourceActorFormID,
			g_matchCount,
			matchedCount == 1 ? 1 : 2);

		return true;
	}

	void Clear()
	{
		Initialize();

		const auto sourceActorFormID = g_sourceActorFormID;
		const auto matchCount = g_matchCount;
		ResetState(true);

		spdlog::info(
			"[TFD][FactionManager] cleared classifier state source={:08X} matchedCount={} (player faction membership unchanged)",
			sourceActorFormID,
			matchCount);
	}

	bool IsActive()
	{
		return g_active;
	}

	bool SharesAllowedFactionExact(RE::Actor* lhs, RE::Actor* rhs)
	{
		Initialize();

		if (!lhs || !rhs) {
			return false;
		}

		for (const auto& entry : g_allowedFactions) {
			auto* faction = entry.faction;
			if (!faction) {
				continue;
			}

			if (!HasExactFaction(lhs, faction)) {
				continue;
			}

			if (HasExactFaction(rhs, faction)) {
				return true;
			}
		}

		return false;
	}
	std::vector<RE::Actor*> CollectTruceActors()
	{
		Initialize();
		return CollectTruceActorsInternal(nullptr);
	}

	bool HasAnyReleaseFollowGrace()
	{
		return !g_releaseFollowGraceEntries.empty();
	}

	bool HasReleaseFollowGrace(RE::Actor* actor)
	{
		if (!actor) {
			return false;
		}
		return g_releaseFollowGraceEntries.find(actor->GetFormID()) != g_releaseFollowGraceEntries.end();
	}

	void ApplyReleaseFollowGraceToSpeakerAndCrowd(RE::Actor* speaker, double durationSeconds, const char* reason)
	{
		Initialize();
		for (auto* actor : CollectTruceActorsInternal(speaker)) {
			ApplyReleaseFollowGraceToActor(actor, durationSeconds, reason);
		}
	}

	void RemoveReleaseFollowGraceFromSpeakerAndCrowd(RE::Actor* speaker, const char* reason)
	{
		Initialize();
		for (auto* actor : CollectTruceActorsInternal(speaker)) {
			RemoveReleaseFollowGraceFromActor(actor, reason);
		}
	}

	void CancelReleaseFollowGraceFromPlayerAggression(RE::Actor* actor, const char* reason)
	{
		if (!actor || !HasReleaseFollowGrace(actor)) {
			return;
		}
		RemoveReleaseFollowGraceFromSpeakerAndCrowd(actor, reason ? reason : "player_attack_cancel");
	}

	void MaintainReleaseFollowGrace()
	{
		if (g_releaseFollowGraceEntries.empty()) {
			return;
		}
		auto* player = Player();
		std::vector<RE::Actor*> actorsToClear{};
		std::vector<RE::FormID> staleIds{};
		const auto now = Now();
		for (auto& [formID, entry] : g_releaseFollowGraceEntries) {
			auto actorPtr = RE::Actor::LookupByHandle(entry.actor.native_handle());
			auto* actor = actorPtr.get();
			const bool expired = now >= entry.expiresAt;
			const bool invalid = !actor || actor->IsDead() || actor->IsDisabled();
			if (!expired && !invalid) {
				if (player) {
					(void)SuppressReleaseFollowTargetingToPlayer(actor, player, entry.source.c_str());
				}
				continue;
			}
			if (actor) {
				actorsToClear.push_back(actor);
			} else {
				staleIds.push_back(formID);
				spdlog::info("[TFD][FactionManager] grace removed stale handle actor={:08X} reason={}", formID, expired ? "timer_expired_missing_actor" : "actor_missing");
			}
		}
		for (auto* actor : actorsToClear) {
			RemoveReleaseFollowGraceFromActor(actor, "timer_or_invalid");
		}
		for (auto formID : staleIds) {
			g_releaseFollowGraceEntries.erase(formID);
		}
	}

	void ClearAllReleaseFollowGrace(const char* reason)
	{
		if (g_releaseFollowGraceEntries.empty()) {
			return;
		}
		std::vector<RE::Actor*> actorsToClear{};
		std::vector<RE::FormID> ids{};
		actorsToClear.reserve(g_releaseFollowGraceEntries.size());
		ids.reserve(g_releaseFollowGraceEntries.size());
		for (auto& [formID, entry] : g_releaseFollowGraceEntries) {
			auto actorPtr = RE::Actor::LookupByHandle(entry.actor.native_handle());
			if (auto* actor = actorPtr.get()) {
				actorsToClear.push_back(actor);
			} else {
				ids.push_back(formID);
			}
		}
		for (auto* actor : actorsToClear) {
			RemoveReleaseFollowGraceFromActor(actor, reason ? reason : "clear_all");
		}
		for (auto formID : ids) {
			g_releaseFollowGraceEntries.erase(formID);
		}
	}

	void SyncDefeatedEnemyMirror(RE::Actor* actor, int& aliasSlot, bool& factionApplied)
	{
		Initialize();
		SyncDefeatedEnemyMirrorInternal(actor, aliasSlot, factionApplied);
	}

	void ClearDefeatedEnemyMirror(RE::Actor* actor, int& aliasSlot, bool& factionApplied, const char* reason)
	{
		Initialize();
		ClearDefeatedEnemyMirrorInternal(actor, aliasSlot, factionApplied, reason);
	}

	void ClearAllDefeatedEnemyMirrors(const char* reason)
	{
		Initialize();
		ClearAllDefeatedEnemyMirrorsInternal(reason);
	}

}
