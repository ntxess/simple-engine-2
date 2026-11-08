// Copyright 2026 The simple-engine-2 Authors.
//
// Input, Intent and Movement: the player's controls, path following and the
// arena clamp. The float expressions follow the legacy reference operation
// for operation (Movement::execute, WayPointSystem, BoundarySystem, sf::Angle
// and sf::Transformable), or the text of rules R11 and R12.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include "game_rampage/components.h"
#include "game_rampage/module.h"
#include "game_rampage/systems.h"
#include "se_core/core_components.h"
#include "se_core/detmath.h"
#include "se_core/schedule.h"
#include "se_core/types.h"
#include "se_core/world.h"

namespace se::rampage {
namespace {

// sf::priv::pi and tau (SFML 3, Angle.inl).
constexpr float kSfmlPi = 3.141592654F;
constexpr float kSfmlTau = kSfmlPi * 2.0F;
// The heading's (180 / pi) of WayPointSystem and R12.
constexpr float kRadToDeg = 180.0F / std::numbers::pi_v<float>;

// A path loop that consumes nothing per iteration (lengths far below the
// step's precision) stops here instead of spinning.
constexpr int kMaxPathIterations = 1 << 16;

RulesVersion Rules(const World& world) {
  const auto* rules = world.CReg().ctx().find<RulesConfig>();
  return rules != nullptr ? rules->version : RulesVersion::kV2;
}

ArenaBounds Bounds(const World& world) {
  const auto* bounds = world.CReg().ctx().find<ArenaBounds>();
  return bounds != nullptr ? *bounds : ArenaBounds{};
}

float Dt(const TickContext& context) {
  return static_cast<float>(context.clock.dt);
}

bool CacheStale(const Path& path, const PathCache& cache) {
  return cache.totals.size() != path.points.size() ||
         cache.lengths.size() != SegmentCount(path);
}

// The legacy WayPointSystem (legacy-ref-v1): the whole step follows the
// current segment, which switches once the distance passes the next point's
// running total; position integrates per tick.
void StepPathV1(entt::registry& reg, entt::entity e, Path& path,
                const PathCache& cache, float speed, float dt,
                Transform& transform) {
  const auto n = static_cast<std::uint32_t>(path.points.size());
  if (n == 0) return;
  std::uint32_t head = std::min(path.segment, n - 1);
  // R6: zero-length segments are skipped without moving.
  while (head + 1 < n && cache.lengths[head] <= 0.0F) ++head;
  path.segment = head;
  if (head + 1 >= n) {
    if (path.loop) {
      path.segment = 0;
      path.u = 0.0F;
    }
    return;
  }
  path.u += speed * dt;
  if (path.u > cache.totals[head + 1]) path.segment = head + 1;
  const Vec2 a = path.points[head];
  const Vec2 b = path.points[head + 1];
  const float length = cache.lengths[head];
  const Vec2 unit{(b.x - a.x) / length, (b.y - a.y) / length};
  const Vec2 velocity{unit.x * speed * dt, unit.y * speed * dt};
  const float heading = (dm::Atan2(velocity.y, velocity.x) * kRadToDeg) + 90.0F;
  transform.rot = RotationFromHeading(heading);
  transform.pos = transform.pos + velocity;
  reg.emplace_or_replace<Moved>(e);
}

}  // namespace

float RotationFromHeading(float degrees) noexcept {
  const float radians = degrees * (kSfmlPi / 180.0F);
  // The int conversion below needs a finite turn count (headings are within
  // a turn of 0; anything else would be undefined behaviour in the legacy
  // code too).
  if (!(std::abs(radians / kSfmlTau) < 1.0e9F)) return 0.0F;
  // sf::priv::positiveRemainder(radians, tau).
  const float value =
      radians -
      static_cast<float>(static_cast<int>(radians / kSfmlTau)) * kSfmlTau;
  return value >= 0.0F ? value : value + kSfmlTau;
}

float RotationDegrees(float radians) noexcept {
  return radians * (180.0F / kSfmlPi);
}

Vec2 ClampToArena(Vec2 pos, Vec2 half, const ArenaBounds& bounds) noexcept {
  const float w = bounds.w;
  const float h = bounds.h;
  const float hw = half.x;
  const float hh = half.y;
  const float x = w < 2.0F * hw ? w * 0.5F : std::clamp(pos.x, hw, w - hw);
  const float y = h < 2.0F * hh ? h * 0.5F : std::clamp(pos.y, hh, h - hh);
  return {x, y};
}

Vec2 ClampToArenaV1(const Transform& transform, Vec2 half,
                    const ArenaBounds& bounds) noexcept {
  // sf::Transformable::getTransform, then Transform::transformRect of the
  // local bounds {0, 0, size} (the origin is the centre).
  const float angle = -transform.rot;
  const dm::SinCosPair<float> sc = dm::SinCos(angle);
  const float cosine = sc.cos;
  const float sine = sc.sin;
  const float sxc = transform.scale.x * cosine;
  const float syc = transform.scale.y * cosine;
  const float sxs = transform.scale.x * sine;
  const float sys = transform.scale.y * sine;
  const float ox = half.x;
  const float oy = half.y;
  const Vec2 position = transform.pos;
  const float tx = -ox * sxc - oy * sys + position.x;
  const float ty = ox * sxs - oy * syc + position.y;
  const float m0 = sxc;
  const float m4 = sys;
  const float m1 = -sxs;
  const float m5 = syc;
  const Vec2 size_local{half.x * 2.0F, half.y * 2.0F};
  const Vec2 corners[4] = {{0.0F, 0.0F},
                           {0.0F, size_local.y},
                           {size_local.x, 0.0F},
                           {size_local.x, size_local.y}};
  Vec2 points[4];
  for (int i = 0; i < 4; ++i) {
    points[i] = {m0 * corners[i].x + m4 * corners[i].y + tx,
                 m1 * corners[i].x + m5 * corners[i].y + ty};
  }
  Vec2 pmin = points[0];
  Vec2 pmax = points[0];
  for (int i = 1; i < 4; ++i) {
    if (points[i].x < pmin.x) {
      pmin.x = points[i].x;
    } else if (points[i].x > pmax.x) {
      pmax.x = points[i].x;
    }
    if (points[i].y < pmin.y) {
      pmin.y = points[i].y;
    } else if (points[i].y > pmax.y) {
      pmax.y = points[i].y;
    }
  }
  const Vec2 size{pmax.x - pmin.x, pmax.y - pmin.y};
  // Each correction sets both coordinates from the unclamped position, so
  // the last one that applies wins.
  Vec2 result = position;
  if (position.x < 0) result = {0.0F, position.y};
  if (position.x + size.x > bounds.w) result = {bounds.w - size.x, position.y};
  if (position.y < 0) result = {position.x, 0.0F};
  if (position.y + size.y > bounds.h) result = {position.x, bounds.h - size.y};
  return result;
}

std::uint32_t SegmentCount(const Path& path) noexcept {
  const auto n = static_cast<std::uint32_t>(path.points.size());
  if (n < 2) return 0;
  return path.loop ? n : n - 1;
}

void BuildPathCache(const Path& path, PathCache& cache) {
  const auto n = static_cast<std::uint32_t>(path.points.size());
  const std::uint32_t segments = SegmentCount(path);
  cache.lengths.assign(segments, 0.0F);
  cache.totals.assign(n, 0.0F);
  for (std::uint32_t i = 0; i < segments; ++i) {
    const Vec2 a = path.points[i];
    const Vec2 b = path.points[(i + 1) % n];
    // WayPoint::link: sqrt(dx * dx + dy * dy).
    cache.lengths[i] =
        std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
  }
  // WayPoint::makePath: next.distanceTotal = last.distanceTotal +
  // last.distanceToNext.
  for (std::uint32_t i = 0; i + 1 < n; ++i) {
    cache.totals[i + 1] = cache.totals[i] + cache.lengths[i];
  }
}

PathStep AdvancePath(Path& path, const PathCache& cache, float rem) noexcept {
  PathStep step;
  const std::uint32_t segments = SegmentCount(path);
  const auto n = static_cast<std::uint32_t>(path.points.size());
  if (path.done || n == 0) return step;
  if (path.loop) {
    // R13: a loop needs a segment of positive length to move at all.
    bool any = false;
    for (std::uint32_t i = 0; i < segments; ++i) {
      any = any || cache.lengths[i] > 0.0F;
    }
    if (!any) return step;
  }
  step.active = true;
  std::uint32_t i = path.segment;
  if (path.loop && i >= segments) i = 0;
  float u = path.u;
  bool travelled = false;
  std::uint32_t last = 0;
  const auto heading_of = [&path, n](std::uint32_t segment) {
    const Vec2 a = path.points[segment];
    const Vec2 b = path.points[(segment + 1) % n];
    return (dm::Atan2(b.y - a.y, b.x - a.x) * kRadToDeg) + 90.0F;
  };
  for (int iteration = 0;; ++iteration) {
    // 1. Skip segments of length 0 (R6).
    while (i < segments && cache.lengths[i] <= 0.0F) {
      ++i;
      if (path.loop && i == segments) i = 0;
    }
    // 2. The end of a non-repeating path: exactly on the last point, for
    // good.
    if (i >= segments) {
      path.done = true;
      path.segment = segments;
      path.u = 0.0F;
      step.pos = path.points[n - 1];
      if (travelled) {
        step.has_heading = true;
        step.heading = heading_of(last);
      }
      return step;
    }
    const float length = cache.lengths[i];
    // 3. The step ends on this segment.
    if (u + rem < length || iteration >= kMaxPathIterations) {
      u = u + rem;
      break;
    }
    // 4. Continue on the next segment with the rest of the step.
    rem = (u + rem) - length;
    u = 0.0F;
    travelled = true;
    last = i;
    ++i;
    if (path.loop && i == segments) i = 0;
  }
  path.segment = i;
  path.u = u;
  const Vec2 a = path.points[i];
  const Vec2 b = path.points[(i + 1) % n];
  const float length = cache.lengths[i];
  step.pos = {a.x + (b.x - a.x) * (u / length),
              a.y + (b.y - a.y) * (u / length)};
  step.has_heading = true;
  step.heading = heading_of(i);
  return step;
}

void PlayerInputSystem::Run(World& world, const TickContext& context) {
  const InputFrame& in = context.input;
  const auto held = [&in](Action action) {
    return in.Held(static_cast<ActionId>(action)) ? 1.0F : 0.0F;
  };
  const Vec2 dir{held(Action::kRight) - held(Action::kLeft),
                 held(Action::kDown) - held(Action::kUp)};
  for (const auto [e, control, intent] :
       world.Reg().view<const PlayerControl, MoveIntent>().each()) {
    intent.dir = dir;
  }
}

void IntentSystem::Run(World& world, const TickContext& /*context*/) {
  for (const auto [e, intent, control, body] :
       world.Reg()
           .view<const MoveIntent, const PlayerControl, KinematicBody>()
           .each()) {
    body.velocity = {intent.dir.x * control.speed,
                     intent.dir.y * control.speed};
  }
}

void KinematicMoveSystem::Run(World& world, const TickContext& context) {
  const float dt = Dt(context);
  entt::registry& reg = world.Reg();
  for (const auto [e, body, transform] :
       reg.view<const KinematicBody, Transform>().each()) {
    if (body.velocity.x == 0.0F && body.velocity.y == 0.0F) continue;
    // Movement::execute: pos + direction * (speed * dt), one axis term each.
    const Vec2 before = transform.pos;
    transform.pos = {before.x + body.velocity.x * dt,
                     before.y + body.velocity.y * dt};
    if (transform.pos != before) reg.emplace_or_replace<Moved>(e);
  }
}

void PathSystem::Run(World& world, const TickContext& context) {
  const float dt = Dt(context);
  const RulesVersion rules = Rules(world);
  entt::registry& reg = world.Reg();
  for (const auto [e, path, cache, stats, transform] :
       reg.view<Path, PathCache, const Stats, Transform>().each()) {
    // Without a Speed stat an entity does not move.
    const float* speed = FindStat(stats, kStatSpeed);
    if (speed == nullptr) continue;
    if (CacheStale(path, cache)) BuildPathCache(path, cache);
    if (rules == RulesVersion::kV1) {
      StepPathV1(reg, e, path, cache, *speed, dt, transform);
      continue;
    }
    const PathStep step = AdvancePath(path, cache, *speed * dt);
    if (!step.active) continue;
    if (step.has_heading) transform.rot = RotationFromHeading(step.heading);
    if (step.pos != transform.pos) {
      transform.pos = step.pos;
      reg.emplace_or_replace<Moved>(e);
    }
  }
}

void BoundarySystem::Run(World& world, const TickContext& /*context*/) {
  const ArenaBounds bounds = Bounds(world);
  const RulesVersion rules = Rules(world);
  for (const auto [e, transform, collider] :
       world.Reg().view<Transform, const Collider>().each()) {
    transform.pos =
        rules == RulesVersion::kV1
            ? ClampToArenaV1(transform, collider.half_extents, bounds)
            : ClampToArena(transform.pos, collider.half_extents, bounds);
  }
}

}  // namespace se::rampage
