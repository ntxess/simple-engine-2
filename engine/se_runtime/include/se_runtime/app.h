// Copyright 2026 The simple-engine-2 Authors.
//
// The App: one game running on SDL's main callbacks (docs/milestone-specs.md
// M5, app.h; docs/design/mt-design.md "Lifecycle", "Shutdown Order" and the
// API sketch's app.cpp). Main thread only.
//
// App::Create runs the Booting steps in the design's order, each undone by
// RAII when a later one fails:
//    1 crash-handler   the pref path, then crash capture with its database
//                      under it and the dump streams (integration.h)
//    2 logging         the log worker, its file under the pref path; then
//                      the profiler with --profile
//    3 sdl             SDL_Init(VIDEO | GAMEPAD), SDL's log routed to SE_LOG
//    4 event-filter    the event filter and Inbox; the wake event
//    5 content-paths   the content root
//    6 window          the window and its Win32 subclass
//    7 gpu-device      the SDL_GPU device, claiming the window with VSYNC;
//                      Renderer2D; the game's texture table is read (its
//                      textures load through the lane, M9)
//    8 cpu-topology    the CPU topology and the pool sizes it gives
//    9 helper-threads  the vblank sampler (Windows, a window), then the
//                      watchdog (boot budget until the first iterate), then
//                      the background lane and the AssetRegistry (M9,
//                      asset_runtime.h); the sound table is read (its
//                      files are every state's default sounds)
//   10 game            the settings file; the audio engine (M8: miniaudio,
//                      the null backend when no device opens, --audio) and
//                      the lane's sound decoder at its format (ADR-087);
//                      the GameModule's registry and storages, the
//                      GameStateStack with the start state pushed (its
//                      AssetScope requests its textures and sounds, which
//                      the lane loads and decodes), the SimDriver writing
//                      the process's InputFrame ring and asking the assets'
//                      TickGate before each tick
// The main thread runs inside its RoleScope from SDL_AppInit to the end of
// SDL_AppQuit (integration::EnterMainThread).
// --headless skips the window and the GPU device (steps 6 and 7 do
// nothing) and uses SDL's dummy video driver.
//
// Each iterate: count the iterate (the watchdog's progress), take the
// inbox, drain the background lane's completions (M9: pages to the upload
// queue, reloads, the scopes of the stack's states), poll the two-phase
// quit (the lane's QuitProvider waits for Durable jobs), update the lifecycle
// (callback rate, held input, frame-clock reset), FramePacer::BeginFrame (the
// N-L fence, present statistics, the FrameClock), plan the frame's ticks with
// the FixedStepper (none while the top state's AssetScope is not resident: the
// sim holds), tick the running Worlds through SimDriver::TickInline with
// the InputService's frames (M8: stamped edges placed on the plan's tick
// windows, the ActionMap from the settings file), then render: the
// GameStateStack's render sources (every state's Worlds, the top one at the
// plan's alpha) through GameSetup::camera, drawn by Renderer2D into the
// composite and blitted to the swapchain (M7); then FramePacer::EndFrame
// (Paced, Standard or VRR; M6b). The pacer presents through RenderPresentPort,
// the adapter over se::present and Renderer2D, and Renderer2D's GPU timings are
// FrameCosts' g (the M6/M7 integration). Iterates that do not render still pump
// the texture uploads. A fault in the render build phase is a fault of the game
// build: it takes the callbacks' fault path (a dump, the message box, the quit
// path).
//
// Device loss (ADR-068): the device status is read at each iterate's start
// and after a failed submit. On kRemoved the lifecycle enters DeviceLost:
// the pacer lets go of the port, Renderer2D releases every GPU object
// through the retire queue, the window is released; then a new device is
// created on main, claims the window, Renderer2D recreates its objects and
// re-uploads every texture from the kept payloads (handles stay valid), a
// new port is attached and the lifecycle returns to Running with a
// frame-clock reset. Worlds are untouched. A recreation that fails is
// retried every second, and the fifth failure takes the fault path.
//
// Shutdown follows the design's order for the parts that exist: the
// watchdog's deadline starts when the quit drain ends; stop the input
// sampler (before step 4, ADR-047), remove the event filter,
// SDL_WaitForGPUIdle, destroy the Worlds, stop the audio engine (step 7),
// stop the background lane (step 8: Durable jobs, the --record replay
// among them, run to the end), stop the vblank sampler (step 9), release
// every AssetScope (step 10), release the present port and the device,
// remove the subclass and destroy the window, SDL_QuitSubSystem, stop the
// log worker (step 16), join the watchdog (step 17), stop the profiler.
// M8: the audio engine goes at step 7, right after the Worlds (their voice
// scopes end with them): every voice stopped and uninitialised, then
// ma_engine_uninit, then the PCM. The input sampler stops before step 4.
//
// The steps run are logged after the teardown ("shutdown order: ...",
// ADR-088).
//
// Audio (M8): each frame the AudioBridge recalibrates its PCM clock and
// takes D = B + T/2 from the plan's budget and the FramePacer's vblank
// period of the frame (PacerFrame::vblank_period_sec, set by BeginFrame);
// the World's audio events start after each tick at max(now, anchor + D);
// the engine's end ring and finished voices are swept every iterate.
// Game code that runs during shutdown (GameSetup::on_shutdown and every
// GameState::OnExit) has its throws contained: the fault is logged and
// dumped and the remaining steps still run (ADR-044).
//
// Test builds add the M4 exit hooks: --crash-test after-ticks:N faults on
// main right after tick N; --dump-test after-ticks:N has the watchdog write
// a dump after tick N, holds the ticks until it is written, then quits.
// M7 adds --inject-frame-fault, --reload-texture and --readback
// (app_config.h); M9 --lane-step-trace; M12 --hang-test (main loops inside
// a system at tick N) and --watchdog-period-ms.
//
// The watchdog (M12): main's iterate counter, the Suspended and Modal flags,
// the heartbeat (KnownLongWork) and the in-driver flag (se_platform/
// driver_wait.h, raised around SDL_GPU and pacer waits) are its inputs; a
// hang (5 periods of 1 s without progress, armed after the first iterate)
// gets one dump per episode. The session header (integration.h) is begun at
// boot step 10 and gets the last tick's hash after each frame's ticks. The
// teardown's length is logged ("teardown took ...").

