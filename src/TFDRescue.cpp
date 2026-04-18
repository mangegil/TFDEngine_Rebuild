#include "TFDRescue.h"

#include "TFDLocation.h"

#include <cmath>
#include <thread>

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

namespace TFD::Rescue
{
	namespace
	{
		RE::TESGlobal* g_stateGlobal = nullptr;
		bool g_logged = false;

		void ResolveGlobal()
		{
			if (!g_stateGlobal) {
				g_stateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDRescueState");
				if (g_stateGlobal && !g_logged) {
					g_logged = true;
					spdlog::info("[TFD][Rescue] TFDRescueState resolved {:08X}", g_stateGlobal->GetFormID());
				}
			}
		}

		static RE::TESObjectREFR* LookupRefByFormID(RE::FormID formID)
		{
			if (!formID) {
				return nullptr;
			}
			return RE::TESForm::LookupByID<RE::TESObjectREFR>(formID);
		}

		static RE::TESObjectREFR* ResolveBestRescueDestination(RE::BGSLocation* safeLoc)
		{
			if (!safeLoc) {
				return nullptr;
			}

			TFD::Location::ApprovedBed bed{};
			if (TFD::Location::GetBestApprovedBedForLocation(safeLoc, bed)) {
				if (auto* ref = LookupRefByFormID(bed.bedRefId)) {
					return ref;
				}
			}

			TFD::Location::SafeCheckpoint cp{};
			if (TFD::Location::GetLastSafeCheckpointForLocation(safeLoc, cp)) {
				if (auto* ref = LookupRefByFormID(cp.insideEntranceRefId)) {
					return ref;
				}
				if (auto* ref = LookupRefByFormID(cp.centerMarkerRefId)) {
					return ref;
				}
				if (auto* ref = LookupRefByFormID(cp.entryDoorRefId)) {
					return ref;
				}
			}

			if (auto* ref = TFD::Location::ResolvePreferredRescueDestination(safeLoc, true)) {
				return ref;
			}

			return nullptr;
		}

		static bool ResolveDestination(const State& state,
			RE::Actor* player,
			const char* reason,
			RE::BGSLocation*& outSafeLoc,
			RE::TESObjectREFR*& outDest)
		{
			outSafeLoc = nullptr;
			outDest = nullptr;

			if (!player) {
				return false;
			}

			if (state.branch == TFD::Transition::FallbackBranch::RescueCached && state.destination) {
				auto destSp = state.destination.get();
				outDest = destSp.get();
				outSafeLoc = TFD::Location::GetLocationFromRef(outDest);
			}

			if (!outDest) {
				outSafeLoc = TFD::Location::ResolveRescueTargetLocationFromRef(player);
				outDest = outSafeLoc ? ResolveBestRescueDestination(outSafeLoc) : nullptr;

				if ((!outSafeLoc || !outDest)) {
					auto* fallbackLoc = TFD::Location::GetMostRecentCachedSafeLocation();
					if (fallbackLoc) {
						auto* fallbackDest = ResolveBestRescueDestination(fallbackLoc);
						if (fallbackDest) {
							spdlog::info("[TFD][Rescue] fallback to recent cache reason={} currentLoc={:08X} fallbackLoc={:08X}",
								reason ? reason : "unknown",
								outSafeLoc ? outSafeLoc->GetFormID() : 0,
								fallbackLoc->GetFormID());
							outSafeLoc = fallbackLoc;
							outDest = fallbackDest;
						}
					}
				}
			}

			return outDest != nullptr;
		}

		static void AssignSaviorIfPresent(const State& state, const Handlers& handlers)
		{
			if (!state.follower || !handlers.assignPlayerSavior) {
				return;
			}

			auto followerSp = RE::Actor::LookupByHandle(state.follower.native_handle());
			if (auto* follower = followerSp.get()) {
				handlers.assignPlayerSavior(follower);
				spdlog::info("[TFD][Rescue] savior assigned {:08X}", follower->GetFormID());
			}
		}
	}

	void SetStateValue(int value)
	{
		ResolveGlobal();
		if (g_stateGlobal) {
			g_stateGlobal->value = static_cast<float>(value);
		}
	}

	int GetStateValue()
	{
		ResolveGlobal();
		return g_stateGlobal ? static_cast<int>(std::lround(g_stateGlobal->value)) : 0;
	}

	bool IsActive()
	{
		return GetStateValue() != 0;
	}

	bool ExecuteResolvedBranch(const State& state,
		const char* reason,
		const Handlers& handlers,
		const std::function<const char*(TFD::Transition::FallbackBranch)>& getBranchName)
	{
		auto* player = handlers.getPlayer ? handlers.getPlayer() : nullptr;
		if (!player) {
			return false;
		}

		RE::BGSLocation* safeLoc = nullptr;
		RE::TESObjectREFR* dest = nullptr;
		if (!ResolveDestination(state, player, reason, safeLoc, dest)) {
			spdlog::info("[TFD][Rescue] unavailable reason={} cause=no_destination", reason ? reason : "unknown");
			return false;
		}

		player->MoveTo(dest);
		std::this_thread::sleep_for(std::chrono::milliseconds(120));
		if (handlers.recoverPlayerForTransition) {
			handlers.recoverPlayerForTransition();
		}

		const int grace = (state.branch == TFD::Transition::FallbackBranch::RescueCached) ? 1 : 4;
		if (handlers.finalizePostDefeatRecoveryWindow) {
			handlers.finalizePostDefeatRecoveryWindow(grace, 1);
		}
		AssignSaviorIfPresent(state, handlers);

		spdlog::info("[TFD][Rescue] complete reason={} safeLoc={:08X} dest={:08X} branch={}",
			reason ? reason : "unknown",
			safeLoc ? safeLoc->GetFormID() : 0u,
			dest->GetFormID(),
			getBranchName ? getBranchName(state.branch) : "unknown");
		return true;
	}
}
