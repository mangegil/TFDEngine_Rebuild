#include "TFDActor.h"
#include "TFDSettings.h"
#include "TFDTeammateManager.h"
#include "TFDTame.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <limits>
#include <numeric>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <RE/B/BGSKeyword.h>
#include <RE/B/BGSListForm.h>
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESFile.h>
#include <RE/T/TESForm.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

namespace TFD::Actor
{
    bool SharesAllowedFactionExact(RE::Actor* lhs, RE::Actor* rhs)
    {
        return Ops::SharesAllowedFactionExact(lhs, rhs);
    }
}


namespace TFD::Actor::PleasureAwareness
{
    namespace
    {
        struct AwarenessEntry
        {
            RE::ActorHandle actor{};
            float distance{ 0.0f };
            bool lineOfSightToPlayer{ false };
        };

        std::mutex g_awarenessLock;
        std::vector<AwarenessEntry> g_awarenessActors;

        static constexpr std::int32_t kDefaultMaxActors = 16;
        static constexpr std::int32_t kHardMaxActors = 64;

        RE::BGSKeyword* GetActorTypeNpcKeywordForAwareness()
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

        bool IsNpcLikeForAwareness(RE::Actor* actor)
        {
            if (!actor) {
                return false;
            }

            auto* kwNpc = GetActorTypeNpcKeywordForAwareness();
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

        float DistanceBetweenRefs(RE::TESObjectREFR* a, RE::TESObjectREFR* b)
        {
            if (!a || !b) {
                return 0.0f;
            }

            const auto pa = a->GetPosition();
            const auto pb = b->GetPosition();
            const float dx = pa.x - pb.x;
            const float dy = pa.y - pb.y;
            const float dz = pa.z - pb.z;
            return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
        }

        bool IsSameLoadedArea(RE::Actor* actor, RE::Actor* player)
        {
            if (!actor || !player) {
                return false;
            }

            auto* playerCell = player->GetParentCell();
            auto* actorCell = actor->GetParentCell();
            if (playerCell && actorCell != playerCell) {
                return false;
            }

            auto* playerWorld = player->GetWorldspace();
            if (playerWorld && actor->GetWorldspace() != playerWorld) {
                return false;
            }

            return true;
        }

        bool HasLineOfSightToPlayer(RE::Actor* actor, RE::Actor* player)
        {
            if (!actor || !player) {
                return false;
            }
            bool hasLOSData = false;
            return actor->HasLineOfSight(player, hasLOSData);
        }

        std::vector<RE::ActorHandle> CollectLoadedActorHandlesForAwareness()
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
    }

    std::int32_t ScanNearbyPleasureActors(float radius, std::int32_t maxCount, bool npcOnly, bool requireLineOfSight)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            std::scoped_lock lk(g_awarenessLock);
            g_awarenessActors.clear();
            return 0;
        }

        if (radius < 0.0f) {
            radius = 0.0f;
        }
        if (maxCount <= 0) {
            maxCount = kDefaultMaxActors;
        }
        maxCount = (std::min)(maxCount, kHardMaxActors);

        const auto handles = CollectLoadedActorHandlesForAwareness();
        std::unordered_set<RE::FormID> seen;
        seen.reserve(handles.size() * 2);

        std::vector<AwarenessEntry> temp;
        temp.reserve(static_cast<std::size_t>(maxCount));

        std::uint32_t skippedDead = 0;
        std::uint32_t skippedNo3D = 0;
        std::uint32_t skippedArea = 0;
        std::uint32_t skippedDistance = 0;
        std::uint32_t skippedNpc = 0;
        std::uint32_t skippedLos = 0;

        for (auto const& h : handles) {
            auto sp = h.get();
            auto* actor = sp.get();
            if (!actor || actor == player) {
                continue;
            }

            const auto formID = actor->GetFormID();
            if (formID == 0 || !seen.insert(formID).second) {
                continue;
            }

            if (actor->IsDead() || actor->IsDisabled()) {
                ++skippedDead;
                continue;
            }
            if (!actor->Is3DLoaded()) {
                ++skippedNo3D;
                continue;
            }
            if (!IsSameLoadedArea(actor, player)) {
                ++skippedArea;
                continue;
            }
            if (npcOnly && !IsNpcLikeForAwareness(actor)) {
                ++skippedNpc;
                continue;
            }

            const float distance = DistanceBetweenRefs(actor, player);
            if (radius > 0.0f && distance > radius) {
                ++skippedDistance;
                continue;
            }

            const bool hasLOS = HasLineOfSightToPlayer(actor, player);
            if (requireLineOfSight && !hasLOS) {
                ++skippedLos;
                continue;
            }

            AwarenessEntry entry{};
            entry.actor = actor->GetHandle();
            entry.distance = distance;
            entry.lineOfSightToPlayer = hasLOS;
            temp.push_back(entry);
        }

        std::sort(temp.begin(), temp.end(), [](const AwarenessEntry& lhs, const AwarenessEntry& rhs) {
            if (lhs.lineOfSightToPlayer != rhs.lineOfSightToPlayer) {
                return lhs.lineOfSightToPlayer && !rhs.lineOfSightToPlayer;
            }
            return lhs.distance < rhs.distance;
            });

        if (temp.size() > static_cast<std::size_t>(maxCount)) {
            temp.resize(static_cast<std::size_t>(maxCount));
        }

        const auto kept = temp.size();
        std::uint32_t losCount = 0;
        for (auto const& entry : temp) {
            if (entry.lineOfSightToPlayer) {
                ++losCount;
            }
        }

        {
            std::scoped_lock lk(g_awarenessLock);
            g_awarenessActors = std::move(temp);
        }

        spdlog::info("[TFD][Actor][DS04] pleasure awareness scan radius={} max={} npcOnly={} requireLOS={} kept={} los={} skipped[dead={} no3d={} area={} npc={} distance={} los={}]",
            radius,
            maxCount,
            npcOnly ? 1 : 0,
            requireLineOfSight ? 1 : 0,
            kept,
            losCount,
            skippedDead,
            skippedNo3D,
            skippedArea,
            skippedNpc,
            skippedDistance,
            skippedLos);

        return static_cast<std::int32_t>(kept);
    }

    std::int32_t GetCount()
    {
        std::scoped_lock lk(g_awarenessLock);
        return static_cast<std::int32_t>(g_awarenessActors.size());
    }

    RE::Actor* GetActor(std::int32_t index)
    {
        std::scoped_lock lk(g_awarenessLock);
        if (index < 0 || static_cast<std::size_t>(index) >= g_awarenessActors.size()) {
            return nullptr;
        }
        auto sp = g_awarenessActors[static_cast<std::size_t>(index)].actor.get();
        return sp.get();
    }

    float GetDistance(std::int32_t index)
    {
        std::scoped_lock lk(g_awarenessLock);
        if (index < 0 || static_cast<std::size_t>(index) >= g_awarenessActors.size()) {
            return 0.0f;
        }
        return g_awarenessActors[static_cast<std::size_t>(index)].distance;
    }

    bool HasLineOfSight(std::int32_t index)
    {
        std::scoped_lock lk(g_awarenessLock);
        if (index < 0 || static_cast<std::size_t>(index) >= g_awarenessActors.size()) {
            return false;
        }
        return g_awarenessActors[static_cast<std::size_t>(index)].lineOfSightToPlayer;
    }
}

namespace TFD::Actor
{
    namespace
    {
        std::int32_t PapyrusScanNearbyPleasureActors(RE::StaticFunctionTag*, float radius, std::int32_t maxCount, bool npcOnly, bool requireLineOfSight)
        {
            return PleasureAwareness::ScanNearbyPleasureActors(radius, maxCount, npcOnly, requireLineOfSight);
        }

        std::int32_t PapyrusGetPleasureScanCount(RE::StaticFunctionTag*)
        {
            return PleasureAwareness::GetCount();
        }

        RE::Actor* PapyrusGetPleasureScanActor(RE::StaticFunctionTag*, std::int32_t index)
        {
            return PleasureAwareness::GetActor(index);
        }

        float PapyrusGetPleasureScanDistance(RE::StaticFunctionTag*, std::int32_t index)
        {
            return PleasureAwareness::GetDistance(index);
        }

        bool PapyrusGetPleasureScanHasLineOfSight(RE::StaticFunctionTag*, std::int32_t index)
        {
            return PleasureAwareness::HasLineOfSight(index);
        }
    }

    bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm)
    {
        if (!a_vm) {
            return false;
        }

        a_vm->RegisterFunction("ScanNearbyPleasureActors", "TFDActorNative", PapyrusScanNearbyPleasureActors);
        a_vm->RegisterFunction("GetPleasureScanCount", "TFDActorNative", PapyrusGetPleasureScanCount);
        a_vm->RegisterFunction("GetPleasureScanActor", "TFDActorNative", PapyrusGetPleasureScanActor);
        a_vm->RegisterFunction("GetPleasureScanDistance", "TFDActorNative", PapyrusGetPleasureScanDistance);
        a_vm->RegisterFunction("GetPleasureScanHasLineOfSight", "TFDActorNative", PapyrusGetPleasureScanHasLineOfSight);

        spdlog::info("[TFD][Actor][DS04] Papyrus natives registered for pleasure awareness scan");
        return true;
    }
}

namespace TFD::Actor::Scan
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

	RE::Actor* SelectFacingTarget()
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

namespace TFD::Actor
{
    namespace
    {
        std::vector<Scan::Entry> CollectEntriesFromLegacyScan(float radius, bool npcOnly)
        {
            Scan::Rescan(radius, npcOnly);
            const auto count = Scan::GetCount();
            std::vector<Scan::Entry> out;
            out.reserve(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i) {
                out.push_back(Scan::GetEntry(i));
            }
            return out;
        }

        bool IsStandingActor(RE::Actor* actor)
        {
            if (!actor || actor->IsDead() || actor->IsDisabled()) {
                return false;
            }
            return actor->GetActorValue(RE::ActorValue::kHealth) > 0.0f;
        }

        bool IsPlayerSideActor(RE::Actor* actor)
        {
            return actor && (actor->IsPlayerTeammate() || TFD::TeammateManager::IsActiveFollowerActor(actor));
        }

        float GetHealthPct(RE::Actor* actor)
        {
            if (!actor) {
                return 0.0f;
            }
            const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
            const float hpNow = (std::max)(0.0f, actor->GetActorValue(RE::ActorValue::kHealth));
            return (hpNow / hpMax) * 100.0f;
        }

