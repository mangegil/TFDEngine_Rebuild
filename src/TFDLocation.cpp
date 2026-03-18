#include "TFDLocation.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string>
#include <string_view>
#include <unordered_map>
#include <limits>
#include <vector>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include "EditorIdCache.h"

namespace TFD::Location
{
	namespace
	{
		// Vanilla BGSLocationRefType formIDs (Skyrim.esm)
		// CaptiveMarker = 000130FA
		static constexpr std::uint32_t kCaptiveMarkerRefType = 0x000130FA;
		static constexpr std::uint32_t kOutsideEntranceRefType = 0x000130FB;
		static constexpr std::uint32_t kInsideEntranceRefType = 0x000130FC;

		// Common ref type EditorID
		static constexpr const char* kBossRefTypeEditorId = "Boss";
		static constexpr const char* kBossContainerRefTypeEditorId = "BossContainer";
		static constexpr const char* kLocationCenterRefTypeEditorId = "LocationCenterMarker";

		RE::BGSLocationRefType* g_captiveType = nullptr;
		RE::BGSLocationRefType* g_insideType = nullptr;
		RE::BGSLocationRefType* g_outsideType = nullptr;

		RE::BGSLocationRefType* g_bossType = nullptr;
		RE::BGSLocationRefType* g_bossContainerType = nullptr;
		RE::BGSLocationRefType* g_centerType = nullptr;

		RE::ObjectRefHandle g_cachedMarker{};

		std::unordered_map<RE::FormID, SafeCheckpoint> g_safeCheckpointByLocation;
		std::unordered_map<RE::FormID, ApprovedBed> g_approvedBedByLocation;
		std::uint32_t g_checkpointVisitSerial = 0;
		std::uint32_t g_bedUseSerial = 0;

		struct RescueCacheHeader
		{
			std::uint32_t checkpointCount{ 0 };
			std::uint32_t bedCount{ 0 };
			std::uint32_t checkpointVisitSerial{ 0 };
			std::uint32_t bedUseSerial{ 0 };
		};

		static constexpr std::uint32_t kRescueCacheVersion = 1;
		static constexpr float kCheckpointBedScanRadius = 8192.0f;

		using Clock = std::chrono::steady_clock;

		struct RescueResolveMemo
		{
			RE::FormID startLocId{ 0 };
			RE::FormID resultLocId{ 0 };
			Clock::time_point expires{};
		};

		RescueResolveMemo g_rescueResolveMemo{};
		Clock::time_point g_lastResolveNullLog{};
		Clock::time_point g_lastResolveMissLog{};
		Clock::time_point g_lastResolveSuccessLog{};
		Clock::time_point g_lastChildResolveSuccessLog{};
		Clock::time_point g_lastResolveFromRefLog{};
		RE::FormID g_lastResolveSuccessStartId = 0;
		RE::FormID g_lastResolveSuccessResultId = 0;
		RE::FormID g_lastChildResolveParentId = 0;
		RE::FormID g_lastChildResolveChildId = 0;
		RE::FormID g_lastResolveFromRefStartId = 0;

		static bool ShouldEmitThrottledLog(Clock::time_point& last, std::chrono::milliseconds window)
		{
			const auto now = Clock::now();
			if (last.time_since_epoch().count() == 0 || now - last >= window) {
				last = now;
				return true;
			}
			return false;
		}

		static bool ShouldEmitDistinctResolveLog(
			Clock::time_point& last,
			RE::FormID& lastStartId,
			RE::FormID& lastResultId,
			RE::FormID startId,
			RE::FormID resultId,
			std::chrono::milliseconds window)
		{
			const auto now = Clock::now();
			if (lastStartId != startId || lastResultId != resultId ||
				last.time_since_epoch().count() == 0 || now - last >= window) {
				last = now;
				lastStartId = startId;
				lastResultId = resultId;
				return true;
			}
			return false;
		}

		static bool ShouldEmitDistinctSingleLog(
			Clock::time_point& last,
			RE::FormID& lastId,
			RE::FormID id,
			std::chrono::milliseconds window)
		{
			const auto now = Clock::now();
			if (lastId != id || last.time_since_epoch().count() == 0 || now - last >= window) {
				last = now;
				lastId = id;
				return true;
			}
			return false;
		}

		static RE::BGSLocation* ResolveMostRecentCachedSafeLocation()
		{
			const SafeCheckpoint* best = nullptr;

			for (const auto& [_, cp] : g_safeCheckpointByLocation) {
				if (cp.safeLocationId == 0) {
					continue;
				}
				if (!best || cp.visitSerial > best->visitSerial) {
					best = &cp;
				}
			}

			if (!best) {
				return nullptr;
			}

			auto* loc = RE::TESForm::LookupByID<RE::BGSLocation>(best->safeLocationId);
			if (!loc) {
				return nullptr;
			}

			return loc;
		}

		static const SafeCheckpoint* ResolveMostRecentSafeCheckpointEntry()
		{
			const SafeCheckpoint* best = nullptr;

			for (const auto& [_, cp] : g_safeCheckpointByLocation) {
				if (cp.safeLocationId == 0) {
					continue;
				}
				if (!best || cp.visitSerial > best->visitSerial) {
					best = &cp;
				}
			}

			return best;
		}

		static const ApprovedBed* ResolveMostRecentApprovedBedEntry()
		{
			const ApprovedBed* best = nullptr;

			for (const auto& [_, bed] : g_approvedBedByLocation) {
				if (bed.safeLocationId == 0 || bed.bedRefId == 0) {
					continue;
				}
				if (!best || bed.useSerial > best->useSerial) {
					best = &bed;
				}
			}

			return best;
		}


		static RE::PlayerCharacter* Player()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

		static bool IsPlayerInterior()
		{
			auto* p = Player();
			auto* cell = p ? p->GetParentCell() : nullptr;
			return cell ? cell->IsInteriorCell() : false;
		}

