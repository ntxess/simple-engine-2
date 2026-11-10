// Copyright 2026 The simple-engine-2 Authors.

#include "se_runtime/app.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_version.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_assets/background_lane.h"
#include "se_assets/lane_jobs.h"
#include "se_audio/audio_engine.h"
#include "se_audio/pcm.h"
#include "se_core/error.h"
#include "se_core/input_frame.h"
#include "se_core/jobs.h"
#include "se_core/log.h"
#include "se_core/profile.h"
#include "se_core/thread_role.h"
#include "se_core/time.h"
#include "se_core/world.h"
#include "se_platform/display_output.h"
#include "se_platform/fatal.h"
#include "se_platform/fs.h"
#include "se_platform/fs_faults.h"
#include "se_platform/gamepads.h"
#include "se_platform/inbox.h"
#include "se_platform/input_sampler.h"
#include "se_platform/sdl_context.h"
#include "se_platform/vblank_sampler.h"
#include "se_platform/wake.h"
#include "se_platform/window.h"
#include "se_render/frame_recorder.h"
#include "se_render/gpu_device.h"
#include "se_render/image_io.h"
#include "se_render/renderer_2d.h"
#include "se_render/texture_table.h"
#include "se_render_core/render_components.h"
#include "se_render_core/render_list.h"
#include "se_runtime/action_map.h"
#include "se_runtime/app_config.h"
#include "se_runtime/audio_bridge.h"
#include "se_runtime/frame_pacer.h"
#include "se_runtime/input_service.h"
#include "se_runtime/integration.h"
#include "se_runtime/lifecycle.h"
#include "se_runtime/pacer_policy.h"
#include "se_runtime/quit.h"
#include "se_runtime/render_present_port.h"
#include "se_runtime/sound_assets.h"
#include "se_runtime/user_settings.h"
#include "se_sim/audio_events.h"
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
              const jobs::ForkJoinPool::Config& pool_config, ActionMap map,
              const InputServiceConfig& input_config,
              audio::AudioEngine* engine)
      : instance(module),
        states(instance.Factory(), WithRegistry(env, instance)),
        pool(pool_config),
        driver(instance.MutableSchedule(), pool),
        audio(engine),
        input(std::move(map), input_config) {}
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
  // The Worlds' audio events onto the engine (M8).
  AudioBridge audio;
  InputService input;
};

