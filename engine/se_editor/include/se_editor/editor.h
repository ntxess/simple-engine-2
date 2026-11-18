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
// Quit (QuitProvider): waits while a file dialog is open; a prompt lists
// every dirty document (Save All, Don't Save, Cancel) and runs over normal
// frames; saves go through the lane as Durable jobs with a progress modal;
// a failed save keeps its document dirty, shows its error and cancels the
// quit (the others are saved). The recovery hook writes every dirty
// document's recovery file synchronously.
//
// Documents (M11b; ADR-117 to ADR-121): a DocumentSet of scenes and
// prefabs, one active. A command records the document it was submitted
// for and applies to that one (dropped, logged, when it was closed); a
// coalesce scope belongs to its document and to the panel that opened it,
// and ends at the frame's apply step when that panel did not draw, when
// its document is closed or deactivated, and at Play. Each document
// watches its file and the prefab files its load read; a change is read on
// the lane and, outside Play, rebased into the open scenes (one undo step
// per document) or reloaded (a clean document; a dirty one asks).

#ifndef SE_EDITOR_EDITOR_H_
#define SE_EDITOR_EDITOR_H_

#include <SDL3/SDL_events.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <map>
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
#include "se_editor/documents.h"
#include "se_editor/edit_document.h"
#include "se_editor/edit_queue.h"
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
#include "se_runtime/integration.h"
#include "se_runtime/quit.h"
#include "se_sim/game_state.h"

namespace se::editor {

// Game-aware script verbs the editor executable provides (stat-name,
// expect-stat-name and stress-press: se_editor_panels' commands).
struct ScriptHooks {
  // The Stats drawer's new-stat edit: a CommandGroup {StatNameCommand,
  // SetComponentCommand} adding stat |name| = |value| to |guid|.
  std::function<std::expected<std::unique_ptr<EditorCommand>, Error>(
      const World& world, Guid guid, std::string_view name, float value)>
      stat_name_edit;
  // Whether |world|'s stat names hold |name|.
  std::function<bool(const World& world, std::string_view name)> has_stat_name;
  // The Stress generator's Edit-mode press of |count| enemies (one
  // CommandGroup of prefab instances; stress-press).
  std::function<std::unique_ptr<EditorCommand>(const World& world,
                                               std::uint32_t count)>
      stress_press;
};

struct EditorOptions {
  // The document opened at boot: a scene (or prefab) file relative to the
  // content root (or absolute).
  std::string scene;
  // --editor-script FILE (test builds): actions at frame boundaries.
  std::string script;
  // ImGui multi-viewports (opt-in).
  bool viewports = false;
  std::uint64_t autosave_quiet_ns = kAutosaveQuietNs;
  ScriptHooks script_hooks;
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
  // The active document.
  [[nodiscard]] const EditDocument* Document() const noexcept {
    return documents_.ActiveDocument();
  }
  [[nodiscard]] const DocumentSet& Documents() const noexcept {
    return documents_;
  }
  [[nodiscard]] const PlaySession* Play() const noexcept { return play_.get(); }
  [[nodiscard]] const LogView& Log() const noexcept { return log_; }
  [[nodiscard]] std::uint32_t ScriptFailures() const noexcept {
    return script_failures_;
  }
  [[nodiscard]] bool ScriptDone() const noexcept { return script_done_; }
  // The last frame's stats, as the panels saw them (their spans and
  // pointers are valid until the next frame).
  [[nodiscard]] const EditorStats& Stats() const noexcept { return stats_; }
  // The Play-start dump stream is registered (Play runs).
  [[nodiscard]] bool PlayDumpAttached() const noexcept {
    return play_stream_.Attached();
  }
  // How often each panel drew (its window was visible), by Name(), and
  // whether it drew in the last frame.
  [[nodiscard]] std::uint64_t PanelDraws(std::string_view name) const noexcept;
  [[nodiscard]] bool PanelDrewLastFrame(std::string_view name) const noexcept;

 private:
  class Commands;
  class PlayEditsImpl;
  class Viewports;
  class Textures;

