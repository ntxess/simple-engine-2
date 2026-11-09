// Copyright 2026 The simple-engine-2 Authors.

#include "se_runtime/app.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_version.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/error.h"
#include "se_core/jobs.h"
#include "se_core/log.h"
#include "se_core/profile.h"
#include "se_core/thread_role.h"
#include "se_core/time.h"
#include "se_core/world.h"
#include "se_platform/fatal.h"
#include "se_platform/fs.h"
#include "se_platform/inbox.h"
#include "se_platform/sdl_context.h"
#include "se_platform/wake.h"
#include "se_platform/window.h"
#include "se_render/frame_recorder.h"
#include "se_render/gpu_device.h"
#include "se_render/image_io.h"
#include "se_render/renderer_2d.h"
#include "se_render/texture_table.h"
#include "se_render_core/render_components.h"
#include "se_render_core/render_list.h"
#include "se_runtime/app_config.h"
#include "se_runtime/basic_input.h"
#include "se_runtime/integration.h"
#include "se_runtime/lifecycle.h"
#include "se_runtime/quit.h"
#include "se_sim/game_module.h"
#include "se_sim/game_state.h"
#include "se_sim/replay.h"
#include "se_sim/session_log.h"
#include "se_sim/sim_driver.h"
#include "src/game_hook.h"

namespace se {

// The simulation side of the App (boot step 10). Not movable: the stack and
// the driver keep pointers into the module instance and the pool.
struct GameRuntime {
  GameRuntime(const GameModule& module, const StateEnv& env,
              const jobs::ForkJoinPool::Config& pool_config)
      : instance(module),
        states(instance.Factory(), WithRegistry(env, instance)),
        pool(pool_config),
        driver(instance.MutableSchedule(), pool),
        input(module.actions) {}
  GameRuntime(const GameRuntime&) = delete;
  GameRuntime& operator=(const GameRuntime&) = delete;

  static StateEnv WithRegistry(StateEnv env, const ModuleInstance& instance) {
    env.registry = &instance.Registry();
    return env;
  }

