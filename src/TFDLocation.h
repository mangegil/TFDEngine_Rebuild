#pragma once
#include <cstdint>

namespace RE
{
	class TESObjectREFR;
	class Actor;
	class BGSLocation;
}

namespace SKSE
{
	class SerializationInterface;
}

namespace TFD::Location
{
	struct SafeCheckpoint
	{
		std::uint32_t safeLocationId{ 0 };
		std::uint32_t parentLocationId{ 0 };
		std::uint32_t centerMarkerRefId{ 0 };
		std::uint32_t insideEntranceRefId{ 0 };
		std::uint32_t entryDoorRefId{ 0 };
		std::uint32_t cellId{ 0 };
		bool isInterior{ false };
		std::uint32_t visitSerial{ 0 };
	};

	struct ApprovedBed
	{
		std::uint32_t safeLocationId{ 0 };
		std::uint32_t bedRefId{ 0 };
		std::uint32_t cellId{ 0 };
		std::uint32_t useSerial{ 0 };
	};

	void Initialize();

	bool RescanCaptiveMarker();
	bool RescanCaptiveMarkerWithAggressor(RE::Actor* aggressor, bool preferInterior);

	RE::TESObjectREFR* GetCachedCaptiveMarker();
	std::uint32_t GetCachedCaptiveMarkerFormID();

	void DumpContextToLog();
	bool TeleportToCaptiveMarker();

	RE::BGSLocation* GetLocationFromRef(RE::TESObjectREFR* ref);

	bool LocationHasKeywordByEditorID(RE::BGSLocation* loc, const char* editorID);
	bool IsRescueCandidateLocation(RE::BGSLocation* loc);

	RE::BGSLocation* ResolveRescueTargetLocation(RE::BGSLocation* startLoc);
	RE::BGSLocation* ResolveRescueTargetLocationFromRef(RE::TESObjectREFR* ref);

	bool RememberSafeCheckpoint(RE::BGSLocation* safeLoc, RE::TESObjectREFR* contextRef = nullptr, RE::TESObjectREFR* entryDoor = nullptr);
	bool RememberSafeCheckpointFromRef(RE::TESObjectREFR* ref, RE::TESObjectREFR* entryDoor = nullptr);

	bool RememberApprovedBed(RE::BGSLocation* safeLoc, RE::TESObjectREFR* bedRef);
	bool RememberApprovedBedFromRefs(RE::TESObjectREFR* safeContextRef, RE::TESObjectREFR* bedRef);
	bool RefreshPlayerInteriorSafeCheckpoint();

	bool GetLastSafeCheckpointForLocation(RE::BGSLocation* loc, SafeCheckpoint& outCp);
	bool GetBestApprovedBedForLocation(RE::BGSLocation* loc, ApprovedBed& outBed);
	RE::BGSLocation* GetMostRecentCachedSafeLocation();

RE::TESObjectREFR* ResolveMostRecentCachedRescueDestination(bool preferInterior = true);

	RE::TESObjectREFR* ResolvePreferredRescueDestination(RE::BGSLocation* safeLoc, bool preferInterior = true);

	void ClearRescueCache();
	bool SaveRescueCache(SKSE::SerializationInterface* intfc);
	bool LoadRescueCache(SKSE::SerializationInterface* intfc, std::uint32_t version, std::uint32_t length);

	void DumpRescueCacheToLog();
}