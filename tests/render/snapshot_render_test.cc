// Copyright 2026 The simple-engine-2 Authors.
//
// Both ends of the dedicated sim driver's snapshots (docs/milestone-specs.md
// M17a; ADR-086), headless:
//   - AppendRenderView then BuildSnapshotRenderList give BuildRenderList's
//     RenderList bit for bit for matching state at every alpha, and at alpha
//     1 always;
//   - spawn, despawn, teleport and a recycled index switch at the
//     half-tick; a new spawn serial on the same full id is a new entity;
//   - the camera is interpolated unless cut; a view that did not step is
//     drawn at alpha 1; another generation is never blended;
//   - SnapshotRenderSource: reliable sections in tick order, the newest 3
//     kept, the pair and alpha from the anchors, a generation change hard
//     cuts, starvation holds the newest pair at alpha 1 and D' decays by at
//     most 5% per frame, an ended session's slots go back;
//   - a warm slot and a warm list allocate nothing.

#include <algorithm>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>
#include <limits>
#include <utility>
#include <vector>

#include "render/alloc_counter.h"
#include "render/render_fixture.h"
#include "se_core/core_components.h"
#include "se_core/guid.h"
#include "se_core/rng.h"
#include "se_core/snapshot.h"
#include "se_core/snapshot_channel.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"
#include "se_render_core/render_list.h"
#include "se_render_core/snapshot_fill.h"
#include "se_render_core/snapshot_render_source.h"
#include "support/test_roles.h"