#ifndef SE_RUNTIME_APP_H_
#define SE_RUNTIME_APP_H_

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "se_audio/audio_engine.h"
#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/time.h"
#include "se_core/types.h"
#include "se_platform/gamepads.h"
#include "se_platform/inbox.h"
#include "se_platform/input_sampler.h"
#include "se_platform/sdl_context.h"
#include "se_platform/wake.h"
#include "se_platform/window.h"
#include "se_render/frame_recorder.h"
#include "se_render/gpu_device.h"
#include "se_render/renderer_2d.h"
#include "se_render/texture_table.h"
#include "se_render_core/render_components.h"
#include "se_runtime/app_config.h"
#include "se_runtime/app_extension.h"
#include "se_runtime/asset_runtime.h"
#include "se_runtime/frame_pacer.h"
#include "se_runtime/heartbeat.h"
#include "se_runtime/integration.h"
#include "se_runtime/lifecycle.h"
#include "se_runtime/quit.h"
#include "se_runtime/render_present_port.h"
#include "se_runtime/sound_assets.h"
#include "se_runtime/user_settings.h"
#include "se_sim/game_module.h"

namespace se {

class App;
struct GameRuntime;

struct ContentPaths {
  // SDL's base path (the executable's directory), with a trailing '/'.
  std::string base;
  // Where the game's content is, without a trailing '/'.
  std::string content_root;
  // The per-user writable directory, with a trailing '/'.
  std::string pref;
  // The cooked atlas pages (M9; --cooked-root, else <base>cooked), without
  // a trailing '/'.
  std::string cooked;
};

// What one iterate did, for tests and tools (GameSetup::on_frame).
struct FrameReport {
  std::uint64_t iterate = 0;
  std::uint64_t now_ns = 0;
  LifecycleState state = LifecycleState::kBooting;
  CallbackRate rate = CallbackRate::kFull;
  StepPlan plan;
  // The first tick this frame ran (when plan.steps > 0) and the next one.
  Tick first_tick = 0;
  Tick next_tick = 0;
  bool gameplay_ran = false;
  bool rendered = false;
  bool presented = false;
  // Renderer2D's numbers for this frame (when rendered).
  Renderer2DStats render;
  bool modal = false;
  bool suspended = false;
  // The pacer's FrameClock and what it did this frame (M6b).
  FrameClock clock;
  PacerFrame pacer;
};

// A game, as the runtime runs it. Made by the game's GameFactory.
struct GameSetup {
  const GameModule* module = nullptr;
  std::string title = "simple-engine";
  // SDL_GetPrefPath(org, app).
  std::string org = "simple-engine";
  std::string app = "game";
  // A path relative to the content root that marks it; searched from SDL's
  // base path upwards. Empty: the base path is the content root.
  std::string content_marker;
  std::uint64_t seed = 0;
  std::uint32_t tick_rate = 120;
  ClearColor clear_color{0.08F, 0.09F, 0.12F, 1.0F};
  int window_width = 1280;
  int window_height = 720;
  LifecycleConfig lifecycle;
  // The texture table (config/texture.json, ADR-026) relative to the
  // content root; empty: no textures (every sprite draws magenta). Its
  // textures load from cooked pages through the background lane (M9).
  std::string texture_table;
  // What each GameState's AssetScope loads before its Worlds step (M9),
  // by state id; empty: every texture of the table.
  std::function<StateAssets(std::string_view state_id)> state_assets;
  // The settings file (config/config.json) relative to the content root:
  // the key bindings and sound volumes (user_settings.h, M8); empty: the
  // module's default bindings and full volumes.
  std::string settings_file;
  // The sound table (config/sound.json: name to file, like the texture
  // table) relative to the content root; empty: no sounds. Its files are
  // every state's default sounds, decoded on the background lane and handed
  // to the audio engine when resident (ADR-087).
  std::string sound_table;
  // The start bytes of a --record replay (rampage: its scene file); empty:
  // none. Their hash goes into the session header too (M12).
  std::function<std::vector<std::byte>(const ContentPaths& paths)> replay_start;
  // The scene the session starts from, relative to the content root, for
  // the session header's replays from a dump (M12); empty: none.
  std::string scene;
  // The camera a World is drawn with; empty: its primary Camera2D, else a
  // default one.
  std::function<Camera2D(const World& world)> camera;
  // Boot step 10, before the start state is pushed: point the game at its
  // content (a scene, settings).
  std::function<std::expected<void, Error>(const ContentPaths& paths)>
      configure;
  // Two-phase quit providers and the synchronous recovery hook (M9, M10;
  // tests). The providers must outlive the App.
  std::vector<QuitProvider*> quit_providers;
  QuitController::RecoveryHook recovery;
  // Observers on main (tests, tools): after boot, at the end of every
  // iterate, and when shutdown starts.
  std::function<void(App& app)> on_boot;
  std::function<void(App& app, const FrameReport& report)> on_frame;
  std::function<void(App& app, SDL_AppResult result)> on_shutdown;

