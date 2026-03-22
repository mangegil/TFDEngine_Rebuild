#include "TFDDefeatMonitor.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <string>
#include <cstring>
#include <vector>
#include <unordered_set>
#include <array>

#include <type_traits>
#include <RE/A/ActorValues.h>
#include <RE/Skyrim.h>
#include <RE/L/LockpickingMenu.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include "TFDSettings.h"
#include "TFDLocation.h"
#include "TFDCaptiveDoorController.h"
#include "TFDAntiAggro.h"
#include "TFDFactionMask.h"
#include "TFDForceGreet.h"
#include "TFDActorScan.h"
#include "TFDAggressionClamp.h"
#include "TFDPreCombatGreet.h"
#include "TFDPacify.h"
#include "EditorIdCache.h"
#include "RE/B/BGSRefAlias.h"
#include "RE/T/TESQuest.h"

namespace TFD::DefeatMonitor
{
	namespace
	{
		static void ClearBleedoutBridgeAliases(RE::TESForm* sender, const char* reason);
		static void AssignBleedoutBridgeActor(RE::Actor* actor);
		enum class CaptivePhaseValue : int
		{
			None = 0,
			Captive = 1,
			Escape = 2,
			ReleasedWork = 3,
			Scene = 4
		};

		std::atomic_bool g_installed{ false };
		std::atomic_bool g_running{ false };
		std::atomic_bool g_loadTransition{ false };
		std::atomic_flag g_tickPending = ATOMIC_FLAG_INIT;
		std::thread g_worker{};

		static RE::TESGlobal* g_captiveStateGlobal = nullptr;
		static RE::TESGlobal* g_captivePhaseGlobal = nullptr;
		static RE::TESGlobal* g_preCombatStateGlobal = nullptr;
		static RE::TESGlobal* g_transitionPendingGlobal = nullptr;
		static RE::TESGlobal* g_transitionBusyGlobal = nullptr;
		static RE::TESGlobal* g_transitionReasonGlobal = nullptr;
		static RE::TESGlobal* g_transitionResultGlobal = nullptr;
		static bool g_loggedCaptiveStateGlobal = false;
		static bool g_loggedCaptivePhaseGlobal = false;
		static bool g_loggedPreCombatStateGlobal = false;
		static bool g_loggedTransitionPendingGlobal = false;
		static bool g_loggedTransitionBusyGlobal = false;
		static bool g_loggedTransitionReasonGlobal = false;
		static bool g_loggedTransitionResultGlobal = false;

		static inline std::chrono::steady_clock::time_point Now()
		{
			return std::chrono::steady_clock::now();
		}

		std::atomic_bool g_inBleedState{ false };
		std::atomic<std::uint64_t> g_bleedBridgeEpoch{ 1 };
		float g_minHp{ 0.0f };
		bool g_playerBleedImmuneForced = false;
		bool g_playerWasEssential = false;
		bool g_playerWasInvulnerable = false;

		std::chrono::steady_clock::time_point g_bleedStart{};
		int g_bleedLastSeconds = -1;
		bool g_bleedPaused = false;
		std::chrono::steady_clock::time_point g_bleedPauseStarted{};
		std::chrono::steady_clock::time_point g_bleedLastCalmPulse{};
		std::chrono::steady_clock::time_point g_bleedLastCrowdAssign{};

		std::vector<RE::FormID> g_bleedCrowdAssigned{};

		std::atomic_bool g_grace{ false };
		std::chrono::steady_clock::time_point g_graceUntil{};

		bool g_leftForDeadActive = false;
		std::chrono::steady_clock::time_point g_leftForDeadUntil{};
		std::chrono::steady_clock::time_point g_leftForDeadNextPulse{};
		bool g_leftForDeadNeedsAggroKick = false;


		RE::ActorHandle g_lastAggressor{};
		bool g_bleedSawDialogue = false;
		bool g_bleedPendingCaptiveOutcome = false;
		bool g_bleedPendingNonCaptiveOutcome = false;

		bool g_captiveState = false;
		CaptivePhaseValue g_captivePhase = CaptivePhaseValue::None;
		bool g_prevDialogueOpen = false;
		bool g_prevLockpickOpen = false;

		struct TeammateRegistryCache
		{
			RE::TESQuest* quest{ nullptr };
			RE::TESFaction* currentFollowerFaction{ nullptr };
			RE::TESFaction* playerFollowerFaction{ nullptr };
			std::array<RE::BGSRefAlias*, 10> teammateAliases{};
			bool resolved{ false };
		};

		TeammateRegistryCache g_teammateRegistry{};

		enum class NoMarkerFallbackBranch : std::uint32_t
		{
			None = 0,
			RecoveryFollower = 1,
			RecoveryPotion = 2,
			RescueCached = 3,
			LeftForDeadSolo = 4,
			LeftForDeadWithFollower = 5
		};

		struct NoMarkerFallbackState
		{
			NoMarkerFallbackBranch branch{ NoMarkerFallbackBranch::None };
			RE::ActorHandle follower{};
			RE::ObjectRefHandle destination{};
			RE::NiPoint3 fallbackPos{};
			bool hasFallbackPos{ false };
			float angleZ{ 0.0f };
			RE::FormID potionFormId{ 0 };
		};

		NoMarkerFallbackState g_noMarkerFallback{};
		RE::ActorHandle g_allyHoldFollower{};
		bool g_allyHoldActive = false;
		std::vector<RE::FormID> g_lockedFallbackCrowdIds{};
		std::unordered_set<RE::FormID> g_bleedFollowerDownIds{};

		RE::ObjectRefHandle g_lockpickDoorCandidate{};
		bool g_lockpickDoorWasLocked = false;
		TFD::CaptiveDoorController g_captiveDoor{};
		RE::ObjectRefHandle g_captiveMarker{};
		RE::FormID g_captiveCellFormID = 0;
		RE::FormID g_captiveLocationFormID = 0;
		bool g_escapeRadiusActive = false;
		std::chrono::steady_clock::time_point g_escapeRadiusSince{};
		RE::ObjectRefHandle g_boundEscapeDoor{};

		static constexpr double kCaptiveEscapeDoorRadius = 512.0;
		static constexpr std::size_t kBleedBridgeMaxActors = 10;

		bool g_hasQueuedProgressState = false;
		bool g_queuedCaptiveState = false;
		CaptivePhaseValue g_queuedCaptivePhase = CaptivePhaseValue::None;

		static RE::PlayerCharacter* Player()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

		static void SetPlayerBleedImmune(bool enable)
		{
			if (enable) {
				if (g_playerBleedImmuneForced) {
					return;
				}

				g_playerBleedImmuneForced = true;
				g_playerWasEssential = false;
				g_playerWasInvulnerable = false;
				spdlog::info("[TFD][Defeat] player bleed soft-guard enabled");
				return;
			}

			if (!g_playerBleedImmuneForced) {
				return;
			}

			g_playerBleedImmuneForced = false;
			g_playerWasEssential = false;
			g_playerWasInvulnerable = false;
			spdlog::info("[TFD][Defeat] player bleed soft-guard released");
		}

		static RE::BGSKeyword* LookupKeyword(const char* editorID)
		{
			if (!editorID || !editorID[0]) {
				return nullptr;
			}
			return RE::TESForm::LookupByEditorID<RE::BGSKeyword>(editorID);
		}

		static bool ActorHasKeywordByEditorID(RE::Actor* actor, const char* editorID)
		{
			if (!actor) {
				return false;
			}
			auto* kw = LookupKeyword(editorID);
			return kw && actor->HasKeyword(kw);
		}

		static bool IsCaptiveSupportedAggressor(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}

			if (ActorHasKeywordByEditorID(actor, "ActorTypeNPC")) {
				return true;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeCreature")) {
				return false;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeAnimal")) {
				return false;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeDragon")) {
				return false;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeDaedra")) {
				return false;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeGhost")) {
				return false;
			}
			if (ActorHasKeywordByEditorID(actor, "ActorTypeUndead")) {
				return false;
			}

			return false;
		}

		static bool IsActiveFollowerActor(RE::Actor* actor);

		static bool IsBleedCrowdSupportedAggressor(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			if (IsActiveFollowerActor(actor)) {
				return false;
			}

			if (ActorHasKeywordByEditorID(actor, "ActorTypeDragon") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeGhost")) {
				return false;
			}

			if (ActorHasKeywordByEditorID(actor, "ActorTypeNPC") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeCreature") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeAnimal") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeUndead") ||
				ActorHasKeywordByEditorID(actor, "ActorTypeDaedra")) {
				return true;
			}

