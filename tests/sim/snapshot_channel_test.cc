// Copyright 2026 The simple-engine-2 Authors.
//
// SnapshotChannel (docs/milestone-specs.md M17a): the pool hands out each
// slot to one side at a time, and seeded random producer and consumer
// schedules (2000 runs, run under TSan in linux-clang-tsan) show that every
// consumed slot is complete, no slot is written while the platform holds
// it, reliable sections are never lost or reordered, and dropped publishes
// are counted.

#include "se_core/snapshot_channel.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <deque>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include "se_core/snapshot.h"

namespace se {
namespace {

TEST_CASE("SnapshotChannel: one owner per slot, returned slots come back",
          "[snapshot_channel]") {
  SnapshotChannel channel(8);
  REQUIRE(channel.SlotCount() == 8);
  std::vector<Snapshot*> slots;
  for (int i = 0; i < 8; ++i) {
    Snapshot* slot = channel.AcquireForWrite();
    REQUIRE(slot != nullptr);
    slots.push_back(slot);
  }
  // The sim never waits: no free slot is nullptr.
  REQUIRE(channel.AcquireForWrite() == nullptr);

  slots[3]->header.tick = 3;
  channel.Publish(slots[3]);
  slots[5]->header.tick = 5;
  channel.Publish(slots[5]);
  channel.Discard(slots[0]);
  REQUIRE(channel.SimFreeCount() == 1);

  std::vector<SlotLease> held;
  const std::uint32_t received = channel.Receive(
      [&](SlotLease lease) { held.push_back(std::move(lease)); });
  REQUIRE(received == 2);
  // In publish order.
  REQUIRE(held[0]->header.tick == 3);
  REQUIRE(held[1]->header.tick == 5);
  // The discarded slot is free; the held ones are not.
  REQUIRE(channel.AcquireForWrite() == slots[0]);
  REQUIRE(channel.AcquireForWrite() == nullptr);
  held[0].Reset();
  REQUIRE(channel.AcquireForWrite() == slots[3]);
  held.clear();
  REQUIRE(channel.AcquireForWrite() == slots[5]);
}

// The item a producer writes at index i of tick t.
RenderItem ItemAt(Tick t, std::uint32_t i) {
  RenderItem item;
  item.entity = static_cast<std::uint32_t>(t * 1000 + i);
  item.spawn_serial = i;
  item.pos = {static_cast<float>(t), static_cast<float>(i)};
  item.texture = static_cast<std::uint32_t>(t ^ i);
  return item;
}

bool Same(const RenderItem& a, const RenderItem& b) {
  return a.entity == b.entity && a.spawn_serial == b.spawn_serial &&
         a.pos == b.pos && a.texture == b.texture;
}

// A short random pause: a spin of up to |max| steps, sometimes a yield.
void Jitter(std::mt19937& rng, std::uint32_t max) {
  static std::atomic<std::uint32_t> sink{0};
  const std::uint32_t r = rng();
  if ((r & 7U) == 0) {
    std::this_thread::yield();
    return;
  }
  const std::uint32_t n = max == 0 ? 0 : (r >> 3) % max;
  for (std::uint32_t i = 0; i < n; ++i) {
    sink.fetch_add(1, std::memory_order_relaxed);
  }
}

struct RunResult {
  std::uint64_t consumed = 0;
  std::uint64_t dropped = 0;
  std::uint64_t incomplete = 0;
  std::uint64_t written_while_held = 0;
  std::uint64_t reliable_out_of_order = 0;
  std::uint64_t drop_count_wrong = 0;
  std::uint64_t reliable_delivered = 0;
  std::uint64_t reliable_made = 0;
};

RunResult RunSchedule(std::uint32_t seed) {
  constexpr Tick kTicks = 64;
  SnapshotChannel channel(8);
  std::array<std::atomic<bool>, 8> held{};
  RunResult result;
  std::atomic<bool> producer_done{false};
  bool producer_threw = false;
  std::uint64_t written_while_held = 0;
  std::uint64_t made = 0;
  std::uint64_t dropped = 0;
  ReliableDeltas leftover;

  std::thread producer([&] {
    try {
      std::mt19937 rng(seed * 2 + 1);
      ReliableDeltas pending;
      std::uint64_t next_seq = 0;
      std::uint32_t drops = 0;
      for (Tick t = 1; t <= kTicks; ++t) {
        // 0 to 2 reliable entries per tick.
        for (std::uint32_t n = rng() % 3; n > 0; --n) {
          AudioCue cue;
          cue.tick = t;
          cue.sound = next_seq++;
          pending.audio.push_back(cue);
        }
        Jitter(rng, 400);
        Snapshot* slot = channel.AcquireForWrite();
        if (slot == nullptr) {
          ++drops;
          continue;
        }
        const std::uint32_t index = channel.IndexOf(slot);
        if (held[index].load(std::memory_order_relaxed)) ++written_while_held;
        slot->ClearRender();
        slot->header.tick = t;
        slot->header.dropped_publishes = drops;
        const std::uint32_t items = 1 + rng() % 40;
        slot->header.world_hash = items;
        for (std::uint32_t i = 0; i < items; ++i) {
          slot->items.push_back(ItemAt(t, i));
          if (i == items / 2) Jitter(rng, 200);
        }
        if (held[index].load(std::memory_order_relaxed)) ++written_while_held;
        slot->reliable.Clear();
        std::swap(slot->reliable, pending);
        channel.Publish(slot);
      }
      made = next_seq;
      dropped = drops;
      leftover = std::move(pending);
      producer_done.store(true, std::memory_order_release);
    } catch (...) {
      producer_threw = true;
      producer_done.store(true, std::memory_order_release);
    }
  });

  std::mt19937 rng(seed * 2);
  std::deque<SlotLease> holding;
  std::uint64_t next_reliable = 0;
  std::uint64_t received = 0;
  Tick last_tick = 0;
  const auto check_reliable = [&](const ReliableDeltas& deltas) {
    for (const AudioCue& cue : deltas.audio) {
      if (cue.sound != next_reliable) ++result.reliable_out_of_order;
      next_reliable = cue.sound + 1;
      ++result.reliable_delivered;
    }
  };
  const auto consume = [&](SlotLease lease) {
    const Snapshot& slot = *lease;
    held[lease.Index()].store(true, std::memory_order_relaxed);
    ++received;
    const auto items = static_cast<std::uint32_t>(slot.header.world_hash);
    bool complete = slot.header.tick > last_tick && slot.items.size() == items;
    for (std::uint32_t i = 0; complete && i < items; ++i) {
      complete = Same(slot.items[i], ItemAt(slot.header.tick, i));
    }
    if (!complete) ++result.incomplete;
    last_tick = slot.header.tick;
    // Every tick so far either reached the platform or was dropped.
    if (slot.header.dropped_publishes != slot.header.tick - received) {
      ++result.drop_count_wrong;
    }
    check_reliable(slot.reliable);
    holding.push_back(std::move(lease));
  };
  for (;;) {
    const bool done = producer_done.load(std::memory_order_acquire);
    channel.Receive(consume);
    // Hold 0 to 8 slots (all of them now and then, to force drops).
    const std::size_t keep = rng() % 9;
    while (holding.size() > keep) {
      // After the last read: flag it free, then return it.
      held[holding.front().Index()].store(false, std::memory_order_relaxed);
      holding.pop_front();
    }
    if (done) break;
    Jitter(rng, 600);
  }
  producer.join();
  REQUIRE_FALSE(producer_threw);
  channel.Receive(consume);
  for (SlotLease& lease : holding) {
    held[lease.Index()].store(false, std::memory_order_relaxed);
  }
  holding.clear();
  // What the producer could not publish at its end is handed over after the
  // join, as SimThread::TakeUnpublished does.
  check_reliable(leftover);
  result.consumed = received;
  result.dropped = dropped;
  result.written_while_held = written_while_held;
  result.reliable_made = made;
  return result;
}

TEST_CASE("SnapshotChannel property: random schedules keep every contract",
          "[snapshot_channel][threads]") {
  constexpr std::uint32_t kRuns = 2000;
  std::uint64_t consumed = 0;
  std::uint64_t dropped = 0;
  std::uint64_t reliable = 0;
  for (std::uint32_t seed = 0; seed < kRuns; ++seed) {
    const RunResult run = RunSchedule(seed);
    INFO("seed " << seed);
    REQUIRE(run.incomplete == 0);
    REQUIRE(run.written_while_held == 0);
    REQUIRE(run.reliable_out_of_order == 0);
    REQUIRE(run.reliable_delivered == run.reliable_made);
    REQUIRE(run.drop_count_wrong == 0);
    REQUIRE(run.consumed + run.dropped == 64);
    consumed += run.consumed;
    dropped += run.dropped;
    reliable += run.reliable_delivered;
  }
  // The schedules really vary: both outcomes happen.
  REQUIRE(consumed > 0);
  REQUIRE(dropped > 0);
  REQUIRE(reliable > 0);
}

}  // namespace
}  // namespace se