  ModuleInstance instance;
  GameStateStack states;
  // No workers (W = 0) until M14; the most it may start comes from the CPU
  // topology (boot step 8).
  jobs::ForkJoinPool pool;
  SimDriver driver;
  SessionLog session;
  ReplayRecorder recorder;
  NullAudioSink audio;
  BasicInputService input;
};

namespace {

constexpr std::uint64_t kTitleIntervalNs = 250'000'000;
constexpr std::uint64_t kLogIntervalNs = 2'000'000'000;
constexpr double kNsToSec = 1e-9;

// A standard-library throw, the kind the fault boundaries contain (test
// hook; engine code never writes `throw`).
void InjectedThrow(std::string_view site) {
  const std::vector<int> empty;
  SE_LOG_WARN("injecting a throw in {}", site);
  const int never = empty.at(site.size());
  SE_LOG_ERROR("the injected throw did not happen ({})", never);
}

// An access violation on the calling thread (the --crash-test hook): the
// crash handler takes it and writes the dump out of process. A C++ catch
// (/EHsc) never sees it.
[[noreturn]] void FaultForCrashTest() {
  volatile int* volatile target = nullptr;
  *target = 42;
  std::abort();  // not reached
}

std::string GpuDriverFromEnvironment() {
  const char* value = SDL_getenv("SE_GPU_DRIVER");
  return value != nullptr ? std::string(value) : std::string();
}

}  // namespace

std::expected<std::unique_ptr<App>, Error> App::Create(const AppConfig& config,
                                                       GameFactory factory) {
  SE_ASSERT_MAIN_THREAD();
  if (factory == nullptr) {
    return Fail(ErrorCode::kInvalidArgument, "App::Create needs a GameFactory");
  }
  auto setup = factory(config);
  if (!setup) return std::unexpected(std::move(setup.error()));
  if (setup->module == nullptr) {
    return Fail(ErrorCode::kInvalidArgument, "the GameSetup has no module");
  }
  std::unique_ptr<App> app(new App(config, std::move(*setup)));
  if (auto booted = app->Boot(); !booted) {
    // ~App undoes the steps that ran, in the shutdown order.
    return std::unexpected(std::move(booted.error()));
  }
  return app;
}

App::App(const AppConfig& config, GameSetup setup)
    : config_(config),
      setup_(std::move(setup)),
      lifecycle_(setup_.lifecycle),
      clock_(config.fixed_frame_dt_ms * 1e-3),
      stepper_(setup_.tick_rate) {}

App::~App() {
  AssertMainThread();
  Teardown();
}

std::expected<void, Error> App::BootStep(int step) const {
  if (kRuntimeTestHooks && config_.fail_init_step == step) {
    return Fail(
        ErrorCode::kInvalidArgument,
        std::format("injected failure at boot step {} ({})", step,
                    InitStepNames()[static_cast<std::size_t>(step - 1)]));
  }
  return {};
}

std::expected<void, Error> App::Boot() {
  boot_start_ns_ = integration::BootClockNs();
  // 1. The crash handler, before logging and SDL. Its database and the log
  // file are under the pref path, which SDL resolves without SDL_Init.
  if (auto ok = BootStep(1); !ok) {
    return ok;
  }
  if (auto pref = ResolvePrefPath(); !pref) {
    return pref;
  }
  auto crash = integration::StartCrashCapture(
      paths_.pref + "crashdb", setup_.app, config_.crash_capture);
  if (!crash) return std::unexpected(std::move(crash.error()));
  crash_.emplace(*crash);

  // 2. The log worker and its sinks, before any other engine thread
  // exists; then the profiler (--profile).
  if (auto ok = BootStep(2); !ok) {
    return ok;
  }
  auto logging = integration::StartLogging(
      config_, paths_.pref + "logs/" + setup_.app + ".log");
  if (!logging) return std::unexpected(std::move(logging.error()));
  logging_.emplace(std::move(*logging));
  SE_LOG_INFO("{}; log file {}", crash_->Status(),
              logging_->FilePath().empty() ? std::string("none")
                                           : logging_->FilePath());
  if (config_.profile) StartProfiler();

  // 3. SDL.
  if (auto ok = BootStep(3); !ok) {
    return ok;
  }
  SdlInitConfig sdl_config;
  sdl_config.app_name = setup_.title;
  if (config_.headless) sdl_config.video_driver = "dummy";
  auto sdl = SdlContext::Init(sdl_config);
  if (!sdl) return std::unexpected(std::move(sdl.error()));
  sdl_.emplace(std::move(*sdl));
  SE_LOG_INFO("SDL {} video driver '{}'", SDL_GetRevision(),
              sdl_->VideoDriver());

  // 4. The event filter and the wake event.
  if (auto ok = BootStep(4); !ok) {
    return ok;
  }
  auto filter = EventFilter::Install(inbox_);
  if (!filter) return std::unexpected(std::move(filter.error()));
  filter_.emplace(std::move(*filter));
  auto wake = WakeEventScope::Register();
  if (!wake) return std::unexpected(std::move(wake.error()));
  wake_.emplace(std::move(*wake));

  // 5. The content root and the pref path.
  if (auto ok = BootStep(5); !ok) {
    return ok;
  }
  if (auto paths = ResolvePaths(); !paths) {
    return paths;
  }

  // 6. The window and its subclass.
  if (auto ok = BootStep(6); !ok) {
    return ok;
  }
  if (!config_.headless) {
    WindowConfig window_config;
    window_config.title = setup_.title;
    window_config.width = setup_.window_width;
    window_config.height = setup_.window_height;
    window_config.activate_when_shown = config_.activate_when_shown;
    auto window = Window::Create(window_config);
    if (!window) return std::unexpected(std::move(window.error()));
    window_ = std::move(*window);
    window_->SetListener(this);
    refresh_hz_ = window_->Refresh().hz;
  }

  // 7. The GPU device, claiming the window with VSYNC.
  if (auto ok = BootStep(7); !ok) {
    return ok;
  }
  if (!config_.headless) {
    GpuDeviceConfig gpu_config;
    gpu_config.driver =
        ResolveGpuDriver(config_.gpu_driver, GpuDriverFromEnvironment());
    auto device = GpuDevice::Create(gpu_config);
    if (!device) return std::unexpected(std::move(device.error()));
    gpu_ = std::make_unique<GpuDevice>(std::move(*device));
    if (auto claimed = gpu_->ClaimWindow(window_->Native()); !claimed) {
      return claimed;
    }
    SE_LOG_INFO("GPU device '{}' (debug mode {}), display {:.3f} Hz",
                gpu_->Driver(), gpu_->DebugMode(), refresh_hz_);
    if (auto started = StartRenderer(); !started) {
      return started;
    }
  }

  // 8. The CPU topology.
  if (auto ok = BootStep(8); !ok) {
    return ok;
  }
  auto cpu = integration::ReadCpuTopology();
  if (!cpu) return std::unexpected(std::move(cpu.error()));
  cpu_ = *cpu;
  SE_LOG_INFO(
      "CPU: {} logical, {} physical cores ({}); fork pool up to {} workers "
      "({} for PerformanceOnly sites), none started",
      cpu_.logical_cores, cpu_.physical_cores,
      cpu_.homogeneous ? "homogeneous" : "hybrid", cpu_.fork_pool_size,
      cpu_.performance_only);

  // 9. The helper threads: the watchdog, with only the boot budget until
  // the first iterate arms it.
  if (auto ok = BootStep(9); !ok) {
    return ok;
  }
  // The App's own flags only: the watchdog runs until shutdown step 17,
  // after the window is gone. The subclass reports Modal changes to the
  // App (OnModalChanged) as it sets its own flag.
  integration::WatchdogInputs inputs;
  inputs.iterate_counter = &iterate_counter_;
  inputs.modal = &modal_flag_;
  inputs.suspended = &suspended_flag_;
  inputs.heartbeat = &heartbeat_flag_;
  auto helpers =
      integration::StartHelperThreads(config_, inputs, boot_start_ns_);
  if (!helpers) return std::unexpected(std::move(helpers.error()));
  helpers_.emplace(std::move(*helpers));

  // 10. The game: registry, storages, states.
  if (auto ok = BootStep(10); !ok) {
    return ok;
  }
  if (auto game = StartGame(); !game) {
    return game;
  }

  quit_.SetRecoveryHook(setup_.recovery);
  quit_.SetDeadlineHook([this] {
    if (helpers_) helpers_->StartShutdownDeadline();
  });
  for (QuitProvider* provider : setup_.quit_providers) {
    quit_.AddProvider(provider);
  }
  const bool focused = window_ == nullptr || window_->Focused();
  const bool visible = window_ == nullptr || window_->Visible();
  lifecycle_.BootComplete(SDL_GetTicksNS(), focused, visible);
  booted_ = true;
  SE_LOG_INFO("booted '{}': {} logical cores, {} Hz ticks, content {}",
              setup_.title, cpu_.logical_cores, setup_.tick_rate,
              paths_.content_root);
  if (setup_.on_boot) setup_.on_boot(*this);
  return {};
}

std::expected<void, Error> App::ResolvePaths() {
  auto base = fs::BasePath();
  if (!base) return std::unexpected(std::move(base.error()));
  paths_.base = *base;
  if (!config_.content_root.empty()) {
    if (!fs::Exists(config_.content_root)) {
      return std::unexpected(Error{ErrorCode::kNotFound,
                                   "--content-root does not exist",
                                   config_.content_root, 0, 0});
    }
    paths_.content_root = fs::Normalize(config_.content_root);
  } else if (!setup_.content_marker.empty()) {
    auto root = fs::FindRootContaining(paths_.base, setup_.content_marker);
    if (!root) return std::unexpected(std::move(root.error()));
    paths_.content_root = *root;
  } else {
    paths_.content_root = fs::Normalize(paths_.base);
  }
  return {};
}

std::expected<void, Error> App::ResolvePrefPath() {
  if (!config_.pref_dir.empty()) {
    if (auto made = fs::CreateDirectories(config_.pref_dir); !made) {
      return made;
    }
    paths_.pref = fs::Normalize(config_.pref_dir) + "/";
  } else {
    auto pref = fs::PrefPath(setup_.org, setup_.app);
    if (!pref) return std::unexpected(std::move(pref.error()));
    paths_.pref = *pref;
  }
  return {};
}

void App::StartProfiler() {
  if (!profile::Available()) {
    SE_LOG_WARN("--profile: this build has no profiler (SE_TRACY is off)");
    return;
  }
  profiling_ = profile::Start();
  if (profiling_) {
    SE_LOG_INFO("profiler started: Tracy listens on localhost for a client");
  } else {
    SE_LOG_WARN("--profile: the profiler did not start");
  }
}

std::expected<void, Error> App::StartGame() {
  StateEnv env;
  env.seed = setup_.seed;
  env.tick_rate = setup_.tick_rate;
  jobs::ForkJoinPool::Config pool_config;
  pool_config.max_workers = cpu_.fork_pool_size;
  game_ = std::make_unique<GameRuntime>(*setup_.module, env, pool_config);
  // Every latched InputFrame goes into the process's ring, a dump stream.
  game_->driver.AttachInputRing(&integration::DumpInputRing());
  for (const std::string& key : game_->input.UnknownKeys()) {
    SE_LOG_WARN("binding to unknown key '{}' ignored", key);
  }
  if (setup_.configure) {
    if (auto configured = setup_.configure(paths_); !configured) {
      return configured;
    }
  }
  const std::string_view start = setup_.module->start_state;
  if (start.empty()) {
    return Fail(ErrorCode::kInvalidArgument,
                std::string(setup_.module->name) + " has no start state");
  }
  const InlineOwnerScope owner(OnMainThread());
  game_->states.Request(StackOp::kPush, start);
  game_->states.ApplyPending();
  if (game_->states.Depth() != 1 || game_->states.RejectedCount() != 0) {
    return Fail(ErrorCode::kInvalidArgument,
                std::format("the start state '{}' could not be pushed", start));
  }
  return {};
}

std::expected<void, Error> App::StartRenderer() {
  auto renderer = Renderer2D::Create(*gpu_);
  if (!renderer) return std::unexpected(std::move(renderer.error()));
  renderer_ = std::move(*renderer);
  if (kRuntimeTestHooks) renderer_->Faults().Configure(config_.frame_fault);
  readbacks_done_.assign(config_.readbacks.size(), false);
  if (setup_.texture_table.empty()) return {};
  auto report = LoadTextureTable(renderer_->Resources(), paths_.content_root,
                                 setup_.texture_table);
  if (!report) return std::unexpected(std::move(report.error()));
  // A texture that does not load draws magenta; the game still boots.
  for (const Error& error : report->errors) {
    SE_LOG_ERROR("{} ({})", error.message, error.path);
  }
  textures_ = std::move(report->entries);
  if (auto flushed = renderer_->Resources().FlushUploads(); !flushed) {
    return flushed;
  }
  SE_LOG_INFO("renderer: {} of {} textures from {} resident", report->loaded,
              textures_.size(), setup_.texture_table);
  return {};
}

Camera2D App::CameraFor(const World& world) const {
  if (setup_.camera) return setup_.camera(world);
  if (const std::optional<Camera2D> primary = PrimaryCamera(world)) {
    return *primary;
  }
  return Camera2D{};
}

Tick App::NextTick() const noexcept {
  return game_ ? game_->driver.NextTick() : 0;
}

void App::RequestQuit() {
  SE_ASSERT_MAIN_THREAD();
  quit_.Request(QuitReason::kRequested);
  SyncQuitState();
}

void App::SyncQuitState() {
  if (quit_.Pending() && !quit_pending_seen_) {
    quit_pending_seen_ = true;
    lifecycle_.OnQuitPending();
    SE_LOG_INFO("quit pending ({})", QuitReasonName(quit_.Reason()));
  } else if (!quit_.Pending() && quit_pending_seen_ && !quit_.ReadyToExit()) {
    quit_pending_seen_ = false;
    lifecycle_.OnQuitCancelled();
    SE_LOG_INFO("quit cancelled by {}", quit_.Blocker());
  }
}

void App::ApplyCallbackRate() {
  const CallbackRate rate = lifecycle_.Rate();
  if (rate == applied_rate_) return;
  applied_rate_ = rate;
  const std::string hint(CallbackRateHint(rate));
  SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, hint.c_str());
}

