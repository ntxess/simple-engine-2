// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/fp.h"

#include <cmath>  // _set_FMA3_enable (MSVC UCRT, x64)
#include <cstdint>

#if defined(_MSC_VER) || defined(__x86_64__)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#define SE_FP_HAVE_CPUID 1
#endif

#if defined(_M_X64) && defined(_MSC_VER)
#define SE_FP_HAVE_FMA3_SWITCH 1
#endif

namespace se::fp {
namespace {

#if defined(SE_FP_HAVE_CPUID)
struct CpuidRegs {
  std::uint32_t eax = 0;
  std::uint32_t ebx = 0;
  std::uint32_t ecx = 0;
  std::uint32_t edx = 0;
};

CpuidRegs Cpuid(std::uint32_t leaf, std::uint32_t subleaf) {
  CpuidRegs regs;
#if defined(_MSC_VER)
  int out[4] = {0, 0, 0, 0};
  __cpuidex(out, static_cast<int>(leaf), static_cast<int>(subleaf));
  regs.eax = static_cast<std::uint32_t>(out[0]);
  regs.ebx = static_cast<std::uint32_t>(out[1]);
  regs.ecx = static_cast<std::uint32_t>(out[2]);
  regs.edx = static_cast<std::uint32_t>(out[3]);
#else
  __cpuid_count(leaf, subleaf, regs.eax, regs.ebx, regs.ecx, regs.edx);
#endif
  return regs;
}
#endif

}  // namespace

void ConfigureProcess() noexcept {
#if defined(SE_FP_HAVE_FMA3_SWITCH)
  static_cast<void>(_set_FMA3_enable(0));
#endif
}

bool SetFma3ForTest(bool enable) noexcept {
#if defined(SE_FP_HAVE_FMA3_SWITCH)
  static_cast<void>(_set_FMA3_enable(enable ? 1 : 0));
  return true;
#else
  static_cast<void>(enable);
  return false;
#endif
}

int Fma3State() noexcept {
#if defined(SE_FP_HAVE_FMA3_SWITCH)
  return _get_FMA3_enable();
#else
  return -1;
#endif
}

std::uint64_t CpuFeatureBits() noexcept {
  std::uint64_t bits = 0;
#if defined(SE_FP_HAVE_CPUID)
  const CpuidRegs leaf0 = Cpuid(0, 0);
  if (leaf0.eax >= 1) {
    const CpuidRegs leaf1 = Cpuid(1, 0);
    if ((leaf1.ecx & (1U << 20)) != 0) bits |= kCpuSse42;
    if ((leaf1.ecx & (1U << 28)) != 0) bits |= kCpuAvx;
    if ((leaf1.ecx & (1U << 12)) != 0) bits |= kCpuFma3;
  }
  if (leaf0.eax >= 7) {
    const CpuidRegs leaf7 = Cpuid(7, 0);
    if ((leaf7.ebx & (1U << 5)) != 0) bits |= kCpuAvx2;
    if ((leaf7.ebx & (1U << 16)) != 0) bits |= kCpuAvx512f;
  }
#endif
  return bits;
}

}  // namespace se::fp
