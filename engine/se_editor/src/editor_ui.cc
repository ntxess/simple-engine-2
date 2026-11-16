// Copyright 2026 The simple-engine-2 Authors.
//
// The Editor's ImGui shell: the DockSpace (seeded once with DockBuilder),
// the menus, the two built-in panels of M10a ("Play" and "Document"; the
// real panels are M10b's), the modals (the quit prompt and its progress,
// the recovery offer, the fault screen) and the PanelContext every panel
// draws from.

#include <imgui.h>
#include <imgui_internal.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>

#include "se_core/log.h"
#include "se_core/thread_role.h"
#include "se_editor/editor.h"
#include "se_editor/panel.h"
#include "se_editor/play_session.h"
#include "se_runtime/app.h"
#include "src/editor_internal.h"

namespace se::editor {
namespace {

constexpr const char* kDockName = "EditorDockSpace";
constexpr const char* kQuitPopup = "Unsaved changes";
constexpr const char* kSavingPopup = "Saving";
constexpr const char* kRecoveryPopup = "Recover unsaved changes";

// Where the seeded layout docks a panel, by its window title (M10b's
// names; unknown ones go right).
enum class Dock : std::uint8_t { kLeft, kRight, kBottom, kCenter };

Dock DockOf(std::string_view name) {
  if (name == "Hierarchy") return Dock::kLeft;
  if (name == "Viewport") return Dock::kCenter;
  if (name == "Log" || name == "Profiler" || name == "Schedule") {
    return Dock::kBottom;
  }
  return Dock::kRight;
}

void Text(std::string_view text) {
  ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

}  // namespace

void Editor::DrawDockSpace() {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  const ImGuiID dock = ImGui::GetID(kDockName);
  if (!dock_seeded_) {
    dock_seeded_ = true;
    // Seeded once: settings from the pref path that hold the node win.
    if (ImGui::DockBuilderGetNode(dock) == nullptr) {
      ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
      ImGui::DockBuilderSetNodeSize(dock, viewport->WorkSize);
      ImGuiID center = dock;
      const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left,
                                                       0.20F, nullptr, &center);
      const ImGuiID right = ImGui::DockBuilderSplitNode(
          center, ImGuiDir_Right, 0.28F, nullptr, &center);
      const ImGuiID bottom = ImGui::DockBuilderSplitNode(
          center, ImGuiDir_Down, 0.28F, nullptr, &center);
      ImGui::DockBuilderDockWindow("Play", right);
      ImGui::DockBuilderDockWindow("Document", right);
      for (const auto& panel : panels_) {
        const std::string name(panel->Name());
        switch (DockOf(name)) {
          case Dock::kLeft:
            ImGui::DockBuilderDockWindow(name.c_str(), left);
            break;
          case Dock::kRight:
            ImGui::DockBuilderDockWindow(name.c_str(), right);
            break;
          case Dock::kBottom:
            ImGui::DockBuilderDockWindow(name.c_str(), bottom);
            break;
          case Dock::kCenter:
            ImGui::DockBuilderDockWindow(name.c_str(), center);
            break;
        }
      }
      ImGui::DockBuilderFinish(dock);
      SE_LOG_INFO("editor: dock layout seeded");
    }
  }
  // The central node lets the composite (the World) show through.
  ImGui::DockSpaceOverViewport(dock, viewport,
                               ImGuiDockNodeFlags_PassthruCentralNode);
}

void Editor::UndoRedo(bool undo) {
  if (!document_ || play_) return;
  const InlineOwnerScope owner(OnMainThread());
  if (auto done = undo ? document_->Undo() : document_->Redo(); !done) {
    last_edit_error_ = done.error().message;
  }
}

void Editor::HandleShortcuts(App& app) {
  // Global routes: an active item that uses the chord (a text field's own
  // Ctrl+Z and Ctrl+Y) takes it first.
  constexpr ImGuiInputFlags kGlobal = ImGuiInputFlags_RouteGlobal;
  constexpr ImGuiInputFlags kRepeat = kGlobal | ImGuiInputFlags_Repeat;
  const EditorMode mode = Mode();
  const bool editing = mode == EditorMode::kEdit && document_ != nullptr;
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, kGlobal) && editing) {
    if (auto saved = SaveDocument(app, {}, /*durable=*/true); !saved) {
      save_error_ = saved.error().message;
    }
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, kRepeat) && editing) {
    UndoRedo(/*undo=*/true);
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, kRepeat) && editing) {
    UndoRedo(/*undo=*/false);
  }
  if (ImGui::Shortcut(ImGuiKey_F5, kGlobal) &&
      (mode == EditorMode::kEdit || mode == EditorMode::kPaused)) {
    request_ = Request::kPlay;
  }
  if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_F5, kGlobal) &&
      mode != EditorMode::kEdit) {
    request_ = Request::kStop;
  }
  if (ImGui::Shortcut(ImGuiKey_F10, kGlobal) && mode == EditorMode::kPaused) {
    request_ = Request::kStep;
  }
}