namespace {

constexpr std::uint64_t kTitleIntervalNs = 250'000'000;
constexpr std::uint64_t kLogIntervalNs = 2'000'000'000;
constexpr double kNsToSec = 1e-9;
// A failed device recreation is retried this often, this many times.
constexpr std::uint64_t kRecreateRetryNs = 1'000'000'000;
constexpr std::uint32_t kMaxRecreateAttempts = 5;

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

// --audio, else SE_AUDIO (device, null or none), else the device (the null
// backend for a headless app); nullopt: no audio engine.
std::optional<audio::Output> AudioOutputFor(const AppConfig& config) {
  AudioRequest request = config.audio;
  if (request == AudioRequest::kDefault) {
    const char* value = SDL_getenv("SE_AUDIO");
    const std::string_view env = value != nullptr ? value : "";
    if (env == "device") {
      request = AudioRequest::kDevice;
    } else if (env == "null") {
      request = AudioRequest::kNull;
    } else if (env == "none") {
      request = AudioRequest::kNone;
    } else {
      request = config.headless ? AudioRequest::kNull : AudioRequest::kDevice;
    }
  }
  switch (request) {
    case AudioRequest::kNone:
      return std::nullopt;
    case AudioRequest::kNull:
      return audio::Output::kNull;
    default:
      return audio::Output::kDevice;
  }
}

FramePacerConfig PacerConfigFrom(const AppConfig& config) {
  FramePacerConfig pacer;
  pacer.request = config.pacing;
#if defined(_WIN32)
  // Paced is the Windows runtime default; elsewhere it is not claimed
  // until measured.
  pacer.paced_by_default = true;
#endif
  pacer.fixed_frame_dt_sec = config.fixed_frame_dt_ms * 1e-3;
  return pacer;
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
      pacer_(PacerConfigFrom(config)),
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
  sdl_config.raw_keyboard = config_.raw_keyboard;
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
    UpdateDisplay(/*force=*/true);
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
    present_ = std::make_unique<RenderPresentPort>(*gpu_, *renderer_);
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
  integration::VblankStart vblank;
  vblank.enabled = window_ != nullptr;
  vblank.monitor = monitor_;
  auto helpers =
      integration::StartHelperThreads(config_, inputs, boot_start_ns_, vblank);
  if (!helpers) return std::unexpected(std::move(helpers.error()));
  helpers_.emplace(std::move(*helpers));
  // M9: the background lane and the AssetRegistry.
  if (auto assets = StartAssets(); !assets) {
    return assets;
  }

  // 10. The game: the settings file, then registry, storages, states.
  if (auto ok = BootStep(10); !ok) {
    return ok;
  }
  if (auto settings = LoadSettings(); !settings) {
    return settings;
  }
  StartAudio();
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
  // Durable lane jobs keep the quit pending until they are done.
  quit_.AddProvider(&assets_->QuitWaiter());
  // The pacer: the present port and the vblank sampler; it chooses the
  // mode (Standard until the sampler has measured, when Paced is possible).
  pacer_.SetDisplayRefresh(PacingRefreshHz());
  pacer_.Attach(present_.get(), MakeVblankReader());
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
  paths_.cooked = config_.cooked_root.empty()
                      ? fs::Join(paths_.base, "cooked")
                      : fs::Normalize(config_.cooked_root);
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
  // The action map: the module's bindings and the settings file's keys;
  // an unknown key is a boot error naming it (M8).
  auto map = ActionMap::Build(setup_.module->actions, settings_);
  if (!map) {
    Error error = std::move(map.error());
    error.path = setup_.settings_file.empty()
                     ? std::string(setup_.module->name)
                     : fs::Join(paths_.content_root, setup_.settings_file);
    return std::unexpected(std::move(error));
  }
  for (const ActionMap::Binding& binding : map->Bindings()) {
    SE_LOG_DEBUG("action '{}': key '{}'{}, pad '{}'", binding.action,
                 binding.key, binding.from_settings ? " (settings)" : "",
                 binding.pad);
  }
  InputServiceConfig input_config;
#if defined(_WIN32)
  input_config.key_source =
      config_.raw_keyboard ? EdgeSource::kRawKeyboard : EdgeSource::kWinMessage;
#else
  input_config.key_source = EdgeSource::kWinMessage;
#endif
  game_ = std::make_unique<GameRuntime>(*setup_.module, env, pool_config,
                                        std::move(*map), input_config,
                                        audio_.get());
  // Every latched InputFrame goes into the process's ring, a dump stream.
  game_->driver.AttachInputRing(&integration::DumpInputRing());
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
  {
    const InlineOwnerScope owner(OnMainThread());
    game_->states.Request(StackOp::kPush, start);
    game_->states.ApplyPending();
  }
  if (game_->states.Depth() != 1 || game_->states.RejectedCount() != 0) {
    return Fail(ErrorCode::kInvalidArgument,
                std::format("the start state '{}' could not be pushed", start));
  }
  // --record: every tick's InputFrame (edges and their sources included)
  // and hash, written at shutdown (M8).
  if (!config_.record_path.empty()) {
    std::vector<std::byte> start_bytes;
    if (setup_.replay_start) start_bytes = setup_.replay_start(paths_);
    game_->recorder.Begin(
        MakeReplayHeader(start_bytes, setup_.seed, setup_.tick_rate),
        start_bytes);
  }
  // The start state's scope requests its assets now; no tick runs until
  // they are resident (M9).
  game_->driver.SetTickGate(assets_.get());
  assets_->BeginFrame(game_->states);
  return {};
}

std::expected<void, Error> App::StartAssets() {
  AssetRuntimeConfig assets;
  assets.content_root = paths_.content_root;
  assets.cooked_root = paths_.cooked;
  assets.textures = texture_table_;
  assets.page_mode = kReleaseBuild ? AtlasRequest::Mode::kPage
                                   : AtlasRequest::Mode::kPageIfFresh;
  assets.hot_reload = config_.hot_reload;
  if (kRuntimeTestHooks && !config_.lane_step_trace.empty()) {
    assets.trace_capacity = 1U << 18;
  }
  assets.state_assets = setup_.state_assets;
  // The sound table: its files are every state's default sounds, decoded
  // on the lane (ADR-087). Without it the game runs silent, as in M8; with
  // --audio none no sound is requested, decoded or waited for.
  if (!setup_.sound_table.empty() && AudioOutputFor(config_).has_value()) {
    auto table = LoadSoundTable(paths_.content_root, setup_.sound_table);
    if (table) {
      sound_table_ = std::move(*table);
    } else {
      SE_LOG_ERROR("sound table: {} ({})", table.error().message,
                   table.error().path);
    }
  }
  for (const SoundTableEntry& sound : sound_table_) {
    assets.sounds.push_back(sound.file);
  }
  auto runtime = AssetRuntime::Start(assets, renderer_.get());
  if (!runtime) return std::unexpected(std::move(runtime.error()));
  assets_ = std::move(*runtime);
  return {};
}

std::expected<void, Error> App::LoadSettings() {
  if (setup_.settings_file.empty()) return {};
  const std::string path = fs::Join(paths_.content_root, setup_.settings_file);
  auto settings = LoadUserSettings(path);
  if (!settings) return std::unexpected(std::move(settings.error()));
  settings_ = std::move(*settings);
  SE_LOG_INFO(
      "settings {}: {} key bindings, music volume {:.0f}, effects volume "
      "{:.0f}",
      path, settings_.key_bindings.size(), settings_.music_volume,
      settings_.effects_volume);
  return {};
}

void App::StartInputSampler() {
  if (input_sampler_ || !sdl_) return;
  InputSamplerConfig sampler_config;
  static_cast<void>(power_.Update(SDL_GetTicksNS()));
  sampler_rate_ = SamplerRateFor(power_.OnBattery(), gamepads_.Count());
  sampler_config.rate_hz = sampler_rate_;
  auto sampler = InputSampler::Start(std::move(sampler_config));
  if (!sampler) {
    SE_LOG_WARN("no input sampler, gamepads are not updated: {}",
                sampler.error().message);
    return;
  }
  input_sampler_ = std::move(*sampler);
  SE_LOG_INFO("input sampler: SDL_UpdateGamepads at {} Hz ({}, {} gamepads)",
              input_sampler_->Stats().rate_hz,
              power_.OnBattery() ? "on battery" : "on AC power",
              gamepads_.Count());
}

void App::UpdateInputSampler(std::uint64_t now_ns) {
  if (!input_sampler_) return;
  // Parked while Suspended or Idle (an atomic doorbell; never blocks).
  input_sampler_->SetParked(lifecycle_.Suspended() || lifecycle_.Idle());
  // Main reads the power source every 5 s; the rate follows it and the
  // open gamepads (20 Hz without one, ADR-090).
  static_cast<void>(power_.Update(now_ns));
  const std::uint32_t rate =
      SamplerRateFor(power_.OnBattery(), gamepads_.Count());
  if (rate != sampler_rate_) {
    sampler_rate_ = rate;
    input_sampler_->SetRate(rate);
    SE_LOG_INFO("input sampler: {} Hz ({}, {} gamepads)", rate,
                power_.OnBattery() ? "on battery" : "on AC power",
                gamepads_.Count());
  }
}

void App::StartAudio() {
  audio::EngineConfig audio_config;
  const std::optional<audio::Output> output = AudioOutputFor(config_);
  if (!output) {
    SE_LOG_INFO("audio: none (--audio none)");
  } else {
    audio_config.output = *output;
    audio_config.group_volume = {
        static_cast<float>(settings_.music_volume / 100.0),
        static_cast<float>(settings_.effects_volume / 100.0)};
    auto engine = audio::AudioEngine::Create(audio_config);
    if (engine) {
      audio_ = std::move(*engine);
    } else {
      SE_LOG_WARN("audio: no engine, the game runs silent: {}",
                  engine.error().message);
    }
  }
  // The sounds decode on the background lane at the engine's format (its
  // default when the engine did not start); the registry hands each one to
  // the engine (ADR-087).
  const std::uint32_t rate =
      audio_ ? audio_->SampleRate() : audio_config.sample_rate;
  const std::uint32_t channels =
      audio_ ? audio_->Channels() : audio_config.channels;
  sound_decoder_ = std::make_unique<LaneSoundDecoder>(rate, channels);
  if (!assets_) return;
  assets_->Registry().SetSoundDecoder(sound_decoder_.get());
  assets_->SetSoundListener(
      [this](std::string_view path, std::shared_ptr<const Pcm> pcm) {
        AssertMainThread();
        OnSound(path, std::move(pcm));
      });
  if (!sound_table_.empty()) {
    SE_LOG_INFO(
        "audio: {} sounds in {}, decoded on the background lane at {} Hz, "
        "{} channels",
        sound_table_.size(), setup_.sound_table, rate, channels);
  }
}

void App::OnSound(std::string_view path, std::shared_ptr<const Pcm> pcm) {
  const auto entry =
      std::ranges::find_if(sound_table_, [&](const SoundTableEntry& sound) {
        return fs::Join(paths_.content_root, sound.file) == path;
      });
  // A state's own sound outside the table has no key to play it by.
  if (entry == sound_table_.end()) return;
  if (pcm == nullptr) {
    SE_LOG_DEBUG("sound '{}' released", entry->name);
    if (audio_) audio_->SetSound(entry->key, nullptr);
    return;
  }
  const bool reload = audio_ && audio_->Sound(entry->key) != nullptr;
  SE_LOG_INFO("sound '{}' {}: {} frames at {} Hz, decoded on the lane",
              entry->name, reload ? "reloaded" : "resident", pcm->frames,
              pcm->sample_rate);
  // New voices get the new PCM; playing ones keep theirs until they end.
  if (audio_) audio_->SetSound(entry->key, std::move(pcm));
}

std::uint64_t App::AudioDelay() const noexcept {
  return game_ ? game_->audio.DelayNs() : 0;
}

void App::BeginAudioFrame(const StepPlan& plan, const FrameClock& clock) {
  // D = B + max(T/2, L) (ADR-093): the plan's budget and cadence, the
  // display's vblank period of this frame, which FramePacer::BeginFrame has
  // just set, and the frame period.
  double vblank_sec = pacer_.LastFrame().vblank_period_sec;
  if (!(vblank_sec > 0.0)) {
    vblank_sec = clock.period_sec / std::max(1, pacer_.PresentInterval());
  }
  game_->audio.BeginFrame(
      SDL_GetTicksNS(),
      PlanAudioDelayNs(plan, vblank_sec, clock.period_sec,
                       1.0 / static_cast<double>(setup_.tick_rate)));
  // Pan follows the camera of the first running World.
  const InlineOwnerScope owner(OnMainThread());
  for (const World* world : game_->states.RunningWorlds()) {
    const Camera2D camera = CameraFor(*world);
    const float zoom = camera.zoom > 0.0F ? camera.zoom : 1.0F;
    game_->audio.SetListener(camera.center.x,
                             camera.virtual_size.x * 0.5F / zoom);
    break;
  }
}

void App::WriteReplay() {
  if (!game_ || !game_->recorder.Active()) return;
  game_->recorder.RecordSession(game_->session);
  const std::size_t frames = game_->recorder.FrameCount();
  std::vector<std::byte> bytes = game_->recorder.Finish();
  if (!assets_) {
    if (auto written = fs::WriteFile(config_.record_path, bytes); !written) {
      SE_LOG_WARN("--record {}: {}", config_.record_path,
                  written.error().message);
      return;
    }
    SE_LOG_INFO("replay of {} ticks written to {}", frames,
                config_.record_path);
    return;
  }
  // Through the lane (ADR-089): a Background save marked Durable, which
  // shutdown step 8 runs to the end before it joins the lane.
  const std::size_t size = bytes.size();
  SaveRequest save;
  save.path = config_.record_path;
  save.bytes = std::move(bytes);
  save.revision = 1;
  save.durable = true;
  replay_failures_before_ = assets_->Lane().DurableFailures();
  SubmitSave(assets_->Lane(), std::move(save), JobClass::kBackground);
  replay_submitted_ = true;
  replay_ticks_ = frames;
  SE_LOG_INFO("replay of {} ticks ({} bytes) to {} through the background lane",
              frames, size, config_.record_path);
}

void App::RunDurableSaveTest() {
  const DurableSaveTestHook& test = config_.durable_save_test;
  if (durable_save_test_done_ || !assets_ ||
      static_cast<std::int64_t>(iterates_) != test.frame) {
    return;
  }
  durable_save_test_done_ = true;
  if constexpr (!fs::kFsFaultHooks) {
    SE_LOG_ERROR("--durable-save-test: the se::fs injector is compiled out");
    return;
  }
  // A throttled disk: every write step of the lane waits first.
  fs::SetFsFaults(fs::FsFaults{.write_step_delay_ms = test.step_ms});
  const std::size_t size = std::size_t{test.kib} * 1024;
  std::string text = R"({"format": "se-durable-save-test-v1", "padding": ")";
  const std::string_view tail = "\"}\n";
  if (size > text.size() + tail.size()) {
    text.append(size - text.size() - tail.size(), 'x');
  }
  text += tail;
  SaveRequest save;
  save.path = fs::Join(paths_.pref, "durable_save_test.json");
  durable_save_test_path_ = save.path;
  save.bytes.resize(text.size());
  std::memcpy(save.bytes.data(), text.data(), text.size());
  save.revision = 1;
  save.durable = true;
  SE_LOG_INFO(
      "durable save test: {} bytes to {} at frame {}, {} ms per write step",
      save.bytes.size(), save.path, iterates_, test.step_ms);
  SubmitSave(assets_->Lane(), std::move(save), JobClass::kBackground);
  RequestQuit();
}

