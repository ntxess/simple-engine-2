// Copyright 2026 The simple-engine-2 Authors.
//
// The present API (docs/milestone-specs.md M6a, "Tests"). The GPU driver
// comes from SE_GPU_DRIVER (CMake registers every test once per driver:
// present.d3d12.* and present.vulkan.*); a driver that cannot create a
// device skips the test with the reason. Tests tagged [gpu] create a device
// without a window and run in the commit gate on Windows; [desktop] tests
// open a window that is shown without taking focus and never touch the
// mouse or the keyboard. Both labels are excluded on Linux.
//
// The same tests run in the unpatched build (msvc-unpatched-dev,
// SE_SDL_PATCHED=0), where every capability must be false, every action a
// no-op, and frames must still present.

#include "se_render/present.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_platform/sdl_context.h"
#include "se_platform/window.h"
#include "se_render/gpu_device.h"
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

// The build and SDL agree on whether the patch is in.
#if SE_SDL_PATCHED && !__has_include(<SDL3/SDL_gpu_se.h>)
#error "SE_SDL_PATCHED=1 but SDL has no SDL_gpu_se.h"
#endif
#if !SE_SDL_PATCHED && __has_include(<SDL3/SDL_gpu_se.h>)
#error "SE_SDL_PATCHED=0 but SDL has SDL_gpu_se.h: the patch leaked in"
#endif

namespace se {
namespace {

constexpr std::uint64_t kNsPerMs = 1'000'000;

// The driver this run tests: SE_GPU_DRIVER, else the platform default.
std::string TestDriver() {
  const char* environment = SDL_getenv("SE_GPU_DRIVER");
  return ResolveGpuDriver("", environment != nullptr ? environment : "");
}

bool IsD3d12(const GpuDevice& device) {
  return device.Driver() == "direct3d12";
}

// SDL, an optional window and a GPU device for TestDriver(). Members are
// destroyed in reverse order: the device (which releases the window), the
// window, then SDL.
struct Fixture {
  std::optional<SdlContext> sdl;
  std::unique_ptr<Window> window;
  std::unique_ptr<GpuDevice> device;

  [[nodiscard]] GpuDevice& Device() const { return *device; }
  void Close() SE_REQUIRES(g_main) {
    if (device) device->WaitForIdle();
    device.reset();
    window.reset();
  }
};

// Creates the device; skips the test when the driver cannot create one.
void CreateDevice(Fixture& fixture) SE_REQUIRES(g_main) {
  GpuDeviceConfig config;
  config.driver = TestDriver();
  auto device = GpuDevice::Create(config);
  if (!device) {
    SKIP("GPU driver '" << config.driver
                        << "' is not available: " << device.error().message);
  }
  fixture.device = std::make_unique<GpuDevice>(std::move(*device));
}

void InitSdl(Fixture& fixture) SE_REQUIRES(g_main) {
  SdlInitConfig sdl_config;
  sdl_config.subsystems = SDL_INIT_VIDEO;
  auto sdl = SdlContext::Init(sdl_config);
  REQUIRE(sdl);
  fixture.sdl.emplace(std::move(*sdl));
}

// A device without a window.
void OpenDevice(Fixture& fixture) SE_REQUIRES(g_main) {
  InitSdl(fixture);
  CreateDevice(fixture);
}

enum class WindowMode : std::uint8_t {
  // 320x240, shown without taking focus: every desktop test.
  kUnfocused,
  // Borderless fullscreen on its display and activated, so that DXGI can
  // present through independent flip: only the manual [.fullscreen] test,
  // which the person running it starts (docs/gates.md).
  kFullscreen,
};

// A device that has claimed a window.
void OpenWindow(Fixture& fixture, WindowMode mode = WindowMode::kUnfocused)
    SE_REQUIRES(g_main) {
  InitSdl(fixture);
  WindowConfig config;
  config.title = "se_present_tests";
  config.width = 320;
  config.height = 240;
  config.activate_when_shown = mode == WindowMode::kFullscreen;
  auto window = Window::Create(config);
  REQUIRE(window);
  fixture.window = std::move(*window);
  if (mode == WindowMode::kFullscreen) {
    REQUIRE(SDL_SetWindowFullscreen(fixture.window->Native(), true));
    REQUIRE(SDL_SyncWindow(fixture.window->Native()));
  }
  CreateDevice(fixture);
  REQUIRE(fixture.device->ClaimWindow(fixture.window->Native()));
}

// One frame on the claimed window with this test's own command buffer: the
// swapchain wait and acquire, a clear, the submit. True when it presented.
bool PresentFrame(GpuDevice& device, float shade) SE_REQUIRES(g_main) {
  SDL_PumpEvents();
  SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device.Native());
  REQUIRE(commands != nullptr);
  SDL_Window* window = device.ClaimedWindow();
  SDL_WaitForGPUSwapchain(device.Native(), window);
  SDL_GPUTexture* swapchain = nullptr;
  REQUIRE(SDL_AcquireGPUSwapchainTexture(commands, window, &swapchain, nullptr,
                                         nullptr));
  if (swapchain != nullptr) {
    SDL_GPUColorTargetInfo target{};
    target.texture = swapchain;
    target.clear_color = SDL_FColor{shade, 0.25F, 1.0F - shade, 1.0F};
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass =
        SDL_BeginGPURenderPass(commands, &target, 1, nullptr);
    REQUIRE(pass != nullptr);
    SDL_EndGPURenderPass(pass);
  }
  REQUIRE(SDL_SubmitGPUCommandBuffer(commands));
  return swapchain != nullptr;
}

std::uint64_t Median(std::vector<std::uint64_t> values) {
  if (values.empty()) return 0;
  std::ranges::sort(values);
  return values[values.size() / 2];
}

// What else happened on the machine while a timing-sensitive test ran:
// input from the person using it (GetLastInputInfo) and the CPU load of
// other processes (GetSystemTimes). Tests report it, and excuse a timing
// miss only when it was there.
class ForeignActivity {
 public:
  ForeignActivity() { Start(); }

