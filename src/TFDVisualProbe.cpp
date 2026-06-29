#include "TFDVisualProbe.h"

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace TFD::VisualProbe
{
	namespace
	{
		constexpr float kMissingZ = -9999.0f;

		struct TrackedActorState
		{
			std::uint32_t actorFormID{ 0 };
			std::uint32_t traceId{ 0 };
			std::chrono::steady_clock::time_point expiresAt{};
			std::string reason;
			Result lastProbe{};
			bool hasLastProbe{ false };
			bool everFault{ false };
			int lastSample{ -1 };
			int lastElapsedMs{ -1 };
			int hitCount{ 0 };
			int nativeRecoveryAttempts{ 0 };
		};

		std::mutex g_traceLock;
		std::unordered_map<std::uint32_t, TrackedActorState> g_trackedActors;
		std::atomic<std::uint32_t> g_traceSerial{ 0 };
		std::atomic_bool g_hitTraceInstalled{ false };

		struct NodeSpec
		{
			const char* key;
			const char* name;
			float suspectDistance;
			float severeDistance;
		};


		constexpr NodeSpec kNodeSpecs[] = {
			{ "Head", "NPC Head [Head]", 220.0f, 420.0f },
			{ "LHand", "NPC L Hand [LHnd]", 260.0f, 520.0f },
			{ "RHand", "NPC R Hand [RHnd]", 260.0f, 520.0f },
			{ "LFoot", "NPC L Foot [Lft ]", 280.0f, 560.0f },
			{ "RFoot", "NPC R Foot [Rft ]", 280.0f, 560.0f }
		};

		bool IsFinitePoint(const RE::NiPoint3& point)
		{
			return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
		}

		bool IsFiniteValue(float value)
		{
			return std::isfinite(value);
		}

		float Distance(const RE::NiPoint3& lhs, const RE::NiPoint3& rhs)
		{
			const float dx = lhs.x - rhs.x;
			const float dy = lhs.y - rhs.y;
			const float dz = lhs.z - rhs.z;
			return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
		}

		RE::NiAVObject* FindNode(RE::NiNode* rootNode, const char* name)
		{
			if (!rootNode || !name || !name[0]) {
				return nullptr;
			}

			return rootNode->GetObjectByName(RE::BSFixedString(name));
		}

		bool ReadNodePoint(RE::NiNode* rootNode, const char* nodeName, RE::NiPoint3& out)
		{
			auto* node = FindNode(rootNode, nodeName);
			if (!node || !IsFinitePoint(node->world.translate)) {
				return false;
			}
			out = node->world.translate;
			return true;
		}

		float ReadDistanceOrMinusOne(RE::NiNode* rootNode, const RE::NiPoint3& origin, const char* nodeName)
		{
			RE::NiPoint3 point{};
			if (!ReadNodePoint(rootNode, nodeName, point)) {
				return -1.0f;
			}
			return Distance(point, origin);
		}

		float GetHealthPct(RE::Actor* actor)
		{
			if (!actor) {
				return 0.0f;
			}
			const float maxHealth = actor->GetPermanentActorValue(RE::ActorValue::kHealth);
			if (maxHealth <= 0.0f) {
				return 0.0f;
			}
			return (actor->GetActorValue(RE::ActorValue::kHealth) / maxHealth) * 100.0f;
		}

		bool IsBleedingOut(RE::Actor* actor)
		{
			auto* state = actor ? actor->AsActorState() : nullptr;
			return state && state->IsBleedingOut();
		}

		bool HasVisualFault(const Result& result)
		{
			return result.verticalSuspect || result.stretchSuspect || result.severeStretchSuspect || !result.hasFiniteTransform;
		}

		void PurgeExpiredTraceLocked(const std::chrono::steady_clock::time_point now)
		{
			for (auto it = g_trackedActors.begin(); it != g_trackedActors.end();) {
				if (it->second.expiresAt <= now) {
					it = g_trackedActors.erase(it);
				} else {
					++it;
				}
			}
		}

		void UpdateTrackedSnapshot(RE::Actor* actor, const Result& result, const char* reason, int sample, int elapsedMs)
		{
			if (!actor || result.actorFormID == 0) {
				return;
			}

			const auto now = std::chrono::steady_clock::now();
			std::scoped_lock lock(g_traceLock);
			PurgeExpiredTraceLocked(now);

			auto it = g_trackedActors.find(result.actorFormID);
			if (it == g_trackedActors.end()) {
				return;
			}

			auto& state = it->second;
			const bool beforeFault = state.hasLastProbe && HasVisualFault(state.lastProbe);
			const bool afterFault = HasVisualFault(result);
			const bool changed = state.hasLastProbe && beforeFault != afterFault;
			const bool recovered = changed && beforeFault && !afterFault;
			const bool wentBad = changed && !beforeFault && afterFault;

			if (changed) {
				spdlog::info(
					"[TFD][VisualTrace][R371A] visual state transition actor={:08X} trace={} reason={} sample={} elapsedMs={} beforeFault={} afterFault={} recovered={} wentBad={} hitCount={} nativeRecoveryAttempts={} beforeVertical={} afterVertical={} beforeFootActorZ={:.1f} afterFootActorZ={:.1f} beforeRootFootZ={:.1f} afterRootFootZ={:.1f} beforeFinite={} afterFinite={} trackedReason={}",
					result.actorFormID,
					state.traceId,
					reason && reason[0] ? reason : "visual_trace_snapshot",
					sample,
					elapsedMs,
					beforeFault ? 1 : 0,
					afterFault ? 1 : 0,
					recovered ? 1 : 0,
					wentBad ? 1 : 0,
					state.hitCount,
					state.nativeRecoveryAttempts,
					state.lastProbe.verticalSuspect ? 1 : 0,
					result.verticalSuspect ? 1 : 0,
					state.lastProbe.minFootMinusActorZ,
					result.minFootMinusActorZ,
					state.lastProbe.rootMinusFootZ,
					result.rootMinusFootZ,
					state.lastProbe.hasFiniteTransform ? 1 : 0,
					result.hasFiniteTransform ? 1 : 0,
					state.reason);
			}

			state.lastProbe = result;
			state.hasLastProbe = true;
			state.everFault = state.everFault || afterFault;
			state.lastSample = sample;
			state.lastElapsedMs = elapsedMs;
		}

		void LogTrackedNativeRecovery(RE::Actor* actor, const char* reason, const Result& before, const Result& after, bool actionOk)
		{
			if (!actor) {
				return;
			}

			const auto formID = actor->GetFormID();
			const auto now = std::chrono::steady_clock::now();
			std::scoped_lock lock(g_traceLock);
			PurgeExpiredTraceLocked(now);

			auto it = g_trackedActors.find(formID);
			if (it == g_trackedActors.end()) {
				return;
			}

			auto& state = it->second;
			++state.nativeRecoveryAttempts;
			const bool beforeFault = HasVisualFault(before);
			const bool afterFault = HasVisualFault(after);
			const bool recovered = beforeFault && !afterFault;
			spdlog::info(
				"[TFD][VisualTrace][R371A] native recovery trace actor={:08X} trace={} reason={} actionOk={} attempt={} beforeFault={} afterFault={} recovered={} beforeVertical={} afterVertical={} beforeFootActorZ={:.1f} afterFootActorZ={:.1f} hitCount={} trackedReason={}",
				formID,
				state.traceId,
				reason && reason[0] ? reason : "native_recovery",
				actionOk ? 1 : 0,
				state.nativeRecoveryAttempts,
				beforeFault ? 1 : 0,
				afterFault ? 1 : 0,
				recovered ? 1 : 0,
				before.verticalSuspect ? 1 : 0,
				after.verticalSuspect ? 1 : 0,
				before.minFootMinusActorZ,
				after.minFootMinusActorZ,
				state.hitCount,
				state.reason);
			state.lastProbe = after;
			state.hasLastProbe = true;
			state.everFault = state.everFault || afterFault;
		}

		void FillVerticalDiagnostics(Result& result, RE::Actor* actor, RE::NiAVObject* rootObject, RE::NiNode* rootNode, const RE::NiPoint3& rootOrigin)
		{
			if (!actor || !rootObject || !rootNode || !IsFinitePoint(rootOrigin)) {
				return;
			}

			const auto actorPos = actor->GetPosition();
			if (!IsFinitePoint(actorPos)) {
				result.hasFiniteTransform = false;
				return;
			}

			result.verticalData = true;
			result.actorZ = actorPos.z;
			result.rootZ = rootOrigin.z;
			result.rootMinusActorZ = result.rootZ - result.actorZ;

			RE::NiPoint3 pelvisPoint{};
			if (ReadNodePoint(rootNode, "NPC Pelvis [Pelv]", pelvisPoint)) {
				result.pelvisZ = pelvisPoint.z;
				result.pelvisMinusActorZ = result.pelvisZ - result.actorZ;
			}

			RE::NiPoint3 headPoint{};
			if (ReadNodePoint(rootNode, "NPC Head [Head]", headPoint)) {
				result.headData = true;
				result.headZ = headPoint.z;
				result.headMinusActorZ = result.headZ - result.actorZ;
			}

			RE::NiPoint3 leftFootPoint{};
			RE::NiPoint3 rightFootPoint{};
			const bool leftFootOk = ReadNodePoint(rootNode, "NPC L Foot [Lft ]", leftFootPoint);
			const bool rightFootOk = ReadNodePoint(rootNode, "NPC R Foot [Rft ]", rightFootPoint);

			if (leftFootOk) {
				result.leftFootZ = leftFootPoint.z;
			}
			if (rightFootOk) {
				result.rightFootZ = rightFootPoint.z;
			}

			if (!leftFootOk && !rightFootOk) {
				return;
			}

			result.footData = true;
			if (leftFootOk && rightFootOk) {
				result.minFootZ = (std::min)(result.leftFootZ, result.rightFootZ);
				result.maxFootZ = (std::max)(result.leftFootZ, result.rightFootZ);
				result.footSpanZ = std::fabs(result.leftFootZ - result.rightFootZ);
			} else if (leftFootOk) {
				result.minFootZ = result.leftFootZ;
				result.maxFootZ = result.leftFootZ;
				result.footSpanZ = 0.0f;
			} else {
				result.minFootZ = result.rightFootZ;
				result.maxFootZ = result.rightFootZ;
				result.footSpanZ = 0.0f;
			}

			result.minFootMinusActorZ = result.minFootZ - result.actorZ;
			result.rootMinusFootZ = result.rootZ - result.minFootZ;
			if (result.pelvisOk && IsFiniteValue(result.pelvisZ) && result.pelvisZ > kMissingZ + 1.0f) {
				result.pelvisMinusFootZ = result.pelvisZ - result.minFootZ;
			}
			if (result.headData) {
				result.headMinusFootZ = result.headZ - result.minFootZ;
			}

			const bool anyFootNearActorZ = std::fabs(result.minFootZ - result.actorZ) <= 24.0f || std::fabs(result.maxFootZ - result.actorZ) <= 24.0f;
			const bool bothFeetCloseTogetherZ = !leftFootOk || !rightFootOk || result.footSpanZ <= 28.0f;
			result.feetNearActorZ = anyFootNearActorZ;
			result.footIKSnapSuspect = anyFootNearActorZ && bothFeetCloseTogetherZ;

			// R364A: this is a blend/IK diagnostic, not a hard truth flag.
			// User-observed bug: feet look snapped to ground while upper body/head/root looks pulled upward.
			// The thresholds are intentionally separate from classic bone-distance stretching.
			const bool rootHighFromFeet = result.rootMinusFootZ >= 72.0f;
			const bool pelvisHighFromFeet = result.pelvisMinusFootZ >= 92.0f;
			const bool headHighFromFeet = result.headMinusFootZ >= 142.0f;
			const bool rootHighFromActor = result.rootMinusActorZ >= 48.0f;
			const bool pelvisHighFromActor = result.pelvisMinusActorZ >= 76.0f;
			result.upperBodyFloatSuspect = rootHighFromFeet || pelvisHighFromFeet || headHighFromFeet || rootHighFromActor || pelvisHighFromActor;
			result.blendContaminationSuspect = result.footIKSnapSuspect && result.upperBodyFloatSuspect && !IsBleedingOut(actor);
			result.verticalSuspect = result.upperBodyFloatSuspect || result.blendContaminationSuspect;
		}
	}

	Result ProbeActor(RE::Actor* actor)
	{
		Result result{};
		result.actorFormID = actor ? actor->GetFormID() : 0u;

		if (!actor || actor->IsDisabled() || actor->IsDead()) {
			return result;
		}

		result.actorValid = true;
		result.loaded = actor->Is3DLoaded();

		auto* rootObject = actor->Get3D(false);
		if (!rootObject) {
			return result;
		}

		result.rootOk = true;
		result.boundRadius = rootObject->worldBound.radius;
		if (!std::isfinite(result.boundRadius)) {
			result.boundRadius = -1.0f;
			result.hasFiniteTransform = false;
		}

		auto* rootNode = rootObject->AsNode();
		if (!rootNode) {
			return result;
		}

		const RE::NiPoint3 rootOrigin = rootObject->world.translate;
		RE::NiPoint3 origin = rootOrigin;
		if (!IsFinitePoint(origin)) {
			result.hasFiniteTransform = false;
			return result;
		}

		if (auto* pelvisNode = FindNode(rootNode, "NPC Pelvis [Pelv]")) {
			if (IsFinitePoint(pelvisNode->world.translate)) {
				origin = pelvisNode->world.translate;
				result.pelvisOk = true;
			} else {
				result.hasFiniteTransform = false;
			}
		}

		FillVerticalDiagnostics(result, actor, rootObject, rootNode, rootOrigin);

		for (const auto& spec : kNodeSpecs) {
			auto* node = FindNode(rootNode, spec.name);
			if (!node) {
				++result.missingNodes;
				continue;
			}
			if (!IsFinitePoint(node->world.translate)) {
				result.hasFiniteTransform = false;
				++result.missingNodes;
				continue;
			}

			++result.sampledNodes;
			const float distance = Distance(node->world.translate, origin);
			if (distance > result.maxBoneDistance) {
				result.maxBoneDistance = distance;
				result.farNode = spec.key;
			}
			if (distance >= spec.suspectDistance) {
				result.stretchSuspect = true;
			}
			if (distance >= spec.severeDistance) {
				result.severeStretchSuspect = true;
			}
		}

		result.headDistance = ReadDistanceOrMinusOne(rootNode, origin, "NPC Head [Head]");
		result.leftHandDistance = ReadDistanceOrMinusOne(rootNode, origin, "NPC L Hand [LHnd]");
		result.rightHandDistance = ReadDistanceOrMinusOne(rootNode, origin, "NPC R Hand [RHnd]");
		result.leftFootDistance = ReadDistanceOrMinusOne(rootNode, origin, "NPC L Foot [Lft ]");
		result.rightFootDistance = ReadDistanceOrMinusOne(rootNode, origin, "NPC R Foot [Rft ]");

		if (!result.hasFiniteTransform) {
			result.stretchSuspect = true;
			result.severeStretchSuspect = true;
		}

		if (result.boundRadius >= 260.0f) {
			result.stretchSuspect = true;
		}
		if (result.boundRadius >= 520.0f) {
			result.severeStretchSuspect = true;
		}

		return result;
	}


	bool IsActorVisualReady(RE::Actor* actor, const char* reason, bool requireLoaded)
	{
		const char* why = reason && reason[0] ? reason : "visual_ready_gate";
		const Result result = ProbeActor(actor);
		const bool ready = result.actorValid &&
			(!requireLoaded || result.loaded) &&
			result.rootOk &&
			result.hasFiniteTransform &&
			!result.verticalSuspect &&
			!result.stretchSuspect &&
			!result.severeStretchSuspect;

		spdlog::info(
			"[TFD][VisualProbe][R370A] visual ready gate actor={:08X} reason={} ready={} requireLoaded={} valid={} loaded={} rootOk={} pelvisOk={} finite={} verticalSuspect={} upperBodyFloat={} blendContam={} feetNearActorZ={} footIKSnap={} stretchSuspect={} severe={} actorZ={:.1f} rootZ={:.1f} pelvisZ={:.1f} headZ={:.1f} minFootZ={:.1f} rootFootZ={:.1f} pelvisFootZ={:.1f} headFootZ={:.1f} rootActorZ={:.1f} pelvisActorZ={:.1f} headActorZ={:.1f} footActorZ={:.1f} boundRadius={:.1f} maxBoneDist={:.1f} farNode={} bleeding={} combat={} drawn={} ai={} hpPct={:.1f}",
			result.actorFormID,
			why,
			ready ? 1 : 0,
			requireLoaded ? 1 : 0,
			result.actorValid ? 1 : 0,
			result.loaded ? 1 : 0,
			result.rootOk ? 1 : 0,
			result.pelvisOk ? 1 : 0,
			result.hasFiniteTransform ? 1 : 0,
			result.verticalSuspect ? 1 : 0,
			result.upperBodyFloatSuspect ? 1 : 0,
			result.blendContaminationSuspect ? 1 : 0,
			result.feetNearActorZ ? 1 : 0,
			result.footIKSnapSuspect ? 1 : 0,
			result.stretchSuspect ? 1 : 0,
			result.severeStretchSuspect ? 1 : 0,
			result.actorZ,
			result.rootZ,
			result.pelvisZ,
			result.headZ,
			result.minFootZ,
			result.rootMinusFootZ,
			result.pelvisMinusFootZ,
			result.headMinusFootZ,
			result.rootMinusActorZ,
			result.pelvisMinusActorZ,
			result.headMinusActorZ,
			result.minFootMinusActorZ,
			result.boundRadius,
			result.maxBoneDistance,
			result.farNode ? result.farNode : "-",
			IsBleedingOut(actor) ? 1 : 0,
			(actor && actor->IsInCombat()) ? 1 : 0,
			(actor && actor->IsWeaponDrawn()) ? 1 : 0,
			(actor && actor->IsAIEnabled()) ? 1 : 0,
			GetHealthPct(actor));
		return ready;
	}

	bool IsActorVerticalSuspect(RE::Actor* actor, const char* reason)
	{
		const char* why = reason && reason[0] ? reason : "visual_vertical_suspect";
		const Result result = ProbeActor(actor);

		// R370A: do not require hasFiniteTransform here.
		// Log 39 proved the actor can be obviously invalid for OStim with:
		//   finite=0, verticalSuspect=1, stretchSuspect=1, severe=1
		// but the old query returned suspect=0 because finite was required.
		// For gate diagnostics, any loaded/rooted visual fault must count as suspect.
		const bool visualFault =
			result.verticalSuspect ||
			result.stretchSuspect ||
			result.severeStretchSuspect ||
			!result.hasFiniteTransform;
		const bool suspect = result.actorValid && result.loaded && result.rootOk && visualFault;

		spdlog::info(
			"[TFD][VisualProbe][R370A] vertical suspect query actor={:08X} reason={} suspect={} visualFault={} valid={} loaded={} rootOk={} finite={} verticalSuspect={} upperBodyFloat={} blendContam={} stretchSuspect={} severe={} footActorZ={:.1f} rootFootZ={:.1f} rootActorZ={:.1f} boundRadius={:.1f} bleeding={} combat={} drawn={} ai={} hpPct={:.1f}",
			result.actorFormID,
			why,
			suspect ? 1 : 0,
			visualFault ? 1 : 0,
			result.actorValid ? 1 : 0,
			result.loaded ? 1 : 0,
			result.rootOk ? 1 : 0,
			result.hasFiniteTransform ? 1 : 0,
			result.verticalSuspect ? 1 : 0,
			result.upperBodyFloatSuspect ? 1 : 0,
			result.blendContaminationSuspect ? 1 : 0,
			result.stretchSuspect ? 1 : 0,
			result.severeStretchSuspect ? 1 : 0,
			result.minFootMinusActorZ,
			result.rootMinusFootZ,
			result.rootMinusActorZ,
			result.boundRadius,
			IsBleedingOut(actor) ? 1 : 0,
			(actor && actor->IsInCombat()) ? 1 : 0,
			(actor && actor->IsWeaponDrawn()) ? 1 : 0,
			(actor && actor->IsAIEnabled()) ? 1 : 0,
			GetHealthPct(actor));
		return suspect;
	}


	float DeltaOrZero(float before, float after)
	{
		if (!std::isfinite(before) || !std::isfinite(after) || before <= kMissingZ + 1.0f || after <= kMissingZ + 1.0f) {
			return 0.0f;
		}
		return after - before;
	}

	void LogDelta(const char* reason, const char* step, int pass, bool actionOk, const Result& before, const Result& after)
	{
		const char* why = reason && reason[0] ? reason : "visual_probe_delta";
		const char* actionStep = step && step[0] ? step : "step";
		const bool verticalWentBad = !before.verticalSuspect && after.verticalSuspect;
		const bool verticalRecovered = before.verticalSuspect && !after.verticalSuspect;
		const bool blendWentBad = !before.blendContaminationSuspect && after.blendContaminationSuspect;
		const bool blendRecovered = before.blendContaminationSuspect && !after.blendContaminationSuspect;
		const bool upperFloatWentBad = !before.upperBodyFloatSuspect && after.upperBodyFloatSuspect;
		const bool upperFloatRecovered = before.upperBodyFloatSuspect && !after.upperBodyFloatSuspect;
		const bool footSnapChanged = before.footIKSnapSuspect != after.footIKSnapSuspect;

		spdlog::info(
			"[TFD][VisualProbeDelta][R365A] reason={} step={} pass={} actor={:08X} actionOk={} beforeVertical={} afterVertical={} verticalWentBad={} verticalRecovered={} beforeBlend={} afterBlend={} blendWentBad={} blendRecovered={} beforeUpperFloat={} afterUpperFloat={} upperFloatWentBad={} upperFloatRecovered={} beforeFootSnap={} afterFootSnap={} footSnapChanged={} rootFootBefore={:.1f} rootFootAfter={:.1f} rootFootDelta={:.1f} pelvisFootBefore={:.1f} pelvisFootAfter={:.1f} pelvisFootDelta={:.1f} headFootBefore={:.1f} headFootAfter={:.1f} headFootDelta={:.1f} footActorBefore={:.1f} footActorAfter={:.1f} footActorDelta={:.1f} rootActorBefore={:.1f} rootActorAfter={:.1f} rootActorDelta={:.1f} boundBefore={:.1f} boundAfter={:.1f} boundDelta={:.1f} maxBoneBefore={:.1f} maxBoneAfter={:.1f} maxBoneDelta={:.1f} loadedBefore={} loadedAfter={} finiteBefore={} finiteAfter={}",
			why,
			actionStep,
			pass,
			after.actorFormID ? after.actorFormID : before.actorFormID,
			actionOk ? 1 : 0,
			before.verticalSuspect ? 1 : 0,
			after.verticalSuspect ? 1 : 0,
			verticalWentBad ? 1 : 0,
			verticalRecovered ? 1 : 0,
			before.blendContaminationSuspect ? 1 : 0,
			after.blendContaminationSuspect ? 1 : 0,
			blendWentBad ? 1 : 0,
			blendRecovered ? 1 : 0,
			before.upperBodyFloatSuspect ? 1 : 0,
			after.upperBodyFloatSuspect ? 1 : 0,
			upperFloatWentBad ? 1 : 0,
			upperFloatRecovered ? 1 : 0,
			before.footIKSnapSuspect ? 1 : 0,
			after.footIKSnapSuspect ? 1 : 0,
			footSnapChanged ? 1 : 0,
			before.rootMinusFootZ,
			after.rootMinusFootZ,
			DeltaOrZero(before.rootMinusFootZ, after.rootMinusFootZ),
			before.pelvisMinusFootZ,
			after.pelvisMinusFootZ,
			DeltaOrZero(before.pelvisMinusFootZ, after.pelvisMinusFootZ),
			before.headMinusFootZ,
			after.headMinusFootZ,
			DeltaOrZero(before.headMinusFootZ, after.headMinusFootZ),
			before.minFootMinusActorZ,
			after.minFootMinusActorZ,
			DeltaOrZero(before.minFootMinusActorZ, after.minFootMinusActorZ),
			before.rootMinusActorZ,
			after.rootMinusActorZ,
			DeltaOrZero(before.rootMinusActorZ, after.rootMinusActorZ),
			before.boundRadius,
			after.boundRadius,
			DeltaOrZero(before.boundRadius, after.boundRadius),
			before.maxBoneDistance,
			after.maxBoneDistance,
			DeltaOrZero(before.maxBoneDistance, after.maxBoneDistance),
			before.loaded ? 1 : 0,
			after.loaded ? 1 : 0,
			before.hasFiniteTransform ? 1 : 0,
			after.hasFiniteTransform ? 1 : 0);
	}


	Result LogWatchSample(RE::Actor* actor, const char* reason, std::uint32_t watchId, int sample, int elapsedMs, const char* runtimePhaseName)
	{
		const char* why = reason && reason[0] ? reason : "visual_watch";
		const char* phaseName = runtimePhaseName && runtimePhaseName[0] ? runtimePhaseName : "unknown";
		Result result = ProbeActor(actor);

		spdlog::info(
			"[TFD][VisualWatch][R366A] actor={:08X} watch={} sample={} elapsedMs={} reason={} phase={} valid={} loaded={} rootOk={} pelvisOk={} finite={} verticalData={} footData={} headData={} actorZ={:.1f} rootZ={:.1f} pelvisZ={:.1f} headZ={:.1f} lFootZ={:.1f} rFootZ={:.1f} minFootZ={:.1f} maxFootZ={:.1f} footSpanZ={:.1f} rootFootZ={:.1f} pelvisFootZ={:.1f} headFootZ={:.1f} rootActorZ={:.1f} pelvisActorZ={:.1f} headActorZ={:.1f} footActorZ={:.1f} feetNearActorZ={} footIKSnap={} upperBodyFloat={} blendContam={} verticalSuspect={} stretchSuspect={} severe={} boundRadius={:.1f} maxBoneDist={:.1f} farNode={} bleeding={} combat={} drawn={} ai={} hpPct={:.1f}",
			result.actorFormID,
			watchId,
			sample,
			elapsedMs,
			why,
			phaseName,
			result.actorValid ? 1 : 0,
			result.loaded ? 1 : 0,
			result.rootOk ? 1 : 0,
			result.pelvisOk ? 1 : 0,
			result.hasFiniteTransform ? 1 : 0,
			result.verticalData ? 1 : 0,
			result.footData ? 1 : 0,
			result.headData ? 1 : 0,
			result.actorZ,
			result.rootZ,
			result.pelvisZ,
			result.headZ,
			result.leftFootZ,
			result.rightFootZ,
			result.minFootZ,
			result.maxFootZ,
			result.footSpanZ,
			result.rootMinusFootZ,
			result.pelvisMinusFootZ,
			result.headMinusFootZ,
			result.rootMinusActorZ,
			result.pelvisMinusActorZ,
			result.headMinusActorZ,
			result.minFootMinusActorZ,
			result.feetNearActorZ ? 1 : 0,
			result.footIKSnapSuspect ? 1 : 0,
			result.upperBodyFloatSuspect ? 1 : 0,
			result.blendContaminationSuspect ? 1 : 0,
			result.verticalSuspect ? 1 : 0,
			result.stretchSuspect ? 1 : 0,
			result.severeStretchSuspect ? 1 : 0,
			result.boundRadius,
			result.maxBoneDistance,
			result.farNode ? result.farNode : "-",
			IsBleedingOut(actor) ? 1 : 0,
			(actor && actor->IsInCombat()) ? 1 : 0,
			(actor && actor->IsWeaponDrawn()) ? 1 : 0,
			(actor && actor->IsAIEnabled()) ? 1 : 0,
			GetHealthPct(actor));

		UpdateTrackedSnapshot(actor, result, why, sample, elapsedMs);
		return result;
	}

	Result LogActor(RE::Actor* actor, const char* reason, int pass)
	{
		const char* why = reason && reason[0] ? reason : "visual_probe";
		Result result = ProbeActor(actor);

		spdlog::info(
			"[TFD][VisualProbe][R365A] actor={:08X} reason={} pass={} valid={} loaded={} rootOk={} pelvisOk={} finite={} sampled={} missing={} boundRadius={:.1f} maxBoneDist={:.1f} farNode={} head={:.1f} lHand={:.1f} rHand={:.1f} lFoot={:.1f} rFoot={:.1f} stretchSuspect={} severe={} verticalData={} footData={} headData={} actorZ={:.1f} rootZ={:.1f} pelvisZ={:.1f} headZ={:.1f} lFootZ={:.1f} rFootZ={:.1f} minFootZ={:.1f} maxFootZ={:.1f} footSpanZ={:.1f} rootFootZ={:.1f} pelvisFootZ={:.1f} headFootZ={:.1f} rootActorZ={:.1f} pelvisActorZ={:.1f} headActorZ={:.1f} footActorZ={:.1f} feetNearActorZ={} footIKSnap={} upperBodyFloat={} blendContam={} verticalSuspect={} bleeding={} combat={} drawn={} ai={} hpPct={:.1f}",
			result.actorFormID,
			why,
			pass,
			result.actorValid ? 1 : 0,
			result.loaded ? 1 : 0,
			result.rootOk ? 1 : 0,
			result.pelvisOk ? 1 : 0,
			result.hasFiniteTransform ? 1 : 0,
			result.sampledNodes,
			result.missingNodes,
			result.boundRadius,
			result.maxBoneDistance,
			result.farNode ? result.farNode : "-",
			result.headDistance,
			result.leftHandDistance,
			result.rightHandDistance,
			result.leftFootDistance,
			result.rightFootDistance,
			result.stretchSuspect ? 1 : 0,
			result.severeStretchSuspect ? 1 : 0,
			result.verticalData ? 1 : 0,
			result.footData ? 1 : 0,
			result.headData ? 1 : 0,
			result.actorZ,
			result.rootZ,
			result.pelvisZ,
			result.headZ,
			result.leftFootZ,
			result.rightFootZ,
			result.minFootZ,
			result.maxFootZ,
			result.footSpanZ,
			result.rootMinusFootZ,
			result.pelvisMinusFootZ,
			result.headMinusFootZ,
			result.rootMinusActorZ,
			result.pelvisMinusActorZ,
			result.headMinusActorZ,
			result.minFootMinusActorZ,
			result.feetNearActorZ ? 1 : 0,
			result.footIKSnapSuspect ? 1 : 0,
			result.upperBodyFloatSuspect ? 1 : 0,
			result.blendContaminationSuspect ? 1 : 0,
			result.verticalSuspect ? 1 : 0,
			IsBleedingOut(actor) ? 1 : 0,
			(actor && actor->IsInCombat()) ? 1 : 0,
			(actor && actor->IsWeaponDrawn()) ? 1 : 0,
			(actor && actor->IsAIEnabled()) ? 1 : 0,
			GetHealthPct(actor));

		UpdateTrackedSnapshot(actor, result, why, pass, -1);
		return result;
	}



	bool RunActorImpactRecoveryPulse(RE::Actor* actor, const char* reason)
	{
		// R372A: recovery pulses are intentionally disabled.
		// R370A proved fake recoil/stagger events and PushActorAway can create false recovery
		// or visible physics shove without durably clearing the bleedout/getup blend.
		// Keep the native registered for save/script compatibility, but make it diagnostic-only.
		const char* why = reason && reason[0] ? reason : "impact_recovery_disabled";
		const Result before = ProbeActor(actor);
		const bool fault = HasVisualFault(before);
		spdlog::info(
			"[TFD][VisualProbe][R372A] impact recovery disabled actor={:08X} reason={} wouldRecover=0 fault={} verticalSuspect={} stretchSuspect={} severe={} finite={} footActorZ={:.1f} rootFootZ={:.1f} rootActorZ={:.1f} bleeding={} combat={} drawn={} ai={}",
			before.actorFormID,
			why,
			fault ? 1 : 0,
			before.verticalSuspect ? 1 : 0,
			before.stretchSuspect ? 1 : 0,
			before.severeStretchSuspect ? 1 : 0,
			before.hasFiniteTransform ? 1 : 0,
			before.minFootMinusActorZ,
			before.rootMinusFootZ,
			before.rootMinusActorZ,
			IsBleedingOut(actor) ? 1 : 0,
			(actor && actor->IsInCombat()) ? 1 : 0,
			(actor && actor->IsWeaponDrawn()) ? 1 : 0,
			(actor && actor->IsAIEnabled()) ? 1 : 0);
		return false;
	}


	class HitTraceSink : public RE::BSTEventSink<RE::TESHitEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* event, RE::BSTEventSource<RE::TESHitEvent>*) override
		{
			if (!event || !event->target) {
				return RE::BSEventNotifyControl::kContinue;
			}

			auto* targetActor = event->target->As<RE::Actor>();
			if (!targetActor) {
				return RE::BSEventNotifyControl::kContinue;
			}

			const auto targetFormID = targetActor->GetFormID();
			TrackedActorState beforeState{};
			bool tracked = false;
			{
				const auto now = std::chrono::steady_clock::now();
				std::scoped_lock lock(g_traceLock);
				PurgeExpiredTraceLocked(now);
				auto it = g_trackedActors.find(targetFormID);
				if (it != g_trackedActors.end()) {
					beforeState = it->second;
					++it->second.hitCount;
					tracked = true;
				}
			}

			if (!tracked) {
				return RE::BSEventNotifyControl::kContinue;
			}

			const Result after = ProbeActor(targetActor);
			const bool beforeFault = beforeState.hasLastProbe && HasVisualFault(beforeState.lastProbe);
			const bool afterFault = HasVisualFault(after);
			const bool recoveredAfterHit = beforeState.hasLastProbe && beforeFault && !afterFault;

			auto* causeRef = event->cause.get();
			RE::Actor* causeActor = causeRef ? causeRef->As<RE::Actor>() : nullptr;
			const auto causeFormID = causeRef ? causeRef->GetFormID() : 0u;
			auto* player = RE::PlayerCharacter::GetSingleton();
			const bool causeIsPlayer = player && causeRef == player;
			const bool powerAttack = event->flags.any(RE::TESHitEvent::Flag::kPowerAttack);

			spdlog::info(
				"[TFD][VisualTrace][R371A] hit event target={:08X} cause={:08X} causeIsPlayer={} causeIsActor={} trace={} trackedReason={} hitCount={} powerAttack={} beforeFault={} afterFault={} recoveredAfterHit={} beforeVertical={} afterVertical={} beforeFootActorZ={:.1f} afterFootActorZ={:.1f} beforeRootFootZ={:.1f} afterRootFootZ={:.1f} beforeFinite={} afterFinite={} sampleBefore={} elapsedBeforeMs={}",
				targetFormID,
				causeFormID,
				causeIsPlayer ? 1 : 0,
				causeActor ? 1 : 0,
				beforeState.traceId,
				beforeState.reason,
				beforeState.hitCount + 1,
				powerAttack ? 1 : 0,
				beforeFault ? 1 : 0,
				afterFault ? 1 : 0,
				recoveredAfterHit ? 1 : 0,
				beforeState.lastProbe.verticalSuspect ? 1 : 0,
				after.verticalSuspect ? 1 : 0,
				beforeState.lastProbe.minFootMinusActorZ,
				after.minFootMinusActorZ,
				beforeState.lastProbe.rootMinusFootZ,
				after.rootMinusFootZ,
				beforeState.lastProbe.hasFiniteTransform ? 1 : 0,
				after.hasFiniteTransform ? 1 : 0,
				beforeState.lastSample,
				beforeState.lastElapsedMs);

			{
				std::scoped_lock lock(g_traceLock);
				auto it = g_trackedActors.find(targetFormID);
				if (it != g_trackedActors.end()) {
					it->second.lastProbe = after;
					it->second.hasLastProbe = true;
					it->second.everFault = it->second.everFault || afterFault;
				}
			}

			return RE::BSEventNotifyControl::kContinue;
		}
	};

	HitTraceSink g_hitTraceSink;

	std::uint32_t TrackActorForHitTrace(RE::Actor* actor, const char* reason, int durationMs)
	{
		if (!actor || actor->IsDisabled() || actor->IsDead()) {
			spdlog::warn(
				"[TFD][VisualTrace][R371A] track skipped actor={:08X} reason={} invalid=1 dead={} disabled={}",
				actor ? actor->GetFormID() : 0u,
				reason && reason[0] ? reason : "track",
				(actor && actor->IsDead()) ? 1 : 0,
				(actor && actor->IsDisabled()) ? 1 : 0);
			return 0;
		}

		const int safeDurationMs = std::clamp(durationMs > 0 ? durationMs : 12000, 1000, 30000);
		const std::uint32_t traceId = ++g_traceSerial;
		const Result initial = ProbeActor(actor);

		TrackedActorState state{};
		state.actorFormID = actor->GetFormID();
		state.traceId = traceId;
		state.expiresAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(safeDurationMs);
		state.reason = reason && reason[0] ? reason : "victory_pleasure_hit_trace";
		state.lastProbe = initial;
		state.hasLastProbe = true;
		state.everFault = HasVisualFault(initial);
		state.lastSample = 0;
		state.lastElapsedMs = 0;

		{
			std::scoped_lock lock(g_traceLock);
			PurgeExpiredTraceLocked(std::chrono::steady_clock::now());
			g_trackedActors[state.actorFormID] = state;
		}

		spdlog::info(
			"[TFD][VisualTrace][R371A] track actor={:08X} trace={} reason={} durationMs={} initialFault={} verticalSuspect={} stretchSuspect={} severe={} finite={} footActorZ={:.1f} rootFootZ={:.1f} hpPct={:.1f}",
			state.actorFormID,
			traceId,
			state.reason,
			safeDurationMs,
			HasVisualFault(initial) ? 1 : 0,
			initial.verticalSuspect ? 1 : 0,
			initial.stretchSuspect ? 1 : 0,
			initial.severeStretchSuspect ? 1 : 0,
			initial.hasFiniteTransform ? 1 : 0,
			initial.minFootMinusActorZ,
			initial.rootMinusFootZ,
			GetHealthPct(actor));
		return traceId;
	}

	void InstallHitTrace()
	{
		if (g_hitTraceInstalled.exchange(true)) {
			return;
		}

		auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
		if (!holder) {
			g_hitTraceInstalled.store(false);
			spdlog::warn("[TFD][VisualTrace][R371A] hit trace install failed sourceHolder=null");
			return;
		}

		holder->AddEventSink<RE::TESHitEvent>(&g_hitTraceSink);
		spdlog::info("[TFD][VisualTrace][R371A] hit trace sink installed");
	}

	namespace
	{
		bool PapyrusIsActorVisualReady(RE::StaticFunctionTag*, RE::Actor* actor, RE::BSFixedString reason)
		{
			return IsActorVisualReady(actor, reason.c_str(), true);
		}

		bool PapyrusIsActorVerticalSuspect(RE::StaticFunctionTag*, RE::Actor* actor, RE::BSFixedString reason)
		{
			return IsActorVerticalSuspect(actor, reason.c_str());
		}

		bool PapyrusRunActorImpactRecoveryPulse(RE::StaticFunctionTag*, RE::Actor* actor, RE::BSFixedString reason)
		{
			return RunActorImpactRecoveryPulse(actor, reason.c_str());
		}
	}

	bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			return false;
		}

		a_vm->RegisterFunction("IsActorVisualReady", "TFDVisualProbeNative", PapyrusIsActorVisualReady);
		a_vm->RegisterFunction("IsActorVerticalSuspect", "TFDVisualProbeNative", PapyrusIsActorVerticalSuspect);
		a_vm->RegisterFunction("RunActorImpactRecoveryPulse", "TFDVisualProbeNative", PapyrusRunActorImpactRecoveryPulse);
		spdlog::info("[TFD][VisualProbe][R372A] Papyrus natives registered for visual ready gate + disabled impact recovery + hit trace");
		return true;
	}
}
