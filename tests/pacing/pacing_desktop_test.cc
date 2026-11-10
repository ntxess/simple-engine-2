// Copyright 2026 The simple-engine-2 Authors.
//
// Desktop tests of the vblank sampler and the pacer (label desktop;
// Windows): real windows on this machine's desktop, opened without taking
// focus, moved only through SDL on the test's own window; no SendInput, no
// SetCursorPos. Timing-sensitive tests report the foreign input
// (GetLastInputInfo) and CPU load they saw and fail only when there was
// none. Tests that need the SDL presentation patch (M6a) skip with the
// reason in an unpatched build, and the Paced tests skip when the display
// is asleep (no vblank sampler, so no Paced; ADR-056).
//
//   sampler_measures_window_monitor  the sampler's period matches the
//                      window's monitor within 0.5%
//   two_monitors       the sampler follows the window to each display
//   display_rates      SDL's refresh against the OS's mode, the sampler and
//                      the VSYNC present rate (ADR-055)
//   unpatched_standard rampage runs Standard without the patch, with the
//                      sampler thread running
//   vrr_deadline       --pacing vrr: frame starts within 0.5 ms p99 of the
//                      120 Hz deadline over 10 s
//   paced_misses       Paced misses under 1% of vblanks over 20 s, on D3D12
//                      and on Vulkan; prints where the misses came from
//                      (present statistics, or the frame-latency wait on
//                      the vblank grid in a composed window: ADR-066)
//   late_frame_drains  an injected late frame drains within 2 frames

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game_rampage/module.h"
#include "platform/support/test_app.h"
#include "se_core/error.h"
#include "se_core/thread_registry.h"
#include "se_core/thread_role.h"
#include "se_platform/display_output.h"
#include "se_platform/fs.h"
#include "se_platform/sdl_context.h"
#include "se_platform/vblank_sampler.h"
#include "se_platform/window.h"
#include "se_render/frame_recorder.h"
#include "se_render/gpu_device.h"
#include "se_render/renderer_2d.h"
#include "se_runtime/app.h"
#include "se_runtime/frame_pacer.h"
#include "se_runtime/pacer_policy.h"
#include "support/test_roles.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace se {
namespace {

#ifndef SE_TEST_SANITIZED
#define SE_TEST_SANITIZED 0
#endif
// A sanitizer build (msvc-asan): frames take several times longer.
[[maybe_unused]] constexpr bool kSanitized = SE_TEST_SANITIZED != 0;

#if defined(_WIN32)

using testing::RunTestApp;
using testing::TestGameSetup;

DWORD LastInputTick() {
  LASTINPUTINFO info{};
  info.cbSize = sizeof(info);
  GetLastInputInfo(&info);
  return info.dwTime;
}

std::uint64_t Ticks(const FILETIME& time) {
  return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) |
         time.dwLowDateTime;
}

// What a person or other processes did while a test ran: foreign input
// (the test generates none), the CPU load, and the cursor and foreground
// window, which must be as they were when nobody touched the machine.
class Desktop {
 public:
  Desktop() : input_tick_(LastInputTick()), foreground_(GetForegroundWindow()) {
    GetCursorPos(&cursor_);
    GetSystemTimes(&idle_, &kernel_, &user_);
  }

  [[nodiscard]] bool ForeignInput() const {
    return LastInputTick() != input_tick_;
  }
  // The share of all CPUs busy since construction.
  [[nodiscard]] double CpuLoad() const {
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    GetSystemTimes(&idle, &kernel, &user);
    const std::uint64_t total =
        (Ticks(kernel) - Ticks(kernel_)) + (Ticks(user) - Ticks(user_));
    const std::uint64_t idle_ticks = Ticks(idle) - Ticks(idle_);
    return total > 0 ? 1.0 - static_cast<double>(idle_ticks) /
                                 static_cast<double>(total)
                     : 0.0;
  }
  [[nodiscard]] std::string Report() const {
    return std::format("foreign input during the test: {}; CPU load {:.0f}%",
                       ForeignInput() ? "yes" : "none", 100.0 * CpuLoad());
  }
  // Disturbed enough to excuse a timing miss.
  [[nodiscard]] bool Busy() const { return ForeignInput() || CpuLoad() > 0.5; }
  void CheckUntouched(HWND ours) const {
    if (ForeignInput()) {
      WARN("user input during the test: cursor and focus checks skipped");
      return;
    }
    POINT now{};
    GetCursorPos(&now);
    CHECK(now.x == cursor_.x);
    CHECK(now.y == cursor_.y);
    if (ours != nullptr) CHECK(GetForegroundWindow() != ours);
    CHECK(GetForegroundWindow() == foreground_);
  }

