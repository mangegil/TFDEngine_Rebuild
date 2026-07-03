#include "TFDPlayerOverkillDamageHook.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <mutex>
#include <utility>

#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include "TFDPlayerDamageGuard.h"
#include "TFDPlayerDownRouter.h"
#include "TFDSettings.h"

namespace TFD::PlayerOverkillDamageHook
{
	namespace
	{
		std::mutex g_contextLock;
		Context g_context;
		bool g_hasContext = false;

		// P10: killmove guard lifecycle is owned here, not by DefeatMonitor.
		bool g_playerKillmoveGuardActive = false;
		bool g_playerKillmoveWasEssential = false;
		bool g_playerKillmoveWasProtected = false;
		bool g_playerKillmoveWasCanSpeakEssentialDown = false;
		std::chrono::steady_clock::time_point g_playerKillmoveGuardUntil{};
		std::chrono::steady_clock::time_point g_playerKillmoveGuardLastLog{};

		// P11: pre-death shield lifecycle is owned here, not by DefeatMonitor.
		bool g_playerPreDeathShieldActive = false;
		std::chrono::steady_clock::time_point g_playerPreDeathShieldLastLog{};

		static std::chrono::steady_clock::time_point Now()
		{
			return std::chrono::steady_clock::now();
		}

		Context ResolveContext()
		{
			std::scoped_lock lock(g_contextLock);
			return g_context;
		}

		static bool IsHealth(RE::ActorValue av)
		{
			return av == RE::ActorValue::kHealth;
		}


		static bool IsObserverAlly(const Context& context, RE::Actor* actor)
		{
			return context.isObserverAlly && context.isObserverAlly(actor);
		}

		static bool IsPreDeathShieldActive(const Context& context)
		{
			return context.isPreDeathShieldActive && context.isPreDeathShieldActive();
		}

		static bool HasPlayerBleedLock(const Context& context)
		{
			return context.hasPlayerBleedLock && context.hasPlayerBleedLock();
		}

		static bool IsInBleedState(const Context& context)
		{
			return context.isInBleedState && context.isInBleedState();
		}

		static bool HasRecentEnemyTargetingPlayer(const Context& context, double maxAgeSec)
		{
			return context.hasRecentEnemyTargetingPlayer && context.hasRecentEnemyTargetingPlayer(maxAgeSec);
		}

		static float ResolveBleedRuntimeSafeHealth(const Context& context, RE::Actor* actor, float thresholdPct)
		{
			if (context.resolveBleedRuntimeSafeHealth) {
				return context.resolveBleedRuntimeSafeHealth(actor, thresholdPct);
			}
			return ResolveSafeFloorHealth(actor, thresholdPct);
		}

		static float GetActorHealthPct(const Context& context, RE::Actor* actor)
		{
			if (context.getActorHealthPct) {
				return context.getActorHealthPct(actor);
			}
			if (!actor) {
				return 100.0f;
			}
			const float maxHp = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float hp = actor->GetActorValue(RE::ActorValue::kHealth);
			return std::clamp((hp / maxHp) * 100.0f, 0.0f, 100.0f);
		}

		static RE::Actor* ResolveAggressor(const Context& context)
		{
			return context.resolveAggressor ? context.resolveAggressor() : nullptr;
		}

		static RE::Actor* ResolveLastEnemyTargetingPlayer(const Context& context, float radius, double maxAgeSec)
		{
			return context.resolveLastEnemyTargetingPlayer ? context.resolveLastEnemyTargetingPlayer(radius, maxAgeSec) : nullptr;
		}

		static float ResolvePreDeathMinimumHealth(RE::Actor* actor, float thresholdPct)
		{
			if (!actor) {
				return 1.0f;
			}
			const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float floorPct = std::clamp(thresholdPct + 1.0f, 3.0f, 95.0f);
			return (std::max)(1.0f, hpMax * (floorPct / 100.0f));
		}

		static bool HasRealBleedOwner(const Context& context)
		{
			return HasPlayerBleedLock(context) || IsInBleedState(context);
		}

		static bool HasCheapPlayerOverkillThreat(const Context& context, RE::Actor* player, RE::Actor* attacker)
		{
			if (!player) {
				return false;
			}
			if (IsPreDeathShieldActive(context) || HasPlayerBleedLock(context) || IsInBleedState(context)) {
				return true;
			}
			if (player->IsInCombat()) {
				return true;
			}
			if (HasRecentEnemyTargetingPlayer(context, 2.50)) {
				return true;
			}
			if (attacker && attacker != player && !attacker->IsDead() && !attacker->IsDisabled() && !IsObserverAlly(context, attacker)) {
				return true;
			}
			return false;
		}