			return false;
		}

		static RE::Actor* ResolveAggressor();
		static RE::Actor* FindBestAggressor(float radius);
		static void ClearNoMarkerFallbackState();
		static void MaintainFollowerHold();
		static void MaintainTransitionCalmWindow();
		static void SnapshotBleedFollowerDownState(float radius);
		static void RestoreFollowerAfterTransition(RE::Actor* actor);
		static void ResolveTeammateRegistry();
		static bool IsRegisteredTeammateActor(RE::Actor* actor);
		static std::vector<RE::Actor*> CollectRegisteredTeammates();

		static void QueuePostRecoveryAggroKick(const char* reason)
		{
			auto* player = Player();
			if (!player) {
				return;
			}

			auto* pCell = player->GetParentCell();
			if (!pCell) {
				return;
			}

			const float radius = (std::max)(2200.0f, TFD::Settings::GetSweepRadius() + 400.0f);
			RE::Actor* primary = ResolveAggressor();
			if (!primary) {
				primary = FindBestAggressor(radius);
			}

			std::unordered_set<RE::FormID> queuedIds;
			std::size_t queued = 0;

			auto queueOne = [&](RE::Actor* actor, bool drawWeapon) {
				if (!actor || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
					return;
				}
				if (actor->GetFormID() == player->GetFormID()) {
					return;
				}
				if (actor->GetParentCell() != pCell) {
					return;
				}
				if (!IsCaptiveSupportedAggressor(actor)) {
					return;
				}

				const auto [_, inserted] = queuedIds.insert(actor->GetFormID());
				if (!inserted) {
					return;
				}

				const bool kickedNow = TFD::Pacify::ForceDetectionAndCombatRefresh(
					actor,
					player,
					TFD::Pacify::ReleaseReason::DialogueClosed,
					drawWeapon);
				TFD::Pacify::QueueDetectionAndCombatRefresh(
					actor,
					player,
					TFD::Pacify::ReleaseReason::DialogueClosed,
					drawWeapon);
				++queued;

				spdlog::info(
					"[TFD][Defeat] post-recovery aggro kick actor={:08X} drawWeapon={} immediate={} reason={}",
					actor->GetFormID(),
					drawWeapon ? 1 : 0,
					kickedNow ? 1 : 0,
					reason ? reason : "unknown");
				};

			if (primary) {
				queueOne(primary, true);
			}

			TFD::ActorScan::Rescan(radius, true);
			const auto n = TFD::ActorScan::GetCount();
			for (int i = 0; i < n; ++i) {
				auto entry = TFD::ActorScan::GetEntry(i);
				auto sp = entry.actor.get();
				auto* actor = sp.get();
				if (!actor || actor == primary) {
					continue;
				}
				if (entry.dist > radius) {
					continue;
				}
				if (!entry.hostile && !entry.inCombat && !actor->IsInCombat() && !actor->IsHostileToActor(player)) {
					continue;
				}

				queueOne(actor, true);

				if (queued >= 8) {
					break;
				}
			}

			if (queued > 0) {
				player->EvaluatePackage(false, true);
				player->EvaluatePackage(true, true);
				player->UpdateCombat();
			}

			spdlog::info(
				"[TFD][Defeat] post-recovery aggro kick queued={} primary={:08X} reason={}",
				queued,
				primary ? primary->GetFormID() : 0u,
				reason ? reason : "unknown");
		}

		static void FinishLeftForDeadRecovery()
		{
			RE::Actor* followerToRestore = nullptr;
			if (g_allyHoldFollower) {
				auto sp = RE::Actor::LookupByHandle(g_allyHoldFollower.native_handle());
				followerToRestore = sp.get();
			}
			else if (g_noMarkerFallback.follower) {
				auto sp = RE::Actor::LookupByHandle(g_noMarkerFallback.follower.native_handle());
				followerToRestore = sp.get();
			}
			TFD::AggressionClamp::Clear();
			TFD::FactionMask::Clear();
			if (g_leftForDeadNeedsAggroKick) {
				QueuePostRecoveryAggroKick("left_for_dead_recovery_finished");
			}
			if (followerToRestore) {
				RestoreFollowerAfterTransition(followerToRestore);
			}
			g_leftForDeadNeedsAggroKick = false;
			g_leftForDeadActive = false;
			g_leftForDeadUntil = {};
			g_leftForDeadNextPulse = {};
			g_allyHoldFollower.reset();
			g_allyHoldActive = false;
			g_lockedFallbackCrowdIds.clear();
			g_noMarkerFallback = {};
			g_bleedFollowerDownIds.clear();
			spdlog::info("[TFD][Defeat] Transition recovery finished");
		}

		static bool IsLeftForDeadCooldownActive()
		{
			if (!g_leftForDeadActive) {
				return false;
			}
			const auto now = Now();
			if (now >= g_leftForDeadUntil) {
				FinishLeftForDeadRecovery();
				return false;
			}
			return true;
		}

		static void ClearLeftForDeadCooldown()
		{
			if (g_leftForDeadActive) {
				FinishLeftForDeadRecovery();
				return;
			}
			g_leftForDeadActive = false;
			g_leftForDeadUntil = {};
			g_leftForDeadNextPulse = {};
			g_leftForDeadNeedsAggroKick = false;
			ClearNoMarkerFallbackState();
		}

		static void BeginLeftForDeadCooldown(int seconds)
		{
			if (seconds <= 0) {
				ClearLeftForDeadCooldown();
				return;
			}
			const auto now = Now();
			g_leftForDeadActive = true;
			g_leftForDeadUntil = now + std::chrono::seconds(seconds);
			g_leftForDeadNextPulse = now;
		}

		static void ShowBlackoutFader()
		{
			auto* queue = RE::UIMessageQueue::GetSingleton();
			auto* strings = RE::InterfaceStrings::GetSingleton();
			if (!queue || !strings) {
				return;
			}
			queue->AddMessage(strings->faderMenu, RE::UI_MESSAGE_TYPE::kShow, nullptr);
			queue->ProcessCommands();
		}

		static void HideBlackoutFader()
		{
			auto* queue = RE::UIMessageQueue::GetSingleton();
			auto* strings = RE::InterfaceStrings::GetSingleton();
			if (!queue || !strings) {
				return;
			}
			queue->AddMessage(strings->faderMenu, RE::UI_MESSAGE_TYPE::kHide, nullptr);
			queue->ProcessCommands();
		}

		static bool SendBridgeModEvent(const char* eventName, RE::TESForm* sender = nullptr, const char* strArg = "", float numArg = 0.0f)
		{
			if (!eventName || !eventName[0]) {
				return false;
			}

			auto* task = SKSE::GetTaskInterface();
			if (!task) {
				spdlog::warn("[TFD][BleedBridge] SendBridgeModEvent failed: no task interface event={}", eventName);
				return false;
			}

			const std::string name{ eventName };
			const std::string sarg{ strArg ? strArg : "" };
			const float narg = numArg;

			std::uint32_t actorHandle = 0;
			RE::FormID senderFormID = 0;

			if (sender) {
				senderFormID = sender->GetFormID();
				if (auto* actor = sender->As<RE::Actor>()) {
					actorHandle = actor->GetHandle().native_handle();
				}
			}

			task->AddTask([name, sarg, narg, actorHandle, senderFormID]() {
				RE::TESForm* outSender = nullptr;

				if (actorHandle != 0) {
					auto actorSp = RE::Actor::LookupByHandle(actorHandle);
					outSender = actorSp.get();
					if (!outSender && senderFormID != 0) {
						outSender = RE::TESForm::LookupByID(senderFormID);
					}
				}
				else if (senderFormID != 0) {
					outSender = RE::TESForm::LookupByID(senderFormID);
				}

				auto* src = SKSE::GetModCallbackEventSource();
				if (!src) {
					spdlog::warn("[TFD][BleedBridge] Dispatch skipped: no callback source event={} sender={:08X}", name, senderFormID);
					return;
				}

				SKSE::ModCallbackEvent ev{ name.c_str(), sarg.c_str(), narg, outSender };
				src->SendEvent(&ev);

				spdlog::info("[TFD][BleedBridge] Dispatch event={} sender={:08X} resolved={:08X}",
					name,
					senderFormID,
					outSender ? outSender->GetFormID() : 0u);
				});

			return true;
		}

		static bool QueueBleedBridgeClearAll(RE::TESForm* sender, const char* reason)
		{
			auto* task = SKSE::GetTaskInterface();
			if (!task) {
				spdlog::warn("[TFD][BleedBridge] ClearAll failed: no task interface reason={}", reason ? reason : "unknown");
				return false;
			}

			const std::uint64_t epoch = g_bleedBridgeEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
			std::uint32_t actorHandle = 0;
			RE::FormID senderFormID = 0;
			if (sender) {
				senderFormID = sender->GetFormID();
				if (auto* actor = sender->As<RE::Actor>()) {
					actorHandle = actor->GetHandle().native_handle();
				}
			}

			task->AddTask([epoch, actorHandle, senderFormID]() {
				RE::TESForm* outSender = nullptr;
				if (actorHandle != 0) {
					auto actorSp = RE::Actor::LookupByHandle(actorHandle);
					outSender = actorSp.get();
					if (!outSender && senderFormID != 0) {
						outSender = RE::TESForm::LookupByID(senderFormID);
					}
				}
				else if (senderFormID != 0) {
					outSender = RE::TESForm::LookupByID(senderFormID);
				}

				auto* src = SKSE::GetModCallbackEventSource();
				if (!src) {
					spdlog::warn("[TFD][BleedBridge] ClearAll dispatch skipped: no callback source sender={:08X} epoch={}", senderFormID, epoch);
					return;
				}

				SKSE::ModCallbackEvent ev{ "TFDBleedoutClearAll", "", 0.0f, outSender };
				src->SendEvent(&ev);
				spdlog::info("[TFD][BleedBridge] Dispatch event=TFDBleedoutClearAll sender={:08X} resolved={:08X} epoch={}",
					senderFormID,
					outSender ? outSender->GetFormID() : 0u,
					epoch);
				});
			return true;
		}

		static bool QueueBleedBridgeAssign(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}

			auto* task = SKSE::GetTaskInterface();
			if (!task) {
				spdlog::warn("[TFD][BleedBridge] Assign failed: no task interface actor={:08X}", actor->GetFormID());
				return false;
			}

			const std::uint64_t epoch = g_bleedBridgeEpoch.load(std::memory_order_acquire);
			const RE::FormID senderFormID = actor->GetFormID();
			const std::uint32_t actorHandle = actor->GetHandle().native_handle();

			task->AddTask([epoch, actorHandle, senderFormID]() {
				const auto currentEpoch = g_bleedBridgeEpoch.load(std::memory_order_acquire);
				if (epoch != currentEpoch) {
					spdlog::info("[TFD][BleedBridge] Skip stale assign sender={:08X} queuedEpoch={} currentEpoch={}",
						senderFormID,
						epoch,
						currentEpoch);
					return;
				}

				auto actorSp = RE::Actor::LookupByHandle(actorHandle);
				RE::TESForm* outSender = actorSp.get();
				if (!outSender && senderFormID != 0) {
					outSender = RE::TESForm::LookupByID(senderFormID);
				}

				if (!outSender) {
					spdlog::info("[TFD][BleedBridge] Skip assign sender={:08X} epoch={} (sender unresolved)", senderFormID, epoch);
					return;
				}

				auto* src = SKSE::GetModCallbackEventSource();
				if (!src) {
					spdlog::warn("[TFD][BleedBridge] Assign dispatch skipped: no callback source sender={:08X} epoch={}", senderFormID, epoch);
					return;
				}

				SKSE::ModCallbackEvent ev{ "TFDBleedoutAssign", "", 0.0f, outSender };
				src->SendEvent(&ev);
				spdlog::info("[TFD][BleedBridge] Dispatch event=TFDBleedoutAssign sender={:08X} resolved={:08X} epoch={}",
					senderFormID,
					outSender ? outSender->GetFormID() : 0u,
					epoch);
				});
			return true;
		}

		static void ClearBleedoutBridgeAliases(RE::TESForm* sender, const char* reason)
		{
			const bool queued = QueueBleedBridgeClearAll(sender, reason);
			spdlog::info("[TFD][BleedBridge] ClearAll reason={} queued={} epoch={}",
				reason ? reason : "unknown",
				queued,
				g_bleedBridgeEpoch.load(std::memory_order_acquire));
		}

		static void AssignBleedoutBridgeActor(RE::Actor* actor)
		{
			if (!actor) {
				return;
			}

			const auto epoch = g_bleedBridgeEpoch.load(std::memory_order_acquire);
			const bool queued = QueueBleedBridgeAssign(actor);
			spdlog::info("[TFD][BleedBridge] Assign actor={:08X} queued={} epoch={}", actor->GetFormID(), queued, epoch);
		}

		static bool IsActorCloseAndFront(RE::Actor* actor, RE::Actor* player, float maxDist);

		static std::vector<RE::Actor*> CollectBleedoutCrowd(float radius, RE::Actor* preferred, bool preserveAssigned = false)
		{
			std::vector<std::pair<float, RE::Actor*>> scored;

			auto* player = Player();
			if (!player) {
				return {};
			}

			auto* pCell = player->GetParentCell();
			if (!pCell) {
				return {};
			}

			std::unordered_set<RE::FormID> preservedIds;
			if (preserveAssigned) {
				preservedIds.insert(g_bleedCrowdAssigned.begin(), g_bleedCrowdAssigned.end());
			}

			const float scanRadius = (std::max)(radius, 2000.0f);
			TFD::ActorScan::Rescan(scanRadius, false);
			const auto n = TFD::ActorScan::GetCount();
			for (int i = 0; i < n; ++i) {
				auto e = TFD::ActorScan::GetEntry(i);
				auto sp = e.actor.get();
				auto* actor = sp.get();
				if (!actor || actor->IsDead() || actor->IsDisabled()) continue;
				if (!actor->Is3DLoaded()) continue;
				if (actor->GetFormID() == player->GetFormID()) continue;
				if (actor->GetParentCell() != pCell) continue;
				if (!IsBleedCrowdSupportedAggressor(actor)) continue;
				if (e.dist > scanRadius) continue;

				const bool targetingPlayer = e.hostile || e.inCombat || actor->IsInCombat() || actor->IsHostileToActor(player);
				const bool preserved = preserveAssigned && preservedIds.find(actor->GetFormID()) != preservedIds.end();
				if (!targetingPlayer && !preserved && actor != preferred) continue;

				float score = e.dist;
				if (actor == preferred) score -= 1000.0f;
				if (targetingPlayer) score -= 140.0f;
				if (e.hostile) score -= 80.0f;
				if (e.inCombat || actor->IsInCombat()) score -= 60.0f;
				if (preserved) score -= 90.0f;
				if (IsActorCloseAndFront(actor, player, 320.0f)) score -= 120.0f;
				scored.emplace_back(score, actor);
			}

			std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
				if (a.first != b.first) {
					return a.first < b.first;
				}
				if (!a.second || !b.second) {
					return a.second != nullptr;
				}
				return a.second->GetFormID() < b.second->GetFormID();
				});

			std::vector<RE::Actor*> result;
			result.reserve((std::min)(scored.size(), kBleedBridgeMaxActors));
			for (const auto& [score, actor] : scored) {
				(void)score;
				if (!actor) {
					continue;
				}
				const auto id = actor->GetFormID();
				bool seen = false;
				for (auto* existing : result) {
					if (existing && existing->GetFormID() == id) {
						seen = true;
						break;
					}
				}
				if (seen) {
					continue;
				}
				result.push_back(actor);
				if (result.size() >= kBleedBridgeMaxActors) {
					break;
				}
			}

			if (preferred) {
				const auto preferredId = preferred->GetFormID();
				auto it = std::find_if(result.begin(), result.end(), [preferredId](RE::Actor* actor) {
					return actor && actor->GetFormID() == preferredId;
					});
				if (it == result.end()) {
					if (result.size() >= kBleedBridgeMaxActors) {
						result.pop_back();
					}
					result.insert(result.begin(), preferred);
				}
				else if (it != result.begin()) {
					std::rotate(result.begin(), it, it + 1);
				}
			}

			return result;
		}

		static void AssignBleedoutBridgeCrowd(const std::vector<RE::Actor*>& actors)
		{
			std::size_t sent = 0;
			for (auto* actor : actors) {
				if (!actor) {
					continue;
				}
				const auto epoch = g_bleedBridgeEpoch.load(std::memory_order_acquire);
				const bool queued = QueueBleedBridgeAssign(actor);
				if (queued) {
					++sent;
				}
				spdlog::info("[TFD][BleedBridge] Assign actor={:08X} queued={} epoch={}", actor->GetFormID(), queued, epoch);
			}

			spdlog::info("[TFD][BleedBridge] Assign crowd sent={} size={} primary={:08X}",
				sent,
				actors.size(),
				!actors.empty() && actors.front() ? actors.front()->GetFormID() : 0u);
		}

		static void RefreshBleedoutBridgeCrowd(float radius, RE::Actor* preferred, bool forceClear, std::vector<RE::Actor*>* explicitCrowd = nullptr)
		{
			std::vector<RE::Actor*> crowd = explicitCrowd ? *explicitCrowd : CollectBleedoutCrowd(radius, preferred, !forceClear);
			std::vector<RE::FormID> next;
			next.reserve(crowd.size());
			for (auto* actor : crowd) {
				if (actor) {
					next.push_back(actor->GetFormID());
				}
			}

			const bool changed = forceClear || next != g_bleedCrowdAssigned;
			if (changed) {
				ClearBleedoutBridgeAliases(preferred, forceClear ? "bleed_crowd_force_refresh" : "bleed_crowd_refresh");
				if (!crowd.empty()) {
					AssignBleedoutBridgeCrowd(crowd);
				}
				g_bleedCrowdAssigned = std::move(next);
			}
			g_bleedLastCrowdAssign = Now();
		}

		static void ResetBleedRuntimeState()
		{
			g_inBleedState.store(false, std::memory_order_release);
			g_minHp = 0.0f;
			g_bleedSawDialogue = false;
			g_bleedPendingCaptiveOutcome = false;
			g_bleedPendingNonCaptiveOutcome = false;
			g_bleedPaused = false;
			g_bleedPauseStarted = {};
			g_bleedLastCalmPulse = {};
			g_bleedLastCrowdAssign = {};
			g_bleedCrowdAssigned.clear();
			g_bleedStart = Now();
			g_bleedLastSeconds = -1;
		}


		static void AdvanceGameHoursSoft(float hours)
		{
			if (hours <= 0.0f) {
				return;
			}
			auto* calendar = RE::Calendar::GetSingleton();
			if (!calendar) {
				return;
			}
			const float dayDelta = hours / 24.0f;
			calendar->rawDaysPassed += dayDelta;
			if (calendar->gameDaysPassed) {
				calendar->gameDaysPassed->value = calendar->rawDaysPassed;
			}
			if (calendar->gameHour) {
				float hour = std::fmod(calendar->gameHour->value + hours, 24.0f);
				if (hour < 0.0f) {
					hour += 24.0f;
				}
				calendar->gameHour->value = hour;
			}
		}

		static void BlackoutAndAdvanceHours(float hours, int holdMs, const char* reason)
		{
			ShowBlackoutFader();
			std::this_thread::sleep_for(std::chrono::milliseconds(350));
			AdvanceGameHoursSoft(hours);
			if (holdMs > 0) {
				std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
			}
			HideBlackoutFader();
			spdlog::info("[TFD][Defeat] blackout advance reason={} hours={:.2f}", reason ? reason : "unknown", hours);
		}

		static CaptivePhaseValue PhaseFromRaw(std::uint32_t raw)
		{
			switch (raw) {
			case 1: return CaptivePhaseValue::Captive;
			case 2: return CaptivePhaseValue::Escape;
			case 3: return CaptivePhaseValue::ReleasedWork;
			case 4: return CaptivePhaseValue::Scene;
			default: return CaptivePhaseValue::None;
			}
		}


		static RE::Actor* ResolveAggressor();
		static RE::Actor* FindBestAggressor(float radius);
		static RE::Actor* FindBestBleedoutSpeaker(float radius, float maxDist, RE::Actor* preferred = nullptr);
		static bool IsReasonableBleedoutSpeaker(RE::Actor* actor, RE::Actor* player, float maxDist, float* outDistance = nullptr);
		static RE::Actor* ResolveRecentPreCombatAggressor(float radius);
		static void SetGraceSeconds(int seconds);
		static void RecoverPlayerForTransition();
		static void MaintainTransitionCalmWindow();
		static void UpdatePreCombatState();
		static bool IsActorCloseAndFront(RE::Actor* actor, RE::Actor* player, float maxDist);
		static bool CanUseAggressorForBleedoutGreet(RE::Actor* player, RE::Actor* aggressor, float& outDistance);
		static RE::TESObjectREFR* LookupRefByFormID(std::uint32_t formID);
		static RE::BGSLocation* GetLocationFromRef(RE::TESObjectREFR* ref);
		static RE::TESObjectREFR* ResolveBestRescueDestination(RE::BGSLocation* safeLoc);
		static bool BeginRescueTransition(const char* reason);
		static void BeginRecoverTransition(const char* reason);
		static void DoBlackoutTeleport();

		enum class CinematicTransitionKind
		{
			None = 0,
			Captive = 1,
			Rescue = 2,
			Recover = 3
		};

		static bool g_pendingCinematicFadeIn = false;
		static CinematicTransitionKind g_pendingCinematicFadeInKind = CinematicTransitionKind::None;
		static std::chrono::steady_clock::time_point g_pendingCinematicFadeInNotBefore{};
		static bool g_pendingCinematicFadeInSawLoadingMenu = false;

		static bool QueueCinematicTransitionRequest(CinematicTransitionKind kind, bool fadeIn, const char* reason);
		static void CompleteCaptiveTransitionNow(const char* reason);
		static bool CompleteRescueTransitionNow(const char* reason);
		static void CompleteRecoverTransitionNow(const char* reason);
		static void EnterNonCaptiveChoice(const char* reason);
		static bool BeginResolvedNoMarkerFallback(const char* reason);
		static void RecoverPlayerAfterTeleport();
		static void SetCaptiveRuntime(bool stateActive, CaptivePhaseValue phase);
		static bool IsDialogueOpen();
		static bool IsLockpickingOpen();
		static void ResetLockpickWatch();
		static void ArmEscapeContextFromCurrentState();
		static void ApplyCalmBubble(float radius);

		static const char* NoMarkerBranchName(NoMarkerFallbackBranch branch)
		{
			switch (branch) {
			case NoMarkerFallbackBranch::RecoveryFollower: return "recovery_follower";
			case NoMarkerFallbackBranch::RecoveryPotion: return "recovery_potion";
			case NoMarkerFallbackBranch::RescueCached: return "rescue_cached";
			case NoMarkerFallbackBranch::LeftForDeadSolo: return "left_for_dead_solo";
			case NoMarkerFallbackBranch::LeftForDeadWithFollower: return "left_for_dead_with_follower";
			default: return "none";
			}
		}

		static void ClearNoMarkerFallbackState()
		{
			g_noMarkerFallback = {};
			g_allyHoldFollower.reset();
			g_allyHoldActive = false;
			g_lockedFallbackCrowdIds.clear();
			g_bleedFollowerDownIds.clear();
		}

		static bool IsActorBleedingOut(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}
			if (auto* state = actor->AsActorState()) {
				return state->IsBleedingOut();
			}
			return false;
		}

		static void ResolveTeammateRegistry()
		{
			if (g_teammateRegistry.resolved) {
				return;
			}
			g_teammateRegistry.resolved = true;
			g_teammateRegistry.quest = RE::TESForm::LookupByEditorID<RE::TESQuest>("TFDPlayerTeammateQuest");
			if (!g_teammateRegistry.quest) {
				spdlog::warn("[TFD][Defeat] teammate registry quest not found");
				return;
			}
			for (auto* baseAlias : g_teammateRegistry.quest->aliases) {
				auto* refAlias = skyrim_cast<RE::BGSRefAlias*>(baseAlias);
				if (!refAlias) {
					continue;
				}
				const auto aliasName = std::string(refAlias->aliasName.c_str());
				if (aliasName.rfind("Teammate", 0) != 0 || aliasName.size() < 10) {
					continue;
				}
				try {
					int slot = std::stoi(aliasName.substr(9));
					if (slot >= 1 && slot <= 10) {
						g_teammateRegistry.teammateAliases[slot - 1] = refAlias;
					}
				}
				catch (...) {
				}
			}
			std::size_t found = 0;
			for (auto* a : g_teammateRegistry.teammateAliases) {
				if (a) {
					++found;
				}
			}
			spdlog::info("[TFD][Defeat] teammate registry resolved quest={:08X} aliases={}",
				g_teammateRegistry.quest ? g_teammateRegistry.quest->GetFormID() : 0u, found);
		}

		static bool IsRegisteredTeammateActor(RE::Actor* actor)
		{
			if (!actor || actor->IsDisabled()) {
				return false;
			}
			ResolveTeammateRegistry();
			for (auto* alias : g_teammateRegistry.teammateAliases) {
				if (!alias) {
					continue;
				}
				auto* slotActor = alias->GetActorReference();
				if (slotActor && slotActor == actor) {
					return true;
				}
			}
			return false;
		}

		static std::vector<RE::Actor*> CollectRegisteredTeammates()
		{
			ResolveTeammateRegistry();
			std::vector<RE::Actor*> out;
			out.reserve(10);
			for (auto* alias : g_teammateRegistry.teammateAliases) {
				if (!alias) {
					continue;
				}
				auto* actor = alias->GetActorReference();
				if (!actor || actor->IsDisabled()) {
					continue;
				}
				out.push_back(actor);
			}
			return out;
		}

		static bool HasFollowerAnchorFaction(RE::Actor* actor)
		{
			if (!actor || actor->IsDisabled()) {
				return false;
			}
			ResolveTeammateRegistry();
			if (g_teammateRegistry.currentFollowerFaction && actor->IsInFaction(g_teammateRegistry.currentFollowerFaction)) {
				return true;
			}
			if (g_teammateRegistry.playerFollowerFaction && actor->IsInFaction(g_teammateRegistry.playerFollowerFaction)) {
				return true;
			}
			return false;
		}

		static std::vector<RE::Actor*> CollectKnownTeammates(float radius)
		{
			auto* player = Player();
			std::vector<RE::Actor*> out;
			if (!player) {
				return out;
			}

			std::unordered_set<RE::FormID> seen;
			const float maxRadius = radius > 0.0f ? (std::max)(radius, 5000.0f) : 5000.0f;

			for (auto* actor : CollectRegisteredTeammates()) {
				if (!actor || actor == player || actor->IsDisabled()) {
					continue;
				}
				if (radius > 0.0f) {
					auto apos = actor->GetPosition();
					auto ppos = player->GetPosition();
					const float dx = apos.x - ppos.x;
					const float dy = apos.y - ppos.y;
					const float dz = apos.z - ppos.z;
					const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
					if (dist > maxRadius) {
						continue;
					}
				}
				seen.insert(actor->GetFormID());
				out.push_back(actor);
			}

			TFD::ActorScan::Rescan(maxRadius, true);
			const auto count = TFD::ActorScan::GetCount();
			auto* pCell = player->GetParentCell();
			for (int i = 0; i < count; ++i) {
				auto entry = TFD::ActorScan::GetEntry(i);
				auto sp = entry.actor.get();
				auto* actor = sp.get();
				if (!actor || actor == player || actor->IsDisabled()) {
					continue;
				}
				if (actor->GetParentCell() != pCell) {
					continue;
				}
				if (entry.dist > maxRadius) {
					continue;
				}
				if (!actor->IsPlayerTeammate() && !HasFollowerAnchorFaction(actor)) {
					continue;
				}
				if (!seen.insert(actor->GetFormID()).second) {
					continue;
				}
				out.push_back(actor);
			}

			return out;
		}

		static bool IsActiveFollowerActor(RE::Actor* actor)
		{
			if (!actor || actor->IsDisabled()) {
				return false;
			}
			if (IsRegisteredTeammateActor(actor)) {
				return true;
			}
			if (actor->IsPlayerTeammate()) {
				return true;
			}
			return HasFollowerAnchorFaction(actor);
		}

		static void SnapshotBleedFollowerDownState(float radius)
		{
			(void)radius;
			auto* player = Player();
			if (!player) {
				return;
			}
			for (auto* actor : CollectKnownTeammates(radius)) {
				if (!actor || actor == player) {
					continue;
				}
				if (actor->IsDead() || IsActorBleedingOut(actor)) {
					g_bleedFollowerDownIds.insert(actor->GetFormID());
				}
			}
		}

		static void RestoreFollowerAfterTransition(RE::Actor* actor)
		{
			if (!actor || actor->IsDisabled()) {
				return;
			}
			actor->AllowPCDialogue(true);
			actor->SetDialogueWithPlayer(false, false, nullptr);
			if (!actor->IsDead()) {
				if (actor->IsInCombat()) {
					actor->StopCombat();
				}
				if (auto* process = RE::ProcessLists::GetSingleton()) {
					process->StopCombatAndAlarmOnActor(actor, false);
				}
				if (actor->IsWeaponDrawn()) {
					actor->DrawWeaponMagicHands(false);
				}
			}
			actor->EvaluatePackage(false, true);
			actor->EvaluatePackage(true, true);
		}

		static void ApplyFollowerHold(RE::Actor* actor)
		{
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				return;
			}
			if (actor->IsInCombat()) {
				actor->StopCombat();
			}
			if (auto* process = RE::ProcessLists::GetSingleton()) {
				process->StopCombatAndAlarmOnActor(actor, false);
			}
			if (actor->IsWeaponDrawn()) {
				actor->DrawWeaponMagicHands(false);
			}
			actor->EvaluatePackage(false, true);
			actor->EvaluatePackage(true, true);
		}

		static void MaintainFollowerHold()
		{
			if (!g_allyHoldActive || !g_allyHoldFollower) {
				return;
			}
			auto sp = RE::Actor::LookupByHandle(g_allyHoldFollower.native_handle());
			auto* actor = sp.get();
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				g_allyHoldFollower.reset();
				g_allyHoldActive = false;
				return;
			}
			ApplyFollowerHold(actor);
		}

		static void SetFollowerHold(RE::Actor* actor)
		{
			if (!actor || actor->IsDead() || actor->IsDisabled()) {
				g_allyHoldFollower.reset();
				g_allyHoldActive = false;
				return;
			}
			g_allyHoldFollower = actor->GetHandle();
			g_allyHoldActive = true;
			ApplyFollowerHold(actor);
		}

		static float Distance3D(const RE::NiPoint3& a, const RE::NiPoint3& b)
		{
			const float dx = a.x - b.x;
			const float dy = a.y - b.y;
			const float dz = a.z - b.z;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		}

		static float ComputeYawFromVector(float dx, float dy)
		{
			return std::atan2(dx, dy);
		}

		static RE::NiPoint3 ComputeCrowdCenterPoint()
		{
			RE::NiPoint3 center{};
			auto* player = Player();
			if (player) {
				center = player->GetPosition();
			}

			std::size_t count = 0;
			for (auto id : g_lockedFallbackCrowdIds) {
				auto* actorRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				auto* actor = actorRef ? actorRef->As<RE::Actor>() : nullptr;
				if (!actor || actor->IsDead() || actor->IsDisabled()) {
					continue;
				}
				const auto pos = actor->GetPosition();
				center.x += pos.x;
				center.y += pos.y;
				center.z += pos.z;
				++count;
			}

			if (count > 0) {
				const float denom = static_cast<float>(count + (player ? 1 : 0));
				center.x /= denom;
				center.y /= denom;
				center.z /= denom;
			}

			return center;
		}

		static float MinDistanceToLockedCrowd(const RE::NiPoint3& pos)
		{
			float best = std::numeric_limits<float>::max();
			for (auto id : g_lockedFallbackCrowdIds) {
				auto* actorRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				auto* actor = actorRef ? actorRef->As<RE::Actor>() : nullptr;
				if (!actor || actor->IsDead() || actor->IsDisabled()) {
					continue;
				}
				best = (std::min)(best, Distance3D(pos, actor->GetPosition()));
			}
			return best;
		}

		struct FollowerResolution
		{
			RE::Actor* standing{ nullptr };
			RE::Actor* downed{ nullptr };
		};

		static FollowerResolution ResolveFollowerCandidates(float radius)
		{
			FollowerResolution result{};
			auto* player = Player();
			if (!player) {
				return result;
			}

			float bestStandingDist = std::numeric_limits<float>::max();
			float bestDownedDist = std::numeric_limits<float>::max();

			for (auto* actor : CollectRegisteredTeammates()) {
				if (!actor || actor == player) {
					continue;
				}
				const float dist = Distance3D(actor->GetPosition(), player->GetPosition());
				if (radius > 0.0f && dist > (std::max)(radius, 5000.0f)) {
					continue;
				}
				const bool wasDownedThisEvent = g_bleedFollowerDownIds.find(actor->GetFormID()) != g_bleedFollowerDownIds.end();
				const bool downed = actor->IsDead() || IsActorBleedingOut(actor) || wasDownedThisEvent;
				if (!downed) {
					if (dist < bestStandingDist) {
						bestStandingDist = dist;
						result.standing = actor;
					}
				}
				else if (dist < bestDownedDist) {
					bestDownedDist = dist;
					result.downed = actor;
				}
			}

			return result;
		}

		static RE::AlchemyItem* ResolveRecoveryPotionCandidate()
		{
			auto* player = Player();
			if (!player) {
				return nullptr;
			}

			RE::AlchemyItem* bestPotion = nullptr;
			const auto inv = player->GetInventory([](RE::TESBoundObject& obj) {
				if (!obj.Is(RE::FormType::AlchemyItem)) {
					return false;
				}
				auto* potion = obj.As<RE::AlchemyItem>();
				return potion && potion->IsMedicine() && !potion->IsPoison() && !potion->IsFood();
				}, true);

			for (const auto& [item, invData] : inv) {
				const auto& [count, entry] = invData;
				(void)entry;
				if (count <= 0) {
					continue;
				}
				auto* potion = item->As<RE::AlchemyItem>();
				if (!potion) {
					continue;
				}
				if (!bestPotion || potion->GetFormID() < bestPotion->GetFormID()) {
					bestPotion = potion;
				}
			}

			return bestPotion;
		}

		static RE::TESObjectREFR* ResolveCachedRescueDestinationForFallback()
		{
			auto* player = Player();
			const bool preferInterior = player && player->GetParentCell() ? player->GetParentCell()->IsInteriorCell() : true;
			if (auto* dest = TFD::Location::ResolveMostRecentCachedRescueDestination(preferInterior)) {
				return dest;
			}
			if (auto* dest = TFD::Location::ResolveMostRecentCachedRescueDestination(!preferInterior)) {
				return dest;
			}
			return nullptr;
		}

		static RE::BGSLocationRefType* ResolveWETravelRefType()
		{
			static RE::BGSLocationRefType* cached = nullptr;
			static bool tried = false;
			if (tried) {
				return cached;
			}
			tried = true;
			cached = RE::TESForm::LookupByEditorID<RE::BGSLocationRefType>("WETravel");
			if (!cached) {
				cached = RE::TESForm::LookupByEditorID<RE::BGSLocationRefType>("WETravelMarker");
			}
			return cached;
		}

		static void AddLocationChainSimple(std::vector<RE::BGSLocation*>& list, RE::BGSLocation* start)
		{
			for (auto* cur = start; cur; cur = cur->parentLoc) {
				bool seen = false;
				for (auto* existing : list) {
					if (existing == cur) {
						seen = true;
						break;
					}
				}
				if (!seen) {
					list.push_back(cur);
				}
			}
		}

		static bool IsGenericMarkerRef(RE::TESObjectREFR* ref, bool& heading)
		{
			heading = false;
			if (!ref) {
				return false;
			}
			auto* base = ref->GetBaseObject();
			if (!base) {
				return false;
			}
			const auto eid = TFD::Util::GetEditorId(base);
			if (eid == "XMarkerHeading") {
				heading = true;
				return true;
			}
			return eid == "XMarker";
		}

		static void LockCurrentBleedCrowdSnapshot(RE::Actor* preferredSpeaker)
		{
			g_lockedFallbackCrowdIds.clear();
			if (!g_bleedCrowdAssigned.empty()) {
				g_lockedFallbackCrowdIds = g_bleedCrowdAssigned;
			}
			if (g_lockedFallbackCrowdIds.empty()) {
				const float radius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius());
				auto crowd = CollectBleedoutCrowd(radius, preferredSpeaker, true);
				for (auto* actor : crowd) {
					if (actor) {
						g_lockedFallbackCrowdIds.push_back(actor->GetFormID());
					}
				}
			}
			if (preferredSpeaker) {
				const auto preferredId = preferredSpeaker->GetFormID();
				if (std::find(g_lockedFallbackCrowdIds.begin(), g_lockedFallbackCrowdIds.end(), preferredId) == g_lockedFallbackCrowdIds.end()) {
					g_lockedFallbackCrowdIds.insert(g_lockedFallbackCrowdIds.begin(), preferredId);
				}
			}
		}

		static void ApplyFallbackFacing(RE::Actor* actor, float angleZ)
		{
			if (!actor) {
				return;
			}
			actor->data.angle.z = angleZ;
		}

		static void ComputeLocalLeftForDeadFallback(RE::NiPoint3& outPos, float& outAngleZ)
		{
			auto* player = Player();
			if (!player) {
				outPos = {};
				outAngleZ = 0.0f;
				return;
			}
			const auto playerPos = player->GetPosition();
			auto crowdCenter = ComputeCrowdCenterPoint();
			float dx = playerPos.x - crowdCenter.x;
			float dy = playerPos.y - crowdCenter.y;
			const float len = std::sqrt(dx * dx + dy * dy);
			if (len < 1.0f) {
				dx = -std::sin(player->GetAngleZ());
				dy = -std::cos(player->GetAngleZ());
			}
			else {
				dx /= len;
				dy /= len;
			}
			const bool exterior = player->GetParentCell() ? player->GetParentCell()->IsExteriorCell() : true;
			const float dist = exterior ? 2300.0f : 384.0f;
			outPos = playerPos;
			outPos.x += dx * dist;
			outPos.y += dy * dist;
			outAngleZ = ComputeYawFromVector(dx, dy);
		}

		static void ResolveLeftForDeadDestination(NoMarkerFallbackState& state)
		{
			state.destination.reset();
			state.hasFallbackPos = false;
			state.angleZ = 0.0f;

			auto* player = Player();
			auto* playerCell = player ? player->GetParentCell() : nullptr;
			if (!player || !playerCell) {
				return;
			}

			const bool exterior = playerCell->IsExteriorCell();
			const auto playerPos = player->GetPosition();
			const auto crowdCenter = ComputeCrowdCenterPoint();
			auto* playerLoc = GetLocationFromRef(player);

			RE::TESObjectREFR* bestRef = nullptr;
			float bestScore = -1.0e30f;
			float bestAngle = 0.0f;

			auto considerRef = [&](RE::TESObjectREFR* ref, int tier) {
				if (!ref || ref->IsDisabled()) {
					return;
				}
				auto* refCell = ref->GetParentCell();
				if (!refCell) {
					return;
				}
				if (exterior) {
					if (ref->GetWorldspace() != player->GetWorldspace()) {
						return;
					}
				}
				else if (refCell != playerCell) {
					return;
				}
				auto* refLoc = GetLocationFromRef(ref);
				if (playerLoc && refLoc && refLoc != playerLoc) {
					return;
				}
				const auto refPos = ref->GetPosition();
				const float playerDist = Distance3D(playerPos, refPos);
				const float crowdDist = MinDistanceToLockedCrowd(refPos);
				const float minPlayer = exterior ? 2048.0f : 256.0f;
				const float maxPlayer = exterior ? 4096.0f : 2048.0f;
				const float minCrowd = exterior ? 2048.0f : 512.0f;
				if (playerDist < minPlayer || playerDist > maxPlayer) {
					return;
				}
				if (crowdDist != std::numeric_limits<float>::max() && crowdDist < minCrowd) {
					return;
				}
				const float ideal = exterior ? 3072.0f : 1024.0f;
				float score = (tier == 1 ? 600.0f : (tier == 2 ? 300.0f : 0.0f));
				if (crowdDist != std::numeric_limits<float>::max()) {
					score += crowdDist * 2.5f;
				}
				score -= std::abs(playerDist - ideal);
				score -= Distance3D(refPos, crowdCenter) * 0.15f;
				if (score > bestScore) {
					bestScore = score;
					bestRef = ref;
					bestAngle = ComputeYawFromVector(refPos.x - crowdCenter.x, refPos.y - crowdCenter.y);
				}
				};

			if (auto* weTravel = ResolveWETravelRefType()) {
				std::vector<RE::BGSLocation*> chain;
				AddLocationChainSimple(chain, playerLoc);
				for (auto* loc : chain) {
					if (!loc) {
						continue;
					}
					for (std::uint32_t i = 0; i < loc->specialRefs.size(); ++i) {
						const auto& sref = loc->specialRefs[i];
						if (!sref.type || sref.type->GetFormID() != weTravel->GetFormID()) {
							continue;
						}
						auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(sref.refData.refID);
						considerRef(ref, 1);
					}
				}
			}

			playerCell->ForEachReference([&](RE::TESObjectREFR* ref) {
				bool heading = false;
				if (!IsGenericMarkerRef(ref, heading)) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				considerRef(ref, heading ? 2 : 3);
				return RE::BSContainer::ForEachResult::kContinue;
				});

			if (bestRef) {
				state.destination = bestRef->GetHandle();
				state.angleZ = bestAngle;
				spdlog::info("[TFD][Defeat] left-for-dead anchor selected ref={:08X} branch={} score={:.1f}",
					bestRef->GetFormID(),
					NoMarkerBranchName(state.branch),
					bestScore);
				return;
			}

			ComputeLocalLeftForDeadFallback(state.fallbackPos, state.angleZ);
			state.hasFallbackPos = true;
			spdlog::info("[TFD][Defeat] left-for-dead anchor fallback=local branch={} pos=({:.1f},{:.1f},{:.1f})",
				NoMarkerBranchName(state.branch), state.fallbackPos.x, state.fallbackPos.y, state.fallbackPos.z);
		}

		static NoMarkerFallbackBranch ResolveNoMarkerFallback(const char* reason)
		{
			(void)reason;
			g_noMarkerFallback = {};

			RE::Actor* preferredSpeaker = nullptr;
			if (g_lastAggressor) {
				auto sp = RE::Actor::LookupByHandle(g_lastAggressor.native_handle());
				preferredSpeaker = sp.get();
			}
			LockCurrentBleedCrowdSnapshot(preferredSpeaker);

			const float followerRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius() + 400.0f);
			auto followers = ResolveFollowerCandidates(followerRadius);
			if (followers.standing) {
				g_noMarkerFallback.branch = NoMarkerFallbackBranch::RecoveryFollower;
				g_noMarkerFallback.follower = followers.standing->GetHandle();
			}
			else if (followers.downed) {
				g_noMarkerFallback.branch = NoMarkerFallbackBranch::LeftForDeadWithFollower;
				g_noMarkerFallback.follower = followers.downed->GetHandle();
				ResolveLeftForDeadDestination(g_noMarkerFallback);
			}
			else if (auto* potion = ResolveRecoveryPotionCandidate()) {
				g_noMarkerFallback.branch = NoMarkerFallbackBranch::RecoveryPotion;
				g_noMarkerFallback.potionFormId = potion->GetFormID();
			}
			else if (auto* rescueDest = ResolveCachedRescueDestinationForFallback()) {
				g_noMarkerFallback.branch = NoMarkerFallbackBranch::RescueCached;
				g_noMarkerFallback.destination = rescueDest->GetHandle();
			}
			else {
				g_noMarkerFallback.branch = NoMarkerFallbackBranch::LeftForDeadSolo;
				ResolveLeftForDeadDestination(g_noMarkerFallback);
			}

			RE::FormID followerId = 0;
			if (g_noMarkerFallback.follower) {
				auto followerSp = RE::Actor::LookupByHandle(g_noMarkerFallback.follower.native_handle());
				if (auto* follower = followerSp.get()) {
					followerId = follower->GetFormID();
				}
			}
			RE::FormID destId = 0;
			if (g_noMarkerFallback.destination) {
				auto destSp = g_noMarkerFallback.destination.get();
				if (auto* dest = destSp.get()) {
					destId = dest->GetFormID();
				}
			}
			spdlog::info("[TFD][Defeat] no-marker fallback resolved branch={} follower={:08X} potion={:08X} dest={:08X} crowdLocked={}",
				NoMarkerBranchName(g_noMarkerFallback.branch),
				followerId,
				g_noMarkerFallback.potionFormId,
				destId,
				g_lockedFallbackCrowdIds.size());

			return g_noMarkerFallback.branch;
		}

		static void ApplyLeftForDeadWakeState(RE::Actor* actor, bool followerStyle)
		{
			if (!actor) {
				return;
			}
			actor->NotifyAnimationGraph("BleedoutStop");
			actor->NotifyAnimationGraph("GetUpStart");
			const float hpMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float targetHp = (std::max)(followerStyle ? 24.0f : 18.0f, hpMax * (followerStyle ? 0.28f : 0.22f));
			const float hpNow = actor->GetActorValue(RE::ActorValue::kHealth);
			if (hpNow < targetHp) {
				actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, targetHp - hpNow);
			}
			const float staminaMax = (std::max)(1.0f, actor->GetPermanentActorValue(RE::ActorValue::kStamina));
			const float staminaTarget = (std::max)(15.0f, staminaMax * 0.25f);
			const float staminaNow = actor->GetActorValue(RE::ActorValue::kStamina);
			if (staminaNow < staminaTarget) {
				actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina, staminaTarget - staminaNow);
			}
			if (actor->IsInCombat()) {
				actor->StopCombat();
			}
			actor->DrawWeaponMagicHands(false);
		}

		static void MoveFollowerNearPlayerForLeftForDead(RE::Actor* follower)
		{
			auto* player = Player();
			if (!player || !follower) {
				return;
			}
			const bool interior = player->GetParentCell() ? player->GetParentCell()->IsInteriorCell() : false;
			const float offset = interior ? 128.0f : 220.0f;
			const float yaw = player->GetAngleZ();
			RE::NiPoint3 pos = player->GetPosition();
			pos.x += std::cos(yaw) * offset;
			pos.y -= std::sin(yaw) * offset;
			if (!follower->IsDead()) {
				follower->MoveTo(player);
			}
			follower->SetPosition(pos, true);
			ApplyFallbackFacing(follower, yaw);
			if (!follower->IsDead()) {
				ApplyLeftForDeadWakeState(follower, true);
				SetFollowerHold(follower);
			}
		}

		static void ExecuteLeftForDeadWake(const char* reason)
		{
			auto* player = Player();
			if (!player) {
				return;
			}
			if (g_noMarkerFallback.destination) {
				auto refSp = g_noMarkerFallback.destination.get();
				if (auto* dest = refSp.get()) {
					player->MoveTo(dest);
				}
			}
			else if (g_noMarkerFallback.hasFallbackPos) {
				player->SetPosition(g_noMarkerFallback.fallbackPos, true);
			}
			ApplyFallbackFacing(player, g_noMarkerFallback.angleZ);
			ApplyLeftForDeadWakeState(player, false);
			if (g_noMarkerFallback.branch == NoMarkerFallbackBranch::LeftForDeadWithFollower && g_noMarkerFallback.follower) {
				auto followerSp = RE::Actor::LookupByHandle(g_noMarkerFallback.follower.native_handle());
				if (auto* follower = followerSp.get()) {
					MoveFollowerNearPlayerForLeftForDead(follower);
				}
			}
			ClearBleedoutBridgeAliases(nullptr, "left_for_dead_escape_state");
			g_bleedCrowdAssigned.clear();
			MaintainTransitionCalmWindow();
			g_leftForDeadNeedsAggroKick = false;
			BeginLeftForDeadCooldown(5);
			SetGraceSeconds(5);
			UpdatePreCombatState();
			spdlog::info("[TFD][Transition] left-for-dead complete branch={} reason={}",
				NoMarkerBranchName(g_noMarkerFallback.branch),
				reason ? reason : "unknown");
		}

		static void ResolveGlobals()
		{
			if (!g_captiveStateGlobal) {
				g_captiveStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDCaptiveState");
				if (g_captiveStateGlobal && !g_loggedCaptiveStateGlobal) {
					g_loggedCaptiveStateGlobal = true;
					spdlog::info("[TFD][Defeat] TFDCaptiveState resolved {:08X}", g_captiveStateGlobal->GetFormID());
				}
			}
			if (!g_captivePhaseGlobal) {
				g_captivePhaseGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDCaptivePhase");
				if (g_captivePhaseGlobal && !g_loggedCaptivePhaseGlobal) {
					g_loggedCaptivePhaseGlobal = true;
					spdlog::info("[TFD][Defeat] TFDCaptivePhase resolved {:08X}", g_captivePhaseGlobal->GetFormID());
				}
			}
			if (!g_preCombatStateGlobal) {
				g_preCombatStateGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDPreCombatState");
				if (g_preCombatStateGlobal && !g_loggedPreCombatStateGlobal) {
					g_loggedPreCombatStateGlobal = true;
					spdlog::info("[TFD][Defeat] TFDPreCombatState resolved {:08X}", g_preCombatStateGlobal->GetFormID());
				}
			}
			if (!g_transitionPendingGlobal) {
				g_transitionPendingGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDTransitionPending");
				if (g_transitionPendingGlobal && !g_loggedTransitionPendingGlobal) {
					g_loggedTransitionPendingGlobal = true;
					spdlog::info("[TFD][Transition] TFDTransitionPending resolved {:08X}", g_transitionPendingGlobal->GetFormID());
				}
			}
			if (!g_transitionBusyGlobal) {
				g_transitionBusyGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDTransitionBusy");
				if (g_transitionBusyGlobal && !g_loggedTransitionBusyGlobal) {
					g_loggedTransitionBusyGlobal = true;
					spdlog::info("[TFD][Transition] TFDTransitionBusy resolved {:08X}", g_transitionBusyGlobal->GetFormID());
				}
			}
			if (!g_transitionReasonGlobal) {
				g_transitionReasonGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDTransitionReason");
				if (g_transitionReasonGlobal && !g_loggedTransitionReasonGlobal) {
					g_loggedTransitionReasonGlobal = true;
					spdlog::info("[TFD][Transition] TFDTransitionReason resolved {:08X}", g_transitionReasonGlobal->GetFormID());
				}
			}
			if (!g_transitionResultGlobal) {
				g_transitionResultGlobal = RE::TESForm::LookupByEditorID<RE::TESGlobal>("TFDTransitionResult");
				if (g_transitionResultGlobal && !g_loggedTransitionResultGlobal) {
					g_loggedTransitionResultGlobal = true;
					spdlog::info("[TFD][Transition] TFDTransitionResult resolved {:08X}", g_transitionResultGlobal->GetFormID());
				}
			}
		}

		static int CinematicReasonCode(CinematicTransitionKind kind, bool fadeIn)
		{
			switch (kind) {
			case CinematicTransitionKind::Captive: return fadeIn ? 21 : 11;
			case CinematicTransitionKind::Rescue:  return fadeIn ? 22 : 12;
			case CinematicTransitionKind::Recover: return fadeIn ? 23 : 13;
			default: return 0;
			}
		}

		static const char* CinematicKindName(CinematicTransitionKind kind)
		{
			switch (kind) {
			case CinematicTransitionKind::Captive: return "captive";
			case CinematicTransitionKind::Rescue: return "rescue";
			case CinematicTransitionKind::Recover: return "recover";
			default: return "none";
			}
		}

		static bool QueueCinematicTransitionRequest(CinematicTransitionKind kind, bool fadeIn, const char* reason)
		{
			ResolveGlobals();
			if (!g_transitionPendingGlobal || !g_transitionReasonGlobal || !g_transitionResultGlobal) {
				spdlog::warn("[TFD][Transition] cinematic request unavailable kind={} phase={} reason={}",
					CinematicKindName(kind), fadeIn ? "fadein" : "fadeout", reason ? reason : "unknown");
				return false;
			}
			if (g_transitionBusyGlobal && g_transitionBusyGlobal->value >= 0.5f) {
				spdlog::info("[TFD][Transition] cinematic request skipped (busy) kind={} phase={} reason={}",
					CinematicKindName(kind), fadeIn ? "fadein" : "fadeout", reason ? reason : "unknown");
				return false;
			}
			if (g_transitionPendingGlobal->value >= 0.5f) {
				spdlog::info("[TFD][Transition] cinematic request skipped (pending={}) kind={} phase={} reason={}",
					g_transitionPendingGlobal->value, CinematicKindName(kind), fadeIn ? "fadein" : "fadeout", reason ? reason : "unknown");
				return false;
			}
			const int code = CinematicReasonCode(kind, fadeIn);
			if (code == 0) {
				return false;
			}
			g_transitionReasonGlobal->value = static_cast<float>(code);
			g_transitionResultGlobal->value = 0.0f;
			g_transitionPendingGlobal->value = 2.0f;
			spdlog::info("[TFD][Transition] queued cinematic kind={} phase={} code={} reason={}",
				CinematicKindName(kind), fadeIn ? "fadein" : "fadeout", code, reason ? reason : "unknown");
			return true;
		}

		static void ClearPendingCinematicFadeIn()
		{
			g_pendingCinematicFadeIn = false;
			g_pendingCinematicFadeInKind = CinematicTransitionKind::None;
			g_pendingCinematicFadeInNotBefore = {};
			g_pendingCinematicFadeInSawLoadingMenu = false;
		}

		static void SchedulePendingCinematicFadeIn(CinematicTransitionKind kind, const char* reason, int settleMs = 300)
		{
			if (kind == CinematicTransitionKind::None) {
				ClearPendingCinematicFadeIn();
				return;
			}
			g_pendingCinematicFadeIn = true;
			g_pendingCinematicFadeInKind = kind;
			g_pendingCinematicFadeInNotBefore = Now() + std::chrono::milliseconds((std::max)(0, settleMs));
			g_pendingCinematicFadeInSawLoadingMenu = false;
			spdlog::info("[TFD][Transition] scheduled fadein kind={} settleMs={} reason={}",
				CinematicKindName(kind), settleMs, reason ? reason : "unknown");
		}

		static void ProcessPendingCinematicFadeIn()
		{
			if (!g_pendingCinematicFadeIn || g_pendingCinematicFadeInKind == CinematicTransitionKind::None) {
				return;
			}

			auto* ui = RE::UI::GetSingleton();
			const bool loadingOpen = ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
			if (loadingOpen) {
				g_pendingCinematicFadeInSawLoadingMenu = true;
				return;
			}

			if (Now() < g_pendingCinematicFadeInNotBefore) {
				return;
			}

			auto* player = Player();
			auto* cell = player ? player->GetParentCell() : nullptr;
			if (!player || !cell) {
				return;
			}

			const auto kind = g_pendingCinematicFadeInKind;
			const bool sawLoading = g_pendingCinematicFadeInSawLoadingMenu;
			ClearPendingCinematicFadeIn();

			spdlog::info("[TFD][Transition] fadein ready kind={} cell={:08X} path={}",
				CinematicKindName(kind), cell->GetFormID(), sawLoading ? "after_loading_close" : "after_settle");

			if (!QueueCinematicTransitionRequest(kind, true, sawLoading ? "fadein_after_loading_close" : "fadein_after_settle")) {
				HideBlackoutFader();
			}
		}

		static bool ResolveCaptiveMarkerForOutcome()
		{
			auto* aggressor = ResolveAggressor();
			bool resolved = false;
			if (aggressor) {
				resolved = TFD::Location::RescanCaptiveMarkerWithAggressor(aggressor, true);
				if (!resolved) {
					resolved = TFD::Location::RescanCaptiveMarkerWithAggressor(aggressor, false);
				}
			}
			if (!resolved) {
				resolved = TFD::Location::RescanCaptiveMarker();
			}
			return resolved && TFD::Location::GetCachedCaptiveMarker();
		}

		static bool TeleportPlayerToCachedMarkerNow()
		{
			auto* player = Player();
			auto* marker = TFD::Location::GetCachedCaptiveMarker();
			if (!player || !marker) {
				return false;
			}
			player->MoveTo(marker);
			spdlog::info("[TFD][Location] Direct MoveTo cached marker {:08X}", marker->GetFormID());
			return true;
		}

		static int ResolveNonCaptiveChoiceReason(const char* reason)
		{
			if (!reason || !reason[0]) {
				return 4;
			}
			if (std::strcmp(reason, "bleed_timeout") == 0) {
				return 2;
			}
			if (std::strcmp(reason, "dialogue_closed_no_marker") == 0) {
				return 3;
			}
			if (std::strcmp(reason, "no_valid_npc") == 0 ||
				std::strcmp(reason, "unsupported_aggressor_nonhumanoid") == 0 ||
				std::strcmp(reason, "unsupported_aggressor_allowlist") == 0) {
				return 1;
			}
			return 4;
		}

		static void QueueNonCaptiveChoiceRequest(const char* reason)
		{
			ResolveGlobals();
			if (!g_transitionPendingGlobal) {
				spdlog::warn("[TFD][Transition] non-captive choice skipped (globals missing) reason={}", reason ? reason : "unknown");
				return;
			}
			if (g_transitionBusyGlobal && g_transitionBusyGlobal->value >= 0.5f) {
				spdlog::info("[TFD][Transition] non-captive choice skipped (busy) reason={}", reason ? reason : "unknown");
				return;
			}
			if (g_transitionPendingGlobal->value >= 0.5f) {
				spdlog::info("[TFD][Transition] non-captive choice skipped (already pending={}) reason={}", g_transitionPendingGlobal->value, reason ? reason : "unknown");
				return;
			}
			const int mappedReason = ResolveNonCaptiveChoiceReason(reason);
			if (g_transitionReasonGlobal) {
				g_transitionReasonGlobal->value = static_cast<float>(mappedReason);
			}
			if (g_transitionResultGlobal) {
				g_transitionResultGlobal->value = 0.0f;
			}
			g_transitionPendingGlobal->value = 1.0f;
			spdlog::info("[TFD][Transition] queued non-captive choice reason={} source={}", mappedReason, reason ? reason : "unknown");
		}

		static int ConsumeTransitionResult()
		{
			ResolveGlobals();
			if (!g_transitionResultGlobal) {
				return 0;
			}
			const int result = static_cast<int>(std::lround(g_transitionResultGlobal->value));
			if (result != 0) {
				g_transitionResultGlobal->value = 0.0f;
			}
			return result;
		}

		static bool IsTransitionAwaiting()
		{
			ResolveGlobals();
			const bool pending = g_transitionPendingGlobal && g_transitionPendingGlobal->value >= 0.5f;
			const bool busy = g_transitionBusyGlobal && g_transitionBusyGlobal->value >= 0.5f;
			return pending || busy;
		}

		static void MaintainTransitionCalmWindow()
		{
			auto* player = Player();
			if (!player) {
				return;
			}
			if (player->IsInCombat()) {
				player->StopCombat();
			}
			player->DrawWeaponMagicHands(false);
			const float radius = (std::max)(3200.0f, TFD::Settings::GetSweepRadius() + 1200.0f);
			TFD::AntiAggro::SweepOnce(radius, false);

			std::size_t applied = 0;
			if (!g_lockedFallbackCrowdIds.empty()) {
				for (auto id : g_lockedFallbackCrowdIds) {
					auto* actorRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
					auto* actor = actorRef ? actorRef->As<RE::Actor>() : nullptr;
					if (!actor || actor->IsDead() || actor->IsDisabled() || IsActiveFollowerActor(actor)) {
						continue;
					}
					TFD::AggressionClamp::Apply(actor);
					actor->StopCombat();
					actor->EvaluatePackage(false, true);
					actor->EvaluatePackage(true, true);
					++applied;
				}
			}
			else {
				TFD::ActorScan::Rescan(radius, false);
				const auto count = TFD::ActorScan::GetCount();
				for (int i = 0; i < count; ++i) {
					auto entry = TFD::ActorScan::GetEntry(i);
					auto actorSP = entry.actor.get();
					auto* actor = actorSP.get();
					if (!actor || actor->IsDead() || actor->IsDisabled() || IsActiveFollowerActor(actor)) {
						continue;
					}
					TFD::AggressionClamp::Apply(actor);
					actor->StopCombat();
					actor->EvaluatePackage(false, true);
					actor->EvaluatePackage(true, true);
					++applied;
				}
			}

			MaintainFollowerHold();
			spdlog::info("[TFD][Transition] calm window maintained applied={} branch={} lockedCrowd={}",
				applied,
				NoMarkerBranchName(g_noMarkerFallback.branch),
				g_lockedFallbackCrowdIds.size());
		}

		static RE::TESObjectREFR* LookupRefByFormID(std::uint32_t formID)
		{
			if (formID == 0) {
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

		static bool CompleteRescueTransitionNow(const char* reason)
		{
			auto* player = Player();
			if (!player) {
				return false;
			}

			RE::BGSLocation* safeLoc = nullptr;
			RE::TESObjectREFR* dest = nullptr;

			if (g_noMarkerFallback.branch == NoMarkerFallbackBranch::RescueCached && g_noMarkerFallback.destination) {
				auto destSp = g_noMarkerFallback.destination.get();
				dest = destSp.get();
				safeLoc = GetLocationFromRef(dest);
			}

			if (!dest) {
				safeLoc = TFD::Location::ResolveRescueTargetLocationFromRef(player);
				dest = safeLoc ? ResolveBestRescueDestination(safeLoc) : nullptr;

				if ((!safeLoc || !dest)) {
					auto* fallbackLoc = TFD::Location::GetMostRecentCachedSafeLocation();
					if (fallbackLoc) {
						auto* fallbackDest = ResolveBestRescueDestination(fallbackLoc);
						if (fallbackDest) {
							spdlog::info("[TFD][Transition] rescue fallback to recent cache reason={} currentLoc={:08X} fallbackLoc={:08X}",
								reason ? reason : "unknown",
								safeLoc ? safeLoc->GetFormID() : 0,
								fallbackLoc->GetFormID());
							safeLoc = fallbackLoc;
							dest = fallbackDest;
						}
					}
				}
			}

			if (!dest) {
				spdlog::info("[TFD][Transition] rescue unavailable reason={} cause=no_destination", reason ? reason : "unknown");
				return false;
			}

			player->MoveTo(dest);
			std::this_thread::sleep_for(std::chrono::milliseconds(120));
			RecoverPlayerForTransition();
			MaintainTransitionCalmWindow();
			g_leftForDeadNeedsAggroKick = false;
			const int grace = (g_noMarkerFallback.branch == NoMarkerFallbackBranch::RescueCached) ? 1 : 4;
			BeginLeftForDeadCooldown(grace);
			SetGraceSeconds(grace);
			UpdatePreCombatState();
			spdlog::info("[TFD][Transition] rescue complete reason={} safeLoc={:08X} dest={:08X} branch={}",
				reason ? reason : "unknown", safeLoc ? safeLoc->GetFormID() : 0u, dest->GetFormID(), NoMarkerBranchName(g_noMarkerFallback.branch));
			return true;
		}

		static bool BeginRescueTransition(const char* reason)
		{
			ClearPendingCinematicFadeIn();
			if (QueueCinematicTransitionRequest(CinematicTransitionKind::Rescue, false, reason)) {
				return true;
			}
			ShowBlackoutFader();
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			const bool ok = CompleteRescueTransitionNow(reason);
			std::this_thread::sleep_for(std::chrono::milliseconds(400));
			HideBlackoutFader();
			return ok;
		}

		static void CompleteRecoverTransitionNow(const char* reason)
		{
			if (g_noMarkerFallback.branch == NoMarkerFallbackBranch::RecoveryFollower) {
				auto followerSp = RE::Actor::LookupByHandle(g_noMarkerFallback.follower.native_handle());
				auto* follower = followerSp.get();
				if (!follower || follower->IsDead() || IsActorBleedingOut(follower)) {
					g_noMarkerFallback.branch = follower ? NoMarkerFallbackBranch::LeftForDeadWithFollower : NoMarkerFallbackBranch::LeftForDeadSolo;
					ResolveLeftForDeadDestination(g_noMarkerFallback);
					ExecuteLeftForDeadWake(reason ? reason : "recovery_follower_degraded_lfd");
					return;
				}
				RecoverPlayerForTransition();
				SetFollowerHold(follower);
				MaintainTransitionCalmWindow();
				g_leftForDeadNeedsAggroKick = false;
				BeginLeftForDeadCooldown(3);
				SetGraceSeconds(3);
				UpdatePreCombatState();
				spdlog::info("[TFD][Transition] recover complete branch={} reason={} follower={:08X}",
					NoMarkerBranchName(g_noMarkerFallback.branch), reason ? reason : "unknown", follower->GetFormID());
				return;
			}

			if (g_noMarkerFallback.branch == NoMarkerFallbackBranch::RecoveryPotion) {
				RecoverPlayerForTransition();
				MaintainTransitionCalmWindow();
				g_leftForDeadNeedsAggroKick = false;
				BeginLeftForDeadCooldown(3);
				SetGraceSeconds(3);
				UpdatePreCombatState();
				spdlog::info("[TFD][Transition] recover complete branch={} reason={} potion={:08X}",
					NoMarkerBranchName(g_noMarkerFallback.branch), reason ? reason : "unknown", g_noMarkerFallback.potionFormId);
				return;
			}

			if (g_noMarkerFallback.branch == NoMarkerFallbackBranch::LeftForDeadSolo ||
				g_noMarkerFallback.branch == NoMarkerFallbackBranch::LeftForDeadWithFollower) {
				ExecuteLeftForDeadWake(reason);
				return;
			}

			RecoverPlayerForTransition();
			MaintainTransitionCalmWindow();
			g_leftForDeadNeedsAggroKick = false;
			BeginLeftForDeadCooldown(3);
			SetGraceSeconds(3);
			UpdatePreCombatState();
			spdlog::info("[TFD][Transition] recover complete branch={} reason={}", NoMarkerBranchName(g_noMarkerFallback.branch), reason ? reason : "unknown");
		}

		static void BeginRecoverTransition(const char* reason)
		{
			ClearPendingCinematicFadeIn();
			if (QueueCinematicTransitionRequest(CinematicTransitionKind::Recover, false, reason)) {
				return;
			}
			ShowBlackoutFader();
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			CompleteRecoverTransitionNow(reason);
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			HideBlackoutFader();
		}

		static void CompleteCaptiveTransitionNow(const char* reason)
		{
			ClearNoMarkerFallbackState();
			ResetBleedRuntimeState();
			ClearBleedoutBridgeAliases(nullptr, "blackout_teleport");
			TFD::ForceGreet::Cancel();
			g_lastAggressor.reset();
			AdvanceGameHoursSoft(1.0f);
			if (!TeleportPlayerToCachedMarkerNow()) {
				EnterNonCaptiveChoice("teleport_failed");
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(120));
			RecoverPlayerAfterTeleport();
			SetGraceSeconds(5);
			SetCaptiveRuntime(true, CaptivePhaseValue::Captive);
			g_prevDialogueOpen = IsDialogueOpen();
			g_prevLockpickOpen = IsLockpickingOpen();
			ResetLockpickWatch();
			ArmEscapeContextFromCurrentState();
			if (g_captiveDoor.HasDoor()) {
				g_captiveDoor.SealToInitial(true);
			}
			ApplyCalmBubble((std::max)(2000.0f, TFD::Settings::GetSweepRadius()));
			spdlog::info("[TFD][Captive] entered captivePhase reason={}", reason ? reason : "unknown");
		}

		static void PollTransitionResult()
		{
			const int result = ConsumeTransitionResult();
			if (result == 0) {
				return;
			}
			if (result == 1) {
				spdlog::info("[TFD][Transition] result=completed");
				return;
			}
			if (result == 2) {
				spdlog::info("[TFD][Transition] result=cancelled");
				return;
			}
			if (result == 3) {
				spdlog::info("[TFD][Transition] result=recover_chosen");
				BeginRecoverTransition("recover_chosen");
				return;
			}
			if (result == 4) {
				spdlog::info("[TFD][Transition] result=rescue_chosen");
				if (!BeginRescueTransition("rescue_chosen")) {
					BeginRecoverTransition("rescue_fallback_recover");
				}
				return;
			}
			if (result == 101) {
				spdlog::info("[TFD][Transition] result=captive_blackout_ready");
				CompleteCaptiveTransitionNow("captived_blackout_ready");
				SchedulePendingCinematicFadeIn(CinematicTransitionKind::Captive, "captived_fadein", 350);
				return;
			}
			if (result == 102) {
				spdlog::info("[TFD][Transition] result=rescue_blackout_ready");
				if (!CompleteRescueTransitionNow("rescue_blackout_ready")) {
					g_noMarkerFallback.branch = NoMarkerFallbackBranch::LeftForDeadSolo;
					g_noMarkerFallback.destination.reset();
					g_noMarkerFallback.hasFallbackPos = false;
					ResolveLeftForDeadDestination(g_noMarkerFallback);
					CompleteRecoverTransitionNow("rescue_fallback_left_for_dead");
					SchedulePendingCinematicFadeIn(CinematicTransitionKind::Recover, "rescue_fallback_left_for_dead_fadein", 300);
				}
				else {
					SchedulePendingCinematicFadeIn(CinematicTransitionKind::Rescue, "rescue_fadein", 350);
				}
				return;
			}
			if (result == 103) {
				spdlog::info("[TFD][Transition] result=recover_blackout_ready");
				CompleteRecoverTransitionNow("recover_blackout_ready");
				SchedulePendingCinematicFadeIn(CinematicTransitionKind::Recover, "recover_fadein", 300);
				return;
			}
			if (result == 201 || result == 202 || result == 203) {
				spdlog::info("[TFD][Transition] result=fadein_complete code={}", result);
				return;
			}
			spdlog::info("[TFD][Transition] result={} (unknown)", result);
		}

		static void SyncCaptiveGlobals(bool stateActive, CaptivePhaseValue phase)
		{
			ResolveGlobals();
			if (g_captiveStateGlobal) {
				g_captiveStateGlobal->value = stateActive ? 1.0f : 0.0f;
			}
			if (g_captivePhaseGlobal) {
				g_captivePhaseGlobal->value = static_cast<float>(static_cast<int>(phase));
			}
		}

		static void SyncPreCombatGlobal(bool active)
		{
			ResolveGlobals();
			if (g_preCombatStateGlobal) {
				g_preCombatStateGlobal->value = active ? 1.0f : 0.0f;
			}
		}

		static void SetCaptiveRuntimeOnly(bool stateActive, CaptivePhaseValue phase)
		{
			g_captiveState = stateActive;
			g_captivePhase = phase;
		}

		static void SetCaptiveRuntime(bool stateActive, CaptivePhaseValue phase)
		{
			SetCaptiveRuntimeOnly(stateActive, phase);
			SyncCaptiveGlobals(stateActive, phase);
		}

		static void UpdatePreCombatState()
		{
			auto* player = Player();
			bool preCombat = false;
			if (player) {
				preCombat = true;
				if (g_loadTransition.load(std::memory_order_acquire)) preCombat = false;
				if (g_inBleedState.load(std::memory_order_acquire)) preCombat = false;
				if (g_captiveState) preCombat = false;
				if (player->IsInCombat()) preCombat = false;
				if (g_leftForDeadActive) preCombat = false;
				auto* st = player->AsActorState();
				if (st && st->IsBleedingOut()) preCombat = false;
			}
			SyncPreCombatGlobal(preCombat);
		}

		static bool IsDialogueOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
		}

		static bool IsLockpickingOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && ui->IsMenuOpen(RE::LockpickingMenu::MENU_NAME);
		}

		static void ResetLockpickWatch()
		{
			g_lockpickDoorCandidate.reset();
			g_lockpickDoorWasLocked = false;
			g_prevLockpickOpen = false;
		}

		static bool IsDoorRef(RE::TESObjectREFR* ref)
		{
			if (!ref) return false;
			auto* base = ref->GetBaseObject();
			if (!base) return false;
			return base->GetFormType() == RE::FormType::Door;
		}

		static bool IsRefLocked(RE::TESObjectREFR* ref)
		{
			if (!ref) return false;
			auto* lock = ref->GetLock();
			return lock && lock->IsLocked();
		}

		static RE::TESObjectREFR* ResolveBoundEscapeDoor()
		{
			if (!g_boundEscapeDoor) return nullptr;
			auto ptr = g_boundEscapeDoor.get();
			return ptr.get();
		}

		static void BindCaptiveDoor(RE::TESObjectREFR* door)
		{
			if (!door) return;
			g_captiveDoor.Bind(door);
			g_boundEscapeDoor = door->GetHandle();
		}

		static RE::TESObjectCELL* GetParentCell(RE::TESObjectREFR* ref)
		{
			return ref ? ref->GetParentCell() : nullptr;
		}

		static RE::BGSLocation* GetLocationFromRef(RE::TESObjectREFR* ref)
		{
			return TFD::Location::GetLocationFromRef(ref);
		}

		static RE::TESObjectREFR* FindNearestDoorNearCaptiveMarker()
		{
			RE::TESObjectREFR* marker = nullptr;
			if (g_captiveMarker) {
				auto ptr = g_captiveMarker.get();
				marker = ptr.get();
			}
			if (!marker) marker = TFD::Location::GetCachedCaptiveMarker();
			if (!marker) return nullptr;
			auto* cell = marker->GetParentCell();
			if (!cell) return nullptr;
			const auto mp = marker->GetPosition();
			RE::TESObjectREFR* best = nullptr;
			double bestDistSq = kCaptiveEscapeDoorRadius * kCaptiveEscapeDoorRadius;
			cell->ForEachReferenceInRange(mp, static_cast<float>(kCaptiveEscapeDoorRadius), [&](RE::TESObjectREFR* candidate) -> RE::BSContainer::ForEachResult {
				if (!candidate || candidate == marker) return RE::BSContainer::ForEachResult::kContinue;
				if (!IsDoorRef(candidate)) return RE::BSContainer::ForEachResult::kContinue;
				const auto rp = candidate->GetPosition();
				const double dx = static_cast<double>(rp.x - mp.x);
				const double dy = static_cast<double>(rp.y - mp.y);
				const double dz = static_cast<double>(rp.z - mp.z);
				const double distSq = dx * dx + dy * dy + dz * dz;
				if (distSq <= bestDistSq) {
					bestDistSq = distSq;
					best = candidate;
				}
				return RE::BSContainer::ForEachResult::kContinue;
				});
			return best;
		}

		static void ClearEscapeContext()
		{
			g_captiveMarker.reset();
			g_captiveCellFormID = 0;
			g_captiveLocationFormID = 0;
			g_escapeRadiusActive = false;
			g_escapeRadiusSince = {};
			g_boundEscapeDoor.reset();
			g_captiveDoor.Reset();
		}

		static void ArmEscapeContextFromCurrentState()
		{
			auto* player = Player();
			if (!player) return;
			auto* marker = TFD::Location::GetCachedCaptiveMarker();
			if (!marker) {
				TFD::Location::RescanCaptiveMarker();
				marker = TFD::Location::GetCachedCaptiveMarker();
			}
			g_captiveMarker = marker ? marker->GetHandle() : RE::ObjectRefHandle{};
			auto* cell = player->GetParentCell();
			g_captiveCellFormID = cell ? cell->GetFormID() : 0;
			auto* loc = cell ? cell->GetLocation() : nullptr;
			g_captiveLocationFormID = loc ? loc->GetFormID() : 0;
			g_escapeRadiusActive = false;
			g_escapeRadiusSince = {};
			if (!g_captiveDoor.HasDoor()) {
				if (auto* door = FindNearestDoorNearCaptiveMarker()) {
					BindCaptiveDoor(door);
					spdlog::info("[TFD][Captive] Bound nearest captive door {:08X} on captive enter", door->GetFormID());
				}
			}
			spdlog::info("[TFD][Captive] Escape context armed marker={:08X} cell={:08X} loc={:08X}", marker ? marker->GetFormID() : 0, g_captiveCellFormID, g_captiveLocationFormID);
		}

		static bool IsDoorNearCaptiveMarker(RE::TESObjectREFR* door)
		{
			if (!door || !IsDoorRef(door)) return false;
			RE::TESObjectREFR* marker = nullptr;
			if (g_captiveMarker) {
				auto ptr = g_captiveMarker.get();
				marker = ptr.get();
			}
			if (!marker) marker = TFD::Location::GetCachedCaptiveMarker();
			if (!marker) return true;
			const auto dp = door->GetPosition();
			const auto mp = marker->GetPosition();
			const double dx = static_cast<double>(dp.x - mp.x);
			const double dy = static_cast<double>(dp.y - mp.y);
			const double dz = static_cast<double>(dp.z - mp.z);
			const double distSq = dx * dx + dy * dy + dz * dz;
			return distSq <= (kCaptiveEscapeDoorRadius * kCaptiveEscapeDoorRadius);
		}

		static RE::Actor* ResolveAggressor();
		static RE::Actor* FindBestAggressor(float radius);

		static RE::TESObjectREFR* ResolveLockpickDoorCandidate(RE::TESObjectREFR* target)
		{
			if (target && IsDoorRef(target) && IsDoorNearCaptiveMarker(target)) return target;
			auto* boundDoor = ResolveBoundEscapeDoor();
			if (boundDoor && IsDoorRef(boundDoor) && IsDoorNearCaptiveMarker(boundDoor)) return boundDoor;
			return nullptr;
		}

		static void EnterEscapeCommit(const char* reason, RE::TESObjectREFR* door)
		{
			if (!g_captiveState || g_captivePhase != CaptivePhaseValue::Captive) return;
			TFD::ForceGreet::Cancel();
			TFD::FactionMask::Clear();
			TFD::AggressionClamp::Clear();
			g_grace.store(false, std::memory_order_release);
			SetCaptiveRuntime(true, CaptivePhaseValue::Escape);
			UpdatePreCombatState();
			ResetLockpickWatch();
			g_prevDialogueOpen = false;
			if (door) BindCaptiveDoor(door); else door = ResolveBoundEscapeDoor();
			auto* player = Player();
			if (player) player->EvaluatePackage(true, false);
			RE::Actor* aggressor = ResolveAggressor();
			if (!aggressor) {
				const float radius = (std::max)(1800.0f, TFD::Settings::GetSweepRadius());
				aggressor = FindBestAggressor(radius);
				if (aggressor) g_lastAggressor = aggressor->GetHandle();
			}
			if (aggressor) {
				aggressor->EvaluatePackage(true, false);
				spdlog::info("[TFD][Captive] Escape aggro nudge actor={:08X}", aggressor->GetFormID());
			}
			else {
				spdlog::info("[TFD][Captive] Escape aggro nudge skipped (no aggressor)");
			}
			spdlog::info("[TFD][Captive] EscapeCommit reason={} door={:08X}", reason ? reason : "unknown", door ? door->GetFormID() : 0);
		}

		static void TryCommitEscapeByRadius()
		{
			if (!g_captiveState || g_captivePhase != CaptivePhaseValue::Captive) {
				g_escapeRadiusActive = false;
				return;
			}
			auto* player = Player();
			if (!player) return;
			RE::TESObjectREFR* marker = nullptr;
			if (g_captiveMarker) {
				auto ptr = g_captiveMarker.get();
				marker = ptr.get();
			}
			if (!marker) marker = TFD::Location::GetCachedCaptiveMarker();
			if (!marker) return;
			const auto pp = player->GetPosition();
			const auto mp = marker->GetPosition();
			const double dx = static_cast<double>(pp.x - mp.x);
			const double dy = static_cast<double>(pp.y - mp.y);
			const double dz = static_cast<double>(pp.z - mp.z);
			const double distSq = dx * dx + dy * dy + dz * dz;
			const bool outside = distSq > (512.0 * 512.0);
			if (!outside) {
				g_escapeRadiusActive = false;
				return;
			}
			if (!g_escapeRadiusActive) {
				g_escapeRadiusActive = true;
				g_escapeRadiusSince = Now();
				return;
			}
			const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(Now() - g_escapeRadiusSince).count();
			if (held >= 2000) {
				EnterEscapeCommit("marker_radius", nullptr);
				g_escapeRadiusActive = false;
			}
		}

		static void TryResolveEscapeByLocation()
		{
			if (!g_captiveState || g_captivePhase != CaptivePhaseValue::Escape) return;
			auto* player = Player();
			if (!player) return;
			auto* loc = GetLocationFromRef(player);
			const auto curLoc = loc ? loc->GetFormID() : 0;
			if (g_captiveLocationFormID != 0 && curLoc != 0 && curLoc != g_captiveLocationFormID) {
				SetCaptiveRuntime(false, CaptivePhaseValue::None);
				ClearEscapeContext();
				UpdatePreCombatState();
				spdlog::info("[TFD][Captive] EscapeResolved by location old={:08X} new={:08X}", g_captiveLocationFormID, curLoc);
			}
		}

		static bool BreakEscapeOnDefeatThreshold(RE::Actor* player)
		{
			if (!player) return false;
			if (!g_captiveState || g_captivePhase != CaptivePhaseValue::Escape) return false;
			const float hpNow = player->GetActorValue(RE::ActorValue::kHealth);
			const float hpMax = (std::max)(1.0f, player->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float pct = (hpNow / hpMax) * 100.0f;
			const float thresh = TFD::Settings::GetDefeatThresholdPct();
			if (pct > thresh) return false;

			g_lastAggressor.reset();
			SetCaptiveRuntime(false, CaptivePhaseValue::None);
			ClearEscapeContext();
			UpdatePreCombatState();
			spdlog::info("[TFD][Captive] Escape broken by defeat threshold pct={:.1f} thresh={:.1f} -> clear stale aggressor", pct, thresh);
			return true;
		}

		static void NormalizeInvalidCaptivePair()
		{
			if (g_captiveState && g_captivePhase == CaptivePhaseValue::None) {
				SetCaptiveRuntime(true, CaptivePhaseValue::Escape);
				spdlog::info("[TFD][Captive] Normalized invalid captive pair -> Escape");
			}
		}

		static void ClampHealth(RE::Actor* actor, float minHp)
		{
			if (!actor) return;
			const float hp = actor->GetActorValue(RE::ActorValue::kHealth);
			if (hp < minHp) {
				actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, (minHp - hp));
			}
		}

		static void SetGraceSeconds(int seconds)
		{
			g_grace.store(true, std::memory_order_release);
			g_graceUntil = Now() + std::chrono::seconds((std::max)(0, seconds));
		}

		static bool IsGraceActive()
		{
			if (!g_grace.load(std::memory_order_acquire)) return false;
			if (Now() >= g_graceUntil) {
				g_grace.store(false, std::memory_order_release);
				return false;
			}
			return true;
		}

		static RE::Actor* ResolveRecentPreCombatAggressor(float radius)
		{
			auto* player = Player();
			if (!player) {
				return nullptr;
			}

			auto* actor = TFD::PreCombatGreet::GetRecentActor(12.0);
			if (!actor) {
				return nullptr;
			}

			if (actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
				return nullptr;
			}

			if (!IsCaptiveSupportedAggressor(actor)) {
				return nullptr;
			}

			auto* pCell = player->GetParentCell();
			if (pCell && actor->GetParentCell() != pCell) {
				return nullptr;
			}

			const auto pp = player->GetPosition();
			const auto ap = actor->GetPosition();
			const float dx = ap.x - pp.x;
			const float dy = ap.y - pp.y;
			const float dz = ap.z - pp.z;
			const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (dist > radius) {
				return nullptr;
			}

			spdlog::info("[TFD][Defeat] using recent precombat actor {:08X} as defeat aggressor dist={:.1f}", actor->GetFormID(), dist);
			return actor;
		}

		static RE::Actor* ResolveAggressor()
		{
			auto* player = Player();
			const float maxSpeakerDist = 900.0f;
			if (g_lastAggressor) {
				auto sp = RE::Actor::LookupByHandle(g_lastAggressor.native_handle());
				float dist = 99999.0f;
				if (auto* actor = sp.get(); IsReasonableBleedoutSpeaker(actor, player, maxSpeakerDist, &dist)) {
					return actor;
				}
				if (auto* actor = sp.get(); actor) {
					spdlog::info("[TFD][Defeat] discard stale last aggressor {:08X} dist={:.1f}", actor->GetFormID(), dist);
				}
			}

			if (auto* recent = ResolveRecentPreCombatAggressor((std::max)(2400.0f, TFD::Settings::GetSweepRadius()))) {
				float dist = 99999.0f;
				if (IsReasonableBleedoutSpeaker(recent, player, maxSpeakerDist, &dist)) {
					g_lastAggressor = recent->GetHandle();
					return recent;
				}
			}

			return nullptr;
		}

		static bool IsActorCloseAndFront(RE::Actor* actor, RE::Actor* player, float maxDist)
		{
			if (!actor || !player) {
				return false;
			}

			const auto pa = player->GetPosition();
			const auto pb = actor->GetPosition();

			const float dx = pb.x - pa.x;
			const float dy = pb.y - pa.y;
			const float d2 = dx * dx + dy * dy;
			if (d2 > (maxDist * maxDist)) {
				return false;
			}

			const float len = std::sqrt((std::max)(1.0f, d2));
			const float ang = player->GetAngleZ();
			const float fx = std::sin(ang);
			const float fy = std::cos(ang);
			const float nx = dx / len;
			const float ny = dy / len;
			const float dot = nx * fx + ny * fy;
			return dot >= 0.20f;
		}

		static bool IsReasonableBleedoutSpeaker(RE::Actor* actor, RE::Actor* player, float maxDist, float* outDistance)
		{
			if (outDistance) {
				*outDistance = 99999.0f;
			}
			if (!actor || !player) {
				return false;
			}
			if (actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
				return false;
			}
			if (actor->GetFormID() == player->GetFormID()) {
				return false;
			}
			if (!IsCaptiveSupportedAggressor(actor)) {
				return false;
			}
			auto* pCell = player->GetParentCell();
			if (pCell && actor->GetParentCell() != pCell) {
				return false;
			}
			const auto pp = player->GetPosition();
			const auto ap = actor->GetPosition();
			const float dx = ap.x - pp.x;
			const float dy = ap.y - pp.y;
			const float dz = ap.z - pp.z;
			const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (outDistance) {
				*outDistance = dist;
			}
			if (dist > maxDist) {
				return false;
			}
			return true;
		}

		static RE::Actor* FindBestBleedoutSpeaker(float radius, float maxDist, RE::Actor* preferred)
		{
			auto* player = Player();
			if (!player) return nullptr;
			auto* pCell = player->GetParentCell();
			if (!pCell) return nullptr;

			float preferredDist = 99999.0f;
			const bool preferredValid = IsReasonableBleedoutSpeaker(preferred, player, maxDist, &preferredDist);

			TFD::ActorScan::Rescan(radius, true);
			RE::Actor* best = nullptr;
			float bestScore = 1.0e30f;
			const auto n = TFD::ActorScan::GetCount();
			for (int i = 0; i < n; ++i) {
				auto e = TFD::ActorScan::GetEntry(i);
				auto sp = e.actor.get();
				auto* a = sp.get();
				if (!a || a->IsDead() || a->IsDisabled()) continue;
				if (!a->Is3DLoaded()) continue;
				if (a->GetFormID() == player->GetFormID()) continue;
				if (a->GetParentCell() != pCell) continue;
				if (!e.inCombat && !e.hostile) continue;
				if (!IsCaptiveSupportedAggressor(a)) continue;
				if (e.dist > maxDist) continue;

				float score = e.dist;
				if (e.hostile) score -= 120.0f;
				if (e.inCombat) score -= 80.0f;
				if (IsActorCloseAndFront(a, player, 320.0f)) score -= 160.0f;
				if (a == preferred && preferredValid) score -= 40.0f;
				if (score < bestScore) {
					bestScore = score;
					best = a;
				}
			}

			if (best) {
				if (!preferredValid) {
					spdlog::info("[TFD][Defeat] local bleedout speaker {:08X} selected (no valid preferred)", best->GetFormID());
					return best;
				}

				float bestDist = 99999.0f;
				IsReasonableBleedoutSpeaker(best, player, maxDist, &bestDist);
				if (best != preferred && bestDist + 128.0f < preferredDist) {
					spdlog::info(
						"[TFD][Defeat] replacing stale/far speaker {:08X} dist={:.1f} with local {:08X} dist={:.1f}",
						preferred ? preferred->GetFormID() : 0,
						preferredDist,
						best->GetFormID(),
						bestDist);
					return best;
				}
			}

			if (preferredValid) {
				return preferred;
			}
			return best;
		}


		static bool CanUseAggressorForBleedoutGreet(RE::Actor* player, RE::Actor* aggressor, float& outDistance)
		{
			outDistance = 99999.0f;
			if (!player || !aggressor) {
				return false;
			}
			if (aggressor->IsDead() || aggressor->IsDisabled() || !aggressor->Is3DLoaded()) {
				return false;
			}
			if (aggressor->GetParentCell() != player->GetParentCell()) {
				return false;
			}

			const auto pa = player->GetPosition();
			const auto pb = aggressor->GetPosition();
			const float dx = pb.x - pa.x;
			const float dy = pb.y - pa.y;
			const float dz = pb.z - pa.z;
			outDistance = std::sqrt(dx * dx + dy * dy + dz * dz);

			if (outDistance > 220.0f) {
				return false;
			}

			bool hasLOS = false;
			if (!aggressor->HasLineOfSight(player, hasLOS) || !hasLOS) {
				return false;
			}

			return true;
		}

		static bool CanUseCaptiveFallbackHeuristic(RE::Actor* player, RE::Actor* aggressor, bool hasCaptiveOutcome, float& outDistance)
		{
			outDistance = 99999.0f;
			if (!hasCaptiveOutcome || !player || !aggressor) {
				return false;
			}

			if (!IsReasonableBleedoutSpeaker(aggressor, player, 768.0f, &outDistance)) {
				return false;
			}

			auto* marker = TFD::Location::GetCachedCaptiveMarker();
			if (!marker) {
				return false;
			}

			return true;
		}

		static RE::Actor* FindBestAggressor(float radius)
		{
			auto* player = Player();
			if (!player) return nullptr;
			auto* pCell = player->GetParentCell();
			if (!pCell) return nullptr;

			TFD::ActorScan::Rescan(radius, true);
			RE::Actor* best = nullptr;
			float bestDist = 1.0e30f;
			const auto n = TFD::ActorScan::GetCount();
			for (int i = 0; i < n; ++i) {
				auto e = TFD::ActorScan::GetEntry(i);
				auto sp = e.actor.get();
				auto* a = sp.get();
				if (!a || a->IsDead() || a->IsDisabled()) continue;
				if (!a->Is3DLoaded()) continue;
				if (a->GetFormID() == player->GetFormID()) continue;
				if (a->GetParentCell() != pCell) continue;
				if (!e.inCombat && !e.hostile) continue;
				if (e.dist < bestDist) {
					bestDist = e.dist;
					best = a;
				}
			}
			return best;
		}

		static void ApplyCalmBubble(float radius)
		{
			auto* player = Player();
			if (!player) {
				return;
			}

			auto* pCell = player->GetParentCell();
			const float sweepRadius = (std::max)(radius, (std::max)(TFD::Settings::GetSweepRadius(), 12000.0f));
			TFD::AntiAggro::SweepOnce(sweepRadius, true);
			TFD::AntiAggro::ScheduleWaves(sweepRadius, true, 12, 120);
			TFD::ActorScan::Rescan(sweepRadius, false);
			const auto n = TFD::ActorScan::GetCount();
			std::size_t applied = 0;
			for (int i = 0; i < n; ++i) {
				auto e = TFD::ActorScan::GetEntry(i);
				auto sp = e.actor.get();
				auto* a = sp.get();
				if (!a || a->IsDead() || a->IsDisabled()) continue;
				if (!a->Is3DLoaded()) continue;
				if (a->GetFormID() == player->GetFormID()) continue;
				if (pCell && a->GetParentCell() != pCell) continue;
				if (!e.hostile && !e.inCombat && !a->IsInCombat()) continue;

				if (auto* process = RE::ProcessLists::GetSingleton()) {
					const bool runDetection = process->runDetection;
					process->runDetection = false;
					process->ClearCachedFactionFightReactions();
					process->StopCombatAndAlarmOnActor(a, false);
					process->runDetection = runDetection;
				}

				TFD::AggressionClamp::Apply(a);
				a->StopCombat();
				if (a->IsWeaponDrawn()) {
					a->DrawWeaponMagicHands(false);
				}
				a->EvaluatePackage(true, false);
				++applied;
			}

			spdlog::info("[TFD][Defeat] calm bubble same-cell applied={} radius={:.0f}", applied, sweepRadius);
		}

		static void RecoverPlayerAfterTeleport()
		{
			auto* p = Player();
			if (!p) return;
			p->NotifyAnimationGraph("BleedoutStop");
			p->NotifyAnimationGraph("GetUpStart");
			const float hpMax = (std::max)(1.0f, p->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float threshPct = std::clamp(TFD::Settings::GetDefeatThresholdPct() / 100.0f, 0.05f, 0.95f);
			const float safePct = std::clamp(threshPct + 0.15f, 0.35f, 0.85f);
			const float target = (std::max)(25.0f, hpMax * safePct);
			const float hpNow = p->GetActorValue(RE::ActorValue::kHealth);
			if (hpNow < target) {
				p->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, (target - hpNow));
			}
			if (p->IsInCombat()) p->StopCombat();
			p->DrawWeaponMagicHands(false);
			SetPlayerBleedImmune(false);
		}

		static void RecoverPlayerForTransition()
		{
			auto* p = Player();
			if (!p) return;
			p->NotifyAnimationGraph("BleedoutStop");
			p->NotifyAnimationGraph("GetUpStart");
			auto restoreToPct = [&](RE::ActorValue av, float pct, float minValue) {
				const float maxValue = (std::max)(1.0f, p->GetPermanentActorValue(av));
				const float target = (std::max)(minValue, maxValue * pct);
				const float nowValue = p->GetActorValue(av);
				if (nowValue < target) {
					p->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, av, (target - nowValue));
				}
				};
			const float threshPct = std::clamp(TFD::Settings::GetDefeatThresholdPct() / 100.0f, 0.05f, 0.95f);
			const float safeHealthPct = std::clamp(threshPct + 0.10f, 0.55f, 1.00f);
			restoreToPct(RE::ActorValue::kHealth, safeHealthPct, 40.0f);
			restoreToPct(RE::ActorValue::kStamina, 0.95f, 35.0f);
			restoreToPct(RE::ActorValue::kMagicka, 0.95f, 25.0f);
			if (p->IsInCombat()) p->StopCombat();
			p->DrawWeaponMagicHands(false);
			SetPlayerBleedImmune(false);
		}

		static void TickLeftForDeadCooldown()
		{
			if (!IsLeftForDeadCooldownActive()) return;
			MaintainFollowerHold();
			const auto now = Now();
			if (g_leftForDeadNextPulse.time_since_epoch().count() != 0 && now < g_leftForDeadNextPulse) return;
			g_leftForDeadNextPulse = now + std::chrono::milliseconds(900);
			MaintainTransitionCalmWindow();
		}

		static void BeginLeftForDeadBlackout(int, const char*)
		{
			// Disabled on C++ side. Native wait / blackout will be handled by CK/Papyrus.
		}

		static bool TickLeftForDeadBlackout()
		{
			return false;
		}

		static void EnterNonCaptiveChoice(const char* reason);
		static bool BeginResolvedNoMarkerFallback(const char* reason);

		static void StartBleedWindow(RE::Actor* player, RE::Actor* aggressor)
		{
			if (!player) {
				return;
			}

			ClearBleedoutBridgeAliases(aggressor, "start_bleed_window");
			ClearNoMarkerFallbackState();

			g_inBleedState.store(true, std::memory_order_release);
			g_bleedSawDialogue = false;
			g_bleedPendingCaptiveOutcome = false;
			g_bleedPendingNonCaptiveOutcome = false;
			g_bleedStart = Now();
			g_bleedLastSeconds = -1;
			g_bleedPaused = false;
			g_bleedPauseStarted = {};
			g_bleedLastCalmPulse = {};
			g_bleedLastCrowdAssign = {};
			g_bleedCrowdAssigned.clear();

			const float maxHp = player->GetPermanentActorValue(RE::ActorValue::kHealth);
			const float minHp = (std::max)(1.0f, maxHp * 0.02f);
			g_minHp = minHp;
			SetPlayerBleedImmune(true);
			ClampHealth(player, g_minHp);
			player->NotifyAnimationGraph("BleedoutStart");

			const float radius = (std::max)(12000.0f, TFD::Settings::GetSweepRadius());
			const float maxSpeakerDist = 900.0f;
			aggressor = FindBestBleedoutSpeaker(radius, maxSpeakerDist, aggressor);
			if (aggressor) {
				float chosenDist = 99999.0f;
				IsReasonableBleedoutSpeaker(aggressor, player, maxSpeakerDist, &chosenDist);
				spdlog::info("[TFD][Defeat] bleed speaker locked {:08X} dist={:.1f}", aggressor->GetFormID(), chosenDist);
			}
			else {
				spdlog::info("[TFD][Defeat] no local bleed speaker within {:.0f} -> hold without greet", maxSpeakerDist);
			}
			auto initialCrowd = CollectBleedoutCrowd(radius, aggressor, false);
			ApplyCalmBubble(radius);
			RefreshBleedoutBridgeCrowd(radius, aggressor, true, &initialCrowd);

			if (aggressor) {
				g_lastAggressor = aggressor->GetHandle();

				if (!IsCaptiveSupportedAggressor(aggressor)) {
					g_bleedPendingCaptiveOutcome = false;
					g_bleedPendingNonCaptiveOutcome = true;
					spdlog::info("[TFD][Defeat] aggressor {:08X} not captive-supported (non-humanoid) -> keep bleed hold pending=noncaptive", aggressor->GetFormID());
				}
				else {
					const bool allowlistSupported = TFD::FactionMask::ApplyFromAggressor(aggressor);
					const bool hasCaptiveOutcome = ResolveCaptiveMarkerForOutcome();
					float fallbackDistance = 99999.0f;
					const bool fallbackSupported = !allowlistSupported &&
						CanUseCaptiveFallbackHeuristic(player, aggressor, hasCaptiveOutcome, fallbackDistance);

					if (!allowlistSupported && fallbackSupported) {
						spdlog::info(
							"[TFD][Defeat] captive fallback accepted via marker+speaker heuristic actor={:08X} dist={:.1f}",
							aggressor->GetFormID(),
							fallbackDistance);
					}

					if (!allowlistSupported && !fallbackSupported) {
						g_bleedPendingCaptiveOutcome = false;
						g_bleedPendingNonCaptiveOutcome = true;
						spdlog::info(
							"[TFD][Defeat] aggressor {:08X} has no captive-supported allowlist faction and fallback rejected (marker={} dist={:.1f}) -> keep bleed hold pending=noncaptive",
							aggressor->GetFormID(),
							hasCaptiveOutcome ? 1 : 0,
							fallbackDistance);
					}
					else {
						g_bleedPendingCaptiveOutcome = hasCaptiveOutcome;
						g_bleedPendingNonCaptiveOutcome = !hasCaptiveOutcome;

						float greetDistance = 99999.0f;
						const bool greetableNow = CanUseAggressorForBleedoutGreet(player, aggressor, greetDistance);
						if (!greetableNow) {
							spdlog::info("[TFD][Defeat] aggressor {:08X} not greetable now dist={:.1f} -> keep bleed hold pending={}",
								aggressor->GetFormID(), greetDistance, hasCaptiveOutcome ? "captive" : "noncaptive");
						}

						if (greetableNow) {
							TFD::ForceGreet::BeginBleedout(aggressor);
						}
					}
				}
			}
			else {
				const bool hasCaptiveOutcome = ResolveCaptiveMarkerForOutcome();
				g_bleedPendingCaptiveOutcome = hasCaptiveOutcome;
				g_bleedPendingNonCaptiveOutcome = !hasCaptiveOutcome;
				spdlog::info("[TFD][Defeat] no speaker -> keep bleed hold pending={}", hasCaptiveOutcome ? "captive" : "noncaptive");
			}

			const int bleedSeconds = TFD::Settings::GetBleedWindowSeconds();
			char msg[96]{};
			std::snprintf(msg, sizeof(msg), "TFDEngine: Bleeding... (%ds)", bleedSeconds);
			RE::DebugNotification(msg);
			spdlog::info("[TFD][Defeat] bleed window started ({}s)", bleedSeconds);
		}

		static void EnterEscapeFromLockpick(RE::TESObjectREFR* door)
		{
			EnterEscapeCommit("lockpick", door);
		}

		static bool BeginResolvedNoMarkerFallback(const char* reason)
		{
			ClearPendingCinematicFadeIn();
			ClearEscapeContext();
			ResetLockpickWatch();
			g_grace.store(false, std::memory_order_release);
			g_prevDialogueOpen = false;
			g_prevLockpickOpen = false;
			SetCaptiveRuntime(false, CaptivePhaseValue::None);

			auto* player = Player();
			if (player) {
				player->NotifyAnimationGraph("BleedoutStop");
				player->NotifyAnimationGraph("GetUpStart");
				if (player->IsInCombat()) {
					player->StopCombat();
				}
				player->DrawWeaponMagicHands(false);
			}

			const auto branch = ResolveNoMarkerFallback(reason);
			if (branch == NoMarkerFallbackBranch::None) {
				return false;
			}

			ClearBleedoutBridgeAliases(nullptr, reason ? reason : "noncaptive");
			TFD::ForceGreet::Cancel();
			SetPlayerBleedImmune(false);
			ResetBleedRuntimeState();
			g_lastAggressor.reset();
			UpdatePreCombatState();

			spdlog::info("[TFD][Transition] committed no-marker fallback branch={} reason={}",
				NoMarkerBranchName(branch),
				reason ? reason : "unknown");

			if (branch == NoMarkerFallbackBranch::RescueCached) {
				if (!BeginRescueTransition(reason ? reason : "rescue_cached")) {
					g_noMarkerFallback.branch = NoMarkerFallbackBranch::LeftForDeadSolo;
					g_noMarkerFallback.destination.reset();
					g_noMarkerFallback.hasFallbackPos = false;
					ResolveLeftForDeadDestination(g_noMarkerFallback);
					BeginRecoverTransition("rescue_cached_fallback_left_for_dead");
				}
				return true;
			}

			BeginRecoverTransition(reason ? reason : NoMarkerBranchName(branch));
			return true;
		}

		static void EnterNonCaptiveChoice(const char* reason)
		{
			if (BeginResolvedNoMarkerFallback(reason)) {
				return;
			}

			ClearBleedoutBridgeAliases(nullptr, reason ? reason : "noncaptive");
			ClearPendingCinematicFadeIn();
			TFD::ForceGreet::Cancel();
			TFD::FactionMask::Clear();
			ClearEscapeContext();
			ResetLockpickWatch();
			g_grace.store(false, std::memory_order_release);
			g_lastAggressor.reset();
			ResetBleedRuntimeState();
			g_prevDialogueOpen = false;
			g_prevLockpickOpen = false;
			SetCaptiveRuntime(false, CaptivePhaseValue::None);
			SetPlayerBleedImmune(false);
			QueueNonCaptiveChoiceRequest(reason);
			spdlog::warn("[TFD][Transition] fallback to legacy non-captive choice reason={}", reason ? reason : "unknown");
		}

		static void DoBlackoutTeleport()
		{
			ResetBleedRuntimeState();
			ClearBleedoutBridgeAliases(nullptr, "blackout_teleport");
			TFD::ForceGreet::Cancel();
			g_lastAggressor.reset();
			RE::DebugNotification("TFDEngine: Blackout -> Captive (1h)");
			if (!ResolveCaptiveMarkerForOutcome()) {
				EnterNonCaptiveChoice("marker_not_found");
				return;
			}
			ClearPendingCinematicFadeIn();
			if (QueueCinematicTransitionRequest(CinematicTransitionKind::Captive, false, "captive_blackout")) {
				return;
			}
			ShowBlackoutFader();
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			CompleteCaptiveTransitionNow("captive_blackout_fallback");
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			HideBlackoutFader();
		}

		static void UpdateLockpickEscapeWatch()
		{
			if (!g_captiveState || g_captivePhase != CaptivePhaseValue::Captive) {
				ResetLockpickWatch();
				return;
			}
			const bool lockOpen = IsLockpickingOpen();
			if (lockOpen && !g_prevLockpickOpen) {
				auto* rawTarget = RE::LockpickingMenu::GetTargetReference();
				auto* target = ResolveLockpickDoorCandidate(rawTarget);
				if (target) {
					g_lockpickDoorCandidate = target->GetHandle();
					g_lockpickDoorWasLocked = IsRefLocked(target);
					if (g_lockpickDoorWasLocked) BindCaptiveDoor(target);
					spdlog::info("[TFD][Captive] lockpick opened on door {:08X} wasLocked={} nearMarker=1 rawTarget={:08X}", target->GetFormID(), g_lockpickDoorWasLocked ? 1 : 0, rawTarget ? rawTarget->GetFormID() : 0);
				}
				else {
					g_lockpickDoorCandidate.reset();
					g_lockpickDoorWasLocked = false;
					auto* boundDoor = ResolveBoundEscapeDoor();
					if (rawTarget) {
						spdlog::info("[TFD][Captive] lockpick target {:08X} ignored (door={} nearMarker={} fallbackBoundDoor={:08X})", rawTarget->GetFormID(), IsDoorRef(rawTarget) ? 1 : 0, IsDoorNearCaptiveMarker(rawTarget) ? 1 : 0, boundDoor ? boundDoor->GetFormID() : 0);
					}
					else {
						spdlog::info("[TFD][Captive] lockpick target null (fallbackBoundDoor={:08X})", boundDoor ? boundDoor->GetFormID() : 0);
					}
				}
			}
			if (g_captiveDoor.HasDoor() && g_captiveDoor.UpdateWatcher()) {
				EnterEscapeCommit("door_watch", nullptr);
			}
			if (!lockOpen && g_prevLockpickOpen) {
				RE::TESObjectREFR* door = nullptr;
				if (g_lockpickDoorCandidate) {
					auto ptr = g_lockpickDoorCandidate.get();
					door = ptr.get();
				}
				if (!door) door = ResolveBoundEscapeDoor();
				if (door && IsDoorRef(door) && g_lockpickDoorWasLocked && !IsRefLocked(door)) {
					EnterEscapeFromLockpick(door);
				}
				else {
					ResetLockpickWatch();
				}
			}
			g_prevLockpickOpen = lockOpen;
		}

		static void TickUI()
		{
			struct Guard {
				~Guard() { g_tickPending.clear(std::memory_order_release); }
			} guard;
			if (!TFD::Settings::GetEnabled()) return;
			if (g_loadTransition.load(std::memory_order_acquire)) return;
			auto* ui = RE::UI::GetSingleton();
			PollTransitionResult();
			ProcessPendingCinematicFadeIn();
			if (IsTransitionAwaiting() || g_pendingCinematicFadeIn) {
				MaintainTransitionCalmWindow();
				UpdatePreCombatState();
				return;
			}
			TFD::ForceGreet::Tick();
			NormalizeInvalidCaptivePair();
			if (g_captiveState && g_captivePhase == CaptivePhaseValue::Captive) {
				const bool dialogOpen = IsDialogueOpen();
				if (!dialogOpen && g_prevDialogueOpen) {
					ApplyCalmBubble((std::max)(1800.0f, TFD::Settings::GetSweepRadius()));
					spdlog::info("[TFD][Captive] Dialogue closed -> calm burst");
				}
				g_prevDialogueOpen = dialogOpen;
				UpdateLockpickEscapeWatch();
				TryCommitEscapeByRadius();
			}
			else if (g_captiveState && g_captivePhase == CaptivePhaseValue::Escape) {
				TryResolveEscapeByLocation();
				if (g_captiveState && g_captivePhase == CaptivePhaseValue::Escape && !BreakEscapeOnDefeatThreshold(Player())) {
					return;
				}
			}

			if (g_captiveState) {
				return;
			}

			if (ui && ui->GameIsPaused()) return;
			auto* player = Player();
			if (!player) {
				SyncPreCombatGlobal(false);
				return;
			}
			UpdatePreCombatState();
			if (IsLeftForDeadCooldownActive()) {
				TickLeftForDeadCooldown();
				return;
			}
			if (IsGraceActive()) return;
			if (g_inBleedState.load(std::memory_order_acquire)) {
				if (g_minHp > 0.0f) {
					SetPlayerBleedImmune(true);
					ClampHealth(player, g_minHp);
				}
				if (g_bleedLastCalmPulse.time_since_epoch().count() == 0 || (Now() - g_bleedLastCalmPulse) >= std::chrono::milliseconds(350)) {
					ApplyCalmBubble((std::max)(12000.0f, TFD::Settings::GetSweepRadius()));
					g_bleedLastCalmPulse = Now();
				}
				const bool dOpen = IsDialogueOpen();
				if (!dOpen) {
					const auto nowBleed = Now();
					if (g_bleedLastCrowdAssign.time_since_epoch().count() == 0 || (nowBleed - g_bleedLastCrowdAssign) >= std::chrono::milliseconds(900)) {
						const float bleedRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius());
						RE::Actor* preferredSpeaker = nullptr;
						if (g_lastAggressor) {
							auto sp = RE::Actor::LookupByHandle(g_lastAggressor.native_handle());
							preferredSpeaker = sp.get();
						}
						RefreshBleedoutBridgeCrowd(bleedRadius, preferredSpeaker, false);

						const bool greetActive = TFD::ForceGreet::IsActive() && TFD::ForceGreet::GetMode() == TFD::ForceGreet::Mode::Bleedout;
						if (!greetActive) {
							auto* speaker = FindBestBleedoutSpeaker(bleedRadius, 900.0f, preferredSpeaker);
							if (speaker) {
								float greetDistance = 99999.0f;
								if (CanUseAggressorForBleedoutGreet(player, speaker, greetDistance)) {
									g_lastAggressor = speaker->GetHandle();
									spdlog::info("[TFD][Defeat] bleed speaker reselect {:08X} dist={:.1f}", speaker->GetFormID(), greetDistance);
									TFD::ForceGreet::BeginBleedout(speaker);
								}
								else {
									spdlog::info("[TFD][Defeat] bleed speaker still not greetable {:08X} dist={:.1f}", speaker->GetFormID(), greetDistance);
								}
							}
						}
					}
				}
				const int bleedSeconds = TFD::Settings::GetBleedWindowSeconds();
				if (dOpen) {
					g_bleedSawDialogue = true;
					if (!g_bleedPaused) {
						g_bleedPaused = true;
						g_bleedPauseStarted = Now();
						spdlog::info("[TFD][Defeat] bleed countdown paused by dialogue");
					}
					g_prevDialogueOpen = true;
					return;
				}
				if (g_bleedPaused) {
					g_bleedStart += (Now() - g_bleedPauseStarted);
					g_bleedPaused = false;
					g_bleedPauseStarted = {};
					g_bleedLastSeconds = -1;
					spdlog::info("[TFD][Defeat] bleed countdown resumed after dialogue");
				}
				if (g_bleedSawDialogue && g_prevDialogueOpen) {
					g_prevDialogueOpen = false;
					if (ResolveCaptiveMarkerForOutcome()) {
						spdlog::info("[TFD][Defeat] bleedout dialogue closed -> captive marker found");
						DoBlackoutTeleport();
						SetGraceSeconds(4);
					}
					else {
						spdlog::info("[TFD][Defeat] bleedout dialogue closed -> no marker -> resolve fallback");
						g_inBleedState.store(false, std::memory_order_release);
						g_minHp = 0.0f;
						g_bleedSawDialogue = false;
						g_bleedLastSeconds = -1;
						EnterNonCaptiveChoice("dialogue_closed_no_marker");
					}
					return;
				}
				const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(Now() - g_bleedStart).count();
				const int remain = bleedSeconds - static_cast<int>(elapsed);
				if (remain != g_bleedLastSeconds) {
					g_bleedLastSeconds = remain;
					if (remain > 0) {
						char msg[96]{};
						std::snprintf(msg, sizeof(msg), "TFDEngine: Bleeding... (%ds)", remain);
						RE::DebugNotification(msg);
					}
				}
				if (remain <= 0) {
					const bool pendingCaptive = g_bleedPendingCaptiveOutcome;
					if (pendingCaptive && ResolveCaptiveMarkerForOutcome()) {
						ResetBleedRuntimeState();
						spdlog::info("[TFD][Defeat] bleed timeout -> captive blackout");
						DoBlackoutTeleport();
						SetGraceSeconds(4);
					}
					else {
						spdlog::info("[TFD][Defeat] bleed timeout -> resolve no-marker fallback");
						EnterNonCaptiveChoice("bleed_timeout");
					}
				}
				return;
			}
			const float hpNow = player->GetActorValue(RE::ActorValue::kHealth);
			const float hpMax = (std::max)(1.0f, player->GetPermanentActorValue(RE::ActorValue::kHealth));
			const float pct = (hpNow / hpMax) * 100.0f;
			const float thresh = TFD::Settings::GetDefeatThresholdPct();
			if (pct <= thresh) {
				const float scanRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius());
				auto* aggressor = ResolveAggressor();
				if (!aggressor) {
					aggressor = FindBestAggressor(scanRadius);
				}
				aggressor = FindBestBleedoutSpeaker(scanRadius, 900.0f, aggressor);
				if (aggressor) {
					g_lastAggressor = aggressor->GetHandle();
				}
				if (!aggressor) {
					spdlog::info("[TFD][Defeat] no valid NPC aggressor -> force bleed hold");
					StartBleedWindow(player, nullptr);
					SetGraceSeconds(1);
					return;
				}
				StartBleedWindow(player, aggressor);
				SetGraceSeconds(1);
				return;
			}
		}

		static void WorkerLoop()
		{
			while (g_running.load(std::memory_order_acquire)) {
				if (!g_tickPending.test_and_set(std::memory_order_acq_rel)) {
					auto* task = SKSE::GetTaskInterface();
					if (task) task->AddTask([]() { TickUI(); });
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
		}
	}

	void Install()
	{
		if (g_installed.exchange(true, std::memory_order_acq_rel)) return;
		g_running.store(true, std::memory_order_release);
		g_loadTransition.store(false, std::memory_order_release);
		SetCaptiveRuntimeOnly(false, CaptivePhaseValue::None);
		g_prevDialogueOpen = false;
		ResetLockpickWatch();
		ClearEscapeContext();
		SetPlayerBleedImmune(false);
		ResetBleedRuntimeState();
		ClearBleedoutBridgeAliases(nullptr, "install");
		TFD::FactionMask::Initialize();
		TFD::Location::Initialize();
		TFD::ForceGreet::Install();
		g_worker = std::thread([]() { WorkerLoop(); });
		TFD::DefeatMonitor::ApplyQueuedProgressState();
		spdlog::info("[TFD][Defeat] monitor installed");
	}

	void Shutdown()
	{
		if (!g_installed.exchange(false, std::memory_order_acq_rel)) return;
		g_running.store(false, std::memory_order_release);
		if (g_worker.joinable()) g_worker.join();
		SetCaptiveRuntime(false, CaptivePhaseValue::None);
		g_hasQueuedProgressState = false;
		g_queuedCaptiveState = false;
		g_queuedCaptivePhase = CaptivePhaseValue::None;
		SetPlayerBleedImmune(false);
		ResetBleedRuntimeState();
		ClearBleedoutBridgeAliases(nullptr, "shutdown");
		g_loadTransition.store(false, std::memory_order_release);
		ResetLockpickWatch();
		ClearEscapeContext();
		ClearLeftForDeadCooldown();
		spdlog::info("[TFD][Defeat] monitor shutdown");
	}

	void ResetGrace()
	{
		g_grace.store(false, std::memory_order_release);
		ClearLeftForDeadCooldown();
	}

	bool GetCaptiveStateForSave()
	{
		return g_captiveState;
	}

	std::uint32_t GetCaptivePhaseForSave()
	{
		if (g_captiveState && g_captivePhase == CaptivePhaseValue::None) return 2u;
		return static_cast<std::uint32_t>(static_cast<int>(g_captivePhase));
	}

	void QueueLoadedProgressState(bool stateActive, std::uint32_t phaseRaw)
	{
		g_hasQueuedProgressState = true;
		g_queuedCaptiveState = stateActive;
		CaptivePhaseValue phase = stateActive ? PhaseFromRaw(phaseRaw) : CaptivePhaseValue::None;
		if (stateActive && phase == CaptivePhaseValue::None) phase = CaptivePhaseValue::Escape;
		g_queuedCaptivePhase = phase;
		spdlog::info("[TFD][Defeat] QueueLoadedProgressState state={} phase={} normalized={}", stateActive ? 1 : 0, phaseRaw, static_cast<int>(g_queuedCaptivePhase));
	}

	void QueueDefaultProgressState()
	{
		g_hasQueuedProgressState = true;
		g_queuedCaptiveState = false;
		g_queuedCaptivePhase = CaptivePhaseValue::None;
		spdlog::info("[TFD][Defeat] QueueDefaultProgressState");
	}

	bool HasQueuedProgressState()
	{
		return g_hasQueuedProgressState;
	}

	void ApplyQueuedProgressState()
	{
		if (!g_hasQueuedProgressState) QueueDefaultProgressState();
		ClearLeftForDeadCooldown();
		SetCaptiveRuntime(g_queuedCaptiveState, g_queuedCaptivePhase);
		g_prevDialogueOpen = IsDialogueOpen();
		g_prevLockpickOpen = IsLockpickingOpen();
		if (g_queuedCaptiveState && g_queuedCaptivePhase == CaptivePhaseValue::Captive) {
			TFD::Location::RescanCaptiveMarker();
			ArmEscapeContextFromCurrentState();
		}
		else {
			ResetLockpickWatch();
			ClearEscapeContext();
		}
		SetPlayerBleedImmune(false);
		ClearBleedoutBridgeAliases(nullptr, "apply_queued_state");
		UpdatePreCombatState();
		spdlog::info("[TFD][Defeat] ApplyQueuedProgressState state={} phase={}", g_queuedCaptiveState ? 1 : 0, static_cast<int>(g_queuedCaptivePhase));
	}

	void ResetForLoad()
	{
		g_grace.store(false, std::memory_order_release);
		SetPlayerBleedImmune(false);
		ResetBleedRuntimeState();
		ClearBleedoutBridgeAliases(nullptr, "reset_for_load");
		g_lastAggressor = RE::ActorHandle{};
		SetCaptiveRuntimeOnly(false, CaptivePhaseValue::None);
		g_prevDialogueOpen = false;
		ResetLockpickWatch();
		ClearEscapeContext();
		TFD::FactionMask::Clear();
		TFD::AggressionClamp::Clear();
		TFD::ForceGreet::Cancel();
		ClearLeftForDeadCooldown();
		spdlog::info("[TFD][Defeat] ResetForLoad -> runtime only");
	}

	void SetLoadTransition(bool active)
	{
		g_loadTransition.store(active, std::memory_order_release);
		if (active) {
			SetPlayerBleedImmune(false);
			ClearBleedoutBridgeAliases(nullptr, "set_load_transition");
			TFD::ForceGreet::Cancel();
			ResetLockpickWatch();
			spdlog::info("[TFD][Defeat] SetLoadTransition(true)");
		}
		else {
			spdlog::info("[TFD][Defeat] SetLoadTransition(false)");
		}
	}

	bool IsLeftForDeadRecoveryActive()
	{
		return g_leftForDeadActive;
	}

	bool IsCaptivePhase()
	{
		return g_captiveState && g_captivePhase == CaptivePhaseValue::Captive;
	}

	std::uint32_t GetCaptivePhaseRaw()
	{
		if (g_captiveState && g_captivePhase == CaptivePhaseValue::None) {
			return 2u;  // legacy/normalized escape fallback
		}
		return static_cast<std::uint32_t>(static_cast<int>(g_captivePhase));
	}

	const char* GetCaptivePhaseName()
	{
		switch (g_captivePhase) {
		case CaptivePhaseValue::None:
			return g_captiveState ? "Escape" : "None";
		case CaptivePhaseValue::Captive:
			return "Captive";
		case CaptivePhaseValue::Escape:
			return "Escape";
		default:
			return "Unknown";
		}
	}

	bool IsCaptiveFamily()
	{
		return g_captiveState || g_captivePhase != CaptivePhaseValue::None;
	}
}