		static RE::BGSLocationRefType* ResolveRefType(std::uint32_t formId)
		{
			return RE::TESForm::LookupByID<RE::BGSLocationRefType>(formId);
		}

		static void EnsureRefTypes()
		{
			if (!g_captiveType) {
				g_captiveType = ResolveRefType(kCaptiveMarkerRefType);
			}
			if (!g_insideType) {
				g_insideType = ResolveRefType(kInsideEntranceRefType);
			}
			if (!g_outsideType) {
				g_outsideType = ResolveRefType(kOutsideEntranceRefType);
			}

			if (!g_bossType) {
				g_bossType = RE::TESForm::LookupByEditorID<RE::BGSLocationRefType>(kBossRefTypeEditorId);
			}
			if (!g_bossContainerType) {
				g_bossContainerType = RE::TESForm::LookupByEditorID<RE::BGSLocationRefType>(kBossContainerRefTypeEditorId);
			}
			if (!g_centerType) {
				g_centerType = RE::TESForm::LookupByEditorID<RE::BGSLocationRefType>(kLocationCenterRefTypeEditorId);
			}
		}

		static bool SameRefType(const RE::BGSLocationRefType* a, const RE::BGSLocationRefType* b)
		{
			if (!a || !b) {
				return false;
			}
			return a == b || a->GetFormID() == b->GetFormID();
		}

		static void AddUnique(std::vector<RE::BGSLocation*>& list, RE::BGSLocation* loc)
		{
			if (!loc) {
				return;
			}
			for (auto* it : list) {
				if (it == loc) {
					return;
				}
			}
			list.push_back(loc);
		}

		static void AddLocationChain(std::vector<RE::BGSLocation*>& list, RE::BGSLocation* start)
		{
			auto* cur = start;
			for (int i = 0; cur && i < 16; ++i) {
				AddUnique(list, cur);
				cur = cur->parentLoc;
			}
		}

		static RE::TESObjectREFR* FindFirstOfType(RE::BGSLocation* loc, RE::BGSLocationRefType* type, bool preferInterior)
		{
			if (!loc || !type) {
				return nullptr;
			}

			RE::TESObjectREFR* firstHit = nullptr;

			for (std::uint32_t i = 0; i < loc->specialRefs.size(); ++i) {
				const auto& sref = loc->specialRefs[i];
				if (!SameRefType(sref.type, type)) {
					continue;
				}

				auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(sref.refData.refID);
				if (!ref) {
					continue;
				}

				if (!firstHit) {
					firstHit = ref;
				}

				auto* cell = ref->GetParentCell();
				if (!cell) {
					continue;
				}

				const bool isInterior = cell->IsInteriorCell();
				if (isInterior == preferInterior) {
					return ref;
				}
			}

			return firstHit;
		}

		static RE::TESObjectREFR* ResolveMarkerFromLocation(RE::BGSLocation* loc, bool preferInterior)
		{
			if (auto* r = FindFirstOfType(loc, g_captiveType, preferInterior)) {
				return r;
			}

			if (preferInterior) {
				if (auto* r = FindFirstOfType(loc, g_insideType, true)) {
					return r;
				}
			}
			else {
				if (auto* r = FindFirstOfType(loc, g_outsideType, false)) {
					return r;
				}
			}

			if (auto* r = FindFirstOfType(loc, g_insideType, true)) {
				return r;
			}
			if (auto* r = FindFirstOfType(loc, g_outsideType, false)) {
				return r;
			}

			return nullptr;
		}

		static RE::TESObjectREFR* ResolveBossAnchorFromLocation(RE::BGSLocation* loc)
		{
			if (!loc) {
				return nullptr;
			}

			if (auto* r = FindFirstOfType(loc, g_bossContainerType, true)) {
				return r;
			}
			if (auto* r = FindFirstOfType(loc, g_bossType, true)) {
				return r;
			}

			if (auto* r = FindFirstOfType(loc, g_bossContainerType, false)) {
				return r;
			}
			if (auto* r = FindFirstOfType(loc, g_bossType, false)) {
				return r;
			}

			return nullptr;
		}

		static void DumpSpecialRefs(RE::BGSLocation* loc)
		{
			if (!loc) {
				return;
			}

			spdlog::info("[TFD][Location] specialRefs size={}", loc->specialRefs.size());
			for (std::uint32_t i = 0; i < loc->specialRefs.size(); ++i) {
				const auto& sref = loc->specialRefs[i];
				const std::uint32_t typeId = sref.type ? sref.type->GetFormID() : 0;
				const std::uint32_t refId = sref.refData.refID;
				spdlog::info("[TFD][Location] sref[{}] type={:08X} ref={:08X}", i, typeId, refId);
			}
		}

