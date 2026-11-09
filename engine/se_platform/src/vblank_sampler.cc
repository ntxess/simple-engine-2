// Copyright 2026 The simple-engine-2 Authors.

#include "se_platform/vblank_sampler.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <semaphore>
#include <stop_token>
#include <utility>

#include "se_core/error.h"
#include "se_core/log.h"
#include "se_core/profile.h"
#include "se_core/thread_role.h"
#include "se_os/thread.h"
#include "se_platform/display_output.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// windows.h first.
#include <dxgi.h>
#endif

namespace se {
namespace {

constexpr std::uint64_t kStopBoundNs = 100'000'000;
constexpr std::uint64_t kStopSlackNs = 50'000'000;

#if defined(_WIN32)

// QueryPerformanceCounter values in the SDL_GetTicksNS domain. SDL's ticks
// are QueryPerformanceCounter scaled from SDL's start, so one calibration
// pair maps every later value exactly (to the QPC resolution).
struct QpcMapping {
  std::int64_t qpc_base = 0;
  std::uint64_t ns_base = 0;
  std::int64_t frequency = 1;

  [[nodiscard]] std::uint64_t ToNs(std::int64_t qpc) const noexcept {
    const std::int64_t delta = qpc - qpc_base;
    const std::int64_t whole = delta / frequency;
    const std::int64_t rest = delta % frequency;
    const std::int64_t ns =
        whole * 1'000'000'000 + rest * 1'000'000'000 / frequency;
    if (ns >= 0) return ns_base + static_cast<std::uint64_t>(ns);
    const auto back = static_cast<std::uint64_t>(-ns);
    return back > ns_base ? 0 : ns_base - back;
  }
};

QpcMapping CalibrateQpc() noexcept {
  LARGE_INTEGER frequency{};
  LARGE_INTEGER before{};
  LARGE_INTEGER after{};
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&before);
  const std::uint64_t ticks = SDL_GetTicksNS();
  QueryPerformanceCounter(&after);
  QpcMapping mapping;
  mapping.frequency = std::max<std::int64_t>(1, frequency.QuadPart);
  mapping.qpc_base = before.QuadPart + (after.QuadPart - before.QuadPart) / 2;
  mapping.ns_base = ticks;
  return mapping;
}

// The DXGI output that shows a monitor. COM objects are released by
// whichever thread drops the source (DXGI is free-threaded).
class DxgiVblankSource final : public VblankSource {
 public:
  explicit DxgiVblankSource(const QpcMapping& mapping) : mapping_(mapping) {}
  ~DxgiVblankSource() override { ReleaseOutput(); }

  bool Resolve(MonitorHandle monitor) override {
    ReleaseOutput();
    if (monitor == nullptr) return false;
    // A new factory each time: an old one does not see outputs that a
    // display change added.
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(&factory)))) {
      return false;
    }
    auto* const wanted = static_cast<HMONITOR>(monitor);
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0;
         output_ == nullptr &&
         factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND;
         ++a) {
      IDXGIOutput* output = nullptr;
      for (UINT o = 0; output_ == nullptr &&
                       adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND;
           ++o) {
        DXGI_OUTPUT_DESC desc{};
        if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == wanted) {
          output_ = output;
          adapter_index_ = a;
          output_index_ = o;
        } else {
          output->Release();
        }
      }
      adapter->Release();
    }
    factory->Release();
    if (output_ != nullptr) {
      SE_LOG_INFO("vblank sampler: monitor {} is output {} of adapter {}",
                  monitor, output_index_, adapter_index_);
    }
    return output_ != nullptr;
  }

  VblankWait Wait() override {
    VblankWait wait;
    if (output_ == nullptr) return wait;
    const HRESULT result = output_->WaitForVBlank();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    wait.stamp_ns = mapping_.ToNs(now.QuadPart);
    wait.kind = SUCCEEDED(result) ? VblankWait::Kind::kVblank
                                  : VblankWait::Kind::kFailed;
    return wait;
  }

 private:
  void ReleaseOutput() noexcept {
    if (output_ != nullptr) output_->Release();
    output_ = nullptr;
  }

  QpcMapping mapping_;
  IDXGIOutput* output_ = nullptr;
  UINT adapter_index_ = 0;
  UINT output_index_ = 0;
};