        bool IsCombatSupportedAggressor(RE::Actor* actor, RE::Actor* player)
        {
            if (!actor || !player || actor == player) {
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

        bool IsReasonableCombatAggressor(RE::Actor* actor, RE::Actor* player, float maxDist, float* outDistance = nullptr)
        {
            if (outDistance) {
                *outDistance = -1.0f;
            }
            if (!IsCombatSupportedAggressor(actor, player)) {
                return false;
            }
            if (!actor->Is3DLoaded()) {
                return false;
            }
            if (IsDownByHealthThreshold(actor, TFD::Settings::GetEnemyDownedThresholdPct())) {
                return false;
            }
            auto* playerCell = player->GetParentCell();
            auto* actorCell = actor->GetParentCell();
            if (playerCell && actorCell != playerCell) {
                return false;
            }
            auto* playerWs = player->GetWorldspace();
            if (playerWs && actor->GetWorldspace() != playerWs) {
                return false;
            }
            const auto pp = player->GetPosition();
            const auto ap = actor->GetPosition();
            const float dx = ap.x - pp.x;
            const float dy = ap.y - pp.y;
            const float dz = ap.z - pp.z;
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (outDistance) {
                *outDistance = dist;
            }
            if (maxDist > 0.0f && dist > maxDist) {
                return false;
            }
            if (actor->IsHostileToActor(player) || actor->IsInCombat()) {
                return true;
            }
            auto* currentTarget = GetCurrentTarget(actor);
            if (currentTarget == player) {
                return true;
            }
            if (currentTarget && TFD::TeammateManager::IsActiveFollowerActor(currentTarget)) {
                return true;
            }
            return false;
        }

        RE::ActorHandle g_lastAggressor;

        float ScoreSpeakerCandidate(const ActorInfo& info)
        {
            float score = 0.0f;
            if (!info.standing) {
                return -1000000.0f;
            }
            if (info.hostileToPlayer) {
                score += 4000.0f;
            }
            if (info.isTargetingAnyone) {
                score += 1500.0f;
            }
            if (info.isMutuallyEngaged) {
                score += 900.0f;
            }
            score -= info.dist;
            return score;
        }

        void FinalizeCoalitionPresentation(const Snapshot& snapshot, CoalitionInfo& coalition)
        {
            coalition.speakerCandidateFormID = 0;
            coalition.crowdCandidateFormIDs.clear();

            float bestScore = -1000000.0f;
            for (auto formID : coalition.standingMemberFormIDs) {
                const ActorInfo* info = nullptr;
                for (auto const& candidate : snapshot.actors) {
                    if (candidate.formID == formID) {
                        info = &candidate;
                        break;
                    }
                }
                if (!info) {
                    continue;
                }
                const float score = ScoreSpeakerCandidate(*info);
                if (coalition.speakerCandidateFormID == 0 || score > bestScore) {
                    bestScore = score;
                    coalition.speakerCandidateFormID = formID;
                }
            }

            for (auto formID : coalition.standingMemberFormIDs) {
                if (formID != coalition.speakerCandidateFormID) {
                    coalition.crowdCandidateFormIDs.push_back(formID);
                }
            }
        }

        struct CoalitionDsu
        {
            explicit CoalitionDsu(std::size_t n) : parent(n)
            {
                std::iota(parent.begin(), parent.end(), 0);
            }

            int find(int x)
            {
                if (parent[x] != x) {
                    parent[x] = find(parent[x]);
                }
                return parent[x];
            }

            void unite(int a, int b)
            {
                a = find(a);
                b = find(b);
                if (a != b) {
                    parent[b] = a;
                }
            }

            std::vector<int> parent;
        };

        bool ShouldLinkActors(RE::Actor* a, const ActorInfo& ai, RE::Actor* b, const ActorInfo& bi)
        {
            if (!a || !b) {
                return false;
            }
            if (!ai.isBattleParticipant || !bi.isBattleParticipant) {
                return false;
            }
            if (ai.playerSide != bi.playerSide) {
                return false;
            }
            if (ai.playerSide && bi.playerSide) {
                return true;
            }
            if (a->IsHostileToActor(b) || b->IsHostileToActor(a)) {
                return false;
            }
            if (SharesAllowedFactionExact(a, b)) {
                return true;
            }
            if (ai.currentTargetFormID != 0 && ai.currentTargetFormID == bi.currentTargetFormID) {
                return true;
            }
            return false;
        }
    }

    Snapshot BuildSnapshot(float radius, bool npcOnly)
    {
        ScanOptions options{};
        options.radius = radius;
        options.npcOnly = npcOnly;
        return BuildSnapshot(RE::PlayerCharacter::GetSingleton(), options);
    }

    Snapshot BuildSnapshot(RE::Actor* player, const ScanOptions& options)
    {
        Snapshot snapshot{};
        if (!player) {
            return snapshot;
        }

        snapshot.player = player->GetHandle();
        snapshot.options = options;

        const auto scannedEntries = CollectEntriesFromLegacyScan(options.radius, options.npcOnly);
        snapshot.actors.reserve(scannedEntries.size());

        std::unordered_map<std::uint32_t, std::size_t> indexByFormID;
        indexByFormID.reserve(scannedEntries.size() * 2);

        for (auto const& entry : scannedEntries) {
            auto actorSp = entry.actor.get();
            auto* actor = actorSp.get();
            if (!actor) {
                continue;
            }

            ActorInfo info{};
            info.actor = entry.actor;
            info.formID = actor->GetFormID();
            info.dist = entry.dist;
            info.hostileToPlayer = entry.hostile || actor->IsHostileToActor(player);
            info.inCombat = entry.inCombat || actor->IsInCombat();
            info.standing = IsStandingActor(actor);
            info.playerSide = IsPlayerSideActor(actor);

            auto targetSp = actor->GetActorRuntimeData().currentCombatTarget.get();
            if (auto* target = targetSp.get()) {
                info.currentTarget = target->GetHandle();
                info.currentTargetFormID = target->GetFormID();
                info.isTargetingAnyone = info.currentTargetFormID != 0;
            }

            indexByFormID.emplace(info.formID, snapshot.actors.size());
            snapshot.actors.push_back(std::move(info));
        }

        for (auto& info : snapshot.actors) {
            if (!info.currentTargetFormID) {
                continue;
            }
            auto it = indexByFormID.find(info.currentTargetFormID);
            if (it == indexByFormID.end()) {
                continue;
            }
            snapshot.actors[it->second].targetedBy.push_back(info.formID);
        }

        for (auto& info : snapshot.actors) {
            info.isTargetedByAnyone = !info.targetedBy.empty();
            if (info.currentTargetFormID) {
                auto it = indexByFormID.find(info.currentTargetFormID);
                if (it != indexByFormID.end()) {
                    auto& targetInfo = snapshot.actors[it->second];
                    info.isMutuallyEngaged = targetInfo.currentTargetFormID == info.formID;
                }
            }

            info.isBattleParticipant = info.standing && (info.inCombat || info.isTargetingAnyone || info.isTargetedByAnyone || info.hostileToPlayer || info.playerSide);
            info.isOutsider = info.standing && !info.isBattleParticipant;
            info.participation = info.isBattleParticipant ? ParticipationKind::Participant : (info.isOutsider ? ParticipationKind::Outsider : ParticipationKind::None);
            if (info.isOutsider) {
                snapshot.outsiderFormIDs.push_back(info.formID);
            }
        }

        CoalitionDsu dsu(snapshot.actors.size());
        for (std::size_t i = 0; i < snapshot.actors.size(); ++i) {
            auto* a = snapshot.actors[i].get();
            for (std::size_t j = i + 1; j < snapshot.actors.size(); ++j) {
                auto* b = snapshot.actors[j].get();
                if (ShouldLinkActors(a, snapshot.actors[i], b, snapshot.actors[j])) {
                    dsu.unite(static_cast<int>(i), static_cast<int>(j));
                }
            }
        }

        std::unordered_map<int, std::int32_t> coalitionIdByRoot;
        std::unordered_map<std::int32_t, std::size_t> coalitionIndexById;
        std::int32_t nextCoalitionID = 1;

        for (std::size_t i = 0; i < snapshot.actors.size(); ++i) {
            auto& info = snapshot.actors[i];
            if (!info.isBattleParticipant) {
                continue;
            }
            const int root = dsu.find(static_cast<int>(i));
            auto [it, inserted] = coalitionIdByRoot.emplace(root, nextCoalitionID);
            if (inserted) {
                CoalitionInfo coalition{};
                coalition.coalitionID = nextCoalitionID;
                snapshot.coalitions.push_back(coalition);
                coalitionIndexById.emplace(nextCoalitionID, snapshot.coalitions.size() - 1);
                ++nextCoalitionID;
            }
            info.coalitionID = it->second;
            auto& coalition = snapshot.coalitions[coalitionIndexById[it->second]];
            coalition.coalitionID = it->second;
            coalition.memberCount += 1;
            if (info.standing) {
                coalition.standingCount += 1;
                coalition.standingMemberFormIDs.push_back(info.formID);
            }
            coalition.playerSide = coalition.playerSide || info.playerSide;
            coalition.memberFormIDs.push_back(info.formID);
        }

        for (auto& coalition : snapshot.coalitions) {
            if (coalition.playerSide && snapshot.playerCoalitionID < 0) {
                snapshot.playerCoalitionID = coalition.coalitionID;
            }
        }
        for (auto& coalition : snapshot.coalitions) {
            coalition.hostileToPlayerSide = !coalition.playerSide;
            FinalizeCoalitionPresentation(snapshot, coalition);
        }

        snapshot.activeCoalitionCount = 0;
        std::uint32_t activeHostileCoalitions = 0;
        for (const auto& coalition : snapshot.coalitions) {
            if (coalition.standingCount > 0) {
                ++snapshot.activeCoalitionCount;
                if (coalition.hostileToPlayerSide) {
                    ++activeHostileCoalitions;
                    snapshot.winningCoalitionCandidateID = coalition.coalitionID;
                } else if (snapshot.winningCoalitionCandidateID < 0) {
                    snapshot.winningCoalitionCandidateID = coalition.coalitionID;
                }
            }
        }
        snapshot.conflictResolved = snapshot.activeCoalitionCount <= 1;
        if (activeHostileCoalitions != 1) {
            if (!snapshot.conflictResolved) {
                snapshot.winningCoalitionCandidateID = -1;
            } else if (activeHostileCoalitions == 0 && snapshot.activeCoalitionCount == 1) {
                // Keep the single remaining coalition even when it is player side.
            } else if (activeHostileCoalitions == 0) {
                snapshot.winningCoalitionCandidateID = -1;
            }
        }

        return snapshot;
    }

    const ActorInfo* FindActorInfo(const Snapshot& snapshot, RE::Actor* actor)
    {
        if (!actor) {
            return nullptr;
        }
        const auto id = actor->GetFormID();
        for (auto const& info : snapshot.actors) {
            if (info.formID == id) {
                return &info;
            }
        }
        return nullptr;
    }

    const CoalitionInfo* FindCoalition(const Snapshot& snapshot, std::int32_t coalitionID)
    {
        if (coalitionID < 0) {
            return nullptr;
        }
        for (auto const& coalition : snapshot.coalitions) {
            if (coalition.coalitionID == coalitionID) {
                return &coalition;
            }
        }
        return nullptr;
    }

    const ActorInfo* FindActorInfo(const Snapshot& snapshot, std::uint32_t formID)
    {
        if (formID == 0) {
            return nullptr;
        }
        for (auto const& info : snapshot.actors) {
            if (info.formID == formID) {
                return &info;
            }
        }
        return nullptr;
    }

    RE::Actor* GetCurrentTarget(const Snapshot& snapshot, RE::Actor* actor)
    {
        if (!actor) {
            return nullptr;
        }
        if (auto* info = FindActorInfo(snapshot, actor)) {
            return info->getCurrentTarget();
        }
        auto targetSp = actor->GetActorRuntimeData().currentCombatTarget.get();
        return targetSp.get();
    }

    RE::Actor* GetCurrentTarget(RE::Actor* actor)
    {
        if (!actor) {
            return nullptr;
        }
        auto targetSp = actor->GetActorRuntimeData().currentCombatTarget.get();
        return targetSp.get();
    }

    RE::Actor* ResolveSpeakerCandidate(const Snapshot& snapshot, std::int32_t coalitionID)
    {
        auto* coalition = FindCoalition(snapshot, coalitionID);
        if (!coalition || coalition->speakerCandidateFormID == 0) {
            return nullptr;
        }
        if (auto* info = FindActorInfo(snapshot, coalition->speakerCandidateFormID)) {
            return info->get();
        }
        return nullptr;
    }

    std::vector<RE::Actor*> ResolveCrowdCandidates(const Snapshot& snapshot, std::int32_t coalitionID)
    {
        std::vector<RE::Actor*> out;
        auto* coalition = FindCoalition(snapshot, coalitionID);
        if (!coalition) {
            return out;
        }
        out.reserve(coalition->crowdCandidateFormIDs.size());
        for (auto formID : coalition->crowdCandidateFormIDs) {
            if (auto* info = FindActorInfo(snapshot, formID)) {
                if (auto* actor = info->get()) {
                    out.push_back(actor);
                }
            }
        }
        return out;
    }

    std::vector<RE::Actor*> ResolveStandingCoalitionMembers(const Snapshot& snapshot, std::int32_t coalitionID)
    {
        std::vector<RE::Actor*> out;
        auto* coalition = FindCoalition(snapshot, coalitionID);
        if (!coalition) {
            return out;
        }
        out.reserve(coalition->standingMemberFormIDs.size());
        for (auto formID : coalition->standingMemberFormIDs) {
            if (auto* info = FindActorInfo(snapshot, formID)) {
                if (auto* actor = info->get()) {
                    out.push_back(actor);
                }
            }
        }
        return out;
    }

    std::vector<RE::Actor*> ResolveStandingPlayerSideActors(const Snapshot& snapshot, bool includePlayer)
    {
        std::vector<RE::Actor*> out;
        auto* coalition = FindCoalition(snapshot, snapshot.playerCoalitionID);
        if (!coalition) {
            return out;
        }
        out.reserve(coalition->standingMemberFormIDs.size());
        for (auto formID : coalition->standingMemberFormIDs) {
            if (auto* info = FindActorInfo(snapshot, formID)) {
                auto* actor = info->get();
                if (!actor) {
                    continue;
                }
                if (!includePlayer) {
                    auto playerSp = snapshot.player.get();
                    if (auto* player = playerSp.get(); player && actor == player) {
                        continue;
                    }
                }
                out.push_back(actor);
            }
        }
        return out;
    }

    std::vector<RE::Actor*> GetAttackersOf(const Snapshot& snapshot, RE::Actor* actor)
    {
        std::vector<RE::Actor*> out;
        if (!actor) {
            return out;
        }
        const auto targetID = actor->GetFormID();
        for (auto const& other : snapshot.actors) {
            if (other.currentTargetFormID == targetID) {
                if (auto* attacker = other.get()) {
                    out.push_back(attacker);
                }
            }
        }
        return out;
    }

    bool IsActorTargetingAnyone(const Snapshot& snapshot, RE::Actor* actor)
    {
        if (auto* info = FindActorInfo(snapshot, actor)) {
            return info->isTargetingAnyone;
        }
        if (!actor) {
            return false;
        }
        auto targetSp = actor->GetActorRuntimeData().currentCombatTarget.get();
        return targetSp.get() != nullptr;
    }

    bool IsActorTargetedByAnyone(const Snapshot& snapshot, RE::Actor* actor)
    {
        if (!actor) {
            return false;
        }
        if (auto* info = FindActorInfo(snapshot, actor)) {
            return info->isTargetedByAnyone;
        }
        const auto targetID = actor->GetFormID();
        for (auto const& other : snapshot.actors) {
            if (other.currentTargetFormID == targetID) {
                return true;
            }
        }
        return false;
    }

    bool IsMutuallyEngaged(const Snapshot& snapshot, RE::Actor* actor)
    {
        if (!actor) {
            return false;
        }
        if (auto* info = FindActorInfo(snapshot, actor)) {
            return info->isMutuallyEngaged;
        }
        auto* target = GetCurrentTarget(snapshot, actor);
        return target && GetCurrentTarget(snapshot, target) == actor;
    }

    bool IsActorParticipatingInBattle(const Snapshot& snapshot, RE::Actor* actor)
    {
        if (auto* info = FindActorInfo(snapshot, actor)) {
            return info->isBattleParticipant;
        }
        return false;
    }

    bool IsActorOutsider(const Snapshot& snapshot, RE::Actor* actor)
    {
        if (auto* info = FindActorInfo(snapshot, actor)) {
            return info->isOutsider;
        }
        return false;
    }

    bool HasStandingPlayerSide(const Snapshot& snapshot)
    {
        auto* coalition = FindCoalition(snapshot, snapshot.playerCoalitionID);
        return coalition && coalition->standingCount > 0;
    }

    bool HasStandingTeammateOnPlayerSide(const Snapshot& snapshot)
    {
        return HasStandingPlayerSide(snapshot);
    }

    bool HasStandingHostileCoalition(const Snapshot& snapshot)
    {
        for (const auto& coalition : snapshot.coalitions) {
            if (!coalition.playerSide && coalition.standingCount > 0) {
                return true;
            }
        }
        return false;
    }

    bool IsConflictResolved(const Snapshot& snapshot)
    {
        return snapshot.conflictResolved;
    }

    bool IsDownByHealthThreshold(RE::Actor* actor, float thresholdPct)
    {
        if (!actor || actor->IsDisabled() || actor->IsDead()) {
            return true;
        }
        return GetHealthPct(actor) <= std::clamp(thresholdPct, 2.0f, 95.0f);
    }

    RE::Actor* FindBestAggressor(float radius, RE::Actor* player)
    {
        player = player ? player : RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return nullptr;
        }

        auto snapshot = BuildSnapshot((std::max)(radius, 1600.0f), false);
        RE::Actor* best = nullptr;
        float bestScore = std::numeric_limits<float>::max();
        for (const auto& info : snapshot.actors) {
            auto* actor = info.get();
            float dist = -1.0f;
            if (!IsReasonableCombatAggressor(actor, player, radius, &dist)) {
                continue;
            }
            float score = dist;
            auto* target = GetCurrentTarget(snapshot, actor);
            if (target == player) {
                score -= 3000.0f;
            } else if (target && TFD::TeammateManager::IsActiveFollowerActor(target)) {
                score -= 1200.0f;
            }
            if (actor->IsHostileToActor(player)) {
                score -= 400.0f;
            }
            if (actor->IsInCombat()) {
                score -= 150.0f;
            }
            if (score < bestScore) {
                bestScore = score;
                best = actor;
            }
        }
        if (best) {
            g_lastAggressor = best->GetHandle();
        }
        return best;
    }

