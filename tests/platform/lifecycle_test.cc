// Copyright 2026 The simple-engine-2 Authors.
//
// The lifecycle state machine, without SDL (docs/milestone-specs.md M5,
// lifecycle.h): every M5 transition and its outputs.

#include "se_runtime/lifecycle.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>

namespace se {
namespace {

constexpr std::uint64_t kSec = 1'000'000'000;

Lifecycle Booted(LifecycleConfig config = {}) {
  Lifecycle lifecycle(config);
  REQUIRE(lifecycle.State() == LifecycleState::kBooting);
  REQUIRE_FALSE(lifecycle.ShouldRender());
  lifecycle.BootComplete(10 * kSec, /*focused=*/true, /*visible=*/true);
  REQUIRE(lifecycle.State() == LifecycleState::kRunning);
  // The first frame starts the frame clock.
  REQUIRE(lifecycle.TakeFrameClockReset());
  return lifecycle;
}

TEST_CASE("Running renders, runs gameplay and iterates at full rate",
          "[lifecycle]") {
  Lifecycle lifecycle = Booted();
  CHECK(lifecycle.ShouldRender());
  CHECK(lifecycle.GameplayRuns());
  CHECK(lifecycle.Rate() == CallbackRate::kFull);
  REQUIRE(CallbackRateHint(CallbackRate::kFull) == "0");
  REQUIRE(CallbackRateHint(CallbackRate::kWaitEvent) == "waitevent");
  REQUIRE(CallbackRateHint(CallbackRate::kSuspended) == "10");
  REQUIRE(LifecycleStateName(LifecycleState::kSuspended) == "Suspended");
}

TEST_CASE("A NULL acquire on a visible window stays Running", "[lifecycle]") {
  Lifecycle lifecycle = Booted();
  for (int i = 0; i < 30; ++i) {
    lifecycle.OnNullAcquire();
    lifecycle.Update(10 * kSec + static_cast<std::uint64_t>(i) * kSec / 60);
  }
  CHECK(lifecycle.State() == LifecycleState::kRunning);
  REQUIRE(lifecycle.NullAcquires() == 30);
  CHECK(lifecycle.ShouldRender());
  CHECK(lifecycle.Rate() == CallbackRate::kFull);
  CHECK_FALSE(lifecycle.TakeFrameClockReset());
}

TEST_CASE("Minimize suspends; restore resumes and resets the frame clock",
          "[lifecycle]") {
  Lifecycle lifecycle = Booted();
  lifecycle.OnWindow(WindowSignal::kMinimized, 11 * kSec);
  CHECK(lifecycle.State() == LifecycleState::kSuspended);
  CHECK(lifecycle.Rate() == CallbackRate::kSuspended);
  CHECK_FALSE(lifecycle.ShouldRender());
  CHECK_FALSE(lifecycle.GameplayRuns());
  CHECK(lifecycle.Suspended());
  CHECK_FALSE(lifecycle.TakeFrameClockReset());

  lifecycle.OnWindow(WindowSignal::kRestored, 15 * kSec);
  CHECK(lifecycle.State() == LifecycleState::kRunning);
  CHECK(lifecycle.Rate() == CallbackRate::kFull);
  CHECK(lifecycle.GameplayRuns());
  // No catch-up for the four seconds spent minimized.
  CHECK(lifecycle.TakeFrameClockReset());
  CHECK_FALSE(lifecycle.TakeFrameClockReset());
}

TEST_CASE("Hidden and occluded suspend; shown and exposed resume",
          "[lifecycle]") {
  Lifecycle lifecycle = Booted();
  lifecycle.OnWindow(WindowSignal::kHidden, 11 * kSec);
  CHECK(lifecycle.State() == LifecycleState::kSuspended);
  lifecycle.OnWindow(WindowSignal::kShown, 12 * kSec);
  CHECK(lifecycle.State() == LifecycleState::kRunning);
  CHECK(lifecycle.TakeFrameClockReset());
  lifecycle.OnWindow(WindowSignal::kOccluded, 13 * kSec);
  CHECK(lifecycle.State() == LifecycleState::kSuspended);
  lifecycle.OnWindow(WindowSignal::kExposed, 14 * kSec);
  CHECK(lifecycle.State() == LifecycleState::kRunning);
  CHECK(lifecycle.TakeFrameClockReset());
}

TEST_CASE("The 10 Hz probe leaves Suspended once a visible window acquires",
          "[lifecycle]") {
  Lifecycle lifecycle = Booted();
  lifecycle.OnWindow(WindowSignal::kMinimized, 11 * kSec);
  lifecycle.OnSuspendProbe(/*window_visible=*/false,
                           /*acquire_succeeded=*/false);
  CHECK(lifecycle.Suspended());
  lifecycle.OnSuspendProbe(/*window_visible=*/true,
                           /*acquire_succeeded=*/false);
  CHECK(lifecycle.Suspended());
  lifecycle.OnSuspendProbe(/*window_visible=*/true, /*acquire_succeeded=*/true);
  CHECK_FALSE(lifecycle.Suspended());
  CHECK(lifecycle.TakeFrameClockReset());
}

TEST_CASE("Power suspend and resume enter and leave Suspended", "[lifecycle]") {
  Lifecycle lifecycle = Booted();
  lifecycle.OnPowerSuspend();
  CHECK(lifecycle.State() == LifecycleState::kSuspended);
  CHECK(lifecycle.Rate() == CallbackRate::kSuspended);
  // A window event does not end a power suspend.
  lifecycle.OnWindow(WindowSignal::kExposed, 11 * kSec);
  CHECK(lifecycle.Suspended());
  lifecycle.OnPowerResume();
  CHECK(lifecycle.State() == LifecycleState::kRunning);
  CHECK(lifecycle.TakeFrameClockReset());
  // Minimized during a power suspend stays suspended after resume.
  lifecycle.OnPowerSuspend();
  lifecycle.OnWindow(WindowSignal::kMinimized, 12 * kSec);
  lifecycle.OnPowerResume();
  CHECK(lifecycle.State() == LifecycleState::kSuspended);
  CHECK_FALSE(lifecycle.TakeFrameClockReset());
}

TEST_CASE("FocusLost clears held input and keeps running", "[lifecycle]") {
  Lifecycle lifecycle = Booted();
  CHECK_FALSE(lifecycle.TakeClearHeldInput());
  lifecycle.OnWindow(WindowSignal::kFocusLost, 11 * kSec);
  CHECK(lifecycle.State() == LifecycleState::kFocusLost);
  CHECK(lifecycle.TakeClearHeldInput());
  CHECK_FALSE(lifecycle.TakeClearHeldInput());
  CHECK(lifecycle.ShouldRender());
  CHECK(lifecycle.GameplayRuns());
  CHECK(lifecycle.Rate() == CallbackRate::kFull);
  lifecycle.OnWindow(WindowSignal::kFocusGained, 12 * kSec);
  CHECK(lifecycle.State() == LifecycleState::kRunning);
  // Input resumes from a clean held state.
  CHECK(lifecycle.TakeClearHeldInput());
}

TEST_CASE("A game may pause while unfocused", "[lifecycle]") {
  LifecycleConfig config;
  config.pause_on_focus_lost = true;
  Lifecycle lifecycle = Booted(config);
  lifecycle.OnWindow(WindowSignal::kFocusLost, 11 * kSec);
  CHECK_FALSE(lifecycle.GameplayRuns());
  CHECK(lifecycle.ShouldRender());
}

TEST_CASE("Modal keeps rendering and ticking and clears held input",
          "[lifecycle]") {
  Lifecycle lifecycle = Booted();
  lifecycle.SetModal(true);
  CHECK(lifecycle.State() == LifecycleState::kModal);
  CHECK(lifecycle.Modal());
  CHECK(lifecycle.TakeClearHeldInput());
  CHECK(lifecycle.ShouldRender());
  CHECK(lifecycle.GameplayRuns());
  CHECK(lifecycle.Rate() == CallbackRate::kFull);
  lifecycle.SetModal(false);
  CHECK(lifecycle.State() == LifecycleState::kRunning);
  CHECK_FALSE(lifecycle.TakeClearHeldInput());
  CHECK_FALSE(lifecycle.TakeFrameClockReset());
}

TEST_CASE("Idle only when allowed, after 1 s without input", "[lifecycle]") {
  SECTION("games never idle") {
    Lifecycle lifecycle = Booted();
    lifecycle.Update(20 * kSec);
    CHECK(lifecycle.State() == LifecycleState::kRunning);
  }
  SECTION("the editor idles and wakes on any event") {
    LifecycleConfig config;
    config.idle_allowed = true;
    Lifecycle lifecycle = Booted(config);
    lifecycle.Update(10 * kSec + kSec / 2);
    CHECK(lifecycle.State() == LifecycleState::kRunning);
    lifecycle.Update(11 * kSec);
    CHECK(lifecycle.State() == LifecycleState::kIdle);
    CHECK(lifecycle.Rate() == CallbackRate::kWaitEvent);
    CHECK(lifecycle.ShouldRender());

    // A wake: the next frame runs, then Idle returns (no input came).
    lifecycle.OnAnyEvent();
    CHECK(lifecycle.State() == LifecycleState::kRunning);
    lifecycle.Update(12 * kSec);
    CHECK(lifecycle.State() == LifecycleState::kRunning);
    CHECK(lifecycle.Rate() == CallbackRate::kFull);
    lifecycle.Update(12 * kSec + 1);
    CHECK(lifecycle.State() == LifecycleState::kIdle);

    // Input restarts the timer.
    lifecycle.OnInputActivity(13 * kSec);
    lifecycle.Update(13 * kSec + kSec / 2);
    CHECK(lifecycle.State() == LifecycleState::kRunning);
    lifecycle.Update(14 * kSec);
    CHECK(lifecycle.State() == LifecycleState::kIdle);
  }
  SECTION("never idle while modal or suspended") {
    LifecycleConfig config;
    config.idle_allowed = true;
    Lifecycle lifecycle = Booted(config);
    lifecycle.SetModal(true);
    lifecycle.Update(20 * kSec);
    CHECK(lifecycle.State() == LifecycleState::kModal);
    lifecycle.SetModal(false);
    lifecycle.OnWindow(WindowSignal::kMinimized, 20 * kSec);
    lifecycle.Update(30 * kSec);
    CHECK(lifecycle.State() == LifecycleState::kSuspended);
    CHECK(lifecycle.Rate() == CallbackRate::kSuspended);
  }
}

TEST_CASE("Quit progress: QuitPending, cancel, ShuttingDown, Exited",
          "[lifecycle]") {
  Lifecycle lifecycle = Booted();
  lifecycle.OnQuitPending();
  CHECK(lifecycle.State() == LifecycleState::kQuitPending);
  // A draining quit keeps iterating and rendering.
  CHECK(lifecycle.ShouldRender());
  CHECK(lifecycle.GameplayRuns());
  CHECK(lifecycle.Rate() == CallbackRate::kFull);
  lifecycle.OnWindow(WindowSignal::kMinimized, 11 * kSec);
  CHECK(lifecycle.State() == LifecycleState::kQuitPending);
  CHECK(lifecycle.Rate() == CallbackRate::kSuspended);
  CHECK_FALSE(lifecycle.ShouldRender());
  lifecycle.OnWindow(WindowSignal::kRestored, 12 * kSec);
  lifecycle.OnQuitCancelled();
  CHECK(lifecycle.State() == LifecycleState::kRunning);
  lifecycle.OnQuitPending();
  lifecycle.OnShutdownStarted();
  CHECK(lifecycle.State() == LifecycleState::kShuttingDown);
  CHECK_FALSE(lifecycle.ShouldRender());
  CHECK_FALSE(lifecycle.GameplayRuns());
  lifecycle.OnExited();
  CHECK(lifecycle.State() == LifecycleState::kExited);
  REQUIRE(lifecycle.Transitions() > 0);
}

TEST_CASE("Booting into a hidden or unfocused window", "[lifecycle]") {
  Lifecycle hidden;
  hidden.BootComplete(kSec, /*focused=*/true, /*visible=*/false);
  CHECK(hidden.State() == LifecycleState::kSuspended);
  Lifecycle unfocused;
  unfocused.BootComplete(kSec, /*focused=*/false, /*visible=*/true);
  CHECK(unfocused.State() == LifecycleState::kFocusLost);
  CHECK(unfocused.GameplayRuns());
}

}  // namespace
}  // namespace se
