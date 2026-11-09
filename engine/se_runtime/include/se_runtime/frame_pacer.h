// Copyright 2026 The simple-engine-2 Authors.
//
// The FramePacer (docs/milestone-specs.md M6b; docs/design/mt-design.md
// "Input And Latency", the API sketch's frame_pacer.hpp, the "Frame
// Timeline Text"): the shell around PacerPolicy that reads the clock, the
// present port and the vblank sampler, and waits. Main thread only.
//
// Each iterate:
//   BeginFrame   the fence of frame N-L (L = FrameLatency()), then the
//                present statistics (PresentTracker, counted beside the
//                pacer's source), the FrameClock, and the frame's miss
//                source: the latency wait (EndFrame, below) when it can
//                resolve the present, else the statistics (ADR-066); the
//                result is reported to FixedStepper::OnPresentResult(missed,
//                modal) (a wait's result one frame later). Returns the
//                FrameClock. Its period is the sampler's when it agrees with
//                the display mode within 0.5%, else the mode, else the
//                measured frame period (ADR-039). Frame starts lie on the
//                vblank grid when the sampler is valid (ADR-016, ADR-057):
//                one interval before the target in Paced, the vblank at or
//                before now in Standard, the deadline in VRR.
//   OnFrameSubmitted  after the frame transaction: the frame's fence goes
//                to the port, and the present is registered with its target
//                vblank for the miss count.
//   EndFrame     nothing while Modal or when the frame presented nothing.
//                Every wait on the waitable polls first, then blocks; a
//                blocked wait's return on the vblank grid gives the hit or
//                miss of the present it waited for (WaitTracker, ADR-066)
//                and the take that calibrates the statistics' own check
//                (ADR-072).
//                Paced: the waitable, then SDL_DelayNS to the predicted
//                vblank of the window's output minus B(next) (never spins).
//                Standard: the waitable when present, then
//                SDL_WaitForGPUSwapchain; in HalfRate, after a frame whose
//                present was taken at the first vblank after its start, the
//                last composite is presented again (RepresentLast) and
//                waited for the same way (ADR-071). VRR: the waitable, then
//                the self-paced deadline. HalfRate and the adaptive frame
//                latency are updated here.
//
// The mode is chosen when the port is attached and re-evaluated twice a
// second and after an output change: Paced is taken only after the
// sampler has been valid and agreeing for kPromoteSec; any other choice
// applies at once. Every change is logged with its reason. Per-frame
// statistics (waitable and fence waits, missed vblanks, NULL acquires,
// mode, interval, latency) are logged once a second at debug level.

#ifndef SE_RUNTIME_FRAME_PACER_H_
#define SE_RUNTIME_FRAME_PACER_H_

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/time.h"
#include "se_platform/vblank_sampler.h"
#include "se_runtime/pacer_policy.h"
#include "se_runtime/present_port.h"

