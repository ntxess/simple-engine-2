// Copyright 2026 The simple-engine-2 Authors.
//
// InputFolder: InputFrame k holds every event stamped at or before the
// sim's deadline L_k, which is the inline driver's placement rule with the
// deadlines as tick windows (the frames equal StampedInput's, latched by the
// same window ends); later events wait at the ring's front; the fold keeps
// held state, counts every press and release, keeps axes, and releases all.

#include "se_sim/input_folder.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

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

TEST_CASE("InputFolder places each event on the first deadline at or after it",
          "[input_folder]") {
  InputEventRing ring(64);
  InputFolder folder;
  REQUIRE(ring.TryPush(Event(100, 1, InputKind::kPress)));
  REQUIRE(ring.TryPush(Event(200, 1, InputKind::kRelease)));
  REQUIRE(ring.TryPush(Event(201, 2, InputKind::kPress)));
  REQUIRE(ring.TryPush(Event(450, 3, InputKind::kPress)));

  // L_1 = 200: the tap at 100..200 counts, held clear again.
  const InputFrame one = folder.Latch(1, 200, ring);
  REQUIRE(one.tick == 1);
  REQUIRE(one.pressed[1] == 1);
  REQUIRE(one.released[1] == 1);
  CHECK_FALSE(one.Held(1));
  REQUIRE(one.edge_count == 2);
  CHECK(one.platform_live);
  // 201 waits at the front.
  REQUIRE(ring.Front() != nullptr);
  REQUIRE(ring.Front()->stamp_ns == 201);

  const InputFrame two = folder.Latch(2, 400, ring);
  REQUIRE(two.pressed[2] == 1);
  CHECK(two.Held(2));
  REQUIRE(two.edge_count == 1);
  // Held stays without new edges.
  const InputFrame three = folder.Latch(3, 449, ring);
  CHECK(three.Held(2));
  REQUIRE(three.edge_count == 0);
  const InputFrame four = folder.Latch(4, 450, ring);
  CHECK(four.Held(3));
  CHECK(ring.Front() == nullptr);
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
  frame = folder.Latch(2, 20, ring);
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
    REQUIRE(ring.TryPush(Event(2000 + i, 2, InputKind::kPress)));
  }
  frame = folder.Latch(4, 5000, ring);
  REQUIRE(frame.pressed[2] == 255);

  folder.Reset();
  REQUIRE(folder.Held() == 0);
}

}  // namespace
}  // namespace se
