// Copyright 2026 The simple-engine-2 Authors.
//
// SnapshotChannel: the sim-to-platform handoff of snapshot slots (docs/
// design/dedicated-driver.md "Snapshot And Interpolation", PUBLISH AND
// MEMORY ORDERS; ADR-085). A pool of slots (8 by default) and two SPSC rings
// of slot indices, 'published' (sim to platform) and 'returned' (platform to
// sim), 16 entries each, so neither can fill:
//   - the sim writes a slot's body, then pushes its index on 'published'
//     (element write, then head.store(release));
//   - the platform pops it (head.load(acquire)), reads the slot, and
//     returns the index after its last read (tail and head stores with
//     release);
//   - the sim reclaims returned indices with an acquire load before it
//     reuses a slot.
// That is one happens-before edge in each direction, with no lock and no
// read-modify-write on the indices. Exactly one side owns a slot at any
// moment, and the platform holds it const through a SlotLease, whose
// destructor returns it. The sim never waits: AcquireForWrite returns
// nullptr when no slot is free, and the sim then skips the render part of
// that publish (keeping its reliable deltas for the next one).
//
// Debug builds track each slot's owner (free, writing, published, held,
// returned) and assert every transition, the design's owner checks on the
// channel's endpoints.

#ifndef SE_CORE_SNAPSHOT_CHANNEL_H_
#define SE_CORE_SNAPSHOT_CHANNEL_H_

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "se_core/assert.h"
#include "se_core/snapshot.h"
#include "se_core/spsc_ring.h"

namespace se {

class SnapshotChannel;

// The platform's hold on one received slot: const access, returned when the
// lease is destroyed or reset. Move-only.
class SlotLease {
 public:
  SlotLease() noexcept = default;
  SlotLease(SlotLease&& other) noexcept
      : channel_(std::exchange(other.channel_, nullptr)),
        index_(other.index_) {}
  SlotLease& operator=(SlotLease&& other) noexcept {
    if (this != &other) {
      Reset();
      channel_ = std::exchange(other.channel_, nullptr);
      index_ = other.index_;
    }
    return *this;
  }
  SlotLease(const SlotLease&) = delete;
  SlotLease& operator=(const SlotLease&) = delete;
  ~SlotLease() { Reset(); }

  [[nodiscard]] explicit operator bool() const noexcept {
    return channel_ != nullptr;
  }
  [[nodiscard]] const Snapshot& operator*() const noexcept { return *Get(); }
  [[nodiscard]] const Snapshot* operator->() const noexcept { return Get(); }
  [[nodiscard]] const Snapshot* Get() const noexcept;
  [[nodiscard]] std::uint32_t Index() const noexcept { return index_; }

  // Returns the slot now (after the holder's last read).
  void Reset() noexcept;

 private:
  friend class SnapshotChannel;
  SlotLease(SnapshotChannel* channel, std::uint32_t index) noexcept
      : channel_(channel), index_(index) {}

  SnapshotChannel* channel_ = nullptr;
  std::uint32_t index_ = 0;
};

class SnapshotChannel {
 public:
  // Capacity of each index ring; the pool may not be larger.
  static constexpr std::uint32_t kRingCapacity = 16;

  explicit SnapshotChannel(std::uint32_t slots = 8);
  SnapshotChannel(const SnapshotChannel&) = delete;
  SnapshotChannel& operator=(const SnapshotChannel&) = delete;
  // Every lease must be gone, and the sim must hold no slot.
  ~SnapshotChannel() = default;

  // Sim side (the one producer thread).
  // A free slot to fill, after reclaiming the returned ones; nullptr when
  // none is free (skip the render publish, keep the reliable deltas).
  [[nodiscard]] Snapshot* AcquireForWrite() noexcept;
  // Hands a filled slot to the platform.
  void Publish(Snapshot* slot) noexcept;
  // Gives an acquired slot back unpublished.
  void Discard(Snapshot* slot) noexcept;
  // Slots the sim could write now without reclaiming (sim side).
  [[nodiscard]] std::uint32_t SimFreeCount() const noexcept {
    return sim_free_count_;
  }

  // Platform side (the one consumer thread). Calls on_slot(SlotLease) for
  // every published slot, in publish order (tick order); returns how many.
  template <class F>
    requires std::is_invocable_v<F&, SlotLease>
  std::uint32_t Receive(F&& on_slot) {
    auto&& visit = std::forward<F>(on_slot);
    std::uint32_t received = 0;
    published_.PopAll([&](std::uint32_t index) {
      Transition(index, kPublished, kHeld);
      ++received;
      visit(SlotLease(this, index));
    });
    return received;
  }

  [[nodiscard]] std::uint32_t SlotCount() const noexcept {
    return static_cast<std::uint32_t>(slots_.size());
  }
  // The slot's index in the pool.
  [[nodiscard]] std::uint32_t IndexOf(const Snapshot* slot) const noexcept;
  // Read-only access by index, for tests and diagnostics.
  [[nodiscard]] const Snapshot& Slot(std::uint32_t index) const noexcept {
    return slots_[index];
  }

 private:
  friend class SlotLease;

  enum Owner : std::uint8_t {
    kFree,
    kWriting,
    kPublished,
    kHeld,
    kReturned,
  };

  // Platform: pushes |index| on 'returned' (release).
  void Return(std::uint32_t index) noexcept;
  void Transition(std::uint32_t index, Owner from, Owner to) noexcept;

  std::vector<Snapshot> slots_;
  std::unique_ptr<std::atomic<std::uint8_t>[]> owners_;
  SpscRing<std::uint32_t> published_{kRingCapacity};
  SpscRing<std::uint32_t> returned_{kRingCapacity};
  // Sim-private: a stack of free slot indices (no allocation).
  std::array<std::uint32_t, kRingCapacity> sim_free_{};
  std::uint32_t sim_free_count_ = 0;
};

inline const Snapshot* SlotLease::Get() const noexcept {
  return channel_ == nullptr ? nullptr : &channel_->slots_[index_];
}

inline void SlotLease::Reset() noexcept {
  if (channel_ != nullptr) {
    std::exchange(channel_, nullptr)->Return(index_);
  }
}

}  // namespace se

#endif  // SE_CORE_SNAPSHOT_CHANNEL_H_
