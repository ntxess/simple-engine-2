// Copyright 2026 The simple-engine-2 Authors.
//
// Process-wide floating-point configuration (design change 20). The MSVC x64
// CRT chooses FMA3 or plain SSE2 code paths for its maths by CPU, so a replay
// could diverge between machines; ConfigureProcess() turns the FMA3 paths
// off. clang-cl links the same UCRT, so the call applies there too. Other
// platforms have nothing to configure.

#ifndef SE_CORE_FP_H_
#define SE_CORE_FP_H_

#include <cstdint>

namespace se::fp {

// Call once at startup, before any simulation code runs.
void ConfigureProcess() noexcept;

// Forces the CRT FMA3 paths on or off; false where the CRT has no switch.
bool SetFma3ForTest(bool enable) noexcept;

// 1 if the CRT uses its FMA3 paths, 0 if not, -1 where there is no switch.
[[nodiscard]] int Fma3State() noexcept;

// CPU feature bits for replay headers.
enum CpuFeature : std::uint64_t {
  kCpuSse42 = 1U << 0,
  kCpuAvx = 1U << 1,
  kCpuAvx2 = 1U << 2,
  kCpuFma3 = 1U << 3,
  kCpuAvx512f = 1U << 4,
};
[[nodiscard]] std::uint64_t CpuFeatureBits() noexcept;

}  // namespace se::fp

#endif  // SE_CORE_FP_H_