void App::NoteTeardown(std::string_view step) noexcept {
  if (teardown_count_ < teardown_steps_.size()) {
    teardown_steps_[teardown_count_++] = step;
  }
}

void App::RunPushKeys(const FrameReport& report) {
  for (const PushKeyHook& hook : config_.push_keys) {
    if (hook.frame != static_cast<std::int64_t>(iterates_)) continue;
    const std::uint32_t keycode = KeycodeFromName(hook.key);
    if (keycode == 0) {
      SE_LOG_ERROR("--push-key: unknown key '{}'", hook.key);
      continue;
    }
    SDL_Event event{};
    event.type = hook.down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.timestamp =
        report.clock.frame_start_ns +
        static_cast<std::uint64_t>(std::llround(hook.offset_ms * 1e6));
    event.key.windowID = window_ != nullptr ? window_->Id() : 0;
    event.key.key = keycode;
    event.key.scancode = SDL_GetScancodeFromKey(keycode, nullptr);
    event.key.down = hook.down;
    event.key.repeat = false;
    if (!SDL_PushEvent(&event)) {
      SE_LOG_ERROR("--push-key: SDL_PushEvent failed");
      continue;
    }
    SE_LOG_INFO("pushed key '{}' {} stamped {} (frame {} start + {:.3f} ms)",
                hook.key, hook.down ? "down" : "up", event.key.timestamp,
                iterates_, hook.offset_ms);
  }
}

