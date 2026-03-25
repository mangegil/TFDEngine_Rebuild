#pragma once

#include <cstdint>

namespace TFD::Settings
{
	void InitProfileIniPersistence();
	void FlushNow();

	bool  GetEnabled();
	void  SetEnabled(bool a_enabled);

	float GetDefeatThresholdPct();
	void  SetDefeatThresholdPct(float a_pct);

	float GetAllyDownedThresholdPct();
	void  SetAllyDownedThresholdPct(float a_pct);

	float GetEnemyDownedThresholdPct();
	void  SetEnemyDownedThresholdPct(float a_pct);

	int   GetBleedWindowSeconds();
	void  SetBleedWindowSeconds(int a_seconds);

	float GetScanRadius();
	void  SetScanRadius(float a_radius);

	float GetSweepRadius();
	void  SetSweepRadius(float a_radius);

	bool GetHotkeyEnabled();
	void SetHotkeyEnabled(bool a_enabled);

	std::uint32_t GetHotkeyScanCode();
	void SetHotkeyScanCode(std::uint32_t a_code);

	int GetHotkeyCooldownMs();
	void SetHotkeyCooldownMs(int a_ms);

	bool GetHotkeyWave();
	void SetHotkeyWave(bool a_enabled);
}