  // M10a: an extension that owns the frame's session and adds its UI (the
  // editor; app_extension.h). The App keeps it until its teardown ends.
  std::shared_ptr<AppExtension> extension;
  // Push the module's start state at boot (false: the extension decides
  // what runs; the editor starts in Edit mode).
  bool push_start_state = true;
  // What --pacing auto means for this program (the editor: Standard), and
  // the frame latency the pacer starts with (the editor: 2).
  PacingRequest default_pacing = PacingRequest::kAuto;
  int frame_latency = 1;
  // SDL's raw keyboard (the editor turns it off: IME, hotkeys); also
  // --no-raw-keyboard.
  bool raw_keyboard = true;
};

using GameFactory =
    std::expected<GameSetup, Error> (*)(const AppConfig& config);

class App final : public WindowListener {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<App>, Error> Create(
      const AppConfig& config, GameFactory factory) SE_REQUIRES(g_main);

  App(const App&) = delete;
  App& operator=(const App&) = delete;
  // Runs the shutdown order for whatever exists (also after a failed boot).
  ~App() override;

  // The SDL callbacks' bodies (sdl_app.cc wraps them in the fault
  // boundary).
  SDL_AppResult Iterate() SE_REQUIRES(g_main);
  SDL_AppResult HandleEvent(const SDL_Event& event) SE_REQUIRES(g_main);
  // Runs the shutdown order. Returns |result|, or SDL_APP_FAILURE when a
  // game hook faulted during shutdown (contained, ADR-044).
  SDL_AppResult Shutdown(SDL_AppResult result) SE_REQUIRES(g_main);
  // A callback caught an exception: request a dump, tell the user (unless
  // SE_NO_MESSAGE_BOX=1), and return SDL_APP_FAILURE so SDL runs the quit
  // path.
  SDL_AppResult OnCallbackFault(std::string_view where,
                                std::string_view what) noexcept;

