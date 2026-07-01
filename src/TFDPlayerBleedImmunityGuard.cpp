#include "TFDPlayerBleedImmunityGuard.h"

#include "TFDPlayerDamageGuard.h"

namespace TFD::PlayerBleedImmunityGuard
{
	namespace
	{
		RE::Actor* ResolvePlayer()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

		const char* ResolveReason(bool active, const char* reason)
		{
			if (reason && reason[0]) {
				return reason;
			}
			return active ? "defeat_monitor_request_enable" : "defeat_monitor_request_release";
		}
	}

	bool IsActive()
	{
		return TFD::PlayerDamageGuard::IsHardImmunityActive();
	}

	void SetActive(RE::Actor* player, bool active, const char* reason)
	{
		if (!player) {
			return;
		}

		TFD::PlayerDamageGuard::SetHardImmunity(player, active, ResolveReason(active, reason));
	}

	void SetPlayerActive(bool active, const char* reason)
	{
		SetActive(ResolvePlayer(), active, reason);
	}

	void Reset(RE::Actor* player, const char* reason)
	{
		SetActive(player ? player : ResolvePlayer(), false, reason ? reason : "player_bleed_immunity_reset");
	}
}