void Editor::DrawMenus(App& app) {
  if (!ImGui::BeginMainMenuBar()) return;
  HandleShortcuts(app);
  const bool editing = !play_;
  SDL_Window* window =
      app.GetWindow() != nullptr ? app.GetWindow()->Native() : nullptr;
  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("Open...", nullptr, false, editing)) {
      dialogs_->ShowOpen(window, app.Paths().content_root);
    }
    if (ImGui::MenuItem("Save", "Ctrl+S", false, editing && document_)) {
      if (auto saved = SaveDocument(app, {}, /*durable=*/true); !saved) {
        save_error_ = saved.error().message;
      }
    }
    if (ImGui::MenuItem("Save As...", nullptr, false, editing && document_)) {
      dialogs_->ShowSave(window, app.Paths().content_root);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Quit")) app.RequestQuit();
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Edit")) {
    const bool can_undo = editing && document_ && document_->UndoCount() > 0;
    const bool can_redo = editing && document_ && document_->RedoCount() > 0;
    if (ImGui::MenuItem("Undo", "Ctrl+Z", false, can_undo)) {
      UndoRedo(/*undo=*/true);
    }
    if (ImGui::MenuItem("Redo", "Ctrl+Y", false, can_redo)) {
      UndoRedo(/*undo=*/false);
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Play")) {
    const EditorMode mode = Mode();
    if (ImGui::MenuItem(
            "Play", "F5", false,
            mode == EditorMode::kEdit || mode == EditorMode::kPaused)) {
      request_ = Request::kPlay;
    }
    if (ImGui::MenuItem("Play game", nullptr, false,
                        mode == EditorMode::kEdit)) {
      request_ = Request::kPlayGame;
    }
    if (ImGui::MenuItem("Pause", nullptr, false,
                        mode == EditorMode::kPlaying)) {
      request_ = Request::kPause;
    }
    if (ImGui::MenuItem("Step", "F10", false, mode == EditorMode::kPaused)) {
      request_ = Request::kStep;
    }
    if (ImGui::MenuItem("Stop", "Shift+F5", false, mode != EditorMode::kEdit)) {
      request_ = Request::kStop;
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("View")) {
    ImGui::MenuItem("60 Hz preview", nullptr, &preview_60hz_);
    ImGui::MenuItem("ImGui demo", nullptr, &show_demo_);
    ImGui::EndMenu();
  }
  ImGui::EndMainMenuBar();
}

void Editor::DrawBuiltInPanels(const PanelContext& context) {
  if (ImGui::Begin("Play")) {
    const EditorMode mode = Mode();
    Text(std::format("Mode: {}", EditorModeName(mode)));
    if (play_) {
      Text(std::format(
          "{} | tick {} | hash {:016x}",
          play_->Kind() == PlayKind::kPlayGame ? "Play game" : "Play",
          play_->LastTick(), play_->Hash()));
      Text(std::format("SessionLog: {} applied, {} pending",
                       play_->Log().Applied().size(),
                       play_->Log().PendingCount()));
    }
    if (context.stats != nullptr) {
      const EditorStats& stats = *context.stats;
      Text(std::format("{} x{} L{} {:.1f} Hz{}{}",
                       PacingModeName(stats.pacing_mode),
                       stats.present_interval, stats.frame_latency,
                       stats.display_hz, stats.half_rate ? " HalfRate" : "",
                       stats.plan.overloaded ? " Overloaded" : ""));
      if (stats.plan.dilation_sec > 0.0) {
        Text(std::format("dilated {:.3f} s", stats.plan.dilation_sec));
      }
    }
    if (mode == EditorMode::kEdit) {
      if (ImGui::Button("Play")) request_ = Request::kPlay;
      ImGui::SameLine();
      if (ImGui::Button("Play game")) request_ = Request::kPlayGame;
    } else {
      if (mode == EditorMode::kPlaying && ImGui::Button("Pause")) {
        request_ = Request::kPause;
      }
      if (mode == EditorMode::kPaused) {
        if (ImGui::Button("Resume")) request_ = Request::kResume;
        ImGui::SameLine();
        if (ImGui::Button("Step")) request_ = Request::kStep;
      }
      ImGui::SameLine();
      if (ImGui::Button("Stop")) request_ = Request::kStop;
    }
    if (play_ && play_->Faulted()) {
      // The fault screen: what, where, when; the World stays inspectable.
      const PlayFault& fault = play_->FaultInfo();
      ImGui::Separator();
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.4F, 0.4F, 1.0F));
      Text(std::format("Play.Faulted: {} fault at tick {}", fault.where,
                       fault.tick));
      if (!fault.system.empty()) Text("system: " + fault.system);
      ImGui::TextWrapped("%s", fault.what.c_str());
      ImGui::PopStyleColor();
      if (!fault_directory_.empty()) Text("saved to " + fault_directory_);
    }
    ImGui::Checkbox("60 Hz preview", &preview_60hz_);
  }
  ImGui::End();
  if (ImGui::Begin("Document")) {
    if (document_) {
      const EditDocument& doc = *document_;
      Text(std::format("{}{}", doc.Desc().name, doc.Dirty() ? " *" : ""));
      Text(doc.Desc().path);
      Text(std::format("{} entities | revision {} | hash {:016x}",
                       doc.Get().EntityCount(), doc.Revision(), doc.Hash()));
      Text(std::format("selection: {}", doc.GetSelection().Count()));
      Text(std::format("undo: {} ({}) | redo: {} ({})", doc.UndoCount(),
                       doc.UndoLabel(), doc.RedoCount(), doc.RedoLabel()));
    }
    if (!last_edit_error_.empty()) Text("last error: " + last_edit_error_);
    if (!save_error_.empty()) Text("save failed: " + save_error_);
  }
  ImGui::End();
}

