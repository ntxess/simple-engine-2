// Copyright 2026 The simple-engine-2 Authors.
//
// The inline ForkJoinPool and ParallelView: chunk boundaries depend only on
// (count, grain), chunks run in order with t_in_fork set, nested forks run
// inline through NestedParallelFor (checked by the analysis, no opt-out),
// breadcrumbs return to the outer chunk, exceptions propagate, and
// ParallelView writes match a serial loop.

#include "se_core/parallel_view.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <entt/core/type_info.hpp>
#include <entt/entity/registry.hpp>
#include <stdexcept>
#include <utility>
#include <vector>

#include "se_core/breadcrumb.h"
#include "se_core/component_registry.h"
#include "se_core/core_components.h"
#include "se_core/jobs.h"
#include "se_core/thread_role.h"
#include "se_core/world.h"
#include "support/test_roles.h"

namespace se {
namespace {

const ComponentRegistry& CoreRegistry() {
  static const ComponentRegistry kRegistry = [] {
    ComponentRegistry r;
    RegisterCoreComponents(r);
    r.Seal();
    return r;
  }();
  return kRegistry;
}

TEST_CASE("Chunk boundaries depend only on count and grain", "[jobs]") {
  REQUIRE(jobs::ChunkCount(0, 16) == 0);
  REQUIRE(jobs::ChunkCount(1, 16) == 1);
  REQUIRE(jobs::ChunkCount(16, 16) == 1);
  REQUIRE(jobs::ChunkCount(17, 16) == 2);
  REQUIRE(jobs::ChunkCount(1000, 64) == 16);
  REQUIRE(jobs::ChunkCount(0xFFFFFFFFU, 0xFFFFFFFFU) == 1);
  REQUIRE(jobs::ChunkCount(10, 0) == 0);

  const jobs::ChunkRange last = jobs::ChunkAt(1000, 64, 15);
  REQUIRE(last.chunk == 15);
  REQUIRE(last.begin == 960);
  REQUIRE(last.end == 1000);
  // Chunks tile [0, count) exactly.
  for (const std::uint32_t grain : {1U, 3U, 7U, 64U, 1024U}) {
    for (const std::uint32_t count : {0U, 1U, 5U, 63U, 64U, 65U, 5000U}) {
      std::uint32_t covered = 0;
      for (std::uint32_t c = 0; c < jobs::ChunkCount(count, grain); ++c) {
        const jobs::ChunkRange r = jobs::ChunkAt(count, grain, c);
        REQUIRE(r.begin == covered);
        REQUIRE(r.end > r.begin);
        REQUIRE(r.end - r.begin <= grain);
        covered = r.end;
      }
      REQUIRE(covered == count);
    }
  }
}

TEST_CASE("ParallelFor runs every chunk in order inside a fork", "[jobs]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  jobs::ForkJoinPool pool;
  REQUIRE(pool.ActiveWorkers() == 0);
  jobs::ForkSite site{.name = "test", .grain = 10};
  std::vector<jobs::ChunkRange> seen;
  bool all_in_fork = true;
  CHECK_FALSE(jobs::t_in_fork);
  pool.ParallelFor(site, 35, [&](jobs::ChunkRange r) {
    all_in_fork = all_in_fork && jobs::t_in_fork;
    seen.push_back(r);
  });
  CHECK_FALSE(jobs::t_in_fork);
  CHECK(all_in_fork);
  REQUIRE(seen.size() == 4);
  for (std::uint32_t i = 0; i < 4; ++i) {
    CHECK(seen[i].chunk == i);
    CHECK(seen[i].begin == i * 10);
  }
  REQUIRE(seen[3].end == 35);
  REQUIRE(site.stats.forks == 1);
  REQUIRE(site.stats.inline_forks == 1);
  REQUIRE(site.stats.chunks == 4);
}

// Runs inside a chunk: forks again, recording each inner chunk and the
// breadcrumb chunk it runs under.
void NestedFork(const jobs::ForkSite& inner, std::vector<int>& order,
                std::vector<int>& crumbs, int outer)
    SE_REQUIRES(jobs::g_fork_chunk) {
  jobs::NestedParallelFor(inner, 2, [&](jobs::ChunkRange r) {
    order.push_back(outer * 10 + static_cast<int>(r.chunk));
    crumbs.push_back(ThreadBreadcrumb().Fields().chunk);
  });
}

TEST_CASE("Nested forks run inline", "[jobs]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  jobs::ForkJoinPool pool;
  jobs::ForkSite outer{.name = "outer", .grain = 1};
  jobs::ForkSite inner{.name = "inner", .grain = 1};
  std::vector<int> order;
  std::vector<int> inner_crumbs;
  std::vector<int> outer_crumbs;
  ThreadBreadcrumb().SetChunk(7);  // what the thread showed before the fork
  // The chunk lambda declares the fork capability it runs under; the
  // analysis checks both the declaration and the nested call.
  pool.ParallelFor(
      outer, 3, [&](jobs::ChunkRange r) SE_REQUIRES(jobs::g_fork_chunk) {
        NestedFork(inner, order, inner_crumbs, static_cast<int>(r.chunk));
        CHECK(jobs::t_in_fork);
        // The nested fork gave the breadcrumb back to this chunk.
        outer_crumbs.push_back(ThreadBreadcrumb().Fields().chunk);
      });
  CHECK_FALSE(jobs::t_in_fork);
  CHECK(order == std::vector<int>{0, 1, 10, 11, 20, 21});
  CHECK(inner_crumbs == std::vector<int>{0, 1, 0, 1, 0, 1});
  CHECK(outer_crumbs == std::vector<int>{0, 1, 2});
  REQUIRE(ThreadBreadcrumb().Fields().chunk == 7);
  // Stats belong to the owner thread: the nested site's stay untouched.
  REQUIRE(outer.stats.forks == 1);
  REQUIRE(outer.stats.chunks == 3);
  REQUIRE(inner.stats.forks == 0);
  ThreadBreadcrumb().SetChunk(0);
}

TEST_CASE("An exception from a chunk propagates to the caller", "[jobs]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  jobs::ForkJoinPool pool;
  jobs::ForkSite site{.name = "throws", .grain = 1};
  int ran = 0;
  CHECK_THROWS_AS(pool.ParallelFor(site, 5,
                                   [&](jobs::ChunkRange r) {
                                     ++ran;
                                     if (r.chunk == 2) {
                                       throw std::runtime_error("chunk 2");
                                     }
                                   }),
                  std::runtime_error);
  REQUIRE(ran == 3);
  CHECK_FALSE(jobs::t_in_fork);
}

TEST_CASE("ParallelView writes match a serial loop", "[parallel_view]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(CoreRegistry(), {});
  for (std::uint64_t i = 0; i < 1000; ++i) {
    const entt::entity e = world.Create(Guid{0, i + 1});
    world.Reg().emplace<Transform>(e, Vec2{static_cast<float>(i), 0.0F}, 0.0F,
                                   Vec2{1.0F, 1.0F}, Tick{0});
    if (i % 4 != 0) world.Reg().emplace<Name>(e, "named");
  }
  static const std::array<entt::id_type, 1> kReads = {
      entt::type_hash<Name>::value()};
  static const std::array<entt::id_type, 1> kWrites = {
      entt::type_hash<Transform>::value()};
  jobs::ForkSite site{
      .name = "move", .grain = 64, .reads = kReads, .writes = kWrites};
  jobs::ForkJoinPool pool;

  auto view = world.Parallel<Transform, Name>(site);
  REQUIRE(view.size() == 1000);
  int visited = 0;
  pool.ParallelFor(site, view.size(), [&](jobs::ChunkRange r) {
    view.ForChunk(r, [&](entt::entity e) {
      ++visited;
      Transform& t = view.Write<Transform>(e);
      t.pos.y =
          t.pos.x * 2.0F + static_cast<float>(view.Read<Name>(e).value.size());
    });
  });
  REQUIRE(visited == 750);
  for (const auto [e, t] : world.CReg().view<const Transform>().each()) {
    const bool named = world.CReg().all_of<Name>(e);
    CHECK(t.pos.y == (named ? t.pos.x * 2.0F + 5.0F : 0.0F));
  }
  REQUIRE(site.stats.chunks == 16);
}

}  // namespace
}  // namespace se