		static bool TryClampPlayerOverkillDamage(const Context& context, RE::Actor* target, RE::Actor* attacker, float originalDamage, float& clampedDamage, float& safeFloorOut)
		{
			clampedDamage = originalDamage;
			safeFloorOut = 1.0f;
			auto* player = context.getPlayer ? context.getPlayer() : RE::PlayerCharacter::GetSingleton();
			if (!player || target != player || originalDamage <= 0.0f || player->IsDisabled()) {
				return false;
			}

			const float thresholdPct = context.getDefeatThresholdPct ? std::clamp(context.getDefeatThresholdPct(), 2.0f, 95.0f) : std::clamp(TFD::Settings::GetDefeatThresholdPct(), 2.0f, 95.0f);
			const float hpNow = player->GetActorValue(RE::ActorValue::kHealth);
			const bool bleedOwned = HasPlayerBleedLock(context) || IsInBleedState(context);
			const float safeFloorHp = bleedOwned ?
				ResolveBleedRuntimeSafeHealth(context, player, thresholdPct) :
				ResolveSafeFloorHealth(player, thresholdPct);
			safeFloorOut = safeFloorHp;

			const bool pendingOverkill = context.hasPendingOverkillRoute ? context.hasPendingOverkillRoute() : TFD::PlayerDownRouter::HasPendingOverkillRoute();
			if (pendingOverkill) {
				const auto clamp = TFD::PlayerDamageGuard::ClampIncomingHealthDamage(
					player,
					attacker,
					originalDamage,
					safeFloorHp,
					true,
					"r451a_followup_pending");
				if (!clamp.blocked) {
					return false;
				}
				clampedDamage = clamp.damageOut;
				TFD::PlayerDownRouter::AccumulatePendingOverkillDamage(originalDamage, clampedDamage, clamp.blockedDamage);
				return true;
			}

			if (bleedOwned) {
				const auto clamp = TFD::PlayerDamageGuard::ClampIncomingHealthDamage(
					player,
					attacker,
					originalDamage,
					safeFloorHp,
					true,
					"r451a_bleed_owned");
				if (!clamp.blocked) {
					return false;
				}
				clampedDamage = clamp.damageOut;
				return true;
			}

			const bool killMoveActive = player->GetActorRuntimeData().boolFlags.all(RE::Actor::BOOL_FLAGS::kIsInKillMove);
			if (killMoveActive) {
				SetKillmoveGuard(true, "check_clamp_killmove_state", std::chrono::milliseconds(2500));
			}

			if (!killMoveActive && !HasCheapPlayerOverkillThreat(context, player, attacker)) {
				return false;
			}

			const float projectedHp = killMoveActive ? -1.0f : hpNow - originalDamage;
			if (projectedHp > safeFloorHp && projectedHp > 0.0f) {
				return false;
			}

			const auto clamp = TFD::PlayerDamageGuard::ClampIncomingHealthDamage(
				player,
				attacker,
				originalDamage,
				safeFloorHp,
				false,
				"r451a_overkill_route");
			if (!clamp.blocked) {
				return false;
			}

			clampedDamage = clamp.damageOut;
			const float blockedDamage = clamp.blockedDamage;

			TFD::PlayerDownRouter::QueueOverkillRoute(
				attacker,
				thresholdPct,
				hpNow,
				originalDamage,
				clampedDamage,
				blockedDamage,
				safeFloorHp,
				"r451a_overkill_route");

			if (attacker && attacker != player && !IsObserverAlly(context, attacker) && context.rememberAggressor) {
				context.rememberAggressor(attacker);
			}

			spdlog::warn(
				"[TFD][PlayerDamageGuard][P10] player overkill route bridge queued attacker={:08X} hpBefore={:.2f} damageIn={:.2f} damageOut={:.2f} blocked={:.2f} projectedHp={:.2f} safeFloor={:.2f} threshold={:.1f} activeShield={} bleedLock={} bleedState={}",
				attacker ? attacker->GetFormID() : 0u,
				hpNow,
				originalDamage,
				clampedDamage,
				blockedDamage,
				projectedHp,
				safeFloorHp,
				thresholdPct,
				IsPreDeathShieldActive(context) ? 1 : 0,
				HasPlayerBleedLock(context) ? 1 : 0,
				IsInBleedState(context) ? 1 : 0);

			return true;
		}


