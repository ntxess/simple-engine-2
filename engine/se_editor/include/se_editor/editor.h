// Copyright 2026 The simple-engine-2 Authors.
//
// Editor: the editor shell on the single loop (docs/milestone-specs.md
// M10a; docs/design/mt-design.md "Editor"; ADR-097 to ADR-099). It is the
// App's AppExtension: the App boots, paces, steps, renders and shuts down
// as for a game, and the Editor owns the frame's session and its UI. Main
// thread only, ImGui included (GImGui is thread_local).
//
// Each frame (AppExtension's order):
//   BeginFrame  the log lines into the LogView; dialog results; the work
//               that releases GPU resources or Worlds (Stop's destroy, a
//               document close, unused viewports); the script's actions;
//               Play requests (Play, Pause, Step, Stop take effect between
//               frames and are logged with their tick); autosave. Returns
//               the session: the document (never steps; Idle allowed), the
//               play session, or Play game.
//   TickFrame   the play session's ticks (contained: a throw is
//               Play.Faulted).
//   BuildFrame  ImGui's frame: the DockSpace (seeded once with DockBuilder,
//               persisted under the pref path), the menus, the panels with
//               a PanelContext, the modals (the quit prompt and progress,
//               the recovery offer, the fault screen); ImGui::Render; then
//               the frame's commands: document edits apply now (before the
//               RenderList build, so they show this frame), play edits go
//               to the SessionLog; then the render request: the play
//               World's RenderLists built on the guarded path (contained:
//               a throw is Play.Faulted, a second one discards the World),
//               the viewports, and ImGui as the swapchain overlay.
//   EndFrame    the 60 Hz preview's frame cap; multi-viewport windows.
//
// Pacing: Standard with frame latency 2 (the editor's GameSetup), the
// Overloaded floor of 0 ticks in editor Play, Idle (waitevent) after 1 s
// without input in Edit mode, a 60 Hz preview toggle.
//
// Quit (QuitProvider): waits while a file dialog is open; a prompt about
// dirty documents runs over normal frames (Save, Don't Save, Cancel); saves
// go through the lane as Durable jobs with a progress modal; a failed save
// keeps its document dirty and cancels the quit. The recovery hook writes
// every dirty document's recovery file synchronously.

#ifndef SE_EDITOR_EDITOR_H_
#define SE_EDITOR_EDITOR_H_

#include <SDL3/SDL_events.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "se_assets/asset_registry.h"
#include "se_assets/background_lane.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/input_frame.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/time.h"
#include "se_editor/commands.h"
#include "se_editor/dialog_mailbox.h"
#include "se_editor/edit_document.h"
#include "se_editor/editor_game.h"
#include "se_editor/editor_script.h"
#include "se_editor/editor_stats.h"
#include "se_editor/log_view.h"
#include "se_editor/panel.h"
#include "se_editor/play_session.h"
#include "se_editor/recovery.h"
#include "se_editor/selection.h"
#include "se_imgui/imgui_context.h"
#include "se_imgui/imgui_renderer.h"
#include "se_render/renderer_2d.h"
#include "se_render_core/render_list.h"
#include "se_runtime/app.h"
#include "se_runtime/app_extension.h"
#include "se_runtime/quit.h"
#include "se_sim/game_state.h"

namespace se::editor {

struct EditorOptions {
  // The document opened at boot: a scene file relative to the content
  // root (or absolute).
  std::string scene;
  // --editor-script FILE (test builds): actions at frame boundaries.
  std::string script;
  // ImGui multi-viewports (opt-in).
  bool viewports = false;
  std::uint64_t autosave_quiet_ns = kAutosaveQuietNs;
};

// The editor's mode, from the panels' point of view.
enum class EditorMode : std::uint8_t {
  kEdit,
  kPlaying,
  kPaused,
  kStepping,
  kFaulted,
};
[[nodiscard]] std::string_view EditorModeName(EditorMode mode) noexcept;

class Editor final : public AppExtension, public QuitProvider {
 public:
  Editor(EditorGame game, EditorOptions options);
  Editor(const Editor&) = delete;
  Editor& operator=(const Editor&) = delete;
  ~Editor() override;

  // Panels drawn each frame, in the order added (M10b's).
  void AddPanel(std::unique_ptr<Panel> panel);

  // --- AppExtension ---
  [[nodiscard]] std::expected<void, Error> Boot(App& app) override
      SE_REQUIRES(g_main);
  bool OnEvent(App& app, const SDL_Event& event) override SE_REQUIRES(g_main);
  [[nodiscard]] ExtensionSession BeginFrame(App& app,
                                            std::uint64_t now_ns) override
      SE_REQUIRES(g_main);
  [[nodiscard]] ExtensionTicks TickFrame(App& app, const StepPlan& plan,
                                         InputSource& input,
                                         AudioSink& audio) override
      SE_REQUIRES(g_main);
  void BuildFrame(App& app, const FrameReport& report,
                  ExtensionFrame& frame) override SE_REQUIRES(g_main);
  bool OnRenderFault(App& app, const Error& error) override SE_REQUIRES(g_main);
  void EndFrame(App& app, const FrameReport& report) override
      SE_REQUIRES(g_main);
  void OnDeviceLost(App& app) override SE_REQUIRES(g_main);
  void OnDeviceRecreated(App& app) override SE_REQUIRES(g_main);
  void ShutdownWorlds(App& app) override SE_REQUIRES(g_main);
  void ShutdownUi(App& app) override SE_REQUIRES(g_main);
  [[nodiscard]] bool ExitFailure() const noexcept override {
    return script_failures_ > 0;
  }

