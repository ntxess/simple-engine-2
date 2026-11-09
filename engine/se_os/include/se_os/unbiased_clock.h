// Copyright 2026 The simple-engine-2 Authors.
//
// A monotonic clock that excludes system sleep, for the watchdog's periods
// (docs/design/mt-design.md "watchdog"): a machine that sleeps for an hour
// must not look like an hour without progress.

#ifndef SE_OS_UNBIASED_CLOCK_H_
#define SE_OS_UNBIASED_CLOCK_H_

#include <cstdint>

namespace se {

// Nanoseconds since an arbitrary origin, not counting time the system was
// asleep: QueryUnbiasedInterruptTimePrecise on Windows (100 ns units),
// CLOCK_MONOTONIC on Linux.
[[nodiscard]] std::uint64_t UnbiasedNowNs() noexcept;

}  // namespace se

#endif  // SE_OS_UNBIASED_CLOCK_H_