 private:
  DWORD input_tick_;
  HWND foreground_;
  POINT cursor_{};
  FILETIME idle_{};
  FILETIME kernel_{};
  FILETIME user_{};
};

void Print(const std::string& line) {
  std::printf("%s\n", line.c_str());
  std::fflush(stdout);
}

// Pumps the window's events for |ms| (main thread).
void Pump(std::uint32_t ms) {
  const std::uint64_t end = SDL_GetTicksNS() + std::uint64_t{ms} * 1'000'000;
  while (SDL_GetTicksNS() < end) {
    SDL_PumpEvents();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
    }
    SDL_DelayNS(2'000'000);
  }
}

std::string DescribeReading(const VblankReading& reading) {
  return std::format(
      "valid {}, resolved {}, fresh {}, output off {}, period {:.4f} ms, "
      "spread {:.4f} ms, {} vblanks, {} early returns, {} failures, monitor "
      "{}",
      reading.valid, reading.resolved, reading.fresh, reading.output_off,
      static_cast<double>(reading.period_ns) * 1e-6,
      static_cast<double>(reading.spread_ns) * 1e-6, reading.count,
      reading.early_returns, reading.failures, reading.monitor);
}

// Waits up to |timeout_ms| for a valid reading of |monitor|, pumping events.
VblankReading WaitForMonitor(const VblankSampler& sampler,
                             MonitorHandle monitor, std::uint32_t timeout_ms) {
  const std::uint64_t end =
      SDL_GetTicksNS() + std::uint64_t{timeout_ms} * 1'000'000;
  VblankReading reading;
  do {
    Pump(10);
    reading = sampler.Read(SDL_GetTicksNS());
    // Enough intervals for a settled median.
    if (reading.valid && reading.monitor == monitor && reading.count >= 64) {
      break;
    }
  } while (SDL_GetTicksNS() < end);
  return reading;
}

// Skips the test when the output's waits return at once (the display is
// off or asleep; measured on this machine, ADR-056).
void SkipIfDisplayOff(const VblankReading& reading) {
  if (!reading.valid && (reading.output_off || reading.early_returns > 0)) {
    SKIP("the display is off or asleep: WaitForVBlank returns at once ("
         << DescribeReading(reading) << ")");
  }
}

struct SamplerRig {
  std::optional<SdlContext> sdl;
  std::unique_ptr<Window> window;
  std::unique_ptr<VblankSampler> sampler;

  [[nodiscard]] HWND Hwnd() const {
    return static_cast<HWND>(window->NativeHandle());
  }
};

void OpenRig(SamplerRig& rig) SE_REQUIRES(g_main) {
  SdlInitConfig sdl_config;
  sdl_config.subsystems = SDL_INIT_VIDEO;
  auto sdl = SdlContext::Init(sdl_config);
  REQUIRE(sdl);
  rig.sdl.emplace(std::move(*sdl));
  WindowConfig config;
  config.title = "se_pacing_tests vblank sampler";
  config.width = 320;
  config.height = 200;
  config.activate_when_shown = false;
  auto window = Window::Create(config);
  REQUIRE(window);
  rig.window = std::move(*window);
  Pump(50);
  VblankSamplerConfig sampler_config;
  sampler_config.monitor = MonitorOfWindow(rig.window->NativeHandle());
  auto sampler = VblankSampler::Start(sampler_config);
  REQUIRE(sampler);
  rig.sampler = std::move(*sampler);
}

void CloseRig(SamplerRig& rig) SE_REQUIRES(g_main) {
  if (rig.sampler) CHECK(rig.sampler->Stop() == VblankStop::kJoined);
  rig.sampler.reset();
  rig.window.reset();
  rig.sdl.reset();
  SDL_Quit();
}

