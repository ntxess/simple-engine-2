// Copyright 2026 The simple-engine-2 Authors.
//
// HangDetector: the watchdog's pure hang rules (docs/design/mt-design.md
// "watchdog", "Hangs"; docs/milestone-specs.md "Integration (M4a + M4b +
// M5)"). The watchdog thread feeds it one HangSample per period, read from
// the atomics main writes, and acts on the verdict; the detector itself
// keeps no clock and touches nothing shared, so it is tested without
// threads.
//
// Until the first iterate arms it, only the boot budget applies: a boot
// that takes longer than boot_budget_ns (30 s) is reported once, unless a
// debugger is attached. Once armed, a period counts as stalled when main's
// iterate counter did not move and nothing excuses it (Suspended, Modal,
// the heartbeat flag or a debugger); hang_periods (5) consecutive stalled
// periods are a hang, reported once per episode. M4 builds the interface
// with declare_hangs off: stalls are counted, hangs are never declared
// until M12 turns it on.

#ifndef SE_CRASH_HANG_DETECTOR_H_
#define SE_CRASH_HANG_DETECTOR_H_

#include <cstdint>

namespace se::crash {

struct HangDetectorConfig {
  // How long the boot may take before the first iterate.
  std::uint64_t boot_budget_ns = 30'000'000'000;
  // Consecutive stalled periods that make a hang.
  std::uint32_t hang_periods = 5;
  // Off until M12: stalls are counted but no hang is declared.
  bool declare_hangs = false;
};

// One period's reading of main's progress (atomics, read by the watchdog).
struct HangSample {
  // The unbiased clock (se::UnbiasedNowNs), which excludes system sleep.
  std::uint64_t now_ns = 0;
  // Main's iterate counter.
  std::uint64_t iterates = 0;
  bool suspended = false;
  bool modal = false;
  bool heartbeat = false;
  bool debugger = false;
};

enum class HangVerdict : std::uint8_t {
  // Booting within the budget, or main made progress.
  kProgress,
  // No progress, but Suspended, Modal, the heartbeat or a debugger excuses
  // it; the stall count starts over.
  kExcused,
  // No progress and nothing excuses it (fewer than hang_periods so far, or
  // hang declaration is off).
  kStalled,
  // The boot took longer than its budget (once).
  kBootOverBudget,
  // hang_periods stalled periods in a row (once per episode).
  kHang,
};

class HangDetector {
 public:
  explicit HangDetector(const HangDetectorConfig& config) noexcept
      : config_(config) {}

  // The boot started at |now_ns|.
  void Start(std::uint64_t now_ns) noexcept;
  // The first iterate ran: the boot budget no longer applies and progress
  // is measured from |iterates|.
  void Arm(std::uint64_t iterates) noexcept;

  // Judges one period.
  [[nodiscard]] HangVerdict Observe(const HangSample& sample) noexcept;

  [[nodiscard]] bool Armed() const noexcept { return armed_; }
  [[nodiscard]] std::uint32_t StalledPeriods() const noexcept {
    return stalled_periods_;
  }
  // True from a declared hang until main makes progress again.
  [[nodiscard]] bool InHang() const noexcept { return in_hang_; }
  [[nodiscard]] bool BootOverBudget() const noexcept { return boot_reported_; }
  [[nodiscard]] const HangDetectorConfig& Config() const noexcept {
    return config_;
  }

 private:
  HangDetectorConfig config_;
  std::uint64_t boot_start_ns_ = 0;
  std::uint64_t last_iterates_ = 0;
  std::uint32_t stalled_periods_ = 0;
  bool armed_ = false;
  bool boot_reported_ = false;
  bool in_hang_ = false;
};

}  // namespace se::crash

#endif  // SE_CRASH_HANG_DETECTOR_H_