  // Starts the two-phase quit.
  void RequestQuit() SE_REQUIRES(g_main);

  // Known long work on main (a large synchronous load, a blocking save at
  // session end; M12): the watchdog's heartbeat is up while the scope
  // lives (heartbeat.h). Main only.
  [[nodiscard]] HeartbeatScope KnownLongWork(std::string_view what) noexcept {
    return HeartbeatScope(heartbeat_flag_, what, &long_work_);
  }
  // The known long work so far (logged at shutdown).
  [[nodiscard]] const LongWorkStats& LongWork() const noexcept {
    return long_work_;
  }
  // The watchdog's counters (M12); zeros without a watchdog.
  [[nodiscard]] crash::WatchdogStats WatchdogStats() const noexcept;

  // WindowListener (the window subclass, on main).
  void OnSessionEnd(SessionEnd kind) noexcept override;
  void OnPower(PowerChange change) noexcept override;
  void OnModalChanged(bool modal) noexcept override;

  [[nodiscard]] const AppConfig& Config() const noexcept { return config_; }
  // The game's module instance and fork pool (boot step 10 to shutdown
  // step 6), for an extension's sessions; null outside them.
  [[nodiscard]] ModuleInstance* GameInstance() noexcept;
  [[nodiscard]] jobs::ForkJoinPool* GamePool() noexcept;
  // The log worker (boot step 2 to shutdown step 16), or null.
  [[nodiscard]] integration::Logging* Logs() noexcept {
    return logging_ ? &*logging_ : nullptr;
  }
  [[nodiscard]] const ContentPaths& Paths() const noexcept { return paths_; }
  [[nodiscard]] const Lifecycle& GetLifecycle() const noexcept {
    return lifecycle_;
  }
  [[nodiscard]] QuitController& Quit() noexcept { return quit_; }
  [[nodiscard]] Window* GetWindow() const noexcept { return window_.get(); }
  [[nodiscard]] GpuDevice* Gpu() const noexcept { return gpu_.get(); }
  [[nodiscard]] Renderer2D* Renderer() const noexcept {
    return renderer_.get();
  }
  // The background lane and the AssetRegistry (M9); null before boot step
  // 9 and after shutdown step 10.
  [[nodiscard]] AssetRuntime* Assets() const noexcept { return assets_.get(); }
  // The texture table's entries, with paths joined to the content root.
  [[nodiscard]] const std::vector<TextureTableEntry>& Textures()
      const noexcept {
    return textures_;
  }
  [[nodiscard]] const FixedStepper& Stepper() const noexcept {
    return stepper_;
  }
  [[nodiscard]] const FramePacer& Pacer() const noexcept { return pacer_; }
  // The input sampler (from the first iterate to shutdown step 4), or
  // nullptr.
  [[nodiscard]] const InputSampler* Sampler() const noexcept {
    return input_sampler_.get();
  }
  // Gamepads the App has open.
  [[nodiscard]] std::size_t OpenGamepads() const noexcept {
    return gamepads_.Count();
  }
  // The settings file's contents (M8).
  [[nodiscard]] const UserSettings& Settings() const noexcept {
    return settings_;
  }
  // The lane's sound decoding so far (ADR-087).
  [[nodiscard]] SoundDecodeStats SoundDecodes() const noexcept {
    return sound_decoder_ ? sound_decoder_->Stats() : SoundDecodeStats{};
  }
  // The AudioBridge's D of the last frame, in nanoseconds (0: none yet).
  [[nodiscard]] std::uint64_t AudioDelay() const noexcept;
  // The audio engine, or nullptr (--audio none, or it could not start).
  [[nodiscard]] audio::AudioEngine* Audio() const noexcept {
    return audio_.get();
  }
  // Device losses recovered so far (ADR-068).
  [[nodiscard]] std::uint64_t DeviceRecoveries() const noexcept {
    return device_recoveries_;
  }
  // The window's display: SDL's refresh, the OS's mode (0: unknown) and
  // the monitor.
  [[nodiscard]] double SdlRefreshHz() const noexcept { return refresh_hz_; }
  [[nodiscard]] const MonitorMode& DisplayMode() const noexcept {
    return display_mode_;
  }
  [[nodiscard]] MonitorHandle Monitor() const noexcept { return monitor_; }
  [[nodiscard]] Tick NextTick() const noexcept;
  // The last tick run: 0 before the first, and while an extension has no
  // session (the editor in Edit mode, whose next tick is 0).
  [[nodiscard]] Tick LastTick() const noexcept {
    const Tick next = NextTick();
    return next == 0 ? 0 : next - 1;
  }
  [[nodiscard]] std::uint64_t Iterates() const noexcept { return iterates_; }
  [[nodiscard]] std::uint64_t PresentedFrames() const noexcept {
    return presented_frames_;
  }
  [[nodiscard]] std::uint64_t InboxEvents() const noexcept {
    return inbox_events_;
  }
  [[nodiscard]] std::uint64_t LowMemoryEvents() const noexcept {
    return low_memory_events_;
  }
  // The watchdog's inputs (integration).
  [[nodiscard]] const std::atomic<std::uint64_t>& IterateCounter()
      const noexcept {
    return iterate_counter_;
  }

