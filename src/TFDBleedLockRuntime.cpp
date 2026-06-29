#include "TFDBleedLockRuntime.h"

#include <algorithm>
#include <tuple>
#include <vector>

#include <RE/A/ActorValues.h>
#include <spdlog/spdlog.h>

#include "TFDBleedout.h"
#include "TFDPlayerDownRouter.h"
#include "TFDTeammateManager.h"

namespace TFD::BleedLockRuntime
{
	namespace
	{
		std::chrono::steady_clock::time_point g_lastScan{};

		const char* KindName(Kind kind)
		{
			return kind == Kind::Player ? "Player" : "Ally";
		}

		std::chrono::steady_clock::time_point Now()
		{
			return std::chrono::steady_clock::now();
		}

		TFD::PlayerDamageGuard::Config BuildGuardConfig(
			const Context& context,
			RE::Actor* actor,
			float thresholdPct,
			float protectedHp,
			float safeFloorHp)
		{
			if (context.buildPlayerDamageGuardConfig) {
				return context.buildPlayerDamageGuardConfig(actor, thresholdPct, protectedHp, safeFloorHp);
			}
			TFD::PlayerDamageGuard::Config config{};
			config.thresholdPct = thresholdPct;
			config.protectedHp = protectedHp;
			config.safeFloorHp = safeFloorHp;
			return config;
		}

		void LogReference(
			const Context& context,
			RE::Actor* actor,
			const Entry& entry,
			const char* point,
			const char* note)
		{
			if (context.logReferenceBleedState) {
				context.logReferenceBleedState(actor, entry, point, note);
			}
		}
	}

	bool HasActive(RE::Actor* actor)
	{
		if (!actor) {
			return false;
		}
		return TFD::BleedLockState::Entries().find(actor->GetFormID()) != TFD::BleedLockState::Entries().end();
	}

	bool HasPlayer(const Context& context)
	{
		auto* player = context.getPlayer ? context.getPlayer() : nullptr;
		if (!player) {
			return false;
		}
		auto& entries = TFD::BleedLockState::Entries();
		auto it = entries.find(player->GetFormID());
		return it != entries.end() && it->second.kind == Kind::Player;
	}

	bool HasAlly(RE::Actor* actor)
	{
		if (!actor) {
			return false;
		}
		auto& entries = TFD::BleedLockState::Entries();
		auto it = entries.find(actor->GetFormID());
		return it != entries.end() && it->second.kind == Kind::Ally;
	}

	void Enter(const Context& context, RE::Actor* actor, Kind kind, float thresholdPct, const char* reason)
	{
		if (!actor || actor->IsDisabled() || actor->IsDead()) {
			return;
		}

		auto& entries = TFD::BleedLockState::Entries();
		const auto formID = actor->GetFormID();
		auto& entry = entries[formID];
		const bool wasNew = !entry.handle;
		entry.handle = actor->GetHandle();
		entry.kind = kind;
		entry.thresholdPct = std::clamp(thresholdPct, 2.0f, 95.0f);
		entry.lastHealthSample = actor->GetActorValue(RE::ActorValue::kHealth);

		if (kind == Kind::Player) {
			const float maxHp = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
			if (context.resetPreDeathShield) {
				context.resetPreDeathShield();
			}
			entry.minHp = (std::max)(1.0f, maxHp * 0.05f);
			if (context.setPlayerBleedImmune) {
				context.setPlayerBleedImmune(true);
			}
			const float safeHealth = context.resolvePlayerBleedRuntimeSafeHealth ?
				context.resolvePlayerBleedRuntimeSafeHealth(actor, entry.thresholdPct) :
				entry.minHp;
			entry.protectedHealth = (std::max)(safeHealth, actor->GetActorValue(RE::ActorValue::kHealth));
			if (context.maxMinHp) {
				context.maxMinHp(entry.protectedHealth);
			}
			entry.lastHealthSample = entry.protectedHealth;
			if (context.clampHealth) {
				context.clampHealth(actor, entry.protectedHealth);
			}
			(void)TFD::PlayerDamageGuard::Arm(
				actor,
				context.resolveAggressor ? context.resolveAggressor() : nullptr,
				BuildGuardConfig(context, actor, entry.thresholdPct, entry.protectedHealth, entry.minHp),
				reason ? reason : "player_bleed_lock_enter");
			spdlog::info(
				"[TFD][BleedLockRuntime][R23] player shield armed actor={:08X} hardFloor={:.2f} protectedHealth={:.2f} threshold={:.1f}",
				actor->GetFormID(),
				entry.minHp,
				entry.protectedHealth,
				entry.thresholdPct);
		}

		if (context.applyBleedRegenOverride) {
			context.applyBleedRegenOverride(actor, entry);
		}
		if (!wasNew) {
			return;
		}

		entry.referenceProbeStartedAt = Now();
		entry.referenceProbeSampleMask = 0;
		entry.referenceProbePulseCount = 0;
		LogReference(context, actor, entry, "reference_entry_before_start", reason);

		if (!actor->IsDead()) {
			actor->NotifyAnimationGraph("BleedoutStart");
			++entry.referenceProbePulseCount;
			LogReference(context, actor, entry, "reference_entry_after_start", reason);
			if (kind == Kind::Player) {
				LogReference(context, actor, entry, "reference_entry_after_package", reason);
			} else {
				LogReference(context, actor, entry, "reference_entry_no_package_ally", reason);
			}
			entry.lastPulse = Now();
		}

		spdlog::info(
			"[TFD][BleedLockRuntime][R23] enter actor={:08X} kind={} threshold={:.1f} reason={}",
			formID,
			KindName(kind),
			entry.thresholdPct,
			reason ? reason : "unknown");
	}

