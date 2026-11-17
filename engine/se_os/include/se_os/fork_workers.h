// Copyright 2026 The simple-engine-2 Authors.
//
// The fork pool's worker threads on this machine (docs/design/mt-design.md
// "Threads", fork-join workers; ADR-107). se_core's ForkJoinPool runs the
// protocol but may not create threads (ADR-029); ForkWorkers starts each
// worker with StartThread:
//   - role kForkWorker, OS name "fork-worker-<i>", Qos::kHigh;
//   - workers 0 .. PerformanceOnlyCount - 1 prefer the P-cores on main's
//     last-level cache (WorkerCpuSet(kPerformanceOnly)): the ones a
//     PerformanceOnly site uses;
//   - every other worker prefers the cores of one efficiency class: the
//     cores in order (fastest class first, main's last-level cache first),
//     past main's and the PerformanceOnly workers', wrapping when W exceeds
//     the cores (tests). On a homogeneous CPU that is every core, as
//     WorkerCpuSet(kAnyPhysical) gives.
// CPU sets are soft affinity (SetThreadSelectedCpuSets). Pool sizes come
// from M4's rules: MaxWorkers is ForkPoolSize, PerformanceOnlyWorkers is
// PerformanceOnlyCount.
//
//   jobs::ForkJoinPool::Config config;
//   config.workers = -1;                 // the machine's W_max
//   config.threads = [] { return MakeForkWorkers(); };

#ifndef SE_OS_FORK_WORKERS_H_
#define SE_OS_FORK_WORKERS_H_

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <vector>

#include "se_core/error.h"
#include "se_core/jobs.h"
#include "se_os/cpu_topology.h"
#include "se_os/thread.h"

namespace se {

class ForkWorkers final : public jobs::WorkerThreads {
 public:
  explicit ForkWorkers(CpuTopology topology);
  ~ForkWorkers() override;

  [[nodiscard]] int MaxWorkers() const override;
  [[nodiscard]] int PerformanceOnlyWorkers() const override;
  [[nodiscard]] jobs::WorkerSpec Placement(int index, int count) const override;
  std::expected<void, Error> Start(
      const jobs::WorkerSpec& spec,
      std::move_only_function<void()> body) override;
  void JoinAll() noexcept override;
  [[nodiscard]] bool OnBattery() const override;

  // The CPU set worker |spec| prefers.
  [[nodiscard]] CpuSetRequest CpusFor(const jobs::WorkerSpec& spec) const;

 private:
  CpuTopology topology_;
  std::vector<Thread> threads_;
};

// ForkWorkers on this machine's topology (one core, so W_max 0, when it
// cannot be read), or on |topology|.
[[nodiscard]] std::unique_ptr<jobs::WorkerThreads> MakeForkWorkers();
[[nodiscard]] std::unique_ptr<jobs::WorkerThreads> MakeForkWorkers(
    const CpuTopology& topology);

// The factory for ForkJoinPool::Config::threads.
[[nodiscard]] std::function<std::unique_ptr<jobs::WorkerThreads>()>
ForkWorkersFactory();

// SE_CHAOS_SEED as a number (decimal or 0x hex); 0 when unset or not one.
[[nodiscard]] std::uint64_t ChaosSeedFromEnv();

// True while the machine runs on battery (any thread; se_platform's
// OnBatteryPower asks SDL on main): GetSystemPowerStatus on Windows;
// on Linux a battery and no online mains supply in /sys/class/power_supply.
[[nodiscard]] bool BatteryPowered();

}  // namespace se

#endif  // SE_OS_FORK_WORKERS_H_
