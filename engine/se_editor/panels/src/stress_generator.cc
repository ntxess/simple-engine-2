// Copyright 2026 The simple-engine-2 Authors.

#include "se_editor_panels/stress_generator.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "game_rampage/components.h"
#include "se_core/guid.h"
#include "se_core/hash.h"
#include "se_core/rng.h"
#include "se_core/types.h"
#include "se_render_core/render_components.h"

namespace se::editor {
namespace {

constexpr std::uint64_t kStressGuidSeed = 0x5354524553534755ULL;  // STRESSGU
constexpr std::uint64_t kStressGuidLo = 0x0000000000000001ULL;

std::string PositionJson(Vec2 pos) {
  return std::format(R"({{"pos":{{"x":{},"y":{}}}}})", pos.x, pos.y);
}

std::string PathJson(const std::vector<Vec2>& points) {
  std::string json = R"({"points":[)";
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (i > 0) json += ',';
    json += std::format(R"({{"x":{},"y":{}}})", points[i].x, points[i].y);
  }
  json += R"(],"loop":true})";
  return json;
}

std::uint32_t DrawBelow(rng::Stream& stream, float extent) {
  const auto n = static_cast<std::uint32_t>(std::max(1.0F, extent));
  return stream.Below(n);
}

}  // namespace

StressSpec DefaultStressSpec(float width, float height) {
  StressSpec spec;
  spec.width = width;
  spec.height = height;
  spec.max_points = 10;
  // Editor.cpp:966-980 as assets/prefabs/enemy.prefab.json has it: the
  // "player" texture, a 48 px sprite centred on its position with a
  // matching collider, team ENEMY, HP 100 and Speed 500 px/s (rule R5), a
  // repeating path. No effect source: generated enemies never hurt anyone.
  spec.components = {
      {"Transform", PositionJson({})},
      {"Collider", R"({"half_extents":{"x":24,"y":24}})"},
      {"SpriteRenderer",
       std::format(R"({{"texture":{},"region":{{"x":0,"y":0,"w":48,"h":48}},)"
                   R"("origin":{{"x":24,"y":24}}}})",
                   TextureKeyOf("player"))},
      {"Team", R"({"id":"enemy"})"},
      {"Stats", std::format(R"({{"values":[{{"id":{},"value":100}},)"
                            R"({{"id":{},"value":500}}]}})",
                            rampage::kStatHp, rampage::kStatSpeed)},
      {"Path", PathJson({})},
      // What rampage's FinishLoad adds to a loaded entity.
      {"PathCache", "{}"},
      {"ActiveEffects", "{}"},
  };
  return spec;
}

Guid StressGuid(std::uint64_t seed, std::uint32_t press,
                std::uint32_t index) noexcept {
  Hasher hasher(kStressGuidSeed);
  hasher.AddU64(seed);
  hasher.AddU64(press);
  hasher.AddU64(index);
  Guid guid;
  guid.hi = hasher.Finish();
  hasher.AddU64(kStressGuidLo);
  guid.lo = hasher.Finish();
  if (guid.IsNil()) guid.lo = kStressGuidLo;
  return guid;
}

std::vector<std::vector<Vec2>> StressPaths(const StressSpec& spec,
                                           std::uint64_t seed,
                                           std::uint32_t press,
                                           std::uint32_t count) {
  std::vector<std::vector<Vec2>> paths;
  paths.reserve(count);
  const std::uint32_t max_points = std::max<std::uint32_t>(1, spec.max_points);
  for (std::uint32_t i = 0; i < count; ++i) {
    rng::Stream stream(seed, press, StressGuid(seed, press, i),
                       kStressRngStream);
    const std::uint32_t n = 1 + stream.Below(max_points);
    std::vector<Vec2> points;
    points.reserve(n);
    for (std::uint32_t j = 0; j < n; ++j) {
      const auto x = static_cast<float>(DrawBelow(stream, spec.width));
      const auto y = static_cast<float>(DrawBelow(stream, spec.height));
      points.push_back({x, y});
    }
    paths.push_back(std::move(points));
  }
  return paths;
}

std::vector<StressSpawn> PlanStress(const StressSpec& spec, std::uint64_t seed,
                                    std::uint32_t press, std::uint32_t count) {
  std::vector<std::vector<Vec2>> paths = StressPaths(spec, seed, press, count);
  std::vector<StressSpawn> spawns;
  spawns.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    StressSpawn spawn;
    spawn.guid = StressGuid(seed, press, i);
    spawn.points = std::move(paths[i]);
    spawn.components = spec.components;
    for (SpawnComponent& component : spawn.components) {
      if (component.component == "Transform") {
        component.json = PositionJson(spawn.points.front());
      } else if (component.component == "Path") {
        component.json = PathJson(spawn.points);
      }
    }
    spawns.push_back(std::move(spawn));
  }
  return spawns;
}

std::vector<SpawnBatch> SpreadSpawns(std::uint32_t count,
                                     std::uint32_t per_tick) {
  const std::uint32_t step = std::max<std::uint32_t>(1, per_tick);
  std::vector<SpawnBatch> batches;
  for (std::uint32_t first = 0, offset = 0; first < count;
       first += step, ++offset) {
    batches.push_back({first, std::min(step, count - first), offset});
  }
  return batches;
}

}  // namespace se::editor