std::uint32_t LiveSamplerThreads() {
  const ThreadRegistry& registry = ThreadRegistry::Instance();
  std::uint32_t live = 0;
  for (std::uint32_t slot = 0; slot < kThreadSlotCount; ++slot) {
    const ThreadSlotInfo info = registry.Read(slot);
    if (info.state == ThreadSlotState::kLive &&
        info.role == ThreadRole::kVblankSampler) {
      ++live;
    }
  }
  return live;
}

double AbsPercentile(std::vector<double> values, double p) {
  if (values.empty()) return 0.0;
  for (double& value : values) value = std::abs(value);
  std::sort(values.begin(), values.end());
  const auto rank = static_cast<std::size_t>(
      std::ceil(p * static_cast<double>(values.size())));
  return values[std::clamp<std::size_t>(rank, 1, values.size()) - 1];
}

TEST_CASE("sampler_measures_window_monitor", "[desktop][vblank]") {
  const testing::MainThreadScope main;
  const Desktop desktop;
  SamplerRig rig;
  OpenRig(rig);
  const MonitorHandle monitor = MonitorOfWindow(rig.window->NativeHandle());
  const MonitorMode mode = QueryMonitorMode(monitor);
  Print("window's monitor: " + DescribeMonitorMode(mode));
  REQUIRE(mode.known);
  const VblankReading reading = WaitForMonitor(*rig.sampler, monitor, 3000);
  Print("sampler: " + DescribeReading(reading));
  if (!reading.valid) {
    CloseRig(rig);
    SkipIfDisplayOff(reading);
  }
  REQUIRE(reading.valid);
  CHECK(reading.monitor == monitor);
  const double period_sec = static_cast<double>(reading.period_ns) * 1e-9;
  Print(std::format("sampler {:.4f} Hz, the mode {:.4f} Hz", 1.0 / period_sec,
                    mode.hz));
  CHECK(PeriodsAgree(period_sec, 1.0 / mode.hz));
  REQUIRE(LiveSamplerThreads() == 1);
  desktop.CheckUntouched(rig.Hwnd());
  CloseRig(rig);
  REQUIRE(LiveSamplerThreads() == 0);
}

TEST_CASE("two_monitors", "[desktop][vblank]") {
  const testing::MainThreadScope main;
  const Desktop desktop;
  SamplerRig rig;
  OpenRig(rig);
  int count = 0;
  SDL_DisplayID* displays = SDL_GetDisplays(&count);
  REQUIRE(displays != nullptr);
  std::vector<SDL_DisplayID> ids(displays, displays + count);
  SDL_free(displays);
  Print(std::format("{} displays", ids.size()));
  if (ids.size() < 2) {
    for (const MonitorInfo& monitor : ListMonitors()) {
      Print("monitor: " + DescribeMonitorMode(monitor.mode));
    }
    CloseRig(rig);
    SKIP(
        "only one display is active; the second monitor is off or "
        "disconnected (docs/gates.md, Two monitors)");
  }
  for (const SDL_DisplayID id : ids) {
    SDL_Rect bounds{};
    REQUIRE(SDL_GetDisplayBounds(id, &bounds));
    // The test's own window, never the cursor.
    REQUIRE(SDL_SetWindowPosition(rig.window->Native(),
                                  bounds.x + bounds.w / 2 - 160,
                                  bounds.y + bounds.h / 2 - 100));
    Pump(200);
    const MonitorHandle monitor = MonitorOfWindow(rig.window->NativeHandle());
    const MonitorMode mode = QueryMonitorMode(monitor);
    Print(std::format("display {} ({}): {}", id, SDL_GetDisplayName(id),
                      DescribeMonitorMode(mode)));
    REQUIRE(SDL_GetDisplayForWindow(rig.window->Native()) == id);
    rig.sampler->SetMonitor(monitor);
    const VblankReading reading = WaitForMonitor(*rig.sampler, monitor, 3000);
    Print("sampler: " + DescribeReading(reading));
    if (!reading.valid) {
      CloseRig(rig);
      SkipIfDisplayOff(reading);
    }
    REQUIRE(reading.valid);
    CHECK(reading.monitor == monitor);
    REQUIRE(mode.known);
    CHECK(PeriodsAgree(static_cast<double>(reading.period_ns) * 1e-9,
                       1.0 / mode.hz));
  }
  desktop.CheckUntouched(rig.Hwnd());
  CloseRig(rig);
}

