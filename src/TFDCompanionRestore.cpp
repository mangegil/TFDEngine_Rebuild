#include "TFDCompanionRestore.h"

#include "TFDPacify.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <thread>

namespace TFD::CompanionRestore
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        struct QuestCache
        {
            bool resolved{ false };
            RE::TESQuest* quest{ nullptr };
            std::array<RE::BGSRefAlias*, 6> teammateAliases{};
        };

        enum class RestoreResult
        {
            kRestored,
            kRetryLater,
            kFailed
        };

        struct RestorePassStats
        {
            std::size_t total{ 0 };
            std::size_t restored{ 0 };
            std::size_t retryLater{ 0 };
            std::size_t failed{ 0 };
        };

        QuestCache g_cache{};
        std::mutex g_lock{};
        std::atomic_bool g_restoreQueued{ false };
        std::atomic_uint64_t g_restoreGeneration{ 0 };

        constexpr double kRestoreCompanionHours = 3.0;
        constexpr std::size_t kRestoreRetryCount = 5;
        constexpr auto kRestoreRetryDelay = std::chrono::milliseconds(1500);
        constexpr auto kRestoreTaskTimeout = std::chrono::seconds(10);
        constexpr const char* kCreatureTeammateAssignEvent = "TFDCreatureTeammateAssign";
        constexpr const char* kTameUnassignEvent = "TFDTameUnassign";

        void SendUnassignTameEvent(RE::Actor* actor)
        {
            if (!actor) {
                return;
            }

            auto* src = SKSE::GetModCallbackEventSource();
            if (!src) {
                return;
            }

            SKSE::ModCallbackEvent e(kTameUnassignEvent, "", 0.0f, actor);
            src->SendEvent(&e);
        }

        void SendAssignEvent(RE::Actor* actor)
        {
            if (!actor) {
                return;
            }

            auto* src = SKSE::GetModCallbackEventSource();
            if (!src) {
                return;
            }

            SKSE::ModCallbackEvent e(kCreatureTeammateAssignEvent, "", 0.0f, actor);
            src->SendEvent(&e);
        }

        double NowSec()
        {
            static const auto t0 = Clock::now();
            return std::chrono::duration<double>(Clock::now() - t0).count();
        }

        RE::PlayerCharacter* Player()
        {
            return RE::PlayerCharacter::GetSingleton();
        }

        void ResolveQuest()
        {
            if (g_cache.resolved) {
                return;
            }

            g_cache.resolved = true;
            g_cache.quest = RE::TESForm::LookupByEditorID<RE::TESQuest>("TFDCreatureTeammateQuest");
            if (!g_cache.quest) {
                spdlog::warn("[TFD][CompanionRestore] quest TFDCreatureTeammateQuest not found");
                return;
            }

            for (auto* baseAlias : g_cache.quest->aliases) {
                auto* refAlias = skyrim_cast<RE::BGSRefAlias*>(baseAlias);
                if (!refAlias) {
                    continue;
                }

                const std::string aliasName = refAlias->aliasName.c_str();
                if (aliasName.rfind("Teammate", 0) != 0 || aliasName.size() < 10) {
                    continue;
                }

                try {
                    const int slot = std::stoi(aliasName.substr(9));
                    if (slot >= 1 && slot <= static_cast<int>(g_cache.teammateAliases.size())) {
                        g_cache.teammateAliases[static_cast<std::array<RE::BGSRefAlias*, 6Ui64>::size_type>(slot) - 1] = refAlias;
                    }
                }
                catch (...) {
                }
            }

            std::size_t found = 0;
            for (auto* alias : g_cache.teammateAliases) {
                if (alias) {
                    ++found;
                }
            }

            spdlog::info("[TFD][CompanionRestore] quest resolved quest={:08X} aliases={}",
                g_cache.quest ? g_cache.quest->GetFormID() : 0u, found);
        }

        bool IsAliasRestoreCandidate(RE::Actor* actor)
        {
            return actor &&
                actor != Player() &&
                !actor->IsDead() &&
                !actor->IsDisabled();
        }

        RestoreResult RestoreActor(RE::Actor* actor)
        {
            if (!IsAliasRestoreCandidate(actor)) {
                return RestoreResult::kFailed;
            }

            if (TFD::Pacify::IsCompanion(actor)) {
                SendAssignEvent(actor);
                if (actor->Is3DLoaded()) {
                    actor->EvaluatePackage();
                }

                spdlog::info("[TFD][CompanionRestore] actor already companion actor={:08X} reprime=1", actor->GetFormID());
                return RestoreResult::kRestored;
            }

            if (!actor->Is3DLoaded()) {
                spdlog::info("[TFD][CompanionRestore] actor not ready yet actor={:08X} loaded=0", actor->GetFormID());
                return RestoreResult::kRetryLater;
            }

            if (TFD::Pacify::HasActiveTameSession(actor)) {
                const bool promoted = TFD::Pacify::PromoteActiveTameToCompanion(actor, kRestoreCompanionHours);
                if (!promoted) {
                    spdlog::warn("[TFD][CompanionRestore] promote existing failed actor={:08X}", actor->GetFormID());
                    return RestoreResult::kRetryLater;
                }

                SendUnassignTameEvent(actor);
                SendAssignEvent(actor);
                actor->EvaluatePackage();

                spdlog::info("[TFD][CompanionRestore] promote existing actor={:08X} ok=1", actor->GetFormID());
                return RestoreResult::kRestored;
            }

            auto* player = Player();
            if (!player) {
                return RestoreResult::kRetryLater;
            }

            const auto started = TFD::Pacify::BeginTameSession(player, actor, NowSec(), false, false);
            if (!started.has_value()) {
                spdlog::warn("[TFD][CompanionRestore] begin tame failed actor={:08X}", actor->GetFormID());
                return RestoreResult::kRetryLater;
            }

            const bool promoted = TFD::Pacify::PromoteActiveTameToCompanion(actor, kRestoreCompanionHours);
            if (!promoted) {
                TFD::Pacify::ReleaseActiveTameActor(actor, TFD::Pacify::ReleaseReason::Generic);
                spdlog::warn("[TFD][CompanionRestore] promote failed after restore begin actor={:08X}", actor->GetFormID());
                return RestoreResult::kRetryLater;
            }

            SendUnassignTameEvent(actor);
            SendAssignEvent(actor);
            actor->EvaluatePackage();

            spdlog::info("[TFD][CompanionRestore] restored actor={:08X} name='{}' hours={:.2f} reprime=1",
                actor->GetFormID(), actor->GetName() ? actor->GetName() : "", kRestoreCompanionHours);
            return RestoreResult::kRestored;
        }

        RestorePassStats RestorePass()
        {
            std::scoped_lock lock(g_lock);
            ResolveQuest();

            RestorePassStats stats{};
            if (!g_cache.quest) {
                return stats;
            }

            for (auto* alias : g_cache.teammateAliases) {
                if (!alias) {
                    continue;
                }

                auto* actor = alias->GetActorReference();
                if (!actor) {
                    continue;
                }

                ++stats.total;

                switch (RestoreActor(actor)) {
                case RestoreResult::kRestored:
                    ++stats.restored;
                    break;
                case RestoreResult::kRetryLater:
                    ++stats.retryLater;
                    break;
                case RestoreResult::kFailed:
                default:
                    ++stats.failed;
                    break;
                }
            }

            return stats;
        }

        void QueueRestoreAfterLoad()
        {
            const std::uint64_t generation = g_restoreGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
            g_restoreQueued.store(true, std::memory_order_release);

            std::thread([generation]() {
                for (std::size_t attempt = 1; attempt <= kRestoreRetryCount; ++attempt) {
                    std::this_thread::sleep_for(kRestoreRetryDelay);

                    if (g_restoreGeneration.load(std::memory_order_acquire) != generation) {
                        return;
                    }

                    auto* task = SKSE::GetTaskInterface();
                    if (!task) {
                        continue;
                    }

                    auto promise = std::make_shared<std::promise<RestorePassStats>>();
                    auto future = promise->get_future();

                    task->AddTask([promise]() {
                        try {
                            promise->set_value(RestorePass());
                        }
                        catch (...) {
                            try {
                                promise->set_exception(std::current_exception());
                            }
                            catch (...) {
                            }
                        }
                    });

                    if (future.wait_for(kRestoreTaskTimeout) != std::future_status::ready) {
                        spdlog::warn("[TFD][CompanionRestore] restore pass timeout attempt={}", attempt);
                        continue;
                    }

                    RestorePassStats stats{};
                    try {
                        stats = future.get();
                    }
                    catch (...) {
                        spdlog::warn("[TFD][CompanionRestore] restore pass exception attempt={}", attempt);
                        continue;
                    }

                    spdlog::info("[TFD][CompanionRestore] restore pass attempt={} total={} restored={} retryLater={} failed={}",
                        attempt, stats.total, stats.restored, stats.retryLater, stats.failed);

                    if (stats.retryLater == 0) {
                        break;
                    }
                }

                if (g_restoreGeneration.load(std::memory_order_acquire) == generation) {
                    g_restoreQueued.store(false, std::memory_order_release);
                }
            }).detach();
        }
    }

    std::size_t RestoreNow()
    {
        return RestorePass().restored;
    }

    void OnSkseMessage(SKSE::MessagingInterface::Message* m)
    {
        if (!m) {
            return;
        }

        if (m->type == SKSE::MessagingInterface::kPreLoadGame) {
            g_restoreGeneration.fetch_add(1, std::memory_order_acq_rel);
            g_restoreQueued.store(false, std::memory_order_release);
            return;
        }

        if (m->type == SKSE::MessagingInterface::kPostLoadGame) {
            QueueRestoreAfterLoad();
        }
    }
}
