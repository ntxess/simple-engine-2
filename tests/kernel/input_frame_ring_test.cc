// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/input_frame_ring.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#include "se_core/dump_stream.h"
#include "se_core/input_frame.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "support/kernel_session.h"
#include "support/test_roles.h"

namespace se {
namespace {

// Frame |n| with every field derived from n, so a torn copy shows.
InputFrame Expected(std::uint64_t n) {
  InputFrame frame;
  frame.tick = n + 1;
  frame.held = n * 0x9E3779B97F4A7C15ULL;
  for (std::size_t i = 0; i < frame.pressed.size(); ++i) {
    frame.pressed[i] = static_cast<std::uint8_t>(n + i);
    frame.released[i] = static_cast<std::uint8_t>(3 * n + i);
  }
  for (std::size_t i = 0; i < frame.axes.size(); ++i) {
    frame.axes[i] = static_cast<float>(n % 1000) + static_cast<float>(i);
  }
  frame.edge_count = static_cast<std::uint8_t>(n % 33);
  for (std::size_t i = 0; i < frame.edges.size(); ++i) {
    frame.edges[i].action = static_cast<ActionId>(i);
    frame.edges[i].down = (n + i) % 2 == 0;
    frame.edges[i].stamp_ns = n * 1000 + i;
    frame.edges[i].order = static_cast<std::uint8_t>(i);
  }
  frame.platform_live = n % 2 == 1;
  return frame;
}

// The tests stand in for the World owner.
void WriteAsOwner(InputFrameRing& ring,
                  const InputFrame& frame) SE_NO_THREAD_SAFETY_ANALYSIS {
  ring.Write(frame);
}

TEST_CASE("A reader thread never accepts a torn InputFrame slot",
          "[input_frame_ring][threads]") {
  auto ring = std::make_unique<InputFrameRing>();
  constexpr std::uint64_t kFrames = 30'000;
  std::atomic<bool> reading{false};
  std::atomic<bool> done{false};
  std::uint64_t accepted = 0;
  std::uint64_t rejected = 0;
  bool all_match = true;
  std::thread reader([&] {
    std::uint64_t probe = 0;
    InputFrame frame;
    reading.store(true, std::memory_order_release);
    // One more pass after the writer is done, so the stable ring is read
    // even when the writer finished before this thread got a core.
    for (bool finished = false; !finished;) {
      finished = done.load(std::memory_order_acquire);
      const std::uint64_t head = ring->Head();
      if (head == 0) continue;
      // The newest frame, and one further back (possibly overwritten).
      for (const std::uint64_t index :
           {head - 1,
            head > 7000 ? head - 1 - (probe++ % 7000) : std::uint64_t{0}}) {
        switch (ring->Read(index, frame)) {
          case InputFrameRead::kOk:
            ++accepted;
            all_match = all_match && frame == Expected(index);
            break;
          case InputFrameRead::kTorn:
          case InputFrameRead::kOverwritten:
            ++rejected;
            break;
          case InputFrameRead::kNotWritten:
            all_match = false;  // below the published head
            break;
        }
      }
    }
  });
  while (!reading.load(std::memory_order_acquire)) std::this_thread::yield();
  for (std::uint64_t n = 0; n < kFrames; ++n) WriteAsOwner(*ring, Expected(n));
  done.store(true, std::memory_order_release);
  reader.join();
  REQUIRE(all_match);
  REQUIRE(accepted > 0);
  REQUIRE(ring->Head() == kFrames);
  // Every frame still in the ring reads back intact.
  InputFrame frame;
  for (std::uint64_t n = kFrames - kInputFrameRingCapacity; n < kFrames; ++n) {
    REQUIRE(ring->Read(n, frame) == InputFrameRead::kOk);
    REQUIRE(frame == Expected(n));
  }
  static_cast<void>(rejected);
}

TEST_CASE("InputFrameRing reports frames not written and overwritten",
          "[input_frame_ring]") {
  auto ring = std::make_unique<InputFrameRing>();
  InputFrame frame;
  REQUIRE(ring->Read(0, frame) == InputFrameRead::kNotWritten);
  for (std::uint64_t n = 0; n < kInputFrameRingCapacity + 5; ++n) {
    WriteAsOwner(*ring, Expected(n));
  }
  REQUIRE(ring->Read(4, frame) == InputFrameRead::kOverwritten);
  REQUIRE(ring->Read(5, frame) == InputFrameRead::kOk);
  REQUIRE(frame == Expected(5));
  REQUIRE(ring->Read(kInputFrameRingCapacity + 5, frame) ==
          InputFrameRead::kNotWritten);
}

TEST_CASE("The InputFrame ring is one kind-3 dump stream",
          "[input_frame_ring][dump_stream]") {
  auto ring = std::make_unique<InputFrameRing>();
  for (std::uint64_t n = 0; n < 10; ++n) WriteAsOwner(*ring, Expected(n));
  const std::span<const std::byte> bytes = ring->StreamBytes();
  REQUIRE(bytes.size() ==
          sizeof(DumpStreamHeader) +
              std::size_t{kInputFrameRingCapacity} * sizeof(InputFrameSlot));
  DumpStreamHeader header;
  std::memcpy(&header, bytes.data(), sizeof(header));
  CHECK(header.magic == kDumpStreamMagic);
  CHECK(header.kind ==
        static_cast<std::uint16_t>(DumpStreamKind::kInputFrameRing));
  CHECK(header.record_size == sizeof(InputFrameSlot));
  CHECK(header.capacity == kInputFrameRingCapacity);
  REQUIRE(header.head == 10);
  // Slot 9 holds frame 9, complete: stamp 2 * 9 + 2.
  InputFrameSlot slot;
  std::memcpy(&slot, bytes.data() + sizeof(DumpStreamHeader) + 9 * sizeof(slot),
              sizeof(slot));
  REQUIRE(slot.stamp == 20);
  CHECK(slot.frame == Expected(9));
}

TEST_CASE("SimDriver writes every latched frame to an attached ring",
          "[input_frame_ring][sim_driver]") {
  const testing::MainThreadScope main_thread;
  auto ring = std::make_unique<InputFrameRing>();
  testing::KernelSessionOptions options;
  options.hz = 60.0;
  options.ticks = 300;
  const testing::KernelSessionResult plain = testing::RunKernelSession(options);
  options.input_ring = ring.get();
  const testing::KernelSessionResult ringed =
      testing::RunKernelSession(options);
  REQUIRE(plain.error.empty());
  REQUIRE(ringed.error.empty());
  // Nothing else changes when a ring is attached.
  REQUIRE(ringed.hashes == plain.hashes);
  REQUIRE(ring->Head() == 300);
  InputFrame frame;
  for (std::uint64_t n = 0; n < 300; ++n) {
    REQUIRE(ring->Read(n, frame) == InputFrameRead::kOk);
    REQUIRE(frame.tick == n + 1);
  }
}

}  // namespace
}  // namespace se