	void Release(const Context& context, RE::Actor* actor, const char* reason, bool playGetUp)
	{
		if (!actor) {
			return;
		}
		auto& entries = TFD::BleedLockState::Entries();
		const auto formID = actor->GetFormID();
		auto it = entries.find(formID);
		if (it == entries.end()) {
			return;
		}
		auto entry = it->second;
		const auto kind = entry.kind;
		LogReference(context, actor, entry, "reference_release_before_owner_cleanup", reason);
		entries.erase(it);
		if (context.restoreBleedRegenOverride) {
			context.restoreBleedRegenOverride(actor, entry);
		}
		if (kind == Kind::Player) {
			TFD::PlayerDamageGuard::ReleaseToVirtualHealth(
				actor,
				BuildGuardConfig(context, actor, entry.thresholdPct, entry.protectedHealth, entry.minHp),
				reason ? reason : "player_bleed_lock_release");
			if (context.setMinHp) {
				context.setMinHp(0.0f);
			}
			if (context.setPlayerBleedImmune) {
				context.setPlayerBleedImmune(false);
			}
		}
		if (playGetUp && !actor->IsDead() && !actor->IsDisabled()) {
			LogReference(context, actor, entry, "reference_release_before_stop", reason);
			actor->NotifyAnimationGraph("BleedoutStop");
			LogReference(context, actor, entry, "reference_release_after_stop", reason);
			actor->NotifyAnimationGraph("GetUpStart");
			LogReference(context, actor, entry, "reference_release_after_getup", reason);
			if (kind == Kind::Player) {
				LogReference(context, actor, entry, "reference_release_after_package", reason);
			} else {
				LogReference(context, actor, entry, "reference_release_no_package_ally", reason);
			}
		}
		else {
			LogReference(context, actor, entry, "reference_release_without_getup", reason);
		}
		spdlog::info(
			"[TFD][BleedLockRuntime][R23] release actor={:08X} reason={} getUp={}",
			formID,
			reason ? reason : "unknown",
			playGetUp ? 1 : 0);
	}

	void ReleasePlayer(const Context& context, const char* reason, bool playGetUp)
	{
		Release(context, context.getPlayer ? context.getPlayer() : nullptr, reason, playGetUp);
	}

	void ClearAll(const Context& context, const char* reason)
	{
		auto& entries = TFD::BleedLockState::Entries();
		for (auto& [formID, entry] : entries) {
			auto sp = entry.handle.get();
			auto* actor = sp.get();
			if (context.restoreBleedRegenOverride) {
				context.restoreBleedRegenOverride(actor, entry);
			}
			if (entry.kind == Kind::Player) {
				if (context.resetPlayerDamageGuard) {
					context.resetPlayerDamageGuard(reason ? reason : "clear_all_bleed_locks");
				} else {
					TFD::PlayerDamageGuard::Reset(reason ? reason : "clear_all_bleed_locks");
				}
				if (context.setMinHp) {
					context.setMinHp(0.0f);
				}
				if (context.setPlayerBleedImmune) {
					context.setPlayerBleedImmune(false);
				}
			}
			spdlog::info(
				"[TFD][BleedLockRuntime][R23] clear actor={:08X} kind={} reason={}",
				formID,
				KindName(entry.kind),
				reason ? reason : "unknown");
		}
		entries.clear();
		g_lastScan = {};
		if (context.resetEnemyThresholdScanTimer) {
			context.resetEnemyThresholdScanTimer();
		}
		if (context.clearPendingOverkillRoute) {
			context.clearPendingOverkillRoute("bleed_lock_runtime_clear");
		}
		if (context.clearPreDeathShield) {
			context.clearPreDeathShield();
		}
	}

