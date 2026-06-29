#pragma once

#include <RE/Skyrim.h>

#include <cstdint>

namespace TFD::VisualProbe
{
	struct Result
	{
		std::uint32_t actorFormID{ 0 };
		bool actorValid{ false };
		bool loaded{ false };
		bool rootOk{ false };
		bool pelvisOk{ false };
		bool hasFiniteTransform{ true };
		bool stretchSuspect{ false };
		bool severeStretchSuspect{ false };
		bool verticalData{ false };
		bool headData{ false };
		bool footData{ false };
		bool feetNearActorZ{ false };
		bool footIKSnapSuspect{ false };
		bool upperBodyFloatSuspect{ false };
		bool blendContaminationSuspect{ false };
		bool verticalSuspect{ false };
		int sampledNodes{ 0 };
		int missingNodes{ 0 };
		float boundRadius{ 0.0f };
		float maxBoneDistance{ 0.0f };
		const char* farNode{ "-" };
		float headDistance{ -1.0f };
		float leftHandDistance{ -1.0f };
		float rightHandDistance{ -1.0f };
		float leftFootDistance{ -1.0f };
		float rightFootDistance{ -1.0f };
		float actorZ{ -9999.0f };
		float rootZ{ -9999.0f };
		float pelvisZ{ -9999.0f };
		float headZ{ -9999.0f };
		float leftFootZ{ -9999.0f };
		float rightFootZ{ -9999.0f };
		float minFootZ{ -9999.0f };
		float maxFootZ{ -9999.0f };
		float footSpanZ{ -1.0f };
		float rootMinusFootZ{ -9999.0f };
		float pelvisMinusFootZ{ -9999.0f };
		float headMinusFootZ{ -9999.0f };
		float rootMinusActorZ{ -9999.0f };
		float pelvisMinusActorZ{ -9999.0f };
		float headMinusActorZ{ -9999.0f };
		float minFootMinusActorZ{ -9999.0f };
	};

	Result ProbeActor(RE::Actor* actor);
	bool IsActorVisualReady(RE::Actor* actor, const char* reason = nullptr, bool requireLoaded = true);
	bool IsActorVerticalSuspect(RE::Actor* actor, const char* reason = nullptr);
	bool RunActorImpactRecoveryPulse(RE::Actor* actor, const char* reason = nullptr);
	std::uint32_t TrackActorForHitTrace(RE::Actor* actor, const char* reason = nullptr, int durationMs = 12000);
	void InstallHitTrace();
	Result LogActor(RE::Actor* actor, const char* reason = nullptr, int pass = -1);
	Result LogWatchSample(RE::Actor* actor, const char* reason, std::uint32_t watchId, int sample, int elapsedMs, const char* runtimePhaseName);
	void LogDelta(const char* reason, const char* step, int pass, bool actionOk, const Result& before, const Result& after);
	bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm);
}
