// Copyright 2026 The simple-engine-2 Authors.
//
// The Editor's ImGui shell: the DockSpace (seeded once with DockBuilder),
// the menus, the two built-in panels ("Play" and "Document") beside the
// panels of se_editor_panels the executable adds, the modals (the quit prompt
// and its progress, the recovery offer, the fault screen) and the PanelContext
// every panel draws from.

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
constexpr const char* kClosePopup = "Unsaved changes##close";
constexpr const char* kReloadPopup = "File changed on disk";

// Where the seeded layout docks a panel, by its window title (the names
// of se_editor_panels; unknown ones go right). The built-in Play and
// Document panels sit above the right node.
enum class Dock : std::uint8_t { kLeft, kRight, kBottom, kCenter };

// The tabs a seeded layout shows first, the last one focused (one a frame,
// so the layout has settled kFocusAfterFrames + 5 frames after the seeding).
constexpr const char* kFirstTabs[] = {"Play", "Log", "Inspector", "Hierarchy",
                                      "Viewport"};
// Frames after the seeding until the windows exist and can be focused.
constexpr int kFocusAfterFrames = 2;

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
      const ImGuiID right_all = ImGui::DockBuilderSplitNode(
          center, ImGuiDir_Right, 0.28F, nullptr, &center);
      const ImGuiID bottom = ImGui::DockBuilderSplitNode(
          center, ImGuiDir_Down, 0.28F, nullptr, &center);
      ImGuiID right = right_all;
      const ImGuiID controls = ImGui::DockBuilderSplitNode(
          right, ImGuiDir_Up, 0.24F, nullptr, &right);
      ImGui::DockBuilderDockWindow("Play", controls);
      ImGui::DockBuilderDockWindow("Document", controls);
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
      focus_delay_ = kFocusAfterFrames;
      for (const char* tab : kFirstTabs) focus_queue_.emplace_back(tab);
      SE_LOG_INFO("editor: dock layout seeded ({} panels)", panels_.size());
    }
  }
  // The central node lets the composite (the World) show through.
  ImGui::DockSpaceOverViewport(dock, viewport,
                               ImGuiDockNodeFlags_PassthruCentralNode);
}

void Editor::UndoRedo(bool undo) {
  EditDocument* document = Active();
  if (document == nullptr || play_) return;
  edits_.EndScopeOf(documents_, documents_.ActiveId(), "an undo or redo");
  const InlineOwnerScope owner(OnMainThread());
  if (auto done = undo ? document->Undo() : document->Redo(); !done) {
    last_edit_error_ = done.error().message;
  }
}