		union ConditionParam
		{
			char c;
			std::int32_t i;
			float f;
			RE::TESForm* form;
		};

		static bool ShouldAttackKill(RE::Actor* attacker, RE::Actor* victim)
		{
			if (!attacker || !victim) {
				return false;
			}

			static RE::TESConditionItem cond;
			static std::once_flag flag;
			std::call_once(flag, [&]() {
				cond.data.functionData.function = RE::FUNCTION_DATA::FunctionID::kShouldAttackKill;
				cond.data.flags.opCode = RE::CONDITION_ITEM_DATA::OpCode::kEqualTo;
				cond.data.comparisonValue.f = 1.0f;
			});

			ConditionParam param{};
			param.form = const_cast<RE::TESObjectREFR*>(victim->As<RE::TESObjectREFR>());
			cond.data.functionData.params[0] = std::bit_cast<void*>(param);

			RE::ConditionCheckParams params(
				const_cast<RE::TESObjectREFR*>(attacker->As<RE::TESObjectREFR>()),
				const_cast<RE::TESObjectREFR*>(victim->As<RE::TESObjectREFR>()));
			return cond(params);
		}

		static bool IsPotentialKaputtPlayerExecution(RE::Actor* player)
		{
			if (!player) {
				return false;
			}
			auto* state = player->AsActorState();
			if (!state) {
				return false;
			}
			if (state->IsBleedingOut()) {
				return true;
			}
			const auto knockState = state->GetKnockState();
			return knockState == RE::KNOCK_STATE_ENUM::kGetUp || knockState == RE::KNOCK_STATE_ENUM::kQueued;
		}

		static bool QueueActionKillmoveDeniedRoute(const Context& context, RE::Actor* player, RE::Actor* attacker, const char* reason)
		{
			if (!player || player->IsDisabled()) {
				return false;
			}

			const float thresholdPct = context.getDefeatThresholdPct ?
				std::clamp(context.getDefeatThresholdPct(), 2.0f, 95.0f) :
				std::clamp(TFD::Settings::GetDefeatThresholdPct(), 2.0f, 95.0f);
			const float hpNow = (std::max)(0.0f, player->GetActorValue(RE::ActorValue::kHealth));
			const float safeFloorHp = HasRealBleedOwner(context) ?
				ResolveBleedRuntimeSafeHealth(context, player, thresholdPct) :
				ResolveSafeFloorHealth(player, thresholdPct);

			if (context.setPlayerBleedImmune) {
				context.setPlayerBleedImmune(true);
			}
			SetKillmoveGuard(true, reason && reason[0] ? reason : "p32f_action_killmove_veto", std::chrono::milliseconds(4500));
			TFD::PlayerDamageGuard::SetHardImmunity(player, true, reason && reason[0] ? reason : "p32f_action_killmove_veto");
			player->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
			if (player->IsDead(false)) {
				player->Resurrect(false, true);
			}
			if (context.clampHealth) {
				context.clampHealth(player, safeFloorHp);
			}

			if (attacker && attacker != player && !IsObserverAlly(context, attacker)) {
				TFD::PlayerDamageGuard::NoteAttacker(attacker);
				if (context.rememberAggressor) {
					context.rememberAggressor(attacker);
				}
				if (context.noteEnemyTargetingPlayer) {
					context.noteEnemyTargetingPlayer(attacker);
				}
			}

			const bool pendingOverkill = context.hasPendingOverkillRoute ? context.hasPendingOverkillRoute() : TFD::PlayerDownRouter::HasPendingOverkillRoute();
			if (!pendingOverkill && !HasRealBleedOwner(context)) {
				TFD::PlayerDownRouter::QueueOverkillRoute(
					attacker,
					thresholdPct,
					hpNow,
					0.0f,
					0.0f,
					0.0f,
					safeFloorHp,
					reason && reason[0] ? reason : "p32f_action_killmove_veto");
			}

			return true;
		}