void App::TakeInbox() {
  for (const SDL_Event& event : inbox_.TryTake()) {
    ++inbox_events_;
    HandleEvent(event);
  }
}

SDL_AppResult App::HandleEvent(const SDL_Event& event) {
  SE_ASSERT_MAIN_THREAD();
  ++events_;
  if (kRuntimeTestHooks && config_.throw_site == ThrowSite::kEvent &&
      static_cast<std::int64_t>(events_) == config_.throw_on_call) {
    InjectedThrow("SDL_AppEvent");
  }
  lifecycle_.OnAnyEvent();
  if (ConsumeWakeEvent(event)) return SDL_APP_CONTINUE;
  const std::uint64_t now = SDL_GetTicksNS();
  const bool ours = window_ != nullptr &&
                    event.type >= SDL_EVENT_WINDOW_FIRST &&
                    event.type <= SDL_EVENT_WINDOW_LAST &&
                    event.window.windowID == window_->Id();
  switch (event.type) {
    case SDL_EVENT_QUIT:
      quit_.Request(QuitReason::kQuitEvent);
      break;
    case SDL_EVENT_TERMINATING:
      SE_LOG_WARN("SDL_EVENT_TERMINATING: running the recovery hook");
      quit_.RequestImmediate(QuitReason::kTerminating);
      std::fflush(stderr);
      break;
    case SDL_EVENT_LOW_MEMORY:
      ++low_memory_events_;
      SE_LOG_WARN("SDL_EVENT_LOW_MEMORY");
      break;
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
    case SDL_EVENT_DID_ENTER_FOREGROUND:
      SE_LOG_INFO("background/foreground event {:#x}", event.type);
      break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      if (ours) quit_.Request(QuitReason::kWindowClose);
      break;
    case SDL_EVENT_WINDOW_SHOWN:
      if (ours) lifecycle_.OnWindow(WindowSignal::kShown, now);
      break;
    case SDL_EVENT_WINDOW_HIDDEN:
      if (ours) lifecycle_.OnWindow(WindowSignal::kHidden, now);
      break;
    case SDL_EVENT_WINDOW_EXPOSED:
      if (ours) lifecycle_.OnWindow(WindowSignal::kExposed, now);
      break;
    case SDL_EVENT_WINDOW_OCCLUDED:
      if (ours) lifecycle_.OnWindow(WindowSignal::kOccluded, now);
      break;
    case SDL_EVENT_WINDOW_MINIMIZED:
      if (ours) lifecycle_.OnWindow(WindowSignal::kMinimized, now);
      break;
    case SDL_EVENT_WINDOW_MAXIMIZED:
      if (ours) lifecycle_.OnWindow(WindowSignal::kMaximized, now);
      break;
    case SDL_EVENT_WINDOW_RESTORED:
      if (ours) lifecycle_.OnWindow(WindowSignal::kRestored, now);
      break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      if (ours) lifecycle_.OnWindow(WindowSignal::kFocusGained, now);
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      if (ours) lifecycle_.OnWindow(WindowSignal::kFocusLost, now);
      break;
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
    case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
      if (window_ != nullptr) refresh_hz_ = window_->Refresh().hz;
      break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
      if (game_) {
        game_->input.OnKey(static_cast<std::uint32_t>(event.key.scancode),
                           event.key.down, event.key.repeat);
      }
      lifecycle_.OnInputActivity(now);
      break;
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      lifecycle_.OnInputActivity(now);
      break;
    default:
      break;
  }
  SyncQuitState();
  return SDL_APP_CONTINUE;
}

