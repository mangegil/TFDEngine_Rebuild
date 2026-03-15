#include "TFDCaptiveDoorController.h"

#include <RE/B/BGSOpenCloseForm.h>
#include <RE/E/ExtraLock.h>
#include <spdlog/spdlog.h>

namespace TFD
{
	RE::TESObjectREFR* CaptiveDoorController::GetDoorPtr() const
	{
		if (!_door) {
			return nullptr;
		}
		auto refPtr = _door.get();
		return refPtr.get();
	}

	bool CaptiveDoorController::IsOpenOrOpening(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return false;
		}
		const auto st = RE::BGSOpenCloseForm::GetOpenState(a_ref);
		return st == RE::BGSOpenCloseForm::OPEN_STATE::kOpen ||
			st == RE::BGSOpenCloseForm::OPEN_STATE::kOpening;
	}

	bool CaptiveDoorController::IsLocked(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return false;
		}
		if (auto* lock = a_ref->GetLock(); lock) {
			return lock->IsLocked();
		}
		return false;
	}

	void CaptiveDoorController::SetOpen(RE::TESObjectREFR* a_ref, bool a_open, bool a_snap)
	{
		if (!a_ref) {
			return;
		}
		RE::BGSOpenCloseForm::SetOpenState(a_ref, a_open, a_snap);
	}

	void CaptiveDoorController::CaptureSnapshot(RE::TESObjectREFR* a_ref, Snapshot& a_out)
	{
		if (!a_ref) {
			return;
		}
		auto* lock = a_ref->GetLock();
		if (!lock) {
			spdlog::warn("[TFD][CaptiveDoor] Door has no lock data: {:08X}", a_ref->GetFormID());
			return;
		}

		a_out.baseLevel = lock->baseLevel;
		a_out.leveled = lock->flags.any(RE::REFR_LOCK::Flag::kLeveled);
		a_out.locked = lock->IsLocked();
		a_out.keyFormID = lock->key ? lock->key->GetFormID() : 0;
		a_out.captured = true;

		spdlog::info("[TFD][CaptiveDoor] Snapshot captured door={:08X} baseLevel={} leveled={} locked={} key={:08X}",
			a_ref->GetFormID(),
			static_cast<int>(a_out.baseLevel),
			a_out.leveled ? 1 : 0,
			a_out.locked ? 1 : 0,
			a_out.keyFormID);
	}

	void CaptiveDoorController::RestoreLockFromSnapshot(RE::TESObjectREFR* a_ref, const Snapshot& a_snap)
	{
		if (!a_ref || !a_snap.captured) {
			return;
		}
		auto* lock = a_ref->GetLock();
		if (!lock) {
			spdlog::warn("[TFD][CaptiveDoor] Restore requested but door has no lock data: {:08X}", a_ref->GetFormID());
			return;
		}

		lock->baseLevel = a_snap.baseLevel;

		if (a_snap.keyFormID != 0) {
			lock->key = RE::TESForm::LookupByID<RE::TESKey>(a_snap.keyFormID);
		} else {
			lock->key = nullptr;
		}

		if (a_snap.leveled) {
			lock->flags.set(RE::REFR_LOCK::Flag::kLeveled);
		} else {
			lock->flags.reset(RE::REFR_LOCK::Flag::kLeveled);
		}

		lock->SetLocked(a_snap.locked);
		lock->numTries = 0;

		spdlog::info("[TFD][CaptiveDoor] Restored lock door={:08X} baseLevel={} leveled={} locked={} key={:08X}",
			a_ref->GetFormID(),
			static_cast<int>(a_snap.baseLevel),
			a_snap.leveled ? 1 : 0,
			a_snap.locked ? 1 : 0,
			a_snap.keyFormID);
	}

	void CaptiveDoorController::Bind(RE::TESObjectREFR* a_doorRef)
	{
		if (!a_doorRef) {
			return;
		}

		_door = a_doorRef->GetHandle();

		if (!_snap.captured) {
			CaptureSnapshot(a_doorRef, _snap);
		}

		_prevLocked = IsLocked(a_doorRef);
		_prevOpenOrOpening = IsOpenOrOpening(a_doorRef);

		spdlog::info("[TFD][CaptiveDoor] Bound door {:08X}", a_doorRef->GetFormID());
	}

	void CaptiveDoorController::SealToInitial(bool a_snapClose)
	{
		auto* door = GetDoorPtr();
		if (!door || !_snap.captured) {
			return;
		}

		_ignoreUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);

		SetOpen(door, false, a_snapClose);

		auto* lock = door->GetLock();
		if (lock) {
			lock->baseLevel = _snap.baseLevel;

			if (_snap.keyFormID != 0) {
				lock->key = RE::TESForm::LookupByID<RE::TESKey>(_snap.keyFormID);
			} else {
				lock->key = nullptr;
			}

			if (_snap.leveled) {
				lock->flags.set(RE::REFR_LOCK::Flag::kLeveled);
			} else {
				lock->flags.reset(RE::REFR_LOCK::Flag::kLeveled);
			}

			// Important: during captive/recapture we always force the door locked,
			// even if the original snapshot was unlocked.
			lock->SetLocked(true);
			lock->numTries = 0;
		}

		// Some persisted refs can remain visually open after the first close.
		if (IsOpenOrOpening(door)) {
			SetOpen(door, false, true);
		}

		_prevLocked = IsLocked(door);
		_prevOpenOrOpening = IsOpenOrOpening(door);

		spdlog::info("[TFD][CaptiveDoor] Resealed captive door={:08X} baseLevel={} leveled={} initialLocked={} forcedLocked={} key={:08X}",
			door->GetFormID(),
			static_cast<int>(_snap.baseLevel),
			_snap.leveled ? 1 : 0,
			_snap.locked ? 1 : 0,
			_prevLocked ? 1 : 0,
			_snap.keyFormID);
	}

	void CaptiveDoorController::UnlockForRelease(double ignoreSeconds, bool a_openDoor, bool a_snapOpen)
	{
		auto* door = GetDoorPtr();
		if (!door) {
			return;
		}

		const auto ms = static_cast<int>(ignoreSeconds * 1000.0);
		_ignoreUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);

		if (auto* lock = door->GetLock(); lock) {
			lock->baseLevel = _snap.baseLevel;
			if (_snap.keyFormID != 0) {
				lock->key = RE::TESForm::LookupByID<RE::TESKey>(_snap.keyFormID);
			} else {
				lock->key = nullptr;
			}
			if (_snap.leveled) {
				lock->flags.set(RE::REFR_LOCK::Flag::kLeveled);
			} else {
				lock->flags.reset(RE::REFR_LOCK::Flag::kLeveled);
			}
			lock->SetLocked(false);
			lock->numTries = 0;
		}

		if (a_openDoor) {
			SetOpen(door, true, a_snapOpen);
		}

		_prevLocked = IsLocked(door);
		_prevOpenOrOpening = IsOpenOrOpening(door);

		spdlog::info("[TFD][CaptiveDoor] UnlockForRelease door={:08X} ignoreMs={}", door->GetFormID(), ms);
	}

	bool CaptiveDoorController::UpdateWatcher()
	{
		auto* door = GetDoorPtr();
		if (!door) {
			return false;
		}

		const auto now = std::chrono::steady_clock::now();
		const bool ignored = now < _ignoreUntil;

		const bool curLocked = IsLocked(door);
		const bool curOpenOrOpening = IsOpenOrOpening(door);

		const bool lockJustBroke = (_prevLocked && !curLocked);
		const bool openedNow = (!_prevOpenOrOpening && curOpenOrOpening);

		_prevLocked = curLocked;
		_prevOpenOrOpening = curOpenOrOpening;

		if (ignored) {
			return false;
		}

		if (lockJustBroke || openedNow) {
			spdlog::info("[TFD][CaptiveDoor] Escape-trigger door={:08X} (unlockEvent={}, openEvent={})",
				door->GetFormID(),
				lockJustBroke ? 1 : 0,
				openedNow ? 1 : 0);
			return true;
		}

		return false;
	}

	void CaptiveDoorController::Reset()
	{
		_door.reset();
		_snap = {};
		_prevLocked = false;
		_prevOpenOrOpening = false;
		_ignoreUntil = {};
	}
}
