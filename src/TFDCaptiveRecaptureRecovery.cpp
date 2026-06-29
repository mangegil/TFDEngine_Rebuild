#include "TFDCaptiveRecaptureRecovery.h"

#include <spdlog/spdlog.h>

#include <mutex>

namespace TFD::CaptiveRecaptureRecovery
{
	namespace
	{
		std::mutex g_contextProviderLock;
		ContextProvider g_contextProvider;

		Context ResolveContextFromProvider()
		{
			ContextProvider provider;
			{
				std::scoped_lock lock(g_contextProviderLock);
				provider = g_contextProvider;
			}

			if (provider) {
				return provider();
			}

			return {};
		}

		RE::Actor* ResolvePlayer(const Context& context)
		{
			return context.getPlayer ? context.getPlayer() : nullptr;
		}

		float ResolveHealthPct(const Context& context, RE::Actor* actor)
		{
			return actor && context.getActorHealthPct ? context.getActorHealthPct(actor) : 0.0f;
		}

		float ResolveDefeatThresholdPct(const Context& context)
		{
			return context.getDefeatThresholdPct ? context.getDefeatThresholdPct() : 0.0f;
		}

		void ResetBleedRuntime(const Context& context, bool preserveCaptive)
		{
			if (context.resetBleedRuntimeState) {
				context.resetBleedRuntimeState(preserveCaptive);
			}
		}

		void ForceStopBleedRuntime(const Context& context, const char* reason)
		{
			if (context.forceStopBleedRuntimeForCaptiveRecapture) {
				context.forceStopBleedRuntimeForCaptiveRecapture(reason);
			}
		}

		void RefreshPostDefeat(const Context& context)
		{
			if (context.refreshPostDefeatGlobals) {
				context.refreshPostDefeatGlobals();
			}
		}
	}

	void SetContextProvider(ContextProvider provider)
	{
		std::scoped_lock lock(g_contextProviderLock);
		g_contextProvider = std::move(provider);
	}

	void ClearContextProvider()
	{
		std::scoped_lock lock(g_contextProviderLock);
		g_contextProvider = nullptr;
	}

	bool HasContextProvider()
	{
		std::scoped_lock lock(g_contextProviderLock);
		return static_cast<bool>(g_contextProvider);
	}

	void ForceRecoverPlayer(const char* reason)
	{
		Context context = ResolveContextFromProvider();
		if (!context.getPlayer && !context.resetBleedRuntimeState && !context.forceStopBleedRuntimeForCaptiveRecapture) {
			spdlog::error("[TFD][CaptiveRecapture][P5] recovery context provider missing reason={}", reason ? reason : "unknown");
			return;
		}

		ForceRecoverPlayer(context, reason);
	}

	void ForceRecoverPlayer(const Context& context, const char* reason)
	{
		const char* why = reason ? reason : "captive_recapture_recover_player";
		auto* player = ResolvePlayer(context);
		if (!player) {
			ResetBleedRuntime(context, true);
			ForceStopBleedRuntime(context, why);
			RefreshPostDefeat(context);
			spdlog::warn("[TFD][CaptiveRecapture] player recover skipped: player missing reason={}", why);
			return;
		}

		if (context.releasePlayerBleedLock) {
			context.releasePlayerBleedLock(why, true);
		}

		if (context.restoreActorHealthToSafePct) {
			context.restoreActorHealthToSafePct(
				player,
				ResolveDefeatThresholdPct(context),
				10.0f,
				90.0f,
				95.0f,
				25.0f,
				why);
		}

		ResetBleedRuntime(context, true);
		ForceStopBleedRuntime(context, why);

		if (context.setPlayerBleedImmune) {
			context.setPlayerBleedImmune(false);
		}

		if (context.playPlayerGetUp) {
			context.playPlayerGetUp(player);
		}

		RefreshPostDefeat(context);
		spdlog::info("[TFD][CaptiveRecapture] player recovered hpPct={:.1f} reason={}",
			ResolveHealthPct(context, player),
			why);
	}
}
