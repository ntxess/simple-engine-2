// Copyright 2026 The simple-engine-2 Authors.
//
// Determinism across worker counts (docs/milestone-specs.md M14): identical
// per-tick hashes for W = 0, 1, 3 and 7 and eight chaos seeds, with every
// fork site enabled (the Schedule's se.PrevTransformCopy, the kernel test
// module's Movers, rampage's KinematicMove, Path, Boundary, Broadphase and
// Narrowphase) and a small grain so the scenes fork:
//   - the kernel test module's golden session (determinism_kernel);
//   - rampage's sandbox (determinism_rampage) and the effects, patrol and
//     stress reference scenarios with their recorded input;
//   - pile-up scenes: every collider in one 64 px cell, with polled and
//     moving event-driven sources, so the broadphase returns everyone and
//     the narrowphase makes thousands of contacts;
//   - the RenderList's cull, keys and vertices (render.RenderList) on the
//     pile-up scene, against the serial BuildRenderList, at alpha 0.5.
// Each W = 0 run uses the same configuration without workers.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game_rampage/collision.h"
#include "game_rampage/components.h"
#include "game_rampage/fork_sites.h"
#include "game_rampage/module.h"
#include "game_rampage/scene.h"
#include "rampage/support/rampage_session.h"
#include "rampage/support/seinput.h"
#include "se_core/core_components.h"
#include "se_core/guid.h"
#include "se_core/hash.h"
#include "se_core/input_frame.h"
#include "se_core/jobs.h"
#include "se_core/rng.h"
#include "se_core/schedule.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"
#include "se_render_core/render_list.h"
#include "se_sim/game_module.h"
#include "support/fork_config.h"
#include "support/golden.h"
#include "support/kernel_session.h"
#include "support/kernel_test_module.h"
#include "support/test_roles.h"