  void Start() {
#if defined(_WIN32)
    LASTINPUTINFO info{};
    info.cbSize = sizeof(info);
    input_tick_ = GetLastInputInfo(&info) != 0 ? info.dwTime : 0;
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetSystemTimes(&idle, &kernel, &user) != 0) {
      idle_ = ToU64(idle);
      busy_ = ToU64(kernel) + ToU64(user);
    }
#endif
  }

  // True when the person used the mouse or keyboard since Start().
  [[nodiscard]] bool InputSeen() const {
#if defined(_WIN32)
    LASTINPUTINFO info{};
    info.cbSize = sizeof(info);
    return GetLastInputInfo(&info) != 0 && info.dwTime != input_tick_;
#else
    return false;
#endif
  }

  // The machine's CPU use since Start(), 0 to 1 (kernel time includes
  // idle time, which is taken out).
  [[nodiscard]] double CpuLoad() const {
#if defined(_WIN32)
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetSystemTimes(&idle, &kernel, &user) == 0) return 0.0;
    const double idle_delta = static_cast<double>(ToU64(idle) - idle_);
    const double total_delta =
        static_cast<double>(ToU64(kernel) + ToU64(user) - busy_);
    if (total_delta <= 0.0) return 0.0;
    return std::clamp(1.0 - (idle_delta / total_delta), 0.0, 1.0);
#else
    return 0.0;
#endif
  }

  // A busy machine: input arrived, or other work kept the CPUs more than
  // half busy.
  [[nodiscard]] bool Busy() const { return InputSeen() || CpuLoad() > 0.5; }

 private:
#if defined(_WIN32)
  static std::uint64_t ToU64(const FILETIME& time) {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) |
           time.dwLowDateTime;
  }
  DWORD input_tick_ = 0;
  std::uint64_t idle_ = 0;
  std::uint64_t busy_ = 0;
#endif
};

