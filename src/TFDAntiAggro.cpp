#include "TFDAntiAggro.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <SKSE/SKSE.h>
#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

#include "TFDActorScan.h"
#include "TFDDefeatMonitor.h"

namespace TFD::AntiAggro
{
	namespace
	{
		std::atomic<std::uint32_t> g_waveGeneration{ 1 };
	}

	void SweepOnce(float radius, bool npcOnly)
	{
		if (TFD::DefeatMonitor::IsPlayerBleedHoldTargetBlocked()) {
			return;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}

		player->StopCombat();

		TFD::ActorScan::Rescan(radius, npcOnly);
		const auto count = TFD::ActorScan::GetCount();

		for (std::int32_t i = 0; i < count; i++) {
			auto* a = TFD::ActorScan::GetActor(i);
			if (!a) {
				continue;
			}
			if (a->IsDead() || a->IsDisabled()) {
				continue;
			}

			a->StopCombat();
		}
	}

	void CancelPending()
	{
		const auto next = g_waveGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
		spdlog::info("[TFD][AntiAggro] cancel pending waves generation={}", next);
	}

	void ScheduleWaves(float radius, bool npcOnly, int waves, int intervalMs)
	{
		if (waves <= 0) {
			return;
		}

		const auto generation = g_waveGeneration.load(std::memory_order_acquire);

		std::thread([radius, npcOnly, waves, intervalMs, generation]() {
			for (int i = 0; i < waves; i++) {
				std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));

				if (generation != g_waveGeneration.load(std::memory_order_acquire)) {
					return;
				}

				if (auto* tasks = SKSE::GetTaskInterface()) {
					tasks->AddUITask([radius, npcOnly, generation]() {
						if (generation != g_waveGeneration.load(std::memory_order_acquire)) {
							return;
						}
						SweepOnce(radius, npcOnly);
					});
				}
			}
		}).detach();

		spdlog::info("[TFD][AntiAggro] scheduled {} waves ({}ms) generation={}", waves, intervalMs, generation);
	}
}