		static bool TryResolveFromList(const std::vector<RE::BGSLocation*>& list, bool preferInterior, RE::TESObjectREFR*& outMarker)
		{
			for (auto* loc : list) {
				if (!loc) {
					continue;
				}

				auto* marker = ResolveMarkerFromLocation(loc, preferInterior);
				if (!marker) {
					continue;
				}

				outMarker = marker;

				spdlog::info(
					"[TFD][Location] Resolve hit loc={:08X} editorId='{}' preferInterior={}",
					loc->GetFormID(),
					TFD::Util::GetEditorId(loc).c_str(),
					preferInterior ? "true" : "false");

				return true;
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

		static bool HasSpecialRefType(RE::BGSLocation* loc, RE::BGSLocationRefType* type)
		{
			if (!loc || !type) {
				return false;
			}

			for (std::uint32_t i = 0; i < loc->specialRefs.size(); ++i) {
				const auto& sref = loc->specialRefs[i];
				auto* cur = sref.type;
				if (!cur) {
					continue;
				}
				if (cur == type || cur->GetFormID() == type->GetFormID()) {
					return true;
				}
			}

			return false;
		}

		static bool LocationChainContains(RE::BGSLocation* child, RE::BGSLocation* ancestor)
		{
			if (!child || !ancestor) {
				return false;
			}

			auto* cur = child;
			for (int i = 0; cur && i < 16; ++i) {
				if (cur == ancestor) {
					return true;
				}
				cur = cur->parentLoc;
			}

			return false;
		}

		static int ScoreChildRescueLocation(RE::BGSLocation* loc, RE::BGSLocation* parentLoc)
		{
			if (!loc) {
				return -100000;
			}

			int score = 0;

			if (LocationHasKeywordByEditorID(loc, "LocTypeInn")) {
				score += 200;
			}
			if (LocationHasKeywordByEditorID(loc, "LocTypeDwelling")) {
				score += 100;
			}

			if (loc->parentLoc == parentLoc) {
				score += 50;
			}

			if (HasSpecialRefType(loc, g_centerType)) {
				score += 60;
			}
			if (HasSpecialRefType(loc, g_insideType)) {
				score += 40;
			}
			if (HasSpecialRefType(loc, g_outsideType)) {
				score += 10;
			}

			return score;
		}

		static RE::BGSLocation* ResolveChildRescueLocationFromParent(RE::BGSLocation* parentLoc)
		{
			if (!parentLoc) {
				return nullptr;
			}

			EnsureRefTypes();

			auto* data = RE::TESDataHandler::GetSingleton();
			if (!data) {
				return nullptr;
			}

			RE::BGSLocation* best = nullptr;
			int bestScore = -100000;

			for (auto* loc : data->GetFormArray<RE::BGSLocation>()) {
				if (!loc || loc == parentLoc) {
					continue;
				}

				if (!IsRescueCandidateLocation(loc)) {
					continue;
				}

				if (!LocationChainContains(loc, parentLoc)) {
					continue;
				}

				const int score = ScoreChildRescueLocation(loc, parentLoc);
				if (score > bestScore) {
					bestScore = score;
					best = loc;
				}
			}

			if (best) {
				spdlog::info(
					"[TFD][Location] Child rescue resolved parent={:08X} '{}' -> child={:08X} '{}' score={}",
					parentLoc->GetFormID(),
					TFD::Util::GetEditorId(parentLoc).c_str(),
					best->GetFormID(),
					TFD::Util::GetEditorId(best).c_str(),
					bestScore);
			}

			return best;
		}

		static RE::TESObjectREFR* ResolveSpecialRef(RE::BGSLocation* loc, RE::BGSLocationRefType* type, bool preferInterior)
		{
			if (!loc || !type) {
				return nullptr;
			}
			return FindFirstOfType(loc, type, preferInterior);
		}

		static RE::BGSLocation* ResolveParentLocationForCheckpoint(RE::BGSLocation* safeLoc)
		{
			if (!safeLoc) {
				return nullptr;
			}

			auto* cur = safeLoc->parentLoc;
			for (int i = 0; cur && i < 16; ++i) {
				if (LocationHasKeywordByEditorID(cur, "LocTypeHabitationHasInn") ||
					LocationHasKeywordByEditorID(cur, "LocTypeHabitation") ||
					LocationHasKeywordByEditorID(cur, "LocTypeTown")) {
					return cur;
				}
				cur = cur->parentLoc;
			}

			return safeLoc->parentLoc;
		}

		static bool ContainsNoCase(std::string_view haystack, std::string_view needle)
		{
			if (needle.empty() || haystack.size() < needle.size()) {
				return false;
			}

			auto lower = [](char c) {
				return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				};

			for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
				bool ok = true;
				for (std::size_t j = 0; j < needle.size(); ++j) {
					if (lower(haystack[i + j]) != lower(needle[j])) {
						ok = false;
						break;
					}
				}
				if (ok) {
					return true;
				}
			}
			return false;
		}

		static bool IsBedLikeBaseForCache(RE::TESBoundObject* base)
		{
			if (!base) {
				return false;
			}

			auto* furn = base->As<RE::TESFurniture>();
			if (!furn) {
				return false;
			}

			if (!furn->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSleep)) {
				return false;
			}

			const auto edid = TFD::Util::GetEditorId(base);
			const char* name = base->GetName() ? base->GetName() : "";

			if (ContainsNoCase(edid, "BedRoll") || ContainsNoCase(name, "Bed Roll")) {
				return false;
			}
			if (ContainsNoCase(edid, "Coffin") || ContainsNoCase(name, "Coffin")) {
				return false;
			}
			if (ContainsNoCase(edid, "Prison") || ContainsNoCase(name, "Prison")) {
				return false;
			}
			if (ContainsNoCase(edid, "Hay") || ContainsNoCase(name, "Hay")) {
				return false;
			}

			if (ContainsNoCase(edid, "Bed") || ContainsNoCase(name, "Bed")) {
				return true;
			}

			return true;
		}