namespace se {
namespace {

using testing::ForkTestConfig;
using testing::ForkTestOptions;
using testing::kChaosSeeds;
using testing::kForkWorkerCounts;

// Small grains, so even the sandbox's 17 entities make several chunks.
constexpr std::uint32_t kTestGrain = 2;

struct Run {
  int workers = 0;
  std::uint64_t seed = 0;
};

// W = 0 first, then each W of 1, 3 and 7 under each chaos seed.
std::vector<Run> Matrix() {
  std::vector<Run> runs = {{0, 0}};
  for (const int workers : kForkWorkerCounts) {
    if (workers == 0) continue;
    for (const std::uint64_t seed : kChaosSeeds) {
      runs.push_back({workers, seed});
    }
  }
  return runs;
}

jobs::ForkJoinPool::Config PoolFor(const Run& run, std::uint32_t grain) {
  ForkTestOptions options;
  options.grain = grain;
  options.chaos_seed = run.seed;
  return ForkTestConfig(run.workers, options);
}

TEST_CASE("Kernel hashes are the same for every W and chaos seed",
          "[jobs][determinism][golden]") {
  const testing::MainThreadScope main;
  testing::KernelSessionOptions options;
  options.clock_seed = 11;
  options.edits_path = testing::EditsFixturePath();
  std::vector<std::uint64_t> baseline;
  for (const Run& run : Matrix()) {
    INFO("W = " << run.workers << ", chaos seed " << run.seed);
    options.pool = PoolFor(run, kTestGrain);
    const testing::KernelSessionResult result =
        testing::RunKernelSession(options);
    REQUIRE(result.error.empty());
    REQUIRE(result.hashes.size() == 600);
    if (baseline.empty()) {
      baseline = result.hashes;
      const testing::GoldenResult golden =
          testing::CheckGolden("determinism_kernel", baseline);
      INFO(golden.message);
      CHECK(golden.ok);
      continue;
    }
    CHECK(result.hashes == baseline);
  }
}

struct Scenario {
  const char* name;
  bool sandbox;
};

TEST_CASE("Rampage hashes are the same for every W and chaos seed",
          "[jobs][determinism][rampage]") {
  const testing::MainThreadScope main;
  for (const Scenario scenario :
       {Scenario{"sandbox", true}, Scenario{"effects", false},
        Scenario{"patrol", false}, Scenario{"stress", false}}) {
    INFO("scenario " << scenario.name);
    std::optional<testing::FrameListInput> recorded;
    testing::RampageSessionOptions options;
    if (scenario.sandbox) {
      options.scene = testing::SandboxSceneRequest();
    } else {
      options.scene = testing::ReferenceScenarioRequest(scenario.name);
    }
    std::vector<InputFrame> frames;
    if (!scenario.sandbox) {
      auto read =
          testing::ReadSeinput(testing::RampageFixtureDir() + "/reference/" +
                               scenario.name + ".seinput");
      REQUIRE(read.has_value());
      frames = std::move(*read);
    }
    std::vector<std::uint64_t> baseline;
    for (const Run& run : Matrix()) {
      INFO("W = " << run.workers << ", chaos seed " << run.seed);
      // The full 3600 ticks at W = 0 (the sandbox's golden), 900 under
      // every other W and seed.
      options.ticks = baseline.empty() ? 3600 : 900;
      options.pool = PoolFor(run, kTestGrain);
      if (!scenario.sandbox) {
        recorded.emplace(frames);
        options.input = &*recorded;
      }
      const testing::RampageSessionResult result =
          testing::RunRampageSession(options);
      REQUIRE(result.error.empty());
      REQUIRE(result.hashes.size() == options.ticks);
      if (baseline.empty()) {
        baseline = result.hashes;
        if (scenario.sandbox) {
          const testing::GoldenResult golden =
              testing::CheckGolden("determinism_rampage", baseline);
          INFO(golden.message);
          CHECK(golden.ok);
        }
        continue;
      }
      CHECK(std::vector<std::uint64_t>(
                baseline.begin(), baseline.begin() + 900) == result.hashes);
    }
  }
}

// A pile-up: |count| colliders in one 64 px cell. A third are polled
// friendly sources with an instant and an over-time effect, a third move
// on small loops inside the cell as event-driven enemy sources, the rest
// are enemies with HP that only receive. Every entity has a sprite.
class PileUp {
 public:
  PileUp(std::uint32_t count, std::uint64_t seed) SE_REQUIRES(g_main)
      : module_(rampage::RampageModule()),
        world_(module_.Registry(), {.name = "pile_up", .seed = seed}) {
    const InlineOwnerScope owner(OnMainThread());
    rampage::SceneSettings settings;
    settings.seed = seed;
    rampage::ApplySceneSettings(world_, settings);
    auto& names = world_.Reg().ctx().get<rampage::StatNames>();
    const rampage::StatId hp = names.Add("HP");
    const rampage::StatId speed = names.Add("Speed");
    for (std::uint32_t i = 0; i < count; ++i) Add(i, seed, hp, speed);
    camera_ = world_.Create(Guid{kGuidHi + 1, 1});
    Camera2D camera;
    camera.center = {120.0F, 120.0F};
    camera.zoom = 4.0F;
    camera.virtual_size = {320.0F, 240.0F};
    camera.primary = true;
    world_.Reg().emplace<Camera2D>(camera_, camera);
    rampage::FinishLoad(world_);
  }

  World& Get() noexcept { return world_; }
  Schedule& Steps() noexcept { return module_.MutableSchedule(); }
  [[nodiscard]] Camera2D Camera() const {
    return world_.CReg().get<Camera2D>(camera_);
  }

 private:
  static constexpr std::uint64_t kGuidHi = 0x9113000000000000ULL;

