// Copyright 2026 The simple-engine-2 Authors.
//
// Registration of game_rampage's components (the only translation unit of
// the module that includes glaze, ADR-010). Enums serialize by name.
//
// Stats, PairCooldowns and ActiveEffects keep their lists sorted (by stat
// id, receiver Guid and seq), and the systems rely on it. Their JSON and
// BEVE hooks sort what they read and reject repeated keys, so a scene,
// snapshot or edit cannot load a list the systems would misread.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <expected>
#include <format>
#include <span>
#include <string_view>
#include <utility>

#include "game_rampage/components.h"
#include "se_core/component_registry.h"
#include "se_core/error.h"
#include "se_core/reflect_glaze.h"

template <>
struct glz::meta<se::rampage::TeamId> {
  using enum se::rampage::TeamId;
  // glaze's customization point is named value.
  static constexpr auto value =  // NOLINT(readability-identifier-naming)
      glz::enumerate("unaffiliated", kUnaffiliated, "friendly", kFriendly,
                     "enemy", kEnemy);
};

template <>
struct glz::meta<se::rampage::EffectKind> {
  using enum se::rampage::EffectKind;
  static constexpr auto value =  // NOLINT(readability-identifier-naming)
      glz::enumerate("instant", kInstant, "over_time", kOverTime, "temp_timed",
                     kTempTimed);
};

template <>
struct glz::meta<se::rampage::ColliderRole> {
  using enum se::rampage::ColliderRole;
  static constexpr auto value =  // NOLINT(readability-identifier-naming)
      glz::enumerate("body", kBody, "hitbox", kHitbox, "hurtbox", kHurtbox,
                     "trigger", kTrigger);
};

namespace se::rampage {
namespace {

// FindStat's binary search needs the values sorted by id, one per id.
std::expected<void, Error> Normalize(Stats& stats) {
  auto& values = stats.values;
  std::stable_sort(
      values.begin(), values.end(),
      [](const StatValue& a, const StatValue& b) { return a.id < b.id; });
  const auto repeated = std::adjacent_find(
      values.begin(), values.end(),
      [](const StatValue& a, const StatValue& b) { return a.id == b.id; });
  if (repeated != values.end()) {
    return Fail(
        ErrorCode::kParse,
        std::format("Stats: the stat id {} appears twice", repeated->id));
  }
  return {};
}

// R2's cooldown lookup needs the pairs sorted by receiver, one per receiver.
std::expected<void, Error> Normalize(PairCooldowns& cooldowns) {
  auto& pairs = cooldowns.pairs;
  std::stable_sort(pairs.begin(), pairs.end(),
                   [](const PairCooldown& a, const PairCooldown& b) {
                     return a.receiver < b.receiver;
                   });
  const auto repeated =
      std::adjacent_find(pairs.begin(), pairs.end(),
                         [](const PairCooldown& a, const PairCooldown& b) {
                           return a.receiver == b.receiver;
                         });
  if (repeated != pairs.end()) {
    return Fail(ErrorCode::kParse, "PairCooldowns: a receiver appears twice");
  }
  return {};
}

// Effects apply in seq order (R8); effects with equal seqs keep their order.
std::expected<void, Error> Normalize(ActiveEffects& active) {
  std::stable_sort(active.effects.begin(), active.effects.end(),
                   [](const ActiveEffect& a, const ActiveEffect& b) {
                     return a.seq < b.seq;
                   });
  return {};
}

// reflect::ReadJson and reflect::ReadBeve, normalizing the value before it
// is added.
template <class T>
std::expected<void, Error> ReadSortedJson(entt::registry& registry,
                                          entt::entity entity,
                                          std::string_view json) {
  T value{};
  if (const glz::error_ctx ec = glz::read_json(value, json); ec) {
    return Fail(ErrorCode::kParse, glz::format_error(ec, json));
  }
  if (auto sorted = Normalize(value); !sorted) {
    return sorted;
  }
  reflect::Emplace(registry, entity, std::move(value));
  return {};
}

template <class T>
std::expected<void, Error> ReadSortedBeve(entt::registry& registry,
                                          entt::entity entity,
                                          std::span<const std::byte> bytes) {
  T value{};
  if (const glz::error_ctx ec = glz::read_beve(value, bytes); ec) {
    return Fail(ErrorCode::kParse, glz::format_error(ec));
  }
  if (auto sorted = Normalize(value); !sorted) {
    return sorted;
  }
  reflect::Emplace(registry, entity, std::move(value));
  return {};
}

// ComponentRegistry::Add<T> with the normalizing read hooks.
template <class T>
void AddSorted(ComponentRegistry& registry, std::string_view name,
               std::uint32_t version, ComponentFlags flags) {
  ComponentRegistry generated;
  generated.Add<T>(name, version, flags);
  ComponentInfo info = generated.All().front();
  info.read_json = &ReadSortedJson<T>;
  info.read_beve = &ReadSortedBeve<T>;
  registry.Register(info);
}

}  // namespace

void RegisterRampageComponents(ComponentRegistry& registry) {
  using enum ComponentFlags;
  constexpr ComponentFlags kState = kHashed | kSerialized | kEditorVisible;
  registry.Add<Team>("Team", 1, kState);
  AddSorted<Stats>(registry, "Stats", 1, kState);
  registry.Add<EffectSource>("EffectSource", 1, kState);
  AddSorted<PairCooldowns>(registry, "PairCooldowns", 1, kHashed | kSerialized);
  AddSorted<ActiveEffects>(registry, "ActiveEffects", 1, kHashed | kSerialized);
  registry.Add<Collider>("Collider", 1, kState);
  registry.Add<KinematicBody>("KinematicBody", 1, kState);
  registry.Add<MoveIntent>("MoveIntent", 1, kHashed | kSerialized);
  registry.Add<PlayerControl>("PlayerControl", 1, kState);
  registry.Add<Path>("Path", 1, kState);
  // Derived from Path; rebuilt on load.
  registry.Add<PathCache>("PathCache", 1, kNone);
  // Set and cleared within a tick. Only a load leaves it between ticks (a
  // legacy scenario's "moving" entities), and then it decides tick 1's
  // contacts, so it is saved like the rest of the state (ADR-028).
  registry.Add<Moved>("Moved", 1, kHashed | kSerialized);
}

}  // namespace se::rampage
