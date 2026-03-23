#include <type_traits>
#include <RE/A/ActorValues.h>
#include <RE/Skyrim.h>
#include "SKSE/SKSE.h"
#include "SKSE/Interfaces.h"
#include "SKSE/Version.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "TFDDefeatMonitor.h"
#include "TFDLocation.h"
#include "TFDFactionMask.h"
#include "TFDPreCombatGreet.h"
#include "TFDPacify.h"
#include "TFDPacifyHooks.h"
#include "TFDTeammateAliasSync.h"

#if !defined(TFDEnableSmf)
#define TFDEnableSmf 1
#endif

#if TFDEnableSmf
namespace TFDMenu
{
    void Init();
}
#endif

static void SetupLog()
{
    auto dir = SKSE::log::log_directory();
    if (!dir) {
        return;
    }

    auto path = *dir;
    path /= "TFDEngine.log";

    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
    auto logger = std::make_shared<spdlog::logger>("TFDEngine", std::move(sink));
    spdlog::set_default_logger(std::move(logger));

    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] [%l] %v");
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);
}

static std::string BuildStamp()
{
    std::string s = "TFDEngine build ";
    s += __DATE__;
    s += " ";
    s += __TIME__;
    return s;
}

static void QueueHud(const char* text)
{
    if (!text) {
        return;
    }

    if (auto* tasks = SKSE::GetTaskInterface()) {
        tasks->AddUITask([t = std::string(text)]() {
            RE::DebugNotification(t.c_str());
            });
    }
}

static void ResetTransientStateForLoad()
{
    TFD::PreCombatGreet::CancelAll();
    TFD::Pacify::Reset();
    TFD::DefeatMonitor::ResetForLoad();
    TFD::DefeatMonitor::ResetGrace();

    spdlog::info("[TFD] ResetTransientStateForLoad complete (runtime only, pacify cleared)");
}

static constexpr std::uint32_t kSerializationID = 'TFDE';
static constexpr std::uint32_t kProgressRecord = 'TDSP';
static constexpr std::uint32_t kProgressVersion = 1;
static constexpr std::uint32_t kLocationCacheRecord = 'TDLC';
static constexpr std::uint32_t kLocationCacheVersion = 1;

struct SavedProgressRecord
{
    std::uint32_t captiveState{ 0 };
    std::uint32_t captivePhase{ 0 };
};

static std::atomic_bool gInitDone{ false };
static std::atomic_bool gSinkRegistered{ false };
static std::atomic_bool gPendingLoadFinalize{ false };

