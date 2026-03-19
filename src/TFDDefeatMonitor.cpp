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

namespace TFD::DefeatMonitor
{
	namespace
	{
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
		std::vector<RE::FormID> g_bleedCrowdSnapshot{};
		std::unordered_set<RE::FormID> g_bleedCalmPrimed{};
		std::unordered_set<RE::FormID> g_transitionCalmPrimed{};
		std::chrono::steady_clock::time_point g_transitionLastCalmPulse{};

		std::atomic_bool g_grace{ false };
		std::chrono::steady_clock::time_point g_graceUntil{};

		bool g_leftForDeadActive = false;
		std::chrono::steady_clock::time_point g_leftForDeadUntil{};
		std::chrono::steady_clock::time_point g_leftForDeadNextPulse{};


		RE::ActorHandle g_lastAggressor{};
		bool g_bleedSawDialogue = false;
		bool g_bleedPendingCaptiveOutcome = false;
		bool g_bleedPendingNonCaptiveOutcome = false;

		bool g_captiveState = false;
		CaptivePhaseValue g_captivePhase = CaptivePhaseValue::None;
		bool g_prevDialogueOpen = false;
		bool g_prevLockpickOpen = false;

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

		static bool IsBleedCrowdSupportedAggressor(RE::Actor* actor)
		{
			if (!actor) {
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

		static void WakeNearbyHostilesAfterCalm(float radius, bool sameCellOnly, const char* reason);
		static void ResetTransitionCalmState();

		static void FinishLeftForDeadRecovery()
		{
			TFD::AntiAggro::CancelPending();
			TFD::AggressionClamp::Clear();
			WakeNearbyHostilesAfterCalm((std::max)(2200.0f, TFD::Settings::GetSweepRadius()), false, "left_for_dead_finish");
			TFD::FactionMask::Clear();
			g_leftForDeadActive = false;
			g_leftForDeadUntil = {};
			g_leftForDeadNextPulse = {};
			ResetTransitionCalmState();
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
				} else if (senderFormID != 0) {
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

		static void ClearBleedoutBridgeAliases(RE::TESForm* sender, const char* reason)
		{
			const bool queued = SendBridgeModEvent("TFDBleedoutClearAll", sender);
			spdlog::info("[TFD][BleedBridge] ClearAll reason={} queued={}", reason ? reason : "unknown", queued);
		}

		static void AssignBleedoutBridgeActor(RE::Actor* actor)
		{
			if (!actor) {
				return;
			}

			const bool queued = SendBridgeModEvent("TFDBleedoutAssign", actor);
			spdlog::info("[TFD][BleedBridge] Assign actor={:08X} queued={}", actor->GetFormID(), queued);
		}

		static bool IsActorCloseAndFront(RE::Actor* actor, RE::Actor* player, float maxDist);

		static float DistanceBetween(RE::TESObjectREFR* a, RE::TESObjectREFR* b)
		{
			if (!a || !b) {
				return 99999.0f;
			}

			const auto pa = a->GetPosition();
			const auto pb = b->GetPosition();

			const float dx = pb.x - pa.x;
			const float dy = pb.y - pa.y;
			const float dz = pb.z - pa.z;

			return std::sqrt(dx * dx + dy * dy + dz * dz);
		}

		static std::vector<RE::Actor*> CollectBleedoutCrowd(float radius, RE::Actor* preferred, bool preserveAssigned = false, bool lockSnapshotSeed = false)
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
				preservedIds.insert(g_bleedCrowdSnapshot.begin(), g_bleedCrowdSnapshot.end());
			}

			std::unordered_set<RE::FormID> scoredIds;
			auto addCandidate = [&](RE::Actor* actor, float baseScore, bool targetingPlayer, bool hostileFlag, bool inCombatFlag, bool preserved, bool weaponDrawn, bool nearPreferred) {
				if (!actor) {
					return;
				}
				const auto id = actor->GetFormID();
				if (!scoredIds.insert(id).second) {
					return;
				}

				float score = baseScore;
				if (actor == preferred) score -= 1000.0f;
				if (targetingPlayer) score -= 140.0f;
				if (hostileFlag) score -= 80.0f;
				if (inCombatFlag || actor->IsInCombat()) score -= 60.0f;
				if (preserved) score -= 90.0f;
				if (weaponDrawn) score -= 55.0f;
				if (lockSnapshotSeed && nearPreferred) score -= 45.0f;
				if (IsActorCloseAndFront(actor, player, 320.0f)) score -= 120.0f;
				scored.emplace_back(score, actor);
			};

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
				const bool weaponDrawn = actor->IsWeaponDrawn();
				const bool nearPreferred = preferred && actor != preferred && actor->GetParentCell() == preferred->GetParentCell() && DistanceBetween(actor, preferred) <= 2600.0f;

				bool include = targetingPlayer || preserved || actor == preferred;
				if (lockSnapshotSeed && !include) {
					if ((weaponDrawn && nearPreferred) || (weaponDrawn && actor->IsHostileToActor(player))) {
						include = true;
					}
				}

				if (!include) continue;
				addCandidate(actor, e.dist, targetingPlayer, e.hostile, e.inCombat, preserved, weaponDrawn, nearPreferred);
			}

			if (lockSnapshotSeed) {
				const RE::NiPoint3 origin = preferred ? preferred->GetPosition() : player->GetPosition();
				std::size_t actorScanSeedCount = scored.size();
				std::size_t cellSeedAdded = 0;

				for (int pass = 0; pass < 3 && scored.size() < kBleedBridgeMaxActors; ++pass) {
					std::vector<RE::Actor*> cluster;
					cluster.reserve(scored.size());
					for (const auto& pair : scored) {
						if (pair.second) {
							cluster.push_back(pair.second);
						}
					}

					bool passAdded = false;
					pCell->ForEachReferenceInRange(origin, scanRadius, [&](RE::TESObjectREFR* candidate) -> RE::BSContainer::ForEachResult {
						auto* actor = candidate ? candidate->As<RE::Actor>() : nullptr;
						if (!actor || actor->IsDead() || actor->IsDisabled()) {
							return RE::BSContainer::ForEachResult::kContinue;
						}
						if (!actor->Is3DLoaded()) {
							return RE::BSContainer::ForEachResult::kContinue;
						}
						if (actor->GetFormID() == player->GetFormID()) {
							return RE::BSContainer::ForEachResult::kContinue;
						}
						if (actor->GetParentCell() != pCell) {
							return RE::BSContainer::ForEachResult::kContinue;
						}
						if (!IsBleedCrowdSupportedAggressor(actor)) {
							return RE::BSContainer::ForEachResult::kContinue;
						}
						if (DistanceBetween(actor, player) > scanRadius) {
							return RE::BSContainer::ForEachResult::kContinue;
						}
						const auto id = actor->GetFormID();
						if (scoredIds.find(id) != scoredIds.end()) {
							return RE::BSContainer::ForEachResult::kContinue;
						}

						const bool preserved = preserveAssigned && preservedIds.find(id) != preservedIds.end();
						const bool hostile = actor->IsHostileToActor(player);
						const bool inCombat = actor->IsInCombat();
						const bool weaponDrawn = actor->IsWeaponDrawn();
						const bool nearPreferred = preferred && actor != preferred && actor->GetParentCell() == preferred->GetParentCell() && DistanceBetween(actor, preferred) <= 2600.0f;

						bool nearCluster = false;
						for (auto* member : cluster) {
							if (!member || member == actor) {
								continue;
							}
							if (DistanceBetween(actor, member) <= 1700.0f) {
								nearCluster = true;
								break;
							}
						}

						const bool include = (actor == preferred) || preserved || hostile || inCombat || (weaponDrawn && nearPreferred) || (weaponDrawn && nearCluster);
						if (!include) {
							return RE::BSContainer::ForEachResult::kContinue;
						}

						addCandidate(actor, DistanceBetween(actor, player), hostile || inCombat, hostile, inCombat, preserved, weaponDrawn, nearPreferred || nearCluster);
						++cellSeedAdded;
						passAdded = true;
						return scored.size() >= kBleedBridgeMaxActors ? RE::BSContainer::ForEachResult::kStop : RE::BSContainer::ForEachResult::kContinue;
					});

					if (!passAdded) {
						break;
					}
				}

				spdlog::info("[TFD][Defeat] bleed crowd seed actorScan={} cellAdded={} preferred={:08X}",
					actorScanSeedCount,
					cellSeedAdded,
					preferred ? preferred->GetFormID() : 0u);
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
				} else if (it != result.begin()) {
					std::rotate(result.begin(), it, it + 1);
				}
			}

