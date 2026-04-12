#pragma once

#include <RE/Skyrim.h>
#include <chrono>

namespace TFD::Captive
{
	class DoorController
	{
	public:
		// Bind door for this captive session (call once when you know the door).
		// It will snapshot initial lock config the first time.
		void Bind(RE::TESObjectREFR* a_doorRef);

		// Close + lock + restore initial level/key/flags.
		// Call after teleport into cell (including recapture). Use snapClose=true for instant close.
		void SealToInitial(bool a_snapClose = true);

		// Unlock/open due to RELEASE (Gold/OStim/etc). This should NOT trigger escape logic.
		// ignoreSeconds: during this window, UpdateWatcher() will ignore door state changes.
		void UnlockForRelease(double ignoreSeconds = 3.0, bool a_openDoor = true, bool a_snapOpen = false);

		// Poll watcher (call every ~0.25-0.5s during CAPTIVE).
		// Returns true if door unlock/open should be treated as ESCAPE start.
		bool UpdateWatcher();

		// Clear internal state (call when session ends / new captive spot).
		void Reset();

		// Quick getters
		bool HasDoor() const { return static_cast<bool>(_door); }

	private:
		struct Snapshot
		{
			bool captured = false;

			std::int8_t baseLevel = 0;
			bool leveled = false;
			bool locked = false;
			RE::FormID keyFormID = 0;  // TESKey formID (0 = none)
		};

		RE::ObjectRefHandle _door{};
		Snapshot _snap{};

		bool _prevLocked = false;
		bool _prevOpenOrOpening = false;

		std::chrono::steady_clock::time_point _ignoreUntil{};

		RE::TESObjectREFR* GetDoorPtr() const;

		static bool IsOpenOrOpening(RE::TESObjectREFR* a_ref);
		static bool IsLocked(RE::TESObjectREFR* a_ref);
		static void SetOpen(RE::TESObjectREFR* a_ref, bool a_open, bool a_snap);
		static void RestoreLockFromSnapshot(RE::TESObjectREFR* a_ref, const Snapshot& a_snap);
		static void CaptureSnapshot(RE::TESObjectREFR* a_ref, Snapshot& a_out);
	};
}

namespace TFD
{
	using CaptiveDoorController = Captive::DoorController;
}