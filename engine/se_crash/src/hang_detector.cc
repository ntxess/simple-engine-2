// Copyright 2026 The simple-engine-2 Authors.

#include "se_crash/hang_detector.h"

#include <cstdint>

namespace se::crash {

void HangDetector::Start(std::uint64_t now_ns) noexcept {
  boot_start_ns_ = now_ns;
  armed_ = false;
  boot_reported_ = false;
  stalled_periods_ = 0;
  in_hang_ = false;
}

void HangDetector::Arm(std::uint64_t iterates) noexcept {
  armed_ = true;
  last_iterates_ = iterates;
  stalled_periods_ = 0;
  in_hang_ = false;
}

HangVerdict HangDetector::Observe(const HangSample& sample) noexcept {
  if (!armed_) {
    const bool over = sample.now_ns > boot_start_ns_ &&
                      sample.now_ns - boot_start_ns_ > config_.boot_budget_ns;
    if (over && !boot_reported_ && !sample.debugger) {
      boot_reported_ = true;
      return HangVerdict::kBootOverBudget;
    }
    return HangVerdict::kProgress;
  }
  if (sample.iterates != last_iterates_) {
    last_iterates_ = sample.iterates;
    stalled_periods_ = 0;
    in_hang_ = false;
    return HangVerdict::kProgress;
  }
  if (sample.suspended || sample.modal || sample.heartbeat || sample.debugger) {
    stalled_periods_ = 0;
    return HangVerdict::kExcused;
  }
  ++stalled_periods_;
  if (config_.declare_hangs && !in_hang_ &&
      stalled_periods_ >= config_.hang_periods) {
    in_hang_ = true;
    return HangVerdict::kHang;
  }
  return HangVerdict::kStalled;
}

}  // namespace se::crash
