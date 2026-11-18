// Copyright 2026 The simple-engine-2 Authors.

#include "se_os/cpu_topology.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "se_core/error.h"
#include "se_core/jobs.h"
#include "se_os/thread.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace se {
namespace {

constexpr int kMaxForkWorkers = 15;

std::uint8_t PerformanceClass(const CpuTopology& topology) noexcept {
  std::uint8_t best = 0;
  for (const CpuCore& core : topology.cores) {
    best = std::max(best, core.efficiency_class);
  }
  return best;
}

#if defined(_WIN32)

std::expected<CpuTopology, Error> ReadPlatformTopology() {
  ULONG length = 0;
  const HANDLE process = GetCurrentProcess();
  GetSystemCpuSetInformation(nullptr, 0, &length, process, 0);
  if (length == 0) {
    return Fail(ErrorCode::kIo,
                std::format("GetSystemCpuSetInformation failed (error {})",
                            GetLastError()));
  }
  // 8-byte aligned storage for the variable-size entries.
  std::vector<std::uint64_t> buffer((length + 7) / 8);
  auto* first = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data());
  if (GetSystemCpuSetInformation(first, length, &length, process, 0) == FALSE) {
    return Fail(ErrorCode::kIo,
                std::format("GetSystemCpuSetInformation failed (error {})",
                            GetLastError()));
  }
  struct Logical {
    WORD group;
    BYTE index;
    BYTE core;
    BYTE llc;
    BYTE efficiency;
    DWORD id;
  };
  std::vector<Logical> logical;
  const auto* bytes = reinterpret_cast<const std::byte*>(buffer.data());
  for (ULONG offset = 0; offset < length;) {
    const auto* entry =
        reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(bytes + offset);
    if (entry->Size == 0) break;
    if (entry->Type == CpuSetInformation) {
      const auto& set = entry->CpuSet;
      logical.push_back({set.Group, set.LogicalProcessorIndex, set.CoreIndex,
                         set.LastLevelCacheIndex, set.EfficiencyClass, set.Id});
    }
    offset += entry->Size;
  }
  std::sort(logical.begin(), logical.end(),
            [](const Logical& a, const Logical& b) {
              return std::pair(a.group, a.index) < std::pair(b.group, b.index);
            });
  CpuTopology topology;
  topology.logical_count = static_cast<std::uint32_t>(logical.size());
  std::map<std::pair<WORD, BYTE>, std::size_t> core_of;
  for (const Logical& l : logical) {
    const auto key = std::pair(l.group, l.core);
    auto [it, inserted] = core_of.try_emplace(key, topology.cores.size());
    if (inserted) {
      CpuCore core;
      core.efficiency_class = l.efficiency;
      core.llc_id = (static_cast<std::uint32_t>(l.group) << 8) | l.llc;
      topology.cores.push_back(std::move(core));
    }
    topology.cores[it->second].logical.push_back(l.id);
  }
  return topology;
}

#else

// The first line of a sysfs file, or empty.
std::string ReadLine(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "r");
  if (file == nullptr) return {};
  char buffer[4096] = {};
  const bool ok = std::fgets(buffer, sizeof(buffer), file) != nullptr;
  std::fclose(file);
  if (!ok) return {};
  std::string line(buffer);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
    line.pop_back();
  }
  return line;
}

// Parses a leading decimal number of |text|.
bool ParseNumber(std::string_view text, int& value) {
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc() && end != text.data();
}

// Parses a CPU list such as "0-3,8,10-11".
std::vector<std::uint32_t> ParseCpuList(std::string_view text) {
  std::vector<std::uint32_t> cpus;
  std::size_t pos = 0;
  while (pos < text.size()) {
    std::size_t end = text.find(',', pos);
    if (end == std::string_view::npos) end = text.size();
    const std::string_view part = text.substr(pos, end - pos);
    const std::size_t dash = part.find('-');
    int first = 0;
    int last = 0;
    if (ParseNumber(part.substr(0, dash), first) && first >= 0) {
      if (dash == std::string_view::npos ||
          !ParseNumber(part.substr(dash + 1), last)) {
        last = first;
      }
      for (int cpu = first; cpu <= last; ++cpu) {
        cpus.push_back(static_cast<std::uint32_t>(cpu));
      }
    }
    pos = end + 1;
  }
  return cpus;
}

int ReadInt(const std::string& path, int fallback) {
  const std::string line = ReadLine(path);
  int value = 0;
  return ParseNumber(line, value) ? value : fallback;
}

// The last-level cache of |cpu|: the id of its highest cache level.
std::uint32_t LlcOf(std::uint32_t cpu) {
  const std::string base =
      std::format("/sys/devices/system/cpu/cpu{}/cache/", cpu);
  int best_level = -1;
  std::uint32_t llc = 0;
  for (int index = 0; index < 8; ++index) {
    const std::string dir = std::format("{}index{}/", base, index);
    const int level = ReadInt(dir + "level", -1);
    if (level < 0) continue;
    if (level < best_level) continue;
    best_level = level;
    const int id = ReadInt(dir + "id", -1);
    if (id >= 0) {
      llc = static_cast<std::uint32_t>(id);
    } else {
      const auto shared = ParseCpuList(ReadLine(dir + "shared_cpu_list"));
      llc = shared.empty() ? 0 : shared.front();
    }
  }
  return llc;
}