		static RE::TESObjectREFR* ResolveBestCheckpointBed(RE::BGSLocation* safeLoc, RE::TESObjectREFR* preferredMarker, RE::TESObjectREFR* contextRef)
		{
			if (!safeLoc) {
				return nullptr;
			}

			auto* scanRef = preferredMarker ? preferredMarker : contextRef;
			auto* cell = scanRef ? scanRef->GetParentCell() : nullptr;
			if (!scanRef || !cell) {
				return nullptr;
			}

			const auto origin = scanRef->GetPosition();
			RE::TESObjectREFR* best = nullptr;
			int bestTier = std::numeric_limits<int>::min();
			int bestScore = std::numeric_limits<int>::min();
			double bestDistSq = std::numeric_limits<double>::max();
			bool bestHasAnyOwner = false;
			bool bestHasActorOwner = false;
			bool bestHasFactionOwner = false;

			cell->ForEachReferenceInRange(origin, kCheckpointBedScanRadius, [&](RE::TESObjectREFR* candidate) -> RE::BSContainer::ForEachResult {
				if (!candidate || candidate == scanRef) {
					return RE::BSContainer::ForEachResult::kContinue;
				}

				if (candidate->IsDisabled()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}

				auto* base = candidate->GetBaseObject();
				if (!IsBedLikeBaseForCache(base)) {
					return RE::BSContainer::ForEachResult::kContinue;
				}

				auto* candidateCell = candidate->GetParentCell();
				if (!candidateCell || candidateCell != cell) {
					return RE::BSContainer::ForEachResult::kContinue;
				}

				auto* ownerForm = candidate->GetOwner();
				const bool hasActorOwner = ownerForm && ownerForm->As<RE::TESNPC>();
				const bool hasFactionOwner = ownerForm && ownerForm->As<RE::TESFaction>();
				const bool hasAnyOwner = ownerForm != nullptr;
				const int tier = hasActorOwner ? 0 : (hasFactionOwner ? 2 : 1);  // faction/no-actor-owner first, actor-owned fallback

				int score = 0;
				auto* candidateLoc = GetLocationFromRef(candidate);
				if (candidateLoc == safeLoc) {
					score += 200;
				}
				else if (LocationChainContains(candidateLoc, safeLoc) || LocationChainContains(safeLoc, candidateLoc)) {
					score += 120;
				}
				else if (candidateLoc) {
					score -= 200;
				}

				if (preferredMarker && preferredMarker->GetParentCell() == candidateCell) {
					score += 300;
				}

				if (hasFactionOwner) {
					score += 180;
				}
				else if (!hasAnyOwner) {
					score += 100;
				}
				else if (hasActorOwner) {
					score -= 300;
				}

				const auto cp = candidate->GetPosition();
				const double dx = static_cast<double>(cp.x - origin.x);
				const double dy = static_cast<double>(cp.y - origin.y);
				const double dz = static_cast<double>(cp.z - origin.z);
				const double distSq = dx * dx + dy * dy + dz * dz;
				const int distPenalty = static_cast<int>(distSq / 512.0);
				score -= distPenalty;

				const auto candidateId = candidate->GetFormID();
				const auto bestId = best ? best->GetFormID() : 0;

				if (!best ||
					tier > bestTier ||
					(tier == bestTier && score > bestScore) ||
					(tier == bestTier && score == bestScore && distSq < bestDistSq) ||
					(tier == bestTier && score == bestScore && distSq == bestDistSq && candidateId < bestId)) {
					best = candidate;
					bestTier = tier;
					bestScore = score;
					bestDistSq = distSq;
					bestHasAnyOwner = hasAnyOwner;
					bestHasActorOwner = hasActorOwner;
					bestHasFactionOwner = hasFactionOwner;
				}

				return RE::BSContainer::ForEachResult::kContinue;
				});

			if (best) {
				spdlog::info(
					"[TFD][Location] Auto checkpoint bed hit loc={:08X} marker={:08X} bed={:08X} cell={:08X} owner={} actorOwner={} factionOwner={} tier={} score={} distSq={:.1f}",
					safeLoc->GetFormID(),
					preferredMarker ? preferredMarker->GetFormID() : 0,
					best->GetFormID(),
					best->GetParentCell() ? best->GetParentCell()->GetFormID() : 0,
					bestHasAnyOwner ? 1 : 0,
					bestHasActorOwner ? 1 : 0,
					bestHasFactionOwner ? 1 : 0,
					bestTier,
					bestScore,
					bestDistSq);
			}
			else {
				spdlog::info(
					"[TFD][Location] Auto checkpoint bed miss loc={:08X} marker={:08X} cell={:08X}",
					safeLoc->GetFormID(),
					preferredMarker ? preferredMarker->GetFormID() : 0,
					cell ? cell->GetFormID() : 0);
			}

			return best;
		}

		static bool DoRescanInternal(RE::Actor* aggressor, bool preferInterior)
		{
			EnsureRefTypes();

			auto* p = Player();
			if (!p) {
				spdlog::warn("[TFD][Location] Rescan: player null");
				return false;
			}

			auto* playerLoc = GetLocationFromRef(p);
			auto* aggressorLoc = GetLocationFromRef(aggressor);

			std::vector<RE::BGSLocation*> candidates;
			AddLocationChain(candidates, playerLoc);
			AddLocationChain(candidates, aggressorLoc);

			RE::BGSLocation* bossLoc = nullptr;
			for (auto* loc : candidates) {
				if (auto* bossAnchor = ResolveBossAnchorFromLocation(loc)) {
					bossLoc = GetLocationFromRef(bossAnchor);
					if (bossLoc) {
						spdlog::info(
							"[TFD][Location] Boss anchor -> {:08X}, bossLoc={:08X} editorId='{}'",
							bossAnchor->GetFormID(),
							bossLoc->GetFormID(),
							TFD::Util::GetEditorId(bossLoc).c_str());
					}
					else {
						spdlog::info("[TFD][Location] Boss anchor -> {:08X}, bossLoc=null", bossAnchor->GetFormID());
					}
					break;
				}
			}

			if (bossLoc) {
				AddLocationChain(candidates, bossLoc);
			}

			RE::TESObjectREFR* marker = nullptr;

			if (!TryResolveFromList(candidates, preferInterior, marker)) {
				TryResolveFromList(candidates, !preferInterior, marker);
			}

			if (!marker) {
				spdlog::warn(
					"[TFD][Location] Rescan: marker not found (preferInterior={})",
					preferInterior ? "true" : "false");

				DumpSpecialRefs(playerLoc ? playerLoc : aggressorLoc);

				g_cachedMarker = {};
				return false;
			}

			g_cachedMarker = marker->GetHandle();
			spdlog::info("[TFD][Location] Marker resolved -> {:08X}", marker->GetFormID());
			return true;
		}

		static void LogMoveContext(RE::TESObjectREFR* marker)
		{
			auto* p = Player();
			if (!p) {
				return;
			}

			const auto pp = p->GetPosition();
			spdlog::info("[TFD][Location] player pos: {:.1f} {:.1f} {:.1f}", pp.x, pp.y, pp.z);

			if (marker) {
				const auto mp = marker->GetPosition();
				auto* mc = marker->GetParentCell();
				spdlog::info(
					"[TFD][Location] marker {:08X} pos: {:.1f} {:.1f} {:.1f} interior={}",
					marker->GetFormID(),
					mp.x,
					mp.y,
					mp.z,
					(mc && mc->IsInteriorCell()) ? "true" : "false");
			}
			else {
				spdlog::info("[TFD][Location] marker: null");
			}
		}
	}