#endif

}  // namespace

struct VblankSampler::State {
  State(std::unique_ptr<VblankSource> source_in,
        const VblankSamplerConfig& config_in)
      : source(std::move(source_in)), config(config_in) {}

  void Run(const std::stop_token& stop);
  // Sleeps up to |ns|, or until a stop or a new SetMonitor.
  void Backoff(const std::stop_token& stop, std::uint64_t ns,
               std::uint32_t generation);
  void Publish(std::array<std::uint64_t, kIntervals>& ring,
               std::size_t size) noexcept;

  std::unique_ptr<VblankSource> source;
  const VblankSamplerConfig config;

  // Main to sampler: the monitor (relaxed), then its generation (release).
  std::atomic<MonitorHandle> requested_monitor{nullptr};
  std::atomic<std::uint32_t> requested_generation{0};
  // Sampler to readers. The stamp is the one release store per vblank.
  std::atomic<std::uint32_t> acted_generation{0};
  std::atomic<bool> resolved{false};
  std::atomic<MonitorHandle> resolved_monitor{nullptr};
  std::atomic<std::uint64_t> last_stamp{0};
  std::atomic<std::uint64_t> count{0};
  std::atomic<std::uint64_t> period{0};
  std::atomic<std::uint64_t> spread{0};
  std::atomic<bool> output_off{false};
  std::atomic<bool> running{false};
  std::atomic<bool> detached{false};
  std::atomic<std::uint64_t> early_returns{0};
  std::atomic<std::uint64_t> failures{0};
  std::atomic<std::uint64_t> resolves{0};

  // The backoff waits; only the sampler thread waits, and only Stop's
  // stop request wakes it (main never takes this lock).
  std::mutex mutex;
  std::condition_variable_any wake;
  // Released when Run has returned.
  std::binary_semaphore exited{0};
};

void VblankSampler::State::Backoff(const std::stop_token& stop,
                                   std::uint64_t ns, std::uint32_t generation) {
  // A SetMonitor ends the backoff at the next poll; main never notifies.
  constexpr std::uint64_t kPollNs = 20'000'000;
  std::uint64_t waited = 0;
  std::unique_lock lock(mutex);
  while (waited < ns && !stop.stop_requested() &&
         requested_generation.load(std::memory_order_acquire) == generation) {
    const std::uint64_t slice = std::min(kPollNs, ns - waited);
    wake.wait_for(lock, stop, std::chrono::nanoseconds(slice),
                  [] { return false; });
    waited += slice;
  }
}

void VblankSampler::State::Publish(std::array<std::uint64_t, kIntervals>& ring,
                                   std::size_t size) noexcept {
  std::array<std::uint64_t, kIntervals> sorted = ring;
  const auto last = sorted.begin() + static_cast<std::ptrdiff_t>(size);
  std::sort(sorted.begin(), last);
  const std::uint64_t median = sorted[size / 2];
  const std::uint64_t p10 = sorted[(size - 1) / 10];
  const std::uint64_t p90 = sorted[(size - 1) * 9 / 10];
  period.store(median, std::memory_order_relaxed);
  spread.store(p90 - p10, std::memory_order_relaxed);
}

