// Copyright 2026 The simple-engine-2 Authors.

#include "game_rampage/projection.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <entt/entity/registry.hpp>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game_rampage/components.h"
#include "game_rampage/systems.h"
#include "se_core/core_components.h"
#include "se_core/world.h"

namespace se::rampage {
namespace {

// %.9g, which round-trips a float.
void AppendFloat(std::string& out, float value) {
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
  out += buffer;
}

void AppendStatName(std::string& out, std::string_view name) {
  for (const char c : name) {
    const bool blank =
        c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '=';
    out += blank ? '_' : c;
  }
}

float NormalisedDegrees(float radians) {
  float degrees = RotationDegrees(radians);
  if (degrees >= 360.0F) degrees -= 360.0F;
  if (degrees < 0.0F) degrees += 360.0F;
  return degrees;
}

}  // namespace

std::uint64_t Fnv1a64(std::uint64_t hash, std::string_view bytes) noexcept {
  for (const char c : bytes) {
    hash ^= static_cast<unsigned char>(c);
    hash *= kFnvPrime;
  }
  return hash;
}

void WriteProjection(const World& world, std::string& out) {
  const entt::registry& reg = world.CReg();
  out += std::format("t {}\n", world.Clock().tick);
  std::vector<std::pair<std::uint32_t, entt::entity>> entities;
  for (const auto [e, spawned] : reg.view<const Spawned>().each()) {
    entities.emplace_back(spawned.spawn_serial, e);
  }
  std::sort(entities.begin(), entities.end());
  const auto* names = reg.ctx().find<StatNames>();
  std::vector<std::pair<std::string, float>> stats;
  for (const auto& [serial, e] : entities) {
    out += std::format("{}", serial);
    if (const auto* transform = reg.try_get<Transform>(e)) {
      out += ' ';
      AppendFloat(out, transform->pos.x);
      out += ' ';
      AppendFloat(out, transform->pos.y);
      out += ' ';
      AppendFloat(out, NormalisedDegrees(transform->rot));
    } else {
      out += " - - -";
    }
    const auto* entity_stats = reg.try_get<Stats>(e);
    out += ' ';
    const float* hp =
        entity_stats != nullptr ? FindStat(*entity_stats, kStatHp) : nullptr;
    if (hp != nullptr) {
      AppendFloat(out, *hp);
    } else {
      out += '-';
    }
    if (entity_stats != nullptr) {
      stats.clear();
      for (const StatValue& stat : entity_stats->values) {
        if (stat.id == kStatHp) continue;
        std::string_view name =
            names != nullptr ? names->Name(stat.id) : std::string_view();
        stats.emplace_back(
            name.empty() ? std::format("#{:08x}", stat.id) : std::string(name),
            stat.value);
      }
      std::sort(stats.begin(), stats.end(), [](const auto& a, const auto& b) {
        return std::string_view(a.first) < std::string_view(b.first);
      });
      for (const auto& [name, value] : stats) {
        out += ' ';
        AppendStatName(out, name);
        out += '=';
        AppendFloat(out, value);
      }
    }
    out += '\n';
  }
}

bool ProjectionFile::Open(const std::string& path, std::uint32_t rate) {
  Close();
  file_.open(path, std::ios::binary | std::ios::trunc);
  if (!file_) return false;
  file_ << std::format(
      "# seproj v1 rate={}\n"
      "# t <tick> <fnv1a64 of the block>, then per entity in SpawnIndex "
      "order:\n"
      "# <spawn> <x> <y> <rotation> <HP> [<stat>=<value> ...] (%.9g, '-' = "
      "absent)\n",
      rate);
  return static_cast<bool>(file_);
}

void ProjectionFile::Write(const World& world) {
  block_.clear();
  WriteProjection(world, block_);
  tick_hash_ = Fnv1a64(kFnvOffsetBasis, block_);
  run_hash_ = Fnv1a64(run_hash_, block_);
  ++ticks_;
  if (!file_.is_open()) return;
  // The stored tick line carries the hash; the hashed block has the bare
  // "t <tick>" line.
  const std::string_view block(block_);
  const std::size_t newline = block.find('\n');
  file_ << block.substr(0, newline) << std::format(" {:016x}\n", tick_hash_)
        << block.substr(newline + 1);
}

void ProjectionFile::Close() {
  if (!file_.is_open()) return;
  file_ << std::format("# run {} {:016x}\n", ticks_, run_hash_);
  file_.close();
}

}  // namespace se::rampage