std::expected<CpuTopology, Error> ReadPlatformTopology() {
  const std::vector<std::uint32_t> online =
      ParseCpuList(ReadLine("/sys/devices/system/cpu/online"));
  if (online.empty()) {
    return Fail(ErrorCode::kIo, "cannot read /sys/devices/system/cpu/online");
  }
  // Hybrid Intel parts list their P-cores and E-cores; elsewhere a
  // cpu_capacity file may rank the cores.
  const std::vector<std::uint32_t> p_cores =
      ParseCpuList(ReadLine("/sys/devices/cpu_core/cpus"));
  CpuTopology topology;
  topology.logical_count = static_cast<std::uint32_t>(online.size());
  std::map<std::pair<int, int>, std::size_t> core_of;
  std::vector<int> capacity;
  for (const std::uint32_t cpu : online) {
    const std::string topo =
        std::format("/sys/devices/system/cpu/cpu{}/topology/", cpu);
    const int package = ReadInt(topo + "physical_package_id", 0);
    const int core_id = ReadInt(topo + "core_id", static_cast<int>(cpu));
    const auto key = std::pair(package, core_id);
    auto [it, inserted] = core_of.try_emplace(key, topology.cores.size());
    if (inserted) {
      CpuCore core;
      core.llc_id = LlcOf(cpu);
      if (!p_cores.empty()) {
        core.efficiency_class =
            std::find(p_cores.begin(), p_cores.end(), cpu) != p_cores.end() ? 1
                                                                            : 0;
      }
      topology.cores.push_back(std::move(core));
      capacity.push_back(ReadInt(
          std::format("/sys/devices/system/cpu/cpu{}/cpu_capacity", cpu), 0));
    }
    topology.cores[it->second].logical.push_back(cpu);
  }
  if (p_cores.empty()) {
    // Rank distinct capacities: the largest gets the highest class.
    std::vector<int> distinct = capacity;
    std::sort(distinct.begin(), distinct.end());
    distinct.erase(std::unique(distinct.begin(), distinct.end()),
                   distinct.end());
    for (std::size_t i = 0; i < topology.cores.size(); ++i) {
      const auto rank =
          std::lower_bound(distinct.begin(), distinct.end(), capacity[i]) -
          distinct.begin();
      topology.cores[i].efficiency_class = static_cast<std::uint8_t>(rank);
    }
  }
  return topology;
}

#endif

CpuSetRequest Primaries(const CpuTopology& topology, bool p_cores_on_main_llc) {
  CpuSetRequest request;
  const std::uint8_t p_class = PerformanceClass(topology);
  const std::uint32_t llc = MainLlc(topology);
  for (const CpuCore& core : topology.cores) {
    if (core.logical.empty()) continue;
    if (p_cores_on_main_llc &&
        (core.efficiency_class != p_class || core.llc_id != llc)) {
      continue;
    }
    request.os_ids.push_back(core.logical.front());
  }
  return request;
}

}  // namespace

bool CpuTopology::Homogeneous() const noexcept {
  return std::all_of(cores.begin(), cores.end(), [&](const CpuCore& core) {
    return core.efficiency_class == cores.front().efficiency_class;
  });
}

std::expected<CpuTopology, Error> ReadCpuTopology() {
  return ReadPlatformTopology();
}

int ForkPoolSize(const CpuTopology& topology) noexcept {
  const auto physical = static_cast<int>(topology.PhysicalCores());
  if (physical < 3) return 0;
  const int reserved = topology.Homogeneous() && physical >= 8 ? 1 : 2;
  return std::min(physical - reserved, kMaxForkWorkers);
}

std::uint32_t MainLlc(const CpuTopology& topology) {
  const std::uint8_t p_class = PerformanceClass(topology);
  std::map<std::uint32_t, int> p_cores_per_llc;
  for (const CpuCore& core : topology.cores) {
    if (core.efficiency_class == p_class) ++p_cores_per_llc[core.llc_id];
  }
  std::uint32_t best = 0;
  int best_count = -1;
  for (const auto& [llc, count] : p_cores_per_llc) {
    if (count > best_count) {
      best = llc;
      best_count = count;
    }
  }
  return best;
}

int PerformanceOnlyCount(const CpuTopology& topology) {
  if (topology.cores.empty()) return 0;
  const std::uint8_t p_class = PerformanceClass(topology);
  const std::uint32_t llc = MainLlc(topology);
  const auto on_main_llc = static_cast<int>(std::count_if(
      topology.cores.begin(), topology.cores.end(), [&](const CpuCore& core) {
        return core.efficiency_class == p_class && core.llc_id == llc;
      }));
  return std::clamp(on_main_llc - 1, 0, ForkPoolSize(topology));
}

CpuSetRequest MainCpuSet(const CpuTopology& topology) {
  return Primaries(topology, true);
}

CpuSetRequest WorkerCpuSet(const CpuTopology& topology, jobs::CoreClass cores) {
  return Primaries(topology, cores == jobs::CoreClass::kPerformanceOnly);
}

CpuSetRequest SamplerCpuSet(const CpuTopology& topology) {
  return Primaries(topology, false);
}

}  // namespace se