    RE::Actor* ResolveAggressor(float radius, RE::Actor* player)
    {
        player = player ? player : RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return nullptr;
        }
        radius = radius > 0.0f ? radius : (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 400.0f);

        auto resolveIfReasonable = [&](RE::Actor* actor) -> RE::Actor* {
            float dist = -1.0f;
            return IsReasonableCombatAggressor(actor, player, radius, &dist) ? actor : nullptr;
        };

        if (g_lastAggressor) {
            if (auto actor = g_lastAggressor.get().get()) {
                if (auto* resolved = resolveIfReasonable(actor->As<RE::Actor>()); resolved) {
                    return resolved;
                }
            }
        }

        return FindBestAggressor(radius, player);
    }
}

namespace TFD::Actor::Interaction
{
    namespace
    {
        constexpr float kMaxTameDistance = 768.0f;
        constexpr float kMaxTrucePreCombatDistance = 2500.0f;
        constexpr float kMaxTruceInCombatDistance = 1600.0f;

        constexpr const char* kPluginName = "TFDEngine.esp";

        // Local FormIDs from TFDEngine.esp
        constexpr std::uint32_t kDialogueCapableRacesLocalID = 0x00047618;
        constexpr std::uint32_t kDialogueCapableActorsLocalID = 0x00047619;
        constexpr std::uint32_t kSimpleCommandRacesLocalID = 0x0004761A;
        constexpr std::uint32_t kNonverbalIntelligentRacesLocalID = 0x0004761B;
        constexpr std::uint32_t kBeastRacesLocalID = 0x0004761C;

        // Skyrim.esm: ActorTypeNPC
        constexpr RE::FormID kActorTypeNpcFormID = 0x00013794;

        struct FormLists
        {
            RE::BGSListForm* dialogueCapableRaces{ nullptr };
            RE::BGSListForm* dialogueCapableActors{ nullptr };
            RE::BGSListForm* simpleCommandRaces{ nullptr };
            RE::BGSListForm* nonverbalIntelligentRaces{ nullptr };
            RE::BGSListForm* beastRaces{ nullptr };
            bool resolved{ false };
        };

        RE::TESNPC* GetActorBase(RE::Actor* actor)
        {
            if (!actor) {
                return nullptr;
            }

            return actor->GetActorBase();
        }

        RE::TESRace* GetRace(RE::Actor* actor)
        {
            auto* base = GetActorBase(actor);
            if (!base) {
                return nullptr;
            }

            return base->GetRace();
        }

        bool IsIgnoredActor(RE::Actor* actor)
        {
            return !actor || actor->IsPlayerRef();
        }

        RE::BGSKeyword* GetActorTypeNpcKeyword()
        {
            static RE::BGSKeyword* cached = nullptr;
            static bool tried = false;

            if (tried) {
                return cached;
            }

            tried = true;
            cached = RE::TESForm::LookupByID<RE::BGSKeyword>(kActorTypeNpcFormID);
            return cached;
        }

        bool HasSafeNpcKeyword(RE::Actor* actor)
        {
            if (!actor) {
                return false;
            }

            auto* kwNpc = GetActorTypeNpcKeyword();
            if (!kwNpc) {
                return false;
            }

            if (auto* race = GetRace(actor); race && race->HasKeyword(kwNpc)) {
                return true;
            }

            if (auto* base = GetActorBase(actor); base && base->HasKeyword(kwNpc)) {
                return true;
            }

            if (actor->HasKeyword(kwNpc)) {
                return true;
            }

            return false;
        }

        bool IsDistanceTooFarForTame(float distanceToPlayer)
        {
            return distanceToPlayer > kMaxTameDistance;
        }

        float GetMaxTruceDistance(TFD::Actor::Interaction::TruceMode truceMode, bool targetInCombat)
        {
            switch (truceMode) {
            case TFD::Actor::Interaction::TruceMode::PreCombat:
                return kMaxTrucePreCombatDistance;
            case TFD::Actor::Interaction::TruceMode::InCombat:
                return kMaxTruceInCombatDistance;
            case TFD::Actor::Interaction::TruceMode::Auto:
            default:
                return targetInCombat ? kMaxTruceInCombatDistance : kMaxTrucePreCombatDistance;
            }
        }

        bool IsDistanceTooFarForTruce(
            float distanceToPlayer,
            TFD::Actor::Interaction::TruceMode truceMode,
            bool targetInCombat)
        {
            return distanceToPlayer > GetMaxTruceDistance(truceMode, targetInCombat);
        }

        RE::FormID ResolveRuntimeFormID(std::uint32_t localFormID)
        {
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!dataHandler) {
                return 0;
            }

            auto* mod = dataHandler->LookupLoadedModByName(kPluginName);
            if (!mod) {
                return 0;
            }

            localFormID &= 0x00FFFFFF;

            if (mod->compileIndex != static_cast<std::uint32_t>(-1) &&
                mod->compileIndex != 0xFF) {
                return (static_cast<RE::FormID>(mod->compileIndex) << 24) | localFormID;
            }

            if (mod->smallFileCompileIndex != static_cast<std::uint32_t>(-1) &&
                mod->smallFileCompileIndex != 0xFFFF) {
                return 0xFE000000 |
                    ((static_cast<RE::FormID>(mod->smallFileCompileIndex) & 0xFFF) << 12) |
                    (localFormID & 0x00000FFF);
            }

            return 0;
        }

        template <class T>
        T* LookupOwnForm(std::uint32_t localFormID)
        {
            const auto runtimeFormID = ResolveRuntimeFormID(localFormID);
            if (!runtimeFormID) {
                return nullptr;
            }

            return RE::TESForm::LookupByID<T>(runtimeFormID);
        }