std::expected<void, Error> App::RenderFrame(FrameReport& report,
                                            double& wait_sec) {
  report.presented = false;
  if (renderer_ == nullptr) return {};
  if (kRuntimeTestHooks && config_.null_acquire_count > 0) {
    const auto frame = static_cast<std::int64_t>(iterates_);
    if (frame >= config_.null_acquire_from &&
        frame < config_.null_acquire_from + config_.null_acquire_count) {
      gpu_->ForceNullAcquires(1);
    }
  }
  layers_.clear();
  for (const WorldRenderSource& source :
       game_->states.RenderSources(report.plan)) {
    if (source.world == nullptr) continue;
    RenderLayer layer;
    layer.world = source.world;
    layer.camera = CameraFor(*source.world);
    layer.alpha = source.alpha;
    layers_.push_back(layer);
  }
  FrameRequest request;
  request.layers = layers_;
  request.clear = setup_.clear_color;
  renderer_->Faults().SetFrame(iterates_);
  FrameResult result = renderer_->Frame(request);
  report.rendered = result.submitted;
  report.presented = result.presented;
  report.render = renderer_->LastFrame();
  wait_sec = static_cast<double>(report.render.swapchain_wait_ns) * kNsToSec;
  if (result.build_fault) {
    return std::unexpected(std::move(*result.build_fault));
  }
  if (result.error) {
    ++frames_skipped_;
    if (frames_skipped_ == 1 || frames_skipped_ % 120 == 0) {
      SE_LOG_WARN("frame {} not rendered ({} so far): {}", iterates_,
                  frames_skipped_, result.error->message);
    }
  }
  if (result.null_acquire) lifecycle_.OnNullAcquire();
  return {};
}

