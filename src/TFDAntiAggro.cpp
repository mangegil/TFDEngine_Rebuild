#include "TFDAntiAggro.h"

#include <chrono>
#include <thread>

#include <SKSE/SKSE.h>
#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

#include "TFDActorScan.h"

namespace TFD::AntiAggro
{
	void SweepOnce(float radius, bool npcOnly)
	{
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

	void ScheduleWaves(float radius, bool npcOnly, int waves, int intervalMs)
	{
		if (waves <= 0) {
			return;
		}

		std::thread([radius, npcOnly, waves, intervalMs]() {
			for (int i = 0; i < waves; i++) {
				std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));

				if (auto* tasks = SKSE::GetTaskInterface()) {
					tasks->AddUITask([radius, npcOnly]() {
						SweepOnce(radius, npcOnly);
					});
				}
			}
		}).detach();

		spdlog::info("[TFD][AntiAggro] scheduled {} waves ({}ms)", waves, intervalMs);
	}
}
