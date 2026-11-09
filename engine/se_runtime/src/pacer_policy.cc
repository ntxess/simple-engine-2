// Copyright 2026 The simple-engine-2 Authors.

#include "se_runtime/pacer_policy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "se_core/time.h"
#include "se_runtime/present_port.h"

namespace se {
namespace {

// Signed nanoseconds a - b (both well below 2^63).
double SignedDeltaNs(std::uint64_t a, std::uint64_t b) noexcept {
  return a >= b ? static_cast<double>(a - b) : -static_cast<double>(b - a);
}

// anchor + offset, clamped at 0.
std::uint64_t Offset(std::uint64_t anchor, double offset_ns) noexcept {
  const auto rounded = static_cast<std::int64_t>(std::llround(offset_ns));
  if (rounded >= 0) return anchor + static_cast<std::uint64_t>(rounded);
  const auto back = static_cast<std::uint64_t>(-rounded);
  return back > anchor ? 0 : anchor - back;
}

}  // namespace

std::string_view PacingModeName(PacingMode mode) noexcept {
  switch (mode) {
    case PacingMode::kPaced:
      return "Paced";
    case PacingMode::kStandard:
      return "Standard";
    case PacingMode::kVrr:
      return "VRR";
  }
  return "?";
}

std::string_view PacingRequestName(PacingRequest request) noexcept {
  switch (request) {
    case PacingRequest::kAuto:
      return "auto";
    case PacingRequest::kPaced:
      return "paced";
    case PacingRequest::kStandard:
      return "standard";
    case PacingRequest::kVrr:
      return "vrr";
  }
  return "?";
}

std::optional<PacingRequest> ParsePacingRequest(
    std::string_view text) noexcept {
  if (text == "auto") return PacingRequest::kAuto;
  if (text == "paced") return PacingRequest::kPaced;
  if (text == "standard") return PacingRequest::kStandard;
  if (text == "vrr") return PacingRequest::kVrr;
  return std::nullopt;
}

bool PeriodsAgree(double measured_sec, double reference_sec) noexcept {
  if (!(measured_sec > 0.0) || !(reference_sec > 0.0)) return false;
  return std::abs(measured_sec - reference_sec) <=
         kPeriodAgreement * reference_sec;
}

bool SamplerFixedRefresh(const SamplerSample& sampler) noexcept {
  return sampler.valid && sampler.period_sec > 0.0 &&
         sampler.spread_sec <= kMaxFixedRefreshSpread * sampler.period_sec;
}

ModeChoice ChooseMode(const ModeInputs& inputs) noexcept {
  switch (inputs.request) {
    case PacingRequest::kStandard:
      return {PacingMode::kStandard, "requested (--pacing standard)"};
    case PacingRequest::kVrr:
      if (inputs.caps.tearing) {
        return {PacingMode::kVrr,
                "requested (--pacing vrr); IMMEDIATE with tearing"};
      }
      return {PacingMode::kStandard,
              "--pacing vrr, but IMMEDIATE (tearing) is not supported"};
    case PacingRequest::kAuto:
      if (!inputs.paced_by_default) {
        return {PacingMode::kStandard,
                "Standard is this platform's default (Paced is not claimed "
                "here until measured)"};
      }
      break;
    case PacingRequest::kPaced:
      break;
  }
  if (!inputs.caps.frame_latency_waitable) {
    return {PacingMode::kStandard,
            "no frame-latency waitable (SDL without the presentation patch)"};
  }
  if (!inputs.sampler.valid) {
    return {PacingMode::kStandard,
            "no valid vblank sampler for the window's output"};
  }
  if (!(inputs.display_period_sec > 0.0)) {
    return {PacingMode::kStandard, "the display mode is unknown"};
  }
  if (!PeriodsAgree(inputs.sampler.period_sec, inputs.display_period_sec)) {
    return {PacingMode::kStandard,
            "the vblank sampler's period disagrees with the display mode by "
            "more than 0.5%"};
  }
  if (!SamplerFixedRefresh(inputs.sampler)) {
    return {PacingMode::kStandard,
            "the vblank intervals vary (not a fixed-refresh display)"};
  }
  return {PacingMode::kPaced,
          "waitable and the vblank sampler agree with the display mode; "
          "misses from the latency wait on the vblank grid"};
}

std::string_view PeriodSourceName(PeriodSource source) noexcept {
  switch (source) {
    case PeriodSource::kSampler:
      return "sampler";
    case PeriodSource::kDisplayMode:
      return "display mode";
    case PeriodSource::kMeasured:
      return "measured";
    case PeriodSource::kFallback:
      return "fallback";
  }
  return "?";
}

PeriodChoice ChoosePeriod(const SamplerSample& sampler,
                          double display_period_sec,
                          double measured_period_sec) noexcept {
  const bool display_known = display_period_sec > 0.0;
  if (sampler.valid && sampler.period_sec > 0.0 &&
      (!display_known ||
       PeriodsAgree(sampler.period_sec, display_period_sec))) {
    return {sampler.period_sec, PeriodSource::kSampler};
  }
  const double reference =
      display_known ? display_period_sec : 1.0 / kFallbackRefreshHz;
  if (measured_period_sec > 0.0 && std::abs(measured_period_sec - reference) >
                                       kMeasuredPeriodTolerance * reference) {
    return {measured_period_sec, PeriodSource::kMeasured};
  }
  return {reference,
          display_known ? PeriodSource::kDisplayMode : PeriodSource::kFallback};
}

std::uint64_t GridFloor(std::uint64_t now_ns, std::uint64_t anchor_ns,
                        double period_ns) noexcept {
  if (!(period_ns > 0.0)) return now_ns;
  const double k = std::floor(SignedDeltaNs(now_ns, anchor_ns) / period_ns);
  std::uint64_t vblank = Offset(anchor_ns, k * period_ns);
  // Rounding to whole nanoseconds may land one past now.
  if (vblank > now_ns) vblank = Offset(anchor_ns, (k - 1.0) * period_ns);
  return vblank;
}

std::uint64_t GridNearest(std::uint64_t time_ns, std::uint64_t anchor_ns,
                          double period_ns) noexcept {
  if (!(period_ns > 0.0)) return time_ns;
  const double k = std::round(SignedDeltaNs(time_ns, anchor_ns) / period_ns);
  return Offset(anchor_ns, k * period_ns);
}

PacedWake PlanPacedWake(std::uint64_t now_ns, std::uint64_t anchor_ns,
                        double period_ns, int interval,
                        std::uint64_t prev_target_ns, double budget_sec,
                        double early_slack_ns) noexcept {
  PacedWake wake;
  if (!(period_ns > 0.0)) {
    wake.target_ns = now_ns;
    wake.wake_ns = now_ns;
    return wake;
  }
  const int vblanks = std::max(1, interval);
  const double step = static_cast<double>(vblanks) * period_ns;
  // A target must lie after this (its true vblank still ahead of now).
  const std::uint64_t ready_ns = Offset(now_ns, std::max(0.0, early_slack_ns));
  const std::uint64_t base =
      prev_target_ns != 0 ? GridNearest(prev_target_ns, anchor_ns, period_ns)
                          : GridFloor(ready_ns, anchor_ns, period_ns);
  // The first step past base that is still ahead of now.
  double k = 1.0;
  const double ahead = SignedDeltaNs(ready_ns, base);
  if (ahead >= step) k = std::floor(ahead / step) + 1.0;
  wake.target_ns = Offset(base, k * step);
  if (wake.target_ns <= ready_ns) {
    k += 1.0;
    wake.target_ns = Offset(base, k * step);
  }
  if (prev_target_ns != 0 && k > 1.0) {
    wake.skipped_intervals = static_cast<std::uint32_t>(k - 1.0) *
                             static_cast<std::uint32_t>(vblanks);
  }
  const auto budget_ns =
      static_cast<std::uint64_t>(std::llround(std::max(0.0, budget_sec) * 1e9));
  wake.wake_ns = budget_ns < wake.target_ns ? wake.target_ns - budget_ns : 0;
  if (wake.wake_ns < now_ns) wake.wake_ns = now_ns;
  return wake;
}

double PacedBudgetSec(int steps, const FrameCosts& costs,
                      double timer_p99_sec) noexcept {
  const double g_hi = std::isnan(costs.gpu_hi) ? 0.0 : costs.gpu_hi;
  return static_cast<double>(std::max(0, steps)) * costs.tick_hi +
         costs.non_tick_hi + g_hi +
         SafetyMarginSec(timer_p99_sec, costs.margin);
}

double VrrRateHz(double display_hz) noexcept {
  if (!(display_hz > 0.0)) return 120.0;
  return display_hz >= 119.5 ? 120.0 : 60.0;
}

VrrDeadline PlanVrrDeadline(std::uint64_t now_ns,
                            std::uint64_t prev_deadline_ns,
                            std::uint64_t period_ns,
                            std::uint64_t lead_ns) noexcept {
  VrrDeadline next;
  if (prev_deadline_ns == 0) {
    next.deadline_ns = now_ns + period_ns;
  } else {
    next.deadline_ns = prev_deadline_ns + period_ns;
    if (next.deadline_ns < now_ns) {
      next.deadline_ns = now_ns;
      next.reanchored = true;
    }
  }
  next.wake_ns = next.deadline_ns > lead_ns ? next.deadline_ns - lead_ns : 0;
  if (next.wake_ns < now_ns) next.wake_ns = now_ns;
  return next;
}

void PresentTracker::OnPresentIssued(std::uint64_t present_id,
                                     std::uint64_t target_ns) noexcept {
  if (size_ == kCapacity) {
    PopFront();
    ++dropped_;
  }
  entries_[(head_ + size_) % kCapacity] = Entry{present_id, target_ns};
  ++size_;
}

void PresentTracker::PopFront() noexcept {
  head_ = (head_ + 1) % kCapacity;
  --size_;
}

void PresentTracker::Reset() noexcept {
  head_ = 0;
  size_ = 0;
  have_last_ = false;
  last_ = PresentStatistics{};
}

PresentTracker::Result PresentTracker::OnStats(const PresentStatistics& stats,
                                               double period_ns) noexcept {
  Result result;
  if (stats.present_count == 0) return result;
  const std::uint64_t newest =
      size_ > 0 ? entries_[(head_ + size_ - 1) % kCapacity].id : 0;
  const bool backwards =
      have_last_ &&
      (stats.present_count < last_.present_count ||
       stats.present_refresh_count < last_.present_refresh_count ||
       stats.sync_refresh_count < last_.sync_refresh_count);
  const bool restarted = size_ > 0 && stats.last_present_count < newest;
  if (backwards || restarted) {
    head_ = 0;
    size_ = 0;
    ++resets_;
    result.reset = true;
    last_ = stats;
    have_last_ = true;
    return result;
  }
  // The refresh a vblank time falls on, by the statistics' sync pair.
  const auto target_refresh = [&](std::uint64_t time_ns) -> std::int64_t {
    const double vblanks =
        SignedDeltaNs(time_ns, stats.sync_time_ns) / period_ns;
    return static_cast<std::int64_t>(stats.sync_refresh_count) +
           static_cast<std::int64_t>(std::llround(vblanks));
  };
  const std::uint64_t shown_id = stats.present_count;
  const auto shown_refresh =
      static_cast<std::int64_t>(stats.present_refresh_count);
  while (size_ > 0 && entries_[head_].id <= shown_id) {
    const Entry entry = entries_[head_];
    PopFront();
    ++result.resolved;
    if (!(period_ns > 0.0)) continue;
    const std::int64_t target = target_refresh(entry.target_ns);
    std::int64_t refresh = target;
    bool late = false;
    if (entry.id == shown_id) {
      refresh = shown_refresh;
      late = refresh > target;
    } else if (have_last_ && entry.id > last_.present_count) {
      // Shown between two samples: no earlier than one refresh after the
      // previous displayed present per present in between.
      refresh = static_cast<std::int64_t>(last_.present_refresh_count) +
                static_cast<std::int64_t>(entry.id - last_.present_count);
      late = target < refresh;
    }
    if (late) {
      ++result.missed;
      result.late_refreshes += static_cast<std::uint64_t>(refresh - target);
    }
  }
  last_ = stats;
  have_last_ = true;
  return result;
}

std::string_view MissSourceName(MissSource source) noexcept {
  switch (source) {
    case MissSource::kNone:
      return "none";
    case MissSource::kStatistics:
      return "present statistics";
    case MissSource::kLatencyWait:
      return "the frame-latency wait on the vblank grid";
  }
  return "?";
}

double EarlySlackNs(double period_ns) noexcept {
  return std::min(WaitTracker::kEarlySlackSec * 1e9,
                  0.25 * std::max(0.0, period_ns));
}

void WaitTracker::OnPresentIssued(std::uint64_t target_ns,
                                  std::uint64_t submit_ns) noexcept {
  if (size_ == kCapacity) PopFront();
  entries_[(head_ + size_) % kCapacity] = Entry{target_ns, submit_ns};
  ++size_;
}

void WaitTracker::PopFront() noexcept {
  head_ = (head_ + 1) % kCapacity;
  --size_;
}

void WaitTracker::Reset() noexcept {
  head_ = 0;
  size_ = 0;
}

WaitTracker::Result WaitTracker::OnWait(bool ready, bool blocked,
                                        std::uint64_t ready_ns, int latency,
                                        double period_ns) noexcept {
  Result result;
  const auto back = static_cast<std::size_t>(std::max(1, latency));
  // Fewer presents than the latency: the wait was not for any of them.
  if (back > size_) return result;
  // The present the wait reports is |latency| - 1 before the newest; older
  // ones never had a wait of their own.
  while (size_ > back) {
    PopFront();
    ++result.unresolved;
  }
  const Entry entry = entries_[head_];
  PopFront();
  const std::uint64_t target_ns = entry.target_ns;
  if (target_ns == 0 || !(period_ns > 0.0)) {
    ++result.unresolved;
    return result;
  }
  const double slack_ns = EarlySlackNs(period_ns);
  if (!ready || !blocked) {
    // When it was taken is unknown; it was late if it was submitted after
    // its target's vblank.
    const double after_ns =
        entry.submit_ns != 0
            ? SignedDeltaNs(entry.submit_ns, target_ns) - slack_ns
            : -1.0;
    if (after_ns > 0.0) {
      ++result.resolved;
      ++result.missed;
      result.late_refreshes =
          static_cast<std::uint64_t>(std::floor(after_ns / period_ns)) + 1;
    } else {
      ++result.unresolved;
    }
    return result;
  }
  const double offset_ns = SignedDeltaNs(ready_ns, target_ns);
  const double late = std::floor((offset_ns + slack_ns) / period_ns);
  ++result.resolved;
  result.blocked = true;
  result.phase_ns = static_cast<std::int64_t>(
      std::llround(offset_ns - std::max(0.0, late) * period_ns));
  if (late >= 1.0) {
    ++result.missed;
    result.late_refreshes = static_cast<std::uint64_t>(late);
  }
  return result;
}

void HalfRatePolicy::ClearWindow() noexcept {
  head_ = 0;
  size_ = 0;
  window_sec_ = 0.0;
  window_resolved_ = 0;
  window_missed_ = 0;
}

void HalfRatePolicy::Reset() noexcept {
  ClearWindow();
  over_budget_sec_ = 0.0;
  margin_sec_ = 0.0;
  active_ = false;
}

double HalfRatePolicy::MissFraction() const noexcept {
  return window_resolved_ > 0 ? static_cast<double>(window_missed_) /
                                    static_cast<double>(window_resolved_)
                              : 0.0;
}

bool HalfRatePolicy::OnFrame(const FrameInput& frame) noexcept {
  if (frame.modal) return false;
  const bool fast =
      frame.vblank_period_sec > 0.0 && 1.0 / frame.vblank_period_sec > kMinHz;
  if (!fast) {
    const bool was_active = active_;
    if (active_) ++exits_;
    Reset();
    return was_active;
  }
  // The window keeps just enough frames to cover kEnterSec.
  if (size_ == kWindowFrames) {
    const Sample& oldest = window_[head_];
    window_sec_ -= oldest.frame_sec;
    window_resolved_ -= oldest.resolved;
    window_missed_ -= oldest.missed;
    head_ = (head_ + 1) % kWindowFrames;
    --size_;
  }
  window_[(head_ + size_) % kWindowFrames] =
      Sample{frame.frame_sec, frame.resolved, frame.missed};
  ++size_;
  window_sec_ += frame.frame_sec;
  window_resolved_ += frame.resolved;
  window_missed_ += frame.missed;
  while (size_ > 1 && window_sec_ - window_[head_].frame_sec >= kEnterSec) {
    const Sample& oldest = window_[head_];
    window_sec_ -= oldest.frame_sec;
    window_resolved_ -= oldest.resolved;
    window_missed_ -= oldest.missed;
    head_ = (head_ + 1) % kWindowFrames;
    --size_;
  }

  if (!active_) {
    over_budget_sec_ = frame.planned_full_rate_sec > frame.vblank_period_sec
                           ? over_budget_sec_ + frame.frame_sec
                           : 0.0;
    // Allow for the frame times not adding up to exactly 2 s.
    const bool full_window = window_sec_ >= kEnterSec - 1e-9;
    const bool misses = full_window && MissFraction() > kMissBudget;
    if (misses || over_budget_sec_ >= kEnterSec - 1e-9) {
      ClearWindow();
      over_budget_sec_ = 0.0;
      margin_sec_ = 0.0;
      active_ = true;
      ++entries_;
      return true;
    }
    return false;
  }
  margin_sec_ =
      frame.full_rate_cost95_sec < kExitLoadFraction * frame.vblank_period_sec
          ? margin_sec_ + frame.frame_sec
          : 0.0;
  if (margin_sec_ >= kExitSec - 1e-9) {
    ClearWindow();
    over_budget_sec_ = 0.0;
    margin_sec_ = 0.0;
    active_ = false;
    ++exits_;
    return true;
  }
  return false;
}

LatencyPolicy::LatencyPolicy(int base, bool adaptive) noexcept
    : base_(std::clamp(base, 1, 2)), adaptive_(adaptive), latency_(base_) {}

void LatencyPolicy::Reset() noexcept {
  latency_ = base_;
  want_sec_ = 0.0;
  clear_sec_ = 0.0;
}

bool LatencyPolicy::OnFrame(double frame_sec, double cpu_sec, double gpu_sec,
                            double period_sec, bool modal) noexcept {
  if (!adaptive_ || base_ != 1 || modal) return false;
  const bool want_two = !std::isnan(gpu_sec) && cpu_sec <= period_sec &&
                        gpu_sec <= period_sec && cpu_sec + gpu_sec > period_sec;
  if (latency_ == 1) {
    want_sec_ = want_two ? want_sec_ + frame_sec : 0.0;
    if (want_sec_ >= kEnterSec - 1e-9) {
      latency_ = 2;
      want_sec_ = 0.0;
      clear_sec_ = 0.0;
      ++changes_;
      return true;
    }
    return false;
  }
  clear_sec_ = want_two ? 0.0 : clear_sec_ + frame_sec;
  if (clear_sec_ >= kExitSec - 1e-9) {
    latency_ = 1;
    want_sec_ = 0.0;
    clear_sec_ = 0.0;
    ++changes_;
    return true;
  }
  return false;
}

void LatenessTracker::Add(double late_sec) noexcept {
  samples_[(head_ + size_) % kCapacity] = std::max(0.0, late_sec);
  if (size_ < kCapacity) {
    ++size_;
  } else {
    head_ = (head_ + 1) % kCapacity;
  }
}

double LatenessTracker::Percentile(double p, double empty) const noexcept {
  if (size_ == 0) return empty;
  std::array<double, kCapacity> sorted{};
  for (std::size_t i = 0; i < size_; ++i) {
    sorted[i] = samples_[(head_ + i) % kCapacity];
  }
  const auto last = sorted.begin() + static_cast<std::ptrdiff_t>(size_);
  std::sort(sorted.begin(), last);
  const double clamped = std::clamp(p, 0.0, 1.0);
  auto rank =
      static_cast<std::size_t>(std::ceil(clamped * static_cast<double>(size_)));
  rank = std::clamp<std::size_t>(rank, 1, size_);
  return sorted[rank - 1];
}

}  // namespace se