 private:
  App(const AppConfig& config, GameSetup setup);

  std::expected<void, Error> Boot() SE_REQUIRES(g_main);
  std::expected<void, Error> BootStep(int step) const;
  std::expected<void, Error> ResolvePaths();
  std::expected<void, Error> ResolvePrefPath();
  void StartProfiler();
  std::expected<void, Error> StartGame() SE_REQUIRES(g_main);
  void Teardown() SE_REQUIRES(g_main);

  void TakeInbox() SE_REQUIRES(g_main);
  void SyncQuitState();
  void ApplyCallbackRate();
  std::expected<void, Error> StartRenderer() SE_REQUIRES(g_main);
  std::expected<void, Error> StartAssets() SE_REQUIRES(g_main);
  // Renders the frame of |report|'s plan and fills its rendered, presented
  // and render fields; |wait_sec| is the time spent in the swapchain wait.
  // An Error is a fault of the render build phase.
  std::expected<void, Error> RenderFrame(FrameReport& report, double& wait_sec)
      SE_REQUIRES(g_main);
  // The frame's layers from |states|' render sources (M10a: split out of
  // RenderFrame for the extension).
  void FillLayers(const GameStateStack& states, const StepPlan& plan)
      SE_REQUIRES(g_main);
  // The stack whose assets load and whose Worlds draw: the extension's
  // session, else the game's.
  [[nodiscard]] const GameStateStack& ActiveStates() const;
  [[nodiscard]] Camera2D CameraFor(const World& world) const;
  // The --reload-texture hooks due at this iterate (through the lane, M9)
  // and the M7 readbacks.
  void RunTextureReloads() SE_REQUIRES(g_main);
  void RunReadbacks(const FrameReport& report) SE_REQUIRES(g_main);
  void UpdateTitle(std::uint64_t now_ns) SE_REQUIRES(g_main);
  // Device loss (ADR-068): the status at frame start (and the flag a
  // failed submit set); DeviceLost's release; the recreation. An Error is
  // a recreation that failed for the last time.
  [[nodiscard]] std::expected<void, Error> CheckDevice() SE_REQUIRES(g_main);
  void EnterDeviceLost() SE_REQUIRES(g_main);
  [[nodiscard]] std::expected<void, Error> RecreateDevice() SE_REQUIRES(g_main);
  // The pacer's view of the vblank sampler (empty: none).
  [[nodiscard]] VblankReader MakeVblankReader() const;
  // Re-reads the window's monitor and display mode after a display change
  // or a move (always with |force|); a new monitor goes to the vblank
  // sampler and the pacer.
  void UpdateDisplay(bool force) SE_REQUIRES(g_main);
  // The refresh the pacer compares the sampler with: the OS's current mode
  // of the window's monitor, else SDL's (ADR-055).
  [[nodiscard]] double PacingRefreshHz() const noexcept;
  void LogProgress(std::uint64_t now_ns);
  // A callback starts: the watchdog's suspended flag follows the lifecycle
  // again, and an Idle wait that an event ended is logged (ADR-111).
  void EndIdleWait();
  // The M4 exit hooks (test builds): the tick they act after (0: none),
  // the frame's plan cut so that it ends at that tick, and the action.
  [[nodiscard]] std::int64_t ExitTestTick() const noexcept;
  void StopPlanAtExitTestTick(StepPlan& plan, Tick first_tick) const;
  void RunExitTests(Tick next_tick) SE_REQUIRES(g_main);
  // M8: the settings file (boot step 10), the --record replay at shutdown,
  // and the --push-key hook at the end of an iterate.
  std::expected<void, Error> LoadSettings();
  // Boot step 10: the audio engine and the sound table (M8). Never fails
  // the boot: without an engine the game runs silent.
  void StartAudio() SE_REQUIRES(g_main);
  // The AssetRuntime's sound listener: a sound of the table landed, was
  // reloaded, or (null) was released (ADR-087).
  void OnSound(std::string_view path, std::shared_ptr<const Pcm> pcm)
      SE_REQUIRES(g_main);
  // Before the frame's ticks: D, the PCM clock, the listener.
  void BeginAudioFrame(const StepPlan& plan, const FrameClock& clock)
      SE_REQUIRES(g_main);
  // The --record replay, through the lane as a Durable save (ADR-089).
  void WriteReplay() SE_REQUIRES(g_main);
  // --durable-save-test (ADR-089).
  void RunDurableSaveTest() SE_REQUIRES(g_main);
  // Records a teardown step for the shutdown-order log (ADR-088).
  void NoteTeardown(std::string_view step) noexcept;
  // Records when a part of the teardown ended, for the log line that says
  // where a slow teardown spent its time (M12's 2 s gate).
  void MarkTeardown(std::string_view part) noexcept;
  void RunPushKeys(const FrameReport& report) SE_REQUIRES(g_main);
  // The input sampler (M8): started at the first iterate, inside ADR-047's
  // window; parked while Suspended or Idle; its rate from the power source
  // every 5 s; stopped before shutdown step 4.
  void StartInputSampler() SE_REQUIRES(g_main);
  void UpdateInputSampler(std::uint64_t now_ns) SE_REQUIRES(g_main);