			return result;
		}

		static std::vector<RE::Actor*> ResolveBleedoutCrowdSnapshot(float radius, RE::Actor* preferred, bool allowTopUp)
		{
			std::vector<RE::Actor*> resolved;
			resolved.reserve(kBleedBridgeMaxActors);

			auto* player = Player();
			if (!player) {
				return resolved;
			}

			auto* pCell = player->GetParentCell();
			if (!pCell) {
				return resolved;
			}

			auto containsResolved = [&](RE::FormID id) {
				for (auto* existing : resolved) {
					if (existing && existing->GetFormID() == id) {
						return true;
					}
				}
				return false;
			};

			auto tryAddActor = [&](RE::Actor* actor) {
				if (!actor || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) {
					return;
				}
				if (actor->GetFormID() == player->GetFormID()) {
					return;
				}
				if (actor->GetParentCell() != pCell) {
					return;
				}
				if (!IsBleedCrowdSupportedAggressor(actor)) {
					return;
				}
				if (DistanceBetween(actor, player) > radius) {
					return;
				}
				const auto id = actor->GetFormID();
				if (containsResolved(id)) {
					return;
				}
				resolved.push_back(actor);
			};

			auto tryAddActorLenient = [&](RE::Actor* actor) {
				if (!actor || actor->IsDead() || actor->IsDisabled()) {
					return;
				}
				if (actor->GetFormID() == player->GetFormID()) {
					return;
				}
				if (pCell && actor->GetParentCell() != pCell) {
					return;
				}
				const auto id = actor->GetFormID();
				if (containsResolved(id)) {
					return;
				}
				resolved.push_back(actor);
			};

			for (auto id : g_bleedCrowdSnapshot) {
				if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(id)) {
					tryAddActor(actor);
				}
			}