TEST_CASE("Present caps match the backend and the build", "[present][gpu]") {
  const testing::MainThreadScope main;
  {
    Fixture fixture;
    OpenDevice(fixture);
    GpuDevice& device = fixture.Device();
    const present::Caps caps = present::QueryCaps(device);
    WARN("driver " << device.Driver() << ": patched=" << caps.patched
                   << " waitable=" << caps.frame_latency_waitable
                   << " stats=" << caps.present_stats << " timestamps="
                   << caps.gpu_timestamps << " handles=" << caps.native_handles
                   << " status=" << caps.device_status
                   << " tearing=" << caps.tearing << " (no window)");
#if SE_SDL_PATCHED
    REQUIRE(caps.patched);
    REQUIRE(caps.native_handles);
    REQUIRE(caps.device_status);
    if (IsD3d12(device)) {
      // DXGI always has the waitable, the statistics and timestamps on the
      // direct queue.
      REQUIRE(caps.frame_latency_waitable);
      REQUIRE(caps.present_stats);
      REQUIRE(caps.gpu_timestamps);
    }
    // Without a window, a frame-latency call has nothing to act on.
    REQUIRE_FALSE(present::SetFrameLatency(device, 1));
    REQUIRE(present::WaitFrameLatency(device, kNsPerMs) ==
            present::WaitResult::kUnsupported);
    present::PresentStats stats;
    REQUIRE_FALSE(present::GetPresentStats(device, stats));
#else
    // The planted check of the unpatched build: nothing is supported and
    // every action is a no-op.
    REQUIRE_FALSE(caps.patched);
    REQUIRE_FALSE(caps.frame_latency_waitable);
    REQUIRE_FALSE(caps.present_stats);
    REQUIRE_FALSE(caps.gpu_timestamps);
    REQUIRE_FALSE(caps.native_handles);
    REQUIRE_FALSE(caps.device_status);
    REQUIRE_FALSE(caps.tearing);
    REQUIRE_FALSE(present::SetFrameLatency(device, 1));
    REQUIRE(present::WaitFrameLatency(device, kNsPerMs) ==
            present::WaitResult::kUnsupported);
    present::PresentStats stats;
    REQUIRE_FALSE(present::GetPresentStats(device, stats));
    present::NativeHandles handles;
    REQUIRE_FALSE(present::GetNativeHandles(device, handles));
    REQUIRE(present::QueryDeviceStatus(device) ==
            present::DeviceStatus::kUnknown);
    REQUIRE_FALSE(present::SetPresentMode(device, SDL_GPU_PRESENTMODE_VSYNC));
    SDL_GPUCommandBuffer* commands =
        SDL_AcquireGPUCommandBuffer(device.Native());
    REQUIRE(commands != nullptr);
    present::BeginGpuTiming(device, commands);
    present::EndGpuTiming(device, commands);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    REQUIRE(fence != nullptr);
    REQUIRE(SDL_WaitForGPUFences(device.Native(), true, &fence, 1));
    REQUIRE_FALSE(present::ReadGpuTime(device, fence).has_value());
    SDL_ReleaseGPUFence(device.Native(), fence);
#endif
    device.WaitForIdle();
  }
  SDL_Quit();
}

#if SE_SDL_PATCHED

// A command buffer that clears |target| |clears| times, timed.
SDL_GPUFence* SubmitTimedClears(GpuDevice& device, SDL_GPUTexture* target,
                                int clears, bool end_timing)
    SE_REQUIRES(g_main) {
  SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device.Native());
  REQUIRE(commands != nullptr);
  present::BeginGpuTiming(device, commands);
  for (int i = 0; i < clears; ++i) {
    SDL_GPUColorTargetInfo info{};
    info.texture = target;
    const float shade = static_cast<float>(i % 7) / 7.0F;
    info.clear_color = SDL_FColor{shade, 1.0F - shade, 0.5F, 1.0F};
    info.load_op = SDL_GPU_LOADOP_CLEAR;
    info.store_op = SDL_GPU_STOREOP_STORE;
    // Cycling would hand each clear a fresh texture; the test wants the
    // same memory written every time.
    info.cycle = false;
    SDL_GPURenderPass* pass =
        SDL_BeginGPURenderPass(commands, &info, 1, nullptr);
    REQUIRE(pass != nullptr);
    SDL_EndGPURenderPass(pass);
  }
  // Without EndGpuTiming the end timestamp is written at submit.
  if (end_timing) present::EndGpuTiming(device, commands);
  SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
  REQUIRE(fence != nullptr);
  return fence;
}