  AppConfig config_;
  GameSetup setup_;

  // Boot steps 1-10, in order.
  std::optional<integration::CrashCapture> crash_;
  std::optional<integration::Logging> logging_;
  std::optional<SdlContext> sdl_;
  Inbox inbox_;
  std::optional<EventFilter> filter_;
  std::optional<WakeEventScope> wake_;
  ContentPaths paths_;
  std::unique_ptr<Window> window_;
  std::unique_ptr<GpuDevice> gpu_;
  std::unique_ptr<RenderPresentPort> present_;
  std::unique_ptr<Renderer2D> renderer_;
  std::vector<TextureTableEntry> textures_;
  // The table as written (paths relative to the content root).
  std::vector<TextureTableEntry> texture_table_;
  std::vector<RenderLayer> layers_;
  std::vector<bool> readbacks_done_;
  integration::CpuInfo cpu_;
  std::optional<integration::HelperThreads> helpers_;
  // The sound table (boot step 9) and the lane's decoder (boot step 10);
  // the decoder outlives the lane's jobs and the registry (ADR-087).
  std::vector<SoundTableEntry> sound_table_;
  std::unique_ptr<LaneSoundDecoder> sound_decoder_;
  std::unique_ptr<AssetRuntime> assets_;
  // Taken at shutdown step 8, logged (and the step trace written) after
  // the teardown.
  std::optional<LaneStats> lane_stats_;
  std::vector<LaneStepRecord> lane_trace_;
  std::uint64_t lane_durable_failures_ = 0;
  // The --record replay submitted to the lane, and the lane's Durable
  // failures before it (ADR-089).
  bool replay_submitted_ = false;
  std::uint64_t replay_failures_before_ = 0;
  std::size_t replay_ticks_ = 0;
  // --durable-save-test.
  bool durable_save_test_done_ = false;
  std::string durable_save_test_path_;
  // When the quit was first seen pending (the "quit ready after" log).
  std::uint64_t quit_requested_ns_ = 0;
  // The teardown steps run, in order (ADR-088).
  std::array<std::string_view, 24> teardown_steps_{};
  std::size_t teardown_count_ = 0;
  // The parts of the teardown and when each ended (SDL_GetTicksNS).
  struct TeardownMark {
    std::string_view part;
    std::uint64_t end_ns = 0;
  };
  std::array<TeardownMark, 24> teardown_marks_{};
  std::size_t teardown_mark_count_ = 0;
  UserSettings settings_;
  std::unique_ptr<audio::AudioEngine> audio_;
  std::unique_ptr<GameRuntime> game_;
  // M10a: the extension's session of this frame and its render request.
  ExtensionSession ext_session_;
  std::vector<ViewportDraw> ext_viewports_;
  SwapchainOverlay* ext_overlay_ = nullptr;
  // The extension's readback target of the frame (null: the composite).
  const ViewportTarget* ext_readback_ = nullptr;
  ClearColor ext_clear_;
  bool ext_frame_ready_ = false;
  // M8: gamepads are opened on main as SDL reports them; the sampler
  // updates them.
  Gamepads gamepads_;
  std::unique_ptr<InputSampler> input_sampler_;
  // The rate the sampler was last given (ADR-090).
  std::uint32_t sampler_rate_ = 0;
  PowerWatch power_;

