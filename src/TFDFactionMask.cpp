#include "TFDFactionMask.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

namespace TFD::FactionMask
{
	namespace
	{
		struct AllowedFactionEntry
		{
			const char* editorID = nullptr;
			RE::TESFaction* faction = nullptr;
		};

		bool g_initialized = false;
		bool g_active = false;
		int g_matchCount = 0;
		std::uint32_t g_sourceActorFormID = 0;

		std::vector<AllowedFactionEntry> g_allowedFactions{};

		static RE::TESGlobal* g_joinEnemyStateGlobal = nullptr;

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
				spdlog::warn("[TFD][FactionMask] unresolved allowlist faction '{}'", editorID);
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
			"[TFD][FactionMask] initialized allowlist entries={}",
			g_allowedFactions.size());
	}

	bool ApplyFromAggressor(RE::Actor* aggressor)
	{
		Initialize();
		ResetState(false);

		if (!aggressor) {
			SetJoinEnemyState(0);
			spdlog::info("[TFD][FactionMask] apply skipped: aggressor missing");
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
				"[TFD][FactionMask] classifier match actor={:08X} fac={:08X} editorID={}",
				aggressor->GetFormID(),
				faction->GetFormID(),
				entry.editorID ? entry.editorID : "unknown");
		}

		if (matchedCount <= 0) {
			SetJoinEnemyState(0);
			spdlog::info(
				"[TFD][FactionMask] aggressor {:08X} had no matching allowlist faction",
				aggressor->GetFormID());
			return false;
		}

		g_active = true;
		g_matchCount = matchedCount;
		g_sourceActorFormID = aggressor->GetFormID();
		SetJoinEnemyState(matchedCount == 1 ? 1 : 2);

		spdlog::info(
			"[TFD][FactionMask] classified aggressor {:08X} matchedCount={} joinEnemyState={} (no faction copied to player)",
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
			"[TFD][FactionMask] cleared classifier state source={:08X} matchedCount={} (player faction membership unchanged)",
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
}