TEST_CASE("GPU timing of a 4096x4096 clear", "[present][gpu]") {
  const testing::MainThreadScope main;
  {
    Fixture fixture;
    OpenDevice(fixture);
    GpuDevice& device = fixture.Device();
    REQUIRE(present::QueryCaps(device).gpu_timestamps);
    SDL_GPUTextureCreateInfo create{};
    create.type = SDL_GPU_TEXTURETYPE_2D;
    create.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    create.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    create.width = 4096;
    create.height = 4096;
    create.layer_count_or_depth = 1;
    create.num_levels = 1;
    SDL_GPUTexture* target = SDL_CreateGPUTexture(device.Native(), &create);
    REQUIRE(target != nullptr);

    SECTION("one clear: 0 < duration < 100 ms, read once") {
      SDL_GPUFence* fence = SubmitTimedClears(device, target, 1, true);
      std::optional<present::GpuTime> time =
          present::ReadGpuTime(device, fence);
      // Never a result before the fence signalled.
      if (time.has_value()) REQUIRE(SDL_QueryGPUFence(device.Native(), fence));
      REQUIRE(SDL_WaitForGPUFences(device.Native(), true, &fence, 1));
      if (!time.has_value()) time = present::ReadGpuTime(device, fence);
      REQUIRE(time.has_value());
      const std::uint64_t duration_ns =
          time.value_or(present::GpuTime{}).duration_ns;
      WARN(device.Driver() << ": 4096x4096 clear took " << duration_ns
                           << " ns");
      REQUIRE(duration_ns > 0);
      REQUIRE(duration_ns < 100 * kNsPerMs);
      // A read consumes the result.
      REQUIRE_FALSE(present::ReadGpuTime(device, fence).has_value());
      SDL_ReleaseGPUFence(device.Native(), fence);
    }

    SECTION("before the fence signals, Read returns nullopt") {
      // About 64 x 64 MB of writes: milliseconds of GPU work, so the read
      // right after the submit comes long before the fence.
      SDL_GPUFence* fence = SubmitTimedClears(device, target, 64, false);
      const std::optional<present::GpuTime> early =
          present::ReadGpuTime(device, fence);
      const bool signalled = SDL_QueryGPUFence(device.Native(), fence);
      if (early.has_value()) {
        // Only possible if this thread was descheduled for the whole run.
        REQUIRE(signalled);
        WARN("the fence had already signalled when the test read it");
      }
      REQUIRE(SDL_WaitForGPUFences(device.Native(), true, &fence, 1));
      const std::optional<present::GpuTime> late =
          early.has_value() ? early : present::ReadGpuTime(device, fence);
      REQUIRE(late.has_value());
      const std::uint64_t duration_ns =
          late.value_or(present::GpuTime{}).duration_ns;
      WARN(device.Driver() << ": 64 clears took " << duration_ns
                           << " ns (end written at submit)");
      REQUIRE(duration_ns > 0);
      REQUIRE(duration_ns < 1000 * kNsPerMs);
      SDL_ReleaseGPUFence(device.Native(), fence);
    }

    SECTION("a fence that was not timed has no result") {
      SDL_GPUCommandBuffer* commands =
          SDL_AcquireGPUCommandBuffer(device.Native());
      REQUIRE(commands != nullptr);
      SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
      REQUIRE(fence != nullptr);
      REQUIRE(SDL_WaitForGPUFences(device.Native(), true, &fence, 1));
      REQUIRE_FALSE(present::ReadGpuTime(device, fence).has_value());
      SDL_ReleaseGPUFence(device.Native(), fence);
    }

    device.WaitForIdle();
    SDL_ReleaseGPUTexture(device.Native(), target);
  }
  SDL_Quit();
}

TEST_CASE("Native handles are non-null for the active backend",
          "[present][gpu]") {
  const testing::MainThreadScope main;
  {
    Fixture fixture;
    OpenDevice(fixture);
    GpuDevice& device = fixture.Device();
    present::NativeHandles handles;
    REQUIRE(present::GetNativeHandles(device, handles));
    if (IsD3d12(device)) {
      REQUIRE(handles.d3d12_device != nullptr);
      REQUIRE(handles.d3d12_queue != nullptr);
      REQUIRE(handles.vk_device == nullptr);
    } else {
      REQUIRE(handles.vk_instance != nullptr);
      REQUIRE(handles.vk_physical_device != nullptr);
      REQUIRE(handles.vk_device != nullptr);
      REQUIRE(handles.vk_queue != nullptr);
      REQUIRE(handles.d3d12_device == nullptr);
    }
    // No window claimed, no swapchain.
    REQUIRE(handles.dxgi_swapchain == nullptr);
    REQUIRE(handles.vk_swapchain == nullptr);
  }
  SDL_Quit();
}