namespace se {
namespace {

using Catch::Matchers::WithinAbs;

constexpr float kW = 1000.0F;
constexpr float kH = 800.0F;

bool SameBits(float a, float b) {
  return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

bool SameQuad(const SpriteQuad& a, const SpriteQuad& b) {
  for (int i = 0; i < 4; ++i) {
    if (!SameBits(a.pos[i].x, b.pos[i].x) ||
        !SameBits(a.pos[i].y, b.pos[i].y) || !SameBits(a.uv[i].x, b.uv[i].x) ||
        !SameBits(a.uv[i].y, b.uv[i].y)) {
      return false;
    }
  }
  return a.tint == b.tint;
}

// Bit-for-bit equality of two RenderLists: quads, batches and stats.
void RequireSameList(const RenderList& inline_list,
                     const RenderList& snapshot_list) {
  REQUIRE(snapshot_list.quads.size() == inline_list.quads.size());
  for (std::size_t i = 0; i < inline_list.quads.size(); ++i) {
    INFO("quad " << i);
    REQUIRE(SameQuad(snapshot_list.quads[i], inline_list.quads[i]));
  }
  REQUIRE(snapshot_list.batches.size() == inline_list.batches.size());
  for (std::size_t i = 0; i < inline_list.batches.size(); ++i) {
    CHECK(snapshot_list.batches[i].texture == inline_list.batches[i].texture);
    CHECK(snapshot_list.batches[i].first == inline_list.batches[i].first);
    CHECK(snapshot_list.batches[i].count == inline_list.batches[i].count);
  }
  CHECK(snapshot_list.stats.sprites == inline_list.stats.sprites);
  CHECK(snapshot_list.stats.interpolated == inline_list.stats.interpolated);
  CHECK(snapshot_list.stats.culled == inline_list.stats.culled);
  CHECK(snapshot_list.stats.quads == inline_list.stats.quads);
  CHECK(snapshot_list.stats.batches == inline_list.stats.batches);
}

Snapshot Extract(const World& world, const Camera2D& camera,
                 std::uint32_t generation = 1) {
  Snapshot slot;
  slot.header.session = 1;
  slot.header.generation = generation;
  slot.header.tick = world.Clock().tick;
  RenderViewOptions options;
  options.camera = camera;
  REQUIRE(AppendRenderView(world, options, slot));
  return slot;
}

Vec2 Center(const SpriteQuad& quad) {
  Vec2 sum;
  for (const Vec2& p : quad.pos) sum = sum + p;
  return sum * 0.25F;
}

// |count| sprites with seeded poses, looks, layers and textures; a tenth
// outside the canvas.
std::vector<entt::entity> Populate(World& world, std::uint32_t count)
    SE_REQUIRES(g_sim_owner) {
  std::vector<entt::entity> entities;
  for (std::uint32_t i = 0; i < count; ++i) {
    rng::Stream draws(31, 0, Guid{2, i}, 0);
    SpriteRenderer sprite =
        testing::CenteredSprite(1 + draws.Below(5), 8.0F + 24.0F * draws.Unit(),
                                8.0F + 24.0F * draws.Unit());
    sprite.region.x = 32.0F * static_cast<float>(draws.Below(4));
    sprite.layer = static_cast<std::int16_t>(
        static_cast<std::int32_t>(draws.Below(3)) - 1);
    sprite.z = draws.Unit();
    sprite.flip_x = draws.Below(2) == 1;
    sprite.flip_y = draws.Below(5) == 1;
    sprite.tint = {static_cast<std::uint8_t>(draws.Below(256)), 128, 64, 255};
    const Vec2 pos{kW * (1.2F * draws.Unit() - 0.1F),
                   kH * (1.2F * draws.Unit() - 0.1F)};
    entities.push_back(testing::AddSprite(world, 100 + i, pos, sprite,
                                          6.28F * draws.Unit() - 3.14F));
  }
  return entities;
}

// One tick of motion for every entity: up to 6 px, up to 20 degrees (some
// across the +-pi seam), scale changes.
void Move(World& world, const std::vector<entt::entity>& entities, Tick tick)
    SE_REQUIRES(g_sim_owner) {
  for (std::size_t i = 0; i < entities.size(); ++i) {
    rng::Stream draws(31, tick, Guid{3, i}, 0);
    Transform& t = testing::MutableTransform(world, entities[i]);
    t.pos =
        t.pos + Vec2{12.0F * draws.Unit() - 6.0F, 12.0F * draws.Unit() - 6.0F};
    t.rot += testing::Degrees(40.0F * draws.Unit() - 20.0F);
    if (t.rot > 3.14159F) t.rot -= 6.28318F;
    if (t.rot < -3.14159F) t.rot += 6.28318F;
    const float s = 0.5F + 1.5F * draws.Unit();
    t.scale = {s, 2.0F - 0.5F * s};
  }
}

TEST_CASE("Snapshot RenderLists equal BuildRenderList for matching state",
          "[render][snapshot]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(testing::RenderRegistry(), {});
  const std::vector<entt::entity> entities = Populate(world, 300);
  const Camera2D camera = testing::WorldCamera(kW, kH);
  testing::StepTick(world);
  Move(world, entities, 1);
  RenderList inline_list;
  RenderList snapshot_list;
  for (Tick tick = 2; tick <= 6; ++tick) {
    const Snapshot older = Extract(world, camera);
    testing::StepTick(world);
    Move(world, entities, tick);
    const Snapshot newer = Extract(world, camera);
    for (const float alpha :
         {0.0F, 0.1F, 0.25F, 0.4999F, 0.5F, 0.6F, 0.75F, 0.999F, 1.0F, -0.5F,
          1.5F, std::numeric_limits<float>::quiet_NaN()}) {
      INFO("tick " << tick << " alpha " << alpha);
      BuildRenderList(world, camera, alpha, inline_list);
      BuildSnapshotRenderList(&older, newer, 0, alpha, snapshot_list);
      REQUIRE(inline_list.stats.interpolated == 300);
      RequireSameList(inline_list, snapshot_list);
    }
  }
}

TEST_CASE("Spawn, despawn, teleport and a recycled index switch at the half",
          "[render][snapshot]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(testing::RenderRegistry(), {});
  const Camera2D camera = testing::WorldCamera(kW, kH);
  const SpriteRenderer sprite = testing::CenteredSprite(1, 10.0F, 10.0F);
  const entt::entity moving =
      testing::AddSprite(world, 1, {100.0F, 100.0F}, sprite);
  const entt::entity dying =
      testing::AddSprite(world, 2, {300.0F, 100.0F}, sprite);
  const entt::entity jumping =
      testing::AddSprite(world, 3, {500.0F, 100.0F}, sprite);
  testing::StepTick(world);
  const Snapshot older = Extract(world, camera);

  testing::StepTick(world);
  const Tick now = world.Clock().tick;
  testing::MutableTransform(world, moving).pos = {110.0F, 100.0F};
  Transform& jump = testing::MutableTransform(world, jumping);
  jump.pos = {500.0F, 600.0F};
  jump.teleport_tick = now;
  world.RequestDestroy(dying);
  world.DrainDestroyRequests();
  // EnTT recycles the freed index at once: a new entity, a new version.
  const entt::entity born =
      testing::AddSprite(world, 4, {700.0F, 100.0F}, sprite);
  REQUIRE(entt::to_entity(born) == entt::to_entity(dying));
  REQUIRE(born != dying);
  const Snapshot newer = Extract(world, camera);

  const auto centers = [&](float alpha) {
    RenderList list;
    BuildSnapshotRenderList(&older, newer, 0, alpha, list);
    std::vector<Vec2> out;
    out.reserve(list.quads.size());
    for (const SpriteQuad& quad : list.quads) out.push_back(Center(quad));
    return out;
  };
  const auto has = [](const std::vector<Vec2>& points, Vec2 p) {
    for (const Vec2& q : points) {
      if (std::abs(q.x - p.x) < 1e-3F && std::abs(q.y - p.y) < 1e-3F) {
        return true;
      }
    }
    return false;
  };
  // Before the half-tick: the dying entity is still drawn, the newborn
  // (on its recycled index) not yet, the teleport at its old place.
  const std::vector<Vec2> early = centers(0.25F);
  REQUIRE(early.size() == 3);
  CHECK(has(early, {102.5F, 100.0F}));
  CHECK(has(early, {300.0F, 100.0F}));
  CHECK(has(early, {500.0F, 100.0F}));
  // From the half-tick on: the newborn, no dying entity, the teleport at
  // its new place, never in between.
  for (const float alpha : {0.5F, 0.75F}) {
    const std::vector<Vec2> late = centers(alpha);
    REQUIRE(late.size() == 3);
    CHECK(has(late, {100.0F + 10.0F * alpha, 100.0F}));
    CHECK(has(late, {700.0F, 100.0F}));
    CHECK(has(late, {500.0F, 600.0F}));
  }
  // At alpha 1 the list is BuildRenderList's, edge cases included.
  RenderList inline_list;
  RenderList snapshot_list;
  BuildRenderList(world, camera, 1.0F, inline_list);
  BuildSnapshotRenderList(&older, newer, 0, 1.0F, snapshot_list);
  RequireSameList(inline_list, snapshot_list);
  REQUIRE(snapshot_list.stats.interpolated == 1);
}

// A slot of one view holding |items| (indices from their ids).
Snapshot HandSlot(std::vector<RenderItem> items, Vec2 camera_center,
                  bool cut = false, std::uint32_t generation = 1) {
  Snapshot slot;
  slot.header.session = 1;
  slot.header.generation = generation;
  SnapshotView& view = slot.views[0];
  slot.view_count = 1;
  view.camera.center = camera_center;
  view.camera.virtual_size = {kW, kH};
  view.camera.cut = cut;
  view.flags = kViewStepped;
  view.item_count = static_cast<std::uint32_t>(items.size());
  std::uint32_t max_index = 0;
  for (const RenderItem& item : items) {
    max_index = std::max(max_index, SnapshotEntityIndex(item.entity));
  }
  slot.items = std::move(items);
  view.sparse_count = max_index + 1;
  slot.sparse.assign(view.sparse_count, kNoSnapshotItem);
  for (std::uint32_t i = 0; i < view.item_count; ++i) {
    slot.sparse[SnapshotEntityIndex(slot.items[i].entity)] = i;
  }
  return slot;
}

RenderItem Item(std::uint32_t entity, std::uint32_t serial, Vec2 pos) {
  RenderItem item;
  item.entity = entity;
  item.spawn_serial = serial;
  item.pos = pos;
  item.texture = 1;
  item.region = {0.0F, 0.0F, 10.0F, 10.0F};
  item.origin = {5.0F, 5.0F};
  item.flags = kItemVisible;
  return item;
}

TEST_CASE("A new spawn serial on the same id is never blended",
          "[render][snapshot]") {
  const Vec2 center{0.5F * kW, 0.5F * kH};
  const Snapshot older = HandSlot({Item(7, 10, {100.0F, 100.0F})}, center);
  const Snapshot newer = HandSlot({Item(7, 11, {300.0F, 300.0F})}, center);
  RenderList list;
  BuildSnapshotRenderList(&older, newer, 0, 0.25F, list);
  REQUIRE(list.quads.size() == 1);
  CHECK(Center(list.quads[0]) == Vec2{100.0F, 100.0F});
  REQUIRE(list.stats.interpolated == 0);
  BuildSnapshotRenderList(&older, newer, 0, 0.5F, list);
  REQUIRE(list.quads.size() == 1);
  CHECK(Center(list.quads[0]) == Vec2{300.0F, 300.0F});
  // The same serial interpolates.
  const Snapshot same = HandSlot({Item(7, 10, {300.0F, 300.0F})}, center);
  BuildSnapshotRenderList(&older, same, 0, 0.5F, list);
  REQUIRE(list.quads.size() == 1);
  CHECK(Center(list.quads[0]) == Vec2{200.0F, 200.0F});
  REQUIRE(list.stats.interpolated == 1);
}

TEST_CASE("Cameras interpolate unless cut; unstepped views draw S_b",
          "[render][snapshot]") {
  const Snapshot older =
      HandSlot({Item(1, 1, {500.0F, 400.0F})}, {500.0F, 400.0F});
  Snapshot newer = HandSlot({Item(1, 1, {500.0F, 400.0F})}, {600.0F, 400.0F});
  RenderList list;
  // The camera moves right by 100: halfway, the sprite is 50 px left of
  // the canvas centre.
  BuildSnapshotRenderList(&older, newer, 0, 0.5F, list);
  REQUIRE(list.quads.size() == 1);
  CHECK_THAT(Center(list.quads[0]).x, WithinAbs(0.5F * kW - 50.0F, 1e-3F));
  // A cut camera is S_b's at once.
  newer.views[0].camera.cut = true;
  BuildSnapshotRenderList(&older, newer, 0, 0.5F, list);
  CHECK_THAT(Center(list.quads[0]).x, WithinAbs(0.5F * kW - 100.0F, 1e-3F));
  // A view whose World did not step is drawn at alpha 1.
  newer.views[0].camera.cut = false;
  newer.views[0].flags = 0;
  BuildSnapshotRenderList(&older, newer, 0, 0.0F, list);
  CHECK_THAT(Center(list.quads[0]).x, WithinAbs(0.5F * kW - 100.0F, 1e-3F));
  // Another generation is never blended: S_b alone.
  const Snapshot other =
      HandSlot({Item(1, 1, {0.0F, 400.0F})}, {500.0F, 400.0F}, false, 2);
  BuildSnapshotRenderList(&older, other, 0, 0.5F, list);
  REQUIRE(list.quads.size() == 1);
  CHECK_THAT(Center(list.quads[0]).x, WithinAbs(0.0F, 1e-3F));
  // No such view: an empty list.
  BuildSnapshotRenderList(&older, newer, 3, 0.5F, list);
  CHECK(list.quads.empty());
}

// The sim side of a SnapshotChannel for the source's tests.
struct FakeSim {
  SnapshotChannel channel{8};
  std::uint32_t generation = 1;
  std::uint64_t session = 1;

  // Publishes tick |tick| anchored at |anchor_ms|, carrying its tick
  // record; false when no slot was free.
  bool Publish(Tick tick, double anchor_ms, bool render = true,
               SimRunState state = SimRunState::kRunning) {
    Snapshot* slot = channel.AcquireForWrite();
    if (slot == nullptr) return false;
    slot->ClearRender();
    slot->reliable.Clear();
    slot->header.session = session;
    slot->header.generation = generation;
    slot->header.tick = tick;
    slot->header.anchor_ns = static_cast<std::uint64_t>(anchor_ms * 1e6);
    slot->header.render = render;
    slot->header.state = state;
    slot->reliable.ticks.push_back(TickRecord{session, tick, tick, 1});
    if (render) {
      SnapshotView& view = slot->views[0];
      view = SnapshotView{};
      view.flags = kViewStepped;
      view.camera.virtual_size = {kW, kH};
      view.camera.center = {0.5F * kW, 0.5F * kH};
      slot->view_count = 1;
    }
    channel.Publish(slot);
    return true;
  }
};

constexpr double kTickMs = 1000.0 / 120.0;

std::uint64_t Ms(double ms) { return static_cast<std::uint64_t>(ms * 1e6); }

TEST_CASE("SnapshotRenderSource keeps 3 slots, applies reliable in order",
          "[render][snapshot]") {
  FakeSim sim;
  SnapshotRenderSource source(sim.channel);
  std::vector<Tick> applied;
  const auto on_reliable = [&applied](const SnapshotHeader& /*header*/,
                                      const ReliableDeltas& deltas) {
    for (const TickRecord& record : deltas.ticks) {
      applied.push_back(record.tick);
    }
  };
  for (Tick tick = 1; tick <= 6; ++tick) {
    // Tick 4 carries only reliable deltas.
    REQUIRE(sim.Publish(tick, 100.0 + kTickMs * static_cast<double>(tick),
                        tick != 4));
  }
  REQUIRE(source.Receive(on_reliable) == 6);
  CHECK(applied == std::vector<Tick>{1, 2, 3, 4, 5, 6});
  REQUIRE(source.Retained() == 3);
  REQUIRE(source.RetainedSlot(0).header.tick == 3);
  REQUIRE(source.RetainedSlot(2).header.tick == 6);
  REQUIRE(source.Stats().reliable_only == 1);
  REQUIRE(source.Stats().returned == 3);
  // The 3 returned slots are free for the sim again: 8 - 3 held.
  for (Tick tick = 7; tick <= 11; ++tick) {
    REQUIRE(sim.Publish(tick, 100.0 + kTickMs * static_cast<double>(tick)));
  }
  CHECK_FALSE(sim.Publish(12, 200.0));
}

TEST_CASE("SnapshotRenderSource picks the pair around tau by the anchors",
          "[render][snapshot]") {
  FakeSim sim;
  SnapshotRenderSourceConfig config;
  config.delay_ns = 4'000'000;
  SnapshotRenderSource source(sim.channel, config);
  // Nothing yet: nothing to draw.
  CHECK(source.Pick(Ms(10.0)).b == nullptr);
  REQUIRE(sim.Publish(1, 20.0));
  source.Receive();
  // One slot: drawn alone.
  SnapshotPick pick = source.Pick(Ms(10.0));
  REQUIRE(pick.b != nullptr);
  CHECK(pick.a == nullptr);
  REQUIRE(sim.Publish(2, 30.0));
  REQUIRE(sim.Publish(3, 35.0));
  source.Receive();
  // tau = 31 - 4 = 27: between ticks 1 (20) and 2 (30), alpha 0.7.
  pick = source.Pick(Ms(31.0));
  REQUIRE(pick.a != nullptr);
  REQUIRE(pick.a->header.tick == 1);
  REQUIRE(pick.b->header.tick == 2);
  CHECK_THAT(pick.alpha, WithinAbs(0.7, 1e-6));
  // tau = 33: ticks 2 and 3, uneven spacing (slow motion needs no case).
  pick = source.Pick(Ms(37.0));
  REQUIRE(pick.a->header.tick == 2);
  CHECK_THAT(pick.alpha, WithinAbs(0.6, 1e-6));
  // tau before every anchor: the oldest pair at alpha 0.
  pick = source.Pick(Ms(20.0));
  CHECK(pick.early);
  REQUIRE(pick.a->header.tick == 1);
  CHECK(pick.alpha == 0.0F);
  RenderList list;
  source.Build(0, list);
  REQUIRE(source.ViewCount() == 1);
}

TEST_CASE("SnapshotRenderSource hard-cuts at a generation change",
          "[render][snapshot]") {
  FakeSim sim;
  SnapshotRenderSource source(sim.channel);
  REQUIRE(sim.Publish(1, 10.0));
  REQUIRE(sim.Publish(2, 10.0 + kTickMs));
  sim.generation = 2;
  REQUIRE(sim.Publish(3, 10.0 + 2 * kTickMs));
  source.Receive();
  // tau in the old generation: drawn normally.
  SnapshotPick pick = source.Pick(Ms(10.0 + 0.5 * kTickMs) + 4'000'000);
  REQUIRE(pick.a != nullptr);
  CHECK_FALSE(pick.hard_cut);
  REQUIRE(pick.b->header.generation == 1);
  // tau between the generations: S_b alone, the old slots returned.
  pick = source.Pick(Ms(10.0 + 1.5 * kTickMs) + 4'000'000);
  CHECK(pick.hard_cut);
  CHECK(pick.a == nullptr);
  REQUIRE(pick.b != nullptr);
  REQUIRE(pick.b->header.generation == 2);
  REQUIRE(source.Retained() == 1);
  REQUIRE(source.Stats().hard_cuts == 1);
}

TEST_CASE("Starvation holds the newest pair and D' decays at most 5% a frame",
          "[render][snapshot]") {
  FakeSim sim;
  SnapshotRenderSourceConfig config;
  config.delay_ns = 4'000'000;
  SnapshotRenderSource source(sim.channel, config);
  double anchor = 100.0;
  Tick tick = 1;
  for (; tick <= 3; ++tick, anchor += kTickMs) {
    REQUIRE(sim.Publish(tick, anchor));
  }
  source.Receive();
  const double newest = anchor - kTickMs;
  // A 165 Hz display; the sim stalls: tau passes the newest anchor.
  double frame = newest + 4.0 + 2.0;
  SnapshotPick pick = source.Pick(Ms(frame));
  REQUIRE(pick.starved);
  CHECK(pick.alpha == 1.0F);
  REQUIRE(pick.b->header.tick == 3);
  REQUIRE(pick.a->header.tick == 2);
  // D' rose by the gap: tau is held at the newest anchor.
  CHECK(pick.tau_ns == static_cast<std::int64_t>(Ms(newest)));
  REQUIRE(std::abs(source.DelayNs() - 6'000'000) <= 2);
  const double frame_ms = 1000.0 / 165.0;
  std::int64_t last_tau = pick.tau_ns;
  for (int f = 0; f < 3; ++f) {
    frame += frame_ms;
    pick = source.Pick(Ms(frame));
    CHECK(pick.starved);
    // Never extrapolated: held at the newest state.
    CHECK(pick.alpha == 1.0F);
    CHECK(pick.tau_ns == last_tau);
  }
  REQUIRE(source.Stats().starved_frames == 4);
  // The sim runs again, one tick ahead of the frames; D' decays by at most
  // 5% of its rise per frame and tau only moves forward, so motion resumes
  // without a jump back.
  std::int64_t raise = source.DelayNs() - config.delay_ns;
  REQUIRE(raise > 18'000'000);
  for (int f = 0; f < 300; ++f) {
    INFO("frame " << f);
    frame += frame_ms;
    while (anchor <= frame + kTickMs) {
      REQUIRE(sim.Publish(tick, anchor));
      ++tick;
      anchor += kTickMs;
      source.Receive();
    }
    pick = source.Pick(Ms(frame));
    REQUIRE_FALSE(pick.starved);
    REQUIRE(pick.b != nullptr);
    const std::int64_t now_raise = source.DelayNs() - config.delay_ns;
    CHECK(now_raise <= raise);
    CHECK(static_cast<double>(now_raise) >=
          0.95 * static_cast<double>(raise) - 1'000.0);
    CHECK(pick.tau_ns > last_tau);
    last_tau = pick.tau_ns;
    raise = now_raise;
  }
  REQUIRE(raise == 0);
  CHECK(source.DelayNs() == config.delay_ns);
}

TEST_CASE("A paused session holds its newest pair without raising D'",
          "[render][snapshot]") {
  FakeSim sim;
  SnapshotRenderSource source(sim.channel);
  REQUIRE(sim.Publish(1, 10.0, true, SimRunState::kPaused));
  REQUIRE(sim.Publish(2, 20.0, true, SimRunState::kPaused));
  source.Receive();
  const SnapshotPick pick = source.Pick(Ms(500.0));
  CHECK(pick.alpha == 1.0F);
  CHECK_FALSE(pick.starved);
  CHECK(source.DelayNs() == SnapshotRenderSourceConfig{}.delay_ns);
  REQUIRE(source.Stats().held_frames == 1);
}

TEST_CASE("The slots of an ended session go back at once",
          "[render][snapshot]") {
  FakeSim sim;
  SnapshotRenderSource source(sim.channel);
  REQUIRE(sim.Publish(1, 10.0));
  REQUIRE(sim.Publish(2, 20.0));
  source.Receive();
  REQUIRE(source.Retained() == 2);
  Snapshot* slot = sim.channel.AcquireForWrite();
  REQUIRE(slot != nullptr);
  slot->ClearRender();
  slot->reliable.Clear();
  slot->header.session = 1;
  slot->header.render = false;
  slot->reliable.events.push_back(
      SessionEvent{SessionEventKind::kEnded, 1, 1, 3, 0});
  sim.channel.Publish(slot);
  std::uint32_t ended = 0;
  source.Receive([&ended](const SnapshotHeader&, const ReliableDeltas& deltas) {
    ended += static_cast<std::uint32_t>(deltas.events.size());
  });
  REQUIRE(ended == 1);
  REQUIRE(source.Retained() == 0);
  CHECK(source.Pick(Ms(30.0)).b == nullptr);
}

TEST_CASE("A warm snapshot fill and build allocate nothing",
          "[render][snapshot]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(testing::RenderRegistry(), {});
  const std::vector<entt::entity> entities = Populate(world, 500);
  const Camera2D camera = testing::WorldCamera(kW, kH);
  testing::StepTick(world);
  Move(world, entities, 1);
  Snapshot older;
  Snapshot newer;
  RenderViewOptions options;
  options.camera = camera;
  RenderList list;
  // Warm up both slots and the list.
  for (int i = 0; i < 2; ++i) {
    older.ClearRender();
    newer.ClearRender();
    REQUIRE(AppendRenderView(world, options, older));
    REQUIRE(AppendRenderView(world, options, newer));
    BuildSnapshotRenderList(&older, newer, 0, 0.3F, list);
  }
  const void* items = newer.items.data();
  const void* quads = list.quads.data();
  const std::uint64_t before = testing::AllocationCount();
  for (int i = 0; i < 10; ++i) {
    newer.ClearRender();
    REQUIRE(AppendRenderView(world, options, newer));
    BuildSnapshotRenderList(&older, newer, 0, 0.1F * static_cast<float>(i),
                            list);
  }
  if (testing::CountingAllocations()) {
    CHECK(testing::AllocationCount() == before);
  }
  CHECK(newer.items.data() == items);
  CHECK(list.quads.data() == quads);
  REQUIRE(newer.items.size() == 500);
}

}  // namespace
}  // namespace se
