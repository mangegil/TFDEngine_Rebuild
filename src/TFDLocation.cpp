#include "TFDLocation.h"

#include <vector>

#include <spdlog/spdlog.h>
#include <SKSE/SKSE.h>

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

		// Vanilla ref type EditorID (Skyrim.esm)
		static constexpr const char* kBossRefTypeEditorId = "Boss";
		static constexpr const char* kBossContainerRefTypeEditorId = "BossContainer";

		RE::BGSLocationRefType* g_captiveType = nullptr;
		RE::BGSLocationRefType* g_insideType = nullptr;
		RE::BGSLocationRefType* g_outsideType = nullptr;

		RE::BGSLocationRefType* g_bossType = nullptr;
		RE::BGSLocationRefType* g_bossContainerType = nullptr;

		RE::ObjectRefHandle g_cachedMarker{};

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

		static RE::BGSLocation* GetLocationFromRef(RE::TESObjectREFR* ref)
		{
			auto* cell = ref ? ref->GetParentCell() : nullptr;
			return cell ? cell->GetLocation() : nullptr;
		}

		static RE::BGSLocation* GetPlayerLocation()
		{
			return GetLocationFromRef(Player());
		}

		static RE::BGSLocationRefType* ResolveRefType(std::uint32_t formId)
		{
			return RE::TESForm::LookupByID<RE::BGSLocationRefType>(formId);
		}

		static void EnsureRefTypes()
		{
			// Aman kalau Initialize kepanggil terlalu awal
			if (!g_captiveType) {
				g_captiveType = ResolveRefType(kCaptiveMarkerRefType);
			}
			if (!g_insideType) {
				g_insideType = ResolveRefType(kInsideEntranceRefType);
			}
			if (!g_outsideType) {
				g_outsideType = ResolveRefType(kOutsideEntranceRefType);
			}

			// Boss types via EditorID (vanilla)
			if (!g_bossType) {
				g_bossType = RE::TESForm::LookupByEditorID<RE::BGSLocationRefType>(kBossRefTypeEditorId);
			}
			if (!g_bossContainerType) {
				g_bossContainerType = RE::TESForm::LookupByEditorID<RE::BGSLocationRefType>(kBossContainerRefTypeEditorId);
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
			for (int i = 0; cur && i < 16; i++) {
				AddUnique(list, cur);
				cur = cur->parentLoc;  // CommonLibSSE-NG: BGSLocation::parentLoc
			}
		}

		static RE::TESObjectREFR* FindFirstOfType(RE::BGSLocation* loc, RE::BGSLocationRefType* type, bool preferInterior)
		{
			if (!loc || !type) {
				return nullptr;
			}

			RE::TESObjectREFR* firstHit = nullptr;

			for (std::uint32_t i = 0; i < loc->specialRefs.size(); i++) {
				const auto& sref = loc->specialRefs[i];
				if (!SameRefType(sref.type, type)) {
					continue;
				}

				// Note: kalau ref non persistent dan cell belum kebuka, bisa null
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
			// 1) CaptiveMarker (vanilla)
			if (auto* r = FindFirstOfType(loc, g_captiveType, preferInterior)) {
				return r;
			}

			// 2) fallback: inside/outside entrance
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

			// 3) last resort: try both
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

			// BossContainer dulu (paling sering persistent dan gampang kebaca)
			if (auto* r = FindFirstOfType(loc, g_bossContainerType, true)) {
				return r;
			}

			// Baru Boss
			if (auto* r = FindFirstOfType(loc, g_bossType, true)) {
				return r;
			}

			// last resort: coba juga prefer exterior
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
			for (std::uint32_t i = 0; i < loc->specialRefs.size(); i++) {
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

				spdlog::info("[TFD][Location] Resolve hit loc={:08X} editorId='{}' preferInterior={}",
					loc->GetFormID(),
					TFD::Util::GetEditorId(loc).c_str(),
					preferInterior ? "true" : "false");

				return true;
			}

			return false;
		}

		static bool DoRescanInternal(RE::Actor* aggressor, bool preferInterior)
		{
			EnsureRefTypes();

			auto* p = Player();
			if (!p) {
				spdlog::warn("[TFD][Location] Rescan: player null");
				return false;
			}

			auto* playerLoc = GetPlayerLocation();
			auto* aggressorLoc = GetLocationFromRef(aggressor);

			std::vector<RE::BGSLocation*> candidates;
			AddLocationChain(candidates, playerLoc);
			AddLocationChain(candidates, aggressorLoc);

			// Boss fallback: cari anchor boss dari kandidat awal
			RE::BGSLocation* bossLoc = nullptr;
			for (auto* loc : candidates) {
				if (auto* bossAnchor = ResolveBossAnchorFromLocation(loc)) {
					bossLoc = GetLocationFromRef(bossAnchor);
					if (bossLoc) {
						spdlog::info("[TFD][Location] Boss anchor -> {:08X}, bossLoc={:08X} editorId='{}'",
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

			// Pass 1: preferInterior
			if (!TryResolveFromList(candidates, preferInterior, marker)) {
				// Pass 2: fallback kebalikannya
				TryResolveFromList(candidates, !preferInterior, marker);
			}

			if (!marker) {
				spdlog::warn("[TFD][Location] Rescan: marker not found (preferInterior={})",
					preferInterior ? "true" : "false");

				// dump dari playerLoc biar gampang debug
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
				spdlog::info("[TFD][Location] marker {:08X} pos: {:.1f} {:.1f} {:.1f} interior={}",
					marker->GetFormID(), mp.x, mp.y, mp.z,
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

		spdlog::info("[TFD][Location] RefTypes: Captive={} (FormID:000130FA) Inside={} Outside={} BossType={} BossContainer={}",
			g_captiveType ? "OK" : "NULL",
			g_insideType ? "OK" : "NULL",
			g_outsideType ? "OK" : "NULL",
			g_bossType ? "OK" : "NULL",
			g_bossContainerType ? "OK" : "NULL");
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
		auto* loc = cell ? cell->GetLocation() : nullptr;

		spdlog::info("[TFD][Location] ===== CONTEXT =====");
		spdlog::info("[TFD][Location] cell={:08X} interior={}",
			cell ? cell->GetFormID() : 0,
			cell ? (cell->IsInteriorCell() ? "true" : "false") : "null");
		spdlog::info("[TFD][Location] location={:08X} editorId='{}' name='{}'",
			loc ? loc->GetFormID() : 0,
			loc ? TFD::Util::GetEditorId(loc).c_str() : "",
			loc ? loc->GetName() : "");

		auto* marker = GetCachedCaptiveMarker();
		spdlog::info("[TFD][Location] cachedMarker={:08X}", marker ? marker->GetFormID() : 0);

		DumpSpecialRefs(loc);
	}

	bool TeleportToCaptiveMarker()
	{
		RE::DebugNotification("TFDEngine: Teleport debug queued");

		// Kalau cache kosong, baru rescan (kidnap style: prefer interior)
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