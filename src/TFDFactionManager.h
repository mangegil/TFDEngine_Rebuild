#pragma once

#include <RE/Skyrim.h>

#include <vector>

namespace TFD::FactionManager
{
	inline constexpr const char* kAllowListEditorId = "TFDHostileFactionAllowList";

	void Initialize();
	bool ApplyFromAggressor(RE::Actor* aggressor);
	void Clear();
	bool IsActive();
	bool SharesAllowedFactionExact(RE::Actor* lhs, RE::Actor* rhs);

	std::vector<RE::Actor*> CollectTruceActors();
	bool HasAnyReleaseFollowGrace();
	bool HasReleaseFollowGrace(RE::Actor* actor);
	void ApplyReleaseFollowGraceToSpeakerAndCrowd(RE::Actor* speaker, double durationSeconds, const char* reason = nullptr);
	void RemoveReleaseFollowGraceFromSpeakerAndCrowd(RE::Actor* speaker, const char* reason = nullptr);
	void CancelReleaseFollowGraceFromPlayerAggression(RE::Actor* actor, const char* reason = nullptr);
	void MaintainReleaseFollowGrace();
	void ClearAllReleaseFollowGrace(const char* reason = nullptr);

	void SyncDefeatedEnemyMirror(RE::Actor* actor, int& aliasSlot, bool& factionApplied);
	void ClearDefeatedEnemyMirror(RE::Actor* actor, int& aliasSlot, bool& factionApplied, const char* reason = nullptr);
	void ClearAllDefeatedEnemyMirrors(const char* reason = nullptr);
}