#include "TFDDefeatCaptiveTickWiring.h"

#include <mutex>
#include <utility>

#include <spdlog/spdlog.h>

namespace TFD::DefeatCaptiveTickWiring
{
	namespace
	{
		std::mutex g_providerLock;
		Dependencies g_dependencies{};
		bool g_hasProvider = false;

		Dependencies ResolveDependencies()
		{
			std::scoped_lock lock(g_providerLock);
			return g_dependencies;
		}
	}

	void InstallProvider(Dependencies dependencies)
	{
		std::scoped_lock lock(g_providerLock);
		g_dependencies = std::move(dependencies);
		g_hasProvider = true;
		spdlog::info("[TFD][CaptiveTick][P21] provider installed");
	}

	void ShutdownProvider()
	{
		std::scoped_lock lock(g_providerLock);
		g_dependencies = {};
		g_hasProvider = false;
		spdlog::info("[TFD][CaptiveTick][P21] provider cleared");
	}

	bool HasProvider()
	{
		std::scoped_lock lock(g_providerLock);
		return g_hasProvider;
	}

	TFD::Captive::EscapeTickHandlers BuildEscapeTickHandlers()
	{
		const auto dependencies = ResolveDependencies();
		TFD::Captive::EscapeTickHandlers handlers{};
		handlers.updatePreCombatState = [dependencies]() {
			if (dependencies.updatePreCombatState) {
				dependencies.updatePreCombatState();
			}
		};
		handlers.setPrevDialogueOpen = [dependencies](bool open) {
			if (dependencies.setDialogueOpenObserved) {
				dependencies.setDialogueOpenObserved(open);
			}
		};
		handlers.clearLastAggressor = [dependencies]() {
			if (dependencies.clearLastAggressor) {
				dependencies.clearLastAggressor();
			}
		};
		handlers.setLastAggressor = [dependencies](RE::Actor* actor) {
			if (dependencies.setLastAggressor) {
				dependencies.setLastAggressor(actor);
			}
		};
		handlers.resolveAggressor = [dependencies]() -> RE::Actor* {
			return dependencies.resolveAggressor ? dependencies.resolveAggressor() : nullptr;
		};
		handlers.findBestAggressor = [dependencies](float radius) -> RE::Actor* {
			return dependencies.findBestAggressor ? dependencies.findBestAggressor(radius) : nullptr;
		};
		handlers.setGraceActive = [dependencies](bool active) {
			if (dependencies.setGraceActive) {
				dependencies.setGraceActive(active);
			}
		};
		return handlers;
	}

	TFD::Captive::RuntimeTickHandlers BuildRuntimeTickHandlers()
	{
		const auto dependencies = ResolveDependencies();
		TFD::Captive::RuntimeTickHandlers handlers{};
		handlers.isDialogueOpen = [dependencies]() -> bool {
			return dependencies.isDialogueOpen ? dependencies.isDialogueOpen() : false;
		};
		handlers.getPrevDialogueOpen = [dependencies]() -> bool {
			return dependencies.wasDialogueOpen ? dependencies.wasDialogueOpen() : false;
		};
		handlers.setPrevDialogueOpen = [dependencies](bool open) {
			if (dependencies.setDialogueOpenObserved) {
				dependencies.setDialogueOpenObserved(open);
			}
		};
		handlers.escape = BuildEscapeTickHandlers();
		return handlers;
	}

	bool TickRuntime(RE::Actor* player, bool captiveBleedOverlay)
	{
		if (!HasProvider()) {
			spdlog::warn("[TFD][CaptiveTick][P21] TickRuntime skipped reason=no_provider");
			return true;
		}

		return TFD::Captive::TickRuntime(player, captiveBleedOverlay, BuildRuntimeTickHandlers());
	}
}
