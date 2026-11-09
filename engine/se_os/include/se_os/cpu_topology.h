// Copyright 2026 The simple-engine-2 Authors.
//
// The CPU topology and the thread-placement rules built on it
// (docs/design/mt-design.md "Threads", fork-join workers): how many fork
// workers the pool may have, how many a PerformanceOnly site may use, and
// the CPU sets for main, the workers and the samplers. The functions are
// pure, so they are tested on synthetic topologies (the 7700X, a 4-core
// laptop, 2P+8E and 6P+8E).

#ifndef SE_OS_CPU_TOPOLOGY_H_
#define SE_OS_CPU_TOPOLOGY_H_

#include <cstdint>
#include <expected>
#include <vector>

#include "se_core/error.h"
#include "se_core/jobs.h"
#include "se_os/thread.h"

namespace se {

// One physical core.
struct CpuCore {
  // Higher is faster (Windows EfficiencyClass); every core has the same
  // class on a homogeneous CPU.
  std::uint8_t efficiency_class = 0;
  // The last-level cache the core shares with others.
  std::uint32_t llc_id = 0;
  // The OS ids of the core's logical processors (CPU set ids on Windows,
  // CPU numbers on Linux): the first is the core's primary, the rest are
  // its SMT siblings.
  std::vector<std::uint32_t> logical;
};

struct CpuTopology {
  std::uint32_t logical_count = 0;
  std::vector<CpuCore> cores;

  [[nodiscard]] std::uint32_t PhysicalCores() const noexcept {
    return static_cast<std::uint32_t>(cores.size());
  }
  // True when every core has the same efficiency class.
  [[nodiscard]] bool Homogeneous() const noexcept;
};

// Reads the machine's topology: GetSystemCpuSetInformation on Windows,
// /sys/devices/system/cpu on Linux.
[[nodiscard]] std::expected<CpuTopology, Error> ReadCpuTopology();

// The fork pool's size W_max: physical cores - 2, or - 1 on a homogeneous
// CPU with 8 or more cores, at most 15, and 0 below 3 physical cores.
// 7700X 7, 4-core laptop 2, 2P+8E 8, 6P+8E 12.
[[nodiscard]] int ForkPoolSize(const CpuTopology& topology) noexcept;

// Workers a PerformanceOnly site may use: the P-cores (highest efficiency
// class) on main's last-level cache, minus 1 for main, and never more than
// the pool (ADR-031). 7700X 7, 4-core laptop 2, 2P+8E 1, 6P+8E 5.
[[nodiscard]] int PerformanceOnlyCount(const CpuTopology& topology);

// The last-level cache main runs on: the one with the most P-cores (the
// lowest id on a tie).
[[nodiscard]] std::uint32_t MainLlc(const CpuTopology& topology);

// CPU sets (SMT siblings excluded: each core's primary only).
// Main: the P-cores on main's last-level cache.
[[nodiscard]] CpuSetRequest MainCpuSet(const CpuTopology& topology);
// Fork workers: every core for kAnyPhysical; main's set for
// kPerformanceOnly.
[[nodiscard]] CpuSetRequest WorkerCpuSet(const CpuTopology& topology,
                                         jobs::CoreClass cores);
// The input and vblank samplers: every core.
[[nodiscard]] CpuSetRequest SamplerCpuSet(const CpuTopology& topology);

}  // namespace se

#endif  // SE_OS_CPU_TOPOLOGY_H_
