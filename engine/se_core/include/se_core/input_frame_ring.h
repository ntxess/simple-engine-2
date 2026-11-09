// Copyright 2026 The simple-engine-2 Authors.
//
// InputFrameRing: the last 7200 InputFrames (60 s at 120 Hz), laid out as a
// kind-3 dump stream (se_core/dump_stream.h): the header, whose `head` is
// the published head, then 7200 InputFrameSlots, in one allocation. Only
// the World owner writes (SimDriver writes every latched frame when a ring
// is attached); the dump code and the watchdog read it from other threads.
//
// Each slot carries a seqlock stamp: 2n + 1 while frame n (the n-th frame
// written, from 0) is being written into it, 2n + 2 once it is complete. A
// reader copies a slot between two loads of its stamp and rejects the copy
// unless both say "frame n, complete", so it never accepts a torn slot.
// Slots are copied word by word with relaxed atomics, so the race a seqlock
// tolerates is not a data race (and TSan agrees).
//
// A ring registered as a dump stream must never be destroyed: the handler
// may read it until the process exits.

#ifndef SE_CORE_INPUT_FRAME_RING_H_
#define SE_CORE_INPUT_FRAME_RING_H_

#include <cstddef>
#include <cstdint>
#include <span>

#include "se_core/dump_stream.h"
#include "se_core/input_frame.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"

namespace se {

inline constexpr std::uint32_t kInputFrameRingCapacity = 7200;

enum class InputFrameRead : std::uint8_t {
  kOk,
  // Frame n is being written (or was overwritten while it was copied).
  kTorn,
  // The slot already holds a later frame.
  kOverwritten,
  // Frame n has not been written yet.
  kNotWritten,
};

class InputFrameRing {
 public:
  InputFrameRing();
  ~InputFrameRing();

  InputFrameRing(const InputFrameRing&) = delete;
  InputFrameRing& operator=(const InputFrameRing&) = delete;

  // Appends |frame| (the World owner only).
  void Write(const InputFrame& frame) noexcept SE_REQUIRES(g_sim_owner);

  // Frames published so far; frame Head() - 1 is the newest.
  [[nodiscard]] std::uint64_t Head() const noexcept;

  // Copies frame |index| (0 = the first frame written) into |out|. Any
  // thread.
  [[nodiscard]] InputFrameRead Read(std::uint64_t index,
                                    InputFrame& out) const noexcept;

  [[nodiscard]] static constexpr std::uint32_t Capacity() noexcept {
    return kInputFrameRingCapacity;
  }

  // The dump-stream region: the header and the slots.
  [[nodiscard]] std::span<const std::byte> StreamBytes() const noexcept;

 private:
  static constexpr std::size_t kSlotWords = sizeof(InputFrameSlot) / 8;
  static constexpr std::size_t kFrameWords = sizeof(InputFrame) / 8;
  static_assert(sizeof(InputFrameSlot) % 8 == 0);
  static_assert(sizeof(InputFrame) % 8 == 0);

  [[nodiscard]] std::uint64_t* SlotWords(std::uint64_t index) const noexcept;

  void* block_;
  DumpStreamHeader* header_;
  // The slots as 64-bit words: word 0 is the stamp, then the frame.
  std::uint64_t* slots_;
  // The OS thread id recorded in the header.
  std::uint64_t writer_os_id_ = 0;
};

}  // namespace se

#endif  // SE_CORE_INPUT_FRAME_RING_H_