std::expected<void, Error> App::StartRenderer() {
  auto renderer = Renderer2D::Create(*gpu_);
  if (!renderer) return std::unexpected(std::move(renderer.error()));
  renderer_ = std::move(*renderer);
  if (kRuntimeTestHooks) renderer_->Faults().Configure(config_.frame_fault);
  readbacks_done_.assign(config_.readbacks.size(), false);
  if (setup_.texture_table.empty()) return {};
  // The table only: its textures load from cooked pages through the
  // background lane (M9), requested by the states' AssetScopes.
  const std::string table = fs::Join(paths_.content_root, setup_.texture_table);
  auto text = fs::ReadTextFile(table);
  if (!text) {
    Error error = std::move(text.error());
    if (error.path.empty()) error.path = table;
    return std::unexpected(std::move(error));
  }
  auto entries = ParseTextureTable(*text, table);
  if (!entries) return std::unexpected(std::move(entries.error()));
  texture_table_ = std::move(*entries);
  textures_.clear();
  for (const TextureTableEntry& entry : texture_table_) {
    textures_.push_back(
        {entry.name, fs::Join(paths_.content_root, entry.path)});
  }
  SE_LOG_INFO("renderer: {} textures in {}; pages from {}", textures_.size(),
              setup_.texture_table, paths_.cooked);
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
    quit_requested_ns_ = SDL_GetTicksNS();
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
      UpdateDisplay(/*force=*/true);
      break;
    case SDL_EVENT_WINDOW_MOVED:
      if (ours) UpdateDisplay(/*force=*/false);
      break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      // Every input event with its SDL timestamp (M8).
      if (game_) game_->input.HandleEvent(event);
      lifecycle_.OnInputActivity(now);
      break;
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_TEXT_INPUT:
      lifecycle_.OnInputActivity(now);
      break;
    case SDL_EVENT_GAMEPAD_ADDED:
      // Open, so that SDL reports its buttons and axes (M8).
      gamepads_.HandleEvent(event);
      break;
    case SDL_EVENT_GAMEPAD_REMOVED:
      // Whatever it held is released first.
      if (game_) game_->input.HandleEvent(event);
      gamepads_.HandleEvent(event);
      break;
    default:
      break;
  }
  SyncQuitState();
  return SDL_APP_CONTINUE;
}

double App::PacingRefreshHz() const noexcept {
  return display_mode_.known && display_mode_.hz > 0.0 ? display_mode_.hz
                                                       : refresh_hz_;
}

void App::UpdateDisplay(bool force) {
  if (window_ == nullptr) return;
  const MonitorHandle monitor = MonitorOfWindow(window_->NativeHandle());
  const double sdl_hz = window_->Refresh().hz;
  const bool moved = monitor != monitor_;
  if (!force && !moved && sdl_hz == refresh_hz_) return;
  refresh_hz_ = sdl_hz;
  monitor_ = monitor;
  display_mode_ = QueryMonitorMode(monitor);
  SE_LOG_INFO("display: SDL reports {:.3f} Hz; the OS reports {}", refresh_hz_,
              DescribeMonitorMode(display_mode_));
  // The sampler re-resolves the output on its next wake (a release store).
  if (helpers_ && helpers_->Vblank() != nullptr) {
    helpers_->Vblank()->SetMonitor(monitor);
  }
  if (booted_) {
    pacer_.SetDisplayRefresh(PacingRefreshHz());
    pacer_.OnOutputChanged();
  }
}

