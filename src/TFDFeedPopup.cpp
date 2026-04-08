#include "TFDFeedPopup.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4099 5054)
#endif
#include "SKSEMenuFramework.h"
#include "TFDPacify.h"
#include "TFDTameBait.h"

namespace TFD::FeedPopup
{
    namespace
    {
        constexpr double kCompanionFeedHours = 3.0;
        constexpr std::int32_t kCalmFeedCost = 1;
        constexpr std::int32_t kCompanionFeedCost = 2;

        enum class PopupStage : std::uint8_t
        {
            Actions = 0,
            BaitSelect
        };

        enum class PopupAction : std::uint8_t
        {
            None = 0,
            CalmFeed,
            CompanionFeed,
            Release
        };

        struct ActionOption
        {
            PopupAction action{ PopupAction::None };
            std::string label{};
        };

        struct BaitOption
        {
            RE::FormID itemId{ 0 };
            std::string label{};
            std::string itemName{};
            std::int32_t count{ 0 };
            std::int32_t cost{ 1 };
            double calmExtendSec{ 0.0 };
        };

        struct PendingFeedPopup
        {
            bool initialized{ false };
            bool active{ false };
            RE::ActorHandle targetHandle{};
            RE::FormID targetId{ 0 };
            std::string targetName{};
            PopupStage stage{ PopupStage::Actions };
            TFD::Pacify::TameDisposition disposition{ TFD::Pacify::TameDisposition::None };
            PopupAction pendingAction{ PopupAction::None };
            std::vector<ActionOption> actions{};
            std::vector<BaitOption> baits{};
            std::size_t selectedIndex{ 0 };
            SKSEMenuFramework::Model::WindowInterface* window{ nullptr };
            std::mutex mutex{};
        };

        struct PopupSnapshot
        {
            bool active{ false };
            RE::ActorHandle targetHandle{};
            RE::FormID targetId{ 0 };
            std::string targetName{};
            PopupStage stage{ PopupStage::Actions };
            TFD::Pacify::TameDisposition disposition{ TFD::Pacify::TameDisposition::None };
            PopupAction pendingAction{ PopupAction::None };
            std::vector<ActionOption> actions{};
            std::vector<BaitOption> baits{};
            std::size_t selectedIndex{ 0 };
        };

        PendingFeedPopup g_popup{};

        static RE::Actor* ResolveTarget(const RE::ActorHandle& handle)
        {
            auto sp = handle.get();
            return sp.get();
        }

        static const char* ActionName(PopupAction action)
        {
            switch (action) {
            case PopupAction::CalmFeed:
                return "Calm Feed";
            case PopupAction::CompanionFeed:
                return "Teammate Feed";
            case PopupAction::Release:
                return "Release";
            default:
                return "None";
            }
        }

        static std::string BuildWindowTitle(const std::string& targetName)
        {
            std::string title = "TFD: Tame Command - ";
            title += targetName.empty() ? "Creature" : targetName;
            return title;
        }

        static PopupSnapshot CopySnapshotLocked()
        {
            PopupSnapshot snap{};
            snap.active = g_popup.active;
            snap.targetHandle = g_popup.targetHandle;
            snap.targetId = g_popup.targetId;
            snap.targetName = g_popup.targetName;
            snap.stage = g_popup.stage;
            snap.disposition = g_popup.disposition;
            snap.pendingAction = g_popup.pendingAction;
            snap.actions = g_popup.actions;
            snap.baits = g_popup.baits;
            snap.selectedIndex = g_popup.selectedIndex;
            return snap;
        }

        static void ClearPopupStateLocked(bool closeWindow)
        {
            g_popup.active = false;
            g_popup.targetHandle = RE::ActorHandle{};
            g_popup.targetId = 0;
            g_popup.targetName.clear();
            g_popup.stage = PopupStage::Actions;
            g_popup.disposition = TFD::Pacify::TameDisposition::None;
            g_popup.pendingAction = PopupAction::None;
            g_popup.actions.clear();
            g_popup.baits.clear();
            g_popup.selectedIndex = 0;
            if (closeWindow && g_popup.window) {
                g_popup.window->BlockUserInput = false;
                g_popup.window->IsOpen = false;
            }
        }