	void Initialize()
	{
		EnsureRefTypes();

		spdlog::info(
			"[TFD][Location] RefTypes: Captive={} Inside={} Outside={} BossType={} BossContainer={} CenterType={}",
			g_captiveType ? "OK" : "NULL",
			g_insideType ? "OK" : "NULL",
			g_outsideType ? "OK" : "NULL",
			g_bossType ? "OK" : "NULL",
			g_bossContainerType ? "OK" : "NULL",
			g_centerType ? "OK" : "NULL");
	}

	RE::BGSLocation* GetLocationFromRef(RE::TESObjectREFR* ref)
	{
		if (!ref) {
			return nullptr;
		}

		if (auto* currentLoc = ref->GetCurrentLocation()) {
			return currentLoc;
		}

		if (auto* cell = ref->GetParentCell()) {
			if (auto* cellLoc = cell->GetLocation()) {
				return cellLoc;
			}
		}

		return nullptr;
	}

	bool LocationHasKeywordByEditorID(RE::BGSLocation* loc, const char* editorID)
	{
		if (!loc) {
			return false;
		}
		auto* kw = LookupKeyword(editorID);
		return kw && loc->HasKeyword(kw);
	}

	bool IsRescueCandidateLocation(RE::BGSLocation* loc)
	{
		if (!loc) {
			return false;
		}
		return LocationHasKeywordByEditorID(loc, "LocTypeInn") ||
			LocationHasKeywordByEditorID(loc, "LocTypeDwelling");
	}

	RE::BGSLocation* ResolveRescueTargetLocation(RE::BGSLocation* startLoc)
	{
		if (!startLoc) {
			if (auto* cached = ResolveMostRecentCachedSafeLocation()) {
				if (ShouldEmitDistinctResolveLog(
					g_lastResolveSuccessLog,
					g_lastResolveSuccessStartId,
					g_lastResolveSuccessResultId,
					0,
					cached->GetFormID(),
					std::chrono::milliseconds(5000))) {
					spdlog::info(
						"[TFD][Location] Rescue target fallback from cache start=00000000 -> result={:08X} '{}'",
						cached->GetFormID(),
						TFD::Util::GetEditorId(cached).c_str());
				}
				return cached;
			}

			if (ShouldEmitThrottledLog(g_lastResolveNullLog, std::chrono::milliseconds(5000))) {
				spdlog::info("[TFD][Location] Rescue target not found: startLoc=null and no cached fallback");
			}
			return nullptr;
		}

		const auto now = Clock::now();
		const auto startId = startLoc->GetFormID();
		if (g_rescueResolveMemo.startLocId == startId &&
			g_rescueResolveMemo.expires.time_since_epoch().count() != 0 &&
			now < g_rescueResolveMemo.expires) {
			if (g_rescueResolveMemo.resultLocId != 0) {
				return RE::TESForm::LookupByID<RE::BGSLocation>(g_rescueResolveMemo.resultLocId);
			}
			return nullptr;
		}

		RE::BGSLocation* resolved = nullptr;

		// Pass 1: current / parent chain
		RE::BGSLocation* cur = startLoc;
		int depth = 0;

		while (cur && depth < 16) {
			if (IsRescueCandidateLocation(cur)) {
				resolved = cur;
				break;
			}

			// Pass 2: from habitation/town-style parent, scan child inn/dwelling
			if (LocationHasKeywordByEditorID(cur, "LocTypeHabitationHasInn") ||
				LocationHasKeywordByEditorID(cur, "LocTypeHabitation") ||
				LocationHasKeywordByEditorID(cur, "LocTypeTown")) {
				if (auto* child = ResolveChildRescueLocationFromParent(cur)) {
					resolved = child;
					break;
				}
			}

			cur = cur->parentLoc;
			++depth;
		}

		g_rescueResolveMemo.startLocId = startId;
		g_rescueResolveMemo.resultLocId = resolved ? resolved->GetFormID() : 0;
		g_rescueResolveMemo.expires = now + std::chrono::milliseconds(resolved ? 5000 : 2000);

		if (resolved) {
			if (ShouldEmitDistinctResolveLog(
				g_lastResolveSuccessLog,
				g_lastResolveSuccessStartId,
				g_lastResolveSuccessResultId,
				startId,
				resolved->GetFormID(),
				std::chrono::milliseconds(5000))) {
				spdlog::info(
					"[TFD][Location] Rescue target resolved start={:08X} '{}' -> result={:08X} '{}'",
					startId,
					TFD::Util::GetEditorId(startLoc).c_str(),
					resolved->GetFormID(),
					TFD::Util::GetEditorId(resolved).c_str());
			}
			return resolved;
		}

		if (ShouldEmitThrottledLog(g_lastResolveMissLog, std::chrono::milliseconds(5000))) {
			spdlog::info("[TFD][Location] Rescue target not found in parent chain or child scan");
		}
		return nullptr;
	}

	RE::BGSLocation* ResolveRescueTargetLocationFromRef(RE::TESObjectREFR* ref)
	{
		auto* startLoc = GetLocationFromRef(ref);
		const auto startId = startLoc ? startLoc->GetFormID() : 0;

		if (ShouldEmitDistinctSingleLog(
			g_lastResolveFromRefLog,
			g_lastResolveFromRefStartId,
			startId,
			std::chrono::milliseconds(5000))) {
			spdlog::info(
				"[TFD][Location] ResolveRescueTargetLocationFromRef startLoc={:08X} editorId='{}'",
				startId,
				startLoc ? TFD::Util::GetEditorId(startLoc).c_str() : "");
		}

		return ResolveRescueTargetLocation(startLoc);
	}