TEST_CASE("display_rates", "[desktop][display]") {
  const testing::MainThreadScope main;
  const Desktop desktop;
  SamplerRig rig;
  OpenRig(rig);
  const MonitorHandle monitor = MonitorOfWindow(rig.window->NativeHandle());
  const MonitorMode mode = QueryMonitorMode(monitor);
  const double sdl_hz = rig.window->Refresh().hz;
  Print(std::format("SDL reports {:.4f} Hz for the window's display", sdl_hz));
  Print("the OS reports " + DescribeMonitorMode(mode));
  REQUIRE(mode.known);
  // SDL's number is DXGI's closest listed mode, as display_output.cc
  // reproduces it (SDL_windowsmodes.c, WIN_GetRefreshRate).
  CHECK(std::abs(sdl_hz - mode.sdl_style_hz) < 0.01);

  // The VSYNC present rate, measured by presenting clear frames.
  GpuDeviceConfig device_config;
  auto created = GpuDevice::Create(device_config);
  REQUIRE(created);
  auto device = std::make_unique<GpuDevice>(std::move(*created));
  REQUIRE(device->ClaimWindow(rig.window->Native()));
  auto renderer = Renderer2D::Create(*device);
  REQUIRE(renderer);
  constexpr int kWarmup = 30;
  constexpr int kFrames = 240;
  std::uint64_t start = 0;
  FrameRequest request;
  request.clear = ClearColor{0.2F, 0.2F, 0.3F, 1.0F};
  for (int i = 0; i < kWarmup + kFrames; ++i) {
    if (i == kWarmup) start = SDL_GetTicksNS();
    SDL_PumpEvents();
    REQUIRE((*renderer)->Frame(request).submitted);
  }
  const double present_hz =
      kFrames / (static_cast<double>(SDL_GetTicksNS() - start) * 1e-9);
  (*renderer)->Shutdown();
  renderer->reset();
  device->WaitForIdle();
  device.reset();
  const VblankReading reading = WaitForMonitor(*rig.sampler, monitor, 2000);
  const double sampler_hz =
      reading.valid ? 1e9 / static_cast<double>(reading.period_ns) : 0.0;
  Print(std::format("VSYNC presents at {:.3f} Hz; the sampler {} ({}); {}",
                    present_hz,
                    reading.valid ? std::format("{:.4f} Hz", sampler_hz)
                                  : std::string("has no valid reading"),
                    DescribeReading(reading), desktop.Report()));
  const bool sdl_agrees = PeriodsAgree(1.0 / sdl_hz, 1.0 / mode.hz);
  Print(sdl_agrees ? "SDL's refresh agrees with the OS's current mode"
                   : std::format("SDL misreports the refresh: {:.4f} Hz "
                                 "against the current mode's {:.4f} Hz",
                                 sdl_hz, mode.hz));
  // The present rate follows the current mode (within 3%: a frame or two
  // of the measurement may be lost to the scheduler).
  if (std::abs(present_hz - mode.hz) > 0.03 * mode.hz && desktop.Busy()) {
    WARN("present rate off with a busy desktop: " << desktop.Report());
  } else {
    CHECK(std::abs(present_hz - mode.hz) <= 0.03 * mode.hz);
  }
  if (reading.valid) CHECK(PeriodsAgree(1.0 / sampler_hz, 1.0 / mode.hz));
  CloseRig(rig);
}

// The rampage game module on the repository's sandbox scene.
GameSetup RampageSetup() {
  GameSetup setup;
  setup.module = &rampage::RampageModule();
  setup.title = "se_pacing_tests rampage";
  setup.org = "simple-engine-tests";
  setup.app = "pacing";
  setup.seed = 1;
  setup.tick_rate = 120;
  setup.configure =
      [](const ContentPaths& paths) -> std::expected<void, Error> {
    rampage::SceneRequest request;
    request.format = rampage::SceneRequest::Format::kScene;
    request.path =
        fs::Join(paths.content_root, "assets/scenes/sandbox.scene.json");
    request.asset_root = fs::Join(paths.content_root, "assets");
    request.legacy_root = paths.content_root;
    rampage::SetSandboxScene(std::move(request));
    return {};
  };
  return setup;
}

