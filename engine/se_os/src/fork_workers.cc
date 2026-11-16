// Copyright 2026 The simple-engine-2 Authors.

#include "se_os/fork_workers.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "se_core/error.h"
#include "se_core/jobs.h"
#include "se_core/log.h"
#include "se_core/thread_role.h"
#include "se_os/cpu_topology.h"
#include "se_os/thread.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>

#include <cstdio>
#include <cstdlib>
#endif

namespace se {
namespace {

std::uint8_t HighestClass(const CpuTopology& topology) noexcept {
  std::uint8_t best = 0;
  for (const CpuCore& core : topology.cores) {
    best = std::max(best, core.efficiency_class);
  }
  return best;
}

// The cores in placement order: fastest class first, main's last-level
// cache first within a class, then the topology's order.
std::vector<const CpuCore*> PlacementOrder(const CpuTopology& topology) {
  const std::uint32_t llc = MainLlc(topology);
  std::vector<const CpuCore*> order;
  order.reserve(topology.cores.size());
  for (const CpuCore& core : topology.cores) order.push_back(&core);
  std::stable_sort(order.begin(), order.end(),
                   [llc](const CpuCore* a, const CpuCore* b) {
                     if (a->efficiency_class != b->efficiency_class) {
                       return a->efficiency_class > b->efficiency_class;
                     }
                     return (a->llc_id == llc) && (b->llc_id != llc);
                   });
  return order;
}

#if !defined(_WIN32)
std::string ReadSmallFile(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return {};
  char buffer[64] = {};
  const std::size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, file);
  std::fclose(file);
  std::string text(buffer, n);
  while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
    text.pop_back();
  }
  return text;
}
#endif

}  // namespace

ForkWorkers::ForkWorkers(CpuTopology topology)
    : topology_(std::move(topology)) {}

ForkWorkers::~ForkWorkers() { JoinAll(); }

int ForkWorkers::MaxWorkers() const { return ForkPoolSize(topology_); }

int ForkWorkers::PerformanceOnlyWorkers() const {
  return PerformanceOnlyCount(topology_);
}

jobs::WorkerSpec ForkWorkers::Placement(int index, int count) const {
  jobs::WorkerSpec spec;
  spec.index = index;
  if (topology_.cores.empty()) return spec;
  const int performance_only = std::min(PerformanceOnlyCount(topology_), count);
  if (index < performance_only) {
    spec.placement = jobs::CoreClass::kPerformanceOnly;
    spec.efficiency_class = HighestClass(topology_);
    return spec;
  }
  // Past main's core (the first) and the PerformanceOnly workers' cores.
  const std::vector<const CpuCore*> order = PlacementOrder(topology_);
  const std::size_t position =
      (static_cast<std::size_t>(index) + 1) % order.size();
  spec.efficiency_class = order[position]->efficiency_class;
  return spec;
}

CpuSetRequest ForkWorkers::CpusFor(const jobs::WorkerSpec& spec) const {
  if (spec.placement == jobs::CoreClass::kPerformanceOnly) {
    return WorkerCpuSet(topology_, jobs::CoreClass::kPerformanceOnly);
  }
  if (topology_.Homogeneous()) {
    return WorkerCpuSet(topology_, jobs::CoreClass::kAnyPhysical);
  }
  CpuSetRequest request;
  for (const CpuCore& core : topology_.cores) {
    if (core.logical.empty() ||
        core.efficiency_class != spec.efficiency_class) {
      continue;
    }
    request.os_ids.push_back(core.logical.front());
  }
  return request;
}

std::expected<void, Error> ForkWorkers::Start(
    const jobs::WorkerSpec& spec, std::move_only_function<void()> body) {
  const std::string name = std::format("fork-worker-{}", spec.index);
  ThreadSpec thread_spec;
  thread_spec.role = ThreadRole::kForkWorker;
  thread_spec.name = name;
  thread_spec.qos = Qos::kHigh;
  thread_spec.cpus = CpusFor(spec);
  auto started = StartThread(
      thread_spec, [run = std::move(body)](
                       const std::stop_token& /*stop*/) mutable { run(); });
  if (!started) return std::unexpected(std::move(started.error()));
  threads_.push_back(std::move(*started));
  return {};
}

void ForkWorkers::JoinAll() noexcept {
  for (Thread& thread : threads_) thread.Join();
  threads_.clear();
}

bool ForkWorkers::OnBattery() const { return BatteryPowered(); }

std::unique_ptr<jobs::WorkerThreads> MakeForkWorkers() {
  auto topology = ReadCpuTopology();
  if (!topology) {
    SE_LOG_WARN("fork workers: the CPU topology is unreadable ({}); W = 0",
                topology.error().message);
    return std::make_unique<ForkWorkers>(CpuTopology{});
  }
  return std::make_unique<ForkWorkers>(std::move(*topology));
}

std::unique_ptr<jobs::WorkerThreads> MakeForkWorkers(
    const CpuTopology& topology) {
  return std::make_unique<ForkWorkers>(topology);
}

std::function<std::unique_ptr<jobs::WorkerThreads>()> ForkWorkersFactory() {
  return [] { return MakeForkWorkers(); };
}

std::uint64_t ChaosSeedFromEnv() {
  std::string text;
#if defined(_WIN32)
  char buffer[64] = {};
  const DWORD n = GetEnvironmentVariableA("SE_CHAOS_SEED", buffer,
                                          static_cast<DWORD>(sizeof(buffer)));
  if (n == 0 || n >= sizeof(buffer)) return 0;
  text.assign(buffer, n);
#else
  const char* value = std::getenv("SE_CHAOS_SEED");
  if (value == nullptr) return 0;
  text = value;
#endif
  int base = 10;
  std::string_view digits = text;
  if (digits.starts_with("0x") || digits.starts_with("0X")) {
    digits.remove_prefix(2);
    base = 16;
  }
  std::uint64_t seed = 0;
  const auto [end, error] =
      std::from_chars(digits.data(), digits.data() + digits.size(), seed, base);
  if (error != std::errc() || end != digits.data() + digits.size()) return 0;
  return seed;
}

bool BatteryPowered() {
#if defined(_WIN32)
  SYSTEM_POWER_STATUS status{};
  if (GetSystemPowerStatus(&status) == FALSE) return false;
  // 0 = offline (battery), 1 = online, 255 = unknown.
  return status.ACLineStatus == 0;
#else
  const std::string root = "/sys/class/power_supply/";
  DIR* dir = opendir(root.c_str());
  if (dir == nullptr) return false;
  bool battery = false;
  bool mains_online = false;
  while (const dirent* entry = readdir(dir)) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    const std::string type = ReadSmallFile(root + name + "/type");
    if (type == "Battery") battery = true;
    if (type == "Mains" && ReadSmallFile(root + name + "/online") == "1") {
      mains_online = true;
    }
  }
  closedir(dir);
  return battery && !mains_online;
#endif
}

}  // namespace se