	bool RememberSafeCheckpoint(RE::BGSLocation* safeLoc, RE::TESObjectREFR* contextRef, RE::TESObjectREFR* entryDoor)
	{
		if (!safeLoc) {
			return false;
		}

		if (!IsRescueCandidateLocation(safeLoc)) {
			return false;
		}

		EnsureRefTypes();

		const bool preferInterior = contextRef ?
			(contextRef->GetParentCell() ? contextRef->GetParentCell()->IsInteriorCell() : false) :
			true;

		auto* centerRef = ResolveSpecialRef(safeLoc, g_centerType, preferInterior);
		auto* insideRef = ResolveSpecialRef(safeLoc, g_insideType, true);

		SafeCheckpoint cp{};
		cp.safeLocationId = safeLoc->GetFormID();

		if (auto* parentLoc = ResolveParentLocationForCheckpoint(safeLoc)) {
			cp.parentLocationId = parentLoc->GetFormID();
		}

		cp.centerMarkerRefId = centerRef ? centerRef->GetFormID() : 0;
		cp.insideEntranceRefId = insideRef ? insideRef->GetFormID() : 0;
		cp.entryDoorRefId = entryDoor ? entryDoor->GetFormID() : 0;
		cp.cellId = contextRef && contextRef->GetParentCell() ? contextRef->GetParentCell()->GetFormID() : 0;
		cp.isInterior = contextRef && contextRef->GetParentCell() ? contextRef->GetParentCell()->IsInteriorCell() : false;
		cp.visitSerial = ++g_checkpointVisitSerial;

		g_safeCheckpointByLocation[cp.safeLocationId] = cp;

		spdlog::info(
			"[TFD][Location] RememberSafeCheckpoint loc={:08X} parent={:08X} center={:08X} inside={:08X} door={:08X} cell={:08X} serial={}",
			cp.safeLocationId,
			cp.parentLocationId,
			cp.centerMarkerRefId,
			cp.insideEntranceRefId,
			cp.entryDoorRefId,
			cp.cellId,
			cp.visitSerial);

		auto* preferredMarker = insideRef ? insideRef : (centerRef ? centerRef : contextRef);
		if (auto* bestBed = ResolveBestCheckpointBed(safeLoc, preferredMarker, contextRef)) {
			RememberApprovedBed(safeLoc, bestBed);
		}

		return true;
	}

	bool RememberSafeCheckpointFromRef(RE::TESObjectREFR* ref, RE::TESObjectREFR* entryDoor)
	{
		auto* safeLoc = ResolveRescueTargetLocationFromRef(ref);
		if (!safeLoc) {
			return false;
		}
		return RememberSafeCheckpoint(safeLoc, ref, entryDoor);
	}

	bool RememberApprovedBed(RE::BGSLocation* safeLoc, RE::TESObjectREFR* bedRef)
	{
		if (!safeLoc || !bedRef) {
			return false;
		}

		auto* base = bedRef->GetBaseObject();
		if (!IsBedLikeBaseForCache(base)) {
			return false;
		}

		ApprovedBed bed{};
		bed.safeLocationId = safeLoc->GetFormID();
		bed.bedRefId = bedRef->GetFormID();
		bed.cellId = bedRef->GetParentCell() ? bedRef->GetParentCell()->GetFormID() : 0;

		if (const auto it = g_approvedBedByLocation.find(bed.safeLocationId); it != g_approvedBedByLocation.end()) {
			if (it->second.bedRefId == bed.bedRefId && it->second.cellId == bed.cellId) {
				return true;
			}
		}

		bed.useSerial = ++g_bedUseSerial;

		g_approvedBedByLocation[bed.safeLocationId] = bed;

		spdlog::info(
			"[TFD][Location] RememberApprovedBed loc={:08X} bed={:08X} cell={:08X} serial={}",
			bed.safeLocationId,
			bed.bedRefId,
			bed.cellId,
			bed.useSerial);

		return true;
	}

	bool RememberApprovedBedFromRefs(RE::TESObjectREFR* safeContextRef, RE::TESObjectREFR* bedRef)
	{
		auto* safeLoc = ResolveRescueTargetLocationFromRef(safeContextRef);
		if (!safeLoc) {
			return false;
		}
		return RememberApprovedBed(safeLoc, bedRef);
	}