  // --- QuitProvider ---
  QuitPoll PollQuit(QuitReason reason) override;
  void QuitCancelled() override;
  [[nodiscard]] std::string_view Name() const override { return "editor"; }
  // GameSetup::recovery: WM_QUERYENDSESSION and TERMINATING write every
  // dirty document's recovery file synchronously.
  void WriteRecoveryFiles(QuitReason reason) SE_REQUIRES(g_main);

  // --- State, for tests ---
  [[nodiscard]] EditorMode Mode() const noexcept;
  [[nodiscard]] const EditDocument* Document() const noexcept {
    return document_.get();
  }
  [[nodiscard]] const PlaySession* Play() const noexcept { return play_.get(); }
  [[nodiscard]] const LogView& Log() const noexcept { return log_; }
  [[nodiscard]] std::uint32_t ScriptFailures() const noexcept {
    return script_failures_;
  }
  [[nodiscard]] bool ScriptDone() const noexcept { return script_done_; }

 private:
  class Commands;
  class PlayEditsImpl;
  class Viewports;
  class Textures;

  // A command waiting for the frame's apply step (after ImGui::Render).
  struct Queued {
    std::unique_ptr<EditorCommand> command;
    // Play edits: the tick (0: the next).
    Tick apply_before = 0;
    bool play = false;
    // A coalesce scope begins (label) or ends here instead.
    bool begin_coalesce = false;
    bool end_coalesce = false;
    std::string label;
  };

  // A save of a document in flight through the lane.
  struct PendingSave {
    Ticket ticket = 0;
    std::uint64_t revision = 0;
    std::string path;
    bool durable = false;
    bool recovery = false;
  };

  enum class Request : std::uint8_t {
    kNone,
    kPlay,
    kPlayGame,
    kPause,
    kResume,
    kStep,
    kStop,
  };

  enum class QuitPhase : std::uint8_t {
    kNone,
    // The prompt is open: Save, Don't Save, Cancel.
    kPrompt,
    // Saves submitted; the progress modal waits for them.
    kSaving,
    kReady,
  };

  // Frame start.
  void FeedLog(App& app) SE_REQUIRES(g_main);
  void TakeDialogs(App& app) SE_REQUIRES(g_main);
  void RunPlayRequest(App& app, std::uint64_t now_ns) SE_REQUIRES(g_main);
  void StartPlay(App& app, PlayKind kind) SE_REQUIRES(g_main);
  void StopPlay(App& app) SE_REQUIRES(g_main);
  void RunScript(App& app, std::uint64_t now_ns) SE_REQUIRES(g_main);
  // One script action; false when it waits (run it again next frame).
  bool RunAction(App& app, const ScriptAction& action) SE_REQUIRES(g_main);
  void ScriptFailure(const ScriptAction& action, std::string_view what);
  void Autosave(App& app, std::uint64_t now_ns) SE_REQUIRES(g_main);
  void OnLaneCompletion(const LaneCompletion& completion) SE_REQUIRES(g_main);

  // Documents.
  [[nodiscard]] std::expected<void, Error> OpenDocument(App& app,
                                                        const std::string& path,
                                                        bool offer_recovery)
      SE_REQUIRES(g_main);
  void RebuildDocumentStack() SE_REQUIRES(g_main);
  // Saves the document to |path| (its own when empty) through the lane.
  [[nodiscard]] std::expected<void, Error> SaveDocument(App& app,
                                                        const std::string& path,
                                                        bool durable)
      SE_REQUIRES(g_main);

  // The frame's UI.
  void DrawUi(App& app, const FrameReport& report) SE_REQUIRES(g_main);
  void DrawDockSpace() SE_REQUIRES(g_main);
  void DrawMenus(App& app) SE_REQUIRES(g_main);
  // The hotkeys the menus name (Ctrl+S, Ctrl+Z, Ctrl+Y, F5, Shift+F5,
  // F10), as ImGui global shortcuts: a focused text field keeps its own.
  void HandleShortcuts(App& app) SE_REQUIRES(g_main);
  // Edit > Undo and Redo (and their hotkeys).
  void UndoRedo(bool undo) SE_REQUIRES(g_main);
  void DrawModals(App& app) SE_REQUIRES(g_main);
  void DrawBuiltInPanels(const PanelContext& context) SE_REQUIRES(g_main);
  void FillStats(App& app, const FrameReport& report) SE_REQUIRES(g_main);
  void ApplyQueued() SE_REQUIRES(g_main);
  // The play World's RenderLists on the guarded path, the viewports.
  void BuildRender(App& app, const FrameReport& report, ExtensionFrame& frame)
      SE_REQUIRES(g_main);