  Lifecycle lifecycle_;
  QuitController quit_;
  FramePacer pacer_;
  FixedStepper stepper_;
  CostTracker costs_;
  // SDL's refresh for the window's display, the OS's mode, the monitor.
  double refresh_hz_ = 0.0;
  MonitorMode display_mode_;
  MonitorHandle monitor_ = nullptr;
  bool late_frame_injected_ = false;
  // Device loss (ADR-068).
  bool device_lost_injected_ = false;
  bool device_removed_ = false;
  std::uint64_t device_recoveries_ = 0;
  std::uint32_t recreate_failures_ = 0;
  std::uint64_t next_recreate_ns_ = 0;
  std::uint64_t device_lost_ns_ = 0;
  // Shutdown step 9 left the vblank sampler in its wait (logged after the
  // teardown).
  bool vblank_detached_ = false;

  // Read by the watchdog (integration). The heartbeat flag is raised only
  // by KnownLongWork (M12: the session-end recovery hook; M10's loads and
  // saves). The suspended flag is also up while the App waits for events
  // in Idle, between the iterate that entered the wait and the next
  // callback (ADR-111).
  std::atomic<std::uint64_t> iterate_counter_{0};
  std::atomic<bool> suspended_flag_{false};
  // The App returned to SDL to wait for events (Idle), and since when.
  bool idle_waiting_ = false;
  std::uint64_t idle_wait_start_ns_ = 0;
  std::atomic<bool> modal_flag_{false};
  std::atomic<bool> heartbeat_flag_{false};
  LongWorkStats long_work_;
  std::uint64_t boot_start_ns_ = 0;
  bool profiling_ = false;
  bool exit_test_fired_ = false;
  bool exit_test_hold_ = false;
  std::uint64_t dumps_before_test_ = 0;

  CallbackRate applied_rate_ = CallbackRate::kFull;
  bool quit_pending_seen_ = false;
  bool booted_ = false;
  // Set when every teardown step has run; a teardown that a throw cut
  // short runs its remaining (idempotent) steps again from ~App.
  bool torn_down_ = false;
  bool shutdown_faulted_ = false;
  std::uint64_t iterates_ = 0;
  std::uint64_t events_ = 0;
  std::uint64_t inbox_events_ = 0;
  std::uint64_t low_memory_events_ = 0;
  std::uint64_t presented_frames_ = 0;
  std::uint64_t frames_skipped_ = 0;
  std::uint64_t ticks_run_ = 0;
  std::uint64_t last_title_ns_ = 0;
  std::uint64_t last_log_ns_ = 0;
};

}  // namespace se

#endif  // SE_RUNTIME_APP_H_