TEST_CASE("unpatched_standard", "[desktop][pacing]") {
  PacingMode mode = PacingMode::kVrr;
  std::string reason;
  PresentCaps caps;
  std::uint32_t sampler_threads = 0;
  std::uint64_t presented = 0;
  // rampage on the sandbox scene (the M6 exit: rampage runs in Standard).
  GameSetup setup = RampageSetup();
  setup.on_frame = [&](App& app, const FrameReport& /*report*/) {
    AssertMainThread();
    mode = app.Pacer().Mode();
    reason = std::string(app.Pacer().ModeReason());
    caps = app.Pacer().Caps();
    presented = app.PresentedFrames();
    sampler_threads = std::max(sampler_threads, LiveSamplerThreads());
  };
  REQUIRE(RunTestApp(std::move(setup), {"--frames", "120", "--content-root",
                                        SE_SOURCE_DIR}) == 0);
  if (caps.patched) {
    SKIP(
        "this build has the SDL presentation patch; msvc-unpatched-dev "
        "runs this test");
  }
  Print("pacing: " + std::string(PacingModeName(mode)) + " (" + reason + ")");
  CHECK(mode == PacingMode::kStandard);
  CHECK(reason.find("waitable") != std::string::npos);
  CHECK_FALSE(caps.frame_latency_waitable);
  CHECK_FALSE(caps.present_stats);
  CHECK_FALSE(caps.gpu_timestamps);
  CHECK_FALSE(caps.native_handles);
  CHECK_FALSE(caps.device_status);
  REQUIRE(presented == 120);
  // The sampler ran beside the App, and stopped with it.
  REQUIRE(sampler_threads == 1);
  REQUIRE(LiveSamplerThreads() == 0);
}

TEST_CASE("vrr_deadline", "[desktop][pacing]") {
  const Desktop desktop;
  constexpr double kRunSec = 10.0;
  constexpr std::uint64_t kWarmupFrames = 30;
  std::vector<double> errors_ms;
  PacingMode mode = PacingMode::kStandard;
  PresentCaps caps;
  std::uint64_t first_ns = 0;
  std::uint64_t vrr_frames = 0;
  GameSetup setup = TestGameSetup();
  setup.on_frame = [&](App& app, const FrameReport& report) {
    AssertMainThread();
    mode = app.Pacer().Mode();
    caps = app.Pacer().Caps();
    if (mode != PacingMode::kVrr) {
      app.RequestQuit();
      return;
    }
    ++vrr_frames;
    if (vrr_frames <= kWarmupFrames || !report.pacer.presented) return;
    if (first_ns == 0) first_ns = report.now_ns;
    errors_ms.push_back(static_cast<double>(report.pacer.deadline_error_ns) *
                        1e-6);
    if (static_cast<double>(report.now_ns - first_ns) * 1e-9 >= kRunSec &&
        !app.Quit().Pending()) {
      app.RequestQuit();
    }
  };
  REQUIRE(RunTestApp(std::move(setup), {"--pacing", "vrr"}) == 0);
  if (!caps.tearing) SKIP("IMMEDIATE (tearing) is not supported here");
  REQUIRE(mode == PacingMode::kVrr);
  const double p50 = AbsPercentile(errors_ms, 0.50);
  const double p99 = AbsPercentile(errors_ms, 0.99);
  const double worst = AbsPercentile(errors_ms, 1.0);
  Print(std::format(
      "VRR: {} frames measured over {:.0f} s at 120 Hz; |start - deadline| "
      "p50 {:.3f} ms, p99 {:.3f} ms, max {:.3f} ms; {}",
      errors_ms.size(), kRunSec, p50, p99, worst, desktop.Report()));
  // 120 frames a second, a few lost to stalls at most, and starts within
  // 0.5 ms p99 of their deadlines; a desktop busy with other work (input,
  // or more than half the CPU) excuses a miss, which is reported.
  const bool rate_ok =
      static_cast<double>(errors_ms.size()) >= 0.95 * 120.0 * kRunSec;
  if ((!rate_ok || p99 > 0.5) && desktop.Busy()) {
    WARN("VRR timing missed with a busy desktop: " << desktop.Report());
  } else {
    CHECK(rate_ok);
    CHECK(p99 <= 0.5);
  }
}