VblankReader App::MakeVblankReader() const {
  if (!helpers_ || helpers_->Vblank() == nullptr) return {};
  const VblankSampler* sampler = helpers_->Vblank();
  return [sampler](std::uint64_t now_ns) { return sampler->Read(now_ns); };
}

std::expected<void, Error> App::CheckDevice() {
  if (!lifecycle_.DeviceLost()) {
    if (present_ == nullptr) return {};
    if (kRuntimeTestHooks && config_.device_lost_at > 0 &&
        !device_lost_injected_ &&
        static_cast<std::int64_t>(iterates_) >= config_.device_lost_at) {
      // The --inject-device-lost hook: the status reports removed once.
      device_lost_injected_ = true;
      SE_LOG_WARN("injecting a device loss at iterate {}", iterates_);
      present_->InjectDeviceLost();
    }
    const bool removed =
        std::exchange(device_removed_, false) ||
        present_->QueryDeviceStatus() == GpuDeviceStatus::kRemoved;
    if (!removed) return {};
    EnterDeviceLost();
  }
  const std::uint64_t now = SDL_GetTicksNS();
  if (now < next_recreate_ns_) return {};
  auto recreated = RecreateDevice();
  if (!recreated) {
    ++recreate_failures_;
    SE_LOG_ERROR("GPU device recreation failed ({} of {}): {}",
                 recreate_failures_, kMaxRecreateAttempts,
                 recreated.error().message);
    if (recreate_failures_ >= kMaxRecreateAttempts) {
      return std::unexpected(std::move(recreated.error()));
    }
    next_recreate_ns_ = now + kRecreateRetryNs;
    return {};
  }
  recreate_failures_ = 0;
  ++device_recoveries_;
  lifecycle_.OnDeviceRecovered();
  SE_LOG_INFO(
      "device recovered: '{}' recreated and {} textures re-uploaded in "
      "{:.1f} ms (recovery {}); Running",
      gpu_->Driver(), textures_.size(),
      static_cast<double>(SDL_GetTicksNS() - device_lost_ns_) * 1e-6,
      device_recoveries_);
  return {};
}

void App::EnterDeviceLost() {
  device_lost_ns_ = SDL_GetTicksNS();
  SE_LOG_ERROR(
      "GPU device '{}' removed (iterate {}): DeviceLost; releasing every GPU "
      "object",
      gpu_->Driver(), iterates_);
  lifecycle_.OnDeviceLost();
  // The pacer lets go of the port; the renderer's objects go through the
  // retire queue (the GPU idle first); the window is released so that the
  // new device can claim it. The old device lives until the new one does.
  pacer_.Detach();
  present_.reset();
  renderer_->ReleaseGpuObjects();
  gpu_->ReleaseWindow();
}