  // A Play edit waiting for the frame's apply step (after ImGui::Render);
  // Edit-mode commands wait in edits_ (EditQueue).
  struct Queued {
    std::unique_ptr<EditorCommand> command;
    // The tick (0: the next).
    Tick apply_before = 0;
  };

  // A save of a document in flight through the lane.
  struct PendingSave {
    Ticket ticket = 0;
    std::uint64_t revision = 0;
    std::string path;
    bool durable = false;
    bool recovery = false;
    DocumentId document = kNoDocument;
    // The file's text as submitted (a save, not a recovery file).
    std::string text;
  };

  // A watched file's change, or its text once read or saved: queued by the
  // lane's completions, handled at a frame start outside Play.
  struct FileEvent {
    std::string path;
    bool read = false;
    std::string text;
    // The read made when the file started being watched (see Watch).
    bool catch_up = false;
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
  // Play (F5) or Resume on a session; logs whether the state changed.
  void ResumePlay() SE_REQUIRES(g_main);
  void StopPlay(App& app) SE_REQUIRES(g_main);
  void RunScript(App& app, std::uint64_t now_ns) SE_REQUIRES(g_main);
  // One script action; false when it waits (run it again next frame).
  bool RunAction(App& app, const ScriptAction& action) SE_REQUIRES(g_main);
  // The M11b verbs; |handled| is false for any other.
  bool RunDocumentAction(App& app, const ScriptAction& action, bool& handled)
      SE_REQUIRES(g_main);
  void ScriptFailure(const ScriptAction& action, std::string_view what);
  void Autosave(App& app, std::uint64_t now_ns) SE_REQUIRES(g_main);
  void OnLaneCompletion(const LaneCompletion& completion) SE_REQUIRES(g_main);

  // Documents (editor_documents.cc).
  // Opens |path| as a new document, or activates the one open on it.
  [[nodiscard]] std::expected<void, Error> OpenDocument(App& app,
                                                        const std::string& path,
                                                        bool offer_recovery)
      SE_REQUIRES(g_main);
  // Loads |text| as a document of |path|: its kind by the text's "format".
  [[nodiscard]] std::expected<std::unique_ptr<EditDocument>, Error>
  LoadDocument(App& app, const std::string& path, std::string_view text)
      SE_REQUIRES(g_main);
  // The document stack shows the active document (rebuilt when it
  // changed).
  void SyncDocumentStack() SE_REQUIRES(g_main);
  void DropDocumentStack() SE_REQUIRES(g_main);
  // Activates |id| (logged; the open scope of the old one ends).
  bool ActivateDocument(DocumentId id) SE_REQUIRES(g_main);
  // Closes |id| at once (no prompt).
  void CloseDocumentNow(DocumentId id) SE_REQUIRES(g_main);
  // Replaces |id| with |document| (a reload or a recovery).
  DocumentId ReplaceDocument(DocumentId id,
                             std::unique_ptr<EditDocument> document)
      SE_REQUIRES(g_main);
  // Saves document |id| to |path| (its own when empty) through the lane.
  [[nodiscard]] std::expected<void, Error> SaveDocument(App& app, DocumentId id,
                                                        const std::string& path,
                                                        bool durable)
      SE_REQUIRES(g_main);
  // Every dirty document; false when one could not be submitted.
  bool SaveAll(App& app) SE_REQUIRES(g_main);
  // The active document (null when none).
  [[nodiscard]] EditDocument* Active() noexcept {
    return documents_.ActiveDocument();
  }
  [[nodiscard]] const EditDocument* Active() const noexcept {
    return documents_.ActiveDocument();
  }
  void OfferRecovery(App& app, DocumentId id) SE_REQUIRES(g_main);
  void SaveCompleted(const PendingSave& save) SE_REQUIRES(g_main);
  [[nodiscard]] RecoveryFile RecoveryOf(const EditDocument& document,
                                        std::string text) const;