void App::RunTextureReloads() {
  if (renderer_ == nullptr) return;
  for (const TextureReloadHook& reload : config_.texture_reloads) {
    if (reload.frame != static_cast<std::int64_t>(iterates_)) continue;
    GpuResources& resources = renderer_->Resources();
    const TextureHandle handle = resources.Find(TextureKeyOf(reload.name));
    std::string path;
    if (!reload.file.empty()) {
      path = fs::Join(paths_.content_root, reload.file);
    } else {
      for (const TextureTableEntry& entry : textures_) {
        if (entry.name == reload.name) path = entry.path;
      }
    }
    if (handle.IsNull() || path.empty()) {
      SE_LOG_ERROR("--reload-texture: no texture '{}' in the table",
                   reload.name);
      continue;
    }
    if (auto loaded = LoadTextureFile(resources, handle, path); !loaded) {
      SE_LOG_ERROR("--reload-texture {}: {} ({})", reload.name,
                   loaded.error().message, loaded.error().path);
      continue;
    }
    SE_LOG_INFO("reloading texture '{}' from {} at frame {}", reload.name, path,
                iterates_);
  }
}

void App::RunReadbacks(const FrameReport& report) {
  if (renderer_ == nullptr || !report.rendered) return;
  for (std::size_t i = 0; i < config_.readbacks.size(); ++i) {
    const ReadbackHook& readback = config_.readbacks[i];
    if (readbacks_done_[i] ||
        static_cast<std::int64_t>(iterates_) < readback.frame) {
      continue;
    }
    readbacks_done_[i] = true;
    auto image = renderer_->Readback();
    if (!image) {
      SE_LOG_ERROR("--readback {}: {}", readback.file, image.error().message);
      continue;
    }
    if (auto written = WritePng(readback.file, *image); !written) {
      SE_LOG_ERROR("--readback {}: {}", readback.file, written.error().message);
      continue;
    }
    SE_LOG_INFO("readback of frame {} (tick {}, {}x{}) written to {}",
                iterates_, game_->driver.NextTick() - 1, image->width,
                image->height, readback.file);
  }
}

