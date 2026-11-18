// Copyright 2026 The simple-engine-2 Authors.
//
// The Editor's AppExtension hooks: boot, the frame (session, ticks,
// commands, the render request), faults and shutdown. The UI is in
// editor_ui.cc, the script, saves and the quit in editor_actions.cc.

#include "se_editor/editor.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_timer.h>
#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_assets/lane_jobs.h"
#include "se_core/assert.h"
#include "se_core/error.h"
#include "se_core/input_frame.h"
#include "se_core/log.h"
#include "se_core/schedule.h"
#include "se_core/thread_registry.h"
#include "se_core/thread_role.h"
#include "se_core/time.h"
#include "se_platform/fs.h"
#include "se_render/renderer_2d.h"
#include "se_render_core/render_list.h"
#include "se_runtime/app.h"
#include "se_runtime/app_config.h"
#include "se_runtime/asset_runtime.h"
#include "se_runtime/integration.h"
#include "src/editor_internal.h"
#include "src/play_fault_boundary.h"
#include "src/play_states.h"

namespace se::editor {
namespace {

// The editor's play input outside 'Play game' with the viewport focused:
// nothing held, nothing pressed.
class EmptyInput final : public InputSource {
 public:
  InputFrame Latch(Tick tick, std::uint64_t /*tick_window_end_ns*/,
                   bool /*last_tick_of_frame*/) override {
    InputFrame frame;
    frame.tick = tick;
    return frame;
  }
};

EmptyInput& NoInput() {
  static auto* const kInput = new EmptyInput();
  return *kInput;
}

constexpr std::uint64_t kLayoutSaveIntervalNs = 5'000'000'000;
constexpr float kHeadlessWidth = 1280.0F;
constexpr float kHeadlessHeight = 720.0F;

}  // namespace

std::string_view EditorModeName(EditorMode mode) noexcept {
  switch (mode) {
    case EditorMode::kEdit:
      return "Edit";
    case EditorMode::kPlaying:
      return "Playing";
    case EditorMode::kPaused:
      return "Paused";
    case EditorMode::kStepping:
      return "Stepping";
    case EditorMode::kFaulted:
      return "Faulted";
  }
  return "?";
}

// --- The panels' interfaces ---

// Edit-mode items record the active document and the panel being drawn:
// they apply to that document (ADR-118).
void Editor::Commands::Submit(std::unique_ptr<EditorCommand> command) {
  editor_->edits_.Submit(editor_->documents_.ActiveId(),
                         editor_->drawing_panel_, std::move(command));
}

void Editor::Commands::BeginCoalesce(std::string_view label) {
  editor_->edits_.BeginCoalesce(editor_->documents_.ActiveId(),
                                editor_->drawing_panel_, label);
}

void Editor::Commands::EndCoalesce() {
  editor_->edits_.EndCoalesce(editor_->documents_.ActiveId(),
                              editor_->drawing_panel_);
}

void Editor::Commands::Select(std::span<const Guid> guids) {
  if (EditDocument* document = editor_->Active()) {
    document->GetSelection().Set(guids);
  }
}

Tick Editor::PlayEditsImpl::NextTick() const noexcept {
  return editor_->play_ ? editor_->play_->NextTick() : 0;
}

void Editor::PlayEditsImpl::Submit(std::unique_ptr<EditorCommand> command,
                                   Tick apply_before) {
  if (command == nullptr) return;
  if (editor_->play_ && editor_->play_->Faulted()) {
    // Play.Faulted: the World is read-only; only Select is taken.
    SE_LOG_WARN("editor: '{}' dropped: the play World is faulted",
                command->Label());
    return;
  }
  Queued queued;
  queued.command = std::move(command);
  queued.apply_before = apply_before;
  editor_->queued_.push_back(std::move(queued));
}

void Editor::PlayEditsImpl::Select(std::span<const Guid> guids) {
  if (EditDocument* document = editor_->Active()) {
    document->GetSelection().Set(guids);
  }
}

void Editor::Viewports::ReportInput(bool focused, bool hovered) {
  AssertMainThread();
  focused_ = focused_ || focused;
  hovered_ = hovered_ || hovered;
  if (!editor_->drawing_panel_.empty() &&
      reporting_panel_ != editor_->drawing_panel_) {
    reporting_panel_ = std::string(editor_->drawing_panel_);
  }
}

ViewportTarget* Editor::Viewports::Viewport(std::string_view name) {
  AssertMainThread();
  Renderer2D* renderer =
      editor_->app_ != nullptr ? editor_->app_->Renderer() : nullptr;
  if (renderer == nullptr) return nullptr;
  for (Entry& entry : entries_) {
    if (entry.name == name) {
      entry.asked = true;
      return entry.target;
    }
  }
  auto made = renderer->CreateViewport(kInitialWidth, kInitialHeight);
  if (!made) {
    SE_LOG_ERROR("editor: viewport '{}': {}", name, made.error().message);
    return nullptr;
  }
  entries_.push_back(Entry{std::string(name), *made, true});
  SE_LOG_INFO(
      "editor: viewport '{}' created: the World is drawn into it and "
      "shown through TextureIds",
      name);
  return *made;
}

void Editor::Viewports::BeginFrame() {
  AssertMainThread();
  Renderer2D* renderer =
      editor_->app_ != nullptr ? editor_->app_->Renderer() : nullptr;
  std::erase_if(entries_, [renderer](Entry& entry) {
    AssertMainThread();
    if (entry.asked) {
      entry.asked = false;
      return false;
    }
    // Not asked for during the last frame: its texture goes now, at a
    // frame start, through the retire queue.
    if (renderer != nullptr) renderer->DestroyViewport(entry.target);
    return true;
  });
}

void Editor::Viewports::DestroyAll() {
  AssertMainThread();
  Renderer2D* renderer =
      editor_->app_ != nullptr ? editor_->app_->Renderer() : nullptr;
  if (renderer != nullptr) {
    for (Entry& entry : entries_) renderer->DestroyViewport(entry.target);
  }
  entries_.clear();
}

std::vector<ViewportTarget*> Editor::Viewports::Asked() const {
  std::vector<ViewportTarget*> asked;
  for (const Entry& entry : entries_) {
    if (entry.asked) asked.push_back(entry.target);
  }
  return asked;
}

ImTextureID Editor::Textures::Id(TextureHandle texture) {
  AssertMainThread();
  if (editor_->imgui_renderer_) {
    return editor_->imgui_renderer_->TextureId(texture);
  }
  // Headless: any id that is not invalid; nothing samples it.
  return static_cast<ImTextureID>(texture.index) + 1;
}

ImVec2 Editor::Textures::Size(TextureHandle texture) {
  AssertMainThread();
  if (editor_->imgui_renderer_) {
    return editor_->imgui_renderer_->TextureSize(texture);
  }
  return ImVec2(1.0F, 1.0F);
}

// --- The Editor ---

Editor::Editor(EditorGame game, EditorOptions options)
    : game_(std::move(game)),
      options_(std::move(options)),
      documents_(options_.autosave_quiet_ns),
      commands_(std::make_unique<Commands>(this)),
      play_edits_(std::make_unique<PlayEditsImpl>(this)),
      viewports_(std::make_unique<Viewports>(this)),
      textures_(std::make_unique<Textures>(this)) {}

Editor::~Editor() = default;

void Editor::AddPanel(std::unique_ptr<Panel> panel) {
  if (panel == nullptr) return;
  panels_.push_back(std::move(panel));
  panel_draws_.push_back(0);
  panel_frame_.push_back(0);
}

bool Editor::PanelDrewLastFrame(std::string_view name) const noexcept {
  for (std::size_t i = 0; i < panels_.size(); ++i) {
    if (panels_[i]->Name() == name) {
      return ui_frame_ > 0 && panel_frame_[i] == ui_frame_;
    }
  }
  return false;
}

std::uint64_t Editor::PanelDraws(std::string_view name) const noexcept {
  for (std::size_t i = 0; i < panels_.size(); ++i) {
    if (panels_[i]->Name() == name) return panel_draws_[i];
  }
  return 0;
}

EditorMode Editor::Mode() const noexcept {
  if (!play_) return EditorMode::kEdit;
  switch (play_->State()) {
    case PlayState::kPlaying:
      return EditorMode::kPlaying;
    case PlayState::kPaused:
      return EditorMode::kPaused;
    case PlayState::kStepping:
      return EditorMode::kStepping;
    case PlayState::kFaulted:
      return EditorMode::kFaulted;
  }
  return EditorMode::kEdit;
}

std::expected<void, Error> Editor::Boot(App& app) {
  AssertMainThread();
  app_ = &app;
  if (app.GameInstance() == nullptr || app.GamePool() == nullptr) {
    return Fail(ErrorCode::kInvalidArgument, "the editor needs the game");
  }
  // ImGui: the one context, its settings (the dock layout) from the pref
  // path.
  layout_path_ = app.Paths().pref + "editor-layout.ini";
  imgui::ContextConfig config;
  config.viewports = options_.viewports;
  if (auto layout = fs::ReadTextFile(layout_path_)) {
    config.settings = std::move(*layout);
  }
  auto context = imgui::Context::Create(config);
  if (!context) return std::unexpected(std::move(context.error()));
  imgui_ = std::move(*context);
  if (app.GetWindow() != nullptr && app.Gpu() != nullptr &&
      app.Renderer() != nullptr) {
    auto renderer = imgui::ImGuiRenderer::Create(
        *imgui_, *app.Gpu(), app.GetWindow()->Native(), *app.Renderer());
    if (!renderer) return std::unexpected(std::move(renderer.error()));
    imgui_renderer_ = std::move(*renderer);
  } else {
    headless_ = std::make_unique<imgui::HeadlessFrames>(*imgui_);
  }
  dialogs_ = DialogMailbox::Create();
  RegisterDocumentState(document_factory_);
  if (AssetRuntime* assets = app.Assets()) {
    // The textures and sounds stay loaded while sessions come and go.
    editor_scope_ = assets->Registry().CreateScope("editor");
    assets->RetainDefaults(editor_scope_);
    assets->SetForeignListener([this](const LaneCompletion& done) {
      // Lane completions are drained on main.
      AssertMainThread();
      OnLaneCompletion(done);
    });
  }
  const std::string scene = fs::Join(app.Paths().content_root, options_.scene);
  if (auto opened = OpenDocument(app, scene, /*offer_recovery=*/true);
      !opened) {
    return opened;
  }
  // No session runs yet: a dump taken now says so (ADR-111).
  BeginIdleHeader();
  if (!options_.script.empty()) {
    if (!kRuntimeTestHooks) {
      return Fail(ErrorCode::kInvalidArgument,
                  "--editor-script needs a build with the test hooks");
    }
    const std::string path =
        fs::Join(app.Paths().content_root, options_.script);
    auto text = fs::ReadTextFile(path);
    if (!text) return std::unexpected(std::move(text.error()));
    auto script = ParseScript(*text);
    if (!script) {
      Error error = std::move(script.error());
      error.path = path;
      return std::unexpected(std::move(error));
    }
    script_ = std::move(*script);
    script_done_ = false;
    SE_LOG_INFO("editor script: {} actions from {}", script_.size(), path);
  }
  SE_LOG_INFO(
      "editor: '{}' open ({} entities, hash {:016x}); ImGui {}, pacing {} "
      "latency {}",
      Active()->Desc().name, Active()->Get().EntityCount(), Active()->Hash(),
      imgui_renderer_ ? "on the window" : "headless",
      PacingModeName(app.Pacer().Mode()), app.Pacer().FrameLatency());
  return {};
}

bool Editor::OnEvent(App& /*app*/, const SDL_Event& event) {
  AssertMainThread();
  const bool input = IsInputEvent(event);
  if (input) last_input_ns_ = SDL_GetTicksNS();
  // ImGui sees every event (its key and mouse state stay whole); whether
  // the game sees it too is the view's business, not io.WantCapture*: the
  // Viewport panel is an ImGui window, so ImGui wants the mouse over it
  // and, with keyboard navigation on, the keyboard while it has the focus.
  if (imgui_renderer_) static_cast<void>(imgui_renderer_->ProcessEvent(event));
  if (!input) return false;
  if (!play_ || play_->Kind() != PlayKind::kPlayGame) return true;
  // 'Play game' (ADR-113). Headless there is no view: the game has it.
  if (!imgui_renderer_) return false;
  // A release always reaches the game, so a key let go after the focus
  // moved does not stay held (a release of nothing held is ignored).
  if (IsReleaseEvent(event)) return false;
  if (IsMouseEvent(event)) return !game_view_hovered_;
  return !game_view_focused_;
}

void Editor::LatchGameInput() {
  AssertMainThread();
  game_view_focused_ = viewports_->Focused();
  game_view_hovered_ = viewports_->Hovered();
  const bool play_game = play_ && play_->Kind() == PlayKind::kPlayGame;
  const bool has = play_game && (!imgui_renderer_ || game_view_focused_ ||
                                 game_view_hovered_);
  if (play_game && has != game_input_focus_) {
    SE_LOG_INFO("play game: the view {} the input (focused {}, hovered {})",
                has ? "has" : "lost", game_view_focused_, game_view_hovered_);
  }
  game_input_focus_ = has;
}

ExtensionSession Editor::BeginFrame(App& app, std::uint64_t now_ns) {
  AssertMainThread();
  ExtensionSession session;
  FeedLog(app);
  TakeDialogs(app);
  // The frame start: what releases Worlds or GPU resources.
  bool reset = false;
  if (stop_pending_) {
    StopPlay(app);
    reset = true;
  }
  viewports_->BeginFrame();
  // Hot reload: the file events of the lane's last completions (outside
  // Play; a Stop above lets the ones kept during Play run now).
  RunFileEvents();
  RunPendingOpen(app);
  RunScript(app, now_ns);
  const PlayState before = play_ ? play_->State() : PlayState::kPlaying;
  RunPlayRequest(app, now_ns);
  if (play_ && play_->State() != before) reset = true;
  CompletePlayBoundary(app);
  Autosave(app, now_ns);
  if (imgui_ && imgui_->WantSaveSettings() &&
      now_ns - layout_saved_ns_ >= kLayoutSaveIntervalNs) {
    layout_saved_ns_ = now_ns;
    const std::string layout = imgui_->SaveSettings();
    SubmitWrite(app, layout_path_, AsBytes(layout), /*durable=*/false);
  }

  if (play_) {
    session.states = &play_->States();
    session.running = play_->Running();
    session.editor_play = true;
    session.next_tick = play_->NextTick();
  } else {
    session.states = document_states_.get();
    session.next_tick = 0;
    // Idle only when nothing waits on frames: no script, no quit flow, no
    // autosave due soon, no modal open, no file read in flight.
    bool autosave_waiting = false;
    bool prompt = close_prompt_ != kNoDocument;
    for (const auto& entry : documents_.All()) {
      const EditDocument& document = *entry->document;
      autosave_waiting =
          autosave_waiting ||
          (document.Dirty() &&
           entry->autosave.SubmittedRevision() != document.Revision());
      prompt = prompt || entry->reload_prompt;
    }
    session.idle_allowed = script_done_ && quit_phase_ == QuitPhase::kNone &&
                           !autosave_waiting && recovery_offer_.empty() &&
                           saves_.empty() && reads_.empty() && !prompt;
  }
  session.reset_accumulator = reset;
  return session;
}

void Editor::CompletePlayBoundary(App& app) {
  if (!play_ || play_->Kind() != PlayKind::kPlayGame || play_->Faulted()) {
    return;
  }
  // 'Play game' (M13 integration, ADR-129): a state waiting at the content
  // gate is created at this boundary once its texts and assets are
  // resident; the App holds the frame's ticks while it waits.
  if (!play_->Waiting().empty()) {
    play_->CompleteBoundary();
    // A state whose scene did not load faulted the session.
    if (play_->Faulted()) {
      SaveFaultFiles(app);
      return;
    }
  }
  AssetRuntime* assets = app.Assets();
  if (assets == nullptr || assets->StateError() == nullptr) return;
  // A text that did not load: the state is never created, and the session
  // faults as a tick would (the editor goes on).
  const Error& error = *assets->StateError();
  PlayFault fault;
  fault.where = "content";
  fault.what = std::format("state '{}' cannot be created: {} ({}:{}:{})",
                           assets->StateErrorState(), error.message, error.path,
                           error.line, error.column);
  fault.tick = play_->LastTick();
  assets->ClearStateError();
  play_->Fault(std::move(fault));
  SaveFaultFiles(app);
}

ExtensionTicks Editor::TickFrame(App& app, const StepPlan& plan,
                                 InputSource& input, AudioSink& audio) {
  AssertMainThread();
  if (!play_) return ExtensionTicks{0, 0};
  InputSource& source = game_input_focus_ ? input : NoInput();
  const bool was_faulted = play_->Faulted();
  // The dump streams first: a hang or a crash inside a tick finds the
  // SessionLog as it is now.
  SyncPlayDump();
  const int ran = play_->TickFrame(plan, source, audio, SDL_GetTicksNS());
  PublishPlayTicks();
  if (!was_faulted && play_->Faulted()) SaveFaultFiles(app);
  return ExtensionTicks{ran, play_->NextTick()};
}

void Editor::BuildFrame(App& app, const FrameReport& report,
                        ExtensionFrame& frame) {
  AssertMainThread();
  if (app.Renderer() != nullptr) {
    // The release log of this frame: ImGui may bind none of them.
    app.Renderer()->Resources().MarkFrameStart();
  }
  const double delta =
      report.clock.real_delta > 0.0 ? report.clock.real_delta : 1.0 / 60.0;
  const std::uint64_t t0 = SDL_GetTicksNS();
  if (imgui_renderer_) {
    imgui_renderer_->NewFrame(delta);
  } else {
    headless_->NewFrame(kHeadlessWidth, kHeadlessHeight, delta);
  }
  const std::uint64_t t1 = SDL_GetTicksNS();
  FillStats(app, report);
  DrawUi(app, report);
  const std::uint64_t t2 = SDL_GetTicksNS();
  if (imgui_renderer_) {
    imgui_renderer_->Render();
  } else {
    headless_->Render();
  }
  // After ImGui::Render and before the RenderList build: this frame's
  // edits show in this frame.
  ApplyQueued();
  BuildRender(app, report, frame);
  frame.overlay = imgui_renderer_.get();
  const std::uint64_t t3 = SDL_GetTicksNS();
  timing_new_ = std::max(timing_new_, t1 - t0);
  timing_ui_ = std::max(timing_ui_, t2 - t1);
  timing_end_ = std::max(timing_end_, t3 - t2);
  if (t3 - timing_logged_ns_ >= 2'000'000'000) {
    timing_logged_ns_ = t3;
    SE_LOG_DEBUG(
        "editor frame work, the longest of 2 s: ImGui new frame {:.3f} ms, "
        "UI {:.3f} ms, render, commands and lists {:.3f} ms",
        static_cast<double>(timing_new_) * 1e-6,
        static_cast<double>(timing_ui_) * 1e-6,
        static_cast<double>(timing_end_) * 1e-6);
    timing_new_ = timing_ui_ = timing_end_ = 0;
  }
}

void Editor::ApplyQueued() {
  std::vector<Queued> queued = std::move(queued_);
  queued_.clear();
  for (Queued& item : queued) {
    if (!play_) continue;
    if (auto edited = play_->Edit(*item.command, item.apply_before); !edited) {
      SE_LOG_ERROR("play edit '{}': {}", item.command->Label(),
                   edited.error().message);
    } else {
      SE_LOG_INFO("play edit '{}' queued for tick {}", item.command->Label(),
                  std::max(item.apply_before, play_->NextTick()));
    }
  }
  if (play_) {
    // The documents are frozen during Play.
    edits_.DropAll("the document is not editable during Play");
    return;
  }
  const InlineOwnerScope owner(OnMainThread());
  const EditQueue::Applied applied = edits_.Apply(
      documents_,
      [this](std::string_view panel) { return PanelDrewLastFrame(panel); });
  if (applied.failed > 0) last_edit_error_ = applied.error;
}

void Editor::BuildRender(App& app, const FrameReport& report,
                         ExtensionFrame& frame) {
  std::vector<RenderLayer>& layers = *frame.layers;
  const std::vector<ViewportTarget*> asked = viewports_->Asked();
  // The play World's RenderLists are built here, on the guarded path, so a
  // throw in a game's render data is Play.Faulted, not a dead editor.
  if (play_) {
    std::size_t used = 0;
    auto guarded = [&](const World& world, const Camera2D& camera,
                       float alpha) -> const RenderList* {
      AssertMainThread();
      if (play_lists_.size() <= used) play_lists_.resize(used + 1);
      RenderList& list = play_lists_[used++];
      const bool was_faulted = play_->Faulted();
      if (play_->BuildRenderList(world, camera, alpha, list)) return &list;
      if (!was_faulted) {
        SaveFaultFiles(app);
      } else {
        // A second render exception: the World is not drawn again (the
        // panels still read it until Stop).
        render_discarded_ = true;
      }
      return nullptr;
    };
    const bool still = play_->DrawsStill();
    std::vector<RenderLayer> built;
    bool failed = render_discarded_;
    for (const RenderLayer& layer : layers) {
      if (failed || layer.world == nullptr) continue;
      RenderLayer next = layer;
      // Paused, stepping and faulted Worlds draw at alpha 1, as the
      // inspector shows them; so does a resumed one until its next tick
      // (the design's prev := current at Pause).
      if (still) next.alpha = 1.0F;
      next.list = guarded(*layer.world, next.camera, next.alpha);
      if (next.list == nullptr) {
        failed = true;
        continue;
      }
      built.push_back(next);
    }
    layers = failed ? std::vector<RenderLayer>() : std::move(built);
    // Viewports see the World through their own cameras.
    if (!failed && !asked.empty()) {
      viewport_layers_.resize(asked.size());
      for (std::size_t v = 0; v < asked.size(); ++v) {
        viewport_layers_[v].clear();
        for (const RenderLayer& layer : layers) {
          RenderLayer seen = layer;
          seen.camera = asked[v]->Camera();
          seen.list = guarded(*layer.world, seen.camera, seen.alpha);
          if (seen.list == nullptr) break;
          viewport_layers_[v].push_back(seen);
        }
      }
    }
  } else if (!asked.empty()) {
    viewport_layers_.resize(asked.size());
    for (std::size_t v = 0; v < asked.size(); ++v) {
      viewport_layers_[v] = layers;
      for (RenderLayer& layer : viewport_layers_[v]) {
        layer.camera = asked[v]->Camera();
      }
    }
  }
  if (!asked.empty()) {
    // The World goes into the viewports; the composite shows the clear
    // colour behind the UI.
    frame.viewports->clear();
    for (std::size_t v = 0; v < asked.size(); ++v) {
      if (v >= viewport_layers_.size()) break;
      frame.viewports->push_back(ViewportDraw{asked[v], viewport_layers_[v]});
    }
    layers.clear();
    // The readback hooks read the World where it is drawn.
    if (!frame.viewports->empty()) frame.readback = asked.front();
  }
  viewport_drawn_ = !frame.viewports->empty();
  static_cast<void>(report);
}

bool Editor::OnRenderFault(App& app, const Error& error) {
  AssertMainThread();
  if (!play_) return false;
  if (!play_->Faulted()) {
    EnterFaulted(app, "render", error.message);
  } else {
    render_discarded_ = true;
  }
  return true;
}

void Editor::EndFrame(App& /*app*/, const FrameReport& /*report*/) {
  AssertMainThread();
  if (imgui_renderer_) imgui_renderer_->RenderPlatformWindows();
  if (!preview_60hz_) {
    preview_frame_ns_ = 0;
    return;
  }
  // The 60 Hz preview: no frame starts sooner than 1/60 s after the last.
  constexpr std::uint64_t kPreviewPeriodNs = 16'666'667;
  const std::uint64_t now = SDL_GetTicksNS();
  if (preview_frame_ns_ != 0 && now < preview_frame_ns_ + kPreviewPeriodNs) {
    SDL_DelayNS(preview_frame_ns_ + kPreviewPeriodNs - now);
  }
  preview_frame_ns_ = SDL_GetTicksNS();
}

void Editor::OnDeviceLost(App& /*app*/) {
  AssertMainThread();
  if (imgui_renderer_) imgui_renderer_->ReleaseDevice();
}

void Editor::OnDeviceRecreated(App& app) {
  AssertMainThread();
  if (imgui_renderer_ && app.Gpu() != nullptr) {
    if (auto made = imgui_renderer_->RecreateDevice(*app.Gpu()); !made) {
      SE_LOG_ERROR("editor: ImGui on the new device: {}", made.error().message);
    }
  }
}

void Editor::ShutdownWorlds(App& app) {
  AssertMainThread();
  JoinScriptThread();
  if (dialogs_) dialogs_->Close();
  if (play_) DetachPlayDump();
  play_.reset();
  if (document_states_) {
    const InlineOwnerScope owner(OnMainThread());
    document_states_->Clear();
  }
  document_states_.reset();
  stack_document_ = kNoDocument;
  documents_.SetFrozen(false);
  documents_.Clear();
  viewports_->DestroyAll();
  if (AssetRuntime* assets = app.Assets()) {
    assets->SetForeignListener({});
    if (!editor_scope_.IsNull()) {
      assets->Registry().ReleaseScope(editor_scope_);
      editor_scope_ = AssetScope{};
    }
  }
  if (!script_.empty() && !script_done_) {
    // The quit ended the run before the script's last action.
    if (script_failures_ == 0) {
      SE_LOG_INFO("editor script: passed ({} of {} actions run at the quit)",
                  script_next_, script_.size());
    } else {
      SE_LOG_ERROR("editor script: FAILED ({} failures; {} of {} actions)",
                   script_failures_, script_next_, script_.size());
    }
  }
  SE_LOG_INFO(
      "editor at shutdown: {} plays, {} script failures, {} dialog callbacks "
      "({} answered during the quit, {} after the editor closed them)",
      play_count_, script_failures_, dialogs_ ? dialogs_->Delivered() : 0,
      dialogs_during_quit_, dialogs_ ? dialogs_->DeliveredAfterClose() : 0);
}

void Editor::ShutdownUi(App& /*app*/) {
  AssertMainThread();
  if (imgui_renderer_) {
    const imgui::RendererStats& stats = imgui_renderer_->Stats();
    SE_LOG_INFO(
        "ImGui at shutdown: {} frames, {} texture updates, {} vertex and {} "
        "index growths, at most {} vertices and {} indices, {} bound textures "
        "checked",
        stats.frames, stats.texture_updates, stats.vertex_growths,
        stats.index_growths, stats.max_vertices, stats.max_indices,
        stats.checked_textures);
    imgui_renderer_->Shutdown();
    imgui_renderer_.reset();
  }
  headless_.reset();
  imgui_.reset();
}

void Editor::FeedLog(App& app) {
  integration::Logging* logs = app.Logs();
  if (logs == nullptr) return;
  std::vector<std::string> lines;
  if (!logs->TryCopyPanelLinesSince(log_serial_, lines)) return;
  for (std::string& line : lines) log_.Append(std::move(line));
  const LogWorkerStats stats = logs->WorkerStats();
  log_.SetCounts(stats.dropped, stats.suppressed);
}

void Editor::FillStats(App& app, const FrameReport& report) {
  stats_ = EditorStats{};
  stats_.frame = report.iterate;
  stats_.next_tick = play_ ? play_->NextTick() : 0;
  stats_.tick = play_ ? play_->LastTick() : 0;
  const FramePacer& pacer = app.Pacer();
  stats_.pacing_mode = pacer.Mode();
  stats_.pacing_reason = pacer.ModeReason();
  stats_.present_interval = pacer.PresentInterval();
  stats_.frame_latency = pacer.FrameLatency();
  stats_.half_rate = pacer.HalfRate();
  stats_.display_hz = pacer.DisplayHz();
  stats_.pacer = &pacer.LastFrame();
  stats_.pacer_totals = &pacer.Totals();
  stats_.plan = report.plan;
  stats_.stepper = &app.Stepper().Stats();
  stats_.editor_play = play_ != nullptr;
  stats_.preview_60hz = preview_60hz_;
  stats_.idle = app.GetLifecycle().Idle();
  if (ModuleInstance* instance = app.GameInstance()) {
    const std::span<const SystemStats> systems =
        instance->MutableSchedule().Stats();
    stats_.systems = systems;
    phase_ns_.assign(kPhaseCount, 0);
    for (const SystemStats& system : systems) {
      phase_ns_[static_cast<std::size_t>(system.phase)] += system.last_ns;
    }
    stats_.phase_ns = phase_ns_;
  }
  if (Renderer2D* renderer = app.Renderer()) {
    stats_.render = &renderer->LastFrame();
    stats_.render_totals = &renderer->Totals();
  }
  FillForkStats(app);
}

void Editor::EnterFaulted(App& app, std::string where, std::string what) {
  if (!play_ || play_->Faulted()) return;
  PlayFault fault;
  fault.where = std::move(where);
  fault.what = std::move(what);
  fault.tick = play_->LastTick();
  fault.breadcrumb = ThreadBreadcrumb().Fields();
  play_->Fault(std::move(fault));
  SaveFaultFiles(app);
}

void Editor::SaveFaultFiles(App& app) {
  if (!play_) return;
  const PlayFault& fault = play_->FaultInfo();
  const std::string directory = std::format(
      "{}faults/play-{}-tick-{}/", app.Paths().pref, play_count_, fault.tick);
  if (auto made = fs::CreateDirectories(directory); !made) {
    SE_LOG_ERROR("Play.Faulted: {}: {}", directory, made.error().message);
    return;
  }
  // The replay (the Play-start bytes, every tick's input and hash), the
  // SessionLog, the breadcrumbs and what happened, through the lane.
  std::vector<std::byte> replay = play_->FinishReplay();
  SubmitWrite(app, directory + "replay.sereplay", std::move(replay),
              /*durable=*/true);
  SubmitWrite(app, directory + "session.selog", play_->Log().Write(),
              /*durable=*/true);
  // A copy by atomic loads: the other threads keep writing their
  // breadcrumbs (copying the live stream's bytes races with them).
  SubmitWrite(app, directory + "breadcrumbs.bin",
              ThreadRegistry::Instance().CopyBreadcrumbStream(),
              /*durable=*/true);
  const std::string text = std::format(
      "where: {}\ntick: {}\nsystem: {}\nphase: {}\nwhat: {}\n", fault.where,
      fault.tick, fault.system.empty() ? std::string("-") : fault.system,
      static_cast<int>(fault.breadcrumb.phase), fault.what);
  SubmitWrite(app, directory + "fault.txt", AsBytes(text), /*durable=*/true);
  fault_directory_ = directory;
  integration::RequestFaultDump("editor Play.Faulted");
  SE_LOG_WARN("Play.Faulted: replay, session log and breadcrumbs go to {}",
              directory);
}

Ticket Editor::SubmitWrite(App& app, std::string path,
                           std::vector<std::byte> bytes, bool durable) {
  AssetRuntime* assets = app.Assets();
  if (assets == nullptr) return 0;
  if (auto made = fs::CreateDirectories(fs::Parent(path)); !made) {
    SE_LOG_ERROR("editor: {}: {}", path, made.error().message);
    return 0;
  }
  SaveRequest request;
  request.path = std::move(path);
  request.bytes = std::move(bytes);
  request.revision = ++write_revision_;
  request.durable = durable;
  return SubmitSave(assets->Lane(), std::move(request),
                    durable ? JobClass::kUrgent : JobClass::kBackground);
}

}  // namespace se::editor