std::expected<void, Error> App::RecreateDevice() {
  GpuDeviceConfig gpu_config;
  gpu_config.driver = std::string(gpu_->Driver());
  auto device = GpuDevice::Create(gpu_config);
  if (!device) return std::unexpected(std::move(device.error()));
  auto fresh = std::make_unique<GpuDevice>(std::move(*device));
  if (auto claimed = fresh->ClaimWindow(window_->Native()); !claimed) {
    return claimed;
  }
  if (auto objects = renderer_->RecreateGpuObjects(*fresh); !objects) {
    // The renderer now refers to the new device: keep it, release what
    // was made and try again later.
    renderer_->ReleaseGpuObjects();
    fresh->ReleaseWindow();
    gpu_ = std::move(fresh);
    return objects;
  }
  gpu_ = std::move(fresh);
  present_ = std::make_unique<RenderPresentPort>(*gpu_, *renderer_);
  pacer_.Attach(present_.get(), MakeVblankReader());
  return {};
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
  // A tick may have changed the stack: the scopes follow it before any
  // World is asked about (M9).
  if (assets_) assets_->BeforeRender(game_->states);
  for (const WorldRenderSource& source :
       game_->states.RenderSources(report.plan)) {
    if (source.world == nullptr) continue;
    // A state whose assets are still loading is not drawn yet (M9).
    if (assets_ && !assets_->Drawable(source.world)) continue;
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
  if (result.submit_failed && present_ != nullptr &&
      present_->QueryDeviceStatus() == GpuDeviceStatus::kRemoved) {
    // The next iterate enters DeviceLost.
    device_removed_ = true;
  }
  return {};
}

void App::RunTextureReloads() {
  if (renderer_ == nullptr || !assets_) return;
  for (const TextureReloadHook& reload : config_.texture_reloads) {
    if (reload.frame != static_cast<std::int64_t>(iterates_)) continue;
    // Re-cooked on the lane (Urgent); it swaps at a frame start once
    // resident.
    if (!assets_->ReloadTexture(reload.name, reload.file)) {
      SE_LOG_ERROR("--reload-texture: no texture '{}' loaded", reload.name);
      continue;
    }
    SE_LOG_INFO("reloading texture '{}'{} on the lane at frame {}", reload.name,
                reload.file.empty() ? std::string() : " from " + reload.file,
                iterates_);
  }
}

void App::RunReadbacks(const FrameReport& report) {
  if (renderer_ == nullptr || !report.rendered) return;
  for (std::size_t i = 0; i < config_.readbacks.size(); ++i) {
    const ReadbackHook& readback = config_.readbacks[i];
    const auto ran = static_cast<std::int64_t>(report.next_tick) - 1;
    const bool due = readback.by_tick ? ran >= readback.frame
                                      : static_cast<std::int64_t>(iterates_) >=
                                            readback.frame;
    if (readbacks_done_[i] || !due) continue;
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
    // The input sampler pushes gamepad events: inside the same window.
    StartInputSampler();
  }
  if (kRuntimeTestHooks && config_.throw_site == ThrowSite::kIterate &&
      static_cast<std::int64_t>(iterates_) == config_.throw_on_call) {
    InjectedThrow("SDL_AppIterate");
  }

  // Events diverted off main by the filter, the background lane's
  // completions (M9), then the two-phase quit.
  TakeInbox();
  if (assets_) assets_->BeginFrame(game_->states);
  quit_.Poll();
  SyncQuitState();
  if (quit_.ReadyToExit()) {
    lifecycle_.OnShutdownStarted();
    if (quit_requested_ns_ != 0) {
      SE_LOG_INFO(
          "quit ready after {:.1f} ms",
          static_cast<double>(SDL_GetTicksNS() - quit_requested_ns_) * 1e-6);
    }
    SE_LOG_INFO("quit ({}): exiting after {} iterates",
                QuitReasonName(quit_.Reason()), iterates_);
    return SDL_APP_SUCCESS;
  }

  const std::uint64_t now = SDL_GetTicksNS();
  if (window_ != nullptr) lifecycle_.SetModal(window_->Modal());
  lifecycle_.Update(now);
  // The device status at frame start: DeviceLost recreates the device here
  // on main (ADR-068).
  if (auto device = CheckDevice(); !device) {
    return OnCallbackFault("GPU device recreation", device.error().message);
  }
  ApplyCallbackRate();
  if (lifecycle_.TakeClearHeldInput()) game_->input.ClearHeld(now);
  if (lifecycle_.TakeFrameClockReset()) {
    pacer_.ResetClock();
    stepper_.ResetAccumulator();
  }
  suspended_flag_.store(lifecycle_.Suspended(), std::memory_order_release);
  modal_flag_.store(lifecycle_.Modal(), std::memory_order_release);
  UpdateInputSampler(now);

  FrameReport report;
  report.iterate = iterates_;
  report.now_ns = now;
  double wait_sec = 0.0;
  const bool device_ok = !lifecycle_.DeviceLost();
  if (kRuntimeTestHooks && device_ok) RunTextureReloads();
  // While Suspended (10 Hz), probe: a window that is visible again and
  // acquires a texture leaves Suspended.
  if (device_ok && lifecycle_.WindowSuspended() &&
      !lifecycle_.PowerSuspended() && window_ != nullptr &&
      window_->Visible()) {
    if (auto rendered = RenderFrame(report, wait_sec); !rendered) {
      return OnCallbackFault("render build", rendered.error().message);
    }
    lifecycle_.OnSuspendProbe(true, report.presented);
    if (lifecycle_.TakeFrameClockReset()) {
      pacer_.ResetClock();
      stepper_.ResetAccumulator();
    }
  }

  const bool probe_rendered = report.rendered;

  // The N-L fence, present statistics into hits and misses (reported to
  // the stepper), and the FrameClock.
  const FrameClock frame_clock =
      pacer_.BeginFrame(lifecycle_.Modal(), stepper_);
  report.clock = frame_clock;
  // The probe's frame went out before BeginFrame (it decides whether this
  // iterate leaves Suspended): report it to this frame like any other, so
  // that the pacer counts it and EndFrame waits for its present (ADR-071).
  if (probe_rendered) {
    pacer_.OnFrameSubmitted(report.presented, report.render.null_acquire);
  }
  // The N-L fence wait is GPU time, not CPU work: it stays out of R.
  const double fence_wait_sec =
      static_cast<double>(pacer_.LastFrame().fence_wait_ns) * kNsToSec;
  // The sim holds while the top state's AssetScope is not resident (M9).
  const bool assets_held = assets_ && assets_->Hold(game_->states);
  report.gameplay_ran = lifecycle_.GameplayRuns() &&
                        game_->states.SimRunning() && !exit_test_hold_ &&
                        !assets_held;
  report.plan =
      stepper_.Plan(frame_clock, costs_.Current(), report.gameplay_ran,
                    /*debugger=*/false, /*editor_play=*/false);
  report.first_tick = game_->driver.NextTick();
  if (kRuntimeTestHooks) StopPlanAtExitTestTick(report.plan, report.first_tick);
  BeginAudioFrame(report.plan, frame_clock);
  const std::uint64_t ticks_start = SDL_GetTicksNS();
  game_->driver.TickInline(game_->states, report.plan, game_->input,
                           game_->session, game_->recorder, game_->audio);
  const double ticks_sec =
      static_cast<double>(SDL_GetTicksNS() - ticks_start) * kNsToSec;
  // The assets' TickGate may have stopped the frame's ticks early.
  const auto ran = static_cast<int>(game_->driver.FrameResults().size());
  ticks_run_ += static_cast<std::uint64_t>(ran);
  for (int i = 0; i < ran; ++i) {
    costs_.AddTick(ticks_sec / ran);
  }
  report.next_tick = game_->driver.NextTick();
  if (kRuntimeTestHooks) RunExitTests(report.next_tick);
  if (kRuntimeTestHooks && config_.late_frame_ms > 0.0 &&
      !late_frame_injected_ &&
      static_cast<std::int64_t>(iterates_) >= config_.late_frame_at) {
    // The --inject-late-frame hook: this frame finishes late.
    late_frame_injected_ = true;
    SE_LOG_WARN("injecting a late frame: {:.1f} ms at iterate {}",
                config_.late_frame_ms, iterates_);
    SDL_DelayNS(static_cast<std::uint64_t>(config_.late_frame_ms * 1e6));
  }

  if (!report.rendered && lifecycle_.ShouldRender()) {
    if (auto rendered = RenderFrame(report, wait_sec); !rendered) {
      // The game build's policy for a render build fault: the callbacks'
      // fault path (ADR-062).
      SE_LOG_ERROR("frame {}: {}", iterates_, rendered.error().message);
      return OnCallbackFault("render build", rendered.error().message);
    }
    if (report.rendered) {
      pacer_.OnFrameSubmitted(report.presented, report.render.null_acquire);
    }
  } else if (!report.rendered && renderer_ != nullptr && device_ok) {
    // Uploads continue while nothing renders (a minimized window).
    renderer_->PumpUploads();
  }
  if (kRuntimeTestHooks) RunReadbacks(report);
  // Finished voices and the end ring, every iterate (Suspended's 10 Hz
  // included).
  game_->audio.Sweep();
  if (report.presented) {
    ++presented_frames_;
    SE_PROFILE_FRAME();
  }

  // The non-tick CPU cost R: the iterate without its ticks, the swapchain
  // wait (ADR-064) and the pacer's N-L fence wait (ADR-067); the render
  // build, recording and submit count.
  const double busy_sec =
      static_cast<double>(SDL_GetTicksNS() - iterate_start) * kNsToSec;
  const double non_tick_sec =
      std::max(0.0, busy_sec - ticks_sec - wait_sec - fence_wait_sec);
  // g: the GPU duration of an earlier frame that arrived this frame
  // (se::present timestamps), else unknown.
  const double gpu_sec =
      report.render.gpu_timed
          ? static_cast<double>(report.render.gpu_ns) * kNsToSec
          : std::numeric_limits<double>::quiet_NaN();
  costs_.AddFrame(frame_clock.real_delta, non_tick_sec, gpu_sec);

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
  // The pacer's wait, the last thing an iterate does: the waitable, then
  // Paced's sleep to the vblank minus B(next), Standard's swapchain wait or
  // VRR's deadline; nothing while Modal.
  pacer_.EndFrame(stepper_.PlanNext(frame_clock, costs_.Current()),
                  costs_.Current(), lifecycle_.Modal());
  report.pacer = pacer_.LastFrame();
  if (kRuntimeTestHooks && !config_.push_keys.empty()) RunPushKeys(report);
  if (kRuntimeTestHooks && config_.durable_save_test.frame > 0) {
    RunDurableSaveTest();
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
  std::string title = std::format("{} | tick {} | {}", setup_.title,
                                  game_->driver.NextTick() - 1,
                                  LifecycleStateName(lifecycle_.State()));
  // Test builds show the pacer's mode, rate, interval, latency and misses.
  if (kRuntimeTestHooks) title += " | " + pacer_.Summary();
  window_->SetTitle(title);
}

void App::LogProgress(std::uint64_t now_ns) {
  if (last_log_ns_ == 0) last_log_ns_ = now_ns;
  if (now_ns - last_log_ns_ < kLogIntervalNs) return;
  last_log_ns_ = now_ns;
  SE_LOG_INFO("tick {} | {} | {} iterates, {} presented, {} NULL acquires | {}",
              game_->driver.NextTick() - 1,
              LifecycleStateName(lifecycle_.State()), iterates_,
              presented_frames_, lifecycle_.NullAcquires(), pacer_.Summary());
  if (renderer_ != nullptr) {
    const Renderer2DStats& last = renderer_->LastFrame();
    SE_LOG_DEBUG(
        "render frame {}: {} quads, {} batches, {} draws, {} upload bytes, "
        "build {:.3f} ms, record {:.3f} ms, swapchain wait {:.3f} ms, GPU "
        "{}",
        last.frame, last.quads, last.batches, last.draws, last.upload_bytes,
        static_cast<double>(last.build_ns) * 1e-6,
        static_cast<double>(last.record_ns) * 1e-6,
        static_cast<double>(last.swapchain_wait_ns) * 1e-6,
        last.gpu_timed
            ? std::format("{:.3f} ms", static_cast<double>(last.gpu_ns) * 1e-6)
            : std::string("unknown"));
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
  const PacerTotals& pacing = pacer_.Totals();
  SE_LOG_INFO(
      "pacing at shutdown: {} ({}); {} mode changes, {} missed presents "
      "({} vblanks) of {} resolved ({} by present statistics, {} by the "
      "latency wait, {} waits unresolved), {} HalfRate entries, {} sleeps, "
      "{} fence waits",
      PacingModeName(pacer_.Mode()), pacer_.ModeReason(), pacing.mode_changes,
      pacing.missed_presents, pacing.missed_vblanks, pacing.resolved_presents,
      pacing.resolved_by_statistics, pacing.resolved_by_wait,
      pacing.unresolved_waits, pacing.half_rate_entries, pacing.sleeps,
      pacing.fence_waits);
  if (pacing.stats_resolved > 0) {
    // The statistics' own check (ADR-072).
    const StatisticsCheck check =
        CheckStatistics(pacing.stats_lateness, pacing.stats_delay);
    const auto& late = pacing.stats_lateness;
    const auto& delay = pacing.stats_delay;
    SE_LOG_INFO(
        "present statistics at shutdown: {} resolved; shown 0/1/2/3/4+ "
        "refreshes after their targets {}/{}/{}/{}/{}, after their takes "
        "{}/{}/{}/{}/{}; composition delay {}, {} later than target + "
        "delay",
        pacing.stats_resolved, late[0], late[1], late[2], late[3], late[4],
        delay[0], delay[1], delay[2], delay[3], delay[4], check.offset,
        check.beyond);
  }
  if (input_sampler_) {
    const InputSamplerStats sampler = input_sampler_->Stats();
    SE_LOG_INFO("input sampler at shutdown: {} polls at {} Hz, parked {} times",
                sampler.polls, sampler.rate_hz, sampler.parks);
  }
  if (device_recoveries_ > 0) {
    SE_LOG_INFO("device losses recovered: {}", device_recoveries_);
  }
  if (game_) {
    const AudioBridgeStats& sound = game_->audio.Stats();
    SE_LOG_INFO(
        "audio at shutdown: {}; {} events, {} played ({} late), {} cosmetic "
        "dropped, {} failed, {} without an engine",
        audio_ ? audio::BackendName(audio_->GetBackend()) : "none",
        sound.events, sound.played, sound.late, sound.cosmetic_dropped,
        sound.failed, sound.no_engine);
  }
  if (kRuntimeTestHooks && !durable_save_test_path_.empty()) {
    // Before shutdown step 8 joins the lane: the quit waited for it.
    const auto info = fs::Stat(durable_save_test_path_);
    SE_LOG_INFO("durable save test: {} bytes in place at shutdown",
                info ? info->size : 0);
  }
  // The --record replay, after the last tick, through the lane (ADR-089).
  if (booted_) WriteReplay();
  // The renderer's totals and leak check, while logging still runs; the
  // teardown below only destroys it.
  if (renderer_) renderer_->Shutdown();
  Teardown();
  if (lane_stats_) AssetRuntime::LogLaneStopped(*lane_stats_);
  if (replay_submitted_) {
    if (lane_durable_failures_ > replay_failures_before_) {
      SE_LOG_WARN("--record {}: the background lane could not write it",
                  config_.record_path);
    } else {
      SE_LOG_INFO("replay of {} ticks written to {} by the background lane",
                  replay_ticks_, config_.record_path);
    }
  }
  {
    std::string order;
    for (std::size_t i = 0; i < teardown_count_; ++i) {
      if (i > 0) order += ", ";
      order += teardown_steps_[i];
    }
    SE_LOG_INFO("shutdown order: {}", order);
  }
  if (kRuntimeTestHooks && !config_.lane_step_trace.empty()) {
    if (auto written = WriteLaneStepTrace(lane_trace_, config_.lane_step_trace);
        !written) {
      SE_LOG_ERROR("--lane-step-trace: {}", written.error().message);
    } else {
      SE_LOG_INFO("lane step trace: {} steps written to {}", lane_trace_.size(),
                  config_.lane_step_trace);
    }
  }
  if (vblank_detached_) {
    SE_LOG_WARN(
        "the vblank sampler was still in WaitForVBlank at shutdown: detached "
        "(it ends when the wait returns)");
  }
  return shutdown_faulted_ ? SDL_APP_FAILURE : result;
}

void App::Teardown() {
  if (torn_down_) return;
  lifecycle_.OnShutdownStarted();
  // Step 3: normally started when the quit drain ended.
  if (booted_ && !quit_.DeadlineStarted() && helpers_) {
    helpers_->StartShutdownDeadline();
  }
  // Before step 4: the input sampler, the other thread that pushes SDL
  // events (ADR-047), joins within one period.
  if (input_sampler_) {
    input_sampler_->Stop();
    input_sampler_.reset();
    NoteTeardown("input-sampler");
  }
  // Step 4: no more wake pushes from other threads (they read the watch
  // list the filter removal changes), no more diverted events; the GPU
  // finishes its work.
  if (wake_) wake_->Close();
  if (filter_) filter_->Remove();
  if (gpu_) gpu_->WaitForIdle();
  NoteTeardown("event-filter (4)");
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
    NoteTeardown("worlds (6)");
  }
  // Step 7: the audio engine: every voice stopped and uninitialised, then
  // ma_engine_uninit (joins miniaudio's device thread), then the PCM.
  if (audio_) {
    audio_.reset();
    NoteTeardown("audio (7)");
  }
  // Step 8: the background lane (queued jobs that are not Durable are
  // cancelled, a running step finishes, Durable jobs run to the end).
  if (assets_) {
    assets_->StopLane();
    lane_stats_ = assets_->Lane().Stats();
    lane_trace_ = assets_->Lane().TakeStepTrace();
    lane_durable_failures_ = assets_->Lane().DurableFailures();
    NoteTeardown("bg-lane (8)");
  }
  // Step 9: the vblank sampler (the pacer lets go of it first).
  pacer_.Detach();
  if (helpers_) {
    if (helpers_->StopSamplers() == VblankStop::kDetached) {
      vblank_detached_ = true;
    }
    NoteTeardown("vblank-sampler (9)");
  }
  // Step 10: every AssetScope (the GPU slots go through the retire queue
  // while the renderer still exists).
  if (assets_) {
    quit_.RemoveProvider(&assets_->QuitWaiter());
    // Releasing allocates (the retire queue, the lane's watch list): a
    // throw is contained like game code's (ADR-044).
    auto release = [this] {
      AssertMainThread();
      assets_->ReleaseScopes();
    };
    if (!RunGameHook("AssetRegistry::ReleaseAll", release)) {
      shutdown_faulted_ = true;
    }
    assets_.reset();
    NoteTeardown("asset-scopes (10)");
  }
  // The lane's sound decoder, after every job that held it.
  sound_decoder_.reset();
  // Step 12: the present port's fences, the renderer's objects (the GPU
  // is idle), then release the window from the device and destroy the
  // device.
  present_.reset();
  renderer_.reset();
  gpu_.reset();
  // Step 14: remove the subclass, destroy the window.
  if (window_) window_->SetListener(nullptr);
  window_.reset();
  wake_.reset();
  filter_.reset();
  SDL_ResetHint(SDL_HINT_MAIN_CALLBACK_RATE);
  // The open gamepads, before their subsystem goes.
  gamepads_.CloseAll();
  // Step 15: SDL_QuitSubSystem(GAMEPAD | VIDEO).
  sdl_.reset();
  // Step 16: SDL's log output to stderr, then drain and stop the log
  // worker (spdlog::shutdown); SE_LOG_* is synchronous from here on.
  if (logging_) {
    logging_->Stop();
    NoteTeardown("log-worker (16)");
  }
  // Step 17: the watchdog, the last engine thread.
  if (helpers_) {
    helpers_->Stop();
    NoteTeardown("watchdog (17)");
  }
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