		static bool TryVetoPlayerKillmoveAction(RE::TESActionData* actionData, const char* source)
		{
			if (!actionData || !actionData->source || !HasContext()) {
				return false;
			}

			auto* attacker = actionData->source->As<RE::Actor>();
			if (!attacker || attacker->IsPlayerRef() || attacker->IsDead() || attacker->IsDisabled()) {
				return false;
			}

			const auto context = ResolveContext();
			auto* player = context.getPlayer ? context.getPlayer() : RE::PlayerCharacter::GetSingleton();
			if (!player || player->IsDisabled()) {
				return false;
			}
			if (IsObserverAlly(context, attacker)) {
				return false;
			}

			auto victimHandle = attacker->GetActorRuntimeData().currentCombatTarget;
			auto victimPtr = victimHandle.get();
			auto* victim = victimPtr.get();
			if (victim != player) {
				return false;
			}

			const bool shouldKill = ShouldAttackKill(attacker, player);
			const bool kaputtExecutionCandidate = IsPotentialKaputtPlayerExecution(player);
			if (!shouldKill && !kaputtExecutionCandidate) {
				return false;
			}

			QueueActionKillmoveDeniedRoute(context, player, attacker, shouldKill ? "p32f_action_should_attack_kill_veto" : "p32f_action_kaputt_execution_veto");
			spdlog::warn(
				"[TFD][PlayerKillmove][P32F] action killmove veto source={} attacker={:08X} victim={:08X} shouldAttackKill={} kaputtExecCandidate={} pending={} bleedLock={} bleedState={}",
				source && source[0] ? source : "unknown",
				attacker->GetFormID(),
				player->GetFormID(),
				shouldKill ? 1 : 0,
				kaputtExecutionCandidate ? 1 : 0,
				(context.hasPendingOverkillRoute ? context.hasPendingOverkillRoute() : TFD::PlayerDownRouter::HasPendingOverkillRoute()) ? 1 : 0,
				HasPlayerBleedLock(context) ? 1 : 0,
				IsInBleedState(context) ? 1 : 0);
			return true;
		}

		class AttackActionKillmoveVetoHook
		{
		public:
			static void Install()
			{
				if (g_installed.exchange(true, std::memory_order_acq_rel)) {
					return;
				}

#if defined(SKYRIM_SUPPORT_AE)
				REL::Relocation<std::uintptr_t> attackAction{ REL::ID(49170), 0x435 };
#else
				REL::Relocation<std::uintptr_t> attackAction{ REL::ID(48139), 0x4D7 };
#endif
				auto& trampoline = SKSE::GetTrampoline();
				_AttackAction = trampoline.write_call<5>(attackAction.address(), AttackAction);
				spdlog::info("[TFD][PlayerKillmove][P32F] AttackAction killmove veto hook installed id=48139 offset=0x4D7 mode=block_npc_player_killmove_and_kaputt");
			}

		private:
			static bool AttackAction(RE::TESActionData* actionData)
			{
				if (TryVetoPlayerKillmoveAction(actionData, "AttackAction")) {
					return false;
				}
				return _AttackAction(actionData);
			}

			static inline std::atomic_bool g_installed{ false };
			static inline REL::Relocation<decltype(AttackAction)> _AttackAction;
		};

		class CheckClampDamageModifierHook
		{
		public:
			static void Install()
			{
				if (g_installed.exchange(true, std::memory_order_acq_rel)) {
					return;
				}

				REL::Relocation<std::uintptr_t> playerCharacterVtbl{ RE::PlayerCharacter::VTABLE[0] };
				_CheckClampDamageModifier = playerCharacterVtbl.write_vfunc(0x127, CheckClampDamageModifier);
				spdlog::info("[TFD][PlayerDamageGuard][P10] CheckClampDamageModifier hook installed by PlayerOverkillDamageHook vfunc=0x127 mode=clamp_health_damage owner=PlayerOverkillDamageHook");
			}