void VblankSampler::State::Run(const std::stop_token& stop) {
  std::array<std::uint64_t, kIntervals> ring{};
  std::size_t ring_head = 0;
  std::size_t ring_size = 0;
  std::uint32_t seen = 0;
  bool force_resolve = true;
  bool have_output = false;
  bool logged_missing = false;
  std::uint64_t prev = 0;
  std::uint32_t early_run = 0;
  std::uint32_t failure_run = 0;
  while (!stop.stop_requested()) {
    const std::uint32_t generation =
        requested_generation.load(std::memory_order_acquire);
    if (generation != seen || force_resolve) {
      const bool new_request = generation != seen;
      seen = generation;
      force_resolve = false;
      const MonitorHandle monitor =
          requested_monitor.load(std::memory_order_relaxed);
      have_output = source->Resolve(monitor);
      resolves.fetch_add(1, std::memory_order_relaxed);
      ring_head = 0;
      ring_size = 0;
      prev = 0;
      early_run = 0;
      failure_run = 0;
      period.store(0, std::memory_order_relaxed);
      spread.store(0, std::memory_order_relaxed);
      output_off.store(false, std::memory_order_relaxed);
      resolved_monitor.store(have_output ? monitor : nullptr,
                             std::memory_order_relaxed);
      resolved.store(have_output, std::memory_order_relaxed);
      acted_generation.store(generation, std::memory_order_release);
      if (!have_output && (new_request || !logged_missing)) {
        SE_LOG_WARN("vblank sampler: no output shows monitor {}", monitor);
        logged_missing = true;
      }
      if (have_output) logged_missing = false;
    }
    if (!have_output) {
      Backoff(stop, config.resolve_retry_ns, generation);
      // Resolve again (the output may have appeared).
      force_resolve = true;
      continue;
    }
    VblankWait wait;
    {
      SE_PROFILE_ZONE("Vblank wait");
      wait = source->Wait();
    }
    if (stop.stop_requested()) break;
    if (wait.kind == VblankWait::Kind::kFailed) {
      failures.fetch_add(1, std::memory_order_relaxed);
      prev = 0;
      if (++failure_run >= 3) {
        // The output may be gone: resolve it again after the backoff.
        failure_run = 0;
        force_resolve = true;
      }
      Backoff(stop, config.failure_backoff_ns, generation);
      continue;
    }
    failure_run = 0;
    if (prev == 0) {
      prev = wait.stamp_ns;
      continue;
    }
    if (wait.stamp_ns < prev + config.min_interval_ns) {
      // Not a vblank: the next interval starts from the next wait.
      early_returns.fetch_add(1, std::memory_order_relaxed);
      prev = 0;
      if (++early_run >= 3) {
        early_run = 0;
        output_off.store(true, std::memory_order_relaxed);
        Backoff(stop, config.early_backoff_ns, generation);
      }
      continue;
    }
    early_run = 0;
    output_off.store(false, std::memory_order_relaxed);
    const std::uint64_t interval = wait.stamp_ns - prev;
    prev = wait.stamp_ns;
    const std::uint64_t known = period.load(std::memory_order_relaxed);
    std::uint64_t vblanks = 1;
    if (known > 0 && interval > known + known / 2) {
      // The sampler missed some (it was not scheduled in time): count them,
      // keep the gap out of the period.
      vblanks = (interval + known / 2) / known;
    } else {
      ring[(ring_head + ring_size) % kIntervals] = interval;
      if (ring_size < kIntervals) {
        ++ring_size;
      } else {
        ring_head = (ring_head + 1) % kIntervals;
      }
      if (ring_size >= config.min_intervals) Publish(ring, ring_size);
    }
    last_stamp.store(wait.stamp_ns, std::memory_order_release);
    count.fetch_add(vblanks, std::memory_order_release);
  }
  running.store(false, std::memory_order_release);
}

bool VblankSampler::Supported() noexcept {
#if defined(_WIN32)
  return true;
#else
  return false;
#endif
}

std::expected<std::unique_ptr<VblankSampler>, Error> VblankSampler::Start(
    const VblankSamplerConfig& config) {
#if defined(_WIN32)
  return StartWithSource(std::make_unique<DxgiVblankSource>(CalibrateQpc()),
                         config);
#else
  static_cast<void>(config);
  return Fail(ErrorCode::kUnsupported,
              "the vblank sampler needs DXGI (Windows only)");
#endif
}

