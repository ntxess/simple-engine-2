// Copyright 2026 The simple-engine-2 Authors.
//
// The se::fs fault and delay injector (docs/milestone-specs.md M9, test
// hooks): a throttled disk and another process's sharing violation, for the
// background lane's tests. In builds with SE_FS_TEST_HOOKS (every build but
// Release) the file streams consult it:
//
//   write_step_delay_ms   every FileWriter::Write sleeps this long first
//   read_step_delay_ms    every FileReader::Read sleeps this long first
//   rename_busy_ms        TryReplace reports kBusy until this long after
//                         the first attempt it refused (another process
//                         holding the destination for that long)
//
// The settings are atomics: a test sets them on its thread while the lane
// runs. Release builds compile the injector out (SetFsFaults does nothing).

#ifndef SE_PLATFORM_FS_FAULTS_H_
#define SE_PLATFORM_FS_FAULTS_H_

#include <cstdint>

#ifndef SE_FS_TEST_HOOKS
#define SE_FS_TEST_HOOKS 0
#endif

namespace se::fs {

inline constexpr bool kFsFaultHooks = SE_FS_TEST_HOOKS != 0;

struct FsFaults {
  std::uint32_t write_step_delay_ms = 0;
  std::uint32_t read_step_delay_ms = 0;
  std::uint32_t rename_busy_ms = 0;
};

struct FsFaultStats {
  std::uint64_t writes_delayed = 0;
  std::uint64_t reads_delayed = 0;
  std::uint64_t renames_refused = 0;
};

// Replaces the injector's settings; the busy window restarts.
void SetFsFaults(const FsFaults& faults) noexcept;
// No faults.
void ClearFsFaults() noexcept;
[[nodiscard]] FsFaultStats GetFsFaultStats() noexcept;

namespace internal {
// Called by the file streams.
void BeforeWriteStep() noexcept;
void BeforeReadStep() noexcept;
// True when TryReplace must report kBusy now.
[[nodiscard]] bool RefuseReplace() noexcept;
}  // namespace internal

}  // namespace se::fs

#endif  // SE_PLATFORM_FS_FAULTS_H_
