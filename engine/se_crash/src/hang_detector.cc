// Copyright 2026 The simple-engine-2 Authors.

#include "se_crash/hang_detector.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace se::crash {

void HangDetector::Start(std::uint64_t now_ns) noexcept {
  boot_start_ns_ = now_ns;
  armed_ = false;
  disarmed_ = false;
  boot_reported_ = false;
  stalled_periods_ = 0;
  stall_ns_ = 0;
  in_hang_ = false;
  hang_in_driver_ = false;
}

void HangDetector::Arm(std::uint64_t iterates, std::uint64_t now_ns) noexcept {
  armed_ = true;
  last_iterates_ = iterates;
  progress_ns_ = now_ns;
  count_from_ns_ = now_ns;
  last_sample_ns_ = now_ns;
  stalled_periods_ = 0;
  stall_ns_ = 0;
  in_hang_ = false;
}

void HangDetector::Disarm() noexcept { disarmed_ = true; }

std::uint64_t HangDetector::MaxSampleGapNs() const noexcept {
  if (config_.max_sample_gap_ns == 0) return 0;
  return std::max(config_.max_sample_gap_ns, 2 * config_.period_ns);
}

HangVerdict HangDetector::Observe(const HangSample& sample) noexcept {
  if (disarmed_) return HangVerdict::kOff;
  if (!armed_) {
    const bool over = sample.now_ns > boot_start_ns_ &&
                      sample.now_ns - boot_start_ns_ > config_.boot_budget_ns;
    if (over && !boot_reported_ && !sample.debugger) {
      boot_reported_ = true;
      return HangVerdict::kBootOverBudget;
    }
    return HangVerdict::kProgress;
  }
  const std::uint64_t gap =
      sample.now_ns > last_sample_ns_ ? sample.now_ns - last_sample_ns_ : 0;
  last_sample_ns_ = std::max(last_sample_ns_, sample.now_ns);
  if (sample.iterates != last_iterates_) {
    last_iterates_ = sample.iterates;
    const bool ended = in_hang_;
    if (ended) {
      episode_ns_ =
          sample.now_ns > progress_ns_ ? sample.now_ns - progress_ns_ : 0;
    }
    progress_ns_ = sample.now_ns;
    count_from_ns_ = sample.now_ns;
    stalled_periods_ = 0;
    stall_ns_ = 0;
    in_hang_ = false;
    return ended ? HangVerdict::kHangEnded : HangVerdict::kProgress;
  }
  stall_ns_ = sample.now_ns > progress_ns_ ? sample.now_ns - progress_ns_ : 0;
  if (sample.suspended || sample.modal || sample.heartbeat || sample.debugger) {
    count_from_ns_ = std::max(count_from_ns_, sample.now_ns);
    stalled_periods_ = 0;
    return HangVerdict::kExcused;
  }
  // Nobody watched main during a gap this long: the process was stopped,
  // so the time is no evidence of a hang. A declared hang stays declared.
  if (const std::uint64_t limit = MaxSampleGapNs();
      limit != 0 && gap > limit && !in_hang_) {
    gap_ns_ = gap;
    count_from_ns_ = std::max(count_from_ns_, sample.now_ns);
    stalled_periods_ = 0;
    return HangVerdict::kUnobserved;
  }
  const std::uint64_t counted =
      sample.now_ns > count_from_ns_ ? sample.now_ns - count_from_ns_ : 0;
  const std::uint64_t periods =
      config_.period_ns > 0 ? counted / config_.period_ns : 0;
  stalled_periods_ = static_cast<std::uint32_t>(std::min<std::uint64_t>(
      periods, std::numeric_limits<std::uint32_t>::max()));
  if (config_.declare_hangs && !in_hang_ &&
      stalled_periods_ >= config_.hang_periods) {
    in_hang_ = true;
    hang_in_driver_ = sample.in_driver;
    ++hangs_;
    return HangVerdict::kHang;
  }
  return HangVerdict::kStalled;
}

}  // namespace se::crash
