#pragma once

#include <cstdint>
#include <string_view>

#include <RE/Skyrim.h>

namespace TFD::PleasureRuntime
{
	enum class Phase : std::uint8_t
	{
		Idle = 0,
		PleasureStartPending,
		PleasureActive,
		PleasureEnding,
		AfterPleasureAwaitQuest,
		AfterPleasureDialogue,
		RedoPending,
		Finalizing,
		Closed
	};

	enum class SourceContext : std::uint8_t
	{
		None = 0,
		PreCombat,
		Bleedout,
		Captive,
		Victory,
		Teammate,
		InCombat
	};

	enum class AfterChoice : std::uint8_t
	{
		None = 0,
		Redo,
		Finish,
		Recruit,
		JoinEnemy,
		Release,
		Work,
		Captive
	};

	struct EventInfo
	{
		RE::Actor* actor = nullptr;
		std::uint32_t actorFormID = 0;
		std::uint32_t senderFormID = 0;
		int sourceFlow = 0;
		int threadID = -1;
	};

	void Install();
	void Shutdown();
	void Tick();

	void ResetRuntime(std::string_view reason);
	void ResetForLoad(std::string_view reason);

	bool BeginPleasure(RE::Actor* speaker, SourceContext source, std::string_view reason);
	bool QueueNextCycleAfterTerminal(RE::Actor* currentActor, SourceContext source, std::string_view reason);
	bool HasQueuedCycleForConsumedActor(RE::Actor* currentActor, SourceContext source);
	bool HasQueuedCycleForConsumedActor(std::uint32_t currentActorFormID, SourceContext source);
	bool HandleModEvent(const RE::BSFixedString& name, const RE::BSFixedString& strArg, float numArg, RE::TESForm* sender);

	void NoteAfterPleasureDialogueCommitted(std::string_view reason);
	void Break(std::string_view reason, bool clearHold, bool clearBridge, bool completeFlow);

	bool IsActive();
	bool IsBlocking();
	bool IsPassiveLockActive();

	Phase GetPhase();
	const char* GetPhaseName();

	SourceContext GetSourceContext();
	const char* GetSourceContextName();

	AfterChoice GetPendingAfterChoice();
	std::uint32_t GetSessionCycleId();

	RE::Actor* GetPleasureSpeaker();
	RE::Actor* GetAfterPleasureSpeaker();
	RE::Actor* GetPrimarySpeaker();

	bool IsActorTracked(RE::Actor* actor);
	bool ShouldProtectPendingDialogue(RE::Actor* actor);
	bool IsInCombatPleasureChainActive();
	std::size_t FlushDeferredInCombatRecruits(std::string_view reason);
}
