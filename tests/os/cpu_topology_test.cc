// Copyright 2026 The simple-engine-2 Authors.

#include "se_os/cpu_topology.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <set>
#include <thread>
#include <vector>

#include "se_core/jobs.h"

namespace se {
namespace {

// Builds a synthetic topology: |p_cores| cores of class 1 with |p_smt|
// logical processors each, then |e_cores| cores of class 0 with one, all on
// last-level cache |llc|. OS ids count up from 100 so they differ from the
// indices.
CpuTopology Synthetic(int p_cores, int p_smt, int e_cores,
                      std::uint8_t p_class = 1, std::uint32_t llc = 0) {
  CpuTopology topology;
  std::uint32_t next_id = 100;
  for (int i = 0; i < p_cores; ++i) {
    CpuCore core;
    core.efficiency_class = p_class;
    core.llc_id = llc;
    for (int t = 0; t < p_smt; ++t) core.logical.push_back(next_id++);
    topology.cores.push_back(core);
  }
  for (int i = 0; i < e_cores; ++i) {
    CpuCore core;
    core.efficiency_class = 0;
    core.llc_id = llc;
    core.logical.push_back(next_id++);
    topology.cores.push_back(core);
  }
  topology.logical_count = next_id - 100;
  return topology;
}

// The design's thread table: W_max and the PerformanceOnly count.
TEST_CASE("Pool sizes for the design's machines", "[cpu_topology]") {
  // Ryzen 7 7700X: 8 cores, 16 threads, one L3, all one class.
  const CpuTopology r7700x = Synthetic(8, 2, 0, /*p_class=*/0);
  CHECK(r7700x.Homogeneous());
  REQUIRE(r7700x.logical_count == 16);
  REQUIRE(ForkPoolSize(r7700x) == 7);
  REQUIRE(PerformanceOnlyCount(r7700x) == 7);

  // A 4-core laptop with SMT.
  const CpuTopology laptop = Synthetic(4, 2, 0, /*p_class=*/0);
  REQUIRE(ForkPoolSize(laptop) == 2);
  REQUIRE(PerformanceOnlyCount(laptop) == 2);

  // 2P+8E: P-cores with SMT, E-cores without, one shared L3.
  const CpuTopology hybrid_small = Synthetic(2, 2, 8);
  CHECK_FALSE(hybrid_small.Homogeneous());
  REQUIRE(ForkPoolSize(hybrid_small) == 8);
  REQUIRE(PerformanceOnlyCount(hybrid_small) == 1);

  // 6P+8E.
  const CpuTopology hybrid_large = Synthetic(6, 2, 8);
  REQUIRE(ForkPoolSize(hybrid_large) == 12);
  REQUIRE(PerformanceOnlyCount(hybrid_large) == 5);
}

TEST_CASE("Small and large machines", "[cpu_topology]") {
  REQUIRE(ForkPoolSize(Synthetic(2, 2, 0, 0)) == 0);
  REQUIRE(PerformanceOnlyCount(Synthetic(2, 2, 0, 0)) == 0);
  REQUIRE(ForkPoolSize(Synthetic(1, 1, 0, 0)) == 0);
  REQUIRE(ForkPoolSize(Synthetic(3, 1, 0, 0)) == 1);
  // Homogeneous 7 cores still reserve 2.
  REQUIRE(ForkPoolSize(Synthetic(7, 2, 0, 0)) == 5);
  // The cap.
  REQUIRE(ForkPoolSize(Synthetic(32, 2, 0, 0)) == 15);
  REQUIRE(PerformanceOnlyCount(Synthetic(32, 2, 0, 0)) == 15);
  REQUIRE(ForkPoolSize(CpuTopology{}) == 0);
  REQUIRE(PerformanceOnlyCount(CpuTopology{}) == 0);
}

TEST_CASE("PerformanceOnly counts the P-cores on main's cache",
          "[cpu_topology]") {
  // Two CCDs of 8 cores (a 7950X): main's L3 has 8 cores.
  CpuTopology two_ccd = Synthetic(8, 2, 0, 0, /*llc=*/0);
  const CpuTopology second = Synthetic(8, 2, 0, 0, /*llc=*/1);
  for (CpuCore core : second.cores) {
    for (std::uint32_t& id : core.logical) id += 1000;
    two_ccd.cores.push_back(core);
  }
  two_ccd.logical_count = 32;
  REQUIRE(ForkPoolSize(two_ccd) == 15);
  REQUIRE(MainLlc(two_ccd) == 0);
  REQUIRE(PerformanceOnlyCount(two_ccd) == 7);
}

TEST_CASE("CPU sets exclude SMT siblings", "[cpu_topology]") {
  const CpuTopology topology = Synthetic(6, 2, 8);
  // P-cores have ids 100..111 in sibling pairs; E-cores 112..119.
  const CpuSetRequest main = MainCpuSet(topology);
  CHECK(main.os_ids ==
        std::vector<std::uint32_t>{100, 102, 104, 106, 108, 110});
  CHECK(WorkerCpuSet(topology, jobs::CoreClass::kPerformanceOnly).os_ids ==
        main.os_ids);
  const CpuSetRequest any =
      WorkerCpuSet(topology, jobs::CoreClass::kAnyPhysical);
  REQUIRE(any.os_ids.size() == 14);
  REQUIRE(any.os_ids.front() == 100);
  REQUIRE(any.os_ids.back() == 119);
  for (const std::uint32_t id : {101U, 103U, 105U, 107U, 109U, 111U}) {
    CHECK(std::set<std::uint32_t>(any.os_ids.begin(), any.os_ids.end())
              .count(id) == 0);
  }
  CHECK(SamplerCpuSet(topology).os_ids == any.os_ids);
}

TEST_CASE("This machine's topology reads back", "[cpu_topology]") {
  const auto topology = ReadCpuTopology();
  REQUIRE(topology.has_value());
  REQUIRE_FALSE(topology->cores.empty());
  std::uint32_t logical = 0;
  std::set<std::uint32_t> ids;
  for (const CpuCore& core : topology->cores) {
    REQUIRE_FALSE(core.logical.empty());
    logical += static_cast<std::uint32_t>(core.logical.size());
    ids.insert(core.logical.begin(), core.logical.end());
  }
  CHECK(logical == topology->logical_count);
  CHECK(ids.size() == logical);
  CHECK(logical == std::thread::hardware_concurrency());
  REQUIRE(ForkPoolSize(*topology) >= 0);
  CHECK(PerformanceOnlyCount(*topology) <= ForkPoolSize(*topology));
  CHECK(MainCpuSet(*topology).os_ids.size() <= topology->PhysicalCores());
}

}  // namespace
}  // namespace se