  void Add(std::uint32_t i, std::uint64_t seed, rampage::StatId hp,
           rampage::StatId speed) SE_REQUIRES(g_sim_owner) {
    rng::Stream draws(seed, 0, Guid{kGuidHi, i + 1}, 0);
    entt::registry& reg = world_.Reg();
    const entt::entity e = world_.Create(Guid{kGuidHi, i + 1});
    // Inside the cell [64, 128) x [64, 128).
    const Vec2 pos{70.0F + 50.0F * draws.Unit(), 70.0F + 50.0F * draws.Unit()};
    reg.emplace<Transform>(e, pos, 0.0F, Vec2{1.0F, 1.0F}, Tick{0});
    reg.emplace<rampage::Collider>(e, Vec2{4.0F, 4.0F});
    SpriteRenderer sprite;
    sprite.texture = 1 + i % 3;
    sprite.region = {0.0F, 0.0F, 8.0F, 8.0F};
    sprite.origin = {4.0F, 4.0F};
    sprite.layer = static_cast<std::int16_t>(i % 2);
    sprite.z = static_cast<float>(i % 5);
    reg.emplace<SpriteRenderer>(e, sprite);
    rampage::Stats stats;
    rampage::SetStat(stats, hp, 1000.0F);
    const std::uint32_t kind = i % 3;
    if (kind == 0) {
      reg.emplace<rampage::Team>(e, rampage::TeamId::kFriendly);
      rampage::EffectSource source;
      source.polled = true;
      source.cooldown_ms = 50;
      source.ready_on_start = true;
      source.effects = {
          {rampage::EffectKind::kInstant, hp, -1.0F, 0, 0},
          {rampage::EffectKind::kOverTime, hp, -0.5F, 200, 50},
      };
      reg.emplace<rampage::EffectSource>(e, std::move(source));
    } else {
      reg.emplace<rampage::Team>(e, rampage::TeamId::kEnemy);
    }
    if (kind == 1) {
      rampage::EffectSource source;
      source.polled = false;
      source.effects = {{rampage::EffectKind::kInstant, hp, -0.25F, 0, 0}};
      reg.emplace<rampage::EffectSource>(e, std::move(source));
      rampage::SetStat(stats, speed, 30.0F + 30.0F * draws.Unit());
      rampage::Path path;
      path.loop = true;
      for (int p = 0; p < 3; ++p) {
        path.points.push_back(
            {66.0F + 60.0F * draws.Unit(), 66.0F + 60.0F * draws.Unit()});
      }
      reg.emplace<rampage::Path>(e, std::move(path));
    }
    reg.emplace<rampage::Stats>(e, std::move(stats));
  }

