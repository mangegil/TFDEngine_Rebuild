#pragma once

#include <cstdint>
#include <string>

#include "RE/Skyrim.h"

namespace TFD::ActorScan
{
	struct Entry
	{
		RE::ActorHandle actor;
		float dist{ 0.0f };
		bool hostile{ false };
		bool inCombat{ false };
	};

	std::int32_t Rescan(float radius, bool npcOnly);
	std::int32_t GetCount();
	RE::Actor* GetActor(std::int32_t index);
	Entry GetEntry(std::int32_t index);
	std::string GetActorName(std::int32_t index);
	RE::Actor* GetBestPreCombatCandidate();
}