        bool ListHasForm(RE::BGSListForm* list, RE::TESForm* form)
        {
            return list && form && list->HasForm(form);
        }

        bool ListHasActor(RE::BGSListForm* list, RE::Actor* actor)
        {
            if (!list || !actor) {
                return false;
            }

            if (list->HasForm(actor)) {
                return true;
            }

            auto* base = GetActorBase(actor);
            if (base && list->HasForm(base)) {
                return true;
            }

            return false;
        }

        FormLists& GetFormLists()
        {
            static FormLists lists;
            static std::once_flag initFlag;

            std::call_once(initFlag, []() {
                lists.dialogueCapableRaces =
                    LookupOwnForm<RE::BGSListForm>(kDialogueCapableRacesLocalID);
                lists.dialogueCapableActors =
                    LookupOwnForm<RE::BGSListForm>(kDialogueCapableActorsLocalID);
                lists.simpleCommandRaces =
                    LookupOwnForm<RE::BGSListForm>(kSimpleCommandRacesLocalID);
                lists.nonverbalIntelligentRaces =
                    LookupOwnForm<RE::BGSListForm>(kNonverbalIntelligentRacesLocalID);
                lists.beastRaces =
                    LookupOwnForm<RE::BGSListForm>(kBeastRacesLocalID);

                lists.resolved = true;
            });

            return lists;
        }

        bool IsDialogueActorOverride(RE::Actor* actor)
        {
            if (!actor) {
                return false;
            }

            auto& lists = GetFormLists();
            return ListHasActor(lists.dialogueCapableActors, actor);
        }

        bool IsDialogueRaceListed(RE::Actor* actor)
        {
            if (!actor) {
                return false;
            }

            auto* race = GetRace(actor);
            if (!race) {
                return false;
            }

            auto& lists = GetFormLists();
            return ListHasForm(lists.dialogueCapableRaces, race);
        }

        bool HasExplicitDialogueCapability(RE::Actor* actor)
        {
            return IsDialogueActorOverride(actor) || IsDialogueRaceListed(actor);
        }

        CreatureClass GetBaseCreatureClassFromLists(RE::Actor* actor)
        {
            if (!actor) {
                return CreatureClass::None;
            }

            auto* race = GetRace(actor);
            if (!race) {
                return CreatureClass::None;
            }

            auto& lists = GetFormLists();

            if (ListHasForm(lists.simpleCommandRaces, race)) {
                return CreatureClass::SimpleCommand;
            }

            if (ListHasForm(lists.nonverbalIntelligentRaces, race)) {
                return CreatureClass::NonverbalIntelligent;
            }

            if (ListHasForm(lists.beastRaces, race)) {
                return CreatureClass::Beast;
            }

            return CreatureClass::None;
        }

        bool AllowsDialogueForClass(RE::Actor* actor, CreatureClass value)
        {
            if (!actor) {
                return false;
            }

            switch (value) {
            case CreatureClass::FullDialogue:
                return true;
            default:
                return false;
            }
        }
    }

    bool IsValidActor(RE::Actor* actor)
    {
        if (!actor) {
            return false;
        }

        if (actor->IsDead()) {
            return false;
        }

        if (IsIgnoredActor(actor)) {
            return false;
        }

        return true;
    }

    CreatureClass GetCreatureClass(RE::Actor* actor)
    {
        if (!IsValidActor(actor)) {
            return CreatureClass::None;
        }

        // Highest priority: explicit actor override.
        // Use this when a specific actor must always behave as full dialogue.
        if (IsDialogueActorOverride(actor)) {
            return CreatureClass::FullDialogue;
        }

        // Second layer: any safe ActorTypeNPC should always behave as full dialogue.
        // This keeps humanoid NPCs on the dialogue path even if they are not in explicit lists.
        if (HasSafeNpcKeyword(actor)) {
            return CreatureClass::FullDialogue;
        }

        // Third layer: explicit dialogue-capable races should stay on the full dialogue path
        // even if they also belong to one of the creature family lists.
        if (IsDialogueRaceListed(actor)) {
            return CreatureClass::FullDialogue;
        }

        // Fourth layer: creature family comes from creature race lists.
        const auto listedCreatureClass = GetBaseCreatureClassFromLists(actor);
        if (listedCreatureClass != CreatureClass::None) {
            return listedCreatureClass;
        }

        // Final fallback stays creature-like, but NPCs should have been promoted above.
        return CreatureClass::UnknownFallback;
    }

    bool IsNegotiable(RE::Actor* actor)
    {
        return GetCreatureClass(actor) == CreatureClass::FullDialogue;
    }

    bool IsCreature(RE::Actor* actor)
    {
        switch (GetCreatureClass(actor)) {
        case CreatureClass::SimpleCommand:
        case CreatureClass::NonverbalIntelligent:
        case CreatureClass::Beast:
        case CreatureClass::UnknownFallback:
            return true;
        default:
            return false;
        }
    }

    bool CanUseTruce(RE::Actor* actor)
    {
        if (!IsValidActor(actor)) {
            return false;
        }

        return GetCreatureClass(actor) != CreatureClass::None;
    }

    bool CanUseTame(RE::Actor* actor)
    {
        if (!IsValidActor(actor)) {
            return false;
        }

        return IsCreature(actor);
    }

    ClassifyResult ClassifyTarget(
        RE::Actor* player,
        RE::Actor* target,
        bool isCaptivePhase,
        bool targetInCombat,
        float distanceToPlayer,
        TruceMode truceMode)
    {
        ClassifyResult result{};

        if (!player || !target) {
            result.valid = false;
            result.rejectReason = RejectReason::InvalidActor;
            return result;
        }

        if (isCaptivePhase) {
            result.valid = false;
            result.rejectReason = RejectReason::CaptiveOnlyMode;
            return result;
        }

        if (!IsValidActor(target)) {
            result.valid = false;
            result.rejectReason = RejectReason::InvalidActor;
            return result;
        }

        result.creatureClass = GetCreatureClass(target);
        result.allowDialogue = AllowsDialogueForClass(target, result.creatureClass);

        if (result.creatureClass == CreatureClass::None) {
            result.kind = TargetKind::Ignore;
            result.intent = Intent::None;
            result.rejectReason = RejectReason::UnsafeState;
            result.valid = false;
            return result;
        }

        if (result.creatureClass == CreatureClass::FullDialogue) {
            if (IsDistanceTooFarForTruce(distanceToPlayer, truceMode, targetInCombat)) {
                result.valid = false;
                result.rejectReason = RejectReason::TooFar;
                return result;
            }

            result.kind = TargetKind::Negotiable;
            result.intent = Intent::Truce;
            result.rejectReason = RejectReason::None;
            result.valid = true;
            result.negotiable = true;
            result.tameable = false;
            result.allowDialogue = true;
            result.requiresPreCombat = !targetInCombat;
            result.allowsInCombat = true;
            return result;
        }

        if (IsDistanceTooFarForTame(distanceToPlayer)) {
            result.valid = false;
            result.rejectReason = RejectReason::TooFar;
            return result;
        }

        result.kind = TargetKind::Creature;
        result.intent = Intent::Tame;
        result.rejectReason = RejectReason::None;
        result.valid = true;
        result.negotiable = true;
        result.tameable = true;
        result.allowDialogue = AllowsDialogueForClass(target, result.creatureClass);
        result.requiresPreCombat = !targetInCombat;
        result.allowsInCombat = true;
        return result;
    }

    const char* ToString(TargetKind value)

    {
        switch (value) {
        case TargetKind::None:
            return "None";
        case TargetKind::Negotiable:
            return "Negotiable";
        case TargetKind::Creature:
            return "Creature";
        case TargetKind::Ignore:
            return "Ignore";
        default:
            return "Unknown";
        }
    }

    const char* ToString(Intent value)
    {
        switch (value) {
        case Intent::None:
            return "None";
        case Intent::Tame:
            return "Tame";
        case Intent::Truce:
            return "Truce";
        default:
            return "Unknown";
        }
    }

    const char* ToString(RejectReason value)
    {
        switch (value) {
        case RejectReason::None:
            return "None";
        case RejectReason::InvalidActor:
            return "InvalidActor";
        case RejectReason::NotNegotiable:
            return "NotNegotiable";
        case RejectReason::NotCreature:
            return "NotCreature";
        case RejectReason::TameRequiresPreCombat:
            return "TameRequiresPreCombat";
        case RejectReason::TooFar:
            return "TooFar";
        case RejectReason::UnsafeState:
            return "UnsafeState";
        case RejectReason::CaptiveOnlyMode:
            return "CaptiveOnlyMode";
        default:
            return "Unknown";
        }
    }

    const char* ToString(CreatureClass value)
    {
        switch (value) {
        case CreatureClass::None:
            return "None";
        case CreatureClass::FullDialogue:
            return "FullDialogue";
        case CreatureClass::SimpleCommand:
            return "SimpleCommand";
        case CreatureClass::NonverbalIntelligent:
            return "NonverbalIntelligent";
        case CreatureClass::Beast:
            return "Beast";
        case CreatureClass::UnknownFallback:
            return "UnknownFallback";
        default:
            return "Unknown";
        }
    }
}