TEST_CASE("QueryDeviceStatus is kOk", "[present][gpu]") {
  const testing::MainThreadScope main;
  {
    Fixture fixture;
    OpenDevice(fixture);
    GpuDevice& device = fixture.Device();
    REQUIRE(present::QueryDeviceStatus(device) == present::DeviceStatus::kOk);
    // Still fine after work went through the queue.
    SDL_GPUCommandBuffer* commands =
        SDL_AcquireGPUCommandBuffer(device.Native());
    REQUIRE(commands != nullptr);
    REQUIRE(SDL_SubmitGPUCommandBuffer(commands));
    device.WaitForIdle();
    REQUIRE(present::QueryDeviceStatus(device) == present::DeviceStatus::kOk);
  }
  SDL_Quit();
}

// The refresh period of the window's output, measured: the median interval
// between presents paced by VSYNC and the frame-latency wait.
std::uint64_t MeasurePeriodNs(GpuDevice& device, int frames)
    SE_REQUIRES(g_main) {
  std::vector<std::uint64_t> intervals;
  std::uint64_t previous = 0;
  for (int i = 0; i < frames; ++i) {
    (void)present::WaitFrameLatency(device, 100 * kNsPerMs);
    const std::uint64_t now = SDL_GetTicksNS();
    if (previous != 0) intervals.push_back(now - previous);
    previous = now;
    PresentFrame(device, 0.5F);
  }
  return Median(std::move(intervals));
}

void CheckLatencyWaits(int latency) SE_REQUIRES(g_main) {
  Fixture fixture;
  OpenWindow(fixture);
  GpuDevice& device = fixture.Device();
  if (!present::QueryCaps(device).frame_latency_waitable) {
    const std::string driver(device.Driver());
    fixture.Close();
    SKIP(driver << " has no frame-latency wait (Vulkan needs "
                   "VK_KHR_present_id and VK_KHR_present_wait)");
  }
  REQUIRE(present::SetFrameLatency(device, latency));
  const std::uint64_t period_ns = MeasurePeriodNs(device, 30);
  REQUIRE(period_ns > 0);
  const DisplayRefresh refresh = fixture.window->Refresh();
  // Two refreshes, plus timer slack.
  const std::uint64_t bound_ns = (2 * period_ns) + (kNsPerMs / 2);

  ForeignActivity activity;
  int ready = 0;
  int late = 0;
  std::uint64_t longest_ns = 0;
  for (int i = 0; i < 120; ++i) {
    const std::uint64_t start = SDL_GetTicksNS();
    const present::WaitResult result =
        present::WaitFrameLatency(device, bound_ns);
    const std::uint64_t waited = SDL_GetTicksNS() - start;
    longest_ns = std::max(longest_ns, waited);
    if (result == present::WaitResult::kReady && waited <= bound_ns) {
      ++ready;
    } else {
      REQUIRE(result != present::WaitResult::kUnsupported);
      ++late;
    }
    PresentFrame(device, static_cast<float>(i % 2));
  }
  WARN(device.Driver() << " latency " << latency << ": " << ready
                       << "/120 waits ready within 2 refreshes (period "
                       << period_ns << " ns measured, SDL display mode "
                       << refresh.hz << " Hz), longest " << longest_ns
                       << " ns; foreign input " << activity.InputSeen()
                       << ", CPU load " << activity.CpuLoad());
  if (late > 0 && activity.Busy()) {
    // Other work kept this thread from running at its vblanks: the late
    // waits say nothing about the wait itself, which still has to work.
    WARN(late << " late waits excused: the machine was busy");
    REQUIRE(ready > 0);
  } else {
    REQUIRE(late == 0);
  }
  fixture.Close();
}

TEST_CASE("Frame latency 1: every wait is ready within 2 refreshes",
          "[present][desktop][gpu]") {
  const testing::MainThreadScope main;
  CheckLatencyWaits(1);
  SDL_Quit();
}

TEST_CASE("Frame latency 2: every wait is ready within 2 refreshes",
          "[present][desktop][gpu]") {
  const testing::MainThreadScope main;
  CheckLatencyWaits(2);
  SDL_Quit();
}

