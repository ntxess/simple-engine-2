// Copyright 2026 The simple-engine-2 Authors.
//
// The vblank sampler (docs/design/mt-design.md "vblank sampler";
// docs/milestone-specs.md M6b). One engine thread, role kVblankSampler,
// HighQoS, started with se::StartThread. On Windows it finds the DXGI output
// that shows the window's monitor (EnumAdapters1 and EnumOutputs on every
// adapter, matching DXGI_OUTPUT_DESC::Monitor), blocks in
// IDXGIOutput::WaitForVBlank, and publishes each vblank's QPC stamp,
// converted to the SDL_GetTicksNS domain, in one 64-bit atomic (release),
// with a vblank count and the median of the last 64 intervals. Elsewhere
// Start() reports kUnsupported; StartWithSource() runs the same loop on any
// VblankSource (the tests' fake, on every platform and under TSan).
//
//   auto sampler = se::VblankSampler::Start({.monitor = MonitorOfWindow(h)});
//   sampler->SetMonitor(MonitorOfWindow(h));  // after DISPLAY_CHANGED
//   const VblankReading now = sampler->Read(SDL_GetTicksNS());
//   sampler->Stop();                          // shutdown step 9
//
// Main publishes a new monitor with SetMonitor (a release store); the
// sampler re-resolves the output on its next wake, and its readings are not
// valid until it has. It never touches World state, SDL, ImGui, the
// swapchain or engine GPU objects.
//
// Bounds. A wait that returns sooner than min_interval_ns after the previous
// one is not a vblank: WaitForVBlank returns at once while the display is
// off (measured on this machine, ADR-056); three in a row mark the output
// "off" and the sampler backs off for early_backoff_ns. The reading turns
// stale when no vblank arrived for stale_after_ns (the 100 ms watchdog: a
// WaitForVBlank that does not return is seen by every reader). Stop()
// returns within one refresh plus 50 ms: it joins the thread when it leaves
// its wait in time, and otherwise marks the sampler stale and detaches the
// thread, which owns everything it uses and ends when the wait returns
// (docs/design/mt-design.md "Shutdown Order" step 9).

#ifndef SE_PLATFORM_VBLANK_SAMPLER_H_
#define SE_PLATFORM_VBLANK_SAMPLER_H_

#include <cstdint>
#include <expected>
#include <memory>

#include "se_core/error.h"
#include "se_os/thread.h"
#include "se_platform/display_output.h"

namespace se {

// One wait for a vblank.
struct VblankWait {
  enum class Kind : std::uint8_t { kVblank, kFailed };
  Kind kind = Kind::kFailed;
  // When the wait returned, in the sampler's clock (SDL_GetTicksNS domain).
  std::uint64_t stamp_ns = 0;
};

// Where vblanks come from. Constructed anywhere; Resolve and Wait run on
// the sampler thread only.
class VblankSource {
 public:
  VblankSource() = default;
  VblankSource(const VblankSource&) = delete;
  VblankSource& operator=(const VblankSource&) = delete;
  virtual ~VblankSource() = default;

  // Finds the output that shows |monitor|; false when none does.
  virtual bool Resolve(MonitorHandle monitor) = 0;
  // Blocks until the next vblank of the resolved output.
  virtual VblankWait Wait() = 0;
};

struct VblankSamplerConfig {
  MonitorHandle monitor = nullptr;
  // A wait returning sooner than this after the previous one is early (no
  // display refreshes faster than 1 kHz).
  std::uint64_t min_interval_ns = 1'000'000;
  // No vblank for this long: the reading is stale.
  std::uint64_t stale_after_ns = 100'000'000;
  // Intervals before the period is published.
  std::uint32_t min_intervals = 16;
  // Backoffs (interrupted by Stop and SetMonitor): after a failed resolve
  // (then resolve again), after a failed wait, after early returns.
  std::uint64_t resolve_retry_ns = 500'000'000;
  std::uint64_t failure_backoff_ns = 100'000'000;
  std::uint64_t early_backoff_ns = 50'000'000;
};

struct VblankReading {
  // The thread runs (it has not been stopped or detached).
  bool running = false;
  // The sampler has resolved an output for the latest SetMonitor.
  bool resolved = false;
  // A vblank arrived within stale_after_ns.
  bool fresh = false;
  // The output's waits return at once (a display that is off).
  bool output_off = false;
  // running, resolved, fresh, not off, and the period is known: what a
  // reader may use.
  bool valid = false;
  // The newest vblank (SDL_GetTicksNS domain) and the vblanks counted so
  // far (gaps of several periods count each vblank).
  std::uint64_t last_vblank_ns = 0;
  std::uint64_t count = 0;
  // Median and p90 - p10 spread of the last 64 intervals; 0 until
  // min_intervals of the current output.
  std::uint64_t period_ns = 0;
  std::uint64_t spread_ns = 0;
  // The monitor of the resolved output.
  MonitorHandle monitor = nullptr;
  // Waits that returned too early, failed waits, and resolves done.
  std::uint64_t early_returns = 0;
  std::uint64_t failures = 0;
  std::uint64_t resolves = 0;
};

enum class VblankStop : std::uint8_t { kNotRunning, kJoined, kDetached };

class VblankSampler {
 public:
  static constexpr std::uint32_t kIntervals = 64;

  // Windows: the DXGI output of config.monitor, stamps from
  // QueryPerformanceCounter mapped to SDL_GetTicksNS (calibrated here, on
  // the calling thread). Elsewhere: kUnsupported.
  [[nodiscard]] static std::expected<std::unique_ptr<VblankSampler>, Error>
  Start(const VblankSamplerConfig& config);
  // The same loop on |source| (tests).
  [[nodiscard]] static std::expected<std::unique_ptr<VblankSampler>, Error>
  StartWithSource(std::unique_ptr<VblankSource> source,
                  const VblankSamplerConfig& config);
  // Whether Start() can work on this platform.
  [[nodiscard]] static bool Supported() noexcept;

  VblankSampler(const VblankSampler&) = delete;
  VblankSampler& operator=(const VblankSampler&) = delete;
  // Stop().
  ~VblankSampler();

  // The output to follow (main thread; never blocks).
  void SetMonitor(MonitorHandle monitor) noexcept;
  [[nodiscard]] MonitorHandle RequestedMonitor() const noexcept;
  // The latest published state. |now_ns| (the stamps' clock) judges
  // freshness. Any thread.
  [[nodiscard]] VblankReading Read(std::uint64_t now_ns) const noexcept;
  // Ends the thread within one refresh plus 50 ms (see the file comment).
  // Idempotent; the caller logs a detach.
  VblankStop Stop() noexcept;

  struct State;

 private:
  VblankSampler(std::shared_ptr<State> state, Thread thread) noexcept;

  std::shared_ptr<State> state_;
  Thread thread_;
};

}  // namespace se

#endif  // SE_PLATFORM_VBLANK_SAMPLER_H_
