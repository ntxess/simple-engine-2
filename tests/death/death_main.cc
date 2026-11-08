// Copyright 2026 The simple-engine-2 Authors.
//
// Death tests: each scenario breaks one kernel contract and must end in a
// failed SE_ASSERT ("SE_ASSERT failed: <expression> -- <message>" on
// stderr, then abort). CTest runs the executable once per scenario and
// matches the message of the assert the scenario is about
// (tests/CMakeLists.txt); a scenario that returns normally, or trips a
// different assert, does not match, so the test fails.
//
//   se_death_tests <scenario>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <thread>

#if defined(_MSC_VER)
#include <crtdbg.h>
#endif

#include <entt/core/type_info.hpp>

#include "se_core/component_registry.h"
#include "se_core/core_components.h"
#include "se_core/jobs.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
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

// Calls that the analysis would reject inside a chunk lambda (that is the
// compile-fail test); here they must fail at run time instead.
void TouchRegistry(World& world) SE_NO_THREAD_SAFETY_ANALYSIS {
  static_cast<void>(world.Reg());
}
void DestroyFromChunk(World& world,
                      entt::entity entity) SE_NO_THREAD_SAFETY_ANALYSIS {
  world.RequestDestroy(entity);
}
void CreateFromChunk(World& world) SE_NO_THREAD_SAFETY_ANALYSIS {
  static_cast<void>(world.Create(Guid{9, 9}));
}
void OwnerForkFromChunk(jobs::ForkJoinPool& pool,
                        jobs::ForkSite& site) SE_NO_THREAD_SAFETY_ANALYSIS {
  pool.ParallelFor(site, 1, [](jobs::ChunkRange) {});
}
void NestedForkFromOwner(const jobs::ForkSite& site)
    SE_NO_THREAD_SAFETY_ANALYSIS {
  jobs::NestedParallelFor(site, 1, [](jobs::ChunkRange) {});
}
void TakeInlineOwner(MainThreadProof proof) {
  const InlineOwnerScope owner(proof);
}

// A World with |count| entities that have Transform and Name.
void Fill(World& world, int count) SE_REQUIRES(g_sim_owner) {
  for (int i = 0; i < count; ++i) {
    const entt::entity e =
        world.Create(Guid{1, static_cast<std::uint64_t>(i + 1)});
    world.Reg().emplace<Transform>(e);
    world.Reg().emplace<Name>(e, "n");
  }
}

const std::array<entt::id_type, 1> kTransformOnly = {
    entt::type_hash<Transform>::value()};

void WorldAccessInFork() {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(CoreRegistry(), {});
  jobs::ForkJoinPool pool;
  jobs::ForkSite site{.name = "death", .grain = 1};
  pool.ParallelFor(site, 1, [&](jobs::ChunkRange) { TouchRegistry(world); });
}

void StructuralCallInFork() {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(CoreRegistry(), {});
  Fill(world, 4);
  const entt::entity victim = *world.Find(Guid{1, 1});
  jobs::ForkSite site{.name = "death", .grain = 2, .writes = kTransformOnly};
  auto view = world.Parallel<Transform>(site);
  jobs::ForkJoinPool pool;
  pool.ParallelFor(site, view.size(), [&](jobs::ChunkRange r) {
    view.ForChunk(r, [&](entt::entity) { DestroyFromChunk(world, victim); });
  });
}

void CreateInFork() {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(CoreRegistry(), {});
  jobs::ForkJoinPool pool;
  jobs::ForkSite site{.name = "death", .grain = 1};
  pool.ParallelFor(site, 1, [&](jobs::ChunkRange) { CreateFromChunk(world); });
}

// A chunk forking through the owner's ParallelFor instead of
// NestedParallelFor.
void OwnerForkInsideChunk() {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  jobs::ForkJoinPool pool;
  jobs::ForkSite outer{.name = "death", .grain = 1};
  jobs::ForkSite inner{.name = "inner", .grain = 1};
  pool.ParallelFor(outer, 1,
                   [&](jobs::ChunkRange) { OwnerForkFromChunk(pool, inner); });
}

void NestedForkOutsideChunk() {
  const jobs::ForkSite site{.name = "death", .grain = 1};
  NestedForkFromOwner(site);
}

void UndeclaredRead() {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(CoreRegistry(), {});
  Fill(world, 4);
  jobs::ForkSite site{.name = "death", .grain = 4, .writes = kTransformOnly};
  auto view = world.Parallel<Transform, Name>(site);
  jobs::ForkJoinPool pool;
  pool.ParallelFor(site, view.size(), [&](jobs::ChunkRange r) {
    view.ForChunk(r, [&](entt::entity e) {
      std::printf("%zu\n", view.Read<Name>(e).value.size());
    });
  });
}

