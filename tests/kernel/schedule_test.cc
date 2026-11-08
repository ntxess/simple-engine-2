// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/schedule.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/breadcrumb.h"
#include "se_core/component_registry.h"
#include "se_core/core_components.h"
#include "se_core/input_frame.h"
#include "se_core/jobs.h"
#include "se_core/thread_role.h"
#include "se_core/world.h"

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

// What the systems saw, for the assertions. A test log, not World state.
std::vector<std::string>& Trace() {
  static std::vector<std::string> trace;
  return trace;
}

template <int Id>
struct Named {
  static constexpr std::string_view kName = "named";
  void Run(World& /*world*/, const TickContext& context) {
    Trace().push_back(std::to_string(Id) + "@" +
                      std::to_string(context.clock.tick));
  }
};

struct InputSystem {
  static constexpr std::string_view kName = "Input";
  void Run(World& /*world*/, const TickContext& context) {
    const BreadcrumbFields crumb = ThreadBreadcrumb().Fields();
    Trace().push_back("input held=" + std::to_string(context.input.held) +
                      " phase=" + std::to_string(crumb.phase) +
                      " tick=" + std::to_string(crumb.tick));
  }
};

// Moves every Transform; checks PrevTransform was copied first.
struct MoveSystem {
  static constexpr std::string_view kName = "Move";
  void Run(World& world, const TickContext& context) SE_REQUIRES(g_sim_owner) {
    auto view = world.Reg().view<Transform, const PrevTransform>();
    for (const auto [entity, t, prev] : view.each()) {
      if (prev.pos == t.pos && prev.tick + 1 == context.clock.tick) {
        Trace().emplace_back("prev copied");
      }
      t.pos.x += 1.0F;
    }
  }
};

// Requests a destroy in Cleanup; the built-in Cleanup drains it.
struct Reaper {
  static constexpr std::string_view kName = "Reaper";
  void Run(World& world, const TickContext& context) SE_REQUIRES(g_sim_owner) {
    if (context.clock.tick != 2) return;
    const auto victim = world.Find(Guid{0, 1});
    if (victim) world.RequestDestroy(*victim);
  }
};

struct Stateful {
  [[maybe_unused]] static constexpr std::string_view kName = "Stateful";
  int counter = 0;
  void Run(World& /*world*/, const TickContext& /*context*/) { ++counter; }
};
struct Nameless {
  void Run(World& /*world*/, const TickContext& /*context*/) {}
};

static_assert(System<MoveSystem>);
static_assert(!System<Stateful>, "systems hold no state");
static_assert(!System<Nameless>, "systems have a static name");

TEST_CASE("Phases run in order, systems in registration order", "[schedule]") {
  InlineOwnerScope owner;
  Trace().clear();
  Schedule schedule;
  // Registered out of phase order on purpose.
  schedule.Add<Named<5>>(Phase::kCleanup);
  schedule.Add<Named<3>>(Phase::kGameRules);
  schedule.Add<Named<1>>(Phase::kIntent);
  schedule.Add<Named<4>>(Phase::kGameRules);
  schedule.Add<Named<2>>(Phase::kMovement);
  schedule.Seal();
  World world(CoreRegistry(), {});
  jobs::ForkJoinPool pool;
  InputFrame frame;
  schedule.Step(world, frame, pool);
  CHECK(Trace() == std::vector<std::string>{"1@1", "2@1", "3@1", "4@1", "5@1"});
  REQUIRE(world.Clock().tick == 1);
}

