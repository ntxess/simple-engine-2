// Copyright 2026 The simple-engine-2 Authors.

#include "se_os/unbiased_clock.h"

#include <cstdint>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

namespace se {

#if defined(_WIN32)

namespace {

using PreciseFn = VOID(WINAPI*)(PULONGLONG);

// QueryUnbiasedInterruptTimePrecise is exported by KernelBase (Windows 10)
// but not by kernel32.lib, so it is looked up once; the coarse
// QueryUnbiasedInterruptTime (kernel32) is the fallback.
PreciseFn LookUpPrecise() noexcept {
  const HMODULE module = GetModuleHandleW(L"kernelbase.dll");
  if (module == nullptr) return nullptr;
  return reinterpret_cast<PreciseFn>(
      GetProcAddress(module, "QueryUnbiasedInterruptTimePrecise"));
}

}  // namespace

std::uint64_t UnbiasedNowNs() noexcept {
  static const PreciseFn kPrecise = LookUpPrecise();
  ULONGLONG units = 0;
  if (kPrecise != nullptr) {
    kPrecise(&units);
  } else {
    QueryUnbiasedInterruptTime(&units);
  }
  return static_cast<std::uint64_t>(units) * 100;
}

#else

std::uint64_t UnbiasedNowNs() noexcept {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(now.tv_nsec);
}

#endif

}  // namespace se
