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
//    7 gpu-device      the SDL_GPU device, claiming the window with VSYNC
//    8 cpu-topology    the CPU topology and the pool sizes it gives
//    9 helper-threads  the watchdog (boot budget until the first iterate)
//   10 game            the GameModule's registry and storages, the
//                      GameStateStack with the start state pushed, the
//                      SimDriver writing the process's InputFrame ring
// The main thread runs inside its RoleScope from SDL_AppInit to the end of
// SDL_AppQuit (integration::EnterMainThread).
// --headless skips the window and the GPU device (steps 6 and 7 do
// nothing) and uses SDL's dummy video driver.
//
// Each iterate: count the iterate (the watchdog's progress), take the
// inbox, poll the two-phase quit, update the lifecycle (callback rate, held
// input, frame-clock reset), plan the frame's ticks with the FixedStepper
// from SDL's clock and display mode, tick the running Worlds through
// SimDriver::TickInline with BasicInputService's frames, record the
// clear-only frame and present it with VSYNC.
//
// Shutdown follows the design's order for the parts that exist: the
// watchdog's deadline starts when the quit drain ends; remove the event
// filter, SDL_WaitForGPUIdle, destroy the Worlds, release the device,
// remove the subclass and destroy the window, SDL_QuitSubSystem, stop the
// log worker (step 16), join the watchdog (step 17), stop the profiler.
//
// Test builds add the M4 exit hooks: --crash-test after-ticks:N faults on
// main right after tick N; --dump-test after-ticks:N has the watchdog write
// a dump after tick N, holds the ticks until it is written, then quits.

#ifndef SE_RUNTIME_APP_H_
#define SE_RUNTIME_APP_H_

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/time.h"
#include "se_core/types.h"
#include "se_platform/inbox.h"
#include "se_platform/sdl_context.h"
#include "se_platform/wake.h"
#include "se_platform/window.h"
#include "se_render/frame_recorder.h"
#include "se_render/gpu_device.h"
#include "se_runtime/app_config.h"
#include "se_runtime/frame_clock.h"
#include "se_runtime/integration.h"
#include "se_runtime/lifecycle.h"
#include "se_runtime/quit.h"
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
  bool modal = false;
  bool suspended = false;
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
  void Shutdown(SDL_AppResult result) SE_REQUIRES(g_main);
  // A callback caught an exception: request a dump, tell the user (unless
  // SE_NO_MESSAGE_BOX=1), and return SDL_APP_FAILURE so SDL runs the quit
  // path.
  SDL_AppResult OnCallbackFault(std::string_view where,
                                std::string_view what) noexcept;

  // Starts the two-phase quit.
  void RequestQuit() SE_REQUIRES(g_main);

  // WindowListener (the window subclass, on main).
  void OnSessionEnd(SessionEnd kind) noexcept override;
  void OnPower(PowerChange change) noexcept override;
  void OnModalChanged(bool modal) noexcept override;

  [[nodiscard]] const AppConfig& Config() const noexcept { return config_; }
  [[nodiscard]] const ContentPaths& Paths() const noexcept { return paths_; }
  [[nodiscard]] const Lifecycle& GetLifecycle() const noexcept {
    return lifecycle_;
  }
  [[nodiscard]] QuitController& Quit() noexcept { return quit_; }
  [[nodiscard]] Window* GetWindow() const noexcept { return window_.get(); }
  [[nodiscard]] GpuDevice* Gpu() const noexcept { return gpu_.get(); }
  [[nodiscard]] const FixedStepper& Stepper() const noexcept {
    return stepper_;
  }
  [[nodiscard]] Tick NextTick() const noexcept;
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
  // Records and submits the clear-only frame; false when nothing could be
  // recorded. |presented| tells whether a swapchain texture was acquired.
  bool RenderClear(bool& presented, double& render_sec) SE_REQUIRES(g_main);
  void UpdateTitle(std::uint64_t now_ns) SE_REQUIRES(g_main);
  void LogProgress(std::uint64_t now_ns);
  // The M4 exit hooks (test builds): the tick they act after (0: none),
  // the frame's plan cut so that it ends at that tick, and the action.
  [[nodiscard]] std::int64_t ExitTestTick() const noexcept;
  void StopPlanAtExitTestTick(StepPlan& plan, Tick first_tick) const;
  void RunExitTests(Tick next_tick) SE_REQUIRES(g_main);

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
  integration::CpuInfo cpu_;
  std::optional<integration::HelperThreads> helpers_;
  std::unique_ptr<GameRuntime> game_;

  Lifecycle lifecycle_;
  QuitController quit_;
  FrameClockSource clock_;
  FixedStepper stepper_;
  CostTracker costs_;
  double refresh_hz_ = 0.0;

  // Read by the watchdog (integration). The heartbeat flag is for long
  // work on main that still makes progress (M9's loads, M12); nothing in
  // M5 raises it.
  std::atomic<std::uint64_t> iterate_counter_{0};
  std::atomic<bool> suspended_flag_{false};
  std::atomic<bool> modal_flag_{false};
  std::atomic<bool> heartbeat_flag_{false};
  std::uint64_t boot_start_ns_ = 0;
  bool profiling_ = false;
  bool exit_test_fired_ = false;
  bool exit_test_hold_ = false;
  std::uint64_t dumps_before_test_ = 0;

  CallbackRate applied_rate_ = CallbackRate::kFull;
  bool quit_pending_seen_ = false;
  bool booted_ = false;
  bool torn_down_ = false;
  std::uint64_t iterates_ = 0;
  std::uint64_t events_ = 0;
  std::uint64_t inbox_events_ = 0;
  std::uint64_t low_memory_events_ = 0;
  std::uint64_t presented_frames_ = 0;
  std::uint64_t render_failures_ = 0;
  std::uint64_t ticks_run_ = 0;
  std::uint64_t last_title_ns_ = 0;
  std::uint64_t last_log_ns_ = 0;
};

}  // namespace se

#endif  // SE_RUNTIME_APP_H_