struct PacedRun {
  PacingMode mode = PacingMode::kStandard;
  std::string reason;
  PresentCaps caps;
  std::string driver;
  // Every frame after the first second, and the iterate of each.
  std::vector<PacerFrame> frames;
  std::vector<std::uint64_t> iterates;
  double vblank_period_sec = 0.0;
  double display_hz = 0.0;
};

// Runs rampage with --pacing paced on |driver| for |seconds| after a 1 s
// warm-up (the sampler measures and the pacer promotes to Paced in it).
PacedRun RunPaced(std::string_view driver, double seconds,
                  std::vector<std::string> args) {
  PacedRun run;
  run.driver = std::string(driver);
  std::uint64_t start_ns = 0;
  bool paced_seen = false;
  GameSetup setup = RampageSetup();
  setup.on_frame = [&](App& app, const FrameReport& report) {
    AssertMainThread();
    run.mode = app.Pacer().Mode();
    run.reason = std::string(app.Pacer().ModeReason());
    run.caps = app.Pacer().Caps();
    run.display_hz = app.Pacer().DisplayHz();
    if (!run.caps.frame_latency_waitable) {
      app.RequestQuit();
      return;
    }
    if (start_ns == 0) start_ns = report.now_ns;
    const double elapsed = static_cast<double>(report.now_ns - start_ns) * 1e-9;
    if (run.mode == PacingMode::kPaced) paced_seen = true;
    // No Paced within 3 s: the sampler has no valid reading (the test
    // skips with the pacer's reason).
    if (elapsed >= 3.0 && !paced_seen) {
      if (!app.Quit().Pending()) app.RequestQuit();
      return;
    }
    if (elapsed < 1.0) return;
    run.frames.push_back(report.pacer);
    run.iterates.push_back(report.iterate);
    run.vblank_period_sec = report.pacer.vblank_period_sec;
    if (elapsed >= 1.0 + seconds && !app.Quit().Pending()) app.RequestQuit();
  };
  args.insert(args.end(), {"--content-root", SE_SOURCE_DIR, "--pacing", "paced",
                           "--gpu-driver", run.driver});
  REQUIRE(RunTestApp(std::move(setup), std::move(args)) == 0);
  return run;
}

// Paced needs the patch's waitable, and a vblank sampler measuring the
// window's output (none while the display sleeps: WaitForVBlank returns at
// once, ADR-056).
void SkipWithoutPaced(const PacedRun& run) {
  if (!run.caps.frame_latency_waitable) {
    SKIP(
        "needs the SDL presentation patch (M6a): no frame-latency waitable "
        "on "
        << run.driver << " in this build");
  }
  if (run.mode != PacingMode::kPaced) {
    SKIP("Paced did not run on " << run.driver << ": " << run.reason
                                 << " (the display may be asleep)");
  }
}

// The nearest-rank percentile p of |values| (0 when empty).
double Percentile(std::vector<double> values, double p) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const auto rank = static_cast<std::size_t>(
      std::ceil(p * static_cast<double>(values.size())));
  return values[std::clamp<std::size_t>(rank, 1, values.size()) - 1];
}

