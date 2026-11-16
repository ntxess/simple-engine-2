// Copyright 2026 The simple-engine-2 Authors.
//
// The FramePacer shell on a fake clock and a fake present port
// (tests/support/fake_present_port.h): the mode per caps and sampler, the
// sleep to the vblank minus B, misses from statistics reported to the
// stepper, a late frame draining, nothing while Modal, the N-L fence, VRR
// deadlines, HalfRate's re-present in Standard, adaptive latency, the
// headless and synthetic clocks, and the M6b command-line options.

#include "se_runtime/frame_pacer.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/error.h"
#include "se_core/time.h"
#include "se_platform/driver_wait.h"
#include "se_platform/vblank_sampler.h"
#include "se_runtime/app_config.h"
#include "se_runtime/pacer_policy.h"
#include "se_runtime/present_port.h"
#include "support/fake_present_port.h"
#include "support/test_roles.h"

namespace se {
namespace {

using testing::FakePacerClock;
using testing::FakePresentPort;

constexpr double kPeriod165 = 1e9 / 165.003;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

PresentCaps Patched() {
  PresentCaps caps;
  caps.patched = true;
  caps.frame_latency_waitable = true;
  caps.present_stats = true;
  caps.tearing = true;
  return caps;
}

// A patched SDL presenting a window DWM composes: the waitable, but no
// present statistics arrive (ADR-052, ADR-066).
PresentCaps Windowed() {
  PresentCaps caps = Patched();
  caps.present_stats = false;
  return caps;
}

// What the unpatched SDL has: IMMEDIATE only.
PresentCaps Unpatched() {
  PresentCaps caps;
  caps.tearing = true;
  return caps;
}

FramePacerConfig WindowsConfig() {
  FramePacerConfig config;
  config.paced_by_default = true;
  return config;
}

struct Frame {
  FrameClock clock;
  std::uint64_t begin_ns = 0;
  double budget = 0.0;
  PacerFrame pacer;
};

class Loop {
 public:
  explicit Loop(PresentCaps caps, FramePacerConfig config = WindowsConfig(),
                double period_ns = kPeriod165)
      : port(&clock, period_ns), pacer(config, &clock), period_ns_(period_ns) {
    port.caps = caps;
    costs.tick_hi = 0.4e-3;
    costs.non_tick_hi = 0.6e-3;
    costs.tick_95 = 0.4e-3;
    costs.non_tick_95 = 0.6e-3;
    costs.gpu_hi = kNaN;
    costs.gpu_95 = kNaN;
  }

  void Attach(double display_hz = 0.0) SE_REQUIRES(g_main) {
    pacer.SetDisplayRefresh(display_hz > 0.0 ? display_hz : 1e9 / period_ns_);
    pacer.Attach(&port, [this](std::uint64_t now) {
      return sampler_valid ? port.Reading(now, sampler_scale) : VblankReading{};
    });
  }

  // One iterate: BeginFrame, the plan, |work_ns| of CPU work, the present,
  // EndFrame.
  Frame Step(std::uint64_t work_ns = 1'000'000, bool modal = false,
             bool present = true) SE_REQUIRES(g_main) {
    Frame frame;
    frame.begin_ns = clock.now_ns;
    frame.clock = pacer.BeginFrame(modal, stepper);
    static_cast<void>(stepper.Plan(frame.clock, costs, true, false, false));
    clock.Advance(work_ns);
    if (present) port.Present();
    pacer.OnFrameSubmitted(present, !present);
    const StepPlan next = stepper.PlanNext(frame.clock, costs);
    frame.budget = next.budget_b;
    pacer.EndFrame(next, costs, modal);
    frame.pacer = pacer.LastFrame();
    frames.push_back(frame);
    return frame;
  }

  // Steps for |sec| of virtual time.
  void Run(double sec, std::uint64_t work_ns = 1'000'000) SE_REQUIRES(g_main) {
    const std::uint64_t end =
        clock.now_ns + static_cast<std::uint64_t>(sec * 1e9);
    while (clock.now_ns < end) Step(work_ns);
  }

  FakePacerClock clock;
  FakePresentPort port;
  FixedStepper stepper{120};
  FrameCosts costs;
  FramePacer pacer;
  bool sampler_valid = true;
  double sampler_scale = 1.0;
  std::vector<Frame> frames;