void Editor::HandleShortcuts(App& app) {
  // Global routes: an active item that uses the chord (a text field's own
  // Ctrl+Z and Ctrl+Y) takes it first.
  constexpr ImGuiInputFlags kGlobal = ImGuiInputFlags_RouteGlobal;
  constexpr ImGuiInputFlags kRepeat = kGlobal | ImGuiInputFlags_Repeat;
  const EditorMode mode = Mode();
  const bool editing = mode == EditorMode::kEdit && Active() != nullptr;
  const bool scene =
      Active() != nullptr && Active()->Desc().kind == DocumentKind::kScene;
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, kGlobal) && editing) {
    if (auto saved = SaveDocument(app, documents_.ActiveId(), {},
                                  /*durable=*/true);
        !saved) {
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
      ((mode == EditorMode::kEdit && scene) || mode == EditorMode::kPaused)) {
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
  const bool has_document = Active() != nullptr;
  const bool scene =
      has_document && Active()->Desc().kind == DocumentKind::kScene;
  SDL_Window* window =
      app.GetWindow() != nullptr ? app.GetWindow()->Native() : nullptr;
  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("Open...", nullptr, false, editing)) {
      dialogs_->ShowOpen(window, app.Paths().content_root);
    }
    if (ImGui::MenuItem("Close", nullptr, false, editing && has_document)) {
      RequestClose(app, documents_.ActiveId());
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Save", "Ctrl+S", false, editing && has_document)) {
      if (auto saved = SaveDocument(app, documents_.ActiveId(), {},
                                    /*durable=*/true);
          !saved) {
        save_error_ = saved.error().message;
      }
    }
    if (ImGui::MenuItem("Save As...", nullptr, false,
                        editing && has_document)) {
      dialogs_->ShowSave(window, app.Paths().content_root);
    }
    if (ImGui::MenuItem("Save All", nullptr, false,
                        editing && !documents_.Dirty().empty())) {
      SaveAll(app);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Quit")) app.RequestQuit();
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Edit")) {
    const bool can_undo = editing && has_document && Active()->UndoCount() > 0;
    const bool can_redo = editing && has_document && Active()->RedoCount() > 0;
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
    // Play plays the active scene; it is disabled on a prefab document.
    if (ImGui::MenuItem("Play", "F5", false,
                        (mode == EditorMode::kEdit && scene) ||
                            mode == EditorMode::kPaused)) {
      request_ = Request::kPlay;
    }
    if (ImGui::MenuItem("Play game", nullptr, false,
                        mode == EditorMode::kEdit && scene)) {
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
      const bool scene =
          Active() != nullptr && Active()->Desc().kind == DocumentKind::kScene;
      ImGui::BeginDisabled(!scene);
      if (ImGui::Button("Play")) request_ = Request::kPlay;
      ImGui::SameLine();
      if (ImGui::Button("Play game")) request_ = Request::kPlayGame;
      ImGui::EndDisabled();
      if (!scene) ImGui::TextDisabled("Play plays a scene document");
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
    // The document tabs: a click activates one (not while Play runs).
    ImGui::BeginDisabled(documents_.Frozen());
    DocumentId activate = kNoDocument;
    for (std::size_t i = 0; i < documents_.size(); ++i) {
      const DocumentEntry& entry = *documents_.All()[i];
      const EditDocument& doc = *entry.document;
      const std::string label = std::format(
          "{}{}{}##doc{}", doc.Desc().name,
          doc.Desc().kind == DocumentKind::kPrefab ? " (prefab)" : "",
          doc.Dirty() ? " *" : "", entry.id);
      if (i > 0) ImGui::SameLine();
      const ImVec2 size = ImGui::CalcTextSize(label.c_str(), nullptr, true);
      if (ImGui::Selectable(label.c_str(), entry.id == documents_.ActiveId(),
                            ImGuiSelectableFlags_None, size)) {
        activate = entry.id;
      }
    }
    ImGui::EndDisabled();
    if (activate != kNoDocument) ActivateDocument(activate);
    ImGui::Separator();
    if (const EditDocument* active = Active()) {
      const EditDocument& doc = *active;
      Text(std::format("{} {}{}", DocumentKindName(doc.Desc().kind),
                       doc.Desc().name, doc.Dirty() ? " *" : ""));
      Text(doc.Desc().path);
      Text(
          std::format("{} entities | revision {} | hash {:016x} | content "
                      "{:016x}",
                      doc.Get().EntityCount(), doc.Revision(), doc.Hash(),
                      doc.ContentHash()));
      Text(std::format("selection: {} | prefabs: {}",
                       doc.GetSelection().Count(), doc.PrefabTexts().size()));
      Text(std::format("undo: {} ({}) | redo: {} ({})", doc.UndoCount(),
                       doc.UndoLabel(), doc.RedoCount(), doc.RedoLabel()));
      if (const DocumentEntry* entry = documents_.Active();
          entry != nullptr && !entry->save_error.empty()) {
        Text("save failed: " + entry->save_error);
      }
    } else {
      ImGui::TextDisabled("No document: File > Open");
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
      // Every dirty document.
      Text("Save changes before quitting?");
      for (const DocumentEntry* entry : documents_.Dirty()) {
        Text(std::format("  {}{}", entry->document->Desc().name,
                         entry->save_error.empty()
                             ? std::string()
                             : " (" + entry->save_error + ")"));
      }
      if (ImGui::Button("Save All")) ResolveQuitPrompt(app, "save");
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
  if (const DocumentEntry* closing = documents_.Find(close_prompt_)) {
    ImGui::OpenPopup(kClosePopup);
    if (ImGui::BeginPopupModal(kClosePopup, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      Text(std::format("Save changes to '{}' before closing it?",
                       closing->document->Desc().name));
      if (ImGui::Button("Save")) ResolveClosePrompt(app, "save");
      ImGui::SameLine();
      if (ImGui::Button("Don't Save")) ResolveClosePrompt(app, "discard");
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) ResolveClosePrompt(app, "cancel");
      ImGui::EndPopup();
    }
  }
  const DocumentEntry* asking = nullptr;
  for (const auto& entry : documents_.All()) {
    if (entry->reload_prompt && asking == nullptr) asking = entry.get();
  }
  if (asking != nullptr) {
    ImGui::OpenPopup(kReloadPopup);
    if (ImGui::BeginPopupModal(kReloadPopup, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      Text(std::format(
          "{} was changed by another program, and '{}' has "
          "unsaved changes.",
          asking->document->Desc().path, asking->document->Desc().name));
      if (ImGui::Button("Reload")) ResolveReloadPrompt(app, "reload");
      ImGui::SameLine();
      if (ImGui::Button("Keep mine")) ResolveReloadPrompt(app, "keep");
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
  EditDocument* document = Active();
  if (play_) {
    context.world = play_->TopWorld();
  } else if (document != nullptr) {
    context.world = &document->Get();
  }
  context.playing = play_ != nullptr && !faulted;
  context.faulted = faulted;
  context.paused = play_ && (play_->State() == PlayState::kPaused ||
                             play_->State() == PlayState::kStepping);
  context.selection = document != nullptr ? &document->GetSelection() : nullptr;
  context.commands = play_ || document == nullptr ? nullptr : commands_.get();
  // Play.Faulted keeps the PlayEdits for its Select only: the frozen World
  // stays inspectable, and Submit drops everything (ADR-111).
  context.play_edits = play_ ? play_edits_.get() : nullptr;
  context.textures = textures_.get();
  context.stats = &stats_;
  context.log = &log_;
  context.real_delta_sec = report.clock.real_delta;
  context.viewports = app.Renderer() != nullptr ? viewports_.get() : nullptr;
  context.play_game =
      play_ != nullptr && play_->Kind() == PlayKind::kPlayGame && !faulted;
  // M11b (ADR-118): the scene hooks and the document panels cache against.
  context.game = &game_;
  if (document != nullptr) {
    context.asset_root = document->Desc().asset_root;
    context.prefab_document = document->Desc().kind == DocumentKind::kPrefab;
    context.document = documents_.ActiveId();
    context.revision = play_ ? 0 : document->Revision();
    if (!play_ && document->Desc().kind == DocumentKind::kScene) {
      context.prefab_texts = &document->PrefabTexts();
    }
  }
  viewports_->StartReports();
  DrawBuiltInPanels(context);
  ++ui_frame_;
  for (std::size_t i = 0; i < panels_.size(); ++i) {
    Panel& panel = *panels_[i];
    const std::string name(panel.Name());
    if (ImGui::Begin(name.c_str())) {
      drawing_panel_ = panel.Name();
      panel.Draw(context);
      drawing_panel_ = {};
      ++panel_draws_[i];
      panel_frame_[i] = ui_frame_;
    }
    ImGui::End();
  }
  LatchGameInput();
  // A seeded layout shows its first tabs; a script brings a panel forward.
  // One window a frame: a dock node selects the tab of the window that has
  // the focus when the frame ends.
  if (focus_delay_ > 0) {
    --focus_delay_;
  } else if (!focus_queue_.empty()) {
    const std::string& name = focus_queue_.front();
    if (ImGui::FindWindowByName(name.c_str()) != nullptr) {
      ImGui::SetWindowFocus(name.c_str());
    }
    focus_queue_.pop_front();
  }
  DrawModals(app);
  if (show_demo_) ImGui::ShowDemoWindow(&show_demo_);
}

}  // namespace se::editor
