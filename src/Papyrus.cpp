#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace FMN
{
	struct ScanEntry
	{
		RE::ActorHandle actor;
		RE::FormID matchedFactionId{ 0 };
		std::int32_t matchedRank{ -2 };
	};

	struct FactionEntry
	{
		RE::FormID factionId{ 0 };
		std::int32_t rank{ -2 };
	};

	static std::mutex dataLock;

	static std::atomic_bool debugEnabled{ false };

	static std::vector<ScanEntry> scanList;
	static std::vector<FactionEntry> playerFactionList;
	static std::vector<FactionEntry> actorFactionList;
	static RE::ActorHandle actorFactionTarget;

	static constexpr std::size_t maxScanKeep = 1024;
	static constexpr std::size_t maxFactionKeep = 2048;

	static void ApplyLogLevel()
	{
		spdlog::set_level(debugEnabled.load() ? spdlog::level::debug : spdlog::level::info);
		spdlog::flush_on(debugEnabled.load() ? spdlog::level::debug : spdlog::level::info);
	}

	bool GetDebugEnabled(RE::StaticFunctionTag*)
	{
		return debugEnabled.load();
	}

	void SetDebugEnabled(RE::StaticFunctionTag*, bool enabled)
	{
		debugEnabled.store(enabled);
		ApplyLogLevel();
		spdlog::info("DebugEnabled = {}", enabled);
	}

	static RE::BGSKeyword* GetActorTypeNpcKeyword()
	{
		static RE::BGSKeyword* cached = nullptr;
		static bool tried = false;

		if (tried) {
			return cached;
		}

		tried = true;
		cached = RE::TESForm::LookupByID<RE::BGSKeyword>(0x00013794);  // ActorTypeNPC
		return cached;
	}

	static bool IsNpcLike(RE::Actor* actor)
	{
		if (!actor) {
			return false;
		}

		auto* kwNpc = GetActorTypeNpcKeyword();
		if (!kwNpc) {
			return true;
		}

		if (auto* race = actor->GetRace(); race && race->HasKeyword(kwNpc)) {
			return true;
		}

		if (auto* base = actor->GetActorBase(); base && base->HasKeyword(kwNpc)) {
			return true;
		}

		if (actor->HasKeyword(kwNpc)) {
			return true;
		}

		return false;
	}

	static std::string GetActorNameSafe(RE::Actor* actor)
	{
		if (!actor) {
			return "";
		}

		if (auto* dn = actor->GetDisplayFullName(); dn && dn[0] != '\0') {
			return std::string(dn);
		}

		if (auto* n = actor->GetName(); n && n[0] != '\0') {
			return std::string(n);
		}

		return "";
	}

	static float DistSq(RE::TESObjectREFR* a, RE::TESObjectREFR* b)
	{
		if (!a || !b) {
			return 0.0f;
		}

		const auto pa = a->GetPosition();
		const auto pb = b->GetPosition();

		const float dx = pa.x - pb.x;
		const float dy = pa.y - pb.y;
		const float dz = pa.z - pb.z;

		return dx * dx + dy * dy + dz * dz;
	}

	static std::vector<RE::ActorHandle> CollectActorHandles()
	{
		std::vector<RE::ActorHandle> out;
		out.reserve(256);

		auto* lists = RE::ProcessLists::GetSingleton();
		if (!lists) {
			return out;
		}

		auto addArr = [&](auto& arr) {
			for (auto& h : arr) {
				if (h) {
					out.push_back(h);
				}
			}
			};

		addArr(lists->highActorHandles);
		addArr(lists->middleHighActorHandles);
		addArr(lists->middleLowActorHandles);
		addArr(lists->lowActorHandles);

		return out;
	}

	static bool FindMatch(RE::Actor* actor, RE::BGSListForm* list, RE::FormID& outFactionId, std::int32_t& outRank)
	{
		outFactionId = 0;
		outRank = -2;

		if (!actor || !list) {
			return false;
		}

		for (auto* form : list->forms) {
			auto* fac = form ? form->As<RE::TESFaction>() : nullptr;
			if (!fac) {
				continue;
			}

			const auto r = actor->GetFactionRank(fac, false);
			if (r != -2) {
				outFactionId = fac->GetFormID();
				outRank = r;
				return true;
			}
		}

		return false;
	}

	// =========================================================
	// FIXED Rescan:
	// Interior  : Is3DLoaded + same cell as player
	// Exterior  : Is3DLoaded + same worldspace as player
	// =========================================================
	std::int32_t Rescan(RE::StaticFunctionTag*, float radius, bool npcOnly, RE::BGSListForm* factionsToCheck, bool requireMatch)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return 0;
		}

		if (!factionsToCheck) {
			requireMatch = false;
		}

		if (radius < 0.0f) {
			radius = 0.0f;
		}

		const float radiusSq = (radius > 0.0f) ? (radius * radius) : 0.0f;

		auto* playerCell = player->GetParentCell();
		auto* playerWs = player->GetWorldspace();

		// Reliable interior detection:
		// In Skyrim, player in interior usually has null worldspace.
		const bool inInterior = (playerWs == nullptr) && (playerCell != nullptr);

		auto handles = CollectActorHandles();

		std::unordered_set<RE::FormID> seen;
		seen.reserve(handles.size() * 2);

		std::vector<ScanEntry> temp;
		temp.reserve(std::min<std::size_t>(handles.size(), maxScanKeep));

		for (auto& h : handles) {
			auto sp = h.get();
			auto* a = sp.get();
			if (!a) {
				continue;
			}

			if (a == player) {
				continue;
			}

			const auto id = a->GetFormID();
			if (id == 0 || !seen.insert(id).second) {
				continue;
			}

			if (a->IsDisabled()) {
				continue;
			}

			// Hard filter: only actors that are actually loaded in scene.
			// This removes pooled/quest actors that cause hundreds/thousands results.
			if (!a->Is3DLoaded()) {
				continue;
			}

			if (inInterior) {
				// Interior must be same cell
				if (a->GetParentCell() != playerCell) {
					continue;
				}
			}
			else {
				// Exterior must be same worldspace
				if (playerWs && a->GetWorldspace() != playerWs) {
					continue;
				}
			}

			if (npcOnly && !IsNpcLike(a)) {
				continue;
			}

			if (radiusSq > 0.0f) {
				const float d2 = DistSq(player, a);
				if (d2 > radiusSq) {
					continue;
				}
			}

			ScanEntry e;
			e.actor = h;

			if (factionsToCheck) {
				RE::FormID mf;
				std::int32_t mr;
				const bool matched = FindMatch(a, factionsToCheck, mf, mr);
				if (!matched && requireMatch) {
					continue;
				}
				e.matchedFactionId = mf;
				e.matchedRank = mr;
			}

			temp.push_back(e);
			if (temp.size() >= maxScanKeep) {
				break;
			}
		}

		{
			std::scoped_lock lock(dataLock);
			scanList = std::move(temp);
		}

		if (debugEnabled.load()) {
			spdlog::debug("Rescan: {}", scanList.size());
		}

		return static_cast<std::int32_t>(scanList.size());
	}

	std::int32_t GetCount(RE::StaticFunctionTag*)
	{
		std::scoped_lock lock(dataLock);
		return static_cast<std::int32_t>(scanList.size());
	}

	RE::Actor* GetActor(RE::StaticFunctionTag*, std::int32_t index)
	{
		std::scoped_lock lock(dataLock);

		if (index < 0 || static_cast<std::size_t>(index) >= scanList.size()) {
			return nullptr;
		}

		auto sp = scanList[static_cast<std::size_t>(index)].actor.get();
		return sp.get();
	}

	std::string GetActorName(RE::StaticFunctionTag*, std::int32_t index)
	{
		std::scoped_lock lock(dataLock);

		if (index < 0 || static_cast<std::size_t>(index) >= scanList.size()) {
			return "";
		}

		auto sp = scanList[static_cast<std::size_t>(index)].actor.get();
		return GetActorNameSafe(sp.get());
	}

	RE::TESFaction* GetMatchedFaction(RE::StaticFunctionTag*, std::int32_t index)
	{
		std::scoped_lock lock(dataLock);

		if (index < 0 || static_cast<std::size_t>(index) >= scanList.size()) {
			return nullptr;
		}

		auto id = scanList[static_cast<std::size_t>(index)].matchedFactionId;
		if (id == 0) {
			return nullptr;
		}

		return RE::TESForm::LookupByID<RE::TESFaction>(id);
	}

	std::int32_t GetMatchedRank(RE::StaticFunctionTag*, std::int32_t index)
	{
		std::scoped_lock lock(dataLock);

		if (index < 0 || static_cast<std::size_t>(index) >= scanList.size()) {
			return -2;
		}

		return scanList[static_cast<std::size_t>(index)].matchedRank;
	}

	std::int32_t RescanPlayerFactions(RE::StaticFunctionTag*)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!player || !dh) {
			std::scoped_lock lock(dataLock);
			playerFactionList.clear();
			return 0;
		}

		std::vector<FactionEntry> temp;
		temp.reserve(256);

		auto& factions = dh->GetFormArray<RE::TESFaction>();
		for (auto* f : factions) {
			if (!f) {
				continue;
			}

			const auto r = player->GetFactionRank(f, false);
			if (r == -2) {
				continue;
			}

			FactionEntry e;
			e.factionId = f->GetFormID();
			e.rank = r;
			temp.push_back(e);

			if (temp.size() >= maxFactionKeep) {
				break;
			}
		}

		{
			std::scoped_lock lock(dataLock);
			playerFactionList = std::move(temp);
		}

		return static_cast<std::int32_t>(playerFactionList.size());
	}

	std::int32_t GetPlayerFactionCount(RE::StaticFunctionTag*)
	{
		std::scoped_lock lock(dataLock);
		return static_cast<std::int32_t>(playerFactionList.size());
	}

	RE::TESFaction* GetPlayerFaction(RE::StaticFunctionTag*, std::int32_t index)
	{
		std::scoped_lock lock(dataLock);

		if (index < 0 || static_cast<std::size_t>(index) >= playerFactionList.size()) {
			return nullptr;
		}

		return RE::TESForm::LookupByID<RE::TESFaction>(playerFactionList[static_cast<std::size_t>(index)].factionId);
	}

	std::int32_t GetPlayerFactionRank(RE::StaticFunctionTag*, std::int32_t index)
	{
		std::scoped_lock lock(dataLock);

		if (index < 0 || static_cast<std::size_t>(index) >= playerFactionList.size()) {
			return -2;
		}

		return playerFactionList[static_cast<std::size_t>(index)].rank;
	}

	std::int32_t RescanActorFactions(RE::StaticFunctionTag*, RE::Actor* target)
	{
		auto* dh = RE::TESDataHandler::GetSingleton();

		std::vector<FactionEntry> temp;
		temp.reserve(256);

		RE::ActorHandle newTarget;

		if (target && dh) {
			newTarget = target->GetHandle();

			auto& factions = dh->GetFormArray<RE::TESFaction>();
			for (auto* f : factions) {
				if (!f) {
					continue;
				}

				const auto r = target->GetFactionRank(f, false);
				if (r == -2) {
					continue;
				}

				FactionEntry e;
				e.factionId = f->GetFormID();
				e.rank = r;
				temp.push_back(e);

				if (temp.size() >= maxFactionKeep) {
					break;
				}
			}
		}

		{
			std::scoped_lock lock(dataLock);
			actorFactionList = std::move(temp);
			actorFactionTarget = newTarget;
		}

		return static_cast<std::int32_t>(actorFactionList.size());
	}

	std::int32_t GetActorFactionCount(RE::StaticFunctionTag*)
	{
		std::scoped_lock lock(dataLock);
		return static_cast<std::int32_t>(actorFactionList.size());
	}

	RE::TESFaction* GetActorFaction(RE::StaticFunctionTag*, std::int32_t index)
	{
		std::scoped_lock lock(dataLock);

		if (index < 0 || static_cast<std::size_t>(index) >= actorFactionList.size()) {
			return nullptr;
		}

		return RE::TESForm::LookupByID<RE::TESFaction>(actorFactionList[static_cast<std::size_t>(index)].factionId);
	}

	std::int32_t GetActorFactionRank(RE::StaticFunctionTag*, std::int32_t index)
	{
		std::scoped_lock lock(dataLock);

		if (index < 0 || static_cast<std::size_t>(index) >= actorFactionList.size()) {
			return -2;
		}

		return actorFactionList[static_cast<std::size_t>(index)].rank;
	}

	RE::Actor* GetActorFactionTarget(RE::StaticFunctionTag*)
	{
		std::scoped_lock lock(dataLock);
		auto sp = actorFactionTarget.get();
		return sp.get();
	}

	std::string GetFactionRankTitle(RE::StaticFunctionTag*, RE::TESFaction* faction, std::int32_t rank, bool female)
	{
		if (!faction || rank < 0) {
			return "";
		}

		std::int32_t i = 0;
		for (auto it = faction->rankData.begin(); it != faction->rankData.end(); ++it) {
			auto* data = *it;
			if (i == rank) {
				if (!data) {
					return "";
				}

				const auto& primary = female ? data->femaleRankTitle : data->maleRankTitle;
				if (primary.c_str() && primary.c_str()[0] != '\0') {
					return std::string(primary.c_str());
				}

				const auto& other = female ? data->maleRankTitle : data->femaleRankTitle;
				if (other.c_str() && other.c_str()[0] != '\0') {
					return std::string(other.c_str());
				}

				return "";
			}
			++i;
		}

		return "";
	}

	bool RegisterPapyrus(RE::BSScript::IVirtualMachine* vm)
	{
		ApplyLogLevel();

		vm->RegisterFunction("GetDebugEnabled", "FactionMonitorNative", GetDebugEnabled);
		vm->RegisterFunction("SetDebugEnabled", "FactionMonitorNative", SetDebugEnabled);

		vm->RegisterFunction("Rescan", "FactionMonitorNative", Rescan);
		vm->RegisterFunction("GetCount", "FactionMonitorNative", GetCount);
		vm->RegisterFunction("GetActor", "FactionMonitorNative", GetActor);
		vm->RegisterFunction("GetActorName", "FactionMonitorNative", GetActorName);
		vm->RegisterFunction("GetMatchedFaction", "FactionMonitorNative", GetMatchedFaction);
		vm->RegisterFunction("GetMatchedRank", "FactionMonitorNative", GetMatchedRank);

		vm->RegisterFunction("RescanPlayerFactions", "FactionMonitorNative", RescanPlayerFactions);
		vm->RegisterFunction("GetPlayerFactionCount", "FactionMonitorNative", GetPlayerFactionCount);
		vm->RegisterFunction("GetPlayerFaction", "FactionMonitorNative", GetPlayerFaction);
		vm->RegisterFunction("GetPlayerFactionRank", "FactionMonitorNative", GetPlayerFactionRank);

		vm->RegisterFunction("RescanActorFactions", "FactionMonitorNative", RescanActorFactions);
		vm->RegisterFunction("GetActorFactionCount", "FactionMonitorNative", GetActorFactionCount);
		vm->RegisterFunction("GetActorFaction", "FactionMonitorNative", GetActorFaction);
		vm->RegisterFunction("GetActorFactionRank", "FactionMonitorNative", GetActorFactionRank);
		vm->RegisterFunction("GetActorFactionTarget", "FactionMonitorNative", GetActorFactionTarget);

		vm->RegisterFunction("GetFactionRankTitle", "FactionMonitorNative", GetFactionRankTitle);

		spdlog::info("Papyrus registered");
		return true;
	}
}