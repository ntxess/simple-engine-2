// Copyright 2026 The simple-engine-2 Authors.
//
// The watchdog (docs/milestone-specs.md, "Integration (M4a + M4b + M5)" and
// M12): the pure HangDetector on a fake unbiased clock (5 periods declare a
// hang, once per episode; Modal, Suspended, the heartbeat and a debugger
// each prevent it; a slow boot under 30 s and system sleep trigger nothing;
// the in-driver label; the episode's end; time the detector did not watch
// is not counted), and the watchdog thread with a test dump function, a
// fake clock and a fake debugger (TSan in WSL).
// Stream registration and withdrawal (UnregisterStream, M12) on whichever
// backend the build has.

#include "se_crash/watchdog.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "se_core/error.h"
#include "se_core/thread_registry.h"
#include "se_core/thread_role.h"
#include "se_crash/crash.h"
#include "se_crash/hang_detector.h"
#include "se_os/thread.h"

namespace se::crash {
namespace {

constexpr std::uint64_t kSecond = 1'000'000'000;
constexpr std::uint64_t kMs = 1'000'000;
// The watchdog's sampling: four samples per 1 s period.
constexpr std::uint64_t kSample = 250 * kMs;

HangSample At(std::uint64_t now_ns, std::uint64_t iterates) {
  HangSample sample;
  sample.now_ns = now_ns;
  sample.iterates = iterates;
  return sample;
}

// Samples every kSample from |from| to |to| (inclusive) with |iterates|,
// modified by |change|; returns the verdicts.
template <class F>
std::vector<HangVerdict> SampleRange(HangDetector& detector, std::uint64_t from,
                                     std::uint64_t to, std::uint64_t iterates,
                                     const F& change) {
  std::vector<HangVerdict> verdicts;
  for (std::uint64_t t = from; t <= to; t += kSample) {
    HangSample sample = At(t, iterates);
    change(sample);
    verdicts.push_back(detector.Observe(sample));
  }
  return verdicts;
}

std::vector<HangVerdict> SampleRange(HangDetector& detector, std::uint64_t from,
                                     std::uint64_t to, std::uint64_t iterates) {
  return SampleRange(detector, from, to, iterates, [](HangSample&) {});
}

int Count(const std::vector<HangVerdict>& verdicts, HangVerdict verdict) {
  int n = 0;
  for (const HangVerdict v : verdicts) n += v == verdict ? 1 : 0;
  return n;
}

int Size(const std::vector<HangVerdict>& verdicts) {
  return static_cast<int>(verdicts.size());
}

TEST_CASE("The boot budget is reported once, and never under a debugger",
          "[watchdog]") {
  HangDetector detector({});
  detector.Start(10 * kSecond);
  REQUIRE(detector.Observe(At(39 * kSecond, 0)) == HangVerdict::kProgress);
  REQUIRE(detector.Observe(At(40 * kSecond, 0)) == HangVerdict::kProgress);
  REQUIRE(detector.Observe(At(41 * kSecond, 0)) ==
          HangVerdict::kBootOverBudget);
  REQUIRE(detector.BootOverBudget());
  REQUIRE(detector.Observe(At(42 * kSecond, 0)) == HangVerdict::kProgress);

  HangDetector debugged({});
  debugged.Start(0);
  HangSample sample = At(60 * kSecond, 0);
  sample.debugger = true;
  REQUIRE(debugged.Observe(sample) == HangVerdict::kProgress);
  REQUIRE_FALSE(debugged.BootOverBudget());
}

TEST_CASE("A slow boot under 30 s triggers nothing", "[watchdog]") {
  HangDetector detector({});
  detector.Start(0);
  // 29.75 s of boot without an iterate: the boot budget only.
  const auto boot = SampleRange(detector, 0, 29'750 * kMs, 0);
  REQUIRE(Count(boot, HangVerdict::kProgress) == Size(boot));
  REQUIRE_FALSE(detector.BootOverBudget());
  REQUIRE(detector.Hangs() == 0);
  // The first iterate arms it; the periods start there.
  detector.Arm(1, 29'800 * kMs);
  const auto armed = SampleRange(detector, 30 * kSecond, 34'750 * kMs, 1);
  REQUIRE(Count(armed, HangVerdict::kHang) == 0);
  REQUIRE(detector.Observe(At(34'800 * kMs, 1)) == HangVerdict::kHang);
}

TEST_CASE("Five periods without progress declare one hang per episode",
          "[watchdog]") {
  HangDetector detector({});
  detector.Start(0);
  detector.Arm(7, 0);
  REQUIRE(detector.Armed());
  // 4.75 s without progress: four whole periods, stalled.
  const auto before = SampleRange(detector, kSample, 4'750 * kMs, 7);
  REQUIRE(Count(before, HangVerdict::kStalled) == Size(before));
  REQUIRE(detector.StalledPeriods() == 4);
  REQUIRE_FALSE(detector.InHang());
  // The fifth period ends at 5 s.
  REQUIRE(detector.Observe(At(5 * kSecond, 7)) == HangVerdict::kHang);
  REQUIRE(detector.InHang());
  REQUIRE(detector.Hangs() == 1);
  REQUIRE(detector.StallNs() == 5 * kSecond);
  REQUIRE_FALSE(detector.HangInDriver());
  // The rest of the episode is one hang: no second declaration.
  const auto during = SampleRange(detector, 5'250 * kMs, 30 * kSecond, 7);
  REQUIRE(Count(during, HangVerdict::kHang) == 0);
  REQUIRE(detector.Hangs() == 1);
  // Progress ends the episode, with its length from the last progress.
  REQUIRE(detector.Observe(At(31 * kSecond, 8)) == HangVerdict::kHangEnded);
  REQUIRE_FALSE(detector.InHang());
  REQUIRE(detector.EpisodeNs() == 31 * kSecond);
  REQUIRE(detector.Observe(At(31'250 * kMs, 9)) == HangVerdict::kProgress);
  // A new episode: another hang five periods after the last progress.
  const auto next = SampleRange(detector, 31'500 * kMs, 36'000 * kMs, 9);
  REQUIRE(Count(next, HangVerdict::kHang) == 0);
  REQUIRE(detector.Observe(At(36'250 * kMs, 9)) == HangVerdict::kHang);
  REQUIRE(detector.Hangs() == 2);
}

TEST_CASE("Periods count from the sample that last saw progress",
          "[watchdog]") {
  // Progress seen at 0.25 s, then none: the hang comes at 5.25 s.
  HangDetector detector({});
  detector.Start(0);
  detector.Arm(1, 0);
  REQUIRE(detector.Observe(At(kSample, 2)) == HangVerdict::kProgress);
  const auto stall = SampleRange(detector, 2 * kSample, 5 * kSecond, 2);
  REQUIRE(Count(stall, HangVerdict::kHang) == 0);
  REQUIRE(detector.Observe(At(5'250 * kMs, 2)) == HangVerdict::kHang);
}

TEST_CASE("Modal, Suspended, the heartbeat and a debugger each prevent a hang",
          "[watchdog]") {
  for (const int excuse : {0, 1, 2, 3}) {
    INFO("excuse " << excuse);
    HangDetector detector({});
    detector.Start(0);
    detector.Arm(1, 0);
    const auto verdicts = SampleRange(detector, kSample, 60 * kSecond, 1,
                                      [excuse](HangSample& s) {
                                        s.suspended = excuse == 0;
                                        s.modal = excuse == 1;
                                        s.heartbeat = excuse == 2;
                                        s.debugger = excuse == 3;
                                      });
    REQUIRE(Count(verdicts, HangVerdict::kExcused) == Size(verdicts));
    REQUIRE(detector.Hangs() == 0);
    REQUIRE(detector.StalledPeriods() == 0);
    // The stall itself is still measured from the last progress.
    REQUIRE(detector.StallNs() == 60 * kSecond);
  }
}

TEST_CASE("An excuse in a stall starts the count over", "[watchdog]") {
  HangDetector detector({});
  detector.Start(0);
  detector.Arm(1, 0);
  const auto first = SampleRange(detector, kSample, 4'750 * kMs, 1);
  REQUIRE(Count(first, HangVerdict::kHang) == 0);
  HangSample modal = At(5 * kSecond, 1);
  modal.modal = true;
  REQUIRE(detector.Observe(modal) == HangVerdict::kExcused);
  // Five more periods from the excuse, not from the last progress.
  const auto second = SampleRange(detector, 5'250 * kMs, 9'750 * kMs, 1);
  REQUIRE(Count(second, HangVerdict::kHang) == 0);
  REQUIRE(detector.Observe(At(10 * kSecond, 1)) == HangVerdict::kHang);
  REQUIRE(detector.StallNs() == 10 * kSecond);
}

TEST_CASE("System sleep triggers nothing: the unbiased clock stands still",
          "[watchdog]") {
  HangDetector detector({});
  detector.Start(0);
  detector.Arm(1, 0);
  REQUIRE(detector.Observe(At(2 * kSecond, 2)) == HangVerdict::kProgress);
  // The machine sleeps for an hour from 2.1 s: the watchdog's samples keep
  // coming (its own timer may fire on resume), but the unbiased clock
  // (QueryUnbiasedInterruptTime, CLOCK_MONOTONIC) did not advance.
  for (int i = 0; i < 14'400; ++i) {
    REQUIRE(detector.Observe(At(2'100 * kMs, 2)) == HangVerdict::kStalled);
  }
  REQUIRE(detector.StalledPeriods() == 0);
  // On resume main runs again within the first awake period.
  REQUIRE(detector.Observe(At(2'400 * kMs, 3)) == HangVerdict::kProgress);
  REQUIRE(detector.Hangs() == 0);
}

TEST_CASE("Time the detector did not watch is not counted towards a hang",
          "[watchdog]") {
  HangDetector detector({});
  detector.Start(0);
  detector.Arm(1, 0);
  // 4 s without progress, watched four times a period.
  const auto first = SampleRange(detector, kSample, 4 * kSecond, 1);
  REQUIRE(Count(first, HangVerdict::kStalled) == Size(first));
  REQUIRE(detector.StalledPeriods() == 4);
  // The whole process is stopped for 20 s (a debugger that detaches while
  // the process stands in it: the next sample no longer sees it).
  REQUIRE(detector.Observe(At(24 * kSecond, 1)) == HangVerdict::kUnobserved);
  REQUIRE(detector.GapNs() == 20 * kSecond);
  REQUIRE(detector.StalledPeriods() == 0);
  REQUIRE(detector.Hangs() == 0);
  // The stall itself is still measured from the last progress.
  REQUIRE(detector.StallNs() == 24 * kSecond);
  // Five watched periods from the end of the gap are a hang.
  const auto second = SampleRange(detector, 24'250 * kMs, 28'750 * kMs, 1);
  REQUIRE(Count(second, HangVerdict::kStalled) == Size(second));
  REQUIRE(detector.Observe(At(29 * kSecond, 1)) == HangVerdict::kHang);
  // A gap inside a declared hang neither ends it nor declares it again.
  REQUIRE(detector.Observe(At(60 * kSecond, 1)) == HangVerdict::kStalled);
  REQUIRE(detector.InHang());
  REQUIRE(detector.Hangs() == 1);
  // Progress seen after a gap is progress.
  REQUIRE(detector.Observe(At(90 * kSecond, 2)) == HangVerdict::kHangEnded);
  REQUIRE(detector.Observe(At(120 * kSecond, 3)) == HangVerdict::kProgress);
}

TEST_CASE("The gap limit is 2 s, or two periods when that is longer",
          "[watchdog]") {
  // Samples exactly 2 s apart are watched: the hang comes with the third.
  HangDetector detector({});
  REQUIRE(detector.MaxSampleGapNs() == 2 * kSecond);
  detector.Start(0);
  detector.Arm(1, 0);
  REQUIRE(detector.Observe(At(2 * kSecond, 1)) == HangVerdict::kStalled);
  REQUIRE(detector.Observe(At(4 * kSecond, 1)) == HangVerdict::kStalled);
  REQUIRE(detector.Observe(At(6 * kSecond, 1)) == HangVerdict::kHang);

  // One nanosecond more is a gap.
  HangDetector gapped({});
  gapped.Start(0);
  gapped.Arm(1, 0);
  REQUIRE(gapped.Observe(At(2 * kSecond + 1, 1)) == HangVerdict::kUnobserved);

  // Long periods widen the limit: two periods of 5 s.
  HangDetector slow({.period_ns = 5 * kSecond});
  REQUIRE(slow.MaxSampleGapNs() == 10 * kSecond);
  slow.Start(0);
  slow.Arm(1, 0);
  REQUIRE(slow.Observe(At(10 * kSecond, 1)) == HangVerdict::kStalled);
  REQUIRE(slow.Observe(At(20 * kSecond + 1, 1)) == HangVerdict::kUnobserved);

  // 0: gaps are not judged.
  HangDetector unjudged({.max_sample_gap_ns = 0});
  REQUIRE(unjudged.MaxSampleGapNs() == 0);
  unjudged.Start(0);
  unjudged.Arm(1, 0);
  REQUIRE(unjudged.Observe(At(60 * kSecond, 1)) == HangVerdict::kHang);
}

TEST_CASE("A hang inside a driver wait is labelled a GPU or compositor stall",
          "[watchdog]") {
  HangDetector detector({});
  detector.Start(0);
  detector.Arm(1, 0);
  const auto verdicts = SampleRange(detector, kSample, 5 * kSecond, 1,
                                    [](HangSample& s) { s.in_driver = true; });
  REQUIRE(Count(verdicts, HangVerdict::kHang) == 1);
  REQUIRE(verdicts.back() == HangVerdict::kHang);
  REQUIRE(detector.HangInDriver());
  // The flag labels the hang; it does not excuse it.
  REQUIRE(Count(verdicts, HangVerdict::kExcused) == 0);
}

TEST_CASE("Declaration can be off; Disarm stops judging", "[watchdog]") {
  HangDetector off({.declare_hangs = false});
  off.Start(0);
  off.Arm(1, 0);
  const auto stalled = SampleRange(off, kSample, 20 * kSecond, 1);
  REQUIRE(Count(stalled, HangVerdict::kStalled) == Size(stalled));
  REQUIRE_FALSE(off.InHang());

  HangDetector disarmed({});
  disarmed.Start(0);
  disarmed.Arm(1, 0);
  disarmed.Disarm();
  const auto verdicts = SampleRange(disarmed, kSample, 20 * kSecond, 1);
  REQUIRE(Count(verdicts, HangVerdict::kOff) == Size(verdicts));
  REQUIRE(disarmed.Hangs() == 0);
}

// --- The watchdog thread ---

// The test dump function: records the reason and the calling thread.
struct Dumps {
  std::mutex mutex;
  std::vector<std::string> reasons;
  std::vector<std::thread::id> threads;
  std::vector<ThreadRole> roles;
  std::atomic<bool> fail{false};
};

Dumps& TestDumps() {
  static auto* const kDumps = new Dumps();
  return *kDumps;
}

std::expected<void, Error> RecordDump(std::string_view reason) {
  Dumps& dumps = TestDumps();
  const std::scoped_lock lock(dumps.mutex);
  dumps.reasons.emplace_back(reason);
  dumps.threads.push_back(std::this_thread::get_id());
  dumps.roles.push_back(CurrentRole());
  if (dumps.fail.load()) {
    return std::unexpected(Error{ErrorCode::kIo, "test failure", "", 0, 0});
  }
  return {};
}

void ResetDumps() {
  Dumps& dumps = TestDumps();
  const std::scoped_lock lock(dumps.mutex);
  dumps.reasons.clear();
  dumps.threads.clear();
  dumps.roles.clear();
  dumps.fail.store(false);
}

std::vector<std::string> DumpReasons() {
  Dumps& dumps = TestDumps();
  const std::scoped_lock lock(dumps.mutex);
  return dumps.reasons;
}

// The fake unbiased clock and debugger of the thread tests.
std::atomic<std::uint64_t> g_fake_now{0};
std::atomic<bool> g_fake_debugger{false};

std::uint64_t FakeNow() { return g_fake_now.load(std::memory_order_acquire); }
bool FakeDebugger() { return g_fake_debugger.load(std::memory_order_acquire); }

template <class F>
bool WaitFor(const F& done) {
  for (int i = 0; i < 400; ++i) {
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return done();
}

// Advances the fake clock by |step| at a time, each step seen by at least
// one of |watchdog|'s samples, until |done| holds or |limit| of fake time
// has passed; returns |done|.
template <class F>
bool AdvanceUntil(const Watchdog& watchdog, std::uint64_t step,
                  std::uint64_t limit, const F& done) {
  const std::uint64_t end = FakeNow() + limit;
  while (FakeNow() < end) {
    if (done()) return true;
    const std::uint64_t samples = watchdog.Stats().samples;
    g_fake_now.fetch_add(step, std::memory_order_acq_rel);
    for (int i = 0; i < 2000 && watchdog.Stats().samples <= samples; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  return done();
}

void Advance(const Watchdog& watchdog, std::uint64_t step,
             std::uint64_t limit) {
  static_cast<void>(AdvanceUntil(watchdog, step, limit, [] { return false; }));
}

bool WatchdogSlotLive() {
  const ThreadRegistry& registry = ThreadRegistry::Instance();
  for (std::uint32_t slot = 0; slot < kThreadSlotCount; ++slot) {
    const ThreadSlotInfo info = registry.Read(slot);
    if (info.state == ThreadSlotState::kLive &&
        info.role == ThreadRole::kWatchdog && info.Name() == "watchdog") {
      return true;
    }
  }
  return false;
}

// Main's flags, as the App keeps them.
struct MainFlags {
  std::atomic<std::uint64_t> iterates{1};
  std::atomic<bool> modal{false};
  std::atomic<bool> suspended{false};
  std::atomic<bool> heartbeat{false};
  std::atomic<bool> in_driver{false};

  [[nodiscard]] WatchdogInputs Inputs() const {
    return {.iterate_counter = &iterates,
            .modal = &modal,
            .suspended = &suspended,
            .heartbeat = &heartbeat,
            .in_driver = &in_driver};
  }
};

// A watchdog on the fake clock, sampling every millisecond of real time.
WatchdogConfig FakeClockConfig(const MainFlags& flags) {
  WatchdogConfig config;
  config.inputs = flags.Inputs();
  config.dump = &RecordDump;
  config.clock = &FakeNow;
  config.debugger = &FakeDebugger;
  config.sample_ms = 1;
  return config;
}

TEST_CASE("The watchdog writes each requested dump on its own thread",
          "[watchdog]") {
  ResetDumps();
  std::atomic<std::uint64_t> iterates{0};
  WatchdogConfig config;
  config.inputs.iterate_counter = &iterates;
  config.dump = &RecordDump;
  auto watchdog = Watchdog::Start(config);
  REQUIRE(watchdog.has_value());
  REQUIRE(watchdog->Running());
  REQUIRE(WaitFor([] { return WatchdogSlotLive(); }));

  REQUIRE(watchdog->RequestDump("first"));
  REQUIRE(WaitFor([&] { return watchdog->Stats().dumps_written == 1; }));
  TestDumps().fail.store(true);
  REQUIRE(watchdog->RequestDump(std::string(500, 'x')));
  REQUIRE(WaitFor([&] { return watchdog->Stats().dumps_failed == 1; }));
  watchdog->Stop();
  REQUIRE_FALSE(watchdog->Running());
  REQUIRE_FALSE(watchdog->RequestDump("after stop"));

  const std::vector<std::string> reasons = DumpReasons();
  REQUIRE(reasons.size() == 2);
  REQUIRE(reasons[0] == "first");
  CHECK(reasons[1] == std::string(kDumpRequestReasonSize, 'x'));
  Dumps& dumps = TestDumps();
  const std::scoped_lock lock(dumps.mutex);
  for (std::size_t i = 0; i < dumps.threads.size(); ++i) {
    CHECK(dumps.threads[i] != std::this_thread::get_id());
    CHECK(dumps.roles[i] == ThreadRole::kWatchdog);
  }
}

TEST_CASE("Stop serves the dump requests still queued", "[watchdog]") {
  ResetDumps();
  WatchdogConfig config;
  config.dump = &RecordDump;
  config.period_ms = 60'000;
  auto watchdog = Watchdog::Start(config);
  REQUIRE(watchdog.has_value());
  for (int i = 0; i < 3; ++i) {
    REQUIRE(watchdog->RequestDump("queued " + std::to_string(i)));
  }
  watchdog->Stop();
  CHECK(DumpReasons() ==
        std::vector<std::string>{"queued 0", "queued 1", "queued 2"});
  REQUIRE(watchdog->Stats().dumps_written == 3);
}

TEST_CASE("A boot over its budget gets one dump; armed, a stall is a hang",
          "[watchdog]") {
  // The real unbiased clock, with a 10 ms period and a 30 ms boot budget.
  ResetDumps();
  std::atomic<std::uint64_t> iterates{0};
  WatchdogConfig config;
  config.inputs.iterate_counter = &iterates;
  config.dump = &RecordDump;
  config.period_ms = 10;
  config.detector.boot_budget_ns = 30 * kMs;
  auto watchdog = Watchdog::Start(config);
  REQUIRE(watchdog.has_value());
  REQUIRE(WaitFor([&] { return watchdog->Stats().boot_over_budget; }));
  REQUIRE(WaitFor([&] { return watchdog->Stats().dumps_written == 1; }));
  watchdog->Arm();
  REQUIRE(WaitFor([&] { return watchdog->Stats().armed; }));
  // Progress: no hang however long it runs.
  const auto busy_until =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
  while (std::chrono::steady_clock::now() < busy_until) {
    iterates.fetch_add(1, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(watchdog->Stats().hangs == 0);
  // No progress: five 10 ms periods make a hang.
  REQUIRE(WaitFor([&] { return watchdog->Stats().hang_dumps_written == 1; }));
  REQUIRE(watchdog->Stats().stalled_periods >= 5);
  watchdog->Stop();
  const std::vector<std::string> reasons = DumpReasons();
  REQUIRE(reasons.size() == 2);
  REQUIRE(reasons[0] == "boot over budget");
  CHECK(reasons[1].starts_with("hang 1: main made no progress for "));
}

TEST_CASE("On a fake unbiased clock the watchdog dumps once per hang episode",
          "[watchdog]") {
  ResetDumps();
  g_fake_now.store(kSecond);
  g_fake_debugger.store(false);
  MainFlags flags;
  auto watchdog = Watchdog::Start(FakeClockConfig(flags));
  REQUIRE(watchdog.has_value());
  watchdog->Arm();
  REQUIRE(WaitFor([&] { return watchdog->Stats().armed; }));
  // 100 ms of fake time per step (a step takes at least a millisecond of
  // real time, and a Windows sleep often 15).
  REQUIRE(AdvanceUntil(*watchdog, 100 * kMs, 30 * kSecond, [&] {
    return watchdog->Stats().hang_dumps_written == 1;
  }));
  WatchdogStats stats = watchdog->Stats();
  REQUIRE(stats.hangs == 1);
  REQUIRE(stats.in_hang);
  CHECK_FALSE(stats.last_hang_in_driver);
  // Ten more fake seconds of the same hang: still one dump.
  Advance(*watchdog, 250 * kMs, 10 * kSecond);
  stats = watchdog->Stats();
  REQUIRE(stats.hangs == 1);
  REQUIRE(stats.dumps_written == 1);
  // Progress ends the episode, with its length.
  flags.iterates.fetch_add(1);
  REQUIRE(AdvanceUntil(*watchdog, kMs, kSecond,
                       [&] { return watchdog->Stats().hangs_ended == 1; }));
  stats = watchdog->Stats();
  REQUIRE_FALSE(stats.in_hang);
  CHECK(stats.last_episode_ns >= 15 * kSecond);
  // A second episode, a second dump.
  REQUIRE(AdvanceUntil(*watchdog, 100 * kMs, 30 * kSecond, [&] {
    return watchdog->Stats().hang_dumps_written == 2;
  }));
  watchdog->Stop();
  const std::vector<std::string> reasons = DumpReasons();
  REQUIRE(reasons.size() == 2);
  CHECK(reasons[0].starts_with("hang 1: main made no progress for 5."));
  CHECK(reasons[1].starts_with("hang 2: main made no progress for 5."));
}

TEST_CASE(
    "Modal, Suspended, the heartbeat and a faked debugger keep the watchdog "
    "from dumping",
    "[watchdog]") {
  for (const int excuse : {0, 1, 2, 3}) {
    INFO("excuse " << excuse);
    ResetDumps();
    g_fake_now.store(kSecond);
    MainFlags flags;
    flags.suspended.store(excuse == 0);
    flags.modal.store(excuse == 1);
    flags.heartbeat.store(excuse == 2);
    g_fake_debugger.store(excuse == 3);
    auto watchdog = Watchdog::Start(FakeClockConfig(flags));
    REQUIRE(watchdog.has_value());
    watchdog->Arm();
    REQUIRE(WaitFor([&] { return watchdog->Stats().armed; }));
    // 20 fake seconds without progress.
    Advance(*watchdog, 250 * kMs, 20 * kSecond);
    const WatchdogStats stats = watchdog->Stats();
    REQUIRE(stats.samples >= 80);
    REQUIRE(stats.hangs == 0);
    REQUIRE(stats.stalled_periods == 0);
    watchdog->Stop();
    CHECK(DumpReasons().empty());
  }
  g_fake_debugger.store(false);
}

TEST_CASE(
    "A slow boot under 30 s and system sleep make the watchdog dump nothing",
    "[watchdog]") {
  ResetDumps();
  g_fake_now.store(kSecond);
  g_fake_debugger.store(false);
  MainFlags flags;
  WatchdogConfig config = FakeClockConfig(flags);
  config.boot_start_ns = kSecond;
  auto watchdog = Watchdog::Start(config);
  REQUIRE(watchdog.has_value());
  // A 29.5 s boot.
  Advance(*watchdog, 500 * kMs, 29'500 * kMs);
  CHECK_FALSE(watchdog->Stats().boot_over_budget);
  watchdog->Arm();
  REQUIRE(WaitFor([&] { return watchdog->Stats().armed; }));
  // Main runs, then the machine sleeps: real time passes, the unbiased
  // clock does not.
  for (int i = 0; i < 10; ++i) {
    flags.iterates.fetch_add(1);
    Advance(*watchdog, 100 * kMs, 200 * kMs);
  }
  const std::uint64_t samples_before = watchdog->Stats().samples;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  // On resume main runs again.
  flags.iterates.fetch_add(1);
  Advance(*watchdog, 10 * kMs, 500 * kMs);
  const WatchdogStats stats = watchdog->Stats();
  CHECK(stats.samples > samples_before + 5);
  REQUIRE(stats.hangs == 0);
  watchdog->Stop();
  CHECK(DumpReasons().empty());
}

TEST_CASE("A stopped process is not a hang: the watchdog counts on from it",
          "[watchdog]") {
  ResetDumps();
  g_fake_now.store(kSecond);
  g_fake_debugger.store(false);
  MainFlags flags;
  auto watchdog = Watchdog::Start(FakeClockConfig(flags));
  REQUIRE(watchdog.has_value());
  watchdog->Arm();
  REQUIRE(WaitFor([&] { return watchdog->Stats().armed; }));
  // 4 fake seconds without progress, each quarter second sampled.
  Advance(*watchdog, 250 * kMs, 4 * kSecond);
  REQUIRE(watchdog->Stats().hangs == 0);
  REQUIRE(watchdog->Stats().unobserved_gaps == 0);
  // The process is stopped for 20 s: the clock ran, the watchdog did not
  // (and no debugger is seen once it runs again). The sample that sees the
  // jump may not be the next one counted: one may be under way.
  g_fake_now.fetch_add(20 * kSecond, std::memory_order_acq_rel);
  REQUIRE(WaitFor([&] { return watchdog->Stats().unobserved_gaps == 1; }));
  WatchdogStats stats = watchdog->Stats();
  REQUIRE(stats.hangs == 0);
  // Main is still stuck: five watched periods later that is a hang, 29 s
  // after main's last progress.
  REQUIRE(AdvanceUntil(*watchdog, 100 * kMs, 30 * kSecond, [&] {
    return watchdog->Stats().hang_dumps_written == 1;
  }));
  stats = watchdog->Stats();
  REQUIRE(stats.hangs == 1);
  REQUIRE(stats.unobserved_gaps == 1);
  watchdog->Stop();
  const std::vector<std::string> reasons = DumpReasons();
  REQUIRE(reasons.size() == 1);
  CHECK(reasons[0].starts_with("hang 1: main made no progress for 29."));
}

TEST_CASE("A hang in a driver wait is dumped as a GPU or compositor stall",
          "[watchdog]") {
  ResetDumps();
  g_fake_now.store(kSecond);
  g_fake_debugger.store(false);
  MainFlags flags;
  flags.in_driver.store(true);
  auto watchdog = Watchdog::Start(FakeClockConfig(flags));
  REQUIRE(watchdog.has_value());
  watchdog->Arm();
  REQUIRE(WaitFor([&] { return watchdog->Stats().armed; }));
  REQUIRE(AdvanceUntil(*watchdog, 100 * kMs, 30 * kSecond, [&] {
    return watchdog->Stats().hang_dumps_written == 1;
  }));
  CHECK(watchdog->Stats().last_hang_in_driver);
  watchdog->Stop();
  const std::vector<std::string> reasons = DumpReasons();
  REQUIRE(reasons.size() == 1);
  CHECK(reasons[0].starts_with(
      "hang 1: GPU or compositor stall, main in an SDL_GPU or pacer wait "
      "for 5."));
}

TEST_CASE("After the shutdown deadline starts no hang is judged",
          "[watchdog]") {
  ResetDumps();
  g_fake_now.store(kSecond);
  g_fake_debugger.store(false);
  MainFlags flags;
  WatchdogConfig config = FakeClockConfig(flags);
  config.shutdown_deadline_ns = 3600 * kSecond;
  config.terminate_on_deadline = false;
  auto watchdog = Watchdog::Start(config);
  REQUIRE(watchdog.has_value());
  watchdog->Arm();
  REQUIRE(WaitFor([&] { return watchdog->Stats().armed; }));
  watchdog->StartShutdownDeadline();
  Advance(*watchdog, 250 * kMs, 20 * kSecond);
  REQUIRE(watchdog->Stats().hangs == 0);
  CHECK_FALSE(watchdog->Stats().deadline_expired);
  watchdog->Stop();
  CHECK(DumpReasons().empty());
}

TEST_CASE("The shutdown deadline dumps once when it expires", "[watchdog]") {
  ResetDumps();
  WatchdogConfig config;
  config.dump = &RecordDump;
  config.shutdown_deadline_ns = 50'000'000;
  config.terminate_on_deadline = false;
  auto watchdog = Watchdog::Start(config);
  REQUIRE(watchdog.has_value());
  CHECK_FALSE(watchdog->Stats().deadline_started);
  const auto started = std::chrono::steady_clock::now();
  watchdog->StartShutdownDeadline();
  REQUIRE(watchdog->Stats().deadline_started);
  REQUIRE(WaitFor([&] { return watchdog->Stats().deadline_expired; }));
  const auto elapsed = std::chrono::steady_clock::now() - started;
  // The deadline is not rounded up to the sample interval.
  CHECK(elapsed >= std::chrono::milliseconds(40));
  CHECK(elapsed < std::chrono::milliseconds(900));
  REQUIRE(WaitFor([&] { return watchdog->Stats().dumps_written == 1; }));
  watchdog->StartShutdownDeadline();
  watchdog->Stop();
  CHECK(DumpReasons() == std::vector<std::string>{"shutdown deadline"});
}

// --- Stream withdrawal (UnregisterStream) ---

TEST_CASE("A withdrawn stream type can be registered again", "[watchdog]") {
  static std::array<std::byte, 64> first{};
  static std::array<std::byte, 64> second{};
  REQUIRE(
      RegisterStream(StreamType::kPlayStartBytes, first.data(), first.size()));
  // One region per type at a time.
  REQUIRE_FALSE(RegisterStream(StreamType::kPlayStartBytes, second.data(),
                               second.size()));
  REQUIRE(UnregisterStream(StreamType::kPlayStartBytes));
  REQUIRE_FALSE(UnregisterStream(StreamType::kPlayStartBytes));
  REQUIRE(RegisterStream(StreamType::kPlayStartBytes, second.data(),
                         second.size()));
  REQUIRE(UnregisterStream(StreamType::kPlayStartBytes));
  // Outside the first-party range.
  REQUIRE_FALSE(UnregisterStream(static_cast<StreamType>(7)));
}

}  // namespace
}  // namespace se::crash