		private:
			static float CheckClampDamageModifier(RE::Actor* target, RE::ActorValue av, float delta)
			{
				if (g_insideCheckClampDamageModifier) {
					return _CheckClampDamageModifier(target, av, delta);
				}

				g_insideCheckClampDamageModifier = true;
				const auto context = ResolveContext();
				auto* player = context.getPlayer ? context.getPlayer() : RE::PlayerCharacter::GetSingleton();
				const bool playerHealth = player && target == player && IsHealth(av);
				const float hpBefore = playerHealth ? player->GetActorValue(RE::ActorValue::kHealth) : 0.0f;
				const float engineResult = _CheckClampDamageModifier(target, av, delta);
				float finalResult = engineResult;

				if (playerHealth && engineResult < -0.001f) {
					const float damageIn = -engineResult;
					float damageOut = damageIn;
					auto* attacker = context.resolveCachedAttacker ? context.resolveCachedAttacker() : nullptr;
					const bool killMoveActive = player->GetActorRuntimeData().boolFlags.all(RE::Actor::BOOL_FLAGS::kIsInKillMove);
					const bool pendingOverkill = context.hasPendingOverkillRoute ? context.hasPendingOverkillRoute() : TFD::PlayerDownRouter::HasPendingOverkillRoute();
					const bool playerBleedLock = context.hasPlayerBleedLock ? context.hasPlayerBleedLock() : false;
					const bool inBleedState = context.isInBleedState ? context.isInBleedState() : false;
					const bool recentEnemyTargeting = context.hasRecentEnemyTargetingPlayer ? context.hasRecentEnemyTargetingPlayer(1.25) : false;
					const bool attackerHostile = attacker && attacker != player && !attacker->IsDead() && !attacker->IsDisabled() &&
						!(context.isObserverAlly && context.isObserverAlly(attacker));
					const bool combatHealthDamage =
						killMoveActive ||
						pendingOverkill ||
						playerBleedLock ||
						inBleedState ||
						player->IsInCombat() ||
						recentEnemyTargeting ||
						attackerHostile;

					if (combatHealthDamage) {
						SetKillmoveGuard(true, "check_clamp_combat_health_damage", std::chrono::milliseconds(1800));
					}
					else if (!combatHealthDamage && IsKillmoveGuardActive()) {
						spdlog::debug(
							"[TFD][Defeat][R460A] killmove guard not armed for non-combat health damage hpBefore={:.2f} damageIn={:.2f} delta={:.2f}",
							hpBefore,
							damageIn,
							delta);
					}

					float safeFloorHp = 1.0f;
					if (TryClampPlayerOverkillDamage(context, target, attacker, damageIn, damageOut, safeFloorHp)) {
						finalResult = -damageOut;
						spdlog::warn(
							"[TFD][PlayerDamageGuard][P10] CheckClampDamageModifier clamped av={} delta={:.2f} engineResult={:.2f} finalResult={:.2f} hpBefore={:.2f} projectedEngine={:.2f} projectedFinal={:.2f} safeFloor={:.2f} pending={} attacker={:08X}",
							static_cast<int>(av),
							delta,
							engineResult,
							finalResult,
							hpBefore,
							hpBefore + engineResult,
							hpBefore + finalResult,
							safeFloorHp,
							pendingOverkill ? 1 : 0,
							attacker ? attacker->GetFormID() : 0u);
					}
				}

				g_insideCheckClampDamageModifier = false;
				return finalResult;
			}

			static inline std::atomic_bool g_installed{ false };
			static inline thread_local bool g_insideCheckClampDamageModifier{ false };
			static inline REL::Relocation<decltype(CheckClampDamageModifier)> _CheckClampDamageModifier;
		};
	}

	float ResolvePreDeathArmPct(float thresholdPct)
	{
		const float threshold = std::clamp(thresholdPct, 2.0f, 95.0f);
		// R446A: dynamic pre-death arm band; moved out of DefeatMonitor in P11.
		const float guardBand = std::clamp((std::max)(6.0f, threshold * 0.5f), 6.0f, 12.0f);
		return std::clamp(threshold + guardBand, 5.0f, 99.0f);
	}

	bool IsPreDeathShieldActive()
	{
		return g_playerPreDeathShieldActive;
	}

	void SetPreDeathShieldActive(bool active, RE::Actor* player, float hpPct, float thresholdPct, const char* reason)
	{
		const auto context = ResolveContext();
		if (active) {
			if (!g_playerPreDeathShieldActive) {
				g_playerPreDeathShieldActive = true;
				g_playerPreDeathShieldLastLog = Now();
				spdlog::info(
					"[TFD][PlayerDamageGuard][P11] player pre-death shield armed hpPct={:.1f} threshold={:.1f} reason={}",
					hpPct,
					thresholdPct,
					reason ? reason : "unknown");
			}
			if (context.setPlayerBleedImmune) {
				context.setPlayerBleedImmune(true);
			}
			if (player) {
				player->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
			}
			return;
		}

		if (!g_playerPreDeathShieldActive) {
			return;
		}

		g_playerPreDeathShieldActive = false;
		g_playerPreDeathShieldLastLog = {};
		if (!HasRealBleedOwner(context) && context.setPlayerBleedImmune) {
			context.setPlayerBleedImmune(false);
		}
		spdlog::info(
			"[TFD][PlayerDamageGuard][P11] player pre-death shield released hpPct={:.1f} threshold={:.1f} reason={}",
			hpPct,
			thresholdPct,
			reason ? reason : "unknown");
	}

