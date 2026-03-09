#pragma once

namespace TFD::AntiAggro
{
	// One-shot stop combat for actors around player.
	void SweepOnce(float radius, bool npcOnly);

	// Multi-wave sweeps on a background thread; schedules work on UI thread.
	void ScheduleWaves(float radius, bool npcOnly, int waves, int intervalMs);
}