  // Faults.
  void EnterFaulted(App& app, std::string where, std::string what)
      SE_REQUIRES(g_main);
  void SaveFaultFiles(App& app) SE_REQUIRES(g_main);

  // Submits |bytes| to |path| through the lane (Durable: Urgent class,
  // quit waits; else Background).
  Ticket SubmitWrite(App& app, std::string path, std::vector<std::byte> bytes,
                     bool durable) SE_REQUIRES(g_main);
  // The script's dialog thread (dialog-callback from-thread), joined once
  // it is done.
  void JoinScriptThread() noexcept;
  // Resolves the quit prompt: "save", "discard" or "cancel".
  void ResolveQuitPrompt(App& app, std::string_view answer) SE_REQUIRES(g_main);
  // Resolves the recovery offer: "accept" or "discard".
  void ResolveRecovery(App& app, std::string_view answer) SE_REQUIRES(g_main);

  EditorGame game_;
  EditorOptions options_;
  App* app_ = nullptr;

  // ImGui.
  std::unique_ptr<imgui::Context> imgui_;
  std::unique_ptr<imgui::ImGuiRenderer> imgui_renderer_;
  std::unique_ptr<imgui::HeadlessFrames> headless_;
  bool dock_seeded_ = false;
  std::string layout_path_;
  std::uint64_t layout_saved_ns_ = 0;

  // The document and its stack (one document state).
  std::unique_ptr<EditDocument> document_;
  GameStateFactory document_factory_;
  std::unique_ptr<GameStateStack> document_states_;
  std::uint64_t initial_hash_ = 0;
  AssetScope editor_scope_;

  // Play.
  std::unique_ptr<PlaySession> play_;
  Request request_ = Request::kNone;
  bool stop_pending_ = false;
  std::uint64_t play_count_ = 0;
  std::uint64_t play_start_document_hash_ = 0;
  // The play World's RenderLists of the frame (guarded builds).
  std::deque<RenderList> play_lists_;
  std::vector<std::vector<RenderLayer>> viewport_layers_;
  // A second render fault while faulted: the World is not drawn again.
  bool render_discarded_ = false;
  std::string fault_directory_;
  // 'Play game' and the view (no ImGui window) has the input.
  bool game_input_focus_ = false;
  // A document to open at the next frame start (a dialog's result).
  std::optional<std::string> pending_open_;

  // Panels and what they see.
  std::vector<std::unique_ptr<Panel>> panels_;
  std::unique_ptr<Commands> commands_;
  std::unique_ptr<PlayEditsImpl> play_edits_;
  std::unique_ptr<Viewports> viewports_;
  std::unique_ptr<Textures> textures_;
  std::vector<Queued> queued_;
  std::string last_edit_error_;
  EditorStats stats_;
  std::vector<std::uint64_t> phase_ns_;
  LogView log_;
  std::uint64_t log_serial_ = 0;
  bool show_demo_ = false;

  // Dialogs.
  std::shared_ptr<DialogMailbox> dialogs_;
  // Dialogs a script opened (dialog-open), oldest first: their SDL
  // userdata, answered by dialog-callback.
  std::deque<void*> script_dialogs_;
  // dialog-callback from-thread.
  struct ScriptThread;
  std::unique_ptr<ScriptThread> script_thread_;

  // Saves, autosave and recovery.
  AutosaveTimer autosave_;
  std::vector<PendingSave> saves_;
  std::uint64_t write_revision_ = 0;
  std::uint64_t last_input_ns_ = 0;
  // The last document change the autosave saw, and when it saw it.
  std::uint64_t seen_revision_ = 0;
  std::uint64_t last_change_ns_ = 0;
  std::vector<RecoveryFile> recovery_offer_;
  std::string save_error_;

  // Quit.
  QuitPhase quit_phase_ = QuitPhase::kNone;
  // The prompt's answer from a script ("save", "discard", "cancel").
  std::string quit_answer_;
  bool quit_cancel_ = false;

  // The longest UI work of the last 2 s (debug log).
  std::uint64_t timing_new_ = 0;
  std::uint64_t timing_ui_ = 0;
  std::uint64_t timing_end_ = 0;
  std::uint64_t timing_logged_ns_ = 0;

  // The 60 Hz preview.
  bool preview_60hz_ = false;
  std::uint64_t preview_frame_ns_ = 0;

  // The script.
  std::vector<ScriptAction> script_;
  std::size_t script_next_ = 0;
  bool script_done_ = true;
  std::uint32_t script_failures_ = 0;
  std::uint64_t wait_frames_ = 0;
  Tick wait_until_tick_ = 0;
  std::uint64_t wait_started_ns_ = 0;
  std::uint64_t wait_until_ns_ = 0;
  // Dialog results that arrived while a quit was pending.
  std::uint64_t dialogs_during_quit_ = 0;
  int steps_left_ = 0;
  bool stepping_ = false;
  // key: the chord (an ImGuiKeyChord) held since the last frame.
  int script_chord_ = 0;
  bool script_key_down_ = false;
  std::string recovery_answer_;
};

}  // namespace se::editor

#endif  // SE_EDITOR_EDITOR_H_
