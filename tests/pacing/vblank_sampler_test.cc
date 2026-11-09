// Copyright 2026 The simple-engine-2 Authors.
//
// The vblank sampler's loop on a fake vblank source (every platform, TSan
// included; docs/milestone-specs.md M6b "Tests"): it publishes stamps, a
// count and a median period, re-resolves the output across a monitor
// change, detects waits that return at once, turns stale when the wait
// blocks, and Stop joins in time or, while the wait is blocked, detaches
// the thread, which ends once the wait returns.

#include "se_platform/vblank_sampler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "se_core/thread_registry.h"
#include "se_core/thread_role.h"
#include "se_platform/display_output.h"

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

using Clock = std::chrono::steady_clock;

// sleep_until with sub-millisecond precision: Windows' default timer
// resolution would round the fake's 4 ms vblanks to whole timer ticks, so
// the fake waits on a high-resolution waitable timer there (no spin).
void PreciseSleepUntil(Clock::time_point when) {
#if defined(_WIN32)
  thread_local HANDLE timer = CreateWaitableTimerExW(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
      TIMER_ALL_ACCESS);
  const auto left = when - Clock::now();
  if (left <= Clock::duration::zero()) return;
  if (timer == nullptr) {
    std::this_thread::sleep_until(when);
    return;
  }
  LARGE_INTEGER due{};
  // Relative, in 100 ns units.
  due.QuadPart = -std::max<std::int64_t>(
      1,
      std::chrono::duration_cast<std::chrono::nanoseconds>(left).count() / 100);
  if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0) != 0) {
    WaitForSingleObject(timer, INFINITE);
  } else {
    std::this_thread::sleep_until(when);
  }
#else
  std::this_thread::sleep_until(when);
#endif
}

std::uint64_t NowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now().time_since_epoch())
          .count());
}

// What the test controls; shared with the source the sampler owns.
struct FakeDisplay {
  // Monitor -> vblank period.
  std::map<MonitorHandle, std::uint64_t> periods;
  std::atomic<bool> early{false};
  std::atomic<bool> fail{false};
  std::atomic<bool> block{false};
  // Every n-th wait returns one vblank late (0: never), as a sampler that
  // was not scheduled in time would.
  std::atomic<std::uint32_t> skip_every{0};
  std::atomic<std::uint32_t> resolves{0};
  std::atomic<std::uint32_t> waits_blocked{0};
  std::mutex mutex;
  std::condition_variable released;
  bool release = false;

  void Release() {
    {
      const std::lock_guard lock(mutex);
      release = true;
      block.store(false);
    }
    released.notify_all();
  }
};

class FakeVblankSource final : public VblankSource {
 public:
  explicit FakeVblankSource(std::shared_ptr<FakeDisplay> display)
      : display_(std::move(display)) {}

  bool Resolve(MonitorHandle monitor) override {
    display_->resolves.fetch_add(1);
    const auto it = display_->periods.find(monitor);
    if (it == display_->periods.end()) return false;
    period_ = std::chrono::nanoseconds(it->second);
    next_ = Clock::now() + period_;
    return true;
  }

  VblankWait Wait() override {
    if (display_->block.load()) {
      display_->waits_blocked.fetch_add(1);
      std::unique_lock lock(display_->mutex);
      display_->released.wait(lock, [&] { return display_->release; });
    }
    if (display_->fail.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      return {VblankWait::Kind::kFailed, NowNs()};
    }
    if (display_->early.load()) {
      // A display that is off: the wait returns at once.
      return {VblankWait::Kind::kVblank, NowNs()};
    }
    // Every vblank of a fixed grid in turn, stamped with the grid's time:
    // a thread that wakes late (a loaded machine) catches up instead of
    // skipping, so the intervals are exact and the tests do not depend on
    // the scheduler. Skipped vblanks come only from skip_every.
    const std::uint32_t skip = display_->skip_every.load();
    if (skip > 0 && ++waits_ % skip == 0) next_ += period_;
    const Clock::time_point vblank = next_;
    PreciseSleepUntil(vblank);
    next_ += period_;
    return {VblankWait::Kind::kVblank,
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    vblank.time_since_epoch())
                    .count())};
  }

 private:
  std::shared_ptr<FakeDisplay> display_;
  std::chrono::nanoseconds period_{4'000'000};
  Clock::time_point next_{};
  std::uint32_t waits_ = 0;
};