// Where the Paced frames' results came from (ADR-066), the latency wait's
// return after the vblank that took each present, the present statistics
// counted beside it, and each present's lead on its target vblank (target
// minus submit) for hits and misses.
std::string DescribeSources(const PacedRun& run) {
  std::uint64_t by_statistics = 0;
  std::uint64_t by_wait = 0;
  std::uint64_t unresolved = 0;
  std::uint64_t stats_resolved = 0;
  std::uint64_t stats_missed = 0;
  std::vector<double> phases_ms;
  std::vector<double> hit_leads_ms;
  std::vector<double> miss_leads_ms;
  for (const PacerFrame& frame : run.frames) {
    if (frame.mode != PacingMode::kPaced) continue;
    if (frame.miss_source == MissSource::kStatistics) {
      by_statistics += frame.resolved;
    } else if (frame.miss_source == MissSource::kLatencyWait) {
      by_wait += frame.resolved;
    }
    unresolved += frame.unresolved;
    stats_resolved += frame.stats_resolved;
    stats_missed += frame.stats_missed;
    if (frame.wait_blocked) {
      phases_ms.push_back(static_cast<double>(frame.wait_phase_ns) * 1e-6);
    }
    // The latency wait resolves the frame's own present (frame latency 1).
    if (frame.miss_source == MissSource::kLatencyWait && frame.resolved > 0 &&
        frame.target_ns != 0 && frame.submit_ns != 0) {
      const double lead_ms = (static_cast<double>(frame.target_ns) -
                              static_cast<double>(frame.submit_ns)) *
                             1e-6;
      (frame.missed > 0 ? miss_leads_ms : hit_leads_ms).push_back(lead_ms);
    }
  }
  std::string source = "both";
  if (by_wait == 0) source = "present statistics";
  if (by_statistics == 0) source = "the frame-latency wait on the vblank grid";
  return std::format(
      "misses from {}: {} presents by the frame-latency wait, {} by present "
      "statistics, {} waits unresolved; the wait returned after the vblank "
      "that took its present: min {:.3f}, p50 {:.3f}, p99 {:.3f}, max {:.3f} "
      "ms; present statistics beside it: {} resolved, {} shown after their "
      "target refresh; submit lead on the target: hits min {:.3f}, p50 "
      "{:.3f} ms ({}), misses min {:.3f}, p50 {:.3f}, max {:.3f} ms ({})",
      source, by_wait, by_statistics, unresolved, Percentile(phases_ms, 0.0),
      Percentile(phases_ms, 0.5), Percentile(phases_ms, 0.99),
      Percentile(phases_ms, 1.0), stats_resolved, stats_missed,
      Percentile(hit_leads_ms, 0.0), Percentile(hit_leads_ms, 0.5),
      hit_leads_ms.size(), Percentile(miss_leads_ms, 0.0),
      Percentile(miss_leads_ms, 0.5), Percentile(miss_leads_ms, 1.0),
      miss_leads_ms.size());
}

// |gate|: the M6 exit (under 1% of the presents late) applies; otherwise
// the numbers are reported only.
void PacedMisses(std::string_view driver, bool gate) {
  const Desktop desktop;
  const PacedRun run = RunPaced(driver, 20.0, {});
  SkipWithoutPaced(run);
  Print("pacing on " + run.driver + ": " +
        std::string(PacingModeName(run.mode)) + " (" + run.reason + ")");
  std::uint64_t paced_frames = 0;
  std::uint64_t resolved = 0;
  std::uint64_t missed = 0;
  std::uint64_t late_vblanks = 0;
  std::array<std::uint64_t, PresentTracker::kLatenessBins> lateness{};
  std::array<std::uint64_t, PresentTracker::kLatenessBins> delay{};
  for (const PacerFrame& frame : run.frames) {
    if (frame.mode != PacingMode::kPaced) continue;
    ++paced_frames;
    resolved += frame.resolved;
    missed += frame.missed;
    late_vblanks += frame.late_vblanks;
    for (std::size_t bin = 0; bin < lateness.size(); ++bin) {
      lateness[bin] += frame.stats_lateness[bin];
      delay[bin] += frame.stats_delay[bin];
    }
  }
  // The M6 exit counts misses "by present statistics": the statistics'
  // own check beside the pacer's source, after the composition delay
  // measured from the latency wait's takes (ADR-072).
  const StatisticsCheck stats = CheckStatistics(lateness, delay);
  Print(std::format(
      "present statistics on {}: {} presents with a known refresh, shown "
      "0/1/2/3/4+ refreshes after their targets {}/{}/{}/{}/{}; {} with a "
      "known take, shown after it {}/{}/{}/{}/{}: composition delay {} "
      "refresh(es); {} ({:.2f}%) shown later than target + delay",
      run.driver, stats.counted, lateness[0], lateness[1], lateness[2],
      lateness[3], lateness[4], stats.delays, delay[0], delay[1], delay[2],
      delay[3], delay[4], stats.offset, stats.beyond,
      stats.counted > 0 ? 100.0 * static_cast<double>(stats.beyond) /
                              static_cast<double>(stats.counted)
                        : 0.0));
  Print(std::format(
      "Paced at {:.3f} Hz (display mode {:.3f} Hz) on {}: {} frames counted "
      "({} Paced), {} presents resolved, {} missed ({:.2f}%, {} vblanks "
      "late); {}; {}",
      1.0 / run.vblank_period_sec, run.display_hz, run.driver,
      run.frames.size(), paced_frames, resolved, missed,
      resolved > 0
          ? 100.0 * static_cast<double>(missed) / static_cast<double>(resolved)
          : 0.0,
      late_vblanks, DescribeSources(run), desktop.Report()));
  // Paced the whole time after the warm-up, and most presents resolved. A
  // present resolves only when the latency wait blocks for it; under a
  // sanitizer the slower frames often find the waitable already signalled
  // (56%, 16% and 72% resolved in three msvc-asan runs right after the
  // commit gate's builds, the 16% while DWM stalled; 88-99% when run
  // alone), so there the share is reported and the misses are checked over
  // the presents that did resolve (at least 10%).
  CHECK(paced_frames * 10 >= run.frames.size() * 9);
  if (kSanitized && resolved * 10 < paced_frames * 8) {
    WARN("only " << resolved << " of " << paced_frames
                 << " presents resolved (sanitizer build)");
    REQUIRE(resolved * 10 >= paced_frames);
  } else {
    REQUIRE(resolved * 10 >= paced_frames * 8);
  }
  const bool under = missed * 100 < resolved;
  if (!under && desktop.Busy()) {
    WARN("misses above 1% with a busy desktop: " << desktop.Report());
  } else if (!under && !gate) {
    WARN("misses above 1% on " << run.driver
                               << " (reported; docs/gates.md, Paced on "
                                  "Vulkan in a window)");
  } else {
    CHECK(under);
  }
  // Where the path gives statistics for most presents (D3D12 in a window
  // with the display awake), fewer than 1% of them may be shown later than
  // their target plus the composition delay.
  if (stats.counted * 10 < resolved * 8) {
    Print("present statistics: too few to check on " + run.driver);
    return;
  }
  CHECK(stats.delays * 10 >= stats.counted * 8);
  const bool stats_under = stats.beyond * 100 < stats.counted;
  if (!stats_under && desktop.Busy()) {
    WARN("present statistics late with a busy desktop: " << desktop.Report());
  } else if (!stats_under && !gate) {
    WARN("present statistics late on " << run.driver << " (reported)");
  } else {
    CHECK(stats_under);
  }
}