void UndeclaredWrite() {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(CoreRegistry(), {});
  Fill(world, 4);
  jobs::ForkSite site{.name = "death", .grain = 4, .reads = kTransformOnly};
  auto view = world.Parallel<Transform>(site);
  jobs::ForkJoinPool pool;
  pool.ParallelFor(site, view.size(), [&](jobs::ChunkRange r) {
    view.ForChunk(r, [&](entt::entity e) { view.Write<Transform>(e).rot = 1; });
  });
}

void WriteOutsideChunk() {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(CoreRegistry(), {});
  Fill(world, 8);
  jobs::ForkSite site{.name = "death", .grain = 4, .writes = kTransformOnly};
  auto view = world.Parallel<Transform>(site);
  // An entity of the second chunk, written from the first.
  const entt::sparse_set& packed = *world.CReg().storage<Transform>();
  const entt::entity other = packed.data()[6];
  jobs::ForkJoinPool pool;
  pool.ParallelFor(site, view.size(), [&](jobs::ChunkRange r) {
    view.ForChunk(r,
                  [&](entt::entity) { view.Write<Transform>(other).rot = 1; });
  });
}

void ViewAfterStructuralChange() {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(CoreRegistry(), {});
  Fill(world, 4);
  jobs::ForkSite site{.name = "death", .grain = 4, .writes = kTransformOnly};
  auto view = world.Parallel<Transform>(site);
  const entt::entity late = world.Create(Guid{2, 2});
  world.Reg().emplace<Transform>(late);
  jobs::ForkJoinPool pool;
  pool.ParallelFor(site, view.size(), [&](jobs::ChunkRange r) {
    view.ForChunk(r, [](entt::entity) {});
  });
}

void PlayMutationWithGateClosed() {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World play(CoreRegistry(), {.kind = WorldKind::kPlay});
  {
    MutationGate gate = play.OpenGate(GateReason::kTest);
    static_cast<void>(play.Create(Guid{0, 1}));
  }
  static_cast<void>(play.Create(Guid{0, 2}));
}

void AccessFromAnotherThread() {
  World world(CoreRegistry(), {});
  std::thread other([&] { TouchRegistry(world); });
  other.join();
}

// Only the main thread owns the Worlds in inline mode: a proof made on main
// and used on another thread is caught at run time.
void InlineOwnerOffMain() {
  const testing::MainThreadScope main_thread;
  const MainThreadProof proof = OnMainThread();
  std::thread other([proof] { TakeInlineOwner(proof); });
  other.join();
}

void DuplicateGuid() {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(CoreRegistry(), {});
  static_cast<void>(world.Create(Guid{0, 5}));
  static_cast<void>(world.Create(Guid{0, 5}));
}

void UnsealedRegistry() {
  ComponentRegistry registry;
  RegisterCoreComponents(registry);
  const World world(registry, {});
}

const std::map<std::string, std::function<void()>>& Scenarios() {
  static const std::map<std::string, std::function<void()>> kScenarios = {
      {"world_access_in_fork", WorldAccessInFork},
      {"structural_call_in_fork", StructuralCallInFork},
      {"create_in_fork", CreateInFork},
      {"owner_fork_inside_chunk", OwnerForkInsideChunk},
      {"nested_fork_outside_chunk", NestedForkOutsideChunk},
      {"undeclared_read", UndeclaredRead},
      {"undeclared_write", UndeclaredWrite},
      {"write_outside_chunk", WriteOutsideChunk},
      {"view_after_structural_change", ViewAfterStructuralChange},
      {"play_mutation_gate_closed", PlayMutationWithGateClosed},
      {"access_from_another_thread", AccessFromAnotherThread},
      {"inline_owner_off_main", InlineOwnerOffMain},
      {"duplicate_guid", DuplicateGuid},
      {"unsealed_registry", UnsealedRegistry},
  };
  return kScenarios;
}

int RunScenario(int argc, char** argv) {
#if defined(_MSC_VER)
  // No "abort() has been called" or debug-CRT dialogs: report to stderr.
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
  if (argc != 2) {
    std::fprintf(stderr, "usage: se_death_tests <scenario>\n");
    for (const auto& [name, run] : Scenarios()) {
      std::fprintf(stderr, "  %s\n", name.c_str());
    }
    return 2;
  }
  const auto it = Scenarios().find(argv[1]);
  if (it == Scenarios().end()) {
    std::fprintf(stderr, "unknown scenario '%s'\n", argv[1]);
    return 2;
  }
  it->second();
  std::fprintf(stderr, "scenario '%s' returned: no assertion fired\n", argv[1]);
  return 1;
}

}  // namespace
}  // namespace se

int main(int argc, char** argv) {
  // Test code may catch (ADR-006 exempts tests): an unexpected exception is
  // a failure, not a pass.
  try {
    return se::RunScenario(argc, argv);
  } catch (...) {
    std::fprintf(stderr, "unexpected exception\n");
    return 3;
  }
}