// Fake monitor handles: the addresses of distinct objects.
MonitorHandle FakeMonitor(int index) {
  static std::array<char, 3> monitors{};
  return &monitors[static_cast<std::size_t>(index)];
}
const MonitorHandle kMonitorA = FakeMonitor(0);
const MonitorHandle kMonitorB = FakeMonitor(1);
const MonitorHandle kUnknownMonitor = FakeMonitor(2);
constexpr std::uint64_t kPeriodA = 4'000'000;
constexpr std::uint64_t kPeriodB = 8'000'000;

std::shared_ptr<FakeDisplay> TwoMonitors() {
  auto display = std::make_shared<FakeDisplay>();
  display->periods[kMonitorA] = kPeriodA;
  display->periods[kMonitorB] = kPeriodB;
  return display;
}

std::unique_ptr<VblankSampler> StartFake(
    const std::shared_ptr<FakeDisplay>& display, MonitorHandle monitor) {
  VblankSamplerConfig config;
  config.monitor = monitor;
  config.early_backoff_ns = 10'000'000;
  config.failure_backoff_ns = 10'000'000;
  config.resolve_retry_ns = 20'000'000;
  auto sampler = VblankSampler::StartWithSource(
      std::make_unique<FakeVblankSource>(display), config);
  REQUIRE(sampler);
  return std::move(*sampler);
}

