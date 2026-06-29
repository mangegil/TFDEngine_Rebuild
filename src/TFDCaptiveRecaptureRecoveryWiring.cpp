#include "TFDCaptiveRecaptureRecoveryWiring.h"

#include "TFDCaptiveRecaptureRecoveryPulse.h"

#include <mutex>
#include <utility>

namespace TFD::CaptiveRecaptureRecoveryWiring
{
	namespace
	{
		std::mutex g_dependenciesLock;
		Dependencies g_dependencies;
		bool g_hasDependencies = false;

		Dependencies ResolveDependencies()
		{
			std::scoped_lock lock(g_dependenciesLock);
			return g_dependencies;
		}
	}

	void SetDependencies(Dependencies dependencies)
	{
		std::scoped_lock lock(g_dependenciesLock);
		g_dependencies = std::move(dependencies);
		g_hasDependencies = true;
	}

	void ClearDependencies()
	{
		std::scoped_lock lock(g_dependenciesLock);
		g_dependencies = {};
		g_hasDependencies = false;
	}

	bool HasDependencies()
	{
		std::scoped_lock lock(g_dependenciesLock);
		return g_hasDependencies;
	}

	TFD::CaptiveRecaptureRecovery::Context BuildContext()
	{
		const auto dependencies = ResolveDependencies();
		TFD::CaptiveRecaptureRecovery::Context context{};
		context.getPlayer = dependencies.getPlayer;
		context.releasePlayerBleedLock = dependencies.releasePlayerBleedLock;
		context.restoreActorHealthToSafePct = dependencies.restoreActorHealthToSafePct;
		context.resetBleedRuntimeState = dependencies.resetBleedRuntimeState;
		context.forceStopBleedRuntimeForCaptiveRecapture = dependencies.forceStopBleedRuntimeForCaptiveRecapture;
		context.setPlayerBleedImmune = dependencies.setPlayerBleedImmune;
		context.playPlayerGetUp = dependencies.playPlayerGetUp;
		context.refreshPostDefeatGlobals = dependencies.refreshPostDefeatGlobals;
		context.getActorHealthPct = dependencies.getActorHealthPct;
		context.getDefeatThresholdPct = dependencies.getDefeatThresholdPct;
		return context;
	}

	TFD::CaptiveRecaptureRecoveryPulse::Context BuildPulseContext()
	{
		const auto dependencies = ResolveDependencies();
		TFD::CaptiveRecaptureRecoveryPulse::Context context{};
		context.getPlayer = dependencies.getPlayer;
		context.recoverPlayer = [](const char* reason) {
			TFD::CaptiveRecaptureRecovery::ForceRecoverPlayer(reason);
			};
		context.isActorBleedingOut = dependencies.isActorBleedingOut;
		context.getActorHealthPct = dependencies.getActorHealthPct;
		context.getDefeatThresholdPct = dependencies.getDefeatThresholdPct;
		return context;
	}

	void ArmPulse(const char* reason)
	{
		TFD::CaptiveRecaptureRecoveryPulse::Arm(BuildPulseContext(), reason);
	}

	void TickPulse()
	{
		TFD::CaptiveRecaptureRecoveryPulse::Tick(BuildPulseContext());
	}

	void ResetPulse()
	{
		TFD::CaptiveRecaptureRecoveryPulse::Reset();
	}

	void InstallProvider(Dependencies dependencies)
	{
		SetDependencies(std::move(dependencies));
		TFD::CaptiveRecaptureRecovery::SetContextProvider([]() { return BuildContext(); });
	}

	void ShutdownProvider()
	{
		ResetPulse();
		TFD::CaptiveRecaptureRecovery::ClearContextProvider();
		ClearDependencies();
	}
}