			if (allowTopUp && resolved.size() < kBleedBridgeMaxActors) {
				auto additions = CollectBleedoutCrowd(radius, preferred, true, true);
				bool snapshotGrew = false;
				for (auto* actor : additions) {
					if (!actor) {
						continue;
					}
					const auto id = actor->GetFormID();
					if (std::find(g_bleedCrowdSnapshot.begin(), g_bleedCrowdSnapshot.end(), id) == g_bleedCrowdSnapshot.end()) {
						g_bleedCrowdSnapshot.push_back(id);
						snapshotGrew = true;
					}
				}
				if (snapshotGrew) {
					spdlog::info("[TFD][Defeat] bleed crowd snapshot grew size={}", g_bleedCrowdSnapshot.size());
				}

				resolved.clear();
				for (auto id : g_bleedCrowdSnapshot) {
					if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(id)) {
						tryAddActor(actor);
						if (resolved.size() >= kBleedBridgeMaxActors) {
							break;
						}
					}
				}
			}

			const std::size_t floorCount = (std::min)(kBleedBridgeMaxActors, (std::max)(g_bleedCrowdSnapshot.size(), g_bleedCrowdAssigned.size()));
			if (floorCount > 0 && resolved.size() < floorCount) {
				const std::size_t beforeProtected = resolved.size();

				for (auto id : g_bleedCrowdAssigned) {
					if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(id)) {
						tryAddActorLenient(actor);
						if (resolved.size() >= floorCount) {
							break;
						}
					}
				}

				for (auto id : g_bleedCrowdSnapshot) {
					if (resolved.size() >= floorCount) {
						break;
					}
					if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(id)) {
						tryAddActorLenient(actor);
					}
				}

				if (resolved.size() > beforeProtected) {
					spdlog::info("[TFD][Defeat] bleed crowd no-shrink preserved before={} after={} floor={} assigned={} snapshot={}",
						beforeProtected,
						resolved.size(),
						floorCount,
						g_bleedCrowdAssigned.size(),
						g_bleedCrowdSnapshot.size());
				}
			}

			if (preferred) {
				const auto preferredId = preferred->GetFormID();
				auto it = std::find_if(resolved.begin(), resolved.end(), [preferredId](RE::Actor* actor) {
					return actor && actor->GetFormID() == preferredId;
				});
				if (it == resolved.end()) {
					if (resolved.size() >= kBleedBridgeMaxActors) {
						resolved.pop_back();
					}
					resolved.insert(resolved.begin(), preferred);
				} else if (it != resolved.begin()) {
					std::rotate(resolved.begin(), it, it + 1);
				}
			}

			return resolved;
		}

		static void AssignBleedoutBridgeCrowd(const std::vector<RE::Actor*>& actors)
		{
			std::size_t sent = 0;
			for (auto* actor : actors) {
				if (!actor) {
					continue;
				}
				const bool queued = SendBridgeModEvent("TFDBleedoutAssign", actor);
				if (queued) {
					++sent;
				}
				spdlog::info("[TFD][BleedBridge] Assign actor={:08X} queued={}", actor->GetFormID(), queued);
			}

			spdlog::info("[TFD][BleedBridge] Assign crowd sent={} size={} primary={:08X}",
				sent,
				actors.size(),
				!actors.empty() && actors.front() ? actors.front()->GetFormID() : 0u);
		}

		static void RefreshBleedoutBridgeCrowd(float radius, RE::Actor* preferred, bool forceClear, std::vector<RE::Actor*>* explicitCrowd = nullptr)
		{
			std::vector<RE::Actor*> crowd = explicitCrowd ? *explicitCrowd : CollectBleedoutCrowd(radius, preferred, !forceClear);

			if (!forceClear && g_inBleedState.load(std::memory_order_acquire)) {
				auto* player = Player();
				auto* pCell = player ? player->GetParentCell() : nullptr;
				std::unordered_set<RE::FormID> seenIds;
				for (auto* actor : crowd) {
					if (actor) {
						seenIds.insert(actor->GetFormID());
					}
				}

				auto appendIfMissing = [&](RE::FormID id) {
					if (!id || seenIds.find(id) != seenIds.end()) {
						return;
					}
					auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
					if (!actor || actor->IsDead() || actor->IsDisabled()) {
						return;
					}
					if (player && actor->GetFormID() == player->GetFormID()) {
						return;
					}
					if (pCell && actor->GetParentCell() != pCell) {
						return;
					}
					if (DistanceBetween(actor, player) > (std::max)(radius, 12000.0f)) {
						return;
					}
					seenIds.insert(id);
					crowd.push_back(actor);
				};

				const std::size_t beforePreserve = crowd.size();
				for (auto id : g_bleedCrowdAssigned) {
					appendIfMissing(id);
				}
				for (auto id : g_bleedCrowdSnapshot) {
					appendIfMissing(id);
				}
				if (crowd.size() > beforePreserve) {
					spdlog::info("[TFD][BleedBridge] no-shrink preserved before={} after={} assigned={} snapshot={} primary={:08X}",
						beforePreserve,
						crowd.size(),
						g_bleedCrowdAssigned.size(),
						g_bleedCrowdSnapshot.size(),
						preferred ? preferred->GetFormID() : 0u);
				}
			}

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
			g_bleedCrowdSnapshot.clear();
			g_bleedCalmPrimed.clear();
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
		static void UpdatePreCombatState();
		static bool IsActorCloseAndFront(RE::Actor* actor, RE::Actor* player, float maxDist);
		static bool CanUseAggressorForBleedoutGreet(RE::Actor* player, RE::Actor* aggressor, float& outDistance);
		static RE::TESObjectREFR* LookupRefByFormID(std::uint32_t formID);
		static RE::TESObjectREFR* ResolveBestRescueDestination(RE::BGSLocation* safeLoc);
		static bool BeginRescueTransition(const char* reason);
		static void BeginRecoverTransition(const char* reason);
		static void DoBlackoutTeleport();

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

		static void ResetTransitionCalmState()
		{
			g_transitionCalmPrimed.clear();
			g_transitionLastCalmPulse = {};
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
			if (player->IsWeaponDrawn()) {
				player->DrawWeaponMagicHands(false);
			}

			const auto now = Now();
			if (g_transitionLastCalmPulse.time_since_epoch().count() != 0 &&
				(now - g_transitionLastCalmPulse) < std::chrono::milliseconds(650)) {
				return;
			}
			g_transitionLastCalmPulse = now;

			const float radius = (std::max)(3200.0f, TFD::Settings::GetSweepRadius() + 1200.0f);
			const bool firstPulse = g_transitionCalmPrimed.empty();
			if (firstPulse) {
				TFD::AntiAggro::CancelPending();
				TFD::AntiAggro::SweepOnce(radius, false);
			}

			TFD::ActorScan::Rescan(radius, false);
			const auto count = TFD::ActorScan::GetCount();
			std::size_t hardened = 0;
			std::size_t maintained = 0;
			for (int i = 0; i < count; ++i) {
				auto entry = TFD::ActorScan::GetEntry(i);
				auto actorSP = entry.actor.get();
				auto* actor = actorSP.get();
				if (!actor || actor->IsDead() || actor->IsDisabled()) {
					continue;
				}
				if (!actor->Is3DLoaded()) {
					continue;
				}
				if (actor->GetFormID() == player->GetFormID()) {
					continue;
				}

				const auto actorId = actor->GetFormID();
				const bool firstTime = g_transitionCalmPrimed.insert(actorId).second;
				if (firstTime) {
					TFD::AggressionClamp::Apply(actor);
					actor->StopCombat();
					if (actor->IsWeaponDrawn()) {
						actor->DrawWeaponMagicHands(false);
					}
					actor->EvaluatePackage(true, false);
					++hardened;
				} else {
					if (actor->IsInCombat()) {
						actor->StopCombat();
					}
					++maintained;
				}
			}

			spdlog::info("[TFD][Defeat] transition calm pulse hardened={} maintained={} radius={:.0f}",
				hardened,
				maintained,
				radius);
		}

		static void WakeNearbyHostilesAfterCalm(float radius, bool sameCellOnly, const char* reason)
		{
			auto* player = Player();
			if (!player) {
				return;
			}

			const float scanRadius = (std::max)(radius, (std::max)(TFD::Settings::GetSweepRadius(), 1800.0f));
			auto* pCell = player->GetParentCell();
			TFD::ActorScan::Rescan(scanRadius, false);
			const auto count = TFD::ActorScan::GetCount();
			std::size_t nudged = 0;

			for (int i = 0; i < count; ++i) {
				auto entry = TFD::ActorScan::GetEntry(i);
				auto actorSP = entry.actor.get();
				auto* actor = actorSP.get();
				if (!actor || actor->IsDead() || actor->IsDisabled()) {
					continue;
				}
				if (!actor->Is3DLoaded()) {
					continue;
				}
				if (actor->GetFormID() == player->GetFormID()) {
					continue;
				}
				if (sameCellOnly && pCell && actor->GetParentCell() != pCell) {
					continue;
				}

				const bool hostile = entry.hostile || actor->IsHostileToActor(player);
				const bool shouldWake = hostile || entry.inCombat || actor->IsInCombat();
				if (!shouldWake) {
					continue;
				}

				actor->SetBeenAttacked(true);
				player->SetBeenAttacked(true);
				actor->RequestDetectionLevel(player, RE::DETECTION_PRIORITY::kCritical);
				if (hostile && !actor->IsWeaponDrawn()) {
					actor->DrawWeaponMagicHands(true);
				}
				actor->EvaluatePackage(true, true);
				actor->UpdateCombat();
				++nudged;
			}

			player->UpdateCombat();
			spdlog::info("[TFD][Defeat] wake nearby hostiles reason={} nudged={} radius={:.0f} sameCellOnly={}",
				reason ? reason : "unknown",
				nudged,
				scanRadius,
				sameCellOnly);
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

		static bool BeginRescueTransition(const char* reason)
		{
			auto* player = Player();
			if (!player) {
				return false;
			}

			auto* safeLoc = TFD::Location::ResolveRescueTargetLocationFromRef(player);
			auto* dest = safeLoc ? ResolveBestRescueDestination(safeLoc) : nullptr;

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

			if (!safeLoc) {
				spdlog::info("[TFD][Transition] rescue unavailable reason={} cause=no_safe_location", reason ? reason : "unknown");
				return false;
			}

			if (!dest) {
				spdlog::info("[TFD][Transition] rescue unavailable reason={} safeLoc={:08X} cause=no_destination",
					reason ? reason : "unknown", safeLoc->GetFormID());
				return false;
			}

			ShowBlackoutFader();
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			player->MoveTo(dest);
			std::this_thread::sleep_for(std::chrono::milliseconds(150));
			RecoverPlayerForTransition();
			MaintainTransitionCalmWindow();
			BeginLeftForDeadCooldown(5);
			SetGraceSeconds(4);
			UpdatePreCombatState();
			std::this_thread::sleep_for(std::chrono::milliseconds(400));
			HideBlackoutFader();
			spdlog::info("[TFD][Transition] rescue complete reason={} safeLoc={:08X} dest={:08X}",
				reason ? reason : "unknown", safeLoc->GetFormID(), dest->GetFormID());
			return true;
		}

		static void BeginRecoverTransition(const char* reason)
		{
			ShowBlackoutFader();
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			RecoverPlayerForTransition();
			MaintainTransitionCalmWindow();
			BeginLeftForDeadCooldown(3);
			SetGraceSeconds(2);
			UpdatePreCombatState();
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			HideBlackoutFader();
			spdlog::info("[TFD][Transition] recover complete reason={}", reason ? reason : "unknown");
		}

		static void PollTransitionResult()
		{
			const int result = ConsumeTransitionResult();
			if (result == 0) {
				return;
			}
			if (result == 1) {
				ResetTransitionCalmState();
				spdlog::info("[TFD][Transition] result=completed");
				return;
			}
			if (result == 2) {
				ResetTransitionCalmState();
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
			TFD::AntiAggro::CancelPending();
			TFD::AggressionClamp::Clear();
			WakeNearbyHostilesAfterCalm((std::max)(1800.0f, TFD::Settings::GetSweepRadius()), true, "escape_commit");
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

		static void ApplyCalmBubble(float radius, std::vector<RE::Actor*>* lockedCrowd = nullptr, bool hardPulse = false, RE::Actor* exemptActor = nullptr)
		{
			auto* player = Player();
			if (!player) {
				return;
			}

			auto* pCell = player->GetParentCell();
			const float sweepRadius = (std::max)(radius, (std::max)(TFD::Settings::GetSweepRadius(), 12000.0f));

			if (hardPulse) {
				TFD::AntiAggro::CancelPending();
				TFD::AntiAggro::SweepOnce(sweepRadius, true);
				TFD::AntiAggro::ScheduleWaves(sweepRadius, true, 6, 160);
			}

			auto calmOne = [&](RE::Actor* a) -> bool {
				if (!a || a->IsDead() || a->IsDisabled()) return false;
				if (!a->Is3DLoaded()) return false;
				if (a->GetFormID() == player->GetFormID()) return false;
				if (pCell && a->GetParentCell() != pCell) return false;

				const auto actorId = a->GetFormID();
				const bool firstTime = g_bleedCalmPrimed.insert(actorId).second;
				const bool isExempt = exemptActor && exemptActor->GetFormID() == actorId;

				if (firstTime || hardPulse) {
					if (!isExempt) {
						if (auto* process = RE::ProcessLists::GetSingleton()) {
							const bool runDetection = process->runDetection;
							process->runDetection = false;
							process->ClearCachedFactionFightReactions();
							process->StopCombatAndAlarmOnActor(a, false);
							process->runDetection = runDetection;
						}
					}

					TFD::AggressionClamp::Apply(a);
					a->StopCombat();
					if (!isExempt && a->IsWeaponDrawn()) {
						a->DrawWeaponMagicHands(false);
					}
					if (!isExempt) {
						a->EvaluatePackage(true, false);
					}
				} else {
					if (a->IsInCombat()) {
						a->StopCombat();
					}
				}
				return true;
			};

			std::size_t applied = 0;
			if (lockedCrowd && !lockedCrowd->empty()) {
				for (auto* actor : *lockedCrowd) {
					if (calmOne(actor)) {
						++applied;
					}
				}
				spdlog::info("[TFD][Defeat] calm bubble locked applied={} snapshot={} radius={:.0f} hardPulse={} exempt={:08X}",
					applied,
					lockedCrowd->size(),
					sweepRadius,
					hardPulse,
					exemptActor ? exemptActor->GetFormID() : 0);
				return;
			}

			TFD::ActorScan::Rescan(sweepRadius, false);
			const auto n = TFD::ActorScan::GetCount();
			for (int i = 0; i < n; ++i) {
				auto e = TFD::ActorScan::GetEntry(i);
				auto sp = e.actor.get();
				auto* a = sp.get();
				if (!a || (!e.hostile && !e.inCombat && !a->IsInCombat())) continue;
				if (calmOne(a)) {
					++applied;
				}
			}

			spdlog::info("[TFD][Defeat] calm bubble same-cell applied={} radius={:.0f} hardPulse={}",
				applied,
				sweepRadius,
				hardPulse);
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
			const auto now = Now();
			if (g_leftForDeadNextPulse.time_since_epoch().count() != 0 && now < g_leftForDeadNextPulse) return;
			g_leftForDeadNextPulse = now + std::chrono::seconds(2);
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

		static void StartBleedWindow(RE::Actor* player, RE::Actor* aggressor)
		{
			if (!player) {
				return;
			}

			ClearBleedoutBridgeAliases(aggressor, "start_bleed_window");

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
			g_bleedCrowdSnapshot.clear();

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
			auto initialCrowd = CollectBleedoutCrowd(radius, aggressor, false, true);
			g_bleedCrowdSnapshot.clear();
			for (auto* actor : initialCrowd) {
				if (actor) {
					g_bleedCrowdSnapshot.push_back(actor->GetFormID());
				}
			}
			ApplyCalmBubble(radius, &initialCrowd, true, aggressor);
			RefreshBleedoutBridgeCrowd(radius, aggressor, true, &initialCrowd);
			spdlog::info("[TFD][Defeat] bleed crowd snapshot locked size={}", g_bleedCrowdSnapshot.size());

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

		static void EnterNonCaptiveChoice(const char* reason)
		{
			ClearBleedoutBridgeAliases(nullptr, reason ? reason : "noncaptive");
			TFD::ForceGreet::Cancel();
			// Keep faction mask alive during pending non-captive choice / short recovery window
			// so nearby actors stay consistently passive instead of hostile-on-radar but AI-frozen.
			ClearEscapeContext();
			ResetLockpickWatch();
			g_grace.store(false, std::memory_order_release);
			g_lastAggressor.reset();
			ResetBleedRuntimeState();
			g_prevDialogueOpen = false;
			g_prevLockpickOpen = false;
			SetCaptiveRuntime(false, CaptivePhaseValue::None);
			ResetTransitionCalmState();

			auto* player = Player();
			if (player) {
				player->NotifyAnimationGraph("BleedoutStop");
				player->NotifyAnimationGraph("GetUpStart");
				if (player->IsInCombat()) {
					player->StopCombat();
				}
				player->DrawWeaponMagicHands(false);
			}

			const float radius = (std::max)(3200.0f, TFD::Settings::GetSweepRadius() + 1200.0f);
			TFD::AntiAggro::CancelPending();
			TFD::AntiAggro::SweepOnce(radius, false);
			TFD::ActorScan::Rescan(radius, false);
			const auto count = TFD::ActorScan::GetCount();
			for (int i = 0; i < count; ++i) {
				auto entry = TFD::ActorScan::GetEntry(i);
				auto actorSP = entry.actor.get();
				auto* actor = actorSP.get();
				if (!actor || actor->IsDead() || actor->IsDisabled()) {
					continue;
				}
				TFD::AggressionClamp::Apply(actor);
				actor->StopCombat();
				actor->EvaluatePackage(true, false);
			}

			SetPlayerBleedImmune(false);
			QueueNonCaptiveChoiceRequest(reason);
			spdlog::warn("[TFD][Transition] non-captive choice armed reason={}", reason ? reason : "unknown");
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
			ShowBlackoutFader();
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			AdvanceGameHoursSoft(1.0f);
			std::this_thread::sleep_for(std::chrono::milliseconds(900));
			if (!TeleportPlayerToCachedMarkerNow()) {
				HideBlackoutFader();
				EnterNonCaptiveChoice("teleport_failed");
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(150));
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
			ApplyCalmBubble((std::max)(2000.0f, TFD::Settings::GetSweepRadius()), nullptr, true);
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			HideBlackoutFader();
			spdlog::info("[TFD][Captive] entered captivePhase");
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
			if (IsTransitionAwaiting()) {
				MaintainTransitionCalmWindow();
				UpdatePreCombatState();
				return;
			}
			TFD::ForceGreet::Tick();
			NormalizeInvalidCaptivePair();
			if (g_captiveState && g_captivePhase == CaptivePhaseValue::Captive) {
				const bool dialogOpen = IsDialogueOpen();
				if (!dialogOpen && g_prevDialogueOpen) {
					ApplyCalmBubble((std::max)(1800.0f, TFD::Settings::GetSweepRadius()), nullptr, true);
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

				RE::Actor* preferredSpeaker = nullptr;
				if (g_lastAggressor) {
					auto sp = RE::Actor::LookupByHandle(g_lastAggressor.native_handle());
					preferredSpeaker = sp.get();
				}

				const bool dOpen = IsDialogueOpen();
				const int bleedSeconds = TFD::Settings::GetBleedWindowSeconds();

				// Handle active dialogue first. While the menu is open we do not want to keep
				// pulsing calm bubble / crowd refresh because that can leave surrounding actors
				// stuck in a half-hostile draw/sheath loop.
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

				// Consume dialogue-close outcome immediately before any crowd refresh or new greet.
				if (g_bleedSawDialogue && g_prevDialogueOpen) {
					g_prevDialogueOpen = false;
					if (g_bleedPaused) {
						g_bleedStart += (Now() - g_bleedPauseStarted);
						g_bleedPaused = false;
						g_bleedPauseStarted = {};
						g_bleedLastSeconds = -1;
						spdlog::info("[TFD][Defeat] bleed countdown resumed after dialogue");
					}
					if (ResolveCaptiveMarkerForOutcome()) {
						spdlog::info("[TFD][Defeat] bleedout dialogue closed -> captive marker found");
						DoBlackoutTeleport();
						SetGraceSeconds(4);
					}
					else {
						spdlog::info("[TFD][Defeat] bleedout dialogue closed -> no marker -> LeftForDead");
						g_inBleedState.store(false, std::memory_order_release);
						g_minHp = 0.0f;
						g_bleedSawDialogue = false;
						g_bleedLastSeconds = -1;
						EnterNonCaptiveChoice("dialogue_closed_no_marker");
					}
					return;
				}

				if (g_bleedPaused) {
					g_bleedStart += (Now() - g_bleedPauseStarted);
					g_bleedPaused = false;
					g_bleedPauseStarted = {};
					g_bleedLastSeconds = -1;
					spdlog::info("[TFD][Defeat] bleed countdown resumed after dialogue");
				}

				if (g_bleedLastCalmPulse.time_since_epoch().count() == 0 || (Now() - g_bleedLastCalmPulse) >= std::chrono::milliseconds(350)) {
					auto lockedCrowd = ResolveBleedoutCrowdSnapshot((std::max)(12000.0f, TFD::Settings::GetSweepRadius()), preferredSpeaker, true);
					ApplyCalmBubble((std::max)(12000.0f, TFD::Settings::GetSweepRadius()), lockedCrowd.empty() ? nullptr : &lockedCrowd, false, preferredSpeaker);
					g_bleedLastCalmPulse = Now();
				}

				const auto nowBleed = Now();
				if (g_bleedLastCrowdAssign.time_since_epoch().count() == 0 || (nowBleed - g_bleedLastCrowdAssign) >= std::chrono::milliseconds(900)) {
					const float bleedRadius = (std::max)(2400.0f, TFD::Settings::GetSweepRadius());
					auto lockedCrowd = ResolveBleedoutCrowdSnapshot((std::max)(12000.0f, TFD::Settings::GetSweepRadius()), preferredSpeaker, true);
					RefreshBleedoutBridgeCrowd(bleedRadius, preferredSpeaker, false, lockedCrowd.empty() ? nullptr : &lockedCrowd);

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
					ResetBleedRuntimeState();
					if (pendingCaptive && ResolveCaptiveMarkerForOutcome()) {
						spdlog::info("[TFD][Defeat] bleed timeout -> captive blackout");
						DoBlackoutTeleport();
						SetGraceSeconds(4);
					}
					else {
						spdlog::info("[TFD][Defeat] bleed timeout -> noncaptive fallback");
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
		TFD::AntiAggro::CancelPending();
		TFD::AggressionClamp::Clear();
		ResetTransitionCalmState();
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