// Polls |sampler| until |done| holds (true) or 5 s pass (false).
bool WaitFor(const VblankSampler& sampler,
             const std::function<bool(const VblankReading&)>& done,
             VblankReading* last = nullptr) {
  const Clock::time_point deadline = Clock::now() + std::chrono::seconds(5);
  while (Clock::now() < deadline) {
    const VblankReading reading = sampler.Read(NowNs());
    if (last != nullptr) *last = reading;
    if (done(reading)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

bool Near(std::uint64_t value, std::uint64_t expected, double fraction) {
  const double diff =
      static_cast<double>(value) - static_cast<double>(expected);
  return std::abs(diff) <= fraction * static_cast<double>(expected);
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

TEST_CASE("The vblank sampler publishes stamps, a count and a median period",
          "[vblank]") {
  const auto display = TwoMonitors();
  auto sampler = StartFake(display, kMonitorA);
  VblankReading reading;
  REQUIRE(WaitFor(
      *sampler, [](const VblankReading& r) { return r.valid; }, &reading));
  CHECK(reading.running);
  CHECK(reading.resolved);
  CHECK(reading.fresh);
  CHECK(reading.monitor == kMonitorA);
  REQUIRE(reading.count >= 16);
  CHECK(Near(reading.period_ns, kPeriodA, 0.001));
  REQUIRE(reading.spread_ns == 0);
  CHECK(reading.last_vblank_ns <= NowNs());
  // It runs as the named role.
  REQUIRE(LiveSamplerThreads() == 1);
  // Stamps keep coming.
  const std::uint64_t before = reading.count;
  REQUIRE(WaitFor(*sampler, [before](const VblankReading& r) {
    return r.count >= before + 10;
  }));
  CHECK(sampler->Stop() == VblankStop::kJoined);
  CHECK(sampler->Stop() == VblankStop::kNotRunning);
  CHECK_FALSE(sampler->Read(NowNs()).running);
  REQUIRE(LiveSamplerThreads() == 0);
}

TEST_CASE("The vblank sampler re-resolves across a monitor change",
          "[vblank]") {
  const auto display = TwoMonitors();
  auto sampler = StartFake(display, kMonitorA);
  REQUIRE(WaitFor(*sampler, [](const VblankReading& r) { return r.valid; }));
  sampler->SetMonitor(kMonitorB);
  CHECK(sampler->RequestedMonitor() == kMonitorB);
  // Not valid until the new output is resolved and measured.
  CHECK_FALSE(sampler->Read(NowNs()).valid);
  VblankReading reading;
  REQUIRE(WaitFor(
      *sampler,
      [](const VblankReading& r) { return r.valid && r.monitor == kMonitorB; },
      &reading));
  CHECK(Near(reading.period_ns, kPeriodB, 0.001));
  REQUIRE(reading.resolves >= 2);
  // And back.
  sampler->SetMonitor(kMonitorA);
  REQUIRE(WaitFor(
      *sampler,
      [](const VblankReading& r) { return r.valid && r.monitor == kMonitorA; },
      &reading));
  CHECK(Near(reading.period_ns, kPeriodA, 0.001));
}

TEST_CASE("A monitor no output shows is not resolved", "[vblank]") {
  const auto display = TwoMonitors();
  auto sampler = StartFake(display, kUnknownMonitor);
  VblankReading reading;
  // It keeps trying (the output may appear) and stays invalid.
  REQUIRE(WaitFor(
      *sampler, [](const VblankReading& r) { return r.resolves >= 3; },
      &reading));
  CHECK_FALSE(reading.resolved);
  CHECK_FALSE(reading.valid);
  REQUIRE(reading.monitor == nullptr);
  sampler->SetMonitor(kMonitorA);
  REQUIRE(WaitFor(*sampler, [](const VblankReading& r) { return r.valid; }));
}

TEST_CASE("Waits that return at once mark the output off", "[vblank]") {
  const auto display = TwoMonitors();
  auto sampler = StartFake(display, kMonitorA);
  REQUIRE(WaitFor(*sampler, [](const VblankReading& r) { return r.valid; }));
  display->early.store(true);
  VblankReading reading;
  REQUIRE(WaitFor(
      *sampler, [](const VblankReading& r) { return r.output_off; }, &reading));
  CHECK_FALSE(reading.valid);
  REQUIRE(reading.early_returns >= 3);
  // Early returns publish no stamps: the count stops.
  const std::uint64_t count = reading.count;
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(sampler->Read(NowNs()).count == count);
  display->early.store(false);
  REQUIRE(WaitFor(*sampler, [](const VblankReading& r) {
    return r.valid && !r.output_off;
  }));
}

TEST_CASE("Failed waits are counted and resolve the output again", "[vblank]") {
  const auto display = TwoMonitors();
  auto sampler = StartFake(display, kMonitorA);
  REQUIRE(WaitFor(*sampler, [](const VblankReading& r) { return r.valid; }));
  const std::uint32_t resolves = display->resolves.load();
  display->fail.store(true);
  VblankReading reading;
  REQUIRE(WaitFor(
      *sampler,
      [&](const VblankReading& r) {
        return r.failures >= 3 && display->resolves.load() > resolves;
      },
      &reading));
  // A stale reading is not valid.
  REQUIRE(WaitFor(*sampler, [](const VblankReading& r) { return !r.valid; }));
  display->fail.store(false);
  REQUIRE(WaitFor(*sampler, [](const VblankReading& r) { return r.valid; }));
}

TEST_CASE("Missed vblanks are counted and kept out of the period", "[vblank]") {
  const auto display = TwoMonitors();
  // Every fourth wait comes one vblank late: a 2-period interval.
  display->skip_every.store(4);
  auto sampler = StartFake(display, kMonitorA);
  VblankReading first;
  REQUIRE(WaitFor(
      *sampler,
      [](const VblankReading& r) { return r.valid && r.count >= 160; },
      &first));
  // The gaps that entered the ring before the period was known have left
  // it after 64 more intervals.
  CHECK(Near(first.period_ns, kPeriodA, 0.001));
  REQUIRE(first.spread_ns == 0);
  VblankReading later;
  REQUIRE(WaitFor(
      *sampler,
      [&](const VblankReading& r) { return r.count >= first.count + 40; },
      &later));
  // Every vblank counts, the skipped ones included.
  const std::uint64_t elapsed = later.last_vblank_ns - first.last_vblank_ns;
  CHECK((elapsed + kPeriodA / 2) / kPeriodA == later.count - first.count);
}

TEST_CASE("A blocked wait turns stale, and Stop detaches the thread in time",
          "[vblank]") {
  const auto display = TwoMonitors();
  auto sampler = StartFake(display, kMonitorA);
  REQUIRE(WaitFor(*sampler, [](const VblankReading& r) { return r.valid; }));
  display->block.store(true);
  // The next wait blocks; 100 ms later no vblank is fresh.
  VblankReading reading;
  REQUIRE(WaitFor(
      *sampler, [](const VblankReading& r) { return !r.fresh; }, &reading));
  REQUIRE(display->waits_blocked.load() >= 1);
  CHECK_FALSE(reading.valid);
  CHECK(reading.running);

  // Stop: one refresh (4 ms) plus 50 ms, then detach.
  const Clock::time_point start = Clock::now();
  CHECK(sampler->Stop() == VblankStop::kDetached);
  const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           Clock::now() - start)
                           .count();
  REQUIRE(stop_ms >= 50);
  REQUIRE(stop_ms < 500);
  CHECK_FALSE(sampler->Read(NowNs()).running);
  // The detached thread still holds its slot, and keeps its state alive
  // after the sampler object is gone.
  REQUIRE(LiveSamplerThreads() == 1);
  sampler.reset();
  display->Release();
  const Clock::time_point deadline = Clock::now() + std::chrono::seconds(5);
  while (LiveSamplerThreads() != 0 && Clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  REQUIRE(LiveSamplerThreads() == 0);
  // The thread's last act is dropping its state, and with it the source and
  // the source's share of the display.
  while (display.use_count() > 1 && Clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  REQUIRE(display.use_count() == 1);
}

TEST_CASE("Stop joins within one refresh plus 50 ms", "[vblank]") {
  const auto display = TwoMonitors();
  auto sampler = StartFake(display, kMonitorB);
  REQUIRE(WaitFor(*sampler, [](const VblankReading& r) { return r.valid; }));
  const Clock::time_point start = Clock::now();
  CHECK(sampler->Stop() == VblankStop::kJoined);
  const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           Clock::now() - start)
                           .count();
  REQUIRE(stop_ms <= 58);
}

TEST_CASE("Stop interrupts a backoff at once", "[vblank]") {
  const auto display = TwoMonitors();
  VblankSamplerConfig config;
  config.monitor = kUnknownMonitor;
  config.resolve_retry_ns = 10'000'000'000;
  auto sampler = VblankSampler::StartWithSource(
      std::make_unique<FakeVblankSource>(display), config);
  REQUIRE(sampler);
  REQUIRE(WaitFor(**sampler,
                  [](const VblankReading& r) { return r.resolves >= 1; }));
  const Clock::time_point start = Clock::now();
  CHECK((*sampler)->Stop() == VblankStop::kJoined);
  CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                              start)
            .count() < 58);
}

TEST_CASE("The DXGI sampler exists on Windows only", "[vblank]") {
#if defined(_WIN32)
  CHECK(VblankSampler::Supported());
#else
  CHECK_FALSE(VblankSampler::Supported());
  auto sampler = VblankSampler::Start({});
  REQUIRE_FALSE(sampler);
  CHECK(sampler.error().code == ErrorCode::kUnsupported);
  CHECK(MonitorOfWindow(nullptr) == nullptr);
  CHECK_FALSE(QueryMonitorMode(nullptr).known);
  CHECK(ListMonitors().empty());
#endif
  CHECK_FALSE(VblankSampler::StartWithSource(nullptr, {}));
}

}  // namespace
}  // namespace se
