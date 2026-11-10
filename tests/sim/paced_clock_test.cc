// Copyright 2026 The simple-engine-2 Authors.
//
// PacedClock (docs/milestone-specs.md M17a): the deadline grid, the spin
// tail and its margin from the timer overshoot's p99, the phi slew bound,
// catch-up bursts and dilation, time scale and the epoch, all on a
// ManualClock; and with the real clock ([slow]) the wake error is recorded
// (<build>/sim_out/paced_clock_real.txt) and no wake comes before its
// deadline.

#include "se_sim/paced_clock.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace se {
namespace {

constexpr std::uint64_t kT0 = 5'000'000'000;
constexpr std::uint64_t kMs = 1'000'000;
constexpr std::uint64_t kUs = 1'000;

TEST_CASE("PacedClock deadlines: rate ticks take exactly one second",
          "[paced_clock]") {
  ManualClock manual;
  for (const std::uint32_t rate : {120U, 144U, 160U}) {
    INFO("rate " << rate);
    PacedClockConfig config;
    config.tick_rate = rate;
    PacedClock clock(manual, config);
    clock.Start(1, kT0);
    REQUIRE(clock.DeadlineNs(1) == kT0);
    REQUIRE(clock.DeadlineNs(1 + rate) == kT0 + 1'000'000'000);
    const Tick minute = static_cast<Tick>(rate) * 60;
    REQUIRE(clock.DeadlineNs(1 + minute) == kT0 + 60'000'000'000);
    const std::uint64_t floor_period = 1'000'000'000 / rate;
    for (Tick k = 2; k <= static_cast<Tick>(rate) * 10; ++k) {
      const std::uint64_t d = clock.DeadlineNs(k) - clock.DeadlineNs(k - 1);
      REQUIRE((d == floor_period || d == floor_period + 1));
    }
  }
}

TEST_CASE("PacedClock never wakes before the deadline and spins the margin",
          "[paced_clock]") {
  ManualClock manual(ManualClock::Mode::kAutoAdvance, kT0 - 20 * kMs);
  // The timer wakes up to 0.25 ms early and up to 0.2 ms late.
  manual.SetOvershoots({-250'000, 0, 40'000, 200'000, -10'000, 120'000});
  manual.SetRelaxStep(1 * kUs);
  PacedClock clock(manual, PacedClockConfig{});
  clock.Start(1, kT0);
  for (Tick k = 1; k <= 1000; ++k) {
    const std::uint64_t deadline = clock.DeadlineNs(k);
    const std::uint64_t woke = clock.WaitForDeadline(k, std::stop_token{});
    REQUIRE(woke >= deadline);
    // Every overshoot is inside the 0.3 ms floor: the spin ends within one
    // relax step of the deadline.
    REQUIRE(woke - deadline < 1 * kUs);
  }
  const PacedClockStats& stats = clock.Stats();
  REQUIRE(stats.waits == 1000);
  REQUIRE(stats.timer_sleeps == 1000);
  REQUIRE(stats.late_waits == 0);
  REQUIRE(stats.spin_iterations > 0);
  REQUIRE(stats.max_overshoot_ns == 200'000);
  REQUIRE(clock.MarginNs() == 300'000);
}

TEST_CASE("PacedClock's margin follows the timer overshoot's p99",
          "[paced_clock]") {
  ManualClock manual(ManualClock::Mode::kAutoAdvance, kT0 - 20 * kMs);
  // 1 in 20 timer waits overshoots by 1.2 ms, the rest by 50 us.
  std::vector<std::int64_t> overshoots(20, 50'000);
  overshoots[7] = 1'200'000;
  manual.SetOvershoots(overshoots);
  manual.SetRelaxStep(1 * kUs);
  PacedClock clock(manual, PacedClockConfig{});
  clock.Start(1, kT0);
  REQUIRE(clock.MarginNs() == 300'000);
  std::uint64_t late = 0;
  Tick k = 1;
  for (; k <= 512; ++k) {
    const std::uint64_t woke = clock.WaitForDeadline(k, std::stop_token{});
    if (woke - clock.DeadlineNs(k) > 1 * kUs) ++late;
  }
  // The p99 of the last 256 overshoots is the 1.2 ms one, so the timer now
  // wakes 1.2 ms early and the spin covers it: no late wake from here on.
  REQUIRE(clock.OvershootP99Ns() == 1'200'000);
  REQUIRE(clock.MarginNs() == 1'200'000);
  REQUIRE(late >= 1);
  const std::uint64_t late_before = late;
  for (; k <= 1024; ++k) {
    const std::uint64_t woke = clock.WaitForDeadline(k, std::stop_token{});
    REQUIRE(woke >= clock.DeadlineNs(k));
    if (woke - clock.DeadlineNs(k) > 1 * kUs) ++late;
  }
  CHECK(late == late_before);
  CHECK(clock.WakeErrorP99Ns() < 1 * kUs);
}

TEST_CASE("PacedClock caps the spin margin at min(2 ms, P / 4)",
          "[paced_clock]") {
  ManualClock manual(ManualClock::Mode::kAutoAdvance, kT0 - 20 * kMs);
  // Three 4 ms timer overshoots in every 64 waits: the p99 is 4 ms, which
  // uncapped would spin 4 ms of each 8.33 ms period.
  std::vector<std::int64_t> overshoots(64, 0);
  overshoots[10] = 4'000'000;
  overshoots[20] = 4'000'000;
  overshoots[30] = 4'000'000;
  manual.SetOvershoots(overshoots);
  manual.SetRelaxStep(1 * kUs);
  PacedClock clock(manual, PacedClockConfig{});
  clock.Start(1, kT0);
  REQUIRE(clock.MarginCapNs() == 2 * kMs);
  Tick k = 1;
  for (; k <= 512; ++k) {
    const std::uint64_t woke = clock.WaitForDeadline(k, std::stop_token{});
    REQUIRE(woke >= clock.DeadlineNs(k));
  }
  REQUIRE(clock.OvershootP99Ns() == 4 * kMs);
  REQUIRE(clock.MarginNs() == 2 * kMs);
  REQUIRE(clock.Stats().capped_waits > 0);
  // The spin of a wait whose timer is on time is at most the cap.
  const std::uint64_t spins_before = clock.Stats().spin_iterations;
  std::uint64_t on_time = 0;
  for (; k <= 1024; ++k) {
    const std::uint64_t woke = clock.WaitForDeadline(k, std::stop_token{});
    REQUIRE(woke >= clock.DeadlineNs(k));
    if (woke - clock.DeadlineNs(k) < 1 * kUs) ++on_time;
  }
  const std::uint64_t spins = clock.Stats().spin_iterations - spins_before;
  REQUIRE(on_time >= 400);
  // 512 waits of at most 2 ms (2000 relax steps) each.
  CHECK(spins <= std::uint64_t{512} * 2000);
  CHECK(spins >= std::uint64_t{400} * 1900);
  // A short period caps lower: P / 4 at 4x speed.
  clock.SetTimeScale(4.0, k);
  REQUIRE(clock.PeriodNs() == 2'083'333);
  CHECK(clock.MarginCapNs() == 2'083'333 / 4);
  CHECK(clock.MarginNs() == 2'083'333 / 4);
}

TEST_CASE("PacedClock: a wait that starts late does not sleep",
          "[paced_clock]") {
  ManualClock manual(ManualClock::Mode::kAutoAdvance, kT0 + 3 * kMs);
  PacedClock clock(manual, PacedClockConfig{});
  clock.Start(1, kT0);
  const std::uint64_t woke = clock.WaitForDeadline(1, std::stop_token{});
  CHECK(woke == kT0 + 3 * kMs);
  REQUIRE(clock.Stats().late_waits == 1);
  REQUIRE(clock.Stats().timer_sleeps == 0);
  REQUIRE(manual.Sleeps() == 0);
}

TEST_CASE("PacedClock's phi slews by at most 0.1 ms per tick",
          "[paced_clock]") {
  ManualClock manual;
  PacedClock clock(manual, PacedClockConfig{});
  clock.Start(1, kT0);
  const std::uint64_t period = clock.PeriodNs();

  // Not a whole multiple of dt: the target is ignored.
  clock.SetPhaseTarget(2'000'000, false);
  for (int i = 0; i < 50; ++i) clock.OnTickRun();
  REQUIRE(clock.Phi() == 0);

  clock.SetPhaseTarget(2'050'000, true);
  std::uint64_t previous = clock.DeadlineNs(1);
  int ticks = 0;
  for (Tick k = 1; clock.Phi() != 2'050'000; ++k) {
    const std::int64_t before = clock.Phi();
    clock.OnTickRun();
    ++ticks;
    REQUIRE(clock.Phi() - before <= 100'000);
    REQUIRE(clock.Phi() - before > 0);
    const std::uint64_t next = clock.DeadlineNs(k + 1);
    // One period plus at most one slew step.
    REQUIRE(next - previous >= period - 1);
    REQUIRE(next - previous <= period + 1 + 100'000);
    previous = next;
    REQUIRE(ticks <= 21);
  }
  REQUIRE(ticks == 21);

  // Back down, negative: the same bound.
  clock.SetPhaseTarget(-300'000, true);
  for (int i = 0; i < 30; ++i) {
    const std::int64_t before = clock.Phi();
    clock.OnTickRun();
    REQUIRE(before - clock.Phi() <= 100'000);
  }
  REQUIRE(clock.Phi() == -300'000);
}

TEST_CASE("PacedClock bursts catch up at most 8 ticks; 250 ms dilates",
          "[paced_clock]") {
  ManualClock manual;
  PacedClock clock(manual, PacedClockConfig{});
  clock.Start(1, kT0);
  const std::uint64_t period = clock.PeriodNs();

  TickBurst none = clock.DueTicks(1, kT0 - 1);
  REQUIRE(none.count == 0);

  TickBurst burst = clock.DueTicks(1, clock.DeadlineNs(1) + 5 * period + 10);
  REQUIRE(burst.first == 1);
  REQUIRE(burst.count == 6);
  CHECK_FALSE(burst.rebased);

  // 20 ticks behind (167 ms): 8, then 8, then the rest.
  const std::uint64_t now = clock.DeadlineNs(1) + 20 * period + 10;
  burst = clock.DueTicks(1, now);
  REQUIRE(burst.count == 8);
  burst = clock.DueTicks(9, now);
  REQUIRE(burst.count == 8);
  burst = clock.DueTicks(17, now);
  REQUIRE(burst.count == 5);
  REQUIRE(burst.Last() == 21);
  REQUIRE(clock.DueTicks(22, now).count == 0);

  // 300 ms behind: no chase, the next tick is due now on a new grid.
  const std::uint32_t epoch = clock.Epoch();
  const std::uint64_t stalled = clock.DeadlineNs(22) + 300 * kMs;
  burst = clock.DueTicks(22, stalled);
  CHECK(burst.rebased);
  REQUIRE(burst.count == 1);
  CHECK(burst.dilated_ns == 300 * kMs);
  CHECK(clock.Epoch() == epoch + 1);
  CHECK(clock.DeadlineNs(22) == stalled);
  REQUIRE(clock.Stats().dilations == 1);
  CHECK(clock.Stats().dilated_ns == 300 * kMs);
}

TEST_CASE("PacedClock time scale changes the wall time per tick, not dt",
          "[paced_clock]") {
  ManualClock manual;
  PacedClock clock(manual, PacedClockConfig{});
  clock.Start(1, kT0);
  const std::uint64_t period = clock.PeriodNs();
  const std::uint64_t at10 = clock.DeadlineNs(10);
  const std::uint32_t epoch = clock.Epoch();
  clock.SetTimeScale(0.5, 10);
  // Tick 10 keeps its deadline; the grid is continuous: same epoch.
  CHECK(clock.DeadlineNs(10) == at10);
  CHECK(clock.Epoch() == epoch);
  CHECK(clock.DeadlineNs(11) - at10 == 2 * period + 1);
  REQUIRE(clock.PeriodNs() == 16'666'667);
  clock.SetTimeScale(4.0, 12);
  REQUIRE(clock.DeadlineNs(13) - clock.DeadlineNs(12) == 2'083'333);
}

TEST_CASE("PacedClock on a blocking ManualClock waits for the time to move",
          "[paced_clock][threads]") {
  ManualClock manual(ManualClock::Mode::kBlocking, kT0 - 4 * kMs);
  PacedClock clock(manual, PacedClockConfig{});
  clock.Start(1, kT0);
  std::atomic<bool> woke{false};
  std::atomic<std::uint64_t> woke_at{0};
  std::thread waiter([&] {
    woke_at.store(clock.WaitForDeadline(1, std::stop_token{}));
    woke.store(true);
  });
  // Up to 1 ns before the deadline: still waiting, whatever the timing.
  for (int i = 0; i < 39; ++i) {
    manual.Advance(100 * kUs);
    std::this_thread::yield();
  }
  manual.Advance(100 * kUs - 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  CHECK_FALSE(woke.load());
  manual.Advance(1);
  waiter.join();
  CHECK(woke.load());
  CHECK(woke_at.load() == kT0);
}

TEST_CASE("PacedClock on the real clock: wake error recorded",
          "[paced_clock][slow]") {
  std::unique_ptr<WaitClock> system = MakeSystemWaitClock();
  PacedClock clock(*system, PacedClockConfig{});
  clock.Start(1, system->NowNs() + 2 * kMs);
  constexpr Tick kTicks = 240;
  std::vector<std::uint64_t> errors;
  errors.reserve(kTicks);
  for (Tick k = 1; k <= kTicks; ++k) {
    const std::uint64_t deadline = clock.DeadlineNs(k);
    const std::uint64_t woke = clock.WaitForDeadline(k, std::stop_token{});
    // The spin guarantees this whatever the timer does.
    REQUIRE(woke >= deadline);
    errors.push_back(woke - deadline);
  }
  std::sort(errors.begin(), errors.end());
  const std::uint64_t p50 = errors[errors.size() / 2];
  const std::uint64_t p99 = errors[(errors.size() * 99 + 99) / 100 - 1];
  const std::uint64_t max = errors.back();
  const PacedClockStats& stats = clock.Stats();
  const std::string line = std::format(
      "paced_clock real: {} ticks at 120 Hz, wake error p50 {:.1f} us p99 "
      "{:.1f} us max {:.1f} us; timer overshoot p99 {:.1f} us max {:.1f} us; "
      "margin {:.1f} us; late waits {}\n",
      kTicks, static_cast<double>(p50) / 1e3, static_cast<double>(p99) / 1e3,
      static_cast<double>(max) / 1e3,
      static_cast<double>(clock.OvershootP99Ns()) / 1e3,
      static_cast<double>(stats.max_overshoot_ns) / 1e3,
      static_cast<double>(clock.MarginNs()) / 1e3, stats.late_waits);
  std::fputs(line.c_str(), stdout);
  std::ofstream(std::string(SE_TEST_OUT_DIR) + "/paced_clock_real.txt") << line;
  // Recorded, not gated: the owner may be using the machine.
  SUCCEED(line);
}

}  // namespace
}  // namespace se
