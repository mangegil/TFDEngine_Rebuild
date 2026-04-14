	void Install()
	{
		if (g_installed.exchange(true, std::memory_order_acq_rel)) return;
		g_running.store(true, std::memory_order_release);
		g_loadTransition.store(false, std::memory_order_release);
		TFD::Captive::ResetForLoad();
		ClearCaptiveOrchestrationResidue(false);
		SetPlayerBleedImmune(false);
		ResetBleedRuntimeState();
		TFD::FlowController::Controller::GetSingleton().ResetRuntime("defeat_install");
		TFD::Bleedout::Builders::InstallPendingSystemEventProvider({
			[](const char* r) { ClearBleedDialogueOutcome(r); },
			[](const char* r) { TFD::Bleedout::ClearSystemEventOutcomeWindow(r); },
			[](const char* r) { auto completion = TFD::Bleedout::Builders::BuildPayReleaseCompletionHandlers(); (void)TFD::Bleedout::CompletePayRelease(r, completion); },
			[]() { TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::FlowHandoff); },
			[]() -> bool { return TFD::Transition::ResolveCaptiveMarkerForOutcome(TFD::Transition::DefeatGlue::BuildTransitionRuntimeHandlers()); },
			[]() { (void)TFD::Bleedout::DoBlackoutTeleport("blackout_teleport", TFD::Bleedout::Builders::BuildBlackoutHandlers()); },
			[](int seconds) { SetGraceSeconds(seconds); },
			[](const char* r) { TFD::Tame::ReleaseBleedNoSpeakerTameSession(r); },
			[]() { ExitBleedSystemEventRuntime(); },
			[](const char* r) { (void)TFD::Bleedout::EnterNonCaptiveChoice(r, TFD::Bleedout::Builders::BuildNonCaptiveChoiceHandlers()); }
		});
		TFD::Bleedout::Builders::InstallDialogueCloseProvider({
			[](const char* r) { ClearBleedDialogueOutcome(r); },
			[](const char* r) { auto completion = TFD::Bleedout::Builders::BuildPayReleaseCompletionHandlers(); (void)TFD::Bleedout::CompletePayRelease(r, completion); }
		});
		TFD::Bleedout::Builders::InstallTimeoutProvider({
			[]() { TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::Generic); },
			[]() -> bool { return TFD::Transition::ResolveCaptiveMarkerForOutcome(TFD::Transition::DefeatGlue::BuildTransitionRuntimeHandlers()); },
			[]() { ResetBleedRuntimeState(); },
			[]() { (void)TFD::Bleedout::DoBlackoutTeleport("blackout_teleport", TFD::Bleedout::Builders::BuildBlackoutHandlers()); },
			[](int seconds) { SetGraceSeconds(seconds); },
			[](const char* r) { (void)TFD::Bleedout::EnterNonCaptiveChoice(r, TFD::Bleedout::Builders::BuildNonCaptiveChoiceHandlers()); }
		});
		TFD::Bleedout::Builders::InstallBaseCompletionProvider({
			[](const char* r) { TFD::Bleedout::ClearSystemEventOutcomeWindow(r); },
			[](BleedTerminalCommit kind, const char* r) { return TFD::Bleedout::TryBeginTerminalCommit(kind, r); },
			[]() { TFD::Transition::ClearPendingFadeIn(); },
			[](const char* r) { TFD::Bleedout::ClearBridgeAliases(nullptr, r); },
			[]() { TFD::Captive::ClearEscapeContext(); },
			[]() { TFD::Captive::ResetLockpickWatch(); },
			[](bool active) { g_grace.store(active, std::memory_order_release); },
			[](bool captive) { TFD::Captive::SetRuntimeState(captive, captive ? CaptivePhaseValue::Captive : CaptivePhaseValue::None); },
			[](bool immune) { SetPlayerBleedImmune(immune); },
			[]() { TFD::Transition::RecoverPlayerForTransition(TFD::Transition::DefeatGlue::BuildTransitionRuntimeHandlers()); },
			[]() { return TFD::Settings::GetSweepRadius(); },
			[](float radius) { ApplyCalmBubble(radius); },
			[]() { UpdatePreCombatState(); },
			[]() -> RE::Actor* { return ResolveBleedRuntimeSpeaker(); },
			[](RE::Actor* speaker, bool captive, const char* why) { BeginBleedPleasureRuntime(speaker, captive, why); },
			[](bool v) { g_prevDialogueOpen = v; },
			[](bool v) { TFD::Captive::SetPrevLockpickOpen(v); }
		});
		TFD::Bleedout::Builders::InstallCaptivePleasureCompletionExtras({
			[]() { g_lastAggressor.reset(); },
			[](bool preserve) { ResetBleedRuntimeState(preserve); },
			[](const char* r) { TFD::Captive::SyncPlayerAlias(Player(), r); }
		});
		TFD::Bleedout::Builders::InstallPayReleaseCompletionExtras({
			[]() { TFD::Actor::Ops::ClearAggressorFactionContext(); },
			[]() { g_lastAggressor.reset(); },
			[](bool preserve) { if (preserve) { ResetBleedRuntimeState(true); } else { ResetBleedRuntimeState(); } },
			[](int secs) { TFD::Transition::BeginLeftForDeadCooldown(secs); },
			[](int secs) { SetGraceSeconds(secs); }
		});
		TFD::Bleedout::Builders::InstallBleedPleasureCompletionExtras({
			[](const char* r) { TransitionBleedRuntimeToPleasureCommit(r); g_lastRouterCombatContextActive = false; },
			[](int secs) { TFD::Transition::BeginLeftForDeadCooldown(secs); },
			[](int secs) { SetGraceSeconds(secs); },
			[]() { RefreshPostDefeatGlobals(); }
		});
		TFD::Transition::DefeatGlue::InstallProviders(
			TFD::Bleedout::Builders::NonCaptiveChoiceProvider{
				[]() { const auto activeCommit = TFD::Bleedout::GetTerminalCommit(); return activeCommit != BleedTerminalCommit::None && activeCommit != BleedTerminalCommit::NonCaptiveFallback; },
				[]() { return TFD::Bleedout::GetTerminalCommitName(TFD::Bleedout::GetTerminalCommit()); },
				{},
				[](const char* r) { TFD::Bleedout::ClearBridgeAliases(nullptr, r); },
				[]() { TFD::Transition::ClearPendingFadeIn(); },
				[]() { TFD::Actor::Ops::ClearAggressorFactionContext(); },
				[]() { TFD::Captive::ClearEscapeContext(); },
				[]() { TFD::Captive::ResetLockpickWatch(); },
				[](bool active) { g_grace.store(active, std::memory_order_release); },
				[]() { g_lastAggressor.reset(); },
				[]() { ResetBleedRuntimeState(); },
				[](bool v) { g_prevDialogueOpen = v; },
				[](bool v) { TFD::Captive::SetPrevLockpickOpen(v); },
				[](bool captive) { TFD::Captive::SetRuntimeState(captive, captive ? CaptivePhaseValue::Captive : CaptivePhaseValue::None); },
				[](bool immune) { SetPlayerBleedImmune(immune); },
				[](const char* r) { QueueNonCaptiveChoiceRequest(r); },
				[](const char* r) { TFD::FlowController::Controller::GetSingleton().ResetRuntime(r ? r : "noncaptive_choice"); }
			},
			TFD::Bleedout::Builders::BlackoutProvider{
				[]() { const auto activeCommit = TFD::Bleedout::GetTerminalCommit(); return activeCommit != BleedTerminalCommit::None && activeCommit != BleedTerminalCommit::Captive; },
				[]() { return TFD::Bleedout::GetTerminalCommitName(TFD::Bleedout::GetTerminalCommit()); },
				[]() { return TFD::Transition::ResolveCaptiveMarkerForOutcome(TFD::Transition::DefeatGlue::BuildTransitionRuntimeHandlers()); },
				[](const char* r) { (void)TFD::Bleedout::EnterNonCaptiveChoice(r, TFD::Bleedout::Builders::BuildNonCaptiveChoiceHandlers()); },
				[](const char* r) -> bool { const auto activeCommit = TFD::Bleedout::GetTerminalCommit(); if (activeCommit == BleedTerminalCommit::None) { return TFD::Bleedout::TryBeginTerminalCommit(BleedTerminalCommit::Captive, r); } return true; },
				[]() { ResetBleedRuntimeState(); },
				[](const char* r) { TFD::Bleedout::ClearBridgeAliases(nullptr, r); },
				[]() { g_lastAggressor.reset(); },
				[]() { RE::DebugNotification("TFDEngine: Blackout -> Captive (1h)"); (void)TFD::FlowController::ApplyObservedDefeatResolution(TFD::FlowController::ObservedDefeatInput{ false, false, true, true, true, true, true, 0u }, "bleed_blackout_teleport"); },
				[]() { TFD::Transition::ClearPendingFadeIn(); },
				[]() { return TFD::Transition::QueueRequest(TFD::Transition::Kind::Captive, false, "captive_blackout"); },
				[]() { TFD::Transition::ShowBlackoutFader(); },
				[](const char* r) { if (!TFD::Transition::CompleteCaptiveTransitionNow(r, TFD::Transition::DefeatGlue::BuildTransitionRuntimeHandlers(), TFD::Transition::DefeatGlue::BuildTransitionCaptiveHandlers())) { (void)TFD::Bleedout::EnterNonCaptiveChoice("teleport_failed", TFD::Bleedout::Builders::BuildNonCaptiveChoiceHandlers()); } },
				[]() { TFD::Transition::HideBlackoutFader(); }
			},
			TFD::Bleedout::Builders::TransitionRuntimeProvider{
				[]() -> RE::Actor* { return Player(); },
				[]() -> RE::Actor* { return ResolveAggressor(); },
				[](float radius) -> RE::Actor* { return FindBestAggressor(radius); },
				[](RE::Actor* actor) { return IsCombatSupportedAggressor(actor); },
				[](RE::Actor* actor) { return IsActiveFollowerActor(actor); },
				[](RE::Actor* actor) { return IsStandingAllyThresholdActor(actor); },
				[]() { return TFD::TeammateManager::CollectRegisteredTeammates(); },
				[](float radius, RE::Actor* preferred, bool preserveAssigned) { return CollectBleedoutCrowd(radius, preferred, preserveAssigned); },
				[]() { std::vector<RE::FormID> ids{}; const auto assigned = TFD::Bleedout::GetBleedCrowdAssignedIDs(); ids.reserve(assigned.size()); for (auto id : assigned) { ids.push_back(static_cast<RE::FormID>(id)); } return ids; },
				[](float radius) { return TryAbortPleasureDueToHostileIntrusion(radius); },
				[](const char* reason, bool playGetUp) { ReleasePlayerBleedLock(reason, playGetUp); },
				[](int secs) { SetGraceSeconds(secs); },
				[](int value) { SetRescueStateValue(value); },
				[]() { RefreshPostDefeatGlobals(); },
				[]() { UpdatePreCombatState(); }
			},
			TFD::Bleedout::Builders::TransitionCaptiveProvider{
				[]() { ResetBleedRuntimeState(); },
				[](const char* reason) { TFD::Bleedout::ClearBridgeAliases(nullptr, reason); },
				[]() { g_lastAggressor.reset(); },
				[](const char* why) { RE::DebugNotification("TFDEngine: Blackout -> Captive (1h)"); (void)TFD::FlowController::ApplyObservedDefeatResolution(TFD::FlowController::ObservedDefeatInput{ false, false, true, true, true, true, true, 0u }, why ? why : "captive_enter"); },
				[]() { TFD::Captive::SetRuntimeState(true, CaptivePhaseValue::Captive); },
				[]() { return IsDialogueOpen(); },
				[](bool v) { g_prevDialogueOpen = v; },
				[]() { TFD::Captive::CaptureCurrentLockpickMenuState(); },
				[]() { TFD::Captive::ResetLockpickWatch(); },
				[]() { TFD::Captive::ArmEscapeContextFromCurrentState(Player()); },
				[]() { TFD::Captive::SealDoorIfPresent(); },
				[](float radius) { ApplyCalmBubble(radius); },
				[](const char* reason, bool starterKitWanted) { TFD::Captive::QueuePendingConfiscation(reason, starterKitWanted); },
				[](RE::Actor* actor, const char* reason) { TFD::Captive::SyncPlayerAlias(actor, reason); }
			},
			TFD::Transition::DefeatGlue::RuntimeProviders{
				[](BleedTerminalCommit kind, const char* r) { return TFD::Bleedout::TryBeginTerminalCommit(kind, r); },
				[]() { ClearCaptiveOrchestrationResidue(); },
				[]() -> RE::Actor* { return Player(); },
				[](const char* r) { TFD::Bleedout::ClearBridgeAliases(nullptr, r); },
				[](bool immune) { SetPlayerBleedImmune(immune); },
				[]() { ResetBleedRuntimeState(); },
				[]() { g_lastAggressor.reset(); },
				[]() { UpdatePreCombatState(); }
			}
		);

		TFD::Bleedout::RuntimeHost::InstallProvider({
			&g_inBleedState,
			&g_minHp,
			&g_bleedStart,
			&g_bleedLastSeconds,
			&g_bleedPaused,
			&g_bleedPauseStarted,
			&g_bleedLastCalmPulse,
			[]() { return TFD::Captive::HasEscapeBreakRebleedPending(); },
			[](bool pending) { TFD::Captive::SetEscapeBreakRebleedPending(pending); },
			&g_bleedPendingCaptiveOutcome,
			&g_bleedPendingNonCaptiveOutcome,
			&g_bleedBattleObservePending,
			&g_bleedBattleObservePendingUntil,
			&g_bleedBattleObservePendingLastRedirect,
			&g_bleedBattleObservePendingEmptyEnemyTicks,
			&g_bleedBattleObserveActive,
			&g_bleedBattleObserveSince,
			&g_bleedBattleObserveLastRedirect,
			&g_bleedBattleObserveActiveEmptyEnemyTicks,
			[]() { return BuildLocalBleedRuntimeHostHandlers(); },
			[]() { return BuildBleedDialogueHotkeyHandlers(); },
			[]() -> RE::Actor* { return Player(); },
			[]() { return (std::max)(2400.0f, TFD::Settings::GetSweepRadius()); },
			[]() { return 1800.0f; }
		});

		TFD::Bleedout::DefeatGlue::InstallProvider({
			&g_grace,
			&g_graceUntil,
			&g_prevDialogueOpen,
			[]() { TFD::Transition::ClearPendingFadeIn(); },
			[]() { TFD::Captive::ClearEscapeContext(); },
			[]() { TFD::Captive::ResetLockpickWatch(); },
			[](bool open) { TFD::Captive::SetPrevLockpickOpen(open); },
			[]() { TFD::Captive::SetRuntimeState(false, CaptivePhaseValue::None); },
			[]() -> RE::Actor* { return Player(); },
			[](const char* reason, bool playGetUp) { ReleasePlayerBleedLock(reason, playGetUp); },
			[]() { return CurrentBleedSpeakerID(); },
			[]() -> RE::Actor* { return CurrentBleedSpeaker(); },
			[]() { return BuildBleedoutSpeakerHandlers(); },
			[]() { return IsDialogueOpen(); },
			[]() -> RE::Actor* { return ResolveAggressor(); },
			[](float radius) -> RE::Actor* { return FindBestAggressor(radius); },
			[](const char* reason) { TFD::Tame::ReleaseBleedNoSpeakerTameSession(reason); },
			[]() { TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::Generic); },
			[](RE::Actor* player, RE::Actor* speaker, const char* reason) { return TFD::HostilityController::StartBleedTruceSessionForSpeaker(player, speaker, reason); },
			[]() { ResetBleedSpeakerKickState(); },
			[](const char* reason) { TFD::BleedoutGreet::ResetRuntime(reason); },
			[](RE::Actor* actor, const char* reason) { (void)TFD::BleedoutGreet::Begin(actor, reason); },
			[](const char* reason) { ClearBleedSupportBridgeAliases(reason); },
			[]() {
				g_bleedBattlePreferredEnemy.reset();
				g_bleedBattleObserver = {};
			},
			[]() { return g_observedCombatCommitDepth > 0; },
			[](RE::Actor* actor) { NoteEnemyTargetingPlayerInternal(actor); },
			[](bool immune) { SetPlayerBleedImmune(immune); },
			[](RE::Actor* actor, float minHp) { ClampHealth(actor, minHp); },
			[](float radius) { return CollectBleedStandingFollowers(radius); },
			[](RE::Actor* player, const std::vector<RE::Actor*>& allies, float baseRadius) { return TFD::Bleedout::DefeatGlue::ComputeObservedEnemyScanRadius(player, allies, baseRadius); },
			[](float radius, double maxAgeSec) -> RE::Actor* { return ResolveLastEnemyTargetingPlayerInternal(radius, maxAgeSec); },
			[](RE::Actor* actor) { return IsObserverAlly(actor); },
			[](RE::Actor* player, float radius, RE::Actor* preferred, const std::vector<RE::Actor*>& allies) { return TFD::Bleedout::DefeatGlue::CollectCurrentObservedEnemies(player, radius, preferred, allies); },
			[](RE::Actor* player, const std::vector<RE::Actor*>& followers, const std::vector<RE::Actor*>& enemies, RE::Actor* preferred) { TFD::Bleedout::DefeatGlue::UpdateObservedBattleRoster(player, followers, enemies, preferred); },
			[]() { return CollectBleedStandingFollowersFromSnapshot(); },
			[]() { return CollectBleedStandingEnemiesFromSnapshot(); },
			[]() { return TFD::Bleedout::DefeatGlue::HadValidObservedEnemy(); },
			[]() { EnterObservedBattleWin(); },
			[](const char* reason) { EnterObservedLeftForDead(reason); },
			[](float radius, float maxDist, RE::Actor* preferred) { return FindBestBleedoutSpeaker(radius, maxDist, preferred); },
			[](RE::Actor* actor, RE::Actor* player, float maxDist, float* outDistance) { return IsReasonableBleedoutSpeaker(actor, player, maxDist, outDistance); },
			[](float radius, RE::Actor* preferred, bool preserveAssigned) { return CollectBleedoutCrowd(radius, preferred, preserveAssigned); },
			[](RE::Actor* actor) { return IsCaptiveSupportedAggressor(actor); },
			[](RE::Actor* actor) { return TFD::Actor::Ops::ApplyAggressorFactionContext(actor); },
			[]() { return TFD::Transition::ResolveCaptiveMarkerForOutcome(BuildTransitionRuntimeHandlers()); },
			[](RE::Actor* player, RE::Actor* aggressor, bool hasCaptiveOutcome, float* outDistance) {
				const float maxDist = hasCaptiveOutcome ? 1200.0f : 900.0f;
				return player && aggressor && IsCaptiveSupportedAggressor(aggressor) && IsReasonableCombatAggressor(aggressor, player, maxDist, outDistance);
			},
			[](const std::vector<RE::Actor*>& actors, const char* reason) {
				auto* player = Player();
				return player && TFD::Bleedout::TryEnsureNoSpeakerTameSession(actors, player, reason, TFD::Bleedout::RuntimeHost::BuildHandlers());
			},
			[](RE::Actor* actor) { g_lastAggressor = actor ? actor->GetHandle() : RE::ActorHandle{}; },
			[](RE::Actor* actor) { return IsBleedCrowdSupportedAggressor(actor); },
			[](RE::Actor* actor, RE::Actor* player) { return IsBleedSpaceCompatible(actor, player); },
			[](const char* msg) { if (msg && msg[0]) RE::DebugNotification(msg); },
			[](RE::Actor* player, float radius, const char* reason) { ClearEnemyTargetsToPlayerForDefeat(player, radius, reason); },
			[](float radius) -> RE::Actor* { return TFD::Captive::ResolveEscapeBreakPreferredAggressor(radius, [](float fallbackRadius) { return FindBestAggressor(fallbackRadius); }); },
			[](RE::Actor* player, RE::Actor* aggressor, float* outDistance) {
				if (!player || !aggressor) {
					if (outDistance) *outDistance = -1.0f;
					return false;
				}
				float dist = -1.0f;
				const bool ok = CanUseAggressorForBleedoutGreet(player, aggressor, dist);
				if (outDistance) *outDistance = dist;
				return ok;
			},
			[](RE::Actor* player, RE::Actor* speaker, const char* reason, bool restartDialogue) { ApplyBleedDialogueOverdrive(player, speaker, reason, restartDialogue); },
			[](RE::Actor* actor) { return IsStandingAllyThresholdActor(actor); },
			[](RE::Actor* actor) { return IsStandingEnemyThresholdActor(actor); }
		});
		TFD::FlowController::InstallPassiveRuntimeProviders(TFD::FlowController::PassiveRuntimeProviders{
			[]() { return g_inBleedState.load(std::memory_order_acquire); },
			[]() { return TFD::Actor::Ops::HasAnyReleaseFollowGrace(); },
			[]() -> RE::Actor* { return ResolveCurrentPassivePrimaryActor(); },
			[](RE::Actor* actor) { return IsActorCoveredByCurrentPassiveContext(actor); },
			[](RE::Actor* actor, const char* reason) { TFD::Actor::Ops::CancelReleaseFollowGraceFromPlayerAggression(actor, reason); },
			[](const char* reason) { TFD::Actor::Ops::ClearAllReleaseFollowGrace(reason); },
			[]() { TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::PlayerAggression); },
			[](const char* reason) { ClearAllBleedLocks(reason); },
			[](RE::Actor* actor, const char* reason) { TFD::Bleedout::ClearBridgeAliases(actor, reason); },
			[]() { ClearPendingDefeatedDialogueTargetInternal(); }
		});
		TFD::FlowController::InstallOutcomeRuntimeProviders(TFD::FlowController::OutcomeRuntimeProviders{
			[](RE::Actor* actor, double seconds, const char* reason) { TFD::Actor::Ops::ApplyReleaseFollowGraceToSpeakerAndCrowd(actor, seconds, reason); },
			[](RE::Actor* actor, const char* reason) { TFD::Actor::Ops::RemoveReleaseFollowGraceFromSpeakerAndCrowd(actor, reason); },
			[]() { return ResolveBleedFlowActorFormID(); },
			[]() { return g_inBleedState.load(std::memory_order_acquire); },
			[](const char* reason) { PreparePlayerForCaptivePleasureScene(reason); },
			[](const char* reason) { auto completion = BuildCaptivePleasureCompletionHandlers(); (void)TFD::Bleedout::CompleteCaptivePleasureHandoff(reason, completion); },
			[](const char* reason) { PreparePlayerForBleedoutPleasureScene(reason); },
			[](const char* reason) { auto completion = BuildBleedPleasureCompletionHandlers(); (void)TFD::Bleedout::CompleteBleedPleasureHandoff(reason ? reason : "bleed_pleasure_handoff", completion); }
		});
		TFD::TeammateManager::RuntimeProviders teammateRuntimeProviders{};
		teammateRuntimeProviders.hasAllyBleedLock = [](RE::Actor* actor) {
			auto it = g_bleedLocks.find(actor ? actor->GetFormID() : 0u);
			return actor && it != g_bleedLocks.end() && it->second.kind == BleedLockKind::Ally;
		};
		teammateRuntimeProviders.isBleedingOutActor = [](RE::Actor* actor) { return IsActorBleedingOut(actor); };
		teammateRuntimeProviders.isDialogueCapableDefeatedEnemy = [](RE::Actor* actor) { return TFD::Actor::Ops::IsDialogueCapableDefeatedEnemy(actor); };
		teammateRuntimeProviders.getDefeatedEnemyRemainingSeconds = [](RE::Actor* actor) { return TFD::Actor::Ops::GetDefeatedEnemyRemainingSeconds(actor); };
		teammateRuntimeProviders.suppressDefeatedReentry = [](RE::Actor* actor, double seconds, const char* reason) { TFD::Actor::Ops::SuppressDefeatedEnemyReentry(actor, seconds, reason); };
		teammateRuntimeProviders.releaseBleedLock = [](RE::Actor* actor, const char* reason, bool playGetUp) { ReleaseBleedLock(actor, reason, playGetUp); };
		teammateRuntimeProviders.restoreActorHealthToSafePct = [](RE::Actor* actor, float thresholdPct, float bonusPct, float minSafePct, float maxSafePct, float minAbsHp, const char* reason) { RestoreActorHealthToSafePct(actor, thresholdPct, bonusPct, minSafePct, maxSafePct, minAbsHp, reason); };
		teammateRuntimeProviders.resolvePendingDefeatedDialogueTarget = []() -> RE::Actor* { return ResolvePendingDefeatedDialogueTargetInternal(); };
		teammateRuntimeProviders.clearPendingDefeatedDialogueTarget = []() { ClearPendingDefeatedDialogueTargetInternal(); };
		teammateRuntimeProviders.setPendingDefeatedDialogueTarget = [](RE::Actor* actor) { TFD::TeammateManager::SetPendingDefeatedDialogueTarget(actor); };
		teammateRuntimeProviders.reviveDownedAlly = [](RE::Actor* actor, float targetHealthPct) { return TFD::TeammateManager::ReviveDownedAlly(actor, targetHealthPct); };
		TFD::TeammateManager::InstallRuntimeProviders(std::move(teammateRuntimeProviders));
		TFD::HostilityController::InstallBleedTruceRuntimeProviders(TFD::HostilityController::BleedTruceRuntimeProviders{
			[](RE::Actor* player, RE::Actor* speaker, const char* reason) {
				return TFD::Bleedout::StartTruceSessionForSpeaker(player, speaker, reason, TFD::Bleedout::RuntimeHost::BuildStateRefs(), TFD::Bleedout::RuntimeHost::BuildHandlers());
			},
			[](TFD::Tame::ReleaseReason reason) {
				TFD::Bleedout::ReleaseTruceSession(reason);
			}
		});
		TFD::Tame::RuntimeProviders tameRuntimeProviders{};
		tameRuntimeProviders.isCreatureDefeatedEnemy = [](RE::Actor* actor) { return TFD::Actor::Ops::IsCreatureDefeatedEnemy(actor); };
		tameRuntimeProviders.getDefeatedEnemyRemainingSeconds = [](RE::Actor* actor) { return TFD::Actor::Ops::GetDefeatedEnemyRemainingSeconds(actor); };
		tameRuntimeProviders.suppressDefeatedReentry = [](RE::Actor* actor, double seconds, const char* reason) { TFD::Actor::Ops::SuppressDefeatedEnemyReentry(actor, seconds, reason); };
		tameRuntimeProviders.releaseBleedLock = [](RE::Actor* actor, const char* reason, bool playGetUp) { ReleaseBleedLock(actor, reason, playGetUp); };
		tameRuntimeProviders.restoreActorHealthToSafePct = [](RE::Actor* actor, float thresholdPct, float bonusPct, float minSafePct, float maxSafePct, float minAbsHp, const char* reason) { RestoreActorHealthToSafePct(actor, thresholdPct, bonusPct, minSafePct, maxSafePct, minAbsHp, reason); };
		tameRuntimeProviders.releaseBleedNoSpeakerTameSession = [](const char* reason) { TFD::Bleedout::ReleaseNoSpeakerTameSession(reason); };
		tameRuntimeProviders.tryEnsureBleedNoSpeakerTameSession = [](const std::vector<RE::Actor*>& actors, const char* reason) {
			auto* player = Player();
			if (!player) {
				return false;
			}
			return TFD::Bleedout::TryEnsureNoSpeakerTameSession(actors, player, reason, TFD::Bleedout::RuntimeHost::BuildHandlers());
		};
		TFD::Tame::InstallRuntimeProviders(std::move(tameRuntimeProviders));
		TFD::FlowController::InstallBattleObserverRuntimeProviders(TFD::FlowController::BattleObserverRuntimeProviders{
			[](const char* reason) { TFD::Bleedout::ClearBridgeAliases(nullptr, reason); },
			[]() { ClearCaptiveOrchestrationResidue(); },
			[]() { TFD::Actor::Ops::ClearAggressorFactionContext(); },
			[]() { RecoverVictoryTeammates(); },
			[]() { ResetBleedRuntimeState(); },
			[]() { ClearPendingDefeatedDialogueTargetInternal(); },
			[](bool immune) { SetPlayerBleedImmune(immune); },
			[](const char* reason) { QueueNonCaptiveChoiceRequest(reason); },
			[]() -> RE::Actor* {
				const float followerRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 400.0f);
				auto followers = ResolveFollowerCandidates(followerRadius);
				return followers.downed;
			},
			[](RE::Actor* follower) { TFD::Transition::ArmObservedLeftForDeadFallback(follower, BuildTransitionRuntimeHandlers()); },
			[](const char* reason) { TFD::Transition::BeginRecoverTransition(reason ? reason : "battle_observe_loss", BuildTransitionRuntimeHandlers()); },
			[]() -> const char* { return TFD::Transition::GetCurrentFallbackBranchName(); }
		});
		TFD::PleasureRuntime::Install();
		g_lastRouterCombatContextActive = false;
		ClearAllBleedLocks("install");
		TFD::Bleedout::ClearBridgeAliases(nullptr, "install");
		TFD::Actor::Ops::Initialize();
		TFD::Actor::Ops::InstallDefeatedEnemyQueryHooks(TFD::Actor::Ops::DefeatedEnemyQueryHooks{
			&IsTrackedDefeatedEnemyHook,
			&IsLastAggressorHook
		});
		TFD::Location::Initialize();
		if (auto* scripts = RE::ScriptEventSourceHolder::GetSingleton()) {
			scripts->AddEventSink<RE::TESHitEvent>(&g_passiveBreakEventSink);
		}
		if (auto* src = SKSE::GetModCallbackEventSource()) {
			src->AddEventSink(&g_bleedOutcomeEventSink);
			src->AddEventSink(&g_passiveBreakEventSink);
		}
		ClearPendingDefeatedDialogueTargetInternal();
		g_worker = std::thread([]() { WorkerLoop(); });
		TFD::DefeatMonitor::ApplyQueuedProgressState();
		SetRescueStateValue(0);
		RefreshPostDefeatGlobals();
		spdlog::info("[TFD][Defeat] monitor installed");
	}

	void Shutdown()
	{
		if (!g_installed.exchange(false, std::memory_order_acq_rel)) return;
		g_running.store(false, std::memory_order_release);
		if (g_worker.joinable()) g_worker.join();
		ClearCaptiveOrchestrationResidue(false);
		g_hasQueuedProgressState = false;
		TFD::Captive::ClearQueuedLoadedState();
		g_queuedBleedOutState = false;
		SetPlayerBleedImmune(false);
		ResetBleedRuntimeState();
		TFD::FlowController::Controller::GetSingleton().ResetRuntime("defeat_shutdown");
		TFD::FlowController::ResetPassiveRuntimeProviders();
		TFD::FlowController::ResetOutcomeRuntimeProviders();
		TFD::FlowController::ResetBattleObserverRuntimeProviders();
		TFD::TeammateManager::ResetRuntimeProviders();
		TFD::Tame::ResetRuntimeProviders();
		TFD::HostilityController::ResetBleedTruceRuntimeProviders();
		g_lastRouterCombatContextActive = false;
		ClearAllBleedLocks("shutdown");
		TFD::Bleedout::ClearBridgeAliases(nullptr, "shutdown");
		g_loadTransition.store(false, std::memory_order_release);
		TFD::Transition::ClearLeftForDeadCooldown(BuildTransitionRuntimeHandlers());
		if (auto* scripts = RE::ScriptEventSourceHolder::GetSingleton()) {
			scripts->RemoveEventSink<RE::TESHitEvent>(&g_passiveBreakEventSink);
		}
		if (auto* src = SKSE::GetModCallbackEventSource()) {
			src->RemoveEventSink(&g_bleedOutcomeEventSink);
			src->RemoveEventSink(&g_passiveBreakEventSink);
		}
		TFD::PleasureRuntime::Shutdown();
		ClearPendingDefeatedDialogueTargetInternal();
		SetRescueStateValue(0);
		RefreshPostDefeatGlobals();
		TFD::Bleedout::Builders::Reset();
		TFD::Bleedout::RuntimeHost::Reset();
		TFD::Transition::DefeatGlue::Reset();
		spdlog::info("[TFD][Defeat] monitor shutdown");
	}

	void ResetGrace()
	{
		g_grace.store(false, std::memory_order_release);
		TFD::Transition::ClearLeftForDeadCooldown(BuildTransitionRuntimeHandlers());
		TFD::Actor::Ops::ClearAllReleaseFollowGrace("reset_grace");
	}

	bool HandlePassiveInvalidationAgainstActor(RE::Actor* actor, const char* reason)
	{
		return TFD::FlowController::HandlePassiveInvalidationAgainstActor(actor, reason, false);
	}

	bool GetCaptiveStateForSave()
	{
		return TFD::Captive::GetStateFlag();
	}

	std::uint32_t GetCaptivePhaseForSave()
	{
		return TFD::Captive::GetPhaseRaw();
	}

	bool GetBleedOutStateForSave()
	{
		if (auto* player = Player()) {
			return ComputePlayerBleedOutState(player);
		}
		return g_queuedBleedOutState;
	}

	void QueueLoadedBleedOutState(bool active)
	{
		g_queuedBleedOutState = active;
		spdlog::info("[TFD][Defeat] QueueLoadedBleedOutState state={}", active ? 1 : 0);
	}

	void QueueLoadedProgressState(bool stateActive, std::uint32_t phaseRaw)
	{
		g_hasQueuedProgressState = true;
		CaptivePhaseValue phase = stateActive ? PhaseFromRaw(phaseRaw) : CaptivePhaseValue::None;
		if (stateActive && phase == CaptivePhaseValue::None) phase = CaptivePhaseValue::Escape;
		TFD::Captive::QueueLoadedState(stateActive, phase);
		spdlog::info("[TFD][Defeat] QueueLoadedProgressState state={} phase={} normalized={}", stateActive ? 1 : 0, phaseRaw, static_cast<int>(TFD::Captive::GetQueuedPhase()));
	}

	void QueueDefaultProgressState()
	{
		g_hasQueuedProgressState = true;
		TFD::Captive::ClearQueuedLoadedState();
		g_queuedBleedOutState = false;
		spdlog::info("[TFD][Defeat] QueueDefaultProgressState");
	}

	bool HasQueuedProgressState()
	{
		return g_hasQueuedProgressState;
	}

	void ApplyQueuedProgressState()
	{
		if (!g_hasQueuedProgressState) QueueDefaultProgressState();
		TFD::Transition::ClearLeftForDeadCooldown(BuildTransitionRuntimeHandlers());
		TFD::Captive::SetRuntimeState(TFD::Captive::GetQueuedStateFlag(), TFD::Captive::GetQueuedPhase());
		g_prevDialogueOpen = IsDialogueOpen();
		TFD::Captive::CaptureCurrentLockpickMenuState();
		if (TFD::Captive::GetQueuedStateFlag() && TFD::Captive::GetQueuedPhase() == CaptivePhaseValue::Captive) {
			TFD::Location::RescanCaptiveMarker();
			TFD::Captive::ArmEscapeContextFromCurrentState(Player());
		}
		else {
			TFD::Captive::ResetLockpickWatch();
			TFD::Captive::ClearEscapeContext();
		}
		SetPlayerBleedImmune(false);
		TFD::Bleedout::ClearBridgeAliases(nullptr, "apply_queued_state");
		SetRescueStateValue(0);
		RefreshPostDefeatGlobals();
		UpdatePreCombatState();
		spdlog::info("[TFD][Defeat] ApplyQueuedProgressState state={} phase={} bleed={}", TFD::Captive::GetQueuedStateFlag() ? 1 : 0, static_cast<int>(TFD::Captive::GetQueuedPhase()), g_queuedBleedOutState ? 1 : 0);
	}

	void ResetForLoad()
	{
		g_grace.store(false, std::memory_order_release);
		SetPlayerBleedImmune(false);
		ResetBleedRuntimeState();
		ClearAllBleedLocks("reset_for_load");
		TFD::Bleedout::ClearBridgeAliases(nullptr, "reset_for_load");
		g_lastAggressor = RE::ActorHandle{};
		ClearLastEnemyTargetingPlayerInternal();
		ClearCaptiveOrchestrationResidue(false);
		TFD::Actor::Ops::ClearAggressorFactionContext();
		TFD::HostilityController::ClearAggressionClamp();
		TFD::Transition::ClearLeftForDeadCooldown(BuildTransitionRuntimeHandlers());
		SetRescueStateValue(0);
		g_lastRouterCombatContextActive = false;
		TFD::PleasureRuntime::ResetForLoad("defeat_reset_for_load");
		RefreshPostDefeatGlobals();
		spdlog::info("[TFD][Defeat] ResetForLoad -> runtime only");
	}

	void SetLoadTransition(bool active)
	{
		g_loadTransition.store(active, std::memory_order_release);
		if (active) {
			SetPlayerBleedImmune(false);
			ClearAllBleedLocks("set_load_transition");
			TFD::Bleedout::ClearBridgeAliases(nullptr, "set_load_transition");
			TFD::Captive::ResetLockpickWatch();
			TFD::PleasureRuntime::ResetForLoad("defeat_set_load_transition");
			spdlog::info("[TFD][Defeat] SetLoadTransition(true)");
		}
		else {
			spdlog::info("[TFD][Defeat] SetLoadTransition(false)");
		}
	}