SDL_AppResult App::Iterate() {
  SE_ASSERT_MAIN_THREAD();
  const std::uint64_t iterate_start = SDL_GetTicksNS();
  iterate_counter_.fetch_add(1, std::memory_order_relaxed);
  ++iterates_;
  if (iterates_ == 1) {
    if (helpers_) helpers_->ArmWatchdog();
    // SDL_AppInit has returned and SDL has added its own event watcher:
    // other threads may push the wake event from now on (ADR-047).
    if (wake_) wake_->Open();
  }
  if (kRuntimeTestHooks && config_.throw_site == ThrowSite::kIterate &&
      static_cast<std::int64_t>(iterates_) == config_.throw_on_call) {
    InjectedThrow("SDL_AppIterate");
  }

  // Events diverted off main by the filter, then the two-phase quit.
  TakeInbox();
  quit_.Poll();
  SyncQuitState();
  if (quit_.ReadyToExit()) {
    lifecycle_.OnShutdownStarted();
    SE_LOG_INFO("quit ({}): exiting after {} iterates",
                QuitReasonName(quit_.Reason()), iterates_);
    return SDL_APP_SUCCESS;
  }

  const std::uint64_t now = SDL_GetTicksNS();
  if (window_ != nullptr) lifecycle_.SetModal(window_->Modal());
  lifecycle_.Update(now);
  ApplyCallbackRate();
  if (lifecycle_.TakeClearHeldInput()) game_->input.ClearHeld();
  if (lifecycle_.TakeFrameClockReset()) {
    clock_.Reset();
    stepper_.ResetAccumulator();
  }
  suspended_flag_.store(lifecycle_.Suspended(), std::memory_order_release);
  modal_flag_.store(lifecycle_.Modal(), std::memory_order_release);

  FrameReport report;
  report.iterate = iterates_;
  report.now_ns = now;
  double wait_sec = 0.0;
  if (kRuntimeTestHooks) RunTextureReloads();
  // While Suspended (10 Hz), probe: a window that is visible again and
  // acquires a texture leaves Suspended.
  if (lifecycle_.WindowSuspended() && !lifecycle_.PowerSuspended() &&
      window_ != nullptr && window_->Visible()) {
    if (auto rendered = RenderFrame(report, wait_sec); !rendered) {
      return OnCallbackFault("render build", rendered.error().message);
    }
    lifecycle_.OnSuspendProbe(true, report.presented);
    if (lifecycle_.TakeFrameClockReset()) {
      clock_.Reset();
      stepper_.ResetAccumulator();
    }
  }

  const FrameClock frame_clock =
      clock_.Next(now, refresh_hz_, lifecycle_.Modal());
  report.gameplay_ran = lifecycle_.GameplayRuns() &&
                        game_->states.SimRunning() && !exit_test_hold_;
  report.plan =
      stepper_.Plan(frame_clock, costs_.Current(), report.gameplay_ran,
                    /*debugger=*/false, /*editor_play=*/false);
  report.first_tick = game_->driver.NextTick();
  if (kRuntimeTestHooks) StopPlanAtExitTestTick(report.plan, report.first_tick);
  const std::uint64_t ticks_start = SDL_GetTicksNS();
  game_->driver.TickInline(game_->states, report.plan, game_->input,
                           game_->session, game_->recorder, game_->audio);
  const double ticks_sec =
      static_cast<double>(SDL_GetTicksNS() - ticks_start) * kNsToSec;
  ticks_run_ += static_cast<std::uint64_t>(report.plan.steps);
  for (int i = 0; i < report.plan.steps; ++i) {
    costs_.AddTick(ticks_sec / report.plan.steps);
  }
  report.next_tick = game_->driver.NextTick();
  if (kRuntimeTestHooks) RunExitTests(report.next_tick);

  if (!report.rendered && lifecycle_.ShouldRender()) {
    if (auto rendered = RenderFrame(report, wait_sec); !rendered) {
      // The game build's policy for a render build fault: the callbacks'
      // fault path (ADR-062).
      SE_LOG_ERROR("frame {}: {}", iterates_, rendered.error().message);
      return OnCallbackFault("render build", rendered.error().message);
    }
  } else if (!report.rendered && renderer_ != nullptr) {
    // Uploads continue while nothing renders (a minimized window).
    renderer_->PumpUploads();
  }
  if (kRuntimeTestHooks) RunReadbacks(report);
  if (report.presented) {
    ++presented_frames_;
    SE_PROFILE_FRAME();
  }
  stepper_.OnPresentResult(/*missed=*/false, lifecycle_.Modal());

  // The non-tick CPU cost R: the iterate without its ticks and without the
  // swapchain wait (ADR-063); the render build, recording and submit
  // count.
  const double busy_sec =
      static_cast<double>(SDL_GetTicksNS() - iterate_start) * kNsToSec;
  const double non_tick_sec = std::max(0.0, busy_sec - ticks_sec - wait_sec);
  costs_.AddFrame(frame_clock.real_delta, non_tick_sec,
                  std::numeric_limits<double>::quiet_NaN());

  report.state = lifecycle_.State();
  report.rate = lifecycle_.Rate();
  report.modal = lifecycle_.Modal();
  report.suspended = lifecycle_.Suspended();
  UpdateTitle(now);
  LogProgress(now);

  // --frames N: presented frames (iterates when headless).
  const std::uint64_t counted = gpu_ != nullptr ? presented_frames_ : iterates_;
  if (config_.frames > 0 &&
      counted >= static_cast<std::uint64_t>(config_.frames)) {
    quit_.Request(QuitReason::kFrames);
    SyncQuitState();
  }
  if (setup_.on_frame) setup_.on_frame(*this, report);
  return SDL_APP_CONTINUE;
}

