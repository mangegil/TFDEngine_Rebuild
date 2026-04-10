#include "TFDCaptiveRuntime.h"

namespace TFD::CaptiveRuntime
{
	namespace
	{
		bool g_state = false;
		PhaseValue g_phase = PhaseValue::None;
		bool g_confiscationApplied = false;
		bool g_confiscationPending = false;
		bool g_starterLockpickPending = false;
		std::string g_pendingConfiscationReason{};
		int g_confiscationAttemptCount = 0;
		std::chrono::steady_clock::time_point g_confiscationNextAttempt{};
		QuestRegistryCache g_registry{};
		TFD::CaptiveDoorController g_door{};
		RE::ObjectRefHandle g_marker{};
		RE::FormID g_cellFormID = 0;
		RE::FormID g_locationFormID = 0;
		bool g_queuedState = false;
		PhaseValue g_queuedPhase = PhaseValue::None;
	}

	bool& StateRef() { return g_state; }
	PhaseValue& PhaseRef() { return g_phase; }
	bool& ConfiscationAppliedRef() { return g_confiscationApplied; }
	bool& ConfiscationPendingRef() { return g_confiscationPending; }
	bool& StarterLockpickPendingRef() { return g_starterLockpickPending; }
	std::string& PendingConfiscationReasonRef() { return g_pendingConfiscationReason; }
	int& ConfiscationAttemptCountRef() { return g_confiscationAttemptCount; }
	std::chrono::steady_clock::time_point& ConfiscationNextAttemptRef() { return g_confiscationNextAttempt; }
	QuestRegistryCache& QuestRegistryRef() { return g_registry; }
	TFD::CaptiveDoorController& DoorControllerRef() { return g_door; }
	RE::ObjectRefHandle& MarkerRef() { return g_marker; }
	RE::FormID& CellFormIDRef() { return g_cellFormID; }
	RE::FormID& LocationFormIDRef() { return g_locationFormID; }
	bool& QueuedStateRef() { return g_queuedState; }
	PhaseValue& QueuedPhaseRef() { return g_queuedPhase; }

	PhaseValue PhaseFromRaw(std::uint32_t raw)
	{
		switch (raw) {
		case 1: return PhaseValue::Captive;
		case 2: return PhaseValue::Escape;
		case 3: return PhaseValue::ReleasedWork;
		case 4: return PhaseValue::Scene;
		default: return PhaseValue::None;
		}
	}

	std::uint32_t GetPhaseRaw(bool stateActive, PhaseValue phase)
	{
		if (stateActive && phase == PhaseValue::None) {
			return 2u;
		}
		return static_cast<std::uint32_t>(static_cast<int>(phase));
	}

	const char* GetPhaseName(bool stateActive, PhaseValue phase)
	{
		switch (phase) {
		case PhaseValue::None:
			return stateActive ? "Escape" : "None";
		case PhaseValue::Captive:
			return "Captive";
		case PhaseValue::Escape:
			return "Escape";
		case PhaseValue::ReleasedWork:
			return "ReleasedWork";
		case PhaseValue::Scene:
			return "Scene";
		default:
			return "Unknown";
		}
	}

	bool IsFamily(bool stateActive, PhaseValue phase)
	{
		return stateActive || phase != PhaseValue::None;
	}

	bool IsActive()
	{
		return IsFamily(g_state, g_phase);
	}

	bool IsEscapeActive()
	{
		return g_phase == PhaseValue::Escape;
	}

	bool IsStandardCaptiveActive()
	{
		return g_phase == PhaseValue::Captive;
	}

	void ResetForLoad()
	{
		g_state = false;
		g_phase = PhaseValue::None;
		g_confiscationApplied = false;
		g_confiscationPending = false;
		g_starterLockpickPending = false;
		g_pendingConfiscationReason.clear();
		g_confiscationAttemptCount = 0;
		g_confiscationNextAttempt = {};
		g_registry = {};
		g_door.Reset();
		g_marker.reset();
		g_cellFormID = 0;
		g_locationFormID = 0;
		g_queuedState = false;
		g_queuedPhase = PhaseValue::None;
	}
}
