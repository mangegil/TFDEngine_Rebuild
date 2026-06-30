#pragma once

namespace TFD::DefeatTerminalLockout
{
	bool IsBleedoutTerminalOutcomeEvent(const char* rawName);
	double ResolveSeconds(const char* rawName, float numArg);
	void Arm(const char* eventName, double seconds, const char* reason);
	bool IsActive(const char* checkReason, float hpPct, float thresholdPct);
	void Clear(const char* reason = nullptr);
}