std::expected<std::unique_ptr<VblankSampler>, Error>
VblankSampler::StartWithSource(std::unique_ptr<VblankSource> source,
                               const VblankSamplerConfig& config) {
  if (source == nullptr) {
    return Fail(ErrorCode::kInvalidArgument,
                "the vblank sampler needs a source");
  }
  auto state = std::make_shared<State>(std::move(source), config);
  state->requested_monitor.store(config.monitor, std::memory_order_relaxed);
  state->requested_generation.store(1, std::memory_order_release);
  state->running.store(true, std::memory_order_release);
  auto thread = StartThread({.role = ThreadRole::kVblankSampler,
                             .name = "vblank-sampler",
                             .qos = Qos::kHigh},
                            [state](const std::stop_token& stop) {
                              state->Run(stop);
                              state->exited.release();
                            });
  if (!thread) {
    state->running.store(false, std::memory_order_release);
    return std::unexpected(std::move(thread.error()));
  }
  return std::unique_ptr<VblankSampler>(
      new VblankSampler(std::move(state), std::move(*thread)));
}

VblankSampler::VblankSampler(std::shared_ptr<State> state,
                             Thread thread) noexcept
    : state_(std::move(state)), thread_(std::move(thread)) {}

VblankSampler::~VblankSampler() { Stop(); }

void VblankSampler::SetMonitor(MonitorHandle monitor) noexcept {
  state_->requested_monitor.store(monitor, std::memory_order_relaxed);
  state_->requested_generation.fetch_add(1, std::memory_order_release);
}

MonitorHandle VblankSampler::RequestedMonitor() const noexcept {
  return state_->requested_monitor.load(std::memory_order_relaxed);
}

VblankReading VblankSampler::Read(std::uint64_t now_ns) const noexcept {
  const State& state = *state_;
  VblankReading reading;
  reading.running = state.running.load(std::memory_order_acquire) &&
                    !state.detached.load(std::memory_order_acquire);
  const bool current =
      state.acted_generation.load(std::memory_order_acquire) ==
      state.requested_generation.load(std::memory_order_acquire);
  reading.resolved = current && state.resolved.load(std::memory_order_relaxed);
  reading.last_vblank_ns = state.last_stamp.load(std::memory_order_acquire);
  reading.count = state.count.load(std::memory_order_acquire);
  reading.period_ns = state.period.load(std::memory_order_relaxed);
  reading.spread_ns = state.spread.load(std::memory_order_relaxed);
  reading.monitor = state.resolved_monitor.load(std::memory_order_relaxed);
  reading.output_off = state.output_off.load(std::memory_order_relaxed);
  reading.early_returns = state.early_returns.load(std::memory_order_relaxed);
  reading.failures = state.failures.load(std::memory_order_relaxed);
  reading.resolves = state.resolves.load(std::memory_order_relaxed);
  reading.fresh =
      reading.last_vblank_ns != 0 &&
      (now_ns <= reading.last_vblank_ns ||
       now_ns - reading.last_vblank_ns <= state.config.stale_after_ns);
  reading.valid = reading.running && reading.resolved && reading.fresh &&
                  !reading.output_off && reading.period_ns > 0;
  return reading;
}

VblankStop VblankSampler::Stop() noexcept {
  if (!thread_.Joinable()) return VblankStop::kNotRunning;
  thread_.RequestStop();
  const std::uint64_t period = state_->period.load(std::memory_order_relaxed);
  const std::uint64_t bound_ns =
      std::min(period > 0 ? period : kStopBoundNs, kStopBoundNs) + kStopSlackNs;
  if (state_->exited.try_acquire_for(std::chrono::nanoseconds(bound_ns))) {
    thread_.Join();
    return VblankStop::kJoined;
  }
  // Still in its wait (an output that stopped refreshing): it ends when
  // the wait returns, and owns its state until then.
  state_->detached.store(true, std::memory_order_release);
  thread_.Detach();
  return VblankStop::kDetached;
}

}  // namespace se
