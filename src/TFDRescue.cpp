#include "TFDRescue.h"

#include "TFDLocation.h"
#include "TFDRescueGreet.h"

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

		static bool IsValidSaviorActor(RE::Actor* actor)
		{
			return actor && !actor->IsDead() && !actor->IsDisabled() && actor->HasKeywordString("ActorTypeNPC");
		}

		static RE::Actor* ResolveStateSavior(const State& state)
		{
			if (!state.follower) {
				return nullptr;
			}

			auto followerSp = RE::Actor::LookupByHandle(state.follower.native_handle());
			auto* follower = followerSp.get();
			return IsValidSaviorActor(follower) ? follower : nullptr;
		}

		static RE::Actor* ResolveCachedBedOwnerSavior(RE::BGSLocation* safeLoc, RE::TESObjectREFR* dest)
		{
			if (auto* owner = TFD::Location::ResolveApprovedBedOwnerActor(dest)) {
				if (IsValidSaviorActor(owner)) {
					return owner;
				}
			}

			if (safeLoc) {
				if (auto* owner = TFD::Location::ResolveBestApprovedBedOwnerActorForLocation(safeLoc)) {
					if (IsValidSaviorActor(owner)) {
						return owner;
					}
				}
			}

			return nullptr;
		}


		static void AssignSaviorForRescue(const State& state, RE::BGSLocation* safeLoc, RE::TESObjectREFR* dest)
		{
			const char* source = "fallback";
			RE::Actor* savior = ResolveStateSavior(state);
			if (savior) {
				source = "standing_teammate";
			}
			else {
				savior = ResolveCachedBedOwnerSavior(safeLoc, dest);
				if (savior) {
					source = "cached_bed_owner";
				}
			}

			if (!savior) {
				spdlog::info("[TFD][Rescue][R36D] no savior armed safeLoc={:08X} dest={:08X}", safeLoc ? safeLoc->GetFormID() : 0u, dest ? dest->GetFormID() : 0u);
				return;
			}

			// R36D: do not assign the Savior alias or hard-open dialogue in the
			// middle of the rescue transition.  Player MoveTo can still be resolving
			// a LoadingMenu/world-ready edge here.  RescueGreet owns the post-teleport
			// queue and will assign the Savior bridge only after the world is ready.
			const bool armed = TFD::RescueGreet::ArmPostTeleportSavior(savior, source, "rescue_post_teleport_r36d");
			spdlog::info(
				"[TFD][Rescue][R36D] post-teleport savior arm actor={:08X} source={} armed={}",
				savior->GetFormID(),
				source,
				armed ? 1 : 0);
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
		spdlog::info("[TFD][Rescue][P33G] MoveTo rescue destination requested without blocking sleep dest={:08X} reason={}",
			dest ? dest->GetFormID() : 0u,
			reason ? reason : "rescue_transition");
		if (handlers.recoverPlayerForTransition) {
			handlers.recoverPlayerForTransition();
		}

		const int grace = (state.branch == TFD::Transition::FallbackBranch::RescueCached) ? 1 : 4;
		if (handlers.finalizePostDefeatRecoveryWindow) {
			handlers.finalizePostDefeatRecoveryWindow(grace, 1);
		}
		AssignSaviorForRescue(state, safeLoc, dest);

		spdlog::info("[TFD][Rescue] complete reason={} safeLoc={:08X} dest={:08X} branch={}",
			reason ? reason : "unknown",
			safeLoc ? safeLoc->GetFormID() : 0u,
			dest->GetFormID(),
			getBranchName ? getBranchName(state.branch) : "unknown");
		return true;
	}
}