namespace se {

// The pacer's clock and sleep: SDL_GetTicksNS and SDL_DelayNS, or a fake.
class PacerClock {
 public:
  PacerClock() = default;
  PacerClock(const PacerClock&) = delete;
  PacerClock& operator=(const PacerClock&) = delete;
  virtual ~PacerClock() = default;
  [[nodiscard]] virtual std::uint64_t NowNs() noexcept = 0;
  virtual void SleepNs(std::uint64_t ns) noexcept = 0;
};

// SDL_GetTicksNS and SDL_DelayNS (a high-resolution waitable timer on
// Windows; never a spin).
[[nodiscard]] PacerClock& SdlClock();

// The vblank sampler's reading at |now_ns|; empty: no sampler.
using VblankReader = std::function<VblankReading(std::uint64_t now_ns)>;

struct FramePacerConfig {
  PacingRequest request = PacingRequest::kAuto;
  // Paced is this platform's runtime default (Windows).
  bool paced_by_default = false;
  // 1, or 2 for the editor (M10).
  int frame_latency = 1;
  bool adaptive_latency = true;
  bool half_rate = true;
  // > 0: a synthetic clock, every frame this long (--fixed-frame-dt).
  double fixed_frame_dt_sec = 0.0;
  // Bounds of the waitable and fence waits.
  std::uint64_t waitable_timeout_ns = 50'000'000;
  std::uint64_t fence_timeout_ns = 100'000'000;
};

// What one frame did, for the stats, the title and tests.
struct PacerFrame {
  std::uint64_t frame = 0;
  PacingMode mode = PacingMode::kStandard;
  int interval = 1;
  int latency = 1;
  PeriodSource period_source = PeriodSource::kFallback;
  // The display's vblank period and the presentation period (x interval;
  // VRR's deadline period).
  double vblank_period_sec = 0.0;
  double period_sec = 0.0;
  bool on_grid = false;
  std::uint64_t frame_start_ns = 0;
  // The vblank this frame's present targets (0: none known).
  std::uint64_t target_ns = 0;
  bool fence_pending = false;
  std::uint64_t fence_wait_ns = 0;
  PresentWait waitable = PresentWait::kUnsupported;
  std::uint64_t waitable_wait_ns = 0;
  std::uint64_t swapchain_wait_ns = 0;
  // The end-of-frame sleep: requested wake, actual wake, length.
  std::uint64_t wake_ns = 0;
  std::uint64_t woke_ns = 0;
  std::uint64_t sleep_ns = 0;
  // VRR: the next deadline, and how late the wake was against it.
  std::uint64_t deadline_ns = 0;
  std::int64_t deadline_error_ns = 0;
  // Presents resolved this frame (statistics at BeginFrame, the latency
  // wait at EndFrame), and where the results came from (ADR-066).
  std::uint32_t resolved = 0;
  std::uint32_t missed = 0;
  // Vblanks the missed ones were late by, summed.
  std::uint64_t late_vblanks = 0;
  MissSource miss_source = MissSource::kNone;
  // Latency waits that told nothing (ready at once, a timeout, no target).
  std::uint32_t unresolved = 0;
  // Present statistics resolved this frame, whatever the source: on a
  // window DWM composes they count DWM's displayed refresh (ADR-066).
  std::uint32_t stats_resolved = 0;
  std::uint32_t stats_missed = 0;
  // Those with a known refresh, by the refreshes they were shown after
  // their targets, and after the vblank the latency wait saw them taken at
  // (CheckStatistics, ADR-072).
  std::array<std::uint32_t, PresentTracker::kLatenessBins> stats_lateness{};
  std::array<std::uint32_t, PresentTracker::kLatenessBins> stats_delay{};
  // A blocked latency wait resolved this frame's present: its return's
  // offset from the vblank the present was taken at.
  bool wait_blocked = false;
  std::int64_t wait_phase_ns = 0;
  bool stats_reset = false;
  bool presented = false;
  bool null_acquire = false;
  bool represented = false;
  // When the frame was submitted (OnFrameSubmitted): target - submit is
  // the lead the present had on its vblank.
  std::uint64_t submit_ns = 0;
};

struct PacerTotals {
  std::uint64_t frames = 0;
  std::uint64_t presented = 0;
  std::uint64_t null_acquires = 0;
  std::uint64_t waitable_waits = 0;
  std::uint64_t waitable_timeouts = 0;
  std::uint64_t fence_waits = 0;
  std::uint64_t fence_wait_ns = 0;
  std::uint64_t fence_pending_frames = 0;
  std::uint64_t swapchain_waits = 0;
  std::uint64_t sleeps = 0;
  std::uint64_t resolved_presents = 0;
  std::uint64_t missed_presents = 0;
  std::uint64_t missed_vblanks = 0;
  // Presents resolved by each source (ADR-066), and latency waits that
  // told nothing.
  std::uint64_t resolved_by_statistics = 0;
  std::uint64_t resolved_by_wait = 0;
  std::uint64_t unresolved_waits = 0;
  // Every present statistics resolved, and the ones later than their
  // targets by them, whether or not they were the source.
  std::uint64_t stats_resolved = 0;
  std::uint64_t stats_missed = 0;
  std::array<std::uint64_t, PresentTracker::kLatenessBins> stats_lateness{};
  std::array<std::uint64_t, PresentTracker::kLatenessBins> stats_delay{};
  std::uint64_t stats_resets = 0;
  std::uint64_t represents = 0;
  std::uint64_t mode_changes = 0;
  std::uint64_t half_rate_entries = 0;
  std::uint64_t half_rate_exits = 0;
  std::uint64_t latency_changes = 0;
  std::uint64_t vrr_reanchors = 0;
};

class FramePacer {
 public:
  // A re-evaluated Paced choice is taken after the sampler was valid and
  // agreeing for this long.
  static constexpr double kPromoteSec = 0.5;
  static constexpr double kReevaluateSec = 0.5;
  static constexpr double kDefaultTimerP99Sec = 0.5e-3;
  // The measured period: weight of each delta, longest delta averaged.
  static constexpr double kMeasureWeight = 0.1;
  static constexpr double kMaxMeasuredSec = 0.25;

  // |clock| outlives the pacer; nullptr: SdlClock().
  explicit FramePacer(const FramePacerConfig& config,
                      PacerClock* clock = nullptr);

  // Wiring: the port (nullptr: headless, the wall clock and no waits) and
  // the sampler (empty: none). Both outlive the pacer or a later Attach;
  // chooses the mode, sets the present mode and the frame latency.
  void Attach(PresentPort* port, VblankReader sampler) SE_REQUIRES(g_main);
  // Drops the port and the sampler (shutdown, before they go).
  void Detach() SE_REQUIRES(g_main);
  // The display mode's refresh of the window's display (the OS's, else
  // SDL's; 0 unknown).
  void SetDisplayRefresh(double hz) SE_REQUIRES(g_main);
  // The window's output changed (a display change, a move to another
  // monitor): the targets restart and the mode is re-evaluated.
  void OnOutputChanged() SE_REQUIRES(g_main);
  // The next frame is a first frame (leaving Suspended): no catch-up.
  void ResetClock() noexcept;