	bool RefreshPlayerInteriorSafeCheckpoint()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return false;
		}

		auto* cell = player->GetParentCell();
		if (!cell || !cell->IsInteriorCell()) {
			return false;
		}

		auto* currentLoc = GetLocationFromRef(player);
		if (!currentLoc) {
			return false;
		}

		RE::BGSLocation* safeLoc = currentLoc;
		if (!IsRescueCandidateLocation(safeLoc)) {
			safeLoc = ResolveRescueTargetLocation(currentLoc);
		}

		if (!safeLoc || !IsRescueCandidateLocation(safeLoc)) {
			return false;
		}

		if (safeLoc != currentLoc && !LocationChainContains(safeLoc, currentLoc) && !LocationChainContains(currentLoc, safeLoc)) {
			return false;
		}

		const bool ok = RememberSafeCheckpoint(safeLoc, player, nullptr);
		spdlog::info(
			"[TFD][Location] RefreshPlayerInteriorSafeCheckpoint cell={:08X} currentLoc={:08X} safeLoc={:08X} ok={}",
			cell->GetFormID(),
			currentLoc ? currentLoc->GetFormID() : 0,
			safeLoc ? safeLoc->GetFormID() : 0,
			ok ? "true" : "false");

		return ok;
	}

	bool GetLastSafeCheckpointForLocation(RE::BGSLocation* loc, SafeCheckpoint& outCp)
	{
		if (!loc) {
			return false;
		}

		const auto it = g_safeCheckpointByLocation.find(loc->GetFormID());
		if (it == g_safeCheckpointByLocation.end()) {
			return false;
		}

		outCp = it->second;
		return true;
	}

	bool GetBestApprovedBedForLocation(RE::BGSLocation* loc, ApprovedBed& outBed)
	{
		if (!loc) {
			return false;
		}

		const auto it = g_approvedBedByLocation.find(loc->GetFormID());
		if (it == g_approvedBedByLocation.end()) {
			return false;
		}

		outBed = it->second;
		return true;
	}

	RE::BGSLocation* GetMostRecentCachedSafeLocation()
	{
		return ResolveMostRecentCachedSafeLocation();
	}

	RE::TESObjectREFR* ResolveMostRecentCachedRescueDestination(bool preferInterior)
	{
		if (const auto* bed = ResolveMostRecentApprovedBedEntry()) {
			if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(bed->bedRefId)) {
				return ref;
			}
		}

		if (const auto* cp = ResolveMostRecentSafeCheckpointEntry()) {
			if (cp->insideEntranceRefId != 0) {
				if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(cp->insideEntranceRefId)) {
					return ref;
				}
			}
			if (cp->centerMarkerRefId != 0) {
				if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(cp->centerMarkerRefId)) {
					return ref;
				}
			}
			if (cp->entryDoorRefId != 0) {
				if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(cp->entryDoorRefId)) {
					return ref;
				}
			}
			if (auto* safeLoc = RE::TESForm::LookupByID<RE::BGSLocation>(cp->safeLocationId)) {
				if (auto* ref = ResolvePreferredRescueDestination(safeLoc, preferInterior)) {
					return ref;
				}
			}
		}

		if (auto* safeLoc = ResolveMostRecentCachedSafeLocation()) {
			return ResolvePreferredRescueDestination(safeLoc, preferInterior);
		}

		return nullptr;
	}

	RE::TESObjectREFR* ResolvePreferredRescueDestination(RE::BGSLocation* safeLoc, bool preferInterior)
	{
		if (!safeLoc) {
			return nullptr;
		}

		EnsureRefTypes();

		if (auto* ref = ResolveSpecialRef(safeLoc, g_insideType, true)) {
			return ref;
		}

		if (auto* ref = ResolveSpecialRef(safeLoc, g_centerType, preferInterior)) {
			return ref;
		}

		if (auto* ref = ResolveSpecialRef(safeLoc, g_outsideType, false)) {
			return ref;
		}

		return nullptr;
	}


	void ClearRescueCache()
	{
		g_safeCheckpointByLocation.clear();
		g_approvedBedByLocation.clear();
		g_checkpointVisitSerial = 0;
		g_bedUseSerial = 0;
		g_rescueResolveMemo = {};
		spdlog::info("[TFD][Location] Rescue cache cleared");
	}

	bool SaveRescueCache(SKSE::SerializationInterface* intfc)
	{
		if (!intfc) {
			return false;
		}

		RescueCacheHeader header{};
		header.checkpointCount = static_cast<std::uint32_t>(g_safeCheckpointByLocation.size());
		header.bedCount = static_cast<std::uint32_t>(g_approvedBedByLocation.size());
		header.checkpointVisitSerial = g_checkpointVisitSerial;
		header.bedUseSerial = g_bedUseSerial;

		if (!intfc->WriteRecordData(&header, sizeof(header))) {
			spdlog::error("[TFD][Location] SaveRescueCache -> header write failed");
			return false;
		}

		for (const auto& [_, cp] : g_safeCheckpointByLocation) {
			if (!intfc->WriteRecordData(&cp, sizeof(cp))) {
				spdlog::error("[TFD][Location] SaveRescueCache -> checkpoint write failed loc={:08X}", cp.safeLocationId);
				return false;
			}
		}

		for (const auto& [_, bed] : g_approvedBedByLocation) {
			if (!intfc->WriteRecordData(&bed, sizeof(bed))) {
				spdlog::error("[TFD][Location] SaveRescueCache -> bed write failed loc={:08X}", bed.safeLocationId);
				return false;
			}
		}

		spdlog::info(
			"[TFD][Location] SaveRescueCache -> checkpoints={} beds={} cpSerial={} bedSerial={}",
			header.checkpointCount,
			header.bedCount,
			header.checkpointVisitSerial,
			header.bedUseSerial);

		return true;
	}

	bool LoadRescueCache(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length)
	{
		ClearRescueCache();

		if (!intfc) {
			spdlog::warn("[TFD][Location] LoadRescueCache -> interface null");
			return false;
		}

		if (version != kRescueCacheVersion) {
			spdlog::warn("[TFD][Location] LoadRescueCache -> unsupported version={} length={}", version, length);
			if (length > 0) {
				std::string skip(length, '\0');
				intfc->ReadRecordData(skip.data(), length);
			}
			return false;
		}

		if (length < sizeof(RescueCacheHeader)) {
			spdlog::warn("[TFD][Location] LoadRescueCache -> short record length={}", length);
			if (length > 0) {
				std::string skip(length, '\0');
				intfc->ReadRecordData(skip.data(), length);
			}
			return false;
		}

		RescueCacheHeader header{};
		if (!intfc->ReadRecordData(&header, static_cast<std::uint32_t>(sizeof(header)))) {
			spdlog::error("[TFD][Location] LoadRescueCache -> header read failed");
			return false;
		}

		std::uint32_t bytesRead = static_cast<std::uint32_t>(sizeof(header));

		for (std::uint32_t i = 0; i < header.checkpointCount; ++i) {
			SafeCheckpoint cp{};
			if (bytesRead + sizeof(cp) > length || !intfc->ReadRecordData(&cp, static_cast<std::uint32_t>(sizeof(cp)))) {
				spdlog::error("[TFD][Location] LoadRescueCache -> checkpoint read failed index={}", i);
				ClearRescueCache();
				return false;
			}
			bytesRead += static_cast<std::uint32_t>(sizeof(cp));
			if (cp.safeLocationId != 0) {
				g_safeCheckpointByLocation[cp.safeLocationId] = cp;
				g_checkpointVisitSerial = (std::max)(g_checkpointVisitSerial, cp.visitSerial);
			}
		}

		for (std::uint32_t i = 0; i < header.bedCount; ++i) {
			ApprovedBed bed{};
			if (bytesRead + sizeof(bed) > length || !intfc->ReadRecordData(&bed, static_cast<std::uint32_t>(sizeof(bed)))) {
				spdlog::error("[TFD][Location] LoadRescueCache -> bed read failed index={}", i);
				ClearRescueCache();
				return false;
			}
			bytesRead += static_cast<std::uint32_t>(sizeof(bed));
			if (bed.safeLocationId != 0) {
				g_approvedBedByLocation[bed.safeLocationId] = bed;
				g_bedUseSerial = (std::max)(g_bedUseSerial, bed.useSerial);
			}
		}

		g_checkpointVisitSerial = (std::max)(g_checkpointVisitSerial, header.checkpointVisitSerial);
		g_bedUseSerial = (std::max)(g_bedUseSerial, header.bedUseSerial);

		if (length > bytesRead) {
			std::string skip(length - bytesRead, '\0');
			intfc->ReadRecordData(skip.data(), static_cast<std::uint32_t>(skip.size()));
		}

		spdlog::info(
			"[TFD][Location] LoadRescueCache -> checkpoints={} beds={} cpSerial={} bedSerial={}",
			g_safeCheckpointByLocation.size(),
			g_approvedBedByLocation.size(),
			g_checkpointVisitSerial,
			g_bedUseSerial);

		return true;
	}

	void DumpRescueCacheToLog()
	{
		spdlog::info("[TFD][Location] ===== SAFE CHECKPOINT CACHE =====");
		for (const auto& it : g_safeCheckpointByLocation) {
			const auto& cp = it.second;
			spdlog::info(
				"[TFD][Location] CP loc={:08X} parent={:08X} center={:08X} inside={:08X} door={:08X} cell={:08X} serial={}",
				cp.safeLocationId,
				cp.parentLocationId,
				cp.centerMarkerRefId,
				cp.insideEntranceRefId,
				cp.entryDoorRefId,
				cp.cellId,
				cp.visitSerial);
		}

		spdlog::info("[TFD][Location] ===== APPROVED BED CACHE =====");
		for (const auto& it : g_approvedBedByLocation) {
			const auto& bed = it.second;
			spdlog::info(
				"[TFD][Location] BED loc={:08X} bed={:08X} cell={:08X} serial={}",
				bed.safeLocationId,
				bed.bedRefId,
				bed.cellId,
				bed.useSerial);
		}
	}

	bool RescanCaptiveMarker()
	{
		const bool ok = DoRescanInternal(nullptr, IsPlayerInterior());

		if (ok) {
			RE::DebugNotification("TFDEngine: Marker resolved");
		}
		else {
			RE::DebugNotification("TFDEngine: Marker NOT found");
		}
		return ok;
	}

	bool RescanCaptiveMarkerWithAggressor(RE::Actor* aggressor, bool preferInterior)
	{
		const bool ok = DoRescanInternal(aggressor, preferInterior);

		if (ok) {
			RE::DebugNotification("TFDEngine: Marker resolved");
		}
		else {
			RE::DebugNotification("TFDEngine: Marker NOT found");
		}
		return ok;
	}

	RE::TESObjectREFR* GetCachedCaptiveMarker()
	{
		auto ni = g_cachedMarker.get();
		return ni.get();
	}

	std::uint32_t GetCachedCaptiveMarkerFormID()
	{
		auto* m = GetCachedCaptiveMarker();
		return m ? m->GetFormID() : 0;
	}

	void DumpContextToLog()
	{
		auto* p = Player();
		auto* cell = p ? p->GetParentCell() : nullptr;
		auto* loc = GetLocationFromRef(p);

		spdlog::info("[TFD][Location] ===== CONTEXT =====");
		spdlog::info(
			"[TFD][Location] cell={:08X} interior={}",
			cell ? cell->GetFormID() : 0,
			cell ? (cell->IsInteriorCell() ? "true" : "false") : "null");
		spdlog::info(
			"[TFD][Location] location={:08X} editorId='{}' name='{}'",
			loc ? loc->GetFormID() : 0,
			loc ? TFD::Util::GetEditorId(loc).c_str() : "",
			loc ? loc->GetName() : "");

		auto* marker = GetCachedCaptiveMarker();
		spdlog::info("[TFD][Location] cachedMarker={:08X}", marker ? marker->GetFormID() : 0);

		DumpSpecialRefs(loc);
		DumpRescueCacheToLog();
	}

	bool TeleportToCaptiveMarker()
	{
		RE::DebugNotification("TFDEngine: Teleport debug queued");

		if (g_cachedMarker.native_handle() == 0) {
			DoRescanInternal(nullptr, true);
		}

		auto handle = g_cachedMarker;
		if (handle.native_handle() == 0) {
			spdlog::warn("[TFD][Location] Teleport: no cached handle");
			RE::DebugNotification("TFDEngine: No marker cached");
			return false;
		}

		auto* tasks = SKSE::GetTaskInterface();
		if (!tasks) {
			spdlog::warn("[TFD][Location] Teleport: TaskInterface missing");
			return false;
		}

		tasks->AddTask([handle]() {
			auto* p = Player();
			if (!p) {
				spdlog::warn("[TFD][Location] Teleport(AddTask): player null");
				return;
			}

			auto ni = handle.get();
			auto* marker = ni.get();
			if (!marker) {
				spdlog::warn("[TFD][Location] Teleport(AddTask): marker handle invalid");
				RE::DebugNotification("TFDEngine: Marker handle invalid");
				return;
			}

			LogMoveContext(marker);

			p->MoveTo(marker);

			RE::DebugNotification("TFDEngine: Teleport -> Marker");
			spdlog::info("[TFD][Location] Teleport executed -> {:08X}", marker->GetFormID());
			});

		return true;
	}
}