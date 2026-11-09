// Copyright 2026 The simple-engine-2 Authors.
//
// The watchdog skeleton (docs/milestone-specs.md, "Integration (M4a + M4b +
// M5)"): the pure HangDetector, and the watchdog thread with a test dump
// function in place of DumpWithoutCrash.

#include "se_crash/watchdog.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
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
#include "se_crash/hang_detector.h"
#include "se_os/thread.h"

namespace se::crash {
namespace {

constexpr std::uint64_t kSecond = 1'000'000'000;

HangSample At(std::uint64_t now_ns, std::uint64_t iterates) {
  HangSample sample;
  sample.now_ns = now_ns;
  sample.iterates = iterates;
  return sample;
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

TEST_CASE("Armed, only unexcused periods without progress are stalls",
          "[watchdog]") {
  HangDetector detector({});
  detector.Start(0);
  detector.Arm(5);
  REQUIRE(detector.Armed());
  // The boot budget no longer applies.
  REQUIRE(detector.Observe(At(100 * kSecond, 6)) == HangVerdict::kProgress);
  REQUIRE(detector.Observe(At(101 * kSecond, 6)) == HangVerdict::kStalled);
  REQUIRE(detector.Observe(At(102 * kSecond, 6)) == HangVerdict::kStalled);
  REQUIRE(detector.StalledPeriods() == 2);
  for (const int excuse : {0, 1, 2, 3}) {
    HangSample sample = At(103 * kSecond, 6);
    sample.suspended = excuse == 0;
    sample.modal = excuse == 1;
    sample.heartbeat = excuse == 2;
    sample.debugger = excuse == 3;
    REQUIRE(detector.Observe(sample) == HangVerdict::kExcused);
    REQUIRE(detector.StalledPeriods() == 0);
  }
  REQUIRE(detector.Observe(At(104 * kSecond, 7)) == HangVerdict::kProgress);
}

TEST_CASE("Hangs are not declared until M12 turns declaration on",
          "[watchdog]") {
  HangDetector off({});
  off.Start(0);
  off.Arm(1);
  for (int period = 0; period < 20; ++period) {
    REQUIRE(off.Observe(At(kSecond * period, 1)) == HangVerdict::kStalled);
  }
  REQUIRE_FALSE(off.InHang());

  HangDetector on({.declare_hangs = true});
  on.Start(0);
  on.Arm(1);
  for (int period = 1; period <= 4; ++period) {
    REQUIRE(on.Observe(At(kSecond * period, 1)) == HangVerdict::kStalled);
  }
  // The fifth period declares the hang, once per episode.
  REQUIRE(on.Observe(At(5 * kSecond, 1)) == HangVerdict::kHang);
  REQUIRE(on.InHang());
  REQUIRE(on.Observe(At(6 * kSecond, 1)) == HangVerdict::kStalled);
  // Progress ends the episode.
  REQUIRE(on.Observe(At(7 * kSecond, 2)) == HangVerdict::kProgress);
  REQUIRE_FALSE(on.InHang());
  for (int period = 8; period <= 11; ++period) {
    REQUIRE(on.Observe(At(kSecond * period, 2)) == HangVerdict::kStalled);
  }
  REQUIRE(on.Observe(At(12 * kSecond, 2)) == HangVerdict::kHang);
}

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

template <class F>
bool WaitFor(const F& done) {
  for (int i = 0; i < 400; ++i) {
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return done();
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

TEST_CASE("A boot over its budget gets one dump; Arm ends the budget",
          "[watchdog]") {
  ResetDumps();
  std::atomic<std::uint64_t> iterates{0};
  WatchdogConfig config;
  config.inputs.iterate_counter = &iterates;
  config.dump = &RecordDump;
  config.period_ms = 10;
  config.detector.boot_budget_ns = 30'000'000;
  auto watchdog = Watchdog::Start(config);
  REQUIRE(watchdog.has_value());
  REQUIRE(WaitFor([&] { return watchdog->Stats().boot_over_budget; }));
  REQUIRE(WaitFor([&] { return watchdog->Stats().dumps_written == 1; }));
  watchdog->Arm();
  REQUIRE(WaitFor([&] { return watchdog->Stats().armed; }));
  // Armed and no progress: stalls are counted, no hang is declared.
  REQUIRE(WaitFor([&] { return watchdog->Stats().stalled_periods >= 3; }));
  watchdog->Stop();
  CHECK(DumpReasons() == std::vector<std::string>{"boot over budget"});
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
  // The deadline is not rounded up to the 1 s period.
  CHECK(elapsed >= std::chrono::milliseconds(40));
  CHECK(elapsed < std::chrono::milliseconds(900));
  REQUIRE(WaitFor([&] { return watchdog->Stats().dumps_written == 1; }));
  watchdog->StartShutdownDeadline();
  watchdog->Stop();
  CHECK(DumpReasons() == std::vector<std::string>{"shutdown deadline"});
}

}  // namespace
}  // namespace se::crash