std::int64_t App::ExitTestTick() const noexcept {
  return config_.crash_test_after_ticks > 0 ? config_.crash_test_after_ticks
                                            : config_.dump_test_after_ticks;
}

void App::StopPlanAtExitTestTick(StepPlan& plan, Tick first_tick) const {
  const std::int64_t target = ExitTestTick();
  if (target <= 0 || exit_test_fired_ || plan.steps <= 0) return;
  const auto first = static_cast<std::int64_t>(first_tick);
  if (first + plan.steps - 1 > target) {
    plan.steps =
        static_cast<int>(std::max<std::int64_t>(0, target - first + 1));
  }
}

void App::RunExitTests(Tick next_tick) {
  const std::int64_t target = ExitTestTick();
  if (target <= 0) return;
  const std::int64_t ran = static_cast<std::int64_t>(next_tick) - 1;
  if (!exit_test_fired_ && ran >= target) {
    exit_test_fired_ = true;
    if (config_.crash_test_after_ticks > 0) {
      SE_LOG_WARN("crash test: an access violation on main after tick {}", ran);
      FaultForCrashTest();
    }
    // The ticks stop here, so the dump shows main at this tick.
    exit_test_hold_ = true;
    dumps_before_test_ = helpers_ ? helpers_->DumpsFinished() : 0;
    SE_LOG_WARN("dump test: the watchdog writes a dump after tick {}", ran);
    if (!helpers_ ||
        !helpers_->RequestDump(std::format("dump test after tick {}", ran))) {
      SE_LOG_ERROR("dump test: the watchdog did not take the request");
      quit_.Request(QuitReason::kRequested);
      SyncQuitState();
    }
    return;
  }
  if (exit_test_hold_ && !quit_.Pending() && helpers_ &&
      helpers_->DumpsFinished() > dumps_before_test_) {
    SE_LOG_INFO("dump test: the watchdog finished the dump");
    quit_.Request(QuitReason::kRequested);
    SyncQuitState();
  }
}

void App::UpdateTitle(std::uint64_t now_ns) {
  if (window_ == nullptr || now_ns - last_title_ns_ < kTitleIntervalNs) return;
  last_title_ns_ = now_ns;
  window_->SetTitle(std::format("{} | tick {} | {}", setup_.title,
                                game_->driver.NextTick() - 1,
                                LifecycleStateName(lifecycle_.State())));
}

void App::LogProgress(std::uint64_t now_ns) {
  if (last_log_ns_ == 0) last_log_ns_ = now_ns;
  if (now_ns - last_log_ns_ < kLogIntervalNs) return;
  last_log_ns_ = now_ns;
  SE_LOG_INFO("tick {} | {} | {} iterates, {} presented, {} NULL acquires",
              game_->driver.NextTick() - 1,
              LifecycleStateName(lifecycle_.State()), iterates_,
              presented_frames_, lifecycle_.NullAcquires());
  if (renderer_ != nullptr) {
    const Renderer2DStats& last = renderer_->LastFrame();
    SE_LOG_DEBUG(
        "render frame {}: {} quads, {} batches, {} draws, {} upload bytes, "
        "build {:.3f} ms, record {:.3f} ms, swapchain wait {:.3f} ms",
        last.frame, last.quads, last.batches, last.draws, last.upload_bytes,
        static_cast<double>(last.build_ns) * 1e-6,
        static_cast<double>(last.record_ns) * 1e-6,
        static_cast<double>(last.swapchain_wait_ns) * 1e-6);
  }
}

