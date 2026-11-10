// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/snapshot_channel.h"

#include <atomic>
#include <cstdint>
#include <memory>

#include "se_core/assert.h"
#include "se_core/snapshot.h"

namespace se {

SnapshotChannel::SnapshotChannel(std::uint32_t slots)
    : slots_(slots),
      owners_(std::make_unique<std::atomic<std::uint8_t>[]>(slots)) {
  SE_ASSERT_MSG(slots >= 1 && slots <= kRingCapacity,
                "a SnapshotChannel has 1 to 16 slots");
  // Handed out from the top: slot 0 first.
  for (std::uint32_t i = slots; i > 0; --i) {
    owners_[i - 1].store(kFree, std::memory_order_relaxed);
    sim_free_[sim_free_count_++] = i - 1;
  }
}

Snapshot* SnapshotChannel::AcquireForWrite() noexcept {
  returned_.PopAll([this](std::uint32_t index) {
    Transition(index, kReturned, kFree);
    sim_free_[sim_free_count_++] = index;
  });
  if (sim_free_count_ == 0) return nullptr;
  const std::uint32_t index = sim_free_[--sim_free_count_];
  Transition(index, kFree, kWriting);
  return &slots_[index];
}

void SnapshotChannel::Publish(Snapshot* slot) noexcept {
  const std::uint32_t index = IndexOf(slot);
  Transition(index, kWriting, kPublished);
  // 16 entries for at most 16 slots: the ring cannot be full.
  const bool pushed = published_.TryPush(index);
  SE_ASSERT_MSG(pushed, "SnapshotChannel's published ring is full");
  static_cast<void>(pushed);
}

void SnapshotChannel::Discard(Snapshot* slot) noexcept {
  const std::uint32_t index = IndexOf(slot);
  Transition(index, kWriting, kFree);
  sim_free_[sim_free_count_++] = index;
}

std::uint32_t SnapshotChannel::IndexOf(const Snapshot* slot) const noexcept {
  SE_ASSERT_MSG(slot >= slots_.data() && slot < slots_.data() + slots_.size(),
                "not a slot of this SnapshotChannel");
  return static_cast<std::uint32_t>(slot - slots_.data());
}

void SnapshotChannel::Return(std::uint32_t index) noexcept {
  Transition(index, kHeld, kReturned);
  const bool pushed = returned_.TryPush(index);
  SE_ASSERT_MSG(pushed, "SnapshotChannel's returned ring is full");
  static_cast<void>(pushed);
}

void SnapshotChannel::Transition(std::uint32_t index, Owner from,
                                 Owner to) noexcept {
#if SE_ASSERTS
  // Relaxed: the rings order the handovers; this only checks them.
  const std::uint8_t was = owners_[index].exchange(
      static_cast<std::uint8_t>(to), std::memory_order_relaxed);
  SE_ASSERT_MSG(was == from, "a snapshot slot changed hands out of order");
#else
  static_cast<void>(index);
  static_cast<void>(from);
  static_cast<void>(to);
#endif
}

}  // namespace se