// Present statistics over 120 frames at frame latency 1 (where the backend
// has the wait): each sample after the frame's wait.
void CheckPresentStatistics(WindowMode mode) SE_REQUIRES(g_main) {
  Fixture fixture;
  OpenWindow(fixture, mode);
  GpuDevice& device = fixture.Device();
  const present::Caps caps = present::QueryCaps(device);
  if (!caps.present_stats) {
    const std::string driver(device.Driver());
    fixture.Close();
    SKIP(driver << " reports no present statistics (Vulkan needs "
                   "VK_GOOGLE_display_timing)");
  }
  const bool waitable =
      caps.frame_latency_waitable && present::SetFrameLatency(device, 1);
  // Warm up: the first statistics come after the first displayed present.
  present::PresentStats stats;
  (void)present::GetPresentStats(device, stats);
  for (int i = 0; i < 30; ++i) {
    if (waitable) (void)present::WaitFrameLatency(device, 100 * kNsPerMs);
    PresentFrame(device, 0.5F);
  }

  struct Sample {
    present::PresentStats stats;
    std::uint64_t now_ns = 0;
  };
  std::vector<Sample> samples;
  std::string no_stats_reason;
  ForeignActivity activity;
  constexpr int kFrames = 120;
  for (int i = 0; i < kFrames; ++i) {
    if (waitable) (void)present::WaitFrameLatency(device, 100 * kNsPerMs);
    Sample sample;
    if (present::GetPresentStats(device, sample.stats)) {
      sample.now_ns = SDL_GetTicksNS();
      samples.push_back(sample);
    } else if (no_stats_reason.empty()) {
      no_stats_reason = SDL_GetError();
    }
    PresentFrame(device, static_cast<float>(i % 2));
  }
  // A window DWM composes (D3D12): statistics arrive only while the display
  // is awake, and they describe DWM's composed presents, so a refresh may
  // show two of the window's presents (measured at the integration,
  // ADR-052, ADR-066); the per-refresh rule below holds in independent
  // flip.
  const bool composed = mode == WindowMode::kUnfocused && IsD3d12(device);
  if (samples.empty() && composed) {
    // DXGI keeps the statistics of a composed (windowed) flip-model
    // swapchain at zero while the display sleeps (ADR-052); independent
    // flip needs the screen, which a desktop test cannot take.
    const std::string driver(device.Driver());
    fixture.Close();
    SKIP(driver << " windowed: " << no_stats_reason
                << " after 150 presents (composed flip model, "
                   "ADR-052); run the [.fullscreen] variant "
                   "(docs/gates.md)");
  }
  INFO("first frame without statistics: " << no_stats_reason);
  REQUIRE(samples.size() >= static_cast<std::size_t>(kFrames - 2));

  int one_step = 0;
  int other_steps = 0;
  int shared_refreshes = 0;
  // Samples whose sync time was more than 100 ms old: a compositor or
  // scheduler hiccup (one sample at 103 ms was seen on an idle machine);
  // a few are reported, more fail unless the machine was busy, none may be
  // a second old (frozen statistics), and a window DWM stopped composing
  // (stale syncs with presents not shown) skips.
  int stale_samples = 0;
  std::uint64_t max_sync_age = 0;
  std::uint64_t max_sync_lag = 0;
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const present::PresentStats& s = samples[i].stats;
    // The displayed present is one this swapchain issued, at a refresh no
    // later than the sampled one, sampled in the recent past.
    REQUIRE(s.present_count <= s.last_present_count);
    REQUIRE(s.present_refresh_count <= s.sync_refresh_count);
    max_sync_lag =
        std::max(max_sync_lag, s.sync_refresh_count - s.present_refresh_count);
    REQUIRE(s.sync_time_ns <= samples[i].now_ns);
    const std::uint64_t sync_age = samples[i].now_ns - s.sync_time_ns;
    max_sync_age = std::max(max_sync_age, sync_age);
    if (sync_age >= 100 * kNsPerMs) ++stale_samples;
    if (i == 0) continue;
    const present::PresentStats& p = samples[i - 1].stats;
    // One present per frame was issued between the samples.
    REQUIRE(s.last_present_count == p.last_present_count + 1);
    REQUIRE(s.present_count >= p.present_count);
    REQUIRE(s.sync_refresh_count >= p.sync_refresh_count);
    // No refresh shows two presents (independent flip).
    if (s.present_refresh_count - p.present_refresh_count <
        s.present_count - p.present_count) {
      ++shared_refreshes;
    }
    if (!composed) {
      REQUIRE(s.present_refresh_count - p.present_refresh_count >=
              s.present_count - p.present_count);
    }
    if (s.present_count == p.present_count + 1) {
      ++one_step;
    } else {
      ++other_steps;
    }
  }
  const present::PresentStats& first = samples.front().stats;
  const present::PresentStats& last = samples.back().stats;
  const std::uint64_t presents = last.present_count - first.present_count;
  const std::uint64_t issued =
      last.last_present_count - first.last_present_count;
  WARN(device.Driver()
       << ": " << samples.size() << " samples, " << one_step
       << " advanced by one present, " << other_steps
       << " otherwise; displayed " << presents << " of " << issued
       << " issued over "
       << (last.present_refresh_count - first.present_refresh_count)
       << " refreshes; sync lag at most " << max_sync_lag << "; "
       << shared_refreshes << " samples where one refresh showed two presents"
       << (composed ? " (composed by DWM)" : "") << "; " << stale_samples
       << " samples with a sync time over 100 ms old (at most "
       << static_cast<double>(max_sync_age) / 1e6 << " ms); foreign input "
       << activity.InputSeen() << ", CPU load " << activity.CpuLoad());
  if (composed && stale_samples > 2 && presents + 2 < issued) {
    // DWM stopped composing the window for a while (its sync times went
    // stale and it showed fewer presents than were issued; seen twice in
    // one night run, 84 and 22 of 119 shown, at most 219 and 595 ms stale):
    // the statistics froze as they do at zero while the display sleeps
    // (ADR-052), so nothing below can be measured.
    const std::string driver(device.Driver());
    fixture.Close();
    SKIP(driver << " windowed: DWM composed " << presents << " of " << issued
                << " presents and " << stale_samples << " of " << samples.size()
                << " samples had a sync time over 100 ms old (at most "
                << static_cast<double>(max_sync_age) / 1e6
                << " ms): the compositor was not composing (ADR-091)");
  }
  // The statistics are live: none is a second old.
  REQUIRE(max_sync_age < 1000 * kNsPerMs);
  // Every issued present is displayed (VSYNC drops none), and the displayed
  // refresh follows the sampled one closely.
  REQUIRE(presents + 2 >= issued);
  REQUIRE(presents <= issued);
  const bool mostly_one_step =
      one_step * 10 >= static_cast<int>(samples.size()) * 9;
  const bool small_lag = max_sync_lag <= 2;
  const bool fresh = stale_samples <= 2;
  if (activity.Busy() && !(mostly_one_step && small_lag && fresh)) {
    // Excused: other work delayed this thread's samples or presents.
    WARN("statistics outside the bounds excused: the machine was busy");
  } else {
    REQUIRE(mostly_one_step);
    REQUIRE(small_lag);
    REQUIRE(fresh);
  }
  fixture.Close();
}