SDL_AppResult App::Shutdown(SDL_AppResult result) {
  SE_ASSERT_MAIN_THREAD();
  if (setup_.on_shutdown) {
    auto hook = [this, result] { setup_.on_shutdown(*this, result); };
    if (!RunGameHook("GameSetup::on_shutdown", hook)) {
      shutdown_faulted_ = true;
    }
  }
  SE_LOG_INFO(
      "shutdown ({}): {} iterates, {} presented frames, ran {} ticks "
      "(next tick {}), {} NULL acquires, {} inbox events",
      result == SDL_APP_FAILURE ? "failure" : "success", iterates_,
      presented_frames_, ticks_run_, NextTick(), lifecycle_.NullAcquires(),
      inbox_events_);
  // The renderer's totals and leak check, while logging still runs; the
  // teardown below only destroys it.
  if (renderer_) renderer_->Shutdown();
  Teardown();
  return shutdown_faulted_ ? SDL_APP_FAILURE : result;
}

void App::Teardown() {
  if (torn_down_) return;
  lifecycle_.OnShutdownStarted();
  // Step 3: normally started when the quit drain ended.
  if (booted_ && !quit_.DeadlineStarted() && helpers_) {
    helpers_->StartShutdownDeadline();
  }
  // Step 4: no more wake pushes from other threads (they read the watch
  // list the filter removal changes), no more diverted events; the GPU
  // finishes its work.
  if (wake_) wake_->Close();
  if (filter_) filter_->Remove();
  if (gpu_) gpu_->WaitForIdle();
  // Steps 5-6: every World, on main. A throwing OnExit leaves its state
  // and the ones below it on the stack; destroying the GameRuntime destroys
  // them (and their Worlds) without another OnExit.
  if (game_) {
    auto clear = [this] {
      AssertMainThread();
      const InlineOwnerScope owner(OnMainThread());
      game_->states.Clear();
    };
    if (!RunGameHook("GameState::OnExit", clear)) shutdown_faulted_ = true;
    game_.reset();
  }
  // Step 12: the renderer's objects (the GPU is idle), then release the
  // window from the device and destroy the device.
  renderer_.reset();
  gpu_.reset();
  // Step 14: remove the subclass, destroy the window.
  if (window_) window_->SetListener(nullptr);
  window_.reset();
  wake_.reset();
  filter_.reset();
  SDL_ResetHint(SDL_HINT_MAIN_CALLBACK_RATE);
  // Step 15: SDL_QuitSubSystem(GAMEPAD | VIDEO).
  sdl_.reset();
  // Step 16: SDL's log output to stderr, then drain and stop the log
  // worker (spdlog::shutdown); SE_LOG_* is synchronous from here on.
  if (logging_) logging_->Stop();
  // Step 17: the watchdog, the last engine thread.
  if (helpers_) helpers_->Stop();
  helpers_.reset();
  logging_.reset();
  // Every profiled engine thread has been joined.
  if (profiling_) {
    profile::Stop();
    profiling_ = false;
  }
  // Crash capture stays until the process exits (integration.h).
  crash_.reset();
  lifecycle_.OnExited();
  torn_down_ = true;
}

SDL_AppResult App::OnCallbackFault(std::string_view where,
                                   std::string_view what) noexcept {
  integration::RequestFaultDump(where);
  ShowFaultMessage(setup_.title, where, what,
                   window_ != nullptr ? window_->Native() : nullptr);
  lifecycle_.OnShutdownStarted();
  return SDL_APP_FAILURE;
}

void App::OnSessionEnd(SessionEnd kind) noexcept {
  // Synchronous: the recovery hook runs before the window procedure
  // returns to Windows.
  quit_.RequestImmediate(QuitReason::kSessionEnd);
  std::fflush(stderr);
  static_cast<void>(kind);
}

void App::OnPower(PowerChange change) noexcept {
  if (change == PowerChange::kSuspend) {
    lifecycle_.OnPowerSuspend();
  } else {
    lifecycle_.OnPowerResume();
  }
  suspended_flag_.store(lifecycle_.Suspended(), std::memory_order_release);
}

void App::OnModalChanged(bool modal) noexcept {
  lifecycle_.SetModal(modal);
  modal_flag_.store(modal, std::memory_order_release);
}

}  // namespace se