 private:
  double period_ns_;
};

bool OnGrid(std::uint64_t time_ns, std::uint64_t anchor_ns, double period_ns) {
  const double k = static_cast<double>(time_ns - anchor_ns) / period_ns;
  return std::abs(k - std::round(k)) * period_ns < 2.0;
}

TEST_CASE("Unpatched: Standard, the swapchain wait, frames on the grid",
          "[pacer]") {
  const testing::MainThreadScope main;
  Loop loop(Unpatched());
  const std::uint64_t anchor = loop.clock.now_ns;
  loop.Attach();
  CHECK(loop.pacer.Mode() == PacingMode::kStandard);
  CHECK(loop.pacer.ModeReason().find("waitable") != std::string_view::npos);
  for (int i = 0; i < 20; ++i) loop.Step();
  REQUIRE(loop.clock.sleep_count == 0);
  REQUIRE(loop.port.swapchain_waits == 20);
  REQUIRE(loop.port.waitable_waits == 0);
  CHECK(loop.port.latencies.empty());
  REQUIRE(loop.port.submitted == 20);
  REQUIRE(loop.pacer.Totals().resolved_presents == 0);
  for (std::size_t i = 1; i < loop.frames.size(); ++i) {
    const FrameClock& clock = loop.frames[i].clock;
    CHECK(clock.vblanks_known);
    REQUIRE(clock.elapsed_vblanks == 1);
    CHECK(OnGrid(clock.frame_start_ns, anchor, kPeriod165));
    CHECK(loop.frames[i].pacer.period_source == PeriodSource::kSampler);
  }
  CHECK(loop.pacer.Summary().starts_with("Standard 165.00 Hz x1 L1"));
}

TEST_CASE("Unpatched without a valid sampler: the wall clock", "[pacer]") {
  const testing::MainThreadScope main;
  Loop loop(Unpatched());
  loop.sampler_valid = false;
  loop.Attach();
  for (int i = 0; i < 5; ++i) loop.Step();
  CHECK_FALSE(loop.frames.back().clock.vblanks_known);
  CHECK(loop.frames.back().pacer.period_source == PeriodSource::kDisplayMode);
  CHECK(std::abs(loop.frames.back().clock.period_sec - kPeriod165 * 1e-9) <
        1e-9);
}

TEST_CASE("Patched with an agreeing sampler: Paced sleeps to vblank - B",
          "[pacer]") {
  const testing::MainThreadScope main;
  Loop loop(Patched());
  loop.Attach();
  REQUIRE(loop.pacer.Mode() == PacingMode::kPaced);
  REQUIRE(loop.port.latencies == std::vector<int>{1});
  for (int i = 0; i < 60; ++i) loop.Step();
  const PacerTotals& totals = loop.pacer.Totals();
  REQUIRE(totals.missed_presents == 0);
  REQUIRE(totals.resolved_presents >= 55);
  REQUIRE(totals.sleeps >= 58);
  REQUIRE(loop.port.waitable_waits == 60);
  // Each frame woke at its target vblank minus the previous plan's B, and
  // started one interval before that target.
  for (std::size_t i = 2; i < loop.frames.size(); ++i) {
    const Frame& frame = loop.frames[i];
    const double lead_ns =
        static_cast<double>(frame.begin_ns - frame.clock.frame_start_ns);
    const double expected = kPeriod165 - loop.frames[i - 1].budget * 1e9;
    CHECK(std::abs(lead_ns - expected) < 2.0);
    CHECK(frame.clock.vblanks_known);
    REQUIRE(frame.clock.elapsed_vblanks == 1);
    CHECK(frame.pacer.target_ns - frame.clock.frame_start_ns ==
          static_cast<std::uint64_t>(std::llround(kPeriod165)));
  }
  // Every present showed at its target.
  for (std::uint64_t id = 2; id < loop.frames.size(); ++id) {
    const std::uint64_t target = loop.frames[id - 1].pacer.target_ns;
    CHECK(loop.port.ShownRefresh(id) ==
          loop.port.RefreshAt(target) + FakePresentPort::kRefreshBase);
  }
}

TEST_CASE("A late frame drains within 2 frames", "[pacer]") {
  const testing::MainThreadScope main;
  Loop loop(Patched());
  loop.Attach();
  for (int i = 0; i < 30; ++i) loop.Step();
  REQUIRE(loop.pacer.Totals().missed_presents == 0);
  loop.Step(30'000'000);
  const std::size_t late = loop.frames.size();
  for (int i = 0; i < 30; ++i) loop.Step();
  // The late present is a miss; no more than 2 frames in a row report one,
  // and the presents after it are on time.
  REQUIRE(loop.pacer.Totals().missed_presents >= 1);
  int run = 0;
  int longest = 0;
  for (std::size_t i = late; i < loop.frames.size(); ++i) {
    run = loop.frames[i].pacer.missed > 0 ? run + 1 : 0;
    longest = std::max(longest, run);
  }
  REQUIRE(longest <= 2);
  for (std::size_t i = late + 3; i < loop.frames.size(); ++i) {
    REQUIRE(loop.frames[i].pacer.missed == 0);
  }
}

TEST_CASE("Presents one refresh late are misses although starts are on time",
          "[pacer]") {
  const testing::MainThreadScope main;
  Loop loop(Patched());
  loop.Attach();
  for (int i = 0; i < 10; ++i) loop.Step();
  loop.port.LatePresents(1000);
  for (int i = 0; i < 40; ++i) loop.Step();
  REQUIRE(loop.pacer.Totals().missed_presents >= 30);
  REQUIRE(loop.pacer.Totals().missed_vblanks >= 30);
}

TEST_CASE("The latency wait is the source; statistics are counted beside it",
          "[pacer]") {
  const testing::MainThreadScope main;
  Loop loop(Patched());
  loop.Attach();
  for (int i = 0; i < 30; ++i) loop.Step();
  const PacerTotals& totals = loop.pacer.Totals();
  for (const Frame& frame : loop.frames) {
    CHECK(frame.pacer.miss_source == MissSource::kLatencyWait);
  }
  REQUIRE(totals.resolved_by_wait == 30);
  REQUIRE(totals.resolved_by_statistics == 0);
  REQUIRE(totals.resolved_presents == totals.resolved_by_wait);
  // The fake shows each present at its target, so the statistics agree.
  REQUIRE(totals.stats_resolved >= 28);
  REQUIRE(totals.stats_missed == 0);
  // The waits polled first, then blocked.
  REQUIRE(loop.port.waitable_polls == 30);
  REQUIRE(loop.port.waitable_waits == 30);
}

TEST_CASE("Without the waitable, present statistics are the source",
          "[pacer]") {
  const testing::MainThreadScope main;
  // Vulkan with VK_GOOGLE_display_timing but no present_wait: Standard.
  PresentCaps caps = Patched();
  caps.frame_latency_waitable = false;
  Loop loop(caps);
  loop.Attach();
  REQUIRE(loop.pacer.Mode() == PacingMode::kStandard);
  for (int i = 0; i < 30; ++i) loop.Step();
  loop.port.LatePresents(1);
  for (int i = 0; i < 10; ++i) loop.Step();
  const PacerTotals& totals = loop.pacer.Totals();
  CHECK(loop.frames.back().pacer.miss_source == MissSource::kStatistics);
  REQUIRE(totals.resolved_by_wait == 0);
  REQUIRE(totals.resolved_by_statistics >= 35);
  REQUIRE(totals.missed_presents == 1);
  REQUIRE(totals.stats_missed == 1);
}

TEST_CASE("A window without statistics: Paced, misses from the latency wait",
          "[pacer]") {
  const testing::MainThreadScope main;
  Loop loop(Windowed());
  loop.Attach();
  REQUIRE(loop.pacer.Mode() == PacingMode::kPaced);
  CHECK(loop.pacer.ModeReason().find("latency wait") != std::string_view::npos);
  for (int i = 0; i < 60; ++i) loop.Step();
  const PacerTotals& totals = loop.pacer.Totals();
  REQUIRE(totals.missed_presents == 0);
  REQUIRE(totals.resolved_by_wait >= 58);
  REQUIRE(totals.resolved_by_statistics == 0);
  for (std::size_t i = 2; i < loop.frames.size(); ++i) {
    const PacerFrame& frame = loop.frames[i].pacer;
    CHECK(frame.miss_source == MissSource::kLatencyWait);
    REQUIRE(frame.resolved == 1);
    CHECK(frame.wait_blocked);
    // The fake's wait returns 0.1 ms after the vblank that showed the
    // present: its target.
    REQUIRE(std::llabs(frame.wait_phase_ns - 100'000) <= 2);
  }

  SECTION("presents one refresh late are misses") {
    loop.port.LatePresents(1000);
    for (int i = 0; i < 40; ++i) loop.Step();
    REQUIRE(totals.missed_presents >= 38);
    REQUIRE(totals.missed_vblanks >= 38);
  }
  SECTION("a late frame drains within 2 frames") {
    loop.Step(30'000'000);
    const std::size_t late = loop.frames.size() - 1;
    for (int i = 0; i < 30; ++i) loop.Step();
    REQUIRE(loop.frames[late].pacer.missed == 1);
    REQUIRE(loop.frames[late].pacer.late_vblanks >= 4);
    for (std::size_t i = late + 1; i < loop.frames.size(); ++i) {
      REQUIRE(loop.frames[i].pacer.missed == 0);
    }
  }
}

TEST_CASE("Sampler stamps after the waits' returns cause no false misses",
          "[pacer]") {
  const testing::MainThreadScope main;
  // The waits return 0.1 ms after each vblank, the sampler stamps it
  // 0.5 ms after: a frame start just after a vblank is before its stamp.
  FramePacerConfig config = WindowsConfig();
  SECTION("Standard") { config.request = PacingRequest::kStandard; }
  SECTION("Paced") { config.request = PacingRequest::kPaced; }
  Loop loop(Windowed(), config);
  loop.port.sampler_lag_ns = 500'000;
  loop.Attach();
  for (int i = 0; i < 60; ++i) loop.Step();
  REQUIRE(loop.pacer.Totals().resolved_by_wait >= 58);
  REQUIRE(loop.pacer.Totals().missed_presents == 0);
  // A present one refresh late is still a miss.
  loop.port.LatePresents(1);
  for (int i = 0; i < 5; ++i) loop.Step();
  REQUIRE(loop.pacer.Totals().missed_presents == 1);
}

TEST_CASE("The sampler decides between Paced and Standard", "[pacer]") {
  const testing::MainThreadScope main;
  Loop loop(Patched());
  // 10% off the display mode: Standard, with the reason.
  loop.sampler_scale = 1.1;
  loop.Attach();
  CHECK(loop.pacer.Mode() == PacingMode::kStandard);
  CHECK(loop.pacer.ModeReason().find("0.5%") != std::string_view::npos);
  loop.Run(1.0);
  CHECK(loop.pacer.Mode() == PacingMode::kStandard);
  // Agreement: Paced after the promotion delay, not at once.
  loop.sampler_scale = 1.0;
  loop.Step();
  CHECK(loop.pacer.Mode() == PacingMode::kStandard);
  loop.Run(1.2);
  CHECK(loop.pacer.Mode() == PacingMode::kPaced);
  const std::uint64_t changes = loop.pacer.Totals().mode_changes;
  // A sampler that goes away drops Paced at the next frame.
  loop.sampler_valid = false;
  loop.Step();
  loop.Step();
  CHECK(loop.pacer.Mode() == PacingMode::kStandard);
  CHECK(loop.pacer.Totals().mode_changes == changes + 1);
}

TEST_CASE("Nothing waits while Modal", "[pacer]") {
  const testing::MainThreadScope main;
  Loop loop(Patched());
  loop.Attach();
  for (int i = 0; i < 10; ++i) loop.Step();
  const std::uint64_t sleeps = loop.clock.sleep_count;
  const std::uint32_t waits = loop.port.waitable_waits;
  loop.port.fence_pending = true;
  const Frame modal = loop.Step(1'000'000, /*modal=*/true);
  CHECK(modal.clock.modal);
  CHECK(modal.clock.gpu_fence_pending_at_start);
  REQUIRE(loop.port.fence_waits == 0);
  for (int i = 0; i < 5; ++i) loop.Step(1'000'000, /*modal=*/true);
  CHECK(loop.clock.sleep_count == sleeps);
  CHECK(loop.port.waitable_waits == waits);
  REQUIRE(loop.port.swapchain_waits == 0);
}

TEST_CASE("Modal frames at the WM_TIMER rate run the tick rate at 165 Hz",
          "[pacer]") {
  const testing::MainThreadScope main;
  Loop loop(Patched());
  // Costs that leave no room for a second tick in a 165 Hz period (an ASan
  // build): k_hi is 0, so with the display's period the cap would be 1.
  loop.costs.non_tick_hi = 5.0e-3;
  loop.costs.non_tick_95 = 5.0e-3;
  loop.costs.tick_hi = 1.0e-3;
  loop.costs.tick_95 = 1.0e-3;
  loop.Attach();
  loop.Run(0.5);
  // SDL's WM_TIMER drives the modal loop's iterates every 15.6 ms; nothing
  // waits.
  std::uint64_t ticks = 0;
  int max_steps = 0;
  int frames = 0;
  int over_cap = 0;
  const std::uint64_t start = loop.clock.now_ns;
  while (loop.clock.now_ns - start < 1'000'000'000) {
    const FrameClock clock = loop.pacer.BeginFrame(true, loop.stepper);
    const StepPlan plan =
        loop.stepper.Plan(clock, loop.costs, true, false, false);
    ticks += static_cast<std::uint64_t>(plan.steps);
    max_steps = std::max(max_steps, plan.steps);
    if (plan.steps > plan.k_max) ++over_cap;
    ++frames;
    loop.clock.Advance(15'600'000);
    loop.port.Present();
    loop.pacer.OnFrameSubmitted(true, false);
    loop.pacer.EndFrame(loop.stepper.PlanNext(clock, loop.costs), loop.costs,
                        true);
  }
  INFO(frames << " modal frames, " << ticks << " ticks, at most " << max_steps
              << " per frame");
  REQUIRE(frames >= 60);
  // 120 ticks a second within a frame's worth, the cap still applied.
  REQUIRE(ticks >= 115);
  REQUIRE(ticks <= 122);
  REQUIRE(over_cap == 0);
  REQUIRE(max_steps >= 2);
  // Out of the modal loop the display's period is back.
  const Frame after = loop.Step();
  CHECK(std::abs(after.clock.period_sec - kPeriod165 * 1e-9) < 1e-6);
}

TEST_CASE("The N-L fence: pending at frame start, then waited for", "[pacer]") {
  const testing::MainThreadScope main;
  Loop loop(Patched());
  loop.Attach();
  for (int i = 0; i < 5; ++i) loop.Step();
  CHECK_FALSE(loop.frames.back().clock.gpu_fence_pending_at_start);
  loop.port.fence_pending = true;
  const Frame frame = loop.Step();
  CHECK(frame.clock.gpu_fence_pending_at_start);
  CHECK(frame.pacer.fence_pending);
  REQUIRE(frame.pacer.fence_wait_ns == 500'000);
  REQUIRE(loop.port.fence_waits == 1);
  REQUIRE(loop.pacer.Totals().fence_waits == 1);
  CHECK_FALSE(loop.Step().clock.gpu_fence_pending_at_start);
}

TEST_CASE("Every wait of the pacer raises the in-driver flag", "[pacer]") {
  // M12: the watchdog labels a hang inside one a GPU or compositor stall.
  const testing::MainThreadScope main;
  for (const bool patched : {true, false}) {
    INFO("patched " << patched);
    Loop loop(patched ? Patched() : Unpatched());
    loop.Attach();
    for (int i = 0; i < 10; ++i) loop.Step();
    loop.port.fence_pending = true;
    for (int i = 0; i < 10; ++i) loop.Step();
    const FakePresentPort& port = loop.port;
    REQUIRE(port.waitable_waits + port.waitable_polls + port.swapchain_waits +
                port.fence_polls + port.fence_waits >
            10);
    REQUIRE(port.waits_outside_driver == 0);
    CHECK_FALSE(InDriverWait());
  }
}

TEST_CASE("Driver wait scopes nest and restore the flag", "[pacer]") {
  REQUIRE_FALSE(InDriverWait());
  {
    const DriverWaitScope outer;
    REQUIRE(InDriverWait());
    {
      const DriverWaitScope inner;
      REQUIRE(DriverWaitFlag().load());
    }
    REQUIRE(InDriverWait());
  }
  REQUIRE_FALSE(InDriverWait());
}

TEST_CASE("Frames that present nothing do not wait", "[pacer]") {
  const testing::MainThreadScope main;
  Loop loop(Unpatched());
  loop.Attach();
  loop.Step(1'000'000, false, /*present=*/false);
  REQUIRE(loop.port.swapchain_waits == 0);
  REQUIRE(loop.pacer.Totals().null_acquires == 1);
}

TEST_CASE("VRR: IMMEDIATE and a 120 Hz deadline, one tick per frame",
          "[pacer]") {
  const testing::MainThreadScope main;
  FramePacerConfig config = WindowsConfig();
  config.request = PacingRequest::kVrr;
  Loop loop(Unpatched(), config);
  loop.Attach();
  REQUIRE(loop.pacer.Mode() == PacingMode::kVrr);
  REQUIRE(loop.port.present_modes ==
          std::vector<SwapchainMode>{SwapchainMode::kImmediate});
  for (int i = 0; i < 40; ++i) loop.Step();
  for (std::size_t i = 2; i < loop.frames.size(); ++i) {
    const Frame& frame = loop.frames[i];
    REQUIRE(frame.clock.frame_start_ns -
                loop.frames[i - 1].clock.frame_start_ns ==
            8'333'333);
    REQUIRE(frame.clock.elapsed_vblanks == 1);
    CHECK(frame.clock.vblanks_known);
    REQUIRE(frame.pacer.deadline_error_ns == 0);
    CHECK(frame.begin_ns == frame.clock.frame_start_ns);
  }
  // 120 Hz ticks: Locked, one per frame.
  const StepPlan plan = loop.stepper.Plan(loop.frames.back().clock, loop.costs,
                                          true, false, false);
  CHECK(plan.cadence == Cadence::kLocked);
  REQUIRE(plan.k_rt == 1);
  REQUIRE(loop.pacer.Totals().resolved_presents == 0);
  loop.pacer.Detach();
  CHECK(loop.port.present_modes.back() == SwapchainMode::kVsync);
}

TEST_CASE("VRR below a 120 Hz panel runs a 60 Hz deadline", "[pacer]") {
  const testing::MainThreadScope main;
  FramePacerConfig config = WindowsConfig();
  config.request = PacingRequest::kVrr;
  Loop loop(Unpatched(), config, 1e9 / 60.0);
  loop.Attach(60.0);
  REQUIRE(loop.pacer.Mode() == PacingMode::kVrr);
  for (int i = 0; i < 5; ++i) loop.Step();
  REQUIRE(loop.frames.back().clock.frame_start_ns -
              loop.frames[loop.frames.size() - 2].clock.frame_start_ns ==
          16'666'667);
}

TEST_CASE("VRR needs IMMEDIATE", "[pacer]") {
  const testing::MainThreadScope main;
  FramePacerConfig config = WindowsConfig();
  config.request = PacingRequest::kVrr;
  SECTION("not supported") {
    Loop loop(PresentCaps{}, config);
    loop.Attach();
    CHECK(loop.pacer.Mode() == PacingMode::kStandard);
  }
  SECTION("refused by the swapchain") {
    Loop loop(Unpatched(), config);
    loop.port.present_mode_result = false;
    loop.Attach();
    CHECK(loop.pacer.Mode() == PacingMode::kStandard);
    CHECK(loop.pacer.ModeReason().find("refused") != std::string_view::npos);
  }
}

TEST_CASE("HalfRate in Standard presents the last composite again", "[pacer]") {
  const testing::MainThreadScope main;
  Loop loop(Unpatched());
  // A full-rate frame plans more than the 6.06 ms period.
  loop.costs.non_tick_hi = 5.0e-3;
  loop.costs.non_tick_95 = 5.0e-3;
  loop.Attach();
  loop.Run(1.5);
  CHECK_FALSE(loop.pacer.HalfRate());
  loop.Run(1.0);
  REQUIRE(loop.pacer.HalfRate());
  REQUIRE(loop.pacer.PresentInterval() == 2);
  REQUIRE(loop.pacer.Totals().half_rate_entries == 1);
  const std::uint32_t represents = loop.port.represents;
  loop.Step();
  const Frame frame = loop.Step();
  CHECK(loop.port.represents == represents + 2);
  CHECK(frame.pacer.represented);
  CHECK(std::abs(frame.clock.period_sec - 2.0 * kPeriod165 * 1e-9) < 1e-9);
  REQUIRE(frame.clock.elapsed_vblanks == 1);
  CHECK(loop.pacer.Summary().find("x2") != std::string::npos);
  // Margin for 4 s: back to every vblank.
  loop.costs.non_tick_hi = 0.6e-3;
  loop.costs.non_tick_95 = 0.6e-3;
  loop.Run(4.5);
  CHECK_FALSE(loop.pacer.HalfRate());
  REQUIRE(loop.pacer.Totals().half_rate_exits == 1);
}

// Standard with the waitable: the latency wait is the miss source.
FramePacerConfig StandardConfig() {
  FramePacerConfig config = WindowsConfig();
  config.request = PacingRequest::kStandard;
  return config;
}

TEST_CASE("Standard HalfRate: a frame's present one vblank late is a miss",
          "[pacer]") {
  const testing::MainThreadScope main;
  Loop loop(Windowed(), StandardConfig());
  // A full-rate frame plans more than the period; the frames themselves
  // are fast (1 ms), so each is taken at the vblank after its start and
  // presented again at the next one.
  loop.costs.non_tick_hi = 5.0e-3;
  loop.costs.non_tick_95 = 5.0e-3;
  loop.Attach();
  REQUIRE(loop.pacer.Mode() == PacingMode::kStandard);
  loop.Run(2.5);
  REQUIRE(loop.pacer.HalfRate());
  for (int i = 0; i < 20; ++i) loop.Step();
  const PacerTotals& totals = loop.pacer.Totals();
  REQUIRE(totals.missed_presents == 0);
  REQUIRE(totals.represents >= 20);
  for (std::size_t i = loop.frames.size() - 19; i < loop.frames.size(); ++i) {
    const PacerFrame& frame = loop.frames[i].pacer;
    CHECK(frame.represented);
    // The frame's own present targets the vblank after its start.
    CHECK(std::abs(static_cast<double>(frame.target_ns - frame.frame_start_ns) -
                   kPeriod165) < 2.0);
  }
  // The next frame's own present shows one vblank late (ADR-071).
  loop.port.LatePresents(1);
  for (int i = 0; i < 6; ++i) loop.Step();
  REQUIRE(totals.missed_presents >= 1);
}

TEST_CASE("Standard at frame latency 2: a present behind one is on time",
          "[pacer]") {
  const testing::MainThreadScope main;
  // The editor's pacing (M10a, ADR-097): Standard at frame latency 2. One
  // present waits in the queue ahead of each frame's own, so a frame's
  // present shows two vblanks after the frame's vblank, and that is on
  // time: no misses and no HalfRate from an editor presenting every
  // vblank.
  FramePacerConfig config = StandardConfig();
  config.frame_latency = 2;
  Loop loop(Windowed(), config);
  loop.port.model_frame_latency = true;
  loop.Attach();
  REQUIRE(loop.pacer.Mode() == PacingMode::kStandard);
  REQUIRE(loop.pacer.FrameLatency() == 2);
  REQUIRE(loop.port.latencies.back() == 2);
  loop.Run(2.0);
  const PacerTotals& totals = loop.pacer.Totals();
  // A frame every vblank.
  REQUIRE(loop.frames.size() > 320);
  REQUIRE(totals.resolved_presents > 300);
  REQUIRE(totals.missed_presents == 0);
  REQUIRE(totals.half_rate_entries == 0);
  REQUIRE_FALSE(loop.pacer.HalfRate());
  for (std::size_t i = loop.frames.size() - 20; i < loop.frames.size(); ++i) {
    const PacerFrame& frame = loop.frames[i].pacer;
    // The target is two vblanks after the frame's vblank, where the fake
    // display shows the present (presents are 1-based, one a frame).
    const std::uint64_t vblank = loop.port.LatestVblank(frame.frame_start_ns);
    CHECK(std::abs(static_cast<double>(frame.target_ns - vblank) -
                   2.0 * kPeriod165) < 2.0);
    const auto shown = loop.port.ShownRefresh(i + 1);
    REQUIRE(shown.has_value());
    CHECK(*shown ==
          loop.port.RefreshAt(frame.target_ns) + FakePresentPort::kRefreshBase);
  }
  // One vblank later than that is a miss.
  loop.port.LatePresents(1);
  for (int i = 0; i < 8; ++i) loop.Step();
  REQUIRE(totals.missed_presents >= 1);
}

TEST_CASE("Standard HalfRate: a frame slower than a vblank holds two",
          "[pacer]") {
  const testing::MainThreadScope main;
  // With the waitable the take is where the wait returned; without it, the
  // first vblank after the submit.
  PresentCaps caps = Windowed();
  SECTION("the waitable") {}
  SECTION("no waitable") { caps = Unpatched(); }
  Loop loop(caps, StandardConfig());
  loop.costs.non_tick_hi = 5.0e-3;
  loop.costs.non_tick_95 = 5.0e-3;
  loop.Attach();
  // 8 ms frames: more than one vblank (6.06 ms), less than two.
  constexpr std::uint64_t kSlow = 8'000'000;
  // HalfRate's timers count presentation periods: these frames take two.
  loop.Run(5.0, kSlow);
  REQUIRE(loop.pacer.HalfRate());
  for (int i = 0; i < 5; ++i) loop.Step(kSlow);
  const PacerTotals& totals = loop.pacer.Totals();
  const std::uint64_t represents = totals.represents;
  const std::uint64_t missed = totals.missed_presents;
  for (int i = 0; i < 20; ++i) loop.Step(kSlow);
  // Taken at the second vblank: no re-present (it would hold the frame
  // for three), frames two vblanks apart, every present on time.
  CHECK(totals.represents == represents);
  CHECK(totals.missed_presents == missed);
  for (std::size_t i = loop.frames.size() - 19; i < loop.frames.size(); ++i) {
    const FrameClock& clock = loop.frames[i].clock;
    const double apart =
        static_cast<double>(clock.frame_start_ns - clock.prev_frame_start_ns);
    CHECK(std::abs(apart - 2.0 * kPeriod165) < 2.0);
    REQUIRE(clock.elapsed_vblanks == 1);
    CHECK_FALSE(loop.frames[i].pacer.represented);
  }
}

TEST_CASE("Paced HalfRate targets every second vblank", "[pacer]") {
  const testing::MainThreadScope main;
  Loop loop(Patched());
  loop.costs.non_tick_hi = 5.0e-3;
  loop.costs.non_tick_95 = 5.0e-3;
  loop.Attach();
  loop.Run(2.5, 500'000);
  REQUIRE(loop.pacer.HalfRate());
  for (int i = 0; i < 4; ++i) loop.Step(500'000);
  const std::size_t n = loop.frames.size();
  const std::uint64_t step =
      loop.frames[n - 1].pacer.target_ns - loop.frames[n - 2].pacer.target_ns;
  CHECK(step == static_cast<std::uint64_t>(std::llround(2.0 * kPeriod165)));
  REQUIRE(loop.port.represents == 0);
}

TEST_CASE("Adaptive latency sets frame latency 2, then 1 again", "[pacer]") {
  const testing::MainThreadScope main;
  FramePacerConfig config = WindowsConfig();
  config.half_rate = false;
  Loop loop(Patched(), config);
  // CPU 3.9 ms and GPU 3 ms each fit in 6.06 ms; together they do not.
  loop.costs.non_tick_95 = 3.5e-3;
  loop.costs.gpu_95 = 3.0e-3;
  loop.Attach();
  loop.Run(2.5, 500'000);
  REQUIRE(loop.pacer.FrameLatency() == 2);
  REQUIRE(loop.port.latencies.back() == 2);
  loop.costs.non_tick_95 = 1.0e-3;
  loop.Run(4.5, 500'000);
  REQUIRE(loop.pacer.FrameLatency() == 1);
  REQUIRE(loop.port.latencies.back() == 1);
  REQUIRE(loop.pacer.Totals().latency_changes == 2);
}

TEST_CASE("Headless: Standard on the wall clock, nothing to wait for",
          "[pacer]") {
  const testing::MainThreadScope main;
  FakePacerClock clock;
  FramePacer pacer(WindowsConfig(), &clock);
  FixedStepper stepper(120);
  pacer.Attach(nullptr, {});
  CHECK(pacer.Mode() == PacingMode::kStandard);
  CHECK(pacer.ModeReason().find("headless") != std::string_view::npos);
  const FrameClock first = pacer.BeginFrame(false, stepper);
  pacer.OnFrameSubmitted(false, false);
  pacer.EndFrame(StepPlan{}, FrameCosts{}, false);
  clock.Advance(16'000'000);
  const FrameClock second = pacer.BeginFrame(false, stepper);
  CHECK_FALSE(second.vblanks_known);
  REQUIRE(second.frame_start_ns - first.frame_start_ns == 16'000'000);
  CHECK(std::abs(second.period_sec - 1.0 / 60.0) < 1e-12);
  REQUIRE(clock.sleep_count == 0);
}

TEST_CASE("A fixed frame dt (--fixed-frame-dt) is a synthetic clock",
          "[pacer]") {
  const testing::MainThreadScope main;
  FramePacerConfig config = WindowsConfig();
  config.fixed_frame_dt_sec = 1.0 / 60.0;
  Loop loop(Unpatched(), config);
  loop.Attach();
  for (int i = 0; i < 5; ++i) loop.Step();
  for (std::size_t i = 1; i < loop.frames.size(); ++i) {
    REQUIRE(loop.frames[i].clock.frame_start_ns -
                loop.frames[i - 1].clock.frame_start_ns ==
            16'666'667);
    CHECK(loop.frames[i].clock.vblanks_known);
  }
}

TEST_CASE("ResetClock makes the next frame a first frame", "[pacer]") {
  const testing::MainThreadScope main;
  Loop loop(Patched());
  loop.Attach();
  for (int i = 0; i < 5; ++i) loop.Step();
  REQUIRE(loop.frames.back().clock.prev_frame_start_ns != 0);
  loop.pacer.ResetClock();
  REQUIRE(loop.Step().clock.prev_frame_start_ns == 0);
  REQUIRE(loop.Step().clock.prev_frame_start_ns != 0);
}

std::expected<AppConfig, Error> Parse(std::vector<const char*> args) {
  return ParseAppConfig(std::span<const char* const>(args));
}

TEST_CASE("The --pacing option chooses the request; auto by default",
          "[pacer]") {
  auto defaults = Parse({});
  REQUIRE(defaults);
  CHECK(defaults->pacing == PacingRequest::kAuto);
  CHECK(defaults->late_frame_ms == 0.0);
  const std::vector<std::pair<const char*, PacingRequest>> requests = {
      {"paced", PacingRequest::kPaced},
      {"standard", PacingRequest::kStandard},
      {"vrr", PacingRequest::kVrr},
      {"auto", PacingRequest::kAuto}};
  for (const auto& [name, request] : requests) {
    auto config = Parse({"--pacing", name});
    REQUIRE(config);
    CHECK(config->pacing == request);
  }
  auto joined = Parse({"--pacing=vrr"});
  REQUIRE(joined);
  CHECK(joined->pacing == PacingRequest::kVrr);
  CHECK_FALSE(Parse({"--pacing"}));
  CHECK_FALSE(Parse({"--pacing", "fast"}));
}

TEST_CASE("The --inject-late-frame FRAME:MS test hook", "[pacer]") {
  auto config = Parse({"--inject-late-frame", "400:30"});
  if (!kRuntimeTestHooks) {
    CHECK_FALSE(config);
    return;
  }
  REQUIRE(config);
  REQUIRE(config->late_frame_at == 400);
  CHECK(config->late_frame_ms == 30.0);
  auto fraction = Parse({"--inject-late-frame=1:2.5"});
  REQUIRE(fraction);
  CHECK(fraction->late_frame_ms == 2.5);
  CHECK_FALSE(Parse({"--inject-late-frame", "400"}));
  CHECK_FALSE(Parse({"--inject-late-frame", "0:30"}));
  CHECK_FALSE(Parse({"--inject-late-frame", "400:0"}));
  CHECK_FALSE(Parse({"--inject-late-frame", "400:1001"}));
  CHECK_FALSE(Parse({"--inject-late-frame", "x:30"}));
}

TEST_CASE("The --inject-device-lost FRAME test hook", "[pacer]") {
  auto config = Parse({"--inject-device-lost", "100"});
  if (!kRuntimeTestHooks) {
    CHECK_FALSE(config);
    return;
  }
  REQUIRE(config);
  REQUIRE(config->device_lost_at == 100);
  auto defaults = Parse({});
  REQUIRE(defaults);
  REQUIRE(defaults->device_lost_at == 0);
  CHECK_FALSE(Parse({"--inject-device-lost", "0"}));
  CHECK_FALSE(Parse({"--inject-device-lost", "x"}));
  CHECK_FALSE(Parse({"--inject-device-lost"}));
}

}  // namespace
}  // namespace se