void Editor::DrawModals(App& app) {
  if (quit_phase_ == QuitPhase::kPrompt) {
    ImGui::OpenPopup(kQuitPopup);
    if (ImGui::BeginPopupModal(kQuitPopup, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      Text(std::format("Save changes to '{}' before quitting?",
                       document_ ? document_->Desc().name : std::string()));
      if (ImGui::Button("Save")) ResolveQuitPrompt(app, "save");
      ImGui::SameLine();
      if (ImGui::Button("Don't Save")) ResolveQuitPrompt(app, "discard");
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) ResolveQuitPrompt(app, "cancel");
      ImGui::EndPopup();
    }
  }
  if (quit_phase_ == QuitPhase::kSaving) {
    ImGui::OpenPopup(kSavingPopup);
    if (ImGui::BeginPopupModal(kSavingPopup, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      std::size_t durable = 0;
      for (const PendingSave& save : saves_) {
        if (save.durable) ++durable;
      }
      Text(std::format("Saving {} file(s)...", durable));
      if (dialogs_->Pending() > 0) Text("Waiting for a file dialog...");
      ImGui::EndPopup();
    }
  }
  if (!recovery_offer_.empty()) {
    ImGui::OpenPopup(kRecoveryPopup);
    if (ImGui::BeginPopupModal(kRecoveryPopup, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      const RecoveryFile& file = recovery_offer_.front();
      Text(
          std::format("'{}' has unsaved changes from an earlier session "
                      "(revision {}).",
                      file.name, file.revision));
      if (ImGui::Button("Recover")) ResolveRecovery(app, "accept");
      ImGui::SameLine();
      if (ImGui::Button("Discard")) ResolveRecovery(app, "discard");
      ImGui::EndPopup();
    }
  }
}

void Editor::DrawUi(App& app, const FrameReport& report) {
  DrawMenus(app);
  DrawDockSpace();
  PanelContext context;
  const bool faulted = play_ && play_->Faulted();
  if (play_) {
    context.world = play_->TopWorld();
  } else if (document_) {
    context.world = &document_->Get();
  }
  context.playing = play_ != nullptr && !faulted;
  context.faulted = faulted;
  context.paused = play_ && (play_->State() == PlayState::kPaused ||
                             play_->State() == PlayState::kStepping);
  context.selection = document_ ? &document_->GetSelection() : nullptr;
  context.commands = play_ || !document_ ? nullptr : commands_.get();
  context.play_edits = context.playing ? play_edits_.get() : nullptr;
  context.textures = textures_.get();
  context.stats = &stats_;
  context.log = &log_;
  context.real_delta_sec = report.clock.real_delta;
  context.viewports = app.Renderer() != nullptr ? viewports_.get() : nullptr;
  DrawBuiltInPanels(context);
  for (const auto& panel : panels_) {
    const std::string name(panel->Name());
    if (ImGui::Begin(name.c_str())) panel->Draw(context);
    ImGui::End();
  }
  DrawModals(app);
  if (show_demo_) ImGui::ShowDemoWindow(&show_demo_);
}

}  // namespace se::editor
