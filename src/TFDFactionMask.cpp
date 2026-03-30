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
		struct SavedFactionEntry
		{
			RE::TESFaction* faction = nullptr;
			std::int8_t oldRank = -2;
			bool hadMembership = false;
		};

		struct AllowedFactionEntry
		{
			const char* editorID = nullptr;
			RE::TESFaction* faction = nullptr;
		};

		bool g_initialized = false;
		bool g_active = false;

		std::vector<AllowedFactionEntry> g_allowedFactions{};
		std::vector<SavedFactionEntry> g_savedPlayerFactions{};

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

		static RE::PlayerCharacter* Player()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

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

		static void SaveAndApplyFaction(RE::PlayerCharacter* player, RE::TESFaction* faction)
		{
			if (!player || !faction) {
				return;
			}

			const std::int8_t oldRank = GetExactFactionRank(player, faction);
			const bool hadMembership = oldRank > -2;

			const auto exists = std::find_if(
				g_savedPlayerFactions.begin(),
				g_savedPlayerFactions.end(),
				[faction](const SavedFactionEntry& e) { return e.faction == faction; });

			if (exists == g_savedPlayerFactions.end()) {
				g_savedPlayerFactions.push_back({ faction, oldRank, hadMembership });
			}

			player->RemoveFromFaction(faction);
			player->AddToFaction(faction, 0);

			const std::int8_t afterRank = GetExactFactionRank(player, faction);

			spdlog::info(
				"[TFD][FactionMask] save/apply fac={:08X} oldRank={} had={} afterRank={}",
				faction->GetFormID(),
				static_cast<int>(oldRank),
				hadMembership ? 1 : 0,
				static_cast<int>(afterRank));
		}
	}

	void Initialize()
	{
		if (g_initialized) {
			return;
		}

		g_initialized = true;
		g_active = false;
		g_allowedFactions.clear();
		g_savedPlayerFactions.clear();

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

		auto* player = Player();
		if (!player || !aggressor) {
			g_active = false;
			return false;
		}

		if (!g_savedPlayerFactions.empty()) {
			Clear();
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

			SaveAndApplyFaction(player, faction);
			++matchedCount;
		}

		if (matchedCount <= 0) {
			g_active = false;
			spdlog::info(
				"[TFD][FactionMask] aggressor {:08X} had no matching allowlist faction",
				aggressor->GetFormID());
			return false;
		}

		g_active = true;

		spdlog::info(
			"[TFD][FactionMask] applied mask from aggressor {:08X} matchedCount={} savedCount={}",
			aggressor->GetFormID(),
			matchedCount,
			g_savedPlayerFactions.size());

		return true;
	}

	void Clear()
	{
		auto* player = Player();
		if (!player) {
			g_savedPlayerFactions.clear();
			g_active = false;
			return;
		}

		for (auto it = g_savedPlayerFactions.rbegin(); it != g_savedPlayerFactions.rend(); ++it) {
			auto* faction = it->faction;
			if (!faction) {
				continue;
			}

			const std::int8_t beforeRank = GetExactFactionRank(player, faction);

			player->RemoveFromFaction(faction);

			if (it->hadMembership) {
				player->AddToFaction(faction, it->oldRank);
			}

			const std::int8_t afterRank = GetExactFactionRank(player, faction);

			spdlog::info(
				"[TFD][FactionMask] restore fac={:08X} beforeRank={} oldRank={} had={} afterRank={}",
				faction->GetFormID(),
				static_cast<int>(beforeRank),
				static_cast<int>(it->oldRank),
				it->hadMembership ? 1 : 0,
				static_cast<int>(afterRank));
		}

		const auto clearedCount = g_savedPlayerFactions.size();
		g_savedPlayerFactions.clear();
		g_active = false;

		spdlog::info("[TFD][FactionMask] cleared/restored {} entries", clearedCount);
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