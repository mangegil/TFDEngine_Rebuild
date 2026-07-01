#include "TFDDefeatEventBridge.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <utility>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include "TFDBleedout.h"
#include "TFDBleedoutGreet.h"
#include "TFDDefeatAggressorResolver.h"
#include "TFDDefeatBleedoutRuntimeTimer.h"
#include "TFDDefeatStickyReopenGrace.h"
#include "TFDDefeatTerminalLockout.h"
#include "TFDDialogueLifecycle.h"
#include "TFDFlowController.h"
#include "TFDPlayerOverkillDamageHook.h"

namespace TFD::DefeatEventBridge
{
	namespace
	{
		constexpr const char* kBleedoutCancelStickyReopenEvent = "TFDBleedoutCancelStickyReopen";
		constexpr const char* kBleedoutGreetConfirmedEvent = "TFDBleedoutGreetConfirmed";

		std::atomic_bool g_installed{ false };
		Dependencies g_dependencies{};

		RE::Actor* PlayerActor()
		{
			return g_dependencies.getPlayerActor ? g_dependencies.getPlayerActor() : nullptr;
		}

		bool IsObserverAlly(RE::Actor* actor)
		{
			return g_dependencies.isObserverAlly ? g_dependencies.isObserverAlly(actor) : false;
		}

		class BleedOutcomeEventSink final : public RE::BSTEventSink<SKSE::ModCallbackEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* ev, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
			{
				if (!ev) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto* rawName = ev->eventName.c_str();
				if (!rawName || !rawName[0]) {
					return RE::BSEventNotifyControl::kContinue;
				}

				if (std::strcmp(rawName, kBleedoutGreetConfirmedEvent) == 0) {
					auto* speaker = ev->sender ? ev->sender->As<RE::Actor>() : nullptr;
					TFD::BleedoutGreet::NotifyFlowGreetConfirmed(speaker, ev->strArg.c_str());
					return RE::BSEventNotifyControl::kContinue;
				}

				if (std::strcmp(rawName, kBleedoutCancelStickyReopenEvent) == 0) {
					auto* speaker = ev->sender ? ev->sender->As<RE::Actor>() : nullptr;
					const auto speakerID = speaker ? speaker->GetFormID() : 0u;
					const bool wasGraceActive = TFD::DefeatStickyReopenGrace::IsActive();
					const auto graceSpeakerID = TFD::DefeatStickyReopenGrace::SpeakerID();
					const char* why = ev->strArg.empty() ? "r441a_reject_cycle_handoff" : ev->strArg.c_str();

					TFD::DefeatStickyReopenGrace::Clear(why);
					TFD::Bleedout::ClearSystemEventOutcomeWindow(why);
					TFD::BleedoutGreet::ClearFlowGreetConfirmed(why);
					TFD::BleedoutGreet::MarkStickyReopenPending(false, why);
					TFD::BleedoutGreet::ResetRuntime(why);
					TFD::DialogueLifecycle::SetDialogueOpenObserved(false);
					TFD::DefeatBleedoutRuntimeTimer::SetCountdownDirty();
					spdlog::info(
						"[TFD][Defeat][R441A] bleed sticky reopen cancelled by reject cycle sender={:08X} graceSpeaker={:08X} wasActive={} reason={}",
						speakerID,
						graceSpeakerID,
						wasGraceActive ? 1 : 0,
						why);
					return RE::BSEventNotifyControl::kContinue;
				}

				if (TFD::FlowController::HandleOutcomeModEvent(rawName, ev->strArg.c_str(), ev->numArg, ev->sender)) {
					if (TFD::DefeatTerminalLockout::IsBleedoutTerminalOutcomeEvent(rawName)) {
						TFD::DefeatStickyReopenGrace::CancelAfterTerminalOutcome(rawName, "mod_event_terminal_outcome");
						TFD::DefeatTerminalLockout::Arm(rawName, TFD::DefeatTerminalLockout::ResolveSeconds(rawName, ev->numArg), "mod_event_terminal_outcome");
					}
					return RE::BSEventNotifyControl::kContinue;
				}

				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class PassiveBreakEventSink final : public RE::BSTEventSink<RE::TESHitEvent>, public RE::BSTEventSink<SKSE::ModCallbackEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* ev, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				if (!ev) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* causeActor = ev->cause ? ev->cause.get()->As<RE::Actor>() : nullptr;
				auto* targetActor = ev->target ? ev->target.get()->As<RE::Actor>() : nullptr;
				(void)TFD::FlowController::HandlePassiveBreakHitEvent(causeActor, targetActor);
				if (targetActor && targetActor == PlayerActor() &&
					causeActor && causeActor != targetActor &&
					TFD::DefeatAggressorResolver::IsCombatSupportedAggressor(causeActor) &&
					!IsObserverAlly(causeActor)) {
					TFD::DefeatAggressorResolver::NoteEnemyTargetingPlayer(causeActor);
					TFD::PlayerOverkillDamageHook::SetKillmoveGuard(true, "tes_hit_event_player_target", std::chrono::milliseconds(2500));
					(void)TFD::PlayerOverkillDamageHook::TryQueueKillmoveBlockedBleedout(targetActor, causeActor, "tes_hit_event_player_target");
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* ev, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
			{
				if (!ev) {
					return RE::BSEventNotifyControl::kContinue;
				}

				(void)TFD::FlowController::HandlePassiveBreakModEvent(ev->eventName.c_str(), ev->strArg.c_str());
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		BleedOutcomeEventSink g_bleedOutcomeEventSink{};
		PassiveBreakEventSink g_passiveBreakEventSink{};
	}

	void Install(Dependencies dependencies)
	{
		if (g_installed.exchange(true, std::memory_order_acq_rel)) {
			g_dependencies = std::move(dependencies);
			return;
		}

		g_dependencies = std::move(dependencies);
		if (auto* scripts = RE::ScriptEventSourceHolder::GetSingleton()) {
			scripts->AddEventSink<RE::TESHitEvent>(&g_passiveBreakEventSink);
		}
		if (auto* src = SKSE::GetModCallbackEventSource()) {
			src->AddEventSink(&g_bleedOutcomeEventSink);
			src->AddEventSink(&g_passiveBreakEventSink);
		}
		spdlog::info("[TFD][DefeatEventBridge][P31A] event sinks installed");
	}

	void Shutdown()
	{
		if (!g_installed.exchange(false, std::memory_order_acq_rel)) {
			g_dependencies = {};
			return;
		}

		if (auto* scripts = RE::ScriptEventSourceHolder::GetSingleton()) {
			scripts->RemoveEventSink<RE::TESHitEvent>(&g_passiveBreakEventSink);
		}
		if (auto* src = SKSE::GetModCallbackEventSource()) {
			src->RemoveEventSink(&g_bleedOutcomeEventSink);
			src->RemoveEventSink(&g_passiveBreakEventSink);
		}
		g_dependencies = {};
		spdlog::info("[TFD][DefeatEventBridge][P31A] event sinks removed");
	}
}
