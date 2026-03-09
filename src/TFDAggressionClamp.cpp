#include "TFDAggressionClamp.h"

#include <spdlog/spdlog.h>

#include <mutex>
#include <unordered_map>

namespace TFD::AggressionClamp
{
    namespace
    {
        std::mutex gLock;
        std::unordered_map<std::uint32_t, float> gSaved; // handle -> original aggression
    }

    void Apply(RE::Actor* a)
    {
        if (!a) {
            return;
        }

        auto* avo = a->AsActorValueOwner();
        if (!avo) {
            return;
        }

        // Aggression AV: 0..3 (umumnya bandit = 1)
        const float cur = avo->GetActorValue(RE::ActorValue::kAggression);
        if (cur <= 0.0f) {
            return;
        }

        const std::uint32_t h = a->GetHandle().native_handle();

        {
            std::scoped_lock lk(gLock);
            if (gSaved.find(h) != gSaved.end()) {
                return; // already clamped
            }
            gSaved.emplace(h, cur);
        }

        // Make it 0 temporarily (reversible) -> stop desire to re-enter combat
        avo->ModActorValue(RE::ActorValue::kAggression, -cur);
        spdlog::info("[TFD][AggClamp] applied actor={:08X} handle={} origAgg={}", a->GetFormID(), h, cur);
    }

    void Clear()
    {
        std::unordered_map<std::uint32_t, float> snap;
        {
            std::scoped_lock lk(gLock);
            snap.swap(gSaved);
        }

        for (auto& it : snap) {
            const std::uint32_t h = it.first;
            const float orig = it.second;

            auto sp = RE::Actor::LookupByHandle(h);
            auto* a = sp.get();
            if (!a) {
                continue;
            }

            auto* avo = a->AsActorValueOwner();
            if (!avo) {
                continue;
            }

            // Restore original aggression
            avo->ModActorValue(RE::ActorValue::kAggression, orig);

            a->EvaluatePackage(true, false);

            spdlog::info("[TFD][AggClamp] restored actor={:08X} handle={} addBack={}", a->GetFormID(), h, orig);
        }
    }
}