	void ClearPreDeathShield(const char* reason, bool releaseBleedImmuneIfUnowned)
	{
		if (!g_playerPreDeathShieldActive) {
			return;
		}
		const auto context = ResolveContext();
		g_playerPreDeathShieldActive = false;
		g_playerPreDeathShieldLastLog = {};
		if (releaseBleedImmuneIfUnowned && !HasRealBleedOwner(context) && context.setPlayerBleedImmune) {
			context.setPlayerBleedImmune(false);
		}
		spdlog::info(
			"[TFD][PlayerDamageGuard][P11] player pre-death shield cleared reason={} release={}",
			reason ? reason : "unknown",
			releaseBleedImmuneIfUnowned ? 1 : 0);
	}

	void TickPreDeathShield()
	{
		if (!g_playerPreDeathShieldActive) {
			return;
		}
		const auto context = ResolveContext();
		auto* player = context.getPlayer ? context.getPlayer() : RE::PlayerCharacter::GetSingleton();
		const float thresholdPct = context.getDefeatThresholdPct ? std::clamp(context.getDefeatThresholdPct(), 2.0f, 95.0f) : std::clamp(TFD::Settings::GetDefeatThresholdPct(), 2.0f, 95.0f);
		if (!player || player->IsDisabled()) {
			SetPreDeathShieldActive(false, player, 0.0f, thresholdPct, "invalid_player");
			return;
		}
		if (HasRealBleedOwner(context)) {
			// Ownership has upgraded to the real bleed runtime.  Keep the hard flags;
			// the bleed lock will release them at the terminal recovery point.
			g_playerPreDeathShieldActive = false;
			g_playerPreDeathShieldLastLog = {};
			return;
		}

		if (context.setPlayerBleedImmune) {
			context.setPlayerBleedImmune(true);
		}
		auto& flags = player->GetActorRuntimeData().boolFlags;
		flags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);

		bool recoveredDeadState = false;
		if (player->IsDead(false)) {
			player->Resurrect(false, true);
			if (context.clampHealth) {
				context.clampHealth(player, ResolvePreDeathMinimumHealth(player, thresholdPct));
			}
			recoveredDeadState = true;
			spdlog::warn("[TFD][PlayerDamageGuard][P11] player pre-death shield recovered dead_state reason=tick");
		}

		const float hpPct = GetActorHealthPct(context, player);
		const bool noImmediateThreat =
			!player->IsInCombat() &&
			ResolveAggressor(context) == nullptr &&
			ResolveLastEnemyTargetingPlayer(context, 2400.0f, 3.0) == nullptr;
		const float nearBleedPct = std::clamp(thresholdPct + 2.0f, 3.0f, 95.0f);
		if (!noImmediateThreat && (recoveredDeadState || hpPct <= nearBleedPct)) {
			if (context.setPlayerBleedImmune) {
				context.setPlayerBleedImmune(true);
			}
			player->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
			const char* promoteReason = recoveredDeadState ? "r445a_predeath_deadstate_promote" : "r445a_predeath_near_threshold_promote";
			if (context.enterPlayerBleedLock) {
				context.enterPlayerBleedLock(player, thresholdPct, promoteReason);
			}
			const bool dispatched = context.dispatchThresholdScanImmediateBleedout ?
				context.dispatchThresholdScanImmediateBleedout(player, hpPct, thresholdPct, promoteReason) :
				false;
			spdlog::warn(
				"[TFD][PlayerDamageGuard][P11] pre-death shield promoted to bleedout dispatched={} hpPct={:.1f} threshold={:.1f} nearBleed={:.1f} recoveredDead={}",
				dispatched ? 1 : 0,
				hpPct,
				thresholdPct,
				nearBleedPct,
				recoveredDeadState ? 1 : 0);
			return;
		}

		if (noImmediateThreat && hpPct <= nearBleedPct) {
			if (context.tryBeginThresholdNoThreatRescueFallback && context.tryBeginThresholdNoThreatRescueFallback(player, thresholdPct, "predeath_no_threat_rescue")) {
				return;
			}
		}

		const bool tooHealthy = hpPct > std::clamp(thresholdPct + 28.0f, 5.0f, 99.0f);
		if (tooHealthy || noImmediateThreat) {
			SetPreDeathShieldActive(false, player, hpPct, thresholdPct, tooHealthy ? "hp_recovered" : "no_immediate_threat");
			return;
		}

