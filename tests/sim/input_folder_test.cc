// Copyright 2026 The simple-engine-2 Authors.
//
// InputFolder: InputFrame k holds every event stamped before the sim's
// deadline L_k, which is the inline driver's placement rule with the
// deadlines as tick windows (half-open: a stamp equal to L_k is tick k + 1's,
// as PlaceEdge places it; the frames equal StampedInput's, latched by the
// same window ends); later events wait at the ring's front; the fold counts
// the inputs holding each action as InputService does, counts every edge,
// keeps axes, and releases all.

#include "se_sim/input_folder.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "se_core/edge_placement.h"
#include "se_core/input_frame.h"
#include "se_sim/paced_clock.h"
#include "support/stamped_input.h"

namespace se {
namespace {

InputEvent Event(std::uint64_t stamp, ActionId action, InputKind kind,
                 float value = 0.0F) {
  InputEvent event;
  event.stamp_ns = stamp;
  event.action = action;
  event.kind = kind;
  event.source = EdgeSource::kRawKeyboard;
  event.value = value;
  return event;
}

TEST_CASE("InputFolder places each event on the first deadline after it",
          "[input_folder]") {
  InputEventRing ring(64);
  InputFolder folder;
  REQUIRE(ring.TryPush(Event(100, 1, InputKind::kPress)));
  REQUIRE(ring.TryPush(Event(199, 1, InputKind::kRelease)));
  REQUIRE(ring.TryPush(Event(200, 2, InputKind::kPress)));
  REQUIRE(ring.TryPush(Event(450, 3, InputKind::kPress)));

  // L_1 = 200: the tap at 100..199 counts, held clear again.
  const InputFrame one = folder.Latch(1, 200, ring);
  REQUIRE(one.tick == 1);
  REQUIRE(one.pressed[1] == 1);
  REQUIRE(one.released[1] == 1);
  CHECK_FALSE(one.Held(1));
  REQUIRE(one.edge_count == 2);
  CHECK(one.platform_live);
  // 200, at the deadline itself, waits at the front (half-open windows).
  REQUIRE(ring.Front() != nullptr);
  REQUIRE(ring.Front()->stamp_ns == 200);

  const InputFrame two = folder.Latch(2, 400, ring);
  REQUIRE(two.pressed[2] == 1);
  CHECK(two.Held(2));
  REQUIRE(two.edge_count == 1);
  // Held stays without new edges.
  const InputFrame three = folder.Latch(3, 449, ring);
  CHECK(three.Held(2));
  REQUIRE(three.edge_count == 0);
  const InputFrame four = folder.Latch(4, 450, ring);
  CHECK_FALSE(four.Held(3));
  REQUIRE(four.edge_count == 0);
  const InputFrame five = folder.Latch(5, 451, ring);
  CHECK(five.Held(3));
  CHECK(ring.Front() == nullptr);
}

TEST_CASE("InputFolder puts a stamp at a deadline where PlaceEdge puts it",
          "[input_folder]") {
  // A burst's deadlines as one frame's window ends.
  const std::array<std::uint64_t, 4> deadlines{1000, 2000, 3000, 4000};
  // Stamps on, just before and just after each boundary; the last window's
  // end is excluded (PlaceEdge gives later stamps to the frame's last tick,
  // which the folder has not: they wait).
  const std::vector<std::uint64_t> stamps{0,    999,  1000, 1001, 1999, 2000,
                                          2001, 2999, 3000, 3001, 3998, 3999};
  InputEventRing ring(64);
  for (std::size_t i = 0; i < stamps.size(); ++i) {
    REQUIRE(ring.TryPush(
        Event(stamps[i], static_cast<ActionId>(i), InputKind::kPress)));
  }
  REQUIRE(ring.TryPush(Event(4000, 40, InputKind::kPress)));
  InputFolder folder;
  // The tick (1-based) each action's press landed on.
  std::array<int, kMaxActions> landed{};
  for (std::size_t k = 0; k < deadlines.size(); ++k) {
    const InputFrame frame =
        folder.Latch(static_cast<Tick>(k + 1), deadlines[k], ring);
    for (std::uint8_t e = 0; e < frame.edge_count; ++e) {
      landed[frame.edges[e].action] = static_cast<int>(k + 1);
    }
  }
  for (std::size_t i = 0; i < stamps.size(); ++i) {
    INFO("stamp " << stamps[i]);
    CHECK(landed[i] == PlaceEdge(stamps[i], deadlines) + 1);
  }
  // The stamp at the last deadline is the next tick's.
  REQUIRE(landed[40] == 0);
  REQUIRE(ring.Front() != nullptr);
  REQUIRE(ring.Front()->stamp_ns == 4000);
}

TEST_CASE("InputFolder counts the inputs holding an action, like InputService",
          "[input_folder]") {
  // Two keys bound to action 4 (a chord): the second press and the first
  // release change nothing; the last release is the edge.
  InputEventRing ring(64);
  InputFolder folder;
  REQUIRE(ring.TryPush(Event(10, 4, InputKind::kPress)));
  REQUIRE(ring.TryPush(Event(20, 4, InputKind::kPress)));
  InputFrame frame = folder.Latch(1, 100, ring);
  REQUIRE(frame.pressed[4] == 1);
  REQUIRE(frame.edge_count == 1);
  CHECK(frame.Held(4));

  REQUIRE(ring.TryPush(Event(110, 4, InputKind::kRelease)));
  frame = folder.Latch(2, 200, ring);
  CHECK(frame.Held(4));
  REQUIRE(frame.released[4] == 0);
  REQUIRE(frame.edge_count == 0);

  REQUIRE(ring.TryPush(Event(210, 4, InputKind::kRelease)));
  // A release of an action nobody holds is ignored.
  REQUIRE(ring.TryPush(Event(220, 4, InputKind::kRelease)));
  frame = folder.Latch(3, 300, ring);
  CHECK_FALSE(frame.Held(4));
  REQUIRE(frame.released[4] == 1);
  REQUIRE(frame.edge_count == 1);
  REQUIRE(folder.Stats().stray_releases == 1);

  // ReleaseAll drops every count: the next press is an edge again.
  REQUIRE(ring.TryPush(Event(310, 4, InputKind::kPress)));
  REQUIRE(ring.TryPush(Event(311, 4, InputKind::kPress)));
  REQUIRE(ring.TryPush(Event(320, 0, InputKind::kReleaseAll)));
  REQUIRE(ring.TryPush(Event(330, 4, InputKind::kPress)));
  frame = folder.Latch(4, 400, ring);
  REQUIRE(frame.pressed[4] == 2);
  REQUIRE(frame.released[4] == 1);
  CHECK(frame.Held(4));
  REQUIRE(ring.TryPush(Event(410, 4, InputKind::kRelease)));
  frame = folder.Latch(5, 500, ring);
  CHECK_FALSE(frame.Held(4));
}

TEST_CASE("InputFolder frames equal the inline placement on the same windows",
          "[input_folder]") {
  // The deadlines of a 120 Hz session whose first tick's window ends at
  // StampedInput's grid point.
  ManualClock manual;
  PacedClock clock(manual, PacedClockConfig{});
  constexpr std::uint64_t kEpoch = 1'000'000'000;
  clock.Start(1, kEpoch + clock.PeriodNs());
  constexpr Tick kTicks = 2400;

  // Inline: StampedInput latched with window ends at the deadlines.
  testing::StampedInput stamped(17, kEpoch);
  std::vector<InputFrame> inline_frames;
  std::vector<InputEvent> events;
  for (Tick k = 1; k <= kTicks; ++k) {
    const InputFrame frame = stamped.Latch(k, clock.DeadlineNs(k), false);
    for (std::uint8_t e = 0; e < frame.edge_count; ++e) {
      const InputEdge& edge = frame.edges[e];
      events.push_back(
          Event(edge.stamp_ns, edge.action,
                edge.down ? InputKind::kPress : InputKind::kRelease));
    }
    inline_frames.push_back(frame);
  }
  REQUIRE(events.size() >= 150);

  // Threaded rule: every event in the ring up front, latched against the
  // same deadlines; the folder must stop at the right ones.
  InputEventRing ring(4096);
  for (const InputEvent& event : events) REQUIRE(ring.TryPush(event));
  InputFolder folder;
  for (Tick k = 1; k <= kTicks; ++k) {
    INFO("tick " << k);
    InputFrame expected = inline_frames[k - 1];
    expected.platform_live = true;
    REQUIRE(folder.Latch(k, clock.DeadlineNs(k), ring) == expected);
  }
  REQUIRE(ring.Front() == nullptr);
  CHECK(folder.Stats().events == events.size());
}

TEST_CASE("InputFolder latches a catch-up burst tick by tick",
          "[input_folder]") {
  InputEventRing ring(64);
  InputFolder folder;
  // One press per tick window, all in the ring before the burst runs.
  for (std::uint64_t k = 1; k <= 4; ++k) {
    REQUIRE(ring.TryPush(
        Event(k * 1000 - 500, static_cast<ActionId>(k), InputKind::kPress)));
  }
  for (Tick k = 1; k <= 4; ++k) {
    const InputFrame frame = folder.Latch(k, k * 1000, ring);
    REQUIRE(frame.edge_count == 1);
    CHECK(frame.pressed[static_cast<std::size_t>(k)] == 1);
  }
}

TEST_CASE("InputFolder folds axes, release-all, counts and edge overflow",
          "[input_folder]") {
  InputEventRing ring(1024);
  InputFolder folder;
  REQUIRE(ring.TryPush(Event(1, 0, InputKind::kAxis, 0.25F)));
  REQUIRE(ring.TryPush(Event(2, 0, InputKind::kAxis, -0.75F)));
  REQUIRE(ring.TryPush(Event(3, 9, InputKind::kAxis, 1.0F)));  // no axis 9
  REQUIRE(ring.TryPush(Event(4, 70, InputKind::kPress)));      // no action 70
  REQUIRE(ring.TryPush(Event(5, 5, InputKind::kPress)));
  REQUIRE(ring.TryPush(Event(5, 6, InputKind::kPress)));
  REQUIRE(ring.TryPush(Event(6, 0, InputKind::kHeartbeat)));
  InputFrame frame = folder.Latch(1, 10, ring);
  CHECK(frame.axes[0] == -0.75F);
  CHECK(frame.Held(5));
  CHECK(frame.Held(6));
  // Same stamp: ordered.
  REQUIRE(frame.edges[0].order == 0);
  REQUIRE(frame.edges[1].order == 1);
  REQUIRE(folder.Stats().ignored == 2);
  REQUIRE(folder.Stats().heartbeats == 1);

  REQUIRE(ring.TryPush(Event(20, 0, InputKind::kReleaseAll)));
  frame = folder.Latch(2, 21, ring);
  REQUIRE(frame.held == 0);
  REQUIRE(frame.released[5] == 1);
  REQUIRE(frame.released[6] == 1);
  CHECK(frame.axes[0] == -0.75F);

  // 300 taps in one tick: counts saturate at 255, 32 edges kept.
  for (std::uint64_t i = 0; i < 150; ++i) {
    REQUIRE(ring.TryPush(Event(30 + i, 1, InputKind::kPress)));
    REQUIRE(ring.TryPush(Event(30 + i, 1, InputKind::kRelease)));
  }
  frame = folder.Latch(3, 1000, ring);
  REQUIRE(frame.pressed[1] == 150);
  CHECK(frame.edge_count == kMaxEdgesPerTick);
  CHECK(folder.Stats().edges_dropped == 300 - kMaxEdgesPerTick);
  for (std::uint64_t i = 0; i < 300; ++i) {
    REQUIRE(ring.TryPush(Event(2000 + 2 * i, 2, InputKind::kPress)));
    REQUIRE(ring.TryPush(Event(2001 + 2 * i, 2, InputKind::kRelease)));
  }
  frame = folder.Latch(4, 5000, ring);
  REQUIRE(frame.pressed[2] == 255);
  REQUIRE(frame.released[2] == 255);

  folder.Reset();
  REQUIRE(folder.Held() == 0);
}

}  // namespace
}  // namespace se