	void Tick(const Context& context)
	{
		const auto now = Now();
		if (context.tickPlayerKillmoveSuppression) {
			context.tickPlayerKillmoveSuppression();
		}
		if (context.tickPlayerOverkillBlockPending) {
			context.tickPlayerOverkillBlockPending();
		}
		if (context.tickPlayerPreDeathShield) {
			context.tickPlayerPreDeathShield();
		}
		if (context.scanBleedLockCandidates &&
			(g_lastScan.time_since_epoch().count() == 0 || (now - g_lastScan) >= std::chrono::milliseconds(250))) {
			g_lastScan = now;
			context.scanBleedLockCandidates();
		}

		std::vector<std::tuple<RE::FormID, RE::Actor*, const char*, bool>> releases;
		auto& entries = TFD::BleedLockState::Entries();
		for (auto& [formID, entry] : entries) {
			auto sp = entry.handle.get();
			auto* actor = sp.get();
			if (!actor || actor->IsDisabled()) {
				releases.emplace_back(formID, actor, "invalid", false);
				continue;
			}
			if (entry.kind == Kind::Ally && actor->IsDead()) {
				releases.emplace_back(formID, actor, "dead", false);
				continue;
			}

			if (context.applyBleedRegenOverride) {
				context.applyBleedRegenOverride(actor, entry);
			}
			if (entry.kind == Kind::Player) {
				if (context.enforcePlayerBleedInvulnerability) {
					context.enforcePlayerBleedInvulnerability(actor, entry);
				}
				if (context.maybeLogReferenceBleedSamples) {
					context.maybeLogReferenceBleedSamples(actor, entry, now);
				}
				if (actor->IsDead(false)) {
					if (context.forcePlayerBleedAlive) {
						context.forcePlayerBleedAlive(actor, entry, "tick_dead_state");
					}
					continue;
				}
			}
			else {
				if (context.maybeLogReferenceBleedSamples) {
					context.maybeLogReferenceBleedSamples(actor, entry, now);
				}
				const bool recoveryDialogueHold = context.isDownedTeammateRecoveryDialogueHoldActor ?
					context.isDownedTeammateRecoveryDialogueHoldActor(actor) : false;
				const bool playerBattleObserveActive = context.isPlayerBattleObserveActive ? context.isPlayerBattleObserveActive() : false;
				const float hpPctBeforeClamp = context.getActorHealthPct ? context.getActorHealthPct(actor) : 100.0f;
				if (!recoveryDialogueHold && hpPctBeforeClamp > (entry.thresholdPct + 8.0f)) {
					if (!playerBattleObserveActive) {
						releases.emplace_back(formID, actor, "ally_recovered", true);
						continue;
					}

					if (entry.referenceProbePulseCount <= 1) {
						spdlog::info(
							"[TFD][BleedLockRuntime][R23] ally recovery held by battle observe actor={:08X} hpPct={:.1f} threshold={:.1f} reason=await_battle_observe_resolution",
							formID,
							hpPctBeforeClamp,
							entry.thresholdPct);
					}
				}

				const float holdCeilingHp = context.resolveActorHealthForPct ?
					(std::max)(1.0f, context.resolveActorHealthForPct(actor, (std::max)(2.0f, entry.thresholdPct - 0.5f))) :
					1.0f;
				const float hpBeforeClamp = actor->GetActorValue(RE::ActorValue::kHealth);
				if (context.clampHealthCeiling) {
					context.clampHealthCeiling(actor, holdCeilingHp);
				}
				const float hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
				if (hpNow > holdCeilingHp + 0.001f) {
					spdlog::info(
						"[TFD][BleedLockRuntime][R23] ally clamp actor={:08X} from={:.2f} to={:.2f} thresholdPct={:.1f}",
						actor->GetFormID(),
						hpBeforeClamp,
						holdCeilingHp,
						entry.thresholdPct);
				}
				entry.healAccumulator = 0.0f;
				entry.lastHealGain = {};
				entry.lastHealthSample = hpNow;
			}

			const bool allyRecoveryDialogueHold =
				entry.kind == Kind::Ally &&
				(context.isDownedTeammateRecoveryDialogueHoldActor ? context.isDownedTeammateRecoveryDialogueHoldActor(actor) : false);
			const auto pulseInterval = std::chrono::milliseconds(250);
			const bool pulseDue = entry.lastPulse.time_since_epoch().count() == 0 || (now - entry.lastPulse) >= pulseInterval;
			const bool bleedingOut = context.isActorBleedingOut ? context.isActorBleedingOut(actor) : false;
			const bool shouldPulse = entry.kind == Kind::Ally ? (!allyRecoveryDialogueHold && pulseDue) : (!bleedingOut || pulseDue);
			if (shouldPulse) {
				const bool firstReassert = entry.referenceProbePulseCount == 1;
				if (firstReassert) {
					LogReference(context, actor, entry, "reference_before_first_reassert", "existing_250ms_bleedout_start");
				}
				actor->NotifyAnimationGraph("BleedoutStart");
				++entry.referenceProbePulseCount;
				if (firstReassert) {
					LogReference(context, actor, entry, "reference_after_first_reassert", "existing_250ms_bleedout_start");
				}
				entry.lastPulse = now;
			}
		}

		for (auto& [formID, actor, reason, playGetUp] : releases) {
			if (actor) {
				Release(context, actor, reason, playGetUp);
			}
			else {
				auto it = entries.find(formID);
				if (it != entries.end()) {
					if (it->second.kind == Kind::Player) {
						if (context.setMinHp) {
							context.setMinHp(0.0f);
						}
						if (context.setPlayerBleedImmune) {
							context.setPlayerBleedImmune(false);
						}
					}
					spdlog::info(
						"[TFD][BleedLockRuntime][R23] release actor={:08X} reason={} getUp=0",
						formID,
						reason ? reason : "unknown");
					entries.erase(it);
				}
			}
		}
	}
}
