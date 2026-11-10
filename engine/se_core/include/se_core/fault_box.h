// Copyright 2026 The simple-engine-2 Authors.
//
// FaultBox: how a thread reports a contained fault to the platform thread
// (docs/design/dedicated-driver.md "Data Flow", Fault, and "Synchronization",
// item 6). The faulting thread first tears down its own state, then posts:
// it writes the exception_ptr, the breadcrumb and the replay, then stores the
// flag with release. The platform loads the flag with acquire after a wake
// (or on a poll) and only then reads the rest. The first post wins; later
// ones are only counted, so the record a reader sees never changes under it.
//
// One box per thread role that can fault (the sim's tick, M17a); the box
// outlives both the poster and the reader.

#ifndef SE_CORE_FAULT_BOX_H_
#define SE_CORE_FAULT_BOX_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/thread_role.h"
#include "se_core/types.h"

namespace se {

struct FaultRecord {
  ThreadRole role = ThreadRole::kLibrary;
  // The thread's packed breadcrumb (role, phase, system, chunk, tick) at the
  // fault (se_core/breadcrumb.h).
  std::uint64_t breadcrumb = 0;
  // The tick that faulted.
  Tick tick = 0;
  // The session the tick belonged to.
  std::uint64_t session = 0;
  // The exception's what(), cut to fit and NUL-terminated.
  std::array<char, 256> message{};

  [[nodiscard]] std::string_view Message() const noexcept {
    return std::string_view(message.data());
  }
  void SetMessage(std::string_view text) noexcept {
    const std::size_t n =
        text.size() < message.size() - 1 ? text.size() : message.size() - 1;
    for (std::size_t i = 0; i < n; ++i) message[i] = text[i];
    message[n] = '\0';
  }
};

class FaultBox {
 public:
  FaultBox() = default;
  FaultBox(const FaultBox&) = delete;
  FaultBox& operator=(const FaultBox&) = delete;

  // The faulting thread. Returns false (and only counts the post) when a
  // fault was posted before.
  bool Post(const std::exception_ptr& error, const FaultRecord& record,
            std::vector<std::byte> replay = {}) noexcept {
    posts_.fetch_add(1, std::memory_order_relaxed);
    if (claimed_.exchange(true, std::memory_order_acq_rel)) return false;
    error_ = error;
    record_ = record;
    replay_ = std::move(replay);
    posted_.store(true, std::memory_order_release);
    return true;
  }

  // Any thread: true once a fault was posted. The accessors below may be
  // called only after it returned true.
  [[nodiscard]] bool Posted() const noexcept {
    return posted_.load(std::memory_order_acquire);
  }
  [[nodiscard]] const std::exception_ptr& Exception() const noexcept {
    return error_;
  }
  [[nodiscard]] const FaultRecord& Record() const noexcept { return record_; }
  // The faulted session's replay up to the fault (empty when none was kept).
  [[nodiscard]] std::span<const std::byte> Replay() const noexcept {
    return replay_;
  }
  // Every Post call, the first included.
  [[nodiscard]] std::uint32_t Posts() const noexcept {
    return posts_.load(std::memory_order_relaxed);
  }

 private:
  std::atomic<bool> claimed_{false};
  std::exception_ptr error_;
  FaultRecord record_;
  std::vector<std::byte> replay_;
  std::atomic<bool> posted_{false};
  std::atomic<std::uint32_t> posts_{0};
};

}  // namespace se

#endif  // SE_CORE_FAULT_BOX_H_
