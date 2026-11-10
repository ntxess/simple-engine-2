// Copyright 2026 The simple-engine-2 Authors.
//
// The sim thread's system clock: se_os's monotonic clock, precise timer and
// pause instruction behind WaitClock, so se_sim itself reads no clock.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

#include "se_core/log.h"
#include "se_os/precise_wait.h"
#include "se_sim/paced_clock.h"

namespace se {
namespace {

// The longest single timer wait: a stop request is seen within it.
constexpr std::uint64_t kSliceNs = 10'000'000;

class SystemWaitClock final : public WaitClock {
 public:
  explicit SystemWaitClock(std::optional<PreciseTimer> timer) noexcept
      : timer_(std::move(timer)) {}

  std::uint64_t NowNs() noexcept override { return MonotonicNowNs(); }

  void SleepUntil(std::uint64_t deadline_ns,
                  const std::stop_token& stop) noexcept override {
    for (;;) {
      const std::uint64_t now = MonotonicNowNs();
      if (now >= deadline_ns || stop.stop_requested()) return;
      const std::uint64_t until = std::min(deadline_ns, now + kSliceNs);
      if (timer_.has_value()) {
        timer_->SleepUntil(until);
      } else {
        // No timer: the caller spins the whole wait.
        return;
      }
    }
  }

  void Relax() noexcept override { CpuRelax(); }

 private:
  std::optional<PreciseTimer> timer_;
};

}  // namespace

std::unique_ptr<WaitClock> MakeSystemWaitClock() {
  std::optional<PreciseTimer> timer;
  if (auto created = PreciseTimer::Create(); created.has_value()) {
    timer.emplace(std::move(*created));
    if (!timer->HighResolution()) {
      SE_LOG_WARN(
          "sim clock: no high-resolution waitable timer; waits wake "
          "at the system timer's granularity");
    }
  } else {
    SE_LOG_WARN("sim clock: {}; the sim thread spins its waits",
                created.error().message);
  }
  return std::make_unique<SystemWaitClock>(std::move(timer));
}

}  // namespace se
