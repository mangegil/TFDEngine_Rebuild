#include "TFDFactionMask.h"

#include <mutex>
#include <vector>
#include <cstdint>

#include <spdlog/spdlog.h>

namespace TFD::FactionMask
{
	namespace
	{
		struct SavedFaction
		{
			RE::FormID factionId{ 0 };
			std::int32_t oldRank{ -2 };   // -2 = player not in faction
			std::int8_t appliedRank{ 0 }; // what we set on player
		};

		std::mutex gMu;
		bool gActive = false;

		RE::BGSListForm* gAllowList = nullptr;
		std::vector<SavedFaction> gSaved;
		RE::ActorHandle gLastAggressor;

		static RE::PlayerCharacter* Player()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

		static std::int8_t ClampRank(std::int32_t r)
		{
			if (r < 0) return 0;
			if (r > 127) return 127;
			return static_cast<std::int8_t>(r);
		}

		static void ResolveAllowListLocked()
		{
			if (gAllowList) {
				return;
			}

			gAllowList = RE::TESForm::LookupByEditorID<RE::BGSListForm>(kAllowListEditorId);
			if (!gAllowList) {
				spdlog::warn("[TFD][FactionMask] allowlist missing (EditorID='{}')", kAllowListEditorId);
				return;
			}

			spdlog::info("[TFD][FactionMask] allowlist resolved -> {:08X} ({} entries)",
				gAllowList->GetFormID(), gAllowList->forms.size());
		}

		static bool IsAlreadySaved(RE::FormID facId)
		{
			for (auto& s : gSaved) {
				if (s.factionId == facId) {
					return true;
				}
			}
			return false;
		}

		static void SaveAndApplyPlayerFaction(RE::PlayerCharacter* player, RE::TESFaction* fac, std::int8_t appliedRank)
		{
			if (!player || !fac) {
				return;
			}

			const auto facId = fac->GetFormID();
			if (IsAlreadySaved(facId)) {
				return;
			}

			SavedFaction s;
			s.factionId = facId;
			s.oldRank = player->GetFactionRank(fac, true);
			s.appliedRank = appliedRank;

			player->AddToFaction(fac, appliedRank);
			gSaved.push_back(s);
		}

		static bool CopyFromAggressorIfPresent(RE::PlayerCharacter* player, RE::Actor* aggressor, RE::TESFaction* fac)
		{
			if (!player || !aggressor || !fac) {
				return false;
			}

			const auto agRank = aggressor->GetFactionRank(fac, false);
			if (agRank == -2) {
				return false; // aggressor not in this faction
			}

			SaveAndApplyPlayerFaction(player, fac, ClampRank(agRank));
			return true;
		}

		static void ClearLocked(RE::PlayerCharacter* player)
		{
			if (!player) {
				return;
			}

			if (!gActive) {
				gSaved.clear();
				gLastAggressor = {};
				return;
			}

			for (auto it = gSaved.rbegin(); it != gSaved.rend(); ++it) {
				auto* fac = RE::TESForm::LookupByID<RE::TESFaction>(it->factionId);
				if (!fac) {
					continue;
				}

				if (it->oldRank == -2) {
					player->RemoveFromFaction(fac);
				}
				else {
					player->AddToFaction(fac, ClampRank(it->oldRank));
				}
			}

			gSaved.clear();
			gActive = false;
			gLastAggressor = {};

			spdlog::info("[TFD][FactionMask] cleared/restored");
		}
	}

	void Initialize()
	{
		std::scoped_lock lk(gMu);
		ResolveAllowListLocked();
	}

	bool IsActive()
	{
		std::scoped_lock lk(gMu);
		return gActive;
	}

	bool ApplyFromAggressor(RE::Actor* aggressor)
	{
		auto* player = Player();
		if (!player || !aggressor) {
			return false;
		}

		std::scoped_lock lk(gMu);

		ResolveAllowListLocked();
		if (!gAllowList || gAllowList->forms.empty()) {
			spdlog::warn("[TFD][FactionMask] allowlist missing/empty -> skip");
			return false;
		}

		// restore previous mask first
		ClearLocked(player);

		gLastAggressor = aggressor->GetHandle();

		bool anyApplied = false;

		for (auto* f : gAllowList->forms) {
			auto* fac = f ? f->As<RE::TESFaction>() : nullptr;
			if (!fac) {
				continue;
			}

			if (CopyFromAggressorIfPresent(player, aggressor, fac)) {
				anyApplied = true;
			}
		}

		if (!anyApplied) {
			spdlog::warn("[TFD][FactionMask] no allowlist faction matched aggressor {:08X}", aggressor->GetFormID());
			return false;
		}

		gActive = true;

		spdlog::info("[TFD][FactionMask] applied mask from aggressor {:08X} (savedCount={})",
			aggressor->GetFormID(), gSaved.size());

		return true;
	}

	void Clear()
	{
		auto* player = Player();
		if (!player) {
			return;
		}

		std::scoped_lock lk(gMu);
		ClearLocked(player);
	}

	void DumpToLog()
	{
		std::scoped_lock lk(gMu);

		spdlog::info("[TFD][FactionMask] active={} savedCount={}",
			gActive ? "true" : "false", gSaved.size());

		for (auto& s : gSaved) {
			spdlog::info("[TFD][FactionMask] fac={:08X} oldRank={} appliedRank={}",
				s.factionId, s.oldRank, s.appliedRank);
		}
	}
}