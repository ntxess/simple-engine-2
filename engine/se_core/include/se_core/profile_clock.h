// Copyright 2026 The simple-engine-2 Authors.
//
// A monotonic clock for profiling only (system timing rings, fork stats). It
// never feeds the simulation: no tick result may depend on it.

#ifndef SE_CORE_PROFILE_CLOCK_H_
#define SE_CORE_PROFILE_CLOCK_H_

#include <cstdint>

namespace se {

// Nanoseconds on a monotonic clock with an arbitrary origin.
[[nodiscard]] std::uint64_t ProfileNowNs() noexcept;

}  // namespace se

#endif  // SE_CORE_PROFILE_CLOCK_H_
