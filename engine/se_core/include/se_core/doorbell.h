// Copyright 2026 The simple-engine-2 Authors.
//
// Doorbell: a wake-up for a thread that waits for work (docs/design/
// dedicated-driver.md "Synchronization", item 2). It is a monotonically
// increasing sequence number, never a flag that toggles back:
//   - Ring(): fetch_add(1, release), then notify;
//   - a waiter reads Seen() before it looks for work, and if there is none
//     calls Wait(seen, stop), which returns once the sequence differs from
//     |seen| or a stop is requested (a std::stop_callback rings the bell,
//     so every wait honours stop requests).
// A ring between Seen() and Wait() is never lost: the sequence has moved, so
// the wait returns at once. std::atomic::wait is WaitOnAddress on MSVC and a
// futex on Linux.

#ifndef SE_CORE_DOORBELL_H_
#define SE_CORE_DOORBELL_H_

#include <atomic>
#include <cstdint>
#include <stop_token>

namespace se {

class Doorbell {
 public:
  Doorbell() = default;
  Doorbell(const Doorbell&) = delete;
  Doorbell& operator=(const Doorbell&) = delete;

  // Any thread.
  void Ring() noexcept {
    seq_.fetch_add(1, std::memory_order_release);
    seq_.notify_all();
  }

  // The sequence now; pass it to Wait after finding no work.
  [[nodiscard]] std::uint32_t Seen() const noexcept {
    return seq_.load(std::memory_order_acquire);
  }

  // Returns once the sequence differs from |seen| or |stop| is requested.
  void Wait(std::uint32_t seen, const std::stop_token& stop) noexcept {
    const std::stop_callback ring_on_stop(stop, [this]() noexcept { Ring(); });
    while (seq_.load(std::memory_order_acquire) == seen &&
           !stop.stop_requested()) {
      seq_.wait(seen, std::memory_order_acquire);
    }
  }

 private:
  std::atomic<std::uint32_t> seq_{0};
};

}  // namespace se

#endif  // SE_CORE_DOORBELL_H_