  // Hot reload (editor_reload.cc).
  // The files a document watches: its own and the prefabs its load read.
  [[nodiscard]] std::vector<std::string> WatchedPaths(
      const EditDocument& document) const;
  void Watch(const EditDocument& document, bool watch) SE_REQUIRES(g_main);
  void OnFileChanged(const FileChange& change) SE_REQUIRES(g_main);
  // A read counts as a handled change only when the file differs from
  // what the documents hold. |catch_up|: the read made when the file
  // started being watched (logged when it found a change).
  void OnFileRead(const std::string& path, std::string text,
                  bool catch_up = false) SE_REQUIRES(g_main);
  // A prefab's file now holds |text|: every open scene holding instances
  // of it gets one undo step "Prefab changed: <ref>". Returns whether any
  // open document recorded another text for it.
  bool RebasePrefab(const std::string& ref, const std::string& text)
      SE_REQUIRES(g_main);
  // The reference of |path| under the asset root ("" when not under it).
  [[nodiscard]] std::string RefOf(std::string_view path) const;
  // Resolves the reload prompt of the first document that has one:
  // "reload" or "keep".
  void ResolveReloadPrompt(App& app, std::string_view answer)
      SE_REQUIRES(g_main);
  // The frame start, outside Play: the file events the lane's completions
  // queued (a change to read, a text read or saved), in order. A document
  // is reloaded only here, never inside the lane's drain (the App still
  // holds the frame's stack then).
  void RunFileEvents() SE_REQUIRES(g_main);

  // The frame's UI.
  void DrawUi(App& app, const FrameReport& report) SE_REQUIRES(g_main);
  // After the panels: whether the next frame's input is the game's.
  void LatchGameInput() SE_REQUIRES(g_main);
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
  // The pool's fork sites and each phase's parallel time (editor_dump.cc).
  void FillForkStats(App& app) SE_REQUIRES(g_main);

  // Dumps (editor_dump.cc; ADR-111). The session header says that no
  // session runs (Edit mode).
  void BeginIdleHeader() SE_REQUIRES(g_main);
  // Play started: the session header describes it, its InputFrames go into
  // the dump ring and the Play-start stream holds its replay file without
  // frames.
  void AttachPlayDump() SE_REQUIRES(g_main);
  // The Play-start stream again when the SessionLog gained an entry.
  void SyncPlayDump() SE_REQUIRES(g_main);
  // The frame's last tick hash into the session header.
  void PublishPlayTicks() SE_REQUIRES(g_main);
  void DetachPlayDump() SE_REQUIRES(g_main);
  void ApplyQueued() SE_REQUIRES(g_main);
  // The play World's RenderLists on the guarded path, the viewports.
  void BuildRender(App& app, const FrameReport& report, ExtensionFrame& frame)
      SE_REQUIRES(g_main);

  // Faults.
  void EnterFaulted(App& app, std::string where, std::string what)
      SE_REQUIRES(g_main);
  void SaveFaultFiles(App& app) SE_REQUIRES(g_main);
  // 'Play game': the content gate's boundary at frame start, and a state
  // whose text did not load faults the session.
  void CompletePlayBoundary(App& app) SE_REQUIRES(g_main);

  // Submits |bytes| to |path| through the lane (Durable: Urgent class,
  // quit waits; else Background).
  Ticket SubmitWrite(App& app, std::string path, std::vector<std::byte> bytes,
                     bool durable) SE_REQUIRES(g_main);
  // The script's dialog thread (dialog-callback from-thread), joined once
  // it is done.
  void JoinScriptThread() noexcept;
  // Resolves the quit prompt: "save", "discard" or "cancel".
  void ResolveQuitPrompt(App& app, std::string_view answer) SE_REQUIRES(g_main);
  // File > Close: a dirty document asks first (the close prompt). "save"
  // saves it and closes once the save completed, "discard" closes at
  // once, "cancel" keeps the document.
  void RequestClose(App& app, DocumentId id) SE_REQUIRES(g_main);
  void ResolveClosePrompt(App& app, std::string_view answer)
      SE_REQUIRES(g_main);
  // The frame start: a dialog's open, or a close waiting for its save.
  void RunPendingOpen(App& app) SE_REQUIRES(g_main);
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

  // The documents, and the stack of the active one (one document state).
  DocumentSet documents_;
  GameStateFactory document_factory_;
  std::unique_ptr<GameStateStack> document_states_;
  // The document the stack shows.
  DocumentId stack_document_ = kNoDocument;
  // The frame's Edit-mode commands and the open coalesce scope.
  EditQueue edits_;
  AssetScope editor_scope_;