  [[nodiscard]] FrameClock BeginFrame(bool modal, FixedStepper& stepper)
      SE_REQUIRES(g_main);
  // |presented|: a swapchain texture was acquired and submitted.
  // |fence|: the frame's fence, handed to the port (nullptr: none).
  void OnFrameSubmitted(bool presented, bool null_acquire,
                        SDL_GPUFence* fence = nullptr) SE_REQUIRES(g_main);
  void EndFrame(const StepPlan& next, const FrameCosts& costs, bool modal)
      SE_REQUIRES(g_main);

  [[nodiscard]] PacingMode Mode() const noexcept { return mode_; }
  [[nodiscard]] std::string_view ModeReason() const noexcept {
    return mode_reason_;
  }
  // 1, or 2 in HalfRate.
  [[nodiscard]] int PresentInterval() const noexcept;
  // 1 or 2 (the editor's default is 2).
  [[nodiscard]] int FrameLatency() const noexcept { return latency_.Latency(); }
  [[nodiscard]] bool HalfRate() const noexcept { return half_rate_.Active(); }
  [[nodiscard]] const PresentCaps& Caps() const noexcept { return caps_; }
  // Where this frame's misses come from (ADR-066).
  [[nodiscard]] MissSource Source() const noexcept {
    return frame_.miss_source;
  }
  [[nodiscard]] double DisplayHz() const noexcept { return display_hz_; }
  [[nodiscard]] const PacerFrame& LastFrame() const noexcept { return frame_; }
  [[nodiscard]] const PacerTotals& Totals() const noexcept { return totals_; }
  // "Paced 165.00 Hz x1 L1 miss 0" for the window title.
  [[nodiscard]] std::string Summary() const;
  // The timer p99 the pacer measured (or the default).
  [[nodiscard]] double TimerP99Sec() const noexcept;

 private:
  void Evaluate(std::uint64_t now_ns, bool initial);
  void SetMode(PacingMode mode, std::string_view reason);
  [[nodiscard]] SamplerSample Sample(const VblankReading& reading) const;
  void RegisterPresent(std::uint64_t target_ns);
  // The waitable: a poll, then a bounded wait; its result feeds the
  // WaitTracker (the miss source when it can resolve the frame's present)
  // and the statistics' composition delay, and is returned (empty without
  // a waitable or in VRR).
  WaitTracker::Result WaitWaitable(std::uint64_t& now_ns);
  void AddWaitResult(const WaitTracker::Result& result);
  void UpdatePolicies(const StepPlan& next, const FrameCosts& costs, bool modal,
                      const WaitTracker::Result& carried);
  void Sleep(std::uint64_t now_ns, std::uint64_t wake_ns);
  void LogSecond(std::uint64_t now_ns);

  FramePacerConfig config_;
  PacerClock* clock_ = nullptr;
  PresentPort* port_ = nullptr;
  VblankReader sampler_;
  PresentCaps caps_;
  double display_hz_ = 0.0;

  PacingMode mode_ = PacingMode::kStandard;
  std::string_view mode_reason_ = "not attached";
  std::uint64_t next_evaluation_ns_ = 0;
  // Since when the evaluation has said Paced while the mode is not.
  std::uint64_t paced_since_ns_ = 0;

  PresentTracker tracker_;
  WaitTracker wait_tracker_;
  // The last GetPresentStats returned statistics (the path produces them).
  bool stats_live_ = false;
  // The latency waits' results of the previous EndFrame, reported to the
  // stepper and HalfRate in the next frame.
  WaitTracker::Result carried_;
  HalfRatePolicy half_rate_;
  LatencyPolicy latency_;
  LatenessTracker lateness_;

  // The frame clock.
  std::uint64_t prev_start_ns_ = 0;
  std::uint64_t prev_now_ns_ = 0;
  std::uint64_t synthetic_ns_ = 0;
  double measured_vblank_sec_ = 0.0;
  double vblank_period_sec_ = 1.0 / kFallbackRefreshHz;
  double tick_dt_sec_ = 1.0 / 120.0;
  // Paced: the target of the frame being built (set by the previous
  // EndFrame). VRR: the deadline the frame started at.
  std::uint64_t target_ns_ = 0;
  std::uint64_t vrr_deadline_ns_ = 0;
  // The present this frame registers: its target.
  std::uint64_t frame_target_ns_ = 0;
  // Standard HalfRate: the vblank the previous frame's own present was
  // taken at (0: none known).
  std::uint64_t half_rate_take_ns_ = 0;
  std::uint64_t last_present_id_ = 0;
  std::uint64_t submitted_ = 0;
  bool presented_this_frame_ = false;

  PacerFrame frame_;
  PacerTotals totals_;
  PacerTotals logged_totals_;
  std::uint64_t last_log_ns_ = 0;
};

}  // namespace se

#endif  // SE_RUNTIME_FRAME_PACER_H_
