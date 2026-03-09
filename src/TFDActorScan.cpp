#include "TFDActorScan.h"
#include "TFDSettings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <spdlog/spdlog.h>

namespace TFD::ActorScan
{
	namespace
	{
		std::mutex g_lock;
		std::vector<Entry> g_list;
		RE::ActorHandle g_bestPreCombat;

		static constexpr std::size_t kMaxKeep = 1024;

		RE::BGSKeyword* GetActorTypeNpcKeyword()
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

		bool IsNpcLike(RE::Actor* actor)
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

		float DistSq(RE::TESObjectREFR* a, RE::TESObjectREFR* b)
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

		std::vector<RE::ActorHandle> CollectActorHandles()
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

		std::string GetActorNameSafe(RE::Actor* actor)
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

		float ForwardDotToActor(RE::PlayerCharacter* player, RE::Actor* actor)
		{
			if (!player || !actor) {
				return -1.0f;
			}

			const auto pp = player->GetPosition();
			const auto pa = actor->GetPosition();

			const float dx = pa.x - pp.x;
			const float dy = pa.y - pp.y;

			const float len = std::sqrt(dx * dx + dy * dy);
			if (len <= 0.001f) {
				return 1.0f;
			}

			const float yaw = player->GetAngleZ();
			const float fx = std::sin(yaw);
			const float fy = std::cos(yaw);

			const float nx = dx / len;
			const float ny = dy / len;

			return (nx * fx) + (ny * fy);
		}

		float ComputePreCombatScore(RE::PlayerCharacter* player, RE::Actor* actor, const Entry& e)
		{
			if (!player || !actor) {
				return -1.0e30f;
			}

			const float dot = ForwardDotToActor(player, actor);
			const bool front = dot >= 0.15f;
			const bool weaponDrawn = actor->IsWeaponDrawn();
			const bool closeWarn = e.dist <= 1600.0f;
			const bool midWarn = e.dist <= 2500.0f;

			float score = 0.0f;

			if (front) {
				score += 2200.0f;
			}
			else {
				score -= 900.0f;
			}

			if (weaponDrawn) {
				score += 2600.0f;
			}

			if (closeWarn) {
				score += 1400.0f;
			}
			else if (midWarn) {
				score += 500.0f;
			}
			else {
				score -= 1400.0f;
			}

			if (e.hostile) {
				score += 1600.0f;
			}
			if (e.inCombat) {
				score += 900.0f;
			}

			if (!weaponDrawn && !e.hostile && !e.inCombat && !front) {
				score -= 2500.0f;
			}

			score += (dot * 300.0f);
			score -= e.dist;

			return score;
		}
	}

	std::int32_t Rescan(float radius, bool npcOnly)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			std::scoped_lock lk(g_lock);
			g_list.clear();
			g_bestPreCombat = RE::ActorHandle{};
			return 0;
		}

		if (radius < 0.0f) {
			radius = 0.0f;
		}

		const float radiusSq = radius > 0.0f ? (radius * radius) : 0.0f;

		auto* playerCell = player->GetParentCell();
		auto* playerWs = player->GetWorldspace();
		const bool inInterior = (playerWs == nullptr) && (playerCell != nullptr);

		auto handles = CollectActorHandles();

		std::unordered_set<RE::FormID> seen;
		seen.reserve(handles.size() * 2);

		std::vector<Entry> temp;
		temp.reserve(std::min<std::size_t>(handles.size(), kMaxKeep));

		RE::ActorHandle bestCandidate;
		float bestScore = -1.0e30f;

		for (auto& h : handles) {
			auto sp = h.get();
			auto* a = sp.get();
			if (!a || a == player) {
				continue;
			}

			const auto id = a->GetFormID();
			if (id == 0 || !seen.insert(id).second) {
				continue;
			}

			if (a->IsDisabled()) {
				continue;
			}

			if (!a->Is3DLoaded()) {
				continue;
			}

			if (inInterior) {
				if (a->GetParentCell() != playerCell) {
					continue;
				}
			}
			else {
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

			Entry e;
			e.actor = a->GetHandle();
			e.dist = std::sqrt(std::max(0.0f, DistSq(player, a)));
			e.inCombat = a->IsInCombat();
			e.hostile = a->IsHostileToActor(player);

			const float score = ComputePreCombatScore(player, a, e);
			if (score > bestScore) {
				bestScore = score;
				bestCandidate = e.actor;
			}

			temp.push_back(e);
			if (temp.size() >= kMaxKeep) {
				break;
			}
		}

		std::sort(temp.begin(), temp.end(), [](const Entry& A, const Entry& B) {
			return A.dist < B.dist;
			});

		{
			std::scoped_lock lk(g_lock);
			g_list = std::move(temp);
			g_bestPreCombat = bestCandidate;
		}

		return static_cast<std::int32_t>(GetCount());
	}

	RE::Actor* GetBestPreCombatCandidate()
	{
		Rescan(TFD::Settings::GetScanRadius(), true);

		std::scoped_lock lk(g_lock);

		auto bestSp = g_bestPreCombat.get();
		if (auto* best = bestSp.get()) {
			return best;
		}

		if (!g_list.empty()) {
			auto firstSp = g_list.front().actor.get();
			return firstSp.get();
		}

		return nullptr;
	}

	std::int32_t GetCount()
	{
		std::scoped_lock lk(g_lock);
		return static_cast<std::int32_t>(g_list.size());
	}

	RE::Actor* GetActor(std::int32_t index)
	{
		std::scoped_lock lk(g_lock);
		if (index < 0 || static_cast<std::size_t>(index) >= g_list.size()) {
			return nullptr;
		}
		auto sp = g_list[static_cast<std::size_t>(index)].actor.get();
		return sp.get();
	}

	Entry GetEntry(std::int32_t index)
	{
		std::scoped_lock lk(g_lock);
		if (index < 0 || static_cast<std::size_t>(index) >= g_list.size()) {
			return {};
		}
		return g_list[static_cast<std::size_t>(index)];
	}

	std::string GetActorName(std::int32_t index)
	{
		auto* a = GetActor(index);
		return GetActorNameSafe(a);
	}
}