TEST_CASE("Step advances the clock and sets the breadcrumb", "[schedule]") {
  InlineOwnerScope owner;
  Trace().clear();
  Schedule schedule;
  schedule.Add<InputSystem>(Phase::kInput);
  schedule.Seal();
  World world(CoreRegistry(), {});
  jobs::ForkJoinPool pool;
  InputFrame frame;
  frame.held = 0b11;
  schedule.Step(world, frame, pool);
  schedule.Step(world, frame, pool);
  CHECK(Trace() == std::vector<std::string>{"input held=3 phase=0 tick=1",
                                            "input held=3 phase=0 tick=2"});
  // After a step the breadcrumb has the last phase and no system.
  const BreadcrumbFields crumb = ThreadBreadcrumb().Fields();
  CHECK(crumb.phase == static_cast<std::uint8_t>(Phase::kCleanup));
  REQUIRE(crumb.system == 0);
  REQUIRE(crumb.tick == 2);
}

TEST_CASE("PrevTransformCopy heads Movement; Cleanup drains and hashes",
          "[schedule]") {
  InlineOwnerScope owner;
  Trace().clear();
  Schedule schedule;
  schedule.Add<MoveSystem>(Phase::kMovement);
  schedule.Add<Reaper>(Phase::kCleanup);
  schedule.Seal();
  World world(CoreRegistry(), {});
  for (std::uint64_t i = 1; i <= 2; ++i) {
    const entt::entity e = world.Create(Guid{0, i});
    world.Reg().emplace<Transform>(e);
  }
  world.FinalizeSpawns();
  jobs::ForkJoinPool pool;
  InputFrame frame;

  schedule.Step(world, frame, pool);
  REQUIRE(Trace().size() == 2);  // both entities saw prev == current
  CHECK_FALSE(world.HashDirty());
  CHECK(world.LastHash() == world.Hash());
  REQUIRE(world.LastHash().tick == 1);
  const entt::entity survivor = *world.Find(Guid{0, 2});
  CHECK(world.CReg().get<PrevTransform>(survivor).tick == 0);
  CHECK(world.CReg().get<Transform>(survivor).pos.x == 1.0F);

  schedule.Step(world, frame, pool);
  REQUIRE(world.EntityCount() == 1);  // the Reaper's request was drained
  CHECK_FALSE(world.Find(Guid{0, 1}).has_value());
  CHECK(world.CReg().get<PrevTransform>(survivor).pos.x == 1.0F);
  CHECK(world.CReg().get<Transform>(survivor).pos.x == 2.0F);
}

TEST_CASE("Every system is timed into its ring", "[schedule]") {
  InlineOwnerScope owner;
  Schedule schedule;
  const SystemId move = schedule.Add<MoveSystem>(Phase::kMovement);
  schedule.Seal();
  REQUIRE(move == 2);
  World world(CoreRegistry(), {});
  jobs::ForkJoinPool pool;
  InputFrame frame;
  for (int i = 0; i < 1500; ++i) schedule.Step(world, frame, pool);
  const auto stats = schedule.Stats();
  REQUIRE(stats.size() == 3);
  REQUIRE(stats[Schedule::kPrevTransformCopyId].name == "PrevTransformCopy");
  CHECK(stats[Schedule::kPrevTransformCopyId].built_in);
  REQUIRE(stats[Schedule::kCleanupId].name == "Cleanup");
  REQUIRE(stats[move].name == "Move");
  for (const SystemStats& s : stats) {
    REQUIRE(s.runs == 1500);
    CHECK(s.count == kSystemRingSize);
    CHECK(s.PercentileNs(0.5) <= s.max_ns);
  }
}

TEST_CASE("A Play World steps inside its MutationGate", "[schedule]") {
  InlineOwnerScope owner;
  Schedule schedule;
  schedule.Add<MoveSystem>(Phase::kMovement);
  schedule.Seal();
  World play(CoreRegistry(), {.kind = WorldKind::kPlay});
  jobs::ForkJoinPool pool;
  InputFrame frame;
  schedule.Step(play, frame, pool);
  CHECK_FALSE(play.GateOpen());
  REQUIRE(play.Clock().tick == 1);
  REQUIRE(PhaseName(Phase::kGameRules) == "GameRules");
}

}  // namespace
}  // namespace se