        static void RefreshActionsLocked(RE::Actor* target)
        {
            g_popup.actions.clear();
            g_popup.disposition = target ? TFD::Pacify::GetDisposition(target) : TFD::Pacify::TameDisposition::None;

            if (g_popup.disposition == TFD::Pacify::TameDisposition::Companion) {
                g_popup.actions.push_back({ PopupAction::CompanionFeed, "Teammate Feed (+3 in-game hours, cost 2 bait)" });
                g_popup.actions.push_back({ PopupAction::Release, "Release" });
            } else {
                g_popup.actions.push_back({ PopupAction::CalmFeed, "Calm Feed (cost 1 bait)" });
                g_popup.actions.push_back({ PopupAction::CompanionFeed, "Teammate Feed (+3 in-game hours, cost 2 bait)" });
                g_popup.actions.push_back({ PopupAction::Release, "Release" });
            }

            g_popup.stage = PopupStage::Actions;
            g_popup.pendingAction = PopupAction::None;
            g_popup.baits.clear();
            g_popup.selectedIndex = 0;
        }

        static std::vector<BaitOption> BuildBaitsForAction(RE::Actor* player, RE::Actor* target, PopupAction action)
        {
            std::vector<BaitOption> result{};
            if (!player || !target) {
                return result;
            }

            const auto options = TFD::TameBait::CollectValidBaits(player, target);
            const std::int32_t cost = (action == PopupAction::CompanionFeed) ? kCompanionFeedCost : kCalmFeedCost;
            for (const auto& opt : options) {
                if (!opt.item || opt.count < cost) {
                    continue;
                }

                BaitOption bait{};
                bait.itemId = opt.item->GetFormID();
                bait.itemName = opt.name;
                bait.count = opt.count;
                bait.cost = cost;
                bait.calmExtendSec = opt.extendSec;

                char buffer[192];
                if (action == PopupAction::CompanionFeed) {
                    std::snprintf(buffer, sizeof(buffer), "%s x%d (cost %d, +%.0fh)", opt.name.c_str(), opt.count, cost, kCompanionFeedHours);
                } else {
                    std::snprintf(buffer, sizeof(buffer), "%s x%d (cost %d, +%.0fs)", opt.name.c_str(), opt.count, cost, opt.extendSec);
                }
                bait.label = buffer;
                result.push_back(std::move(bait));
            }
            return result;
        }

        static bool StepSelectionInternal(int delta)
        {
            std::scoped_lock lock(g_popup.mutex);
            if (!g_popup.active) {
                return false;
            }

            const auto count = (g_popup.stage == PopupStage::Actions) ? g_popup.actions.size() : g_popup.baits.size();
            if (count == 0) {
                return false;
            }

            int idx = static_cast<int>(g_popup.selectedIndex);
            idx = (idx + delta) % static_cast<int>(count);
            if (idx < 0) {
                idx += static_cast<int>(count);
            }
            g_popup.selectedIndex = static_cast<std::size_t>(idx);
            return true;
        }