  // Play.
  std::unique_ptr<PlaySession> play_;
  Request request_ = Request::kNone;
  bool stop_pending_ = false;
  std::uint64_t play_count_ = 0;
  std::uint64_t play_start_document_hash_ = 0;
  std::uint64_t play_start_content_hash_ = 0;
  // The document Play plays (the set is frozen meanwhile).
  DocumentId play_document_ = kNoDocument;
  // The play World's RenderLists of the frame (guarded builds).
  std::deque<RenderList> play_lists_;
  std::vector<std::vector<RenderLayer>> viewport_layers_;
  // A second render fault while faulted: the World is not drawn again.
  bool render_discarded_ = false;
  // The last frame drew the World into a panel's viewport.
  bool viewport_drawn_ = false;
  std::string fault_directory_;
  // 'Play game' and the view has the input: the frame's ticks latch the
  // game's InputService (ADR-113). Set from the last UI frame's reports.
  bool game_input_focus_ = false;
  // The last UI frame's view reports (EditorViewports::ReportInput).
  bool game_view_focused_ = false;
  bool game_view_hovered_ = false;
  // The panel being drawn (its Name()), for the view's report.
  std::string_view drawing_panel_;
  // A document to open at the next frame start (a dialog's result).
  std::optional<std::string> pending_open_;

  // Dumps: the Play-start stream while Play runs, and the SessionLog
  // entries it was attached with.
  static constexpr std::size_t kNoPlayStream = ~std::size_t{0};
  integration::PlayStartStream play_stream_;
  std::size_t play_stream_entries_ = kNoPlayStream;

  // Panels and what they see.
  std::vector<std::unique_ptr<Panel>> panels_;
  std::vector<std::uint64_t> panel_draws_;
  // The UI frame each panel last drew in, and the UI frames so far.
  std::vector<std::uint64_t> panel_frame_;
  std::uint64_t ui_frame_ = 0;
  // Windows to bring to the front, one a frame (a dock node selects the
  // tab of the window focused when the frame ends): a seeded layout's
  // first tabs, and the script's show-panel. focus_delay_ frames pass
  // first (a seeded layout's windows exist by then).
  std::deque<std::string> focus_queue_;
  int focus_delay_ = 0;
  std::unique_ptr<Commands> commands_;
  std::unique_ptr<PlayEditsImpl> play_edits_;
  std::unique_ptr<Viewports> viewports_;
  std::unique_ptr<Textures> textures_;
  std::vector<Queued> queued_;
  std::string last_edit_error_;
  EditorStats stats_;
  std::vector<std::uint64_t> phase_ns_;
  std::vector<std::uint64_t> phase_parallel_ns_;
  std::vector<ForkSiteStats> fork_sites_;
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
  std::vector<PendingSave> saves_;
  std::uint64_t write_revision_ = 0;
  std::uint64_t last_input_ns_ = 0;
  std::vector<RecoveryFile> recovery_offer_;
  // The document the recovery offer is about.
  DocumentId recovery_document_ = kNoDocument;
  // The last save error of any document (the Document panel shows it).
  std::string save_error_;

  // Hot reload: how many documents watch each path, the reads in flight,
  // the queued file events, and the changes handled so far.
  std::map<std::string, int, std::less<>> watch_refs_;
  std::vector<std::pair<Ticket, std::string>> reads_;
  // The reads among reads_ made when their file started being watched.
  std::vector<Ticket> catch_up_reads_;
  std::deque<FileEvent> file_events_;
  std::uint64_t file_events_handled_ = 0;
  std::uint64_t wait_reload_base_ = 0;
  bool waiting_reload_ = false;
  std::string reload_answer_;

  // Quit.
  QuitPhase quit_phase_ = QuitPhase::kNone;
  // The prompt's answer from a script ("save", "discard", "cancel").
  std::string quit_answer_;
  bool quit_cancel_ = false;

  // File > Close of a dirty document: the document waiting for the
  // prompt's answer, and the one to close once its save completed.
  DocumentId close_prompt_ = kNoDocument;
  DocumentId close_after_save_ = kNoDocument;
  // The prompt's answer from a script.
  std::string close_answer_;

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
