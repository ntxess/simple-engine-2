// Copyright 2026 The simple-engine-2 Authors.
//
// HangDetector: the watchdog's pure hang rules (docs/design/mt-design.md
// "watchdog", "Running -> Hung -> Running"; docs/milestone-specs.md M12).
// The watchdog thread feeds it HangSamples, read from the atomics main
// writes, and acts on the verdicts; the detector itself keeps no clock and
// touches nothing shared, so it is tested with a fake clock and no threads.
//
// Until the first iterate arms it, only the boot budget applies: a boot
// that takes longer than boot_budget_ns (30 s) on the unbiased clock is
// reported once, unless a debugger is attached. Once armed, a hang is
// hang_periods (5) consecutive periods of period_ns (1 s) without progress
// of main's iterate counter, on the unbiased clock (which does not advance
// while the system sleeps), with nothing excusing them: a sample that finds
// main Suspended, Modal, inside known long work (the heartbeat) or under a
// debugger starts the count over. The periods are counted from the sample
// that last saw progress, so the watchdog samples several times a period
// and declares a hang within one sample interval of the fifth period's end
// (ADR-104). A hang is declared once per episode, labelled a GPU or
// compositor stall when main was inside an SDL_GPU or pacer wait (the
// in-driver flag); the episode ends at the first sample that sees progress
// again, with its length. After Disarm() (the shutdown deadline owns the
// end of the process) nothing is judged.
//
// The periods must have been watched: when the detector itself got no
// sample for longer than max_sample_gap_ns (2 s, or two periods when that
// is longer), the whole process was stopped (a debugger that detached
// while the process stood in it, SIGSTOP, a paused virtual machine), not
// only main, and the count starts over from the sample after the gap.

#ifndef SE_CRASH_HANG_DETECTOR_H_
#define SE_CRASH_HANG_DETECTOR_H_

#include <cstdint>

namespace se::crash {

struct HangDetectorConfig {
  // How long the boot may take before the first iterate.
  std::uint64_t boot_budget_ns = 30'000'000'000;
  // One period.
  std::uint64_t period_ns = 1'000'000'000;
  // Consecutive periods without progress that make a hang.
  std::uint32_t hang_periods = 5;
  // M4 counted stalls without declaring hangs; M12 declares them.
  bool declare_hangs = true;
  // The longest time between two samples that still counts as watched (the
  // watchdog samples four times a period); two periods when that is
  // longer. A longer gap without progress starts the count over. 0: gaps
  // are not judged.
  std::uint64_t max_sample_gap_ns = 2'000'000'000;
};

// One reading of main's progress (atomics, read by the watchdog).
struct HangSample {
  // The unbiased clock (se::UnbiasedNowNs), which excludes system sleep.
  std::uint64_t now_ns = 0;
  // Main's iterate counter.
  std::uint64_t iterates = 0;
  bool suspended = false;
  bool modal = false;
  bool heartbeat = false;
  bool debugger = false;
  // Main is inside an SDL_GPU acquire, present or fence wait, or one of the
  // pacer's waits (se_platform/driver_wait.h).
  bool in_driver = false;
};

enum class HangVerdict : std::uint8_t {
  // Booting within the budget, or main made progress.
  kProgress,
  // No progress, but Suspended, Modal, the heartbeat or a debugger excuses
  // it; the count starts over.
  kExcused,
  // No progress and nothing excuses it: fewer than hang_periods periods so
  // far, a hang already declared in this episode, or declaration is off.
  kStalled,
  // No progress, and the detector had no sample for longer than the gap
  // limit: the process was stopped, the count starts over.
  kUnobserved,
  // The boot took longer than its budget (once).
  kBootOverBudget,
  // hang_periods periods without progress (once per episode).
  kHang,
  // Main made progress again after a declared hang: the episode is over
  // (EpisodeNs() is its length).
  kHangEnded,
  // Disarmed: nothing is judged.
  kOff,
};

class HangDetector {
 public:
  explicit HangDetector(const HangDetectorConfig& config) noexcept
      : config_(config) {}

  // The boot started at |now_ns|.
  void Start(std::uint64_t now_ns) noexcept;
  // The first iterate ran: the boot budget no longer applies, and progress
  // is measured from |iterates| at |now_ns|.
  void Arm(std::uint64_t iterates, std::uint64_t now_ns) noexcept;
  // Nothing is judged from now on (the shutdown deadline started).
  void Disarm() noexcept;

  // Judges one sample. Samples may come at any rate; their clock must not
  // go backwards.
  [[nodiscard]] HangVerdict Observe(const HangSample& sample) noexcept;

  [[nodiscard]] bool Armed() const noexcept { return armed_; }
  [[nodiscard]] bool Disarmed() const noexcept { return disarmed_; }
  // Whole periods without progress, counted from the sample that last saw
  // progress, an excuse or the end of an unwatched gap.
  [[nodiscard]] std::uint32_t StalledPeriods() const noexcept {
    return stalled_periods_;
  }
  // How long main had made no progress at the last sample, from the sample
  // that last saw progress (an excuse does not shorten it).
  [[nodiscard]] std::uint64_t StallNs() const noexcept { return stall_ns_; }
  // True from a declared hang until main makes progress again.
  [[nodiscard]] bool InHang() const noexcept { return in_hang_; }
  // Hangs declared so far.
  [[nodiscard]] std::uint64_t Hangs() const noexcept { return hangs_; }
  // The last declared hang found main in a driver wait (the GPU or
  // compositor stall label).
  [[nodiscard]] bool HangInDriver() const noexcept { return hang_in_driver_; }
  // The length of the last episode that ended: from the sample that last
  // saw progress before the hang to the one that saw progress again.
  [[nodiscard]] std::uint64_t EpisodeNs() const noexcept { return episode_ns_; }
  [[nodiscard]] bool BootOverBudget() const noexcept { return boot_reported_; }
  // The length of the last unwatched gap (kUnobserved).
  [[nodiscard]] std::uint64_t GapNs() const noexcept { return gap_ns_; }
  // The gap limit in force: max_sample_gap_ns or two periods; 0 when gaps
  // are not judged.
  [[nodiscard]] std::uint64_t MaxSampleGapNs() const noexcept;
  [[nodiscard]] const HangDetectorConfig& Config() const noexcept {
    return config_;
  }

 private:
  HangDetectorConfig config_;
  std::uint64_t boot_start_ns_ = 0;
  std::uint64_t last_iterates_ = 0;
  // The sample that last saw progress, and the one that last saw progress,
  // an excuse or the end of an unwatched gap (the periods are counted from
  // it).
  std::uint64_t progress_ns_ = 0;
  std::uint64_t count_from_ns_ = 0;
  // The last armed sample, and the last unwatched gap.
  std::uint64_t last_sample_ns_ = 0;
  std::uint64_t gap_ns_ = 0;
  std::uint64_t stall_ns_ = 0;
  std::uint64_t episode_ns_ = 0;
  std::uint64_t hangs_ = 0;
  std::uint32_t stalled_periods_ = 0;
  bool armed_ = false;
  bool disarmed_ = false;
  bool boot_reported_ = false;
  bool in_hang_ = false;
  bool hang_in_driver_ = false;
};

}  // namespace se::crash

#endif  // SE_CRASH_HANG_DETECTOR_H_