        static bool ApplyBaitSelection(const PopupSnapshot& snap, std::size_t index)
        {
            if (index >= snap.baits.size()) {
                return false;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* target = ResolveTarget(snap.targetHandle);
            if (!player || !target) {
                RE::DebugNotification("TFD: Command Failed. Target Lost.");
                return true;
            }

            if (!TFD::Pacify::HasActiveTameSession(target)) {
                RE::DebugNotification("TFD: Command Failed. No active tame session.");
                return true;
            }

            const auto& chosen = snap.baits[index];
            auto* item = RE::TESForm::LookupByID<RE::TESBoundObject>(chosen.itemId);
            if (!item) {
                RE::DebugNotification("TFD: Command Failed. Bait Missing.");
                return true;
            }

            bool ok = false;
            if (snap.pendingAction == PopupAction::CalmFeed) {
                ok = TFD::Pacify::ExtendActiveTameSession(target, chosen.calmExtendSec, 0.0);
                if (!ok) {
                    RE::DebugNotification("TFD: Calm Feed Failed.");
                    return true;
                }
                if (!TFD::TameBait::ConsumeBait(player, item, chosen.cost)) {
                    RE::DebugNotification("TFD: Calm Feed Failed. Could not consume bait.");
                    return true;
                }
                char msg[128];
                std::snprintf(msg, sizeof(msg), "TFD: Calm +%.0fs", chosen.calmExtendSec);
                RE::DebugNotification(msg);
                return true;
            }

            if (snap.pendingAction == PopupAction::CompanionFeed) {
                if (TFD::Pacify::IsCompanion(target)) {
                    ok = TFD::Pacify::ExtendActiveCompanionHours(target, kCompanionFeedHours);
                } else {
                    ok = TFD::Pacify::PromoteActiveTameToCompanion(target, kCompanionFeedHours);
                }

                if (!ok) {
                    RE::DebugNotification("TFD: Teammate Feed Failed.");
                    return true;
                }
                if (!TFD::TameBait::ConsumeBait(player, item, chosen.cost)) {
                    RE::DebugNotification("TFD: Teammate Feed Failed. Could not consume bait.");
                    return true;
                }
                char msg[128];
                std::snprintf(msg, sizeof(msg), "TFD: Teammate +%.0fh", kCompanionFeedHours);
                RE::DebugNotification(msg);
                return true;
            }

            return false;
        }

        static bool EnterBaitStageOrApply(PopupAction action)
        {
            RE::Actor* target = nullptr;
            {
                std::scoped_lock lock(g_popup.mutex);
                if (!g_popup.active) {
                    return false;
                }
                target = ResolveTarget(g_popup.targetHandle);
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !target) {
                Close();
                RE::DebugNotification("TFD: Command Failed. Target Lost.");
                return true;
            }

            const auto baits = BuildBaitsForAction(player, target, action);
            if (baits.empty()) {
                Close();
                RE::DebugNotification("TFD: Command Failed. You do not have the correct bait.");
                return true;
            }

            if (baits.size() == 1) {
                PopupSnapshot snap{};
                snap.active = true;
                snap.targetHandle = target->GetHandle();
                snap.targetId = target->GetFormID();
                snap.targetName = (target->GetName() && target->GetName()[0]) ? target->GetName() : "Creature";
                snap.stage = PopupStage::BaitSelect;
                snap.disposition = TFD::Pacify::GetDisposition(target);
                snap.pendingAction = action;
                snap.baits = baits;
                Close();
                return ApplyBaitSelection(snap, 0);
            }

            {
                std::scoped_lock lock(g_popup.mutex);
                if (!g_popup.active) {
                    return false;
                }
                g_popup.stage = PopupStage::BaitSelect;
                g_popup.pendingAction = action;
                g_popup.baits = baits;
                g_popup.selectedIndex = 0;
            }
            return true;
        }

        static bool ApplyActionSelectionInternal(std::size_t index)
        {
            PopupAction action = PopupAction::None;
            RE::Actor* target = nullptr;
            {
                std::scoped_lock lock(g_popup.mutex);
                if (!g_popup.active || index >= g_popup.actions.size()) {
                    return false;
                }
                action = g_popup.actions[index].action;
                target = ResolveTarget(g_popup.targetHandle);
            }

            if (!target || !TFD::Pacify::HasActiveTameSession(target)) {
                Close();
                RE::DebugNotification("TFD: Command Failed. No active tame session.");
                return true;
            }

            switch (action) {
            case PopupAction::Release:
                Close();
                if (!TFD::Pacify::ReleaseActiveTameActor(target, TFD::Pacify::ReleaseReason::Generic)) {
                    RE::DebugNotification("TFD: Release Failed.");
                } else {
                    RE::DebugNotification("TFD: Tame Released");
                }
                return true;
            case PopupAction::CalmFeed:
            case PopupAction::CompanionFeed:
                return EnterBaitStageOrApply(action);
            case PopupAction::None:
            default:
                return false;
            }
        }

        static void RenderActionStage(const PopupSnapshot& snap)
        {
            if (snap.disposition == TFD::Pacify::TameDisposition::Companion) {
                ImGuiMCP::TextWrapped("Temporary teammate active. [Up/Down] choose command, [Enter] confirm, [Esc] close.");
                ImGuiMCP::Text("Remaining teammate time: %.1f hours", TFD::Pacify::GetRemainingCompanionHours(ResolveTarget(snap.targetHandle)));
            } else {
                ImGuiMCP::TextWrapped("Calm active. [Up/Down] choose command, [Enter] confirm, [Esc] close.");
            }
            ImGuiMCP::Separator();
            for (std::size_t i = 0; i < snap.actions.size(); ++i) {
                const bool selected = (i == snap.selectedIndex);
                ImGuiMCP::Text(selected ? "> %s" : "  %s", snap.actions[i].label.c_str());
            }
        }

        static void RenderBaitStage(const PopupSnapshot& snap)
        {
            ImGuiMCP::TextWrapped("%s - choose bait with [Up/Down], [Enter] confirm, [Esc] back.", ActionName(snap.pendingAction));
            ImGuiMCP::Separator();
            for (std::size_t i = 0; i < snap.baits.size(); ++i) {
                const bool selected = (i == snap.selectedIndex);
                ImGuiMCP::Text(selected ? "> %s" : "  %s", snap.baits[i].label.c_str());
            }
        }

        static void __stdcall RenderWindow()
        {
            PopupSnapshot snap{};
            {
                std::scoped_lock lock(g_popup.mutex);
                snap = CopySnapshotLocked();
                if (g_popup.window) {
                    g_popup.window->BlockUserInput = false;
                    g_popup.window->IsOpen = g_popup.active;
                }
            }

            if (!snap.active) {
                return;
            }

            auto* target = ResolveTarget(snap.targetHandle);
            if (!target || !TFD::Pacify::HasActiveTameSession(target)) {
                Close();
                return;
            }

            ImGuiMCP::SetNextWindowSize(ImGuiMCP::ImVec2(520.0f, 0.0f), ImGuiMCP::ImGuiCond_Appearing);
            const auto title = BuildWindowTitle(snap.targetName);
            const auto flags = ImGuiMCP::ImGuiWindowFlags_NoCollapse |
                               ImGuiMCP::ImGuiWindowFlags_NoSavedSettings |
                               ImGuiMCP::ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiMCP::ImGuiWindowFlags_NoInputs;

            if (ImGuiMCP::Begin(title.c_str(), nullptr, flags)) {
                if (snap.stage == PopupStage::Actions) {
                    RenderActionStage(snap);
                } else {
                    RenderBaitStage(snap);
                }
            }
            ImGuiMCP::End();
        }
    }