  ModuleInstance module_;
  World world_;
  entt::entity camera_ = entt::null;
};

std::uint64_t HashRenderList(const RenderList& list) {
  Hasher hasher(0x9E57);
  for (const SpriteQuad& quad : list.quads) {
    std::uint32_t words[4 * 4 + 1] = {};
    std::size_t w = 0;
    for (int k = 0; k < 4; ++k) {
      std::memcpy(&words[w++], &quad.pos[k].x, 4);
      std::memcpy(&words[w++], &quad.pos[k].y, 4);
      std::memcpy(&words[w++], &quad.uv[k].x, 4);
      std::memcpy(&words[w++], &quad.uv[k].y, 4);
    }
    std::memcpy(&words[w], &quad.tint, 4);
    for (const std::uint32_t word : words) hasher.AddU64(word);
  }
  for (const QuadBatch& batch : list.batches) {
    hasher.AddU64(batch.texture);
    hasher.AddU64(batch.first);
    hasher.AddU64(batch.count);
  }
  hasher.AddU64(list.stats.sprites);
  hasher.AddU64(list.stats.interpolated);
  hasher.AddU64(list.stats.culled);
  return hasher.Finish();
}

struct PileUpRun {
  std::vector<std::uint64_t> hashes;
  std::vector<std::uint64_t> render;
  std::uint64_t contacts = 0;
  std::uint64_t threaded_forks = 0;
};

// |ticks| ticks of a pile-up with the player-free scene; each tick's world
// hash and the hash of its RenderList at alpha 0.5, built forked or (with
// |serial_render|) by the serial builder.
PileUpRun RunPileUp(const jobs::ForkJoinPool::Config& config,
                    std::uint32_t ticks, bool serial_render)
    SE_REQUIRES(g_main) {
  PileUp scene(240, 7);
  jobs::ForkJoinPool pool(config);
  PileUpRun out;
  RenderList list;
  for (std::uint32_t t = 1; t <= ticks; ++t) {
    const InlineOwnerScope owner(OnMainThread());
    InputFrame frame;
    frame.tick = scene.Get().Clock().tick + 1;
    scene.Steps().Step(scene.Get(), frame, pool);
    out.hashes.push_back(scene.Get().LastHash().Folded());
    if (serial_render) {
      BuildRenderList(scene.Get(), scene.Camera(), 0.5F, list);
    } else {
      BuildRenderListForked(scene.Get(), scene.Camera(), 0.5F, list, pool);
    }
    out.render.push_back(HashRenderList(list));
    out.contacts += scene.Get()
                        .CReg()
                        .ctx()
                        .get<rampage::CollisionScratch>()
                        .contacts.size();
  }
  for (const jobs::ForkSite* site : pool.Sites()) {
    out.threaded_forks += site->stats.threaded_forks;
  }
  return out;
}

TEST_CASE("Pile-up hashes and RenderLists are the same for every W",
          "[jobs][determinism][rampage]") {
  const testing::MainThreadScope main;
  constexpr std::uint32_t kTicks = 240;
  // The reference: W = 0, the serial RenderList builder.
  const PileUpRun serial =
      RunPileUp(PoolFor({0, 0}, kTestGrain), kTicks, /*serial_render=*/true);
  // Everyone overlaps everyone: thousands of contacts a tick.
  CHECK(serial.contacts > std::uint64_t{1000} * kTicks);
  REQUIRE(serial.threaded_forks == 0);
  for (const Run& run : Matrix()) {
    INFO("W = " << run.workers << ", chaos seed " << run.seed);
    // Grain 8 here: 240 colliders give 30 chunks per site.
    const PileUpRun forked = RunPileUp(PoolFor(run, 8), kTicks, false);
    CHECK(forked.hashes == serial.hashes);
    CHECK(forked.render == serial.render);
    CHECK(forked.contacts == serial.contacts);
    if (run.workers > 0) REQUIRE(forked.threaded_forks > 0);
  }
}

TEST_CASE("rampage's fork sites are disabled unless a pool enables them",
          "[jobs][rampage]") {
  const testing::MainThreadScope main;
  PileUp scene(64, 3);
  // A pool with workers but no site enabled, as a shipped one would be.
  jobs::ForkJoinPool::Config config = ForkTestConfig(3);
  config.enabled_sites.clear();
  jobs::ForkJoinPool pool(config);
  for (int t = 0; t < 10; ++t) {
    const InlineOwnerScope owner(OnMainThread());
    InputFrame frame;
    frame.tick = scene.Get().Clock().tick + 1;
    scene.Steps().Step(scene.Get(), frame, pool);
  }
  REQUIRE(pool.ActiveWorkers() == 0);
  std::vector<std::string> names;
  for (const jobs::ForkSite* site : pool.Sites()) {
    names.emplace_back(site->name);
    CHECK_FALSE(site->enabled);
    REQUIRE(site->stats.threaded_forks == 0);
    REQUIRE(site->stats.forks == 10);
  }
  // Every rampage site and the Schedule's ran through ParallelFor.
  for (const std::string_view name : rampage::ForkSiteNames()) {
    CHECK(std::find(names.begin(), names.end(), std::string(name)) !=
          names.end());
  }
  CHECK(std::find(names.begin(), names.end(), "se.PrevTransformCopy") !=
        names.end());
}

}  // namespace
}  // namespace se