namespace TFD::Actor::Ops
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

		static constexpr std::size_t kDefeatedEnemyAliasCount = 20;

		struct DefeatedEnemyRegistryCache
		{
			RE::TESQuest* quest{ nullptr };
			RE::TESFaction* faction{ nullptr };
			std::array<RE::BGSRefAlias*, kDefeatedEnemyAliasCount> enemyAliases{};
			bool resolved{ false };
		};

		struct ReleaseFollowGraceEntry
		{
			RE::ActorHandle actor{};
			std::chrono::steady_clock::time_point expiresAt{};
			std::string source{};
			bool hadHelperFaction{ false };
			bool addedHelperFaction{ false };
			bool hadDialogueHelperFaction{ false };
			bool addedDialogueHelperFaction{ false };
			bool hasOriginalAggression{ false };
			float originalAggression{ 0.0f };
		};

		bool g_initialized = false;
		bool g_active = false;
		int g_matchCount = 0;
		std::uint32_t g_sourceActorFormID = 0;

		std::vector<AllowedFactionEntry> g_allowedFactions{};
		TruceQuestRegistryCache g_truceQuestRegistry{};
		DefeatedEnemyRegistryCache g_defeatedEnemyRegistry{};
		std::unordered_map<RE::FormID, ReleaseFollowGraceEntry> g_releaseFollowGraceEntries{};
		TFD::Actor::Ops::DefeatedEnemyQueryHooks g_defeatedEnemyQueryHooks{};
		TFD::Actor::Ops::DefeatedEnemyStateHooks g_defeatedEnemyStateHooks{};
		std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_defeatedReentrySuppress{};

		static RE::TESGlobal* g_joinEnemyStateGlobal = nullptr;
		static RE::TESFaction* g_releaseFollowHelperFaction = nullptr;
		static RE::TESFaction* g_dialogueHelperFaction = nullptr;
		static RE::TESFaction* g_permanentTeammateFaction = nullptr;

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
				g_releaseFollowHelperFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDExpiredTeammate");
				if (!g_releaseFollowHelperFaction) {
					spdlog::warn("[TFD][FactionManager] helper faction not found editorId=TFDExpiredTeammate");
				}
			}
			if (!g_dialogueHelperFaction) {
				g_dialogueHelperFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDPacifyFaction");
				if (!g_dialogueHelperFaction) {
					spdlog::warn("[TFD][FactionManager] dialogue helper faction not found editorId=TFDPacifyFaction");
				}
			}
			if (!g_permanentTeammateFaction) {
				g_permanentTeammateFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDTeammateFaction");
				if (!g_permanentTeammateFaction) {
					spdlog::warn("[TFD][FactionManager] permanent teammate faction not found editorId=TFDTeammateFaction");
				}
			}
		}

		static bool ActorHasPermanentTeammateFaction(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			ResolveReleaseFollowHelperFaction();
			return g_permanentTeammateFaction && GetExactFactionRank(actor, g_permanentTeammateFaction) >= 0;
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
				// TFDCaptiveFaction is a dialogue-condition tag only; do not
				// preserve pacify/release grace because of it.
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

		static RE::BGSKeyword* LookupKeyword(const char* editorID)
		{
			if (!editorID || !editorID[0]) {
				return nullptr;
			}
			return RE::TESForm::LookupByEditorID<RE::BGSKeyword>(editorID);
		}

		static bool ActorHasKeywordByEditorID(RE::Actor* actor, const char* editorID)
		{
			if (!actor) {
				return false;
			}
			auto* kw = LookupKeyword(editorID);
			return kw && actor->HasKeyword(kw);
		}

		static bool IsCaptiveSupportedAggressor(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeNPC")) {
				return true;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeCreature") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeAnimal") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeDragon") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeDaedra") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeGhost") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeUndead")) {
				return false;
			}
			return false;
		}

		static bool IsBleedCrowdSupportedAggressor(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			if (TFD::TeammateManager::IsActiveFollowerActor(actor)) {
				return false;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeDragon") || ActorHasKeywordByEditorID(actor, "ActorTypeGhost")) {
				return false;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeNPC") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeCreature") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeAnimal") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeUndead") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeDaedra")) {
				return true;
			}
			return false;
		}

		static bool IsDefeatedReentrySuppressedInternal(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			auto it = g_defeatedReentrySuppress.find(actor->GetFormID());
			if (it == g_defeatedReentrySuppress.end()) {
				return false;
			}
			if (Now() >= it->second) {
				g_defeatedReentrySuppress.erase(it);
				return false;
			}
			return true;
		}

		static void ResolveDefeatedEnemyRegistry()
		{
			if (g_defeatedEnemyRegistry.resolved) {
				return;
			}
			g_defeatedEnemyRegistry.resolved = true;
			g_defeatedEnemyRegistry.quest = RE::TESForm::LookupByEditorID<RE::TESQuest>("TFDVictoryQuest");
			g_defeatedEnemyRegistry.faction = RE::TESForm::LookupByEditorID<RE::TESFaction>("TFDDefeatedFaction");
			if (!g_defeatedEnemyRegistry.quest) {
				spdlog::warn("[TFD][FactionManager] victory registry quest not found editorId=TFDVictoryQuest");
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

		static bool IsTrackedDefeatedEnemy(RE::Actor* actor)
		{
			return g_defeatedEnemyQueryHooks.isTrackedEnemy ? g_defeatedEnemyQueryHooks.isTrackedEnemy(actor) : false;
		}

		static bool IsLastAggressorActor(RE::Actor* actor)
		{
			return g_defeatedEnemyQueryHooks.isLastAggressor ? g_defeatedEnemyQueryHooks.isLastAggressor(actor) : false;
		}

		static bool IsDefeatedEnemyCandidateInternal(RE::Actor* actor)
		{
			auto* player = Player();
			if (!actor || !player || actor == player || actor->IsDead() || actor->IsDisabled()) {
				return false;
			}
			if (IsDefeatedReentrySuppressedInternal(actor)) {
				return false;
			}
            if (actor->IsPlayerTeammate() || TFD::TeammateManager::IsActiveFollowerActor(actor) || TFD::TeammateManager::IsPlayerSideTeammateActor(actor)) {
                return false;
            }
			if (TFD::Tame::IsCompanion(actor) || TFD::Tame::HasActiveSession(actor)) {
				return false;
			}
			if (!IsBleedCrowdSupportedAggressor(actor)) {
				return false;
			}
			if (actor->IsHostileToActor(player)) {
				return true;
			}
			auto targetSp = actor->GetActorRuntimeData().currentCombatTarget.get();
			if (auto* current = targetSp.get()) {
				if (current == player || TFD::TeammateManager::IsActiveFollowerActor(current)) {
					return true;
				}
			}
			if (IsTrackedDefeatedEnemy(actor)) {
				return true;
			}
			if (IsLastAggressorActor(actor)) {
				return true;
			}
			return actor->IsInCombat();
		}

		static bool IsDefeatedEnemyKnockedInternal(RE::Actor* actor, std::uint8_t lockKindValue, bool defeatedManaged)
		{
			if (!actor) {
				return false;
			}
			return lockKindValue == 2 && defeatedManaged;
		}

		static bool IsDialogueCapableDefeatedEnemyInternal(RE::Actor* actor, std::uint8_t lockKindValue, bool defeatedManaged)
		{
			return IsDefeatedEnemyKnockedInternal(actor, lockKindValue, defeatedManaged) && actor && IsCaptiveSupportedAggressor(actor);
		}

		static bool IsCreatureDefeatedEnemyInternal(RE::Actor* actor, std::uint8_t lockKindValue, bool defeatedManaged)
		{
			if (!IsDefeatedEnemyKnockedInternal(actor, lockKindValue, defeatedManaged) || !actor) {
				return false;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeNPC")) {
				return false;
			}
			return ActorHasKeywordByEditorID(actor, "ActorTypeCreature") || ActorHasKeywordByEditorID(actor, "ActorTypeAnimal") || ActorHasKeywordByEditorID(actor, "ActorTypeDaedra") || ActorHasKeywordByEditorID(actor, "ActorTypeUndead");
		}

		static double GetDefeatedEnemyRemainingSecondsInternal(bool defeatedManaged, std::chrono::steady_clock::time_point deadline)
		{
			if (!defeatedManaged) {
				return 0.0;
			}
			const auto now = Now();
			if (deadline <= now) {
				return 0.0;
			}
			return std::chrono::duration<double>(deadline - now).count();
		}

		static void ApplyDefeatedEnemyPassiveOverrideInternal(RE::Actor* actor, float& savedAggression, bool& aggressionOverridden)
		{
			auto* avo = actor ? actor->AsActorValueOwner() : nullptr;
			if (!avo) {
				return;
			}
			if (!aggressionOverridden) {
				savedAggression = avo->GetActorValue(RE::ActorValue::kAggression);
				aggressionOverridden = true;
			}
			avo->SetActorValue(RE::ActorValue::kAggression, 0.0f);
		}

		static void RestoreDefeatedEnemyPassiveOverrideInternal(RE::Actor* actor, float& savedAggression, bool& aggressionOverridden)
		{
			if (!aggressionOverridden) {
				return;
			}
			auto* avo = actor ? actor->AsActorValueOwner() : nullptr;
			if (avo) {
				avo->SetActorValue(RE::ActorValue::kAggression, savedAggression);
			}
			aggressionOverridden = false;
		}

		static void ClearDefeatedEnemyStateInternal(RE::Actor* actor, int& aliasSlot, bool& factionApplied, bool& defeatedManaged, bool& defeatedAutoDeathIssued, bool& defeatedFatalDamageApplied, std::chrono::steady_clock::time_point& defeatedDeadline, float& savedAggression, bool& aggressionOverridden, const char* reason)
		{
			ClearDefeatedEnemyMirrorInternal(actor, aliasSlot, factionApplied, reason);
			RestoreDefeatedEnemyPassiveOverrideInternal(actor, savedAggression, aggressionOverridden);
			defeatedManaged = false;
			defeatedAutoDeathIssued = false;
			defeatedFatalDamageApplied = false;
			defeatedDeadline = {};
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

		static RE::Actor* ResolveCoalitionSpeakerFromSnapshot(const TFD::Actor::Snapshot& snapshot)
		{
			if (snapshot.winningCoalitionCandidateID >= 0) {
				if (auto* speaker = TFD::Actor::ResolveSpeakerCandidate(snapshot, snapshot.winningCoalitionCandidateID)) {
					return speaker;
				}
			}

			RE::Actor* bestSpeaker = nullptr;
			float bestDist = std::numeric_limits<float>::max();
			for (const auto& coalition : snapshot.coalitions) {
				if (!coalition.hostileToPlayerSide || coalition.standingCount == 0) {
					continue;
				}
				auto* speaker = TFD::Actor::ResolveSpeakerCandidate(snapshot, coalition.coalitionID);
				if (!speaker) {
					continue;
				}
				if (const auto* info = TFD::Actor::FindActorInfo(snapshot, speaker); info && info->dist < bestDist) {
					bestDist = info->dist;
					bestSpeaker = speaker;
				}
			}
			return bestSpeaker;
		}

		static std::vector<RE::Actor*> CollectCoalitionActorsForSpeaker(RE::Actor* speaker)
		{
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

			auto* player = Player();
			if (!player) {
				addUnique(speaker);
				return actors;
			}

			TFD::Actor::ScanOptions options{};
			options.radius = (std::max)(2000.0f, TFD::Settings::GetSweepRadius());
			options.npcOnly = false;
			auto snapshot = TFD::Actor::BuildSnapshot(player, options);
			if (!speaker) {
				speaker = ResolveCoalitionSpeakerFromSnapshot(snapshot);
			}
			if (auto* info = TFD::Actor::FindActorInfo(snapshot, speaker); info && info->coalitionID >= 0) {
				addUnique(speaker);
				for (auto* actor : TFD::Actor::ResolveCrowdCandidates(snapshot, info->coalitionID)) {
					addUnique(actor);
				}
				if (!actors.empty()) {
					return actors;
				}
			}

			addUnique(speaker);
			return actors;
		}

		static std::vector<RE::Actor*> CollectTruceActorsInternal(RE::Actor* speaker)
		{
			ResolveTruceQuestRegistry();

			std::vector<RE::Actor*> actors = CollectCoalitionActorsForSpeaker(speaker);
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

			// Truce aliases are the authoritative visible crowd during dialogue.
			// Merge them even when the snapshot already returned the speaker only.
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


		static void StabilizeReleaseFollowGraceActor(RE::Actor* actor, ReleaseFollowGraceEntry& entry, const char* reason)
		{
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				return;
			}

			ResolveReleaseFollowHelperFaction();
			bool changedFaction = false;
			bool changedAggression = false;

			if (g_releaseFollowHelperFaction) {
				if (!entry.hadHelperFaction && !entry.addedHelperFaction) {
					entry.hadHelperFaction = actor->IsInFaction(g_releaseFollowHelperFaction);
				}
				if (!actor->IsInFaction(g_releaseFollowHelperFaction)) {
					actor->AddToFaction(g_releaseFollowHelperFaction, 0);
					if (!entry.hadHelperFaction) {
						entry.addedHelperFaction = true;
					}
					changedFaction = true;
				}
			}

			if (g_dialogueHelperFaction) {
				if (!entry.hadDialogueHelperFaction && !entry.addedDialogueHelperFaction) {
					entry.hadDialogueHelperFaction = actor->IsInFaction(g_dialogueHelperFaction);
				}
				if (!actor->IsInFaction(g_dialogueHelperFaction)) {
					actor->AddToFaction(g_dialogueHelperFaction, 0);
					if (!entry.hadDialogueHelperFaction) {
						entry.addedDialogueHelperFaction = true;
					}
					changedFaction = true;
				}
			}

			auto* avo = actor->AsActorValueOwner();
			if (avo) {
				const float currentAggression = avo->GetActorValue(RE::ActorValue::kAggression);
				if (!entry.hasOriginalAggression && currentAggression > 0.0f) {
					entry.originalAggression = currentAggression;
					entry.hasOriginalAggression = true;
				}
				if (currentAggression > 0.0f) {
					avo->SetActorValue(RE::ActorValue::kAggression, 0.0f);
					changedAggression = true;
				}
			}

			if (auto* player = Player()) {
				(void)SuppressReleaseFollowTargetingToPlayer(actor, player, reason ? reason : "release_grace_stabilize");
			}

			if (actor->IsWeaponDrawn()) {
				actor->DrawWeaponMagicHands(false);
			}

			if (changedFaction || changedAggression) {
				if (auto* process = RE::ProcessLists::GetSingleton()) {
					process->ClearCachedFactionFightReactions();
				}
				actor->EvaluatePackage(false, true);
				actor->EvaluatePackage(true, true);
				actor->UpdateCombat();
				if (auto* player = Player()) {
					player->UpdateCombat();
				}
				spdlog::info(
					"[TFD][FactionManager][W48] grace stabilized actor={:08X} helperAdded={} pacifyAdded={} aggressionHeld={} reason={}",
					actor->GetFormID(),
					entry.addedHelperFaction ? 1 : 0,
					entry.addedDialogueHelperFaction ? 1 : 0,
					entry.hasOriginalAggression ? 1 : 0,
					reason ? reason : "unknown");
			}
		}

		static void RemoveReleaseFollowGraceFromActor(RE::Actor* actor, const char* reason)
		{
			if (!actor) {
				return;
			}
			ResolveReleaseFollowHelperFaction();

			ReleaseFollowGraceEntry entry{};
			bool hadEntry = false;
			if (auto it = g_releaseFollowGraceEntries.find(actor->GetFormID()); it != g_releaseFollowGraceEntries.end()) {
				entry = it->second;
				hadEntry = true;
			}

			const bool keepPacifyForPermanentTeammate = ActorHasPermanentTeammateFaction(actor);
			const bool keepPacifyForActiveDialogue = ActorHasActiveDialoguePhaseFaction(actor);

			if (g_releaseFollowHelperFaction && actor->IsInFaction(g_releaseFollowHelperFaction)) {
				if (!hadEntry || entry.addedHelperFaction || !entry.hadHelperFaction) {
					actor->RemoveFromFaction(g_releaseFollowHelperFaction);
				}
			}
			if (g_dialogueHelperFaction && actor->IsInFaction(g_dialogueHelperFaction)) {
				if (keepPacifyForPermanentTeammate || keepPacifyForActiveDialogue) {
					spdlog::info(
						"[TFD][FactionManager] grace keep pacify actor={:08X} reason={} guard={}",
						actor->GetFormID(),
						reason ? reason : "unknown",
						keepPacifyForPermanentTeammate ? "permanent_teammate" : "active_dialogue_phase");
				}
				else if (!hadEntry || entry.addedDialogueHelperFaction || !entry.hadDialogueHelperFaction) {
					actor->RemoveFromFaction(g_dialogueHelperFaction);
				}
			}

			if (hadEntry && entry.hasOriginalAggression) {
				if (auto* avo = actor->AsActorValueOwner()) {
					avo->SetActorValue(RE::ActorValue::kAggression, entry.originalAggression);
				}
			}

			g_releaseFollowGraceEntries.erase(actor->GetFormID());
			if (auto* process = RE::ProcessLists::GetSingleton()) {
				process->ClearCachedFactionFightReactions();
			}
			actor->EvaluatePackage(false, true);
			actor->EvaluatePackage(true, true);
			actor->UpdateCombat();
			if (auto* player = Player()) {
				player->UpdateCombat();
			}
			spdlog::info(
				"[TFD][FactionManager] grace removed actor={:08X} reason={} restoredAggression={}",
				actor->GetFormID(),
				reason ? reason : "unknown",
				(hadEntry && entry.hasOriginalAggression) ? 1 : 0);
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

			auto& entry = g_releaseFollowGraceEntries[actor->GetFormID()];
			entry.actor = actor->GetHandle();
			entry.expiresAt = Now() + std::chrono::milliseconds(static_cast<int>((std::max)(0.0, durationSeconds) * 1000.0));
			entry.source = reason ? reason : "unknown";

			StabilizeReleaseFollowGraceActor(actor, entry, reason ? reason : "grace_apply");

			spdlog::info("[TFD][FactionManager] grace applied actor={:08X} helperFaction={:08X} dialogueFaction={:08X} duration={:.2f} reason={} aggressionHeld={}",
				actor->GetFormID(),
				g_releaseFollowHelperFaction ? g_releaseFollowHelperFaction->GetFormID() : 0u,
				g_dialogueHelperFaction ? g_dialogueHelperFaction->GetFormID() : 0u,
				durationSeconds,
				reason ? reason : "unknown",
				entry.hasOriginalAggression ? 1 : 0);
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

	bool ApplyAggressorFactionContext(RE::Actor* aggressor)
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

	void ClearAggressorFactionContext()
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

	bool HasAggressorFactionContext()
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

	std::vector<RE::Actor*> CollectTruceActorsForSpeaker(RE::Actor* speaker)
	{
		Initialize();
		return CollectTruceActorsInternal(speaker);
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

	void ApplyReleaseFollowGraceToActorOnly(RE::Actor* actor, double durationSeconds, const char* reason)
	{
		Initialize();
		ApplyReleaseFollowGraceToActor(actor, durationSeconds, reason);
	}

	void RemoveReleaseFollowGraceFromActorOnly(RE::Actor* actor, const char* reason)
	{
		Initialize();
		RemoveReleaseFollowGraceFromActor(actor, reason);
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
		std::vector<RE::Actor*> actorsToClear{};
		std::vector<RE::FormID> staleIds{};
		const auto now = Now();
		for (auto& [formID, entry] : g_releaseFollowGraceEntries) {
			auto actorPtr = RE::Actor::LookupByHandle(entry.actor.native_handle());
			auto* actor = actorPtr.get();
			const bool expired = now >= entry.expiresAt;
			const bool invalid = !actor || actor->IsDead() || actor->IsDisabled();
			if (!expired && !invalid) {
				StabilizeReleaseFollowGraceActor(actor, entry, entry.source.c_str());
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

	void InstallDefeatedEnemyQueryHooks(const DefeatedEnemyQueryHooks& hooks)
	{
		g_defeatedEnemyQueryHooks = hooks;
	}

	void InstallDefeatedEnemyStateHooks(const DefeatedEnemyStateHooks& hooks)
	{
		g_defeatedEnemyStateHooks = hooks;
	}

	bool IsDefeatedEnemyCandidate(RE::Actor* actor)
	{
		Initialize();
		return IsDefeatedEnemyCandidateInternal(actor);
	}

	void SuppressDefeatedEnemyReentry(RE::Actor* actor, double seconds, const char* reason)
	{
		Initialize();
		if (!actor) {
			return;
		}
		const auto secs = (std::max)(0.5, seconds);
		g_defeatedReentrySuppress[actor->GetFormID()] = Now() + std::chrono::milliseconds(static_cast<int>(secs * 1000.0));
		spdlog::info("[TFD][FactionManager] defeated reentry suppress actor={:08X} seconds={:.1f} reason={}",
			actor->GetFormID(),
			secs,
			reason ? reason : "unknown");
	}

	void ApplyDefeatedEnemyPassiveOverride(RE::Actor* actor, float& savedAggression, bool& aggressionOverridden)
	{
		Initialize();
		ApplyDefeatedEnemyPassiveOverrideInternal(actor, savedAggression, aggressionOverridden);
	}

	void RestoreDefeatedEnemyPassiveOverride(RE::Actor* actor, float& savedAggression, bool& aggressionOverridden)
	{
		Initialize();
		RestoreDefeatedEnemyPassiveOverrideInternal(actor, savedAggression, aggressionOverridden);
	}


	static bool QueryDefeatedEnemyState(RE::Actor* actor, std::uint8_t& lockKindValue, bool& defeatedManaged, std::chrono::steady_clock::time_point& deadline)
	{
		if (!g_defeatedEnemyStateHooks.tryGetState) {
			return false;
		}
		return g_defeatedEnemyStateHooks.tryGetState(actor, &lockKindValue, &defeatedManaged, &deadline);
	}

	bool IsDefeatedEnemyKnocked(RE::Actor* actor)
	{
		Initialize();
		std::uint8_t lockKindValue = 0;
		bool defeatedManaged = false;
		std::chrono::steady_clock::time_point deadline{};
		return QueryDefeatedEnemyState(actor, lockKindValue, defeatedManaged, deadline) && IsDefeatedEnemyKnockedInternal(actor, lockKindValue, defeatedManaged);
	}
	bool IsDefeatedEnemyKnocked(RE::Actor* actor, std::uint8_t lockKindValue, bool defeatedManaged)
	{
		Initialize();
		return IsDefeatedEnemyKnockedInternal(actor, lockKindValue, defeatedManaged);
	}


	bool IsDialogueCapableDefeatedEnemy(RE::Actor* actor)
	{
		Initialize();
		std::uint8_t lockKindValue = 0;
		bool defeatedManaged = false;
		std::chrono::steady_clock::time_point deadline{};
		return QueryDefeatedEnemyState(actor, lockKindValue, defeatedManaged, deadline) && IsDialogueCapableDefeatedEnemyInternal(actor, lockKindValue, defeatedManaged);
	}
	bool IsDialogueCapableDefeatedEnemy(RE::Actor* actor, std::uint8_t lockKindValue, bool defeatedManaged)
	{
		Initialize();
		return IsDialogueCapableDefeatedEnemyInternal(actor, lockKindValue, defeatedManaged);
	}


	bool IsCreatureDefeatedEnemy(RE::Actor* actor)
	{
		Initialize();
		std::uint8_t lockKindValue = 0;
		bool defeatedManaged = false;
		std::chrono::steady_clock::time_point deadline{};
		return QueryDefeatedEnemyState(actor, lockKindValue, defeatedManaged, deadline) && IsCreatureDefeatedEnemyInternal(actor, lockKindValue, defeatedManaged);
	}
	bool IsCreatureDefeatedEnemy(RE::Actor* actor, std::uint8_t lockKindValue, bool defeatedManaged)
	{
		Initialize();
		return IsCreatureDefeatedEnemyInternal(actor, lockKindValue, defeatedManaged);
	}


	double GetDefeatedEnemyRemainingSeconds(RE::Actor* actor)
	{
		Initialize();
		std::uint8_t lockKindValue = 0;
		bool defeatedManaged = false;
		std::chrono::steady_clock::time_point deadline{};
		if (!QueryDefeatedEnemyState(actor, lockKindValue, defeatedManaged, deadline)) {
			return 0.0;
		}
		return GetDefeatedEnemyRemainingSecondsInternal(defeatedManaged, deadline);
	}
	double GetDefeatedEnemyRemainingSeconds(RE::Actor* actor, bool defeatedManaged, std::chrono::steady_clock::time_point deadline)
	{
		Initialize();
		(void) actor;
		return GetDefeatedEnemyRemainingSecondsInternal(defeatedManaged, deadline);
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

	void ClearDefeatedEnemyState(RE::Actor* actor, int& aliasSlot, bool& factionApplied, bool& defeatedManaged, bool& defeatedAutoDeathIssued, bool& defeatedFatalDamageApplied, std::chrono::steady_clock::time_point& defeatedDeadline, float& savedAggression, bool& aggressionOverridden, const char* reason)
	{
		Initialize();
		ClearDefeatedEnemyStateInternal(actor, aliasSlot, factionApplied, defeatedManaged, defeatedAutoDeathIssued, defeatedFatalDamageApplied, defeatedDeadline, savedAggression, aggressionOverridden, reason);
	}

	void ClearAllDefeatedEnemyMirrors(const char* reason)
	{
		Initialize();
		ClearAllDefeatedEnemyMirrorsInternal(reason);
	}

}