static void InitOnceAfterLoad()
{
    if (gInitDone.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    spdlog::info("[TFD] InitOnceAfterLoad");

    TFD::Location::Initialize();
    TFD::FactionMask::Initialize();
    TFD::DefeatMonitor::Install();
    TFD::DefeatMonitor::ResetGrace();
    TFD::PreCombatGreet::Install();
    TFD::TeammateAliasSync::Install();

    QueueHud("TFDEngine: Init OK (after load)");
    QueueHud(BuildStamp().c_str());
    QueueHud("TFDEngine: DefeatMonitor running");
}

static void FinalizeLoadAfterWorldReady()
{
    if (!gPendingLoadFinalize.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    spdlog::info("[TFD] FinalizeLoadAfterWorldReady -> begin");

    if (!TFD::DefeatMonitor::HasQueuedProgressState()) {
        TFD::DefeatMonitor::QueueDefaultProgressState();
    }

    ResetTransientStateForLoad();
    TFD::DefeatMonitor::SetLoadTransition(false);
    TFD::DefeatMonitor::ApplyQueuedProgressState();

    spdlog::info("[TFD] FinalizeLoadAfterWorldReady -> done");
}

static void OnSerializationSave(SKSE::SerializationInterface* intfc)
{
    if (!intfc) {
        return;
    }

    SavedProgressRecord rec{};
    rec.captiveState = TFD::DefeatMonitor::GetCaptiveStateForSave() ? 1u : 0u;
    rec.captivePhase = TFD::DefeatMonitor::GetCaptivePhaseForSave();

    if (!intfc->OpenRecord(kProgressRecord, kProgressVersion)) {
        spdlog::error("[TFD] Serialization Save -> OpenRecord failed");
        return;
    }

    if (!intfc->WriteRecordData(&rec, sizeof(rec))) {
        spdlog::error("[TFD] Serialization Save -> WriteRecordData failed");
        return;
    }

    spdlog::info("[TFD] Serialization Save -> state={} phase={}", rec.captiveState, rec.captivePhase);

    if (!intfc->OpenRecord(kLocationCacheRecord, kLocationCacheVersion)) {
        spdlog::error("[TFD] Serialization Save -> OpenRecord location cache failed");
        return;
    }

    if (!TFD::Location::SaveRescueCache(intfc)) {
        spdlog::error("[TFD] Serialization Save -> SaveRescueCache failed");
        return;
    }
}

static void OnSerializationRevert(SKSE::SerializationInterface*)
{
    spdlog::info("[TFD] Serialization Revert -> prepare save swap");
    TFD::DefeatMonitor::QueueDefaultProgressState();
    TFD::Location::ClearRescueCache();
    ResetTransientStateForLoad();
    TFD::DefeatMonitor::SetLoadTransition(true);
    gPendingLoadFinalize.store(true, std::memory_order_release);
}

static void OnSerializationLoad(SKSE::SerializationInterface* intfc)
{
    spdlog::info("[TFD] Serialization Load -> read cosave progress state");

    TFD::DefeatMonitor::QueueDefaultProgressState();
    TFD::Location::ClearRescueCache();
    gPendingLoadFinalize.store(true, std::memory_order_release);

    if (!intfc) {
        spdlog::warn("[TFD] Serialization Load -> interface null, using default state");
        return;
    }

    std::uint32_t type = 0;
    std::uint32_t version = 0;
    std::uint32_t length = 0;
    bool sawProgress = false;
    bool sawLocationCache = false;

    while (intfc->GetNextRecordInfo(type, version, length)) {
        if (type == kProgressRecord) {
            SavedProgressRecord rec{};
            const auto toRead = (std::min)(static_cast<std::uint32_t>(sizeof(rec)), length);
            if (toRead > 0 && !intfc->ReadRecordData(&rec, toRead)) {
                spdlog::error("[TFD] Serialization Load -> ReadRecordData failed");
                break;
            }

            if (length > toRead) {
                std::string skip(length - toRead, '\0');
                intfc->ReadRecordData(skip.data(), static_cast<std::uint32_t>(skip.size()));
            }

            TFD::DefeatMonitor::QueueLoadedProgressState(rec.captiveState >= 1u, rec.captivePhase);
            spdlog::info("[TFD] Serialization Load -> state={} phase={} version={}", rec.captiveState, rec.captivePhase, version);
            sawProgress = true;
            continue;
        }

        if (type == kLocationCacheRecord) {
            if (TFD::Location::LoadRescueCache(intfc, version, length)) {
                sawLocationCache = true;
            }
            continue;
        }

        std::string skip(length, '\0');
        if (length > 0) {
            intfc->ReadRecordData(skip.data(), length);
        }
    }

    spdlog::info("[TFD] Serialization Load -> progressRecord={} locationCache={}",
        sawProgress ? "yes" : "no",
        sawLocationCache ? "yes" : "no");
}

class LoadMenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
public:
    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* e, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
    {
        if (!e) {
            return RE::BSEventNotifyControl::kContinue;
        }

        if (e->menuName == RE::LoadingMenu::MENU_NAME && !e->opening) {
            auto* ui = RE::UI::GetSingleton();
            if (ui && ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) {
                spdlog::info("[TFD] LoadingMenu closed but MainMenu still open -> ignore");
                return RE::BSEventNotifyControl::kContinue;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* cell = player ? player->GetParentCell() : nullptr;
            if (!cell) {
                spdlog::info("[TFD] LoadingMenu closed but player cell null -> ignore");
                return RE::BSEventNotifyControl::kContinue;
            }

            spdlog::info("[TFD] LoadingMenu closed -> player in world (cell={:08X})", cell->GetFormID());
            InitOnceAfterLoad();
            if (cell->IsInteriorCell()) {
                TFD::Location::RefreshPlayerInteriorSafeCheckpoint();
            }
            FinalizeLoadAfterWorldReady();
        }

        return RE::BSEventNotifyControl::kContinue;
    }
};

static LoadMenuSink gLoadSink;

#if (defined(ENABLE_SKYRIM_SE) && ENABLE_SKYRIM_SE) && (defined(ENABLE_SKYRIM_AE) && ENABLE_SKYRIM_AE)
static_assert(false, "Build SE dan AE harus dipisah. Set salah satu: ENABLE_SKYRIM_SE atau ENABLE_SKYRIM_AE.");
#endif

extern "C" __declspec(dllexport) constinit SKSE::PluginVersionData SKSEPlugin_Version = []() {
    SKSE::PluginVersionData v;

    v.PluginName("TFDEngine");
    v.PluginVersion(REL::Version(0, 1, 0, 0));
    v.AuthorName("Bang Egil");
    v.UsesAddressLibrary(true);

#if defined(ENABLE_SKYRIM_AE) && ENABLE_SKYRIM_AE
    v.UsesStructsPost629(true);
    v.CompatibleVersions({
        REL::Version(1, 6, 629, 0),
        REL::Version(1, 6, 640, 0),
        REL::Version(1, 6, 1130, 0),
        REL::Version(1, 6, 1170, 0)
        });
#else
    v.CompatibleVersions({ SKSE::RUNTIME_SSE_1_5_97 });
#endif

    return v;
    }();

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface* skse, SKSE::PluginInfo* info)
{
    info->infoVersion = SKSE::PluginInfo::kVersion;
    info->name = SKSEPlugin_Version.pluginName;
    info->version = 1;

    if (skse->IsEditor()) {
        return false;
    }
    return true;
}

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* skse)
{
    SetupLog();
    SKSE::Init(skse);

    spdlog::info("[TFD] SKSEPlugin_Load");
    spdlog::info("[TFD] {}", BuildStamp());

    TFD::PacifyHooks::Install();

#if TFDEnableSmf
    TFDMenu::Init();
#endif

    if (auto* ser = SKSE::GetSerializationInterface()) {
        ser->SetUniqueID(kSerializationID);
        ser->SetSaveCallback(OnSerializationSave);
        ser->SetRevertCallback(OnSerializationRevert);
        ser->SetLoadCallback(OnSerializationLoad);
        spdlog::info("[TFD] Serialization callbacks registered (id={:08X})", kSerializationID);
    }
    else {
        spdlog::warn("[TFD] SerializationInterface missing");
    }

    if (auto* msg = SKSE::GetMessagingInterface()) {
        msg->RegisterListener([](SKSE::MessagingInterface::Message* m) {
            if (!m) {
                return;
            }

            if (m->type == SKSE::MessagingInterface::kPostLoadGame ||
                m->type == SKSE::MessagingInterface::kNewGame) {
                const bool isNewGame = (m->type == SKSE::MessagingInterface::kNewGame);
                spdlog::info("[TFD] Messaging load event type={} -> mark finalize pending", m->type);

                if (isNewGame) {
                    TFD::DefeatMonitor::QueueDefaultProgressState();
                }
                gPendingLoadFinalize.store(true, std::memory_order_release);
            }
            });
    }

    if (auto* tasks = SKSE::GetTaskInterface()) {
        tasks->AddTask([]() {
            if (gSinkRegistered.exchange(true, std::memory_order_acq_rel)) {
                return;
            }

            if (auto* ui = RE::UI::GetSingleton()) {
                ui->AddEventSink<RE::MenuOpenCloseEvent>(&gLoadSink);
                spdlog::info("[TFD] LoadMenuSink registered");
            }
            else {
                spdlog::warn("[TFD] UI singleton null (cannot register sink)");
                gSinkRegistered.store(false, std::memory_order_release);
            }
            });
    }

    return true;
}
