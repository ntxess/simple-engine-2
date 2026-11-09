// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/input_frame_ring.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <span>

#include "se_core/dump_stream.h"
#include "se_core/input_frame.h"
#include "se_core/thread_registry.h"
#include "se_core/thread_role.h"

namespace se {
namespace {

constexpr std::align_val_t kBlockAlignment{64};
constexpr std::size_t kBlockSize =
    sizeof(DumpStreamHeader) +
    std::size_t{kInputFrameRingCapacity} * sizeof(InputFrameSlot);

}  // namespace

InputFrameRing::InputFrameRing()
    : block_(::operator new(kBlockSize, kBlockAlignment)),
      header_(new (block_) DumpStreamHeader{}),
      slots_(reinterpret_cast<std::uint64_t*>(static_cast<std::byte*>(block_) +
                                              sizeof(DumpStreamHeader))) {
  std::memset(slots_, 0, kBlockSize - sizeof(DumpStreamHeader));
  InitDumpStreamHeader(*header_, DumpStreamKind::kInputFrameRing,
                       sizeof(InputFrameSlot), kInputFrameRingCapacity,
                       "input-frames");
}

InputFrameRing::~InputFrameRing() {
  ::operator delete(block_, kBlockAlignment);
}

std::uint64_t* InputFrameRing::SlotWords(std::uint64_t index) const noexcept {
  return slots_ + (index % kInputFrameRingCapacity) * kSlotWords;
}

void InputFrameRing::Write(const InputFrame& frame) noexcept {
  if (const std::uint64_t os_id = CurrentOsThreadId(); os_id != writer_os_id_) {
    writer_os_id_ = os_id;
    AtomicStore(header_->os_thread_id, os_id, std::memory_order_relaxed);
    AtomicStore(header_->role, static_cast<std::uint8_t>(CurrentRole()),
                std::memory_order_relaxed);
  }
  const std::uint64_t index =
      AtomicLoad(header_->head, std::memory_order_relaxed);
  std::uint64_t* slot = SlotWords(index);
  std::array<std::uint64_t, kFrameWords> words;
  std::memcpy(words.data(), &frame, sizeof(InputFrame));
  // Odd stamp first, then the words, then the even stamp (a seqlock).
  std::atomic_ref<std::uint64_t>(slot[0]).store(2 * index + 1,
                                                std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  for (std::size_t i = 0; i < kFrameWords; ++i) {
    std::atomic_ref<std::uint64_t>(slot[1 + i])
        .store(words[i], std::memory_order_relaxed);
  }
  std::atomic_ref<std::uint64_t>(slot[0]).store(2 * index + 2,
                                                std::memory_order_release);
  AtomicStore(header_->head, index + 1, std::memory_order_release);
}

std::uint64_t InputFrameRing::Head() const noexcept {
  return AtomicLoad(header_->head, std::memory_order_acquire);
}

InputFrameRead InputFrameRing::Read(std::uint64_t index,
                                    InputFrame& out) const noexcept {
  const std::uint64_t* slot = SlotWords(index);
  const std::uint64_t complete = 2 * index + 2;
  const std::uint64_t before = AtomicLoad(slot[0], std::memory_order_acquire);
  if (before != complete) {
    if (before == complete - 1) return InputFrameRead::kTorn;
    return before > complete ? InputFrameRead::kOverwritten
                             : InputFrameRead::kNotWritten;
  }
  std::array<std::uint64_t, kFrameWords> words;
  for (std::size_t i = 0; i < kFrameWords; ++i) {
    words[i] = AtomicLoad(slot[1 + i], std::memory_order_relaxed);
  }
  std::atomic_thread_fence(std::memory_order_acquire);
  if (AtomicLoad(slot[0], std::memory_order_relaxed) != before) {
    return InputFrameRead::kTorn;
  }
  std::memcpy(&out, words.data(), sizeof(InputFrame));
  return InputFrameRead::kOk;
}

std::span<const std::byte> InputFrameRing::StreamBytes() const noexcept {
  return {static_cast<const std::byte*>(block_), kBlockSize};
}

}  // namespace se