TEST_CASE("Present statistics advance one present at a time",
          "[present][desktop][gpu]") {
  const testing::MainThreadScope main;
  CheckPresentStatistics(WindowMode::kUnfocused);
  SDL_Quit();
}

// Hidden (never discovered by CTest): it takes the screen and the focus.
// docs/gates.md, "Present statistics in independent flip", says how to run
// it.
TEST_CASE("Present statistics advance in independent flip",
          "[present][.fullscreen]") {
  const testing::MainThreadScope main;
  CheckPresentStatistics(WindowMode::kFullscreen);
  SDL_Quit();
}

TEST_CASE("Native handles include the claimed window's swapchain",
          "[present][desktop][gpu]") {
  const testing::MainThreadScope main;
  {
    Fixture fixture;
    OpenWindow(fixture);
    GpuDevice& device = fixture.Device();
    PresentFrame(device, 0.5F);
    present::NativeHandles handles;
    REQUIRE(present::GetNativeHandles(device, handles));
    if (IsD3d12(device)) {
      REQUIRE(handles.d3d12_device != nullptr);
      REQUIRE(handles.dxgi_swapchain != nullptr);
    } else {
      REQUIRE(handles.vk_device != nullptr);
      REQUIRE(handles.vk_swapchain != nullptr);
    }
    fixture.Close();
  }
  SDL_Quit();
}