// The M6 exit: rampage's default driver.
TEST_CASE("paced_misses", "[desktop][pacing]") {
  PacedMisses("direct3d12", /*gate=*/true);
}

// Reported: NVIDIA's Vulkan path in a window takes about 3% of the presents
// one vblank late whatever their lead (measured 2026-11-09; gates.md).
TEST_CASE("paced_misses_vulkan", "[desktop][pacing]") {
  PacedMisses("vulkan", /*gate=*/false);
}

TEST_CASE("late_frame_drains", "[desktop][pacing]") {
  const Desktop desktop;
  constexpr std::uint64_t kLateAt = 400;
  const PacedRun run =
      RunPaced("direct3d12", 3.0,
               {"--inject-late-frame", std::format("{}:30", kLateAt)});
  SkipWithoutPaced(run);
  Print(DescribeSources(run));
  // The frames from the late one on: misses in at most 2 consecutive
  // frames, then none.
  int run_length = 0;
  int longest = 0;
  std::size_t first = run.frames.size();
  std::size_t last_miss = 0;
  std::uint64_t misses = 0;
  for (std::size_t i = 0; i < run.frames.size(); ++i) {
    if (run.iterates[i] < kLateAt) continue;
    first = std::min(first, i);
    const bool missed = run.frames[i].missed > 0;
    run_length = missed ? run_length + 1 : 0;
    longest = std::max(longest, run_length);
    if (missed) {
      last_miss = i;
      misses += run.frames[i].missed;
    }
  }
  REQUIRE(first < run.frames.size());
  Print(std::format(
      "late frame at iterate {}: {} misses, longest run {}, "
      "last miss {} frames after it; {}",
      kLateAt, misses, longest, last_miss - first, desktop.Report()));
  if (desktop.ForeignInput() && (longest > 2 || last_miss > first + 3)) {
    WARN("the late frame did not drain with foreign input");
  } else {
    REQUIRE(misses >= 1);
    REQUIRE(longest <= 2);
    CHECK(last_miss <= first + 3);
  }
}

#endif  // _WIN32

}  // namespace
}  // namespace se
