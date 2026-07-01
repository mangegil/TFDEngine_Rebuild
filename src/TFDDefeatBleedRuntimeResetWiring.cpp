#include "TFDDefeatBleedRuntimeResetWiring.h"

#include <cstdint>

#include "TFDBleedout.h"
#include "TFDBleedoutGreet.h"
#include "TFDBleedLockRuntime.h"
#include "TFDDefeatBleedLockWiring.h"
#include "TFDHostilityController.h"
#include "TFDTame.h"
#include "TFDDefeatBridge.h"
#include "TFDDefeatBleedRuntimeState.h"
#include "TFDDefeatBattleObserveState.h"
#include "TFDDefeatBleedoutRuntimeTimer.h"
#include "TFDCaptive.h"
#include "TFDDefeatAggressorResolver.h"
#include "TFDPleasureRuntime.h"
#include "TFDPlayerOverkillDamageHook.h"

namespace TFD::DefeatBleedRuntimeResetWiring
{
	void Reset(bool preserveCaptive)
	{
		TFD::Bleedout::RuntimeResetHandlers handlers{};
		handlers.releasePlayerBleedLock = [&](const char* reason) { TFD::BleedLockRuntime::ReleasePlayer(TFD::DefeatBleedLockWiring::BuildContext(), reason, false); };
		handlers.releaseBleedTruceSession = [&]() { TFD::HostilityController::ReleaseBleedTruceSession(TFD::Tame::ReleaseReason::Generic); };
		handlers.releaseNoSpeakerTameSession = [&](const char* reason) { TFD::Tame::ReleaseBleedNoSpeakerTameSession(reason); };
		handlers.clearBridgeAliases = [&](const char* reason) { TFD::DefeatBridge::ClearBleedSupportAliases(reason); };
		handlers.setBleedActive = [&](bool active, const char*) {
			TFD::DefeatBleedRuntimeState::SetInBleedState(active);
		};
		handlers.resetGreetRuntime = [&](const char* reason) { TFD::BleedoutGreet::ResetRuntime(reason); };
		handlers.resetSystemEventState = [&](const char* reason) { TFD::Bleedout::ResetSystemEventState(reason); };
		handlers.clearCaptorAliases = [&](const char* reason) { TFD::Bleedout::ClearCaptorAliases(reason); };
		handlers.clearDialogueOutcome = [&](const char* reason) { TFD::Bleedout::ClearDialogueOutcome(reason); };
		handlers.resetBattleObserveTracking = [&]() {
			TFD::DefeatBattleObserveState::ResetTracking();
		};
		handlers.resetDialogueRuntimeState = [&]() {
			TFD::DefeatBleedRuntimeState::ClearPendingOutcomes();
			TFD::DefeatBleedoutRuntimeTimer::ResetDialogueRuntimeClock();
			TFD::Bleedout::ResetBleedSpeakerKick();
			TFD::Bleedout::SetBleedDialogueRetryCount(0);
			TFD::Bleedout::BleedLastCrowdAssignRef() = {};
			TFD::Bleedout::ClearBleedCrowdAssigned();
			TFD::Bleedout::ClearBleedRejectedSpeakerIds();
		};
		handlers.resetBattleObserveState = [&]() {
			TFD::DefeatBattleObserveState::ResetFlags();
		};
		handlers.clearEscapeBreakState = [&]() { TFD::Captive::ClearEscapeBreakRebleed(); };
		handlers.clearLastEnemyTargetingPlayer = [&]() { TFD::DefeatAggressorResolver::ClearLastEnemyTargetingPlayer(); };
		handlers.clearOutcomeWindow = [&](const char* reason) { TFD::Bleedout::ClearSystemEventOutcomeWindow(reason); };
		handlers.resetPleasureRuntime = [&](const char* reason) { TFD::PleasureRuntime::ResetRuntime(reason); };

		TFD::Bleedout::ResetRuntimeState(preserveCaptive, "reset_bleed_runtime", handlers);
		TFD::PlayerOverkillDamageHook::SetKillmoveGuard(false, "reset_bleed_runtime");
	}

	void TransitionToPleasureCommit(const char* reason)
	{
		TFD::Bleedout::TransitionRuntimeToPleasureCommit(
			reason,
			static_cast<std::uint32_t>(TFD::Bleedout::GetBleedSpeakerID()),
			TFD::Bleedout::GetTruceSessionID() != 0,
			static_cast<std::uint32_t>(TFD::Bleedout::GetActiveCaptorFormID()),
			TFD::Bleedout::RuntimePleasureCommitHandlers{
				[&](const char* why) { TFD::Tame::ReleaseBleedNoSpeakerTameSession(why); },
				[&](const char* why) { TFD::DefeatBridge::ClearBleedSupportAliases(why); },
				[&](bool active, const char*) {
					TFD::DefeatBleedRuntimeState::SetInBleedState(active);
				},
				[&](const char* why) { TFD::BleedoutGreet::ResetRuntime(why); },
				[&]() {
					TFD::DefeatBleedRuntimeState::ClearPendingOutcomes();
					TFD::DefeatBleedoutRuntimeTimer::ResetDialogueRuntimeClock();
					TFD::Bleedout::ResetBleedSpeakerKick();
					TFD::Bleedout::SetBleedDialogueRetryCount(0);
					TFD::Bleedout::BleedLastCrowdAssignRef() = {};
					TFD::Bleedout::ClearBleedCrowdAssigned();
					TFD::Bleedout::ClearBleedRejectedSpeakerIds();
				},
				[&]() {
					TFD::DefeatBattleObserveState::ResetFlags();
					TFD::DefeatBattleObserveState::ResetTracking();
				},
				[&]() { TFD::Captive::ClearEscapeBreakRebleed(); },
				[&]() { TFD::DefeatAggressorResolver::ClearLastEnemyTargetingPlayer(); },
				[&](const char* why) { TFD::Bleedout::ClearSystemEventOutcomeWindow(why); }
			});
	}
}