TEST_CASE("The frame-latency wait survives a resize and a present mode change",
          "[present][desktop][gpu]") {
  const testing::MainThreadScope main;
  {
    Fixture fixture;
    OpenWindow(fixture);
    GpuDevice& device = fixture.Device();
    const present::Caps caps = present::QueryCaps(device);
    if (!caps.frame_latency_waitable) {
      const std::string driver(device.Driver());
      fixture.Close();
      SKIP(driver << " has no frame-latency wait");
    }
    REQUIRE(present::SetFrameLatency(device, 1));
    for (int i = 0; i < 5; ++i) {
      REQUIRE(present::WaitFrameLatency(device, 100 * kNsPerMs) !=
              present::WaitResult::kUnsupported);
      REQUIRE(PresentFrame(device, 0.25F));
    }
    // The test resizes its own window: SDL resizes the swapchain buffers at
    // the next acquire (D3D12: ResizeBuffers, which must keep the
    // swapchain's flags, or DXGI refuses it).
    REQUIRE(SDL_SetWindowSize(fixture.window->Native(), 400, 300));
    REQUIRE(SDL_SyncWindow(fixture.window->Native()));
    int presented = 0;
    for (int i = 0; i < 10; ++i) {
      REQUIRE(present::WaitFrameLatency(device, 100 * kNsPerMs) !=
              present::WaitResult::kUnsupported);
      if (PresentFrame(device, 0.75F)) ++presented;
    }
    REQUIRE(presented >= 8);
    // A present mode change recreates the swapchain; it keeps the waitable.
    REQUIRE(present::SetPresentMode(device, SDL_GPU_PRESENTMODE_IMMEDIATE));
    WARN(device.Driver() << ": tearing " << caps.tearing << " (window "
                         << present::QueryCaps(device).tearing << ")");
    for (int i = 0; i < 10; ++i) {
      REQUIRE(present::WaitFrameLatency(device, 100 * kNsPerMs) !=
              present::WaitResult::kUnsupported);
      REQUIRE(PresentFrame(device, 0.25F));
    }
    REQUIRE(present::SetPresentMode(device, SDL_GPU_PRESENTMODE_VSYNC));
    // Latency 2 on the recreated swapchain.
    REQUIRE(present::SetFrameLatency(device, 2));
    for (int i = 0; i < 10; ++i) {
      REQUIRE(present::WaitFrameLatency(device, 100 * kNsPerMs) !=
              present::WaitResult::kUnsupported);
      REQUIRE(PresentFrame(device, 0.75F));
    }
    REQUIRE(present::QueryDeviceStatus(device) == present::DeviceStatus::kOk);
    fixture.Close();
  }
  SDL_Quit();
}

#endif  // SE_SDL_PATCHED

TEST_CASE("Frames present whatever the present caps",
          "[present][desktop][gpu]") {
  const testing::MainThreadScope main;
  {
    Fixture fixture;
    OpenWindow(fixture);
    GpuDevice& device = fixture.Device();
    const present::Caps caps = present::QueryCaps(device);
    WARN(device.Driver() << " with a window: patched=" << caps.patched
                         << " waitable=" << caps.frame_latency_waitable
                         << " stats=" << caps.present_stats
                         << " timestamps=" << caps.gpu_timestamps
                         << " handles=" << caps.native_handles << " status="
                         << caps.device_status << " tearing=" << caps.tearing);
#if !SE_SDL_PATCHED
    REQUIRE_FALSE(caps.patched);
    REQUIRE_FALSE(caps.frame_latency_waitable);
    REQUIRE_FALSE(caps.present_stats);
    REQUIRE_FALSE(caps.tearing);
    // Standard pacing: SDL's own swapchain wait.
    REQUIRE_FALSE(present::SetFrameLatency(device, 1));
#endif
    int presented = 0;
    for (int i = 0; i < 60; ++i) {
      if (PresentFrame(device, static_cast<float>(i % 2))) ++presented;
    }
    REQUIRE(presented >= 55);
    fixture.Close();
  }
  SDL_Quit();
}

}  // namespace
}  // namespace se
