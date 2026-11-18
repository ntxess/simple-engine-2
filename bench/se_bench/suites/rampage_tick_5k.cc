// Copyright 2026 The simple-engine-2 Authors.
//
// rampage_tick_5k (docs/milestone-specs.md M3c): one tick of game_rampage's
// own Schedule (Input, Intent, Movement with PathSystem and the R11 clamp,
// the spatial-hash collision, effects, the death rule, Cleanup and the
// world hash) over n moving colliders on paths, 5k by default. The design's
// budget for a tick is 1/160 s.
//
// Scene: the legacy stress scenario at scale. The player of Sandbox::init
// (a polled source with its three effects, standing at (200, 200)) and n
// entities of the stress generator's archetype (Editor::generateEntities):
// team enemy, HP 100, Speed 500 px/s, a 48 x 48 collider and a repeating
// path of 2 to 10 seeded points in the 1920 x 1080 arena, starting on its
// first point. Every timed iteration is the World's next tick, so the
// player keeps hitting the enemies that pass it, as in the sandbox.

#include <cstdint>
#include <entt/entity/registry.hpp>
#include <expected>
#include <string>
#include <utility>

#include "game_rampage/collision.h"
#include "game_rampage/components.h"
#include "game_rampage/module.h"
#include "game_rampage/scene.h"
#include "se_bench/suite.h"
#include "se_core/core_components.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/input_frame.h"
#include "se_core/jobs.h"
#include "se_core/rng.h"
#include "se_core/schedule.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_sim/game_module.h"

namespace se::bench {
namespace {

using rampage::StatId;

constexpr std::uint64_t kGuidHi = 0x5EB0000000005000ULL;
constexpr Vec2 kHalf{24.0F, 24.0F};

void AddPlayer(World& world, StatId hp) SE_REQUIRES(g_sim_owner) {
  entt::registry& reg = world.Reg();
  const entt::entity e = world.Create(Guid{kGuidHi, 1});
  reg.emplace<Transform>(e, Vec2{200.0F, 200.0F}, 0.0F, Vec2{1.0F, 1.0F},
                         Tick{0});
  reg.emplace<rampage::Collider>(e, kHalf);
  reg.emplace<rampage::Team>(e, rampage::TeamId::kFriendly);
  reg.emplace<rampage::PlayerControl>(e, std::uint8_t{0}, 120.0F);
  reg.emplace<rampage::MoveIntent>(e);
  reg.emplace<rampage::KinematicBody>(e);
  rampage::EffectSource source;
  source.polled = true;
  source.cooldown_ms = 1000;
  source.ready_on_start = true;
  source.effects = {
      {rampage::EffectKind::kInstant, hp, -10.0F, 0, 0},
      {rampage::EffectKind::kOverTime, hp, -1.0F, 5000, 1000},
      {rampage::EffectKind::kTempTimed, hp, -10.0F, 5000, 0},
  };
  reg.emplace<rampage::EffectSource>(e, std::move(source));
  rampage::Stats stats;
  rampage::SetStat(stats, hp, 100.0F);
  reg.emplace<rampage::Stats>(e, std::move(stats));
}

void AddMover(World& world, std::uint32_t index, std::uint64_t seed, StatId hp,
              StatId speed) SE_REQUIRES(g_sim_owner) {
  const rampage::ArenaBounds arena;
  rng::Stream draws(seed, 0, Guid{kGuidHi, index}, 0);
  rampage::Path path;
  path.loop = true;
  const std::uint32_t points = 2 + draws.Below(9);
  for (std::uint32_t p = 0; p < points; ++p) {
    path.points.push_back({arena.w * draws.Unit(), arena.h * draws.Unit()});
  }
  entt::registry& reg = world.Reg();
  const entt::entity e = world.Create(Guid{kGuidHi, index + 2});
  reg.emplace<Transform>(e, path.points.front(), 0.0F, Vec2{1.0F, 1.0F},
                         Tick{0});
  reg.emplace<rampage::Collider>(e, kHalf);
  reg.emplace<rampage::Team>(e, rampage::TeamId::kEnemy);
  rampage::Stats stats;
  rampage::SetStat(stats, hp, 100.0F);
  rampage::SetStat(stats, speed, 500.0F);
  reg.emplace<rampage::Stats>(e, std::move(stats));
  reg.emplace<rampage::Path>(e, std::move(path));
}

std::expected<void, Error> RunRampageTick(const SuiteOptions& options,
                                          Sampler& sampler) {
  AssertMainThread();
  ModuleInstance module(rampage::RampageModule());
  World world(module.Registry(),
              {.name = "rampage_tick_5k", .seed = options.seed});
  {
    const InlineOwnerScope owner(OnMainThread());
    rampage::SceneSettings settings;
    settings.seed = options.seed;
    rampage::ApplySceneSettings(world, settings);
    rampage::StatNames& names = world.Reg().ctx().get<rampage::StatNames>();
    const StatId hp = names.Add("HP");
    const StatId speed = names.Add("Speed");
    AddPlayer(world, hp);
    for (std::uint32_t i = 0; i < options.n; ++i) {
      AddMover(world, i, options.seed, hp, speed);
    }
    rampage::FinishLoad(world);
  }

  Schedule& schedule = module.MutableSchedule();
  jobs::ForkJoinPool pool;
  // One tick, with no keys held. Like the inline driver's frame, each tick
  // takes the World's ownership on the main thread (two thread-id checks,
  // nanoseconds against the tick).
  const auto tick = [&] {
    AssertMainThread();
    const InlineOwnerScope owner(OnMainThread());
    InputFrame frame;
    frame.tick = world.Clock().tick + 1;
    schedule.Step(world, frame, pool);
  };
  sampler.Run(tick);

  const auto& scratch = world.CReg().ctx().get<rampage::CollisionScratch>();
  sampler.AddCounter("colliders", options.n);
  sampler.AddCounter("entities", static_cast<double>(world.EntityCount()));
  sampler.AddCounter("ticks", static_cast<double>(world.Clock().tick));
  sampler.AddCounter("contacts_last_tick",
                     static_cast<double>(scratch.contacts.size()));
  // Where a tick goes: each system's median over the Schedule's timing ring
  // (the last 1024 ticks), in microseconds.
  for (const SystemStats& stats : schedule.Stats()) {
    sampler.AddCounter(std::string(stats.name) + "_p50_us",
                       static_cast<double>(stats.PercentileNs(0.5)) / 1000.0);
  }
  return {};
}

const SuiteRegistrar kRampageTick5k({
    .name = "rampage_tick_5k",
    .description = "One game_rampage tick of 5k moving colliders on paths",
    .default_n = 5000,
    .default_iterations = 1000,
    .default_warmup = 50,
    .run = &RunRampageTick,
});

}  // namespace
}  // namespace se::bench