		const auto now = Now();
		if (g_playerPreDeathShieldLastLog.time_since_epoch().count() == 0 ||
			(now - g_playerPreDeathShieldLastLog) >= std::chrono::milliseconds(1200)) {
			g_playerPreDeathShieldLastLog = now;
			spdlog::info(
				"[TFD][PlayerDamageGuard][P11] player pre-death shield maintained hpPct={:.1f} threshold={:.1f} inCombat={} noUndetect=1",
				hpPct,
				thresholdPct,
				player->IsInCombat() ? 1 : 0);
		}
	}

	void SetKillmoveGuard(bool enable, const char* reason, std::chrono::milliseconds hold)
	{
		const auto context = ResolveContext();
		auto* player = context.getPlayer ? context.getPlayer() : RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}

		auto& boolFlags = player->GetActorRuntimeData().boolFlags;
		if (enable) {
			const auto until = Now() + hold;
			if (g_playerKillmoveGuardUntil.time_since_epoch().count() == 0 || until > g_playerKillmoveGuardUntil) {
				g_playerKillmoveGuardUntil = until;
			}

			if (!g_playerKillmoveGuardActive) {
				g_playerKillmoveWasEssential = boolFlags.all(RE::Actor::BOOL_FLAGS::kEssential);
				g_playerKillmoveWasProtected = boolFlags.all(RE::Actor::BOOL_FLAGS::kProtected);
				g_playerKillmoveWasCanSpeakEssentialDown = boolFlags.all(RE::Actor::BOOL_FLAGS::kCanSpeakToEssentialDown);
				g_playerKillmoveGuardActive = true;
				spdlog::info(
					"[TFD][PlayerDamageGuard][P10] player killmove guard enabled reason={} essentialWas={} protectedWas={} canSpeakWas={}",
					reason ? reason : "-",
					g_playerKillmoveWasEssential ? 1 : 0,
					g_playerKillmoveWasProtected ? 1 : 0,
					g_playerKillmoveWasCanSpeakEssentialDown ? 1 : 0);
			}

			boolFlags.set(RE::Actor::BOOL_FLAGS::kEssential);
			boolFlags.set(RE::Actor::BOOL_FLAGS::kProtected);
			boolFlags.set(RE::Actor::BOOL_FLAGS::kCanSpeakToEssentialDown);
			boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
			return;
		}

		if (!g_playerKillmoveGuardActive) {
			return;
		}

		// Do not weaken the player while stronger hard immunity owns the same flags.
		// The guard will be restored after the hard immune owner exits.
		if (TFD::PlayerDamageGuard::IsHardImmunityActive()) {
			boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
			return;
		}

		if (!g_playerKillmoveWasEssential) {
			boolFlags.reset(RE::Actor::BOOL_FLAGS::kEssential);
		}
		if (!g_playerKillmoveWasProtected) {
			boolFlags.reset(RE::Actor::BOOL_FLAGS::kProtected);
		}
		if (!g_playerKillmoveWasCanSpeakEssentialDown) {
			boolFlags.reset(RE::Actor::BOOL_FLAGS::kCanSpeakToEssentialDown);
		}
		boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);

		g_playerKillmoveGuardActive = false;
		g_playerKillmoveWasEssential = false;
		g_playerKillmoveWasProtected = false;
		g_playerKillmoveWasCanSpeakEssentialDown = false;
		g_playerKillmoveGuardUntil = {};
		g_playerKillmoveGuardLastLog = {};
		spdlog::info("[TFD][PlayerDamageGuard][P10] player killmove guard released reason={}", reason ? reason : "-");
	}

	bool IsKillmoveGuardActive()
	{
		return g_playerKillmoveGuardActive;
	}

	bool TryQueueKillmoveBlockedBleedout(RE::Actor* player, RE::Actor* attacker, const char* reason)
	{
		const auto context = ResolveContext();
		auto* expectedPlayer = context.getPlayer ? context.getPlayer() : RE::PlayerCharacter::GetSingleton();
		if (!player || player != expectedPlayer || player->IsDisabled()) {
			return false;
		}
		if (HasPlayerBleedLock(context) || IsInBleedState(context) || (context.hasPendingOverkillRoute ? context.hasPendingOverkillRoute() : TFD::PlayerDownRouter::HasPendingOverkillRoute())) {
			player->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
			return false;
		}

		auto& boolFlags = player->GetActorRuntimeData().boolFlags;
		const bool wasKillMove = boolFlags.all(RE::Actor::BOOL_FLAGS::kIsInKillMove);
		if (!wasKillMove) {
			return false;
		}

		const float thresholdPct = context.getDefeatThresholdPct ? std::clamp(context.getDefeatThresholdPct(), 2.0f, 95.0f) : std::clamp(TFD::Settings::GetDefeatThresholdPct(), 2.0f, 95.0f);
		const float hpNow = (std::max)(0.0f, player->GetActorValue(RE::ActorValue::kHealth));
		const float safeFloorHp = ResolveSafeFloorHealth(player, thresholdPct);

		if (context.setPlayerBleedImmune) {
			context.setPlayerBleedImmune(true);
		}
		boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
		if (context.clampHealth) {
			context.clampHealth(player, (std::max)(hpNow, safeFloorHp));
		}

		if (attacker && attacker != player && !IsObserverAlly(context, attacker)) {
			if (context.rememberAggressor) {
				context.rememberAggressor(attacker);
			}
			if (context.noteEnemyTargetingPlayer) {
				context.noteEnemyTargetingPlayer(attacker);
			}
		}

		TFD::PlayerDownRouter::QueueOverkillRoute(
			attacker,
			thresholdPct,
			hpNow,
			0.0f,
			0.0f,
			0.0f,
			safeFloorHp,
			reason ? reason : "killmove_blocked_bleedout");

		spdlog::warn(
			"[TFD][PlayerDamageGuard][P10] player killmove attempt blocked queued bleedout attacker={:08X} hpBefore={:.2f} safeFloor={:.2f} threshold={:.1f} reason={}",
			attacker ? attacker->GetFormID() : 0u,
			hpNow,
			safeFloorHp,
			thresholdPct,
			reason ? reason : "-");
		return true;
	}

	void TickKillmoveSuppression()
	{
		const auto context = ResolveContext();
		auto* player = context.getPlayer ? context.getPlayer() : RE::PlayerCharacter::GetSingleton();
		if (!player || player->IsDisabled()) {
			SetKillmoveGuard(false, "invalid_player");
			return;
		}

		const bool threatActive =
			player->IsInCombat() ||
			(context.hasPendingOverkillRoute ? context.hasPendingOverkillRoute() : TFD::PlayerDownRouter::HasPendingOverkillRoute()) ||
			HasPlayerBleedLock(context) ||
			IsInBleedState(context) ||
			HasRecentEnemyTargetingPlayer(context, 2.50);

		if (threatActive) {
			SetKillmoveGuard(true, "active_player_threat", std::chrono::milliseconds(1800));
		}
		else if (g_playerKillmoveGuardActive &&
			g_playerKillmoveGuardUntil.time_since_epoch().count() != 0 &&
			Now() >= g_playerKillmoveGuardUntil) {
			SetKillmoveGuard(false, "threat_expired");
		}

		if (g_playerKillmoveGuardActive) {
			auto& boolFlags = player->GetActorRuntimeData().boolFlags;
			if (boolFlags.all(RE::Actor::BOOL_FLAGS::kIsInKillMove)) {
				auto* attacker = context.resolveCachedAttacker ? context.resolveCachedAttacker() : nullptr;
				(void)TryQueueKillmoveBlockedBleedout(player, attacker, "killmove_flag_tick");
			}
			else {
				boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
			}
		}
	}

	float ResolveSafeFloorHealth(RE::Actor* actor, float thresholdPct)
	{
		if (!actor) {
			return 1.0f;
		}
		const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
		const float threshold = std::clamp(thresholdPct, 2.0f, 95.0f);
		const float floorPct = std::clamp((std::max)(5.0f, threshold + 1.0f), 5.0f, 95.0f);
		return (std::max)(1.0f, hpMax * (floorPct / 100.0f));
	}

	void SetContext(Context context)
	{
		std::scoped_lock lock(g_contextLock);
		g_context = std::move(context);
		g_hasContext = true;
	}

	void ClearContext()
	{
		SetKillmoveGuard(false, "clear_context");
		ClearPreDeathShield("clear_context", false);
		std::scoped_lock lock(g_contextLock);
		g_context = {};
		g_hasContext = false;
	}

	bool HasContext()
	{
		std::scoped_lock lock(g_contextLock);
		return g_hasContext;
	}

	void Install()
	{
		CheckClampDamageModifierHook::Install();
		AttackActionKillmoveVetoHook::Install();
	}
}
