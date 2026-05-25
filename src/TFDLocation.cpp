#include "TFDLocation.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4505)
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <chrono>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <limits>
#include <vector>

#include <RE/Skyrim.h>
#include <RE/E/ExtraLock.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include "EditorIdCache.h"
#include "TFDCaptive.h"

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
		static constexpr const char* kContainerRefTypeEditorId = "Container";
		static constexpr const char* kLocationCenterRefTypeEditorId = "LocationCenterMarker";

		RE::BGSLocationRefType* g_captiveType = nullptr;
		RE::BGSLocationRefType* g_insideType = nullptr;
		RE::BGSLocationRefType* g_outsideType = nullptr;

		RE::BGSLocationRefType* g_bossType = nullptr;
		RE::BGSLocationRefType* g_bossContainerType = nullptr;
		RE::BGSLocationRefType* g_containerType = nullptr;
		RE::BGSLocationRefType* g_centerType = nullptr;

		RE::ObjectRefHandle g_cachedMarker{};

		RE::TESGlobal* g_bossContainerMarkerStateGlobal = nullptr;
		RE::TESGlobal* g_bossMarkerStateGlobal = nullptr;
		RE::TESGlobal* g_captiveMarkerStateGlobal = nullptr;
		RE::TESGlobal* g_containerMarkerStateGlobal = nullptr;
		RE::TESGlobal* g_escapeRouteStateGlobal = nullptr;
		RE::TESGlobal* g_rescueMarkerStateGlobal = nullptr;

		RE::TESGlobal* g_workMiningStateGlobal = nullptr;
		RE::TESGlobal* g_workCraftingStateGlobal = nullptr;
		RE::TESGlobal* g_workJobTypeGlobal = nullptr;
		RE::TESGlobal* g_workAssignmentStateGlobal = nullptr;
		RE::TESGlobal* g_workForgeStateGlobal = nullptr;
		RE::TESGlobal* g_workSmelterStateGlobal = nullptr;
		RE::TESGlobal* g_workTanningStateGlobal = nullptr;
		RE::TESGlobal* g_workSharpeningStateGlobal = nullptr;
		RE::TESGlobal* g_workWorkbenchStateGlobal = nullptr;
		RE::TESGlobal* g_workChoppingStateGlobal = nullptr;
		RE::TESGlobal* g_workCookingStateGlobal = nullptr;
		RE::TESGlobal* g_workAlchemyStateGlobal = nullptr;
		RE::TESGlobal* g_workEnchantingStateGlobal = nullptr;

		static RE::TESObjectREFR* ResolveSpecialRef(RE::BGSLocation* loc, RE::BGSLocationRefType* type, bool preferInterior);
		TFD::Location::CaptiveStorageDebugSnapshot g_lastCaptiveStorageDebugSnapshot{};

		RE::FormID g_lastAmbientCellId = 0;
		RE::FormID g_lastAmbientWorldspaceId = 0;
		RE::FormID g_lastAmbientLocationId = 0;
		bool g_lastAmbientInterior = false;
		bool g_lastAmbientWatcherPrimed = false;
		bool g_lastAmbientMarkerAvailable = false;

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

		static constexpr float kCaptiveWorkResourceScanRadius = 12000.0f;
		static constexpr float kCaptiveWorkFurnitureScanRadius = 12000.0f;
		static constexpr float kCaptiveWorkMineOccupiedRadius = 512.0f;
		static constexpr auto kCaptiveWorkResourceScanMinInterval = std::chrono::milliseconds(750);

		RE::FormID g_lastWorkResourceCellId = 0;
		int g_lastWorkMiningState = -1;
		int g_lastWorkCraftingState = -1;
		int g_activeWorkCraftingSubtypeRequest = 0;
		RE::FormID g_lastWorkMiningRefId = 0;
		RE::FormID g_lastWorkCraftingRefId = 0;
		std::array<RE::FormID, 10> g_lastWorkCraftingRefIds{};
		Clock::time_point g_lastWorkResourceScan{};
		std::unordered_set<RE::FormID> g_depletedWorkMiningRefs{};

		struct OccupiedWorkMineEntry
		{
			RE::FormID actorId{ 0 };
			RE::FormID cellId{ 0 };
			Clock::time_point expires{};
		};

		static constexpr auto kCaptiveWorkMineActivateOccupiedTTL = std::chrono::seconds(45);
		static constexpr float kCaptiveWorkMineActivatedActorMaxDistance = 768.0f;
		std::unordered_map<RE::FormID, OccupiedWorkMineEntry> g_occupiedWorkMiningRefs{};
		bool g_activateSinkRegistered = false;

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

		static void ResolveGlobal(RE::TESGlobal*& global, const char* editorId)
		{
			if (!global) {
				global = RE::TESForm::LookupByEditorID<RE::TESGlobal>(editorId);
			}
		}

		static void SetGlobalInt(RE::TESGlobal* global, int value)
		{
			if (global) {
				global->value = static_cast<float>(value);
			}
		}

		static int GetGlobalInt(RE::TESGlobal* global)
		{
			if (!global) {
				return 0;
			}
			return static_cast<int>(global->value);
		}

		static bool IsCaptiveWorkCraftingSubtype(int state)
		{
			return state >= 1 && state <= 9;
		}

		static int GetActiveCaptiveWorkCraftingSubtypeRequest()
		{
			// WorkJobType: 2 = shared Crafting channel.
			// WorkAssignmentState: 2 = Doing Work, 3 = Return Report.
			// During an active job, periodic resource refresh must preserve the selected
			// station subtype. Otherwise a generic tick with no requested state can
			// replace Chopping/Tanning/Tempering/Cooking/Alchemy/Enchant aliases with
			// the default Forge alias.
			const int jobType = GetGlobalInt(g_workJobTypeGlobal);
			const int assignment = GetGlobalInt(g_workAssignmentStateGlobal);
			const int currentCraftingState = GetGlobalInt(g_workCraftingStateGlobal);

			if (jobType != 2 || (assignment != 2 && assignment != 3)) {
				if (g_activeWorkCraftingSubtypeRequest != 0) {
					spdlog::info(
						"[TFD][Location][W07] active crafting subtype latch cleared job={} assignment={} oldLatch={}",
						jobType,
						assignment,
						g_activeWorkCraftingSubtypeRequest);
				}
				g_activeWorkCraftingSubtypeRequest = 0;
				return 0;
			}

			if (IsCaptiveWorkCraftingSubtype(g_activeWorkCraftingSubtypeRequest)) {
				return g_activeWorkCraftingSubtypeRequest;
			}

			if (IsCaptiveWorkCraftingSubtype(currentCraftingState)) {
				g_activeWorkCraftingSubtypeRequest = currentCraftingState;
				return currentCraftingState;
			}

			return 0;
		}

		static void ResolveMarkerGlobals()
		{
			ResolveGlobal(g_bossContainerMarkerStateGlobal, "TFDBossContainerMarkerState");
			ResolveGlobal(g_bossMarkerStateGlobal, "TFDBossMarkerState");
			ResolveGlobal(g_captiveMarkerStateGlobal, "TFDCaptiveMarkerState");
			ResolveGlobal(g_containerMarkerStateGlobal, "TFDContainerMarkerState");
			ResolveGlobal(g_escapeRouteStateGlobal, "TFDEscapeRouteState");
			ResolveGlobal(g_rescueMarkerStateGlobal, "TFDRescueMarkerState");
		}

		static void ResolveCaptiveWorkResourceGlobals()
		{
			ResolveGlobal(g_workMiningStateGlobal, "TFDMiningState");
			ResolveGlobal(g_workCraftingStateGlobal, "TFDCraftingState");
			ResolveGlobal(g_workJobTypeGlobal, "TFDWorkJobType");
			ResolveGlobal(g_workAssignmentStateGlobal, "TFDWorkAssignmentState");
			ResolveGlobal(g_workForgeStateGlobal, "TFDForgeState");
			ResolveGlobal(g_workSmelterStateGlobal, "TFDSmelterState");
			ResolveGlobal(g_workTanningStateGlobal, "TFDTanningState");
			ResolveGlobal(g_workSharpeningStateGlobal, "TFDSharpeningState");
			ResolveGlobal(g_workWorkbenchStateGlobal, "TFDWorkbenchState");
			ResolveGlobal(g_workChoppingStateGlobal, "TFDChoppingState");
			ResolveGlobal(g_workCookingStateGlobal, "TFDCookingState");
			ResolveGlobal(g_workAlchemyStateGlobal, "TFDAlchemyState");
			ResolveGlobal(g_workEnchantingStateGlobal, "TFDEnchantingState");
		}

		static void SetCaptiveWorkFurnitureGlobals(const std::array<bool, 10>& available)
		{
			SetGlobalInt(g_workForgeStateGlobal, available[1] ? 1 : 0);
			SetGlobalInt(g_workSmelterStateGlobal, available[2] ? 1 : 0);
			SetGlobalInt(g_workTanningStateGlobal, available[3] ? 1 : 0);
			SetGlobalInt(g_workSharpeningStateGlobal, available[4] ? 1 : 0);
			SetGlobalInt(g_workWorkbenchStateGlobal, available[5] ? 1 : 0);
			SetGlobalInt(g_workChoppingStateGlobal, available[6] ? 1 : 0);
			SetGlobalInt(g_workCookingStateGlobal, available[7] ? 1 : 0);
			SetGlobalInt(g_workAlchemyStateGlobal, available[8] ? 1 : 0);
			SetGlobalInt(g_workEnchantingStateGlobal, available[9] ? 1 : 0);
		}

		static void ClearCaptiveWorkFurnitureGlobals()
		{
			SetCaptiveWorkFurnitureGlobals({});
		}

		static int CountNonZero3(const std::array<std::uint32_t, 3>& ids)
		{
			int count = 0;
			for (auto id : ids) {
				if (id != 0) {
					++count;
				}
			}
			return count;
		}

		static int ComputeEscapeRouteState(RE::TESObjectREFR* marker)
		{
			if (!marker) {
				return 0;
			}

			auto* loc = GetLocationFromRef(marker);
			if (loc && g_insideType) {
				if (auto* insideRef = ResolveSpecialRef(loc, g_insideType, true)) {
					if (insideRef->GetParentCell() == marker->GetParentCell()) {
						return 1;
					}
				}
			}

			return 2;
		}

		static void RefreshMarkerGlobals()
		{
			ResolveMarkerGlobals();

			RE::TESObjectREFR* marker = nullptr;
			if (g_cachedMarker) {
				marker = g_cachedMarker.get().get();
			}

			const int captiveMarker = marker ? 1 : 0;
			const int escapeRoute = ComputeEscapeRouteState(marker);
			const int bossMarker = CountNonZero3(g_lastCaptiveStorageDebugSnapshot.bossActorFormIDs) > 0 ? 1 : 0;
			const int bossContainer = CountNonZero3(g_lastCaptiveStorageDebugSnapshot.bossContainerFormIDs) > 0 ? 1 : 0;
			const int containerMarker = CountNonZero3(g_lastCaptiveStorageDebugSnapshot.containerFormIDs) > 0 ? 1 : 0;
			const int rescueMarker = ResolveMostRecentCachedSafeLocation() ? 1 : 0;

			SetGlobalInt(g_bossContainerMarkerStateGlobal, bossContainer);
			SetGlobalInt(g_bossMarkerStateGlobal, bossMarker);
			SetGlobalInt(g_captiveMarkerStateGlobal, captiveMarker);
			SetGlobalInt(g_containerMarkerStateGlobal, containerMarker);
			SetGlobalInt(g_escapeRouteStateGlobal, escapeRoute);
			SetGlobalInt(g_rescueMarkerStateGlobal, rescueMarker);
		}


		static bool IsPlayerInterior()
		{
			auto* p = Player();
			auto* cell = p ? p->GetParentCell() : nullptr;
			return cell ? cell->IsInteriorCell() : false;
		}

		static bool ResolvePlayerAmbientContext(RE::FormID& outCellId, RE::FormID& outWorldspaceId, RE::FormID& outLocationId, bool& outInterior)
		{
			auto* player = Player();
			auto* cell = player ? player->GetParentCell() : nullptr;
			if (!player || !cell) {
				return false;
			}

			outCellId = cell->GetFormID();
			outInterior = cell->IsInteriorCell();
			outWorldspaceId = 0;
			if (auto* ws = player->GetWorldspace()) {
				outWorldspaceId = ws->GetFormID();
			}
			outLocationId = 0;
			if (auto* loc = GetLocationFromRef(player)) {
				outLocationId = loc->GetFormID();
			}
			return true;
		}

		static bool AmbientContextChanged(RE::FormID cellId, RE::FormID worldspaceId, RE::FormID locationId, bool interior)
		{
			if (!g_lastAmbientWatcherPrimed) {
				return true;
			}
			return g_lastAmbientCellId != cellId ||
				g_lastAmbientWorldspaceId != worldspaceId ||
				g_lastAmbientLocationId != locationId ||
				g_lastAmbientInterior != interior;
		}

		static void RememberAmbientContext(RE::FormID cellId, RE::FormID worldspaceId, RE::FormID locationId, bool interior, bool markerAvailable)
		{
			g_lastAmbientCellId = cellId;
			g_lastAmbientWorldspaceId = worldspaceId;
			g_lastAmbientLocationId = locationId;
			g_lastAmbientInterior = interior;
			g_lastAmbientWatcherPrimed = true;
			g_lastAmbientMarkerAvailable = markerAvailable;
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
			if (!g_containerType) {
				g_containerType = RE::TESForm::LookupByEditorID<RE::BGSLocationRefType>(kContainerRefTypeEditorId);
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

		static bool IsUsableCampfireCookingStation(std::string_view edid)
		{
			// W19: Do not treat UC_* campfire activators as captive-work cooking stations.
			// Runtime testing showed UC_Campfire01Burning can be selected as the marker
			// target while normal cookpot COBJ recipes such as BYOH mudcrab food do not
			// appear in the menu the player opens. Captive Work should only assign cooking
			// when the nearby reference is an actual TESFurniture cooking station, such as
			// CookingPot/CookingSpit/Cookpot. This prevents valid recipes from being paired
			// with a nonstandard activator/campfire menu and creating stuck assignments.
			(void)edid;
			return false;
		}

		static int CaptiveWorkMiningStatePriority(int state)
		{
			switch (state) {
			case 1: return 10;   // Iron: safest fallback
			case 2: return 20;   // Corundum
			case 3: return 30;   // Silver
			case 4: return 40;   // Gold
			case 5: return 50;   // Quicksilver
			case 6: return 60;   // Moonstone
			case 7: return 70;   // Malachite
			case 8: return 80;   // Orichalcum
			default: return 100000;
			}
		}

		static int CaptiveWorkMiningStateFromEditorID(std::string_view edid)
		{
			if (edid.empty()) {
				return 0;
			}

			if (ContainsNoCase(edid, "MineOreIron")) {
				return 1;
			}
			if (ContainsNoCase(edid, "MineOreCorundum")) {
				return 2;
			}
			if (ContainsNoCase(edid, "MineOreSilver")) {
				return 3;
			}
			if (ContainsNoCase(edid, "MineOreGold")) {
				return 4;
			}
			if (ContainsNoCase(edid, "MineOreQuicksilver")) {
				return 5;
			}
			if (ContainsNoCase(edid, "MineOreMoonstone")) {
				return 6;
			}
			if (ContainsNoCase(edid, "MineOreMalachite")) {
				return 7;
			}
			if (ContainsNoCase(edid, "MineOreOrichalcum")) {
				return 8;
			}
			// Ebony and Stalhrim are intentionally not enabled for the first Work assignment pass.
			// They can be detected later as explicit high-tier work, but default Captive Work
			// must stay low-tier and level-1 friendly.

			return 0;
		}

		static int CaptiveWorkCraftingStatePriority(int state)
		{
			switch (state) {
			case 1: return 10; // Forge / anvil: Iron Dagger style task
			case 2: return 20; // Smelter: Iron Ore -> Iron Ingot
			case 3: return 30; // Tanning rack: Leather -> strips
			case 4: return 40; // Grindstone: low-tier weapon temper
			case 5: return 50; // Armor workbench: low-tier armor temper
			case 6: return 60; // Chopping block: Firewood
			case 7: return 70; // Cooking pot: food work, backend pending
			case 8: return 80; // Alchemy lab: potion work, backend pending
			case 9: return 90; // Enchanting table: enchant work, backend pending
			default: return 100000;
			}
		}

		static int CaptiveWorkCraftingStateFromEditorID(std::string_view edid)
		{
			if (edid.empty()) {
				return 0;
			}

			if (ContainsNoCase(edid, "SkyForge") ||
				ContainsNoCase(edid, "BlacksmithForge") ||
				ContainsNoCase(edid, "SmithingForge") ||
				ContainsNoCase(edid, "CraftingForge") ||
				ContainsNoCase(edid, "CraftingSmithingForge") ||
				ContainsNoCase(edid, "Anvil")) {
				return 1;
			}

			if (ContainsNoCase(edid, "CraftingSmelter") || ContainsNoCase(edid, "Smelter")) {
				return 2;
			}

			if (ContainsNoCase(edid, "CraftingTanningRack") ||
				ContainsNoCase(edid, "TanningRack") ||
				ContainsNoCase(edid, "Tanning") ||
				ContainsNoCase(edid, "LeatherRack")) {
				return 3;
			}

			if (ContainsNoCase(edid, "SharpeningWheel") ||
				ContainsNoCase(edid, "Grindstone") ||
				ContainsNoCase(edid, "GrindStone") ||
				ContainsNoCase(edid, "WeaponRackSharpen")) {
				return 4;
			}

			if (ContainsNoCase(edid, "ChoppingBlock") ||
				ContainsNoCase(edid, "WoodChoppingBlock")) {
				return 6;
			}

			if (IsUsableCampfireCookingStation(edid)) {
				return 7;
			}

			if (ContainsNoCase(edid, "CookingPot") ||
				ContainsNoCase(edid, "CookingSpit") ||
				ContainsNoCase(edid, "Cookpot") ||
				ContainsNoCase(edid, "CookPot") ||
				ContainsNoCase(edid, "CookingStand") ||
				ContainsNoCase(edid, "CookingFire") ||
				ContainsNoCase(edid, "CookingPlace") ||
				ContainsNoCase(edid, "CookingKettle") ||
				ContainsNoCase(edid, "CraftingCook") ||
				ContainsNoCase(edid, "CraftingCooking")) {
				return 7;
			}

			// Check magic stations before the generic Workbench fallback. Otherwise
			// AlchemyWorkbench / EnchantingWorkbench can be misclassified as armor
			// workbenches and their dialogue globals never become available.
			if (ContainsNoCase(edid, "AlchemyWorkbench") ||
				ContainsNoCase(edid, "AlchemyTable") ||
				ContainsNoCase(edid, "AlchemyLab") ||
				ContainsNoCase(edid, "AlchemyStation") ||
				ContainsNoCase(edid, "CraftingAlchemy")) {
				return 8;
			}

			if (ContainsNoCase(edid, "EnchantingWorkbench") ||
				ContainsNoCase(edid, "EnchantingTable") ||
				ContainsNoCase(edid, "EnchantingStation") ||
				ContainsNoCase(edid, "ArcaneEnchanter") ||
				ContainsNoCase(edid, "CraftingEnchant")) {
				return 9;
			}

			if (ContainsNoCase(edid, "ArmorTable") ||
				ContainsNoCase(edid, "BlacksmithArmor") ||
				ContainsNoCase(edid, "SmithingArmor") ||
				ContainsNoCase(edid, "ArmorWorkbench") ||
				ContainsNoCase(edid, "CraftingSmithingArmor") ||
				ContainsNoCase(edid, "Workbench")) {
				return 5;
			}

			return 0;
		}

		static int ParseRequestedCaptiveWorkCraftingState(std::string_view reason)
		{
			if (reason.empty()) {
				return 0;
			}

			constexpr std::string_view key{ "work_request_crafting_state_" };
			const auto pos = reason.find(key);
			if (pos == std::string_view::npos) {
				return 0;
			}

			std::size_t index = pos + key.size();
			int value = 0;
			while (index < reason.size()) {
				const unsigned char ch = static_cast<unsigned char>(reason[index]);
				if (!std::isdigit(ch)) {
					break;
				}
				value = (value * 10) + static_cast<int>(reason[index] - '0');
				++index;
			}

			if (value < 1 || value > 9) {
				return 0;
			}
			return value;
		}

		static void PruneExpiredCaptiveWorkMineOccupancy(const char* reason)
		{
			const auto now = Clock::now();
			std::uint32_t removed = 0;

			for (auto it = g_occupiedWorkMiningRefs.begin(); it != g_occupiedWorkMiningRefs.end();) {
				if (it->second.expires.time_since_epoch().count() == 0 || it->second.expires <= now) {
					it = g_occupiedWorkMiningRefs.erase(it);
					++removed;
				}
				else {
					++it;
				}
			}

			if (removed > 0) {
				spdlog::info(
					"[TFD][Location] captive work occupied mine cache pruned removed={} reason={} remaining={}",
					removed,
					reason ? reason : "unknown",
					g_occupiedWorkMiningRefs.size());
			}
		}

		static bool IsActorStillOccupyingCachedMine(RE::TESObjectREFR* mineRef, const OccupiedWorkMineEntry& entry, RE::FormID& outActorId)
		{
			outActorId = 0;
			if (!mineRef || entry.actorId == 0) {
				return false;
			}

			auto* actor = RE::TESForm::LookupByID<RE::Actor>(entry.actorId);
			if (!actor || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
				return false;
			}

			auto* cell = mineRef->GetParentCell();
			if (!cell || actor->GetParentCell() != cell) {
				return false;
			}

			if (actor->IsInCombat() || actor->IsWeaponDrawn()) {
				return false;
			}

			const float distance = actor->GetPosition().GetDistance(mineRef->GetPosition());
			if (distance > kCaptiveWorkMineActivatedActorMaxDistance) {
				return false;
			}

			outActorId = actor->GetFormID();
			return true;
		}

		static bool MarkCaptiveWorkMineOccupiedByActivation(RE::TESObjectREFR* mineRef, RE::Actor* actor, const char* reason)
		{
			if (!mineRef || !actor) {
				return false;
			}

			auto* player = Player();
			if (player && actor == player) {
				return false;
			}

			if (actor->IsDead() || actor->IsDisabled()) {
				return false;
			}

			auto* base = mineRef->GetBaseObject();
			if (!base) {
				return false;
			}

			const auto edid = TFD::Util::GetEditorId(base);
			const int miningState = CaptiveWorkMiningStateFromEditorID(edid);
			if (miningState <= 0) {
				return false;
			}

			OccupiedWorkMineEntry entry{};
			entry.actorId = actor->GetFormID();
			entry.cellId = mineRef->GetParentCell() ? mineRef->GetParentCell()->GetFormID() : 0;
			entry.expires = Clock::now() + kCaptiveWorkMineActivateOccupiedTTL;
			g_occupiedWorkMiningRefs[mineRef->GetFormID()] = entry;
			g_lastWorkResourceScan = Clock::time_point{};

			spdlog::info(
				"[TFD][Location] captive work mine occupied by activation mine={:08X} actor={:08X} miningState={} mineEditor='{}' ttlMs={} reason={}",
				mineRef->GetFormID(),
				actor->GetFormID(),
				miningState,
				edid.c_str(),
				static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(kCaptiveWorkMineActivateOccupiedTTL).count()),
				reason ? reason : "unknown");
			return true;
		}

		static bool IsCaptiveWorkMineOccupiedByActor(RE::TESObjectREFR* mineRef, RE::FormID& outActorId)
		{
			outActorId = 0;
			if (!mineRef) {
				return false;
			}

			PruneExpiredCaptiveWorkMineOccupancy("mine_occupancy_check");

			const RE::FormID mineRefId = mineRef->GetFormID();
			auto occupiedIt = g_occupiedWorkMiningRefs.find(mineRefId);
			if (occupiedIt != g_occupiedWorkMiningRefs.end()) {
				RE::FormID cachedActorId = 0;
				if (IsActorStillOccupyingCachedMine(mineRef, occupiedIt->second, cachedActorId)) {
					outActorId = cachedActorId;
					return true;
				}

				spdlog::info(
					"[TFD][Location] captive work occupied mine cache stale mine={:08X} actor={:08X}",
					mineRefId,
					occupiedIt->second.actorId);
				g_occupiedWorkMiningRefs.erase(occupiedIt);
			}

			auto* cell = mineRef->GetParentCell();
			auto* player = Player();
			if (!cell || !player) {
				return false;
			}

			const auto minePos = mineRef->GetPosition();
			bool occupied = false;

			cell->ForEachReferenceInRange(minePos, kCaptiveWorkMineOccupiedRadius, [&](RE::TESObjectREFR* nearby) -> RE::BSContainer::ForEachResult {
				if (!nearby || nearby == mineRef || nearby == player) {
					return RE::BSContainer::ForEachResult::kContinue;
				}

				auto* actor = nearby->As<RE::Actor>();
				if (!actor || actor == player) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				if (actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				if (actor->GetParentCell() != cell) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				if (actor->IsInCombat() || actor->IsWeaponDrawn()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}

				const float distance = actor->GetPosition().GetDistance(minePos);
				if (distance > kCaptiveWorkMineOccupiedRadius) {
					return RE::BSContainer::ForEachResult::kContinue;
				}

				occupied = true;
				outActorId = actor->GetFormID();
				return RE::BSContainer::ForEachResult::kStop;
				});

			return occupied;
		}

		struct CaptiveWorkResourceScanResult
		{
			int miningState{ 0 };
			int craftingState{ 0 };
			RE::FormID cellId{ 0 };
			RE::FormID miningRefId{ 0 };
			RE::FormID miningBaseId{ 0 };
			RE::FormID craftingRefId{ 0 };
			RE::FormID craftingBaseId{ 0 };
			std::array<RE::FormID, 10> craftingRefIds{};
			std::array<RE::FormID, 10> craftingBaseIds{};
			std::string miningEditorId{};
			std::string craftingEditorId{};
			std::array<std::string, 10> craftingEditorIds{};
			std::array<bool, 10> craftingAvailable{};
		};

		static bool ScanCaptiveWorkResources(CaptiveWorkResourceScanResult& out, int requestedCraftingState = 0)
		{
			out = {};
			auto* player = Player();
			auto* cell = player ? player->GetParentCell() : nullptr;
			if (!player || !cell) {
				return false;
			}

			out.cellId = cell->GetFormID();
			const auto origin = player->GetPosition();
			int bestMiningPriority = 100000;
			int bestCraftingPriority = 100000;
			std::uint32_t considered = 0;
			std::uint32_t skippedDepletedMines = 0;
			std::uint32_t skippedOccupiedMines = 0;
			RE::FormID lastOccupiedMineActorId = 0;
			std::uint32_t skippedNonFurnitureCrafting = 0;
			std::uint32_t skippedFarCrafting = 0;
			std::uint32_t acceptedUsableCampfireCooking = 0;

			cell->ForEachReferenceInRange(origin, kCaptiveWorkResourceScanRadius, [&](RE::TESObjectREFR* candidate) -> RE::BSContainer::ForEachResult {
				if (!candidate || candidate == player) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				if (candidate->IsDisabled()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}

				auto* base = candidate->GetBaseObject();
				if (!base) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				++considered;

				const auto edid = TFD::Util::GetEditorId(base);
				const int miningState = CaptiveWorkMiningStateFromEditorID(edid);
				if (miningState > 0) {
					const auto miningRefId = candidate->GetFormID();
					if (miningRefId != 0 && g_depletedWorkMiningRefs.find(miningRefId) != g_depletedWorkMiningRefs.end()) {
						++skippedDepletedMines;
					}
					else {
						RE::FormID occupiedActorId = 0;
						if (IsCaptiveWorkMineOccupiedByActor(candidate, occupiedActorId)) {
							++skippedOccupiedMines;
							lastOccupiedMineActorId = occupiedActorId;
						}
						else {
							const int priority = CaptiveWorkMiningStatePriority(miningState);
							if (priority < bestMiningPriority ||
								(priority == bestMiningPriority && (out.miningRefId == 0 || miningRefId < out.miningRefId))) {
								bestMiningPriority = priority;
								out.miningState = miningState;
								out.miningRefId = miningRefId;
								out.miningBaseId = base->GetFormID();
								out.miningEditorId = edid;
							}
						}
					}
				}

				const int craftingState = CaptiveWorkCraftingStateFromEditorID(edid);
				if (craftingState > 0) {
					const bool isFurniture = base->As<RE::TESFurniture>() != nullptr;
					const bool isUsableCampfireCooking = craftingState == 7 && IsUsableCampfireCookingStation(edid);
					const bool acceptedCraftingTarget = isFurniture || isUsableCampfireCooking;
					const auto candidatePos = candidate->GetPosition();
					const double dx = static_cast<double>(candidatePos.x - origin.x);
					const double dy = static_cast<double>(candidatePos.y - origin.y);
					const double dz = static_cast<double>(candidatePos.z - origin.z);
					const double distSq = (dx * dx) + (dy * dy) + (dz * dz);
					const double furnitureRadiusSq = static_cast<double>(kCaptiveWorkFurnitureScanRadius) * static_cast<double>(kCaptiveWorkFurnitureScanRadius);
					const bool localFurniture = distSq <= furnitureRadiusSq;

					if (!acceptedCraftingTarget) {
						++skippedNonFurnitureCrafting;
					}
					else if (!localFurniture) {
						++skippedFarCrafting;
					}
					else {
						if (isUsableCampfireCooking) {
							++acceptedUsableCampfireCooking;
						}
						if (craftingState > 0 && craftingState < static_cast<int>(out.craftingAvailable.size())) {
							out.craftingAvailable[craftingState] = true;
							const auto craftingRefId = candidate->GetFormID();
							if (craftingRefId != 0 &&
								(out.craftingRefIds[craftingState] == 0 || craftingRefId < out.craftingRefIds[craftingState])) {
								out.craftingRefIds[craftingState] = craftingRefId;
								out.craftingBaseIds[craftingState] = base->GetFormID();
								out.craftingEditorIds[craftingState] = edid;
							}
						}

						if (requestedCraftingState <= 0 || craftingState == requestedCraftingState) {
							const int priority = CaptiveWorkCraftingStatePriority(craftingState);
							if (priority < bestCraftingPriority ||
								(priority == bestCraftingPriority && (out.craftingRefId == 0 || candidate->GetFormID() < out.craftingRefId))) {
								bestCraftingPriority = priority;
								out.craftingState = craftingState;
								out.craftingRefId = candidate->GetFormID();
								out.craftingBaseId = base->GetFormID();
								out.craftingEditorId = edid;
							}
						}
					}
				}

				return RE::BSContainer::ForEachResult::kContinue;
				});

			spdlog::info(
				"[TFD][Location] captive work resource scan cell={:08X} considered={} requestedCraftingState={} miningState={} miningRef={:08X} miningBase={:08X} miningEditor='{}' depletedSkipped={} occupiedSkipped={} occupiedActor={:08X} craftingState={} craftingRef={:08X} craftingBase={:08X} craftingEditor='{}' furnitureRadius={} skippedCrafting[nonFurniture={} far={}] usableCampfireCooking={} furniture[forge={} smelter={} tanning={} sharpening={} workbench={} chopping={} cooking={} alchemy={} enchanting={}]",
				out.cellId,
				considered,
				requestedCraftingState,
				out.miningState,
				out.miningRefId,
				out.miningBaseId,
				out.miningEditorId.c_str(),
				skippedDepletedMines,
				skippedOccupiedMines,
				lastOccupiedMineActorId,
				out.craftingState,
				out.craftingRefId,
				out.craftingBaseId,
				out.craftingEditorId.c_str(),
				static_cast<int>(kCaptiveWorkFurnitureScanRadius),
				skippedNonFurnitureCrafting,
				skippedFarCrafting,
				acceptedUsableCampfireCooking,
				out.craftingAvailable[1] ? 1 : 0,
				out.craftingAvailable[2] ? 1 : 0,
				out.craftingAvailable[3] ? 1 : 0,
				out.craftingAvailable[4] ? 1 : 0,
				out.craftingAvailable[5] ? 1 : 0,
				out.craftingAvailable[6] ? 1 : 0,
				out.craftingAvailable[7] ? 1 : 0,
				out.craftingAvailable[8] ? 1 : 0,
				out.craftingAvailable[9] ? 1 : 0);

			return true;
		}

		class CaptiveWorkActivateSink final : public RE::BSTEventSink<RE::TESActivateEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* ev, RE::BSTEventSource<RE::TESActivateEvent>*) override
			{
				if (!ev) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* activatedRef = ev->objectActivated ? ev->objectActivated.get() : nullptr;
				auto* actionRef = ev->actionRef ? ev->actionRef.get() : nullptr;
				auto* actor = actionRef ? actionRef->As<RE::Actor>() : nullptr;
				if (!activatedRef || !actor) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* player = RE::PlayerCharacter::GetSingleton();
				if (player && actor == player) {
					(void)TFD::Captive::NotifyRecoverGearContainerOpened(activatedRef, "tes_activate_event");
				}

				(void)MarkCaptiveWorkMineOccupiedByActivation(activatedRef, actor, "tes_activate_event");
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		CaptiveWorkActivateSink g_captiveWorkActivateSink{};

		static void RegisterCaptiveWorkActivateSink()
		{
			if (g_activateSinkRegistered) {
				return;
			}

			auto* scripts = RE::ScriptEventSourceHolder::GetSingleton();
			if (!scripts) {
				spdlog::warn("[TFD][Location] TESActivateEvent sink registration failed source=null");
				return;
			}

			scripts->AddEventSink<RE::TESActivateEvent>(&g_captiveWorkActivateSink);
			g_activateSinkRegistered = true;
			spdlog::info("[TFD][Location] TESActivateEvent sink registered for captive work mine occupancy");
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


		static bool DoRescanInternal(RE::Actor* aggressor, bool preferInterior);

		static constexpr float kCaptiveStorageScanRadius = 12000.0f;

		enum class CaptiveStorageKind
		{
			BossContainer = 0,
			BossActor = 1,
			Container = 2,
			FallbackActor = 3
		};

		struct CaptiveStorageCandidate
		{
			RE::TESObjectREFR* ref{ nullptr };
			CaptiveStorageKind kind{ CaptiveStorageKind::Container };
			double distSq{ 0.0 };
		};


		static void ClearCaptiveStorageDebugSnapshot(bool hasMarker)
		{
			g_lastCaptiveStorageDebugSnapshot = {};
			g_lastCaptiveStorageDebugSnapshot.hasMarker = hasMarker;
			RefreshMarkerGlobals();
		}

		static std::uint32_t CaptiveStorageKindCode(CaptiveStorageKind kind)
		{
			switch (kind) {
			case CaptiveStorageKind::BossContainer:
				return 1;
			case CaptiveStorageKind::BossActor:
				return 2;
			case CaptiveStorageKind::Container:
				return 3;
			case CaptiveStorageKind::FallbackActor:
				return 4;
			default:
				return 0;
			}
		}

		static double DistSq(RE::TESObjectREFR* a, RE::TESObjectREFR* b)
		{
			if (!a || !b) {
				return std::numeric_limits<double>::max();
			}

			const auto ap = a->GetPosition();
			const auto bp = b->GetPosition();
			const double dx = static_cast<double>(ap.x - bp.x);
			const double dy = static_cast<double>(ap.y - bp.y);
			const double dz = static_cast<double>(ap.z - bp.z);
			return dx * dx + dy * dy + dz * dz;
		}

		static bool IsRefInSameCell(RE::TESObjectREFR* a, RE::TESObjectREFR* b)
		{
			if (!a || !b) {
				return false;
			}
			auto* acell = a->GetParentCell();
			auto* bcell = b->GetParentCell();
			return acell && bcell && acell == bcell;
		}

		static int CaptiveStorageTiePriority(CaptiveStorageKind kind)
		{
			switch (kind) {
			case CaptiveStorageKind::BossContainer:
				return 0;
			case CaptiveStorageKind::BossActor:
				return 1;
			case CaptiveStorageKind::Container:
				return 2;
			case CaptiveStorageKind::FallbackActor:
				return 3;
			default:
				return 99;
			}
		}

		static const char* EvaluateBossStorageRejectReason(RE::TESObjectREFR* marker, RE::TESObjectREFR* ref)
		{
			if (!marker) {
				return "marker_none";
			}
			if (!ref) {
				return "ref_none";
			}
			if (ref == marker) {
				return "same_as_marker";
			}
			if (ref->IsDisabled()) {
				return "disabled";
			}
			if (!IsRefInSameCell(marker, ref)) {
				return "different_cell";
			}
			const auto maxDistSq = static_cast<double>(kCaptiveStorageScanRadius) * static_cast<double>(kCaptiveStorageScanRadius);
			if (DistSq(marker, ref) > maxDistSq) {
				return "out_of_radius";
			}
			return nullptr;
		}

		static void LogCaptiveStorageReject(CaptiveStorageKind kind, RE::TESObjectREFR* marker, RE::TESObjectREFR* ref, const char* reason)
		{
			if (!reason) {
				return;
			}

			spdlog::info(
				"[TFD][Location] captive storage reject kind={} marker={:08X} ref={:08X} base={:08X} markerCell={:08X} refCell={:08X} reason={}",
				CaptiveStorageKindCode(kind),
				marker ? marker->GetFormID() : 0u,
				ref ? ref->GetFormID() : 0u,
				(ref && ref->GetBaseObject()) ? ref->GetBaseObject()->GetFormID() : 0u,
				(marker && marker->GetParentCell()) ? marker->GetParentCell()->GetFormID() : 0u,
				(ref && ref->GetParentCell()) ? ref->GetParentCell()->GetFormID() : 0u,
				reason);
		}

		static bool IsValidBossStorageRef(RE::TESObjectREFR* marker, RE::TESObjectREFR* ref)
		{
			return EvaluateBossStorageRejectReason(marker, ref) == nullptr;
		}

		static bool IsRefKeyOnlyLocked(RE::TESObjectREFR* ref)
		{
			if (!ref) {
				return false;
			}

			auto* lock = ref->GetLock();
			if (!lock || !lock->IsLocked() || !lock->key) {
				return false;
			}

			// Only reject true key-only locks. Many normal containers can be opened
			// either with a matching key or with lockpicking, and those should stay valid.
			// In practice, the unpickable / key-required case resolves to lock level 255.
			const auto lockLevel = static_cast<std::int32_t>(ref->GetLockLevel());
			return lockLevel >= 255;
		}

		static const char* EvaluateContainerStorageRejectReason(RE::TESObjectREFR* marker, RE::TESObjectREFR* ref)
		{
			if (const auto* bossReason = EvaluateBossStorageRejectReason(marker, ref)) {
				return bossReason;
			}

			auto* base = ref ? ref->GetBaseObject() : nullptr;
			if (!base) {
				return "base_none";
			}

			if (!base->As<RE::TESObjectCONT>()) {
				return "not_container";
			}

			if (IsRefKeyOnlyLocked(ref)) {
				return "requires_key";
			}

			return nullptr;
		}

		static bool IsValidContainerStorageRef(RE::TESObjectREFR* marker, RE::TESObjectREFR* ref)
		{
			return EvaluateContainerStorageRejectReason(marker, ref) == nullptr;
		}

		static bool IsValidFallbackActor(RE::TESObjectREFR* marker, RE::Actor* actor)
		{
			if (!marker || !actor) {
				return false;
			}
			if (actor == Player()) {
				return false;
			}
			if (actor->IsDisabled() || actor->IsDead()) {
				return false;
			}
			if (actor->IsPlayerTeammate()) {
				return false;
			}
			if (!IsRefInSameCell(marker, actor)) {
				return false;
			}
			return DistSq(marker, actor) <= static_cast<double>(kCaptiveStorageScanRadius) * static_cast<double>(kCaptiveStorageScanRadius);
		}

		static void PushBestStorageCandidate(
			std::vector<CaptiveStorageCandidate>& list,
			RE::TESObjectREFR* marker,
			RE::TESObjectREFR* ref,
			CaptiveStorageKind kind)
		{
			if (!marker || !ref) {
				return;
			}

			const char* rejectReason = nullptr;
			const bool valid = [&]() {
				switch (kind) {
				case CaptiveStorageKind::Container:
					rejectReason = EvaluateContainerStorageRejectReason(marker, ref);
					break;
				case CaptiveStorageKind::BossContainer:
				case CaptiveStorageKind::BossActor:
				case CaptiveStorageKind::FallbackActor:
				default:
					rejectReason = EvaluateBossStorageRejectReason(marker, ref);
					break;
				}
				return rejectReason == nullptr;
				}();

			if (!valid) {
				LogCaptiveStorageReject(kind, marker, ref, rejectReason);
				return;
			}

			const auto id = ref->GetFormID();
			for (const auto& it : list) {
				if (it.ref && it.ref->GetFormID() == id) {
					return;
				}
			}

			list.push_back(CaptiveStorageCandidate{ ref, kind, DistSq(marker, ref) });
		}

		static RE::TESObjectREFR* PickBestStorageCandidate(const std::vector<CaptiveStorageCandidate>& list)
		{
			const CaptiveStorageCandidate* best = nullptr;

			for (const auto& it : list) {
				if (!it.ref) {
					continue;
				}

				if (!best ||
					it.distSq < best->distSq ||
					(it.distSq == best->distSq && CaptiveStorageTiePriority(it.kind) < CaptiveStorageTiePriority(best->kind)) ||
					(it.distSq == best->distSq && CaptiveStorageTiePriority(it.kind) == CaptiveStorageTiePriority(best->kind) && it.ref->GetFormID() < best->ref->GetFormID())) {
					best = &it;
				}
			}

			return best ? best->ref : nullptr;
		}

		static void SortCaptiveStorageCandidates(std::vector<CaptiveStorageCandidate>& list)
		{
			std::sort(list.begin(), list.end(), [](const CaptiveStorageCandidate& a, const CaptiveStorageCandidate& b) {
				if (!a.ref && !b.ref) {
					return false;
				}
				if (!a.ref) {
					return false;
				}
				if (!b.ref) {
					return true;
				}
				if (a.distSq != b.distSq) {
					return a.distSq < b.distSq;
				}
				if (CaptiveStorageTiePriority(a.kind) != CaptiveStorageTiePriority(b.kind)) {
					return CaptiveStorageTiePriority(a.kind) < CaptiveStorageTiePriority(b.kind);
				}
				return a.ref->GetFormID() < b.ref->GetFormID();
				});
		}

		static void StoreCaptiveStorageDebugSnapshot(
			RE::TESObjectREFR* marker,
			std::vector<CaptiveStorageCandidate> bossActors,
			std::vector<CaptiveStorageCandidate> bossContainers,
			std::vector<CaptiveStorageCandidate> containers,
			RE::TESObjectREFR* finalTarget,
			CaptiveStorageKind finalKind)
		{
			ClearCaptiveStorageDebugSnapshot(marker != nullptr);
			SortCaptiveStorageCandidates(bossActors);
			SortCaptiveStorageCandidates(bossContainers);
			SortCaptiveStorageCandidates(containers);

			for (std::size_t i = 0; i < g_lastCaptiveStorageDebugSnapshot.bossActorFormIDs.size() && i < bossActors.size(); ++i) {
				g_lastCaptiveStorageDebugSnapshot.bossActorFormIDs[i] = bossActors[i].ref ? bossActors[i].ref->GetFormID() : 0u;
			}
			for (std::size_t i = 0; i < g_lastCaptiveStorageDebugSnapshot.bossContainerFormIDs.size() && i < bossContainers.size(); ++i) {
				g_lastCaptiveStorageDebugSnapshot.bossContainerFormIDs[i] = bossContainers[i].ref ? bossContainers[i].ref->GetFormID() : 0u;
			}
			for (std::size_t i = 0; i < g_lastCaptiveStorageDebugSnapshot.containerFormIDs.size() && i < containers.size(); ++i) {
				g_lastCaptiveStorageDebugSnapshot.containerFormIDs[i] = containers[i].ref ? containers[i].ref->GetFormID() : 0u;
			}

			g_lastCaptiveStorageDebugSnapshot.finalTargetFormID = finalTarget ? finalTarget->GetFormID() : 0u;
			g_lastCaptiveStorageDebugSnapshot.finalTargetKind = finalTarget ? CaptiveStorageKindCode(finalKind) : 0u;
			RefreshMarkerGlobals();
		}

		static RE::TESObjectREFR* ResolveNearestFallbackActor(RE::TESObjectREFR* marker, RE::Actor* preferredActor)
		{
			if (!marker) {
				return nullptr;
			}

			RE::TESObjectREFR* best = nullptr;
			double bestDistSq = std::numeric_limits<double>::max();

			if (IsValidFallbackActor(marker, preferredActor)) {
				best = preferredActor;
				bestDistSq = DistSq(marker, preferredActor);
			}

			auto* cell = marker->GetParentCell();
			if (!cell) {
				return best;
			}

			const auto origin = marker->GetPosition();
			cell->ForEachReferenceInRange(origin, kCaptiveStorageScanRadius, [&](RE::TESObjectREFR* candidate) -> RE::BSContainer::ForEachResult {
				auto* actor = candidate ? candidate->As<RE::Actor>() : nullptr;
				if (!IsValidFallbackActor(marker, actor)) {
					return RE::BSContainer::ForEachResult::kContinue;
				}

				const double distSq = DistSq(marker, actor);
				if (!best || distSq < bestDistSq || (distSq == bestDistSq && actor->GetFormID() < best->GetFormID())) {
					best = actor;
					bestDistSq = distSq;
				}

				return RE::BSContainer::ForEachResult::kContinue;
				});

			return best;
		}

		static RE::TESObjectREFR* ResolveNearestCaptiveStorageInternal(RE::Actor* preferredActor)
		{
			EnsureRefTypes();
			(void)preferredActor;

			auto* marker = GetCachedCaptiveMarker();
			if (!marker) {
				DoRescanInternal(preferredActor, IsPlayerInterior());
				marker = GetCachedCaptiveMarker();
			}
			if (!marker) {
				ClearCaptiveStorageDebugSnapshot(false);
				spdlog::info("[TFD][Location] captive storage resolve miss reason=no_marker");
				return nullptr;
			}

			auto* markerLoc = GetLocationFromRef(marker);
			std::vector<RE::BGSLocation*> locs;
			AddLocationChain(locs, markerLoc);

			std::vector<CaptiveStorageCandidate> storageCandidates;
			std::vector<CaptiveStorageCandidate> bossActorCandidates;
			std::vector<CaptiveStorageCandidate> bossContainerCandidates;
			std::vector<CaptiveStorageCandidate> containerCandidates;
			storageCandidates.reserve(16);
			bossActorCandidates.reserve(8);
			bossContainerCandidates.reserve(8);
			containerCandidates.reserve(8);

			for (auto* loc : locs) {
				if (!loc) {
					continue;
				}

				for (std::uint32_t i = 0; i < loc->specialRefs.size(); ++i) {
					const auto& sref = loc->specialRefs[i];
					auto* type = sref.type;
					if (!type) {
						continue;
					}

					auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(sref.refData.refID);
					if (!ref) {
						continue;
					}

					if (SameRefType(type, g_bossType)) {
						PushBestStorageCandidate(bossActorCandidates, marker, ref, CaptiveStorageKind::BossActor);
						continue;
					}

					if (SameRefType(type, g_bossContainerType)) {
						PushBestStorageCandidate(storageCandidates, marker, ref, CaptiveStorageKind::BossContainer);
						PushBestStorageCandidate(bossContainerCandidates, marker, ref, CaptiveStorageKind::BossContainer);
						continue;
					}

					if (SameRefType(type, g_containerType)) {
						PushBestStorageCandidate(storageCandidates, marker, ref, CaptiveStorageKind::Container);
						PushBestStorageCandidate(containerCandidates, marker, ref, CaptiveStorageKind::Container);
					}
				}
			}

			spdlog::info(
				"[TFD][Location] captive storage candidate summary marker={:08X} all={} bossActors={} bossContainers={} containers={} specialRefOnly=1 noActorFallback=1",
				marker->GetFormID(),
				storageCandidates.size(),
				bossActorCandidates.size(),
				bossContainerCandidates.size(),
				containerCandidates.size());

			if (auto* storage = PickBestStorageCandidate(storageCandidates)) {
				CaptiveStorageKind finalKind = CaptiveStorageKind::Container;
				for (const auto& it : storageCandidates) {
					if (it.ref && it.ref->GetFormID() == storage->GetFormID()) {
						finalKind = it.kind;
						break;
					}
				}

				StoreCaptiveStorageDebugSnapshot(marker, bossActorCandidates, bossContainerCandidates, containerCandidates, storage, finalKind);
				const auto dist = std::sqrt(DistSq(marker, storage));
				spdlog::info(
					"[TFD][Location] captive storage resolved marker={:08X} storage={:08X} kind={} dist={:.1f} cell={:08X}",
					marker->GetFormID(),
					storage->GetFormID(),
					CaptiveStorageKindCode(finalKind),
					dist,
					storage->GetParentCell() ? storage->GetParentCell()->GetFormID() : 0);
				return storage;
			}

			StoreCaptiveStorageDebugSnapshot(marker, bossActorCandidates, bossContainerCandidates, containerCandidates, nullptr, CaptiveStorageKind::Container);
			spdlog::info(
				"[TFD][Location] captive storage resolve miss marker={:08X} specialRefOnly=1 noActorFallback=1 bossActors={} bossContainers={} containers={} -> keep player inventory",
				marker->GetFormID(),
				bossActorCandidates.size(),
				bossContainerCandidates.size(),
				containerCandidates.size());
			return nullptr;
		}


		static bool DoRescanInternal(RE::Actor* aggressor, bool preferInterior)
		{
			EnsureRefTypes();

			auto* p = Player();
			if (!p) {
				spdlog::warn("[TFD][Location] Rescan: player null");
				g_cachedMarker = {};
				ClearCaptiveStorageDebugSnapshot(false);
				RefreshMarkerGlobals();
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
				ClearCaptiveStorageDebugSnapshot(false);
				RefreshMarkerGlobals();
				return false;
			}

			g_cachedMarker = marker->GetHandle();
			spdlog::info("[TFD][Location] Marker resolved -> {:08X}", marker->GetFormID());
			RefreshMarkerGlobals();
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
			"[TFD][Location] RefTypes: Captive={} Inside={} Outside={} BossType={} BossContainer={} ContainerType={} CenterType={}",
			g_captiveType ? "OK" : "NULL",
			g_insideType ? "OK" : "NULL",
			g_outsideType ? "OK" : "NULL",
			g_bossType ? "OK" : "NULL",
			g_bossContainerType ? "OK" : "NULL",
			g_containerType ? "OK" : "NULL",
			g_centerType ? "OK" : "NULL");

		RefreshMarkerGlobals();
		RegisterCaptiveWorkActivateSink();
		ClearCaptiveWorkResourceState("initialize");
	}

	void ResetAmbientKidnapAvailabilityWatcher()
	{
		g_lastAmbientCellId = 0;
		g_lastAmbientWorldspaceId = 0;
		g_lastAmbientLocationId = 0;
		g_lastAmbientInterior = false;
		g_lastAmbientWatcherPrimed = false;
		g_lastAmbientMarkerAvailable = false;
	}

	void ClearCaptiveWorkResourceState(const char* reason)
	{
		ResolveCaptiveWorkResourceGlobals();
		SetGlobalInt(g_workMiningStateGlobal, 0);
		SetGlobalInt(g_workCraftingStateGlobal, 0);
		ClearCaptiveWorkFurnitureGlobals();
		g_lastWorkResourceCellId = 0;
		g_lastWorkMiningState = -1;
		g_lastWorkCraftingState = -1;
		g_activeWorkCraftingSubtypeRequest = 0;
		g_lastWorkMiningRefId = 0;
		g_lastWorkCraftingRefId = 0;
		g_lastWorkCraftingRefIds = {};
		g_lastWorkResourceScan = {};
		spdlog::info("[TFD][Location] captive work resource state cleared reason={}", reason ? reason : "unknown");
	}

	bool RefreshCaptiveWorkResourceState(bool force, const char* reason)
	{
		ResolveCaptiveWorkResourceGlobals();

		const std::string_view reasonView = reason ? std::string_view{ reason } : std::string_view{};
		int requestedCraftingState = ParseRequestedCaptiveWorkCraftingState(reasonView);
		if (requestedCraftingState > 0) {
			g_activeWorkCraftingSubtypeRequest = requestedCraftingState;
			spdlog::info(
				"[TFD][Location][W07] active crafting subtype latch set requested={} reason={}",
				requestedCraftingState,
				reason ? reason : "unknown");
		}
		else {
			requestedCraftingState = GetActiveCaptiveWorkCraftingSubtypeRequest();
		}

		CaptiveWorkResourceScanResult result{};
		auto* player = Player();
		auto* cell = player ? player->GetParentCell() : nullptr;
		const RE::FormID cellId = cell ? cell->GetFormID() : 0;
		const auto now = Clock::now();

		if (!force && requestedCraftingState <= 0 && cellId != 0 && g_lastWorkResourceCellId == cellId &&
			g_lastWorkResourceScan.time_since_epoch().count() != 0 &&
			now - g_lastWorkResourceScan < kCaptiveWorkResourceScanMinInterval) {
			return true;
		}

		if (!ScanCaptiveWorkResources(result, requestedCraftingState)) {
			SetGlobalInt(g_workMiningStateGlobal, 0);
			SetGlobalInt(g_workCraftingStateGlobal, 0);
			ClearCaptiveWorkFurnitureGlobals();
			g_lastWorkResourceCellId = 0;
			g_lastWorkMiningState = 0;
			g_lastWorkCraftingState = 0;
			g_lastWorkMiningRefId = 0;
			g_lastWorkCraftingRefId = 0;
			g_lastWorkCraftingRefIds = {};
			g_lastWorkResourceScan = now;
			spdlog::warn("[TFD][Location] captive work resource refresh failed reason={}", reason ? reason : "unknown");
			return false;
		}

		SetGlobalInt(g_workMiningStateGlobal, result.miningState);
		SetGlobalInt(g_workCraftingStateGlobal, result.craftingState);
		SetCaptiveWorkFurnitureGlobals(result.craftingAvailable);
		g_lastWorkResourceCellId = result.cellId;
		g_lastWorkMiningState = result.miningState;
		g_lastWorkCraftingState = result.craftingState;
		g_lastWorkMiningRefId = result.miningRefId;
		g_lastWorkCraftingRefId = result.craftingRefId;
		g_lastWorkCraftingRefIds = result.craftingRefIds;
		g_lastWorkResourceScan = now;

		spdlog::info(
			"[TFD][Location] captive work globals refreshed reason={} requestedCraftingState={} activeCraftingRequest={} cell={:08X} TFDMiningState={} TFDCraftingState={} furnitureRadius={} furniture[forge={} smelter={} tanning={} sharpening={} workbench={} chopping={} cooking={} alchemy={} enchanting={}]",
			reason ? reason : "unknown",
			requestedCraftingState,
			g_activeWorkCraftingSubtypeRequest,
			result.cellId,
			result.miningState,
			result.craftingState,
			static_cast<int>(kCaptiveWorkFurnitureScanRadius),
			result.craftingAvailable[1] ? 1 : 0,
			result.craftingAvailable[2] ? 1 : 0,
			result.craftingAvailable[3] ? 1 : 0,
			result.craftingAvailable[4] ? 1 : 0,
			result.craftingAvailable[5] ? 1 : 0,
			result.craftingAvailable[6] ? 1 : 0,
			result.craftingAvailable[7] ? 1 : 0,
			result.craftingAvailable[8] ? 1 : 0,
			result.craftingAvailable[9] ? 1 : 0);

		return true;
	}

	std::uint32_t GetLastCaptiveWorkMiningRefFormID()
	{
		return g_lastWorkMiningRefId;
	}

	std::uint32_t GetLastCaptiveWorkCraftingRefFormID()
	{
		return g_lastWorkCraftingRefId;
	}

	std::uint32_t GetLastCaptiveWorkCraftingRefFormIDForState(int craftingState)
	{
		if (craftingState <= 0 || craftingState >= static_cast<int>(g_lastWorkCraftingRefIds.size())) {
			return 0;
		}
		return g_lastWorkCraftingRefIds[static_cast<std::size_t>(craftingState)];
	}

	bool MarkLastCaptiveWorkMiningRefDepleted(const char* reason)
	{
		if (g_lastWorkMiningRefId == 0) {
			spdlog::info("[TFD][Location] captive work mining depleted mark skipped no last ref reason={}", reason ? reason : "unknown");
			return false;
		}

		g_depletedWorkMiningRefs.insert(g_lastWorkMiningRefId);
		g_lastWorkResourceScan = Clock::time_point{};
		spdlog::info(
			"[TFD][Location] captive work mining ref marked depleted ref={:08X} reason={} blockedCount={}",
			g_lastWorkMiningRefId,
			reason ? reason : "unknown",
			g_depletedWorkMiningRefs.size());
		return true;
	}

	void ClearCaptiveWorkMiningDepletedCache(const char* reason)
	{
		const auto count = g_depletedWorkMiningRefs.size();
		g_depletedWorkMiningRefs.clear();
		spdlog::info("[TFD][Location] captive work mining depleted cache cleared reason={} count={}", reason ? reason : "unknown", count);
	}

	RE::TESObjectREFR* GetLastCaptiveWorkMiningRef()
	{
		if (g_lastWorkMiningRefId == 0) {
			return nullptr;
		}
		return RE::TESForm::LookupByID<RE::TESObjectREFR>(g_lastWorkMiningRefId);
	}

	RE::TESObjectREFR* GetLastCaptiveWorkCraftingRef()
	{
		if (g_lastWorkCraftingRefId == 0) {
			return nullptr;
		}
		return RE::TESForm::LookupByID<RE::TESObjectREFR>(g_lastWorkCraftingRefId);
	}

	RE::TESObjectREFR* GetLastCaptiveWorkCraftingRefForState(int craftingState)
	{
		const auto formID = GetLastCaptiveWorkCraftingRefFormIDForState(craftingState);
		if (formID == 0) {
			return nullptr;
		}
		return RE::TESForm::LookupByID<RE::TESObjectREFR>(formID);
	}

	bool UpdateAmbientKidnapAvailability(bool force)
	{
		RE::FormID cellId = 0;
		RE::FormID worldspaceId = 0;
		RE::FormID locationId = 0;
		bool interior = false;

		if (!ResolvePlayerAmbientContext(cellId, worldspaceId, locationId, interior)) {
			return false;
		}

		const bool changed = AmbientContextChanged(cellId, worldspaceId, locationId, interior);
		if (!force && !changed) {
			return g_lastAmbientMarkerAvailable;
		}

		const bool ok = DoRescanInternal(nullptr, interior);
		RememberAmbientContext(cellId, worldspaceId, locationId, interior, ok);

		spdlog::info(
			"[TFD][Location] ambient captive marker refresh force={} changed={} cell={:08X} world={:08X} loc={:08X} interior={} result={}",
			force ? 1 : 0,
			changed ? 1 : 0,
			cellId,
			worldspaceId,
			locationId,
			interior ? 1 : 0,
			ok ? 1 : 0);

		return ok;
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
		RefreshMarkerGlobals();
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

	bool RefreshCaptiveMarkerSilent(RE::Actor* aggressor, bool preferInterior)
	{
		return DoRescanInternal(aggressor, preferInterior);
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


	RE::TESObjectREFR* ResolveNearestCaptiveStorageTarget(RE::Actor* preferredActor)
	{
		return ResolveNearestCaptiveStorageInternal(preferredActor);
	}

	std::uint32_t ResolveNearestCaptiveStorageTargetFormID(RE::Actor* preferredActor)
	{
		auto* ref = ResolveNearestCaptiveStorageInternal(preferredActor);
		return ref ? ref->GetFormID() : 0;
	}

	bool GetLastCaptiveStorageDebugSnapshot(CaptiveStorageDebugSnapshot& out)
	{
		out = g_lastCaptiveStorageDebugSnapshot;
		return out.hasMarker;
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
#ifdef _MSC_VER
#pragma warning(pop)
#endif