    void Init()
    {
        std::scoped_lock lock(g_popup.mutex);
        if (g_popup.initialized) {
            return;
        }

        g_popup.window = SKSEMenuFramework::AddWindow(RenderWindow, false);
        g_popup.initialized = (g_popup.window != nullptr);
        if (g_popup.window) {
            g_popup.window->BlockUserInput = false;
            g_popup.window->IsOpen = false;
            spdlog::info("TFDFeedPopup: SMF window registered (action popup)");
        } else {
            spdlog::warn("TFDFeedPopup: failed to register SMF window");
        }
    }

    bool Open(RE::Actor* target)
    {
        if (!target) {
            return false;
        }

        Init();
        std::scoped_lock lock(g_popup.mutex);
        if (!g_popup.window) {
            return false;
        }

        g_popup.active = true;
        g_popup.targetHandle = target->GetHandle();
        g_popup.targetId = target->GetFormID();
        g_popup.targetName = (target->GetName() && target->GetName()[0]) ? target->GetName() : "Creature";
        RefreshActionsLocked(target);
        if (g_popup.actions.empty()) {
            ClearPopupStateLocked(false);
            return false;
        }

        g_popup.window->BlockUserInput = false;
        g_popup.window->IsOpen = true;
        return true;
    }

    void Close()
    {
        std::scoped_lock lock(g_popup.mutex);
        ClearPopupStateLocked(true);
    }

    bool IsOpen()
    {
        std::scoped_lock lock(g_popup.mutex);
        return g_popup.active;
    }

    bool PrevSelection()
    {
        return StepSelectionInternal(-1);
    }

    bool NextSelection()
    {
        return StepSelectionInternal(1);
    }

    bool ConfirmSelection()
    {
        PopupSnapshot snap{};
        {
            std::scoped_lock lock(g_popup.mutex);
            if (!g_popup.active) {
                return false;
            }
            snap = CopySnapshotLocked();
        }

        if (snap.stage == PopupStage::Actions) {
            return ApplyActionSelectionInternal(snap.selectedIndex);
        }
        return ApplyBaitSelection(snap, snap.selectedIndex);
    }

    bool CancelSelection()
    {
        std::scoped_lock lock(g_popup.mutex);
        if (!g_popup.active) {
            return false;
        }

        if (g_popup.stage == PopupStage::BaitSelect) {
            auto* target = ResolveTarget(g_popup.targetHandle);
            RefreshActionsLocked(target);
            return true;
        }

        ClearPopupStateLocked(true);
        return true;
    }
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif
