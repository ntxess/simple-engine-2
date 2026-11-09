// Copyright 2026 The simple-engine-2 Authors.
//
// JSON and BEVE round trips for every component game_rampage registers
// (docs/milestone-specs.md, M3b "Components"): each registered name needs a
// sample here, so a new component cannot skip the test.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "game_rampage/components.h"
#include "se_core/component_registry.h"
#include "se_core/core_components.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/hash.h"

namespace se::rampage {
namespace {

// A non-default value of each component in its canonical JSON form.
const std::map<std::string_view, std::string_view>& Samples() {
  static const auto* const kSamples =
      new std::map<std::string_view, std::string_view>{
          {"Team", R"({"id":"enemy"})"},
          {"Stats", R"({"values":[{"id":7,"value":100},{"id":3105354525,)"
                    R"("value":-2.5}]})"},
          {"EffectSource",
           R"({"effects":[{"kind":"over_time","stat":3105354525,"value":-1,)"
           R"("duration_ms":5000,"interval_ms":1000},{"kind":"temp_timed",)"
           R"("stat":7,"value":0.75,"duration_ms":-3,"interval_ms":0}],)"
           R"("polled":true,"cooldown_ms":5500,"ready_on_start":false})"},
          {"PairCooldowns",
           R"({"pairs":[{"receiver":{"hi":1,"lo":2},"last":77},)"
           R"({"receiver":{"hi":1,"lo":3},"last":0}]})"},
          {"ActiveEffects",
           R"({"effects":[{"def":{"kind":"instant","stat":5,"value":-10,)"
           R"("duration_ms":0,"interval_ms":0},"source":{"guid":{"hi":3,)"
           R"("lo":4}},"start":10,"next":610,"applied":1,)"
           R"("seq":42949672961}]})"},
          {"Collider", R"({"half_extents":{"x":24,"y":9.5},"role":"hurtbox",)"
                       R"("layer":2,"mask":4294967295})"},
          {"KinematicBody", R"({"velocity":{"x":-120,"y":0}})"},
          {"MoveIntent", R"({"dir":{"x":1,"y":-1}})"},
          {"PlayerControl", R"({"player":1,"speed":120})"},
          {"Path", R"({"points":[{"x":1,"y":2},{"x":3.5,"y":-4}],"loop":true,)"
                   R"("segment":1,"u":0.25,"done":false})"},
          {"PathCache", R"({"lengths":[1.5],"totals":[0,1.5]})"},
          {"Moved", R"({})"},
      };
  return *kSamples;
}

ComponentRegistry Registry() {
  ComponentRegistry registry;
  RegisterCoreComponents(registry);
  RegisterRampageComponents(registry);
  registry.Seal();
  return registry;
}

bool IsCore(std::string_view name) {
  ComponentRegistry core;
  RegisterCoreComponents(core);
  return core.FindByName(name) != nullptr;
}

TEST_CASE("Every rampage component has a round-trip sample",
          "[rampage][serialization]") {
  const ComponentRegistry registry = Registry();
  std::size_t rampage = 0;
  for (const ComponentInfo& info : registry.All()) {
    if (IsCore(info.name)) continue;
    INFO(info.name);
    CHECK(Samples().contains(info.name));
    ++rampage;
  }
  CHECK(rampage == Samples().size());
}

TEST_CASE("JSON and BEVE round trips for every rampage component",
          "[rampage][serialization]") {
  const ComponentRegistry registry = Registry();
  for (const ComponentInfo& info : registry.All()) {
    const auto sample = Samples().find(info.name);
    if (sample == Samples().end()) continue;
    INFO(info.name);
    entt::registry reg;
    for (const ComponentInfo& each : registry.All()) each.precreate(reg);
    const entt::entity a = reg.create();
    const entt::entity b = reg.create();
    const entt::entity c = reg.create();

    REQUIRE(info.read_json(reg, a, sample->second).has_value());
    const auto json = info.write_json(reg, a);
    REQUIRE(json.has_value());
    CHECK(*json == sample->second);

    REQUIRE(info.read_json(reg, b, *json).has_value());
    CHECK(info.write_json(reg, b).value() == *json);

    const auto beve = info.write_beve(reg, a);
    REQUIRE(beve.has_value());
    CHECK_FALSE(beve->empty());
    REQUIRE(info.read_beve(reg, c, *beve).has_value());
    CHECK(info.write_json(reg, c).value() == *json);
    CHECK(info.write_beve(reg, c).value() == *beve);

    if (!info.empty) {
      const auto& storage = *reg.storage(info.id);
      Hasher ha;
      info.hash(storage.value(a), ha);
      Hasher hc;
      info.hash(storage.value(c), hc);
      CHECK(ha.Finish() == hc.Finish());
    }
  }
}

TEST_CASE("Rampage component flags", "[rampage][serialization]") {
  const ComponentRegistry registry = Registry();
  for (const std::string_view name :
       {"Team", "Stats", "EffectSource", "PairCooldowns", "ActiveEffects",
        "Collider", "KinematicBody", "MoveIntent", "PlayerControl", "Path",
        "Moved"}) {
    INFO(name);
    const ComponentInfo* info = registry.FindByName(name);
    REQUIRE(info != nullptr);
    CHECK(HasFlag(info->flags, ComponentFlags::kHashed));
    CHECK(HasFlag(info->flags, ComponentFlags::kSerialized));
  }
  // The derived path cache is neither.
  const ComponentInfo* cache = registry.FindByName("PathCache");
  REQUIRE(cache != nullptr);
  CHECK_FALSE(HasFlag(cache->flags, ComponentFlags::kHashed));
  CHECK_FALSE(HasFlag(cache->flags, ComponentFlags::kSerialized));
  CHECK(registry.FindByName("Moved")->empty);
}

// A component read by the registry hook of |name|: from |json|, or from
// the BEVE form of |value|; the hook's error when it fails.
template <class T>
std::expected<T, Error> ReadJsonHook(const ComponentRegistry& registry,
                                     std::string_view name,
                                     std::string_view json) {
  const ComponentInfo* info = registry.FindByName(name);
  REQUIRE(info != nullptr);
  entt::registry reg;
  for (const ComponentInfo& each : registry.All()) each.precreate(reg);
  const entt::entity e = reg.create();
  if (auto read = info->read_json(reg, e, json); !read) {
    return std::unexpected(read.error());
  }
  return reg.get<T>(e);
}

template <class T>
std::expected<T, Error> ReadBeveHook(const ComponentRegistry& registry,
                                     std::string_view name, const T& value) {
  const ComponentInfo* info = registry.FindByName(name);
  REQUIRE(info != nullptr);
  entt::registry reg;
  for (const ComponentInfo& each : registry.All()) each.precreate(reg);
  const entt::entity from = reg.create();
  reg.emplace<T>(from, value);
  const auto beve = info->write_beve(reg, from);
  REQUIRE(beve.has_value());
  const entt::entity to = reg.create();
  if (auto read = info->read_beve(reg, to, *beve); !read) {
    return std::unexpected(read.error());
  }
  return reg.get<T>(to);
}

std::vector<StatId> Ids(const Stats& stats) {
  std::vector<StatId> ids;
  ids.reserve(stats.values.size());
  for (const StatValue& value : stats.values) ids.push_back(value.id);
  return ids;
}

std::vector<Guid> Receivers(const PairCooldowns& cooldowns) {
  std::vector<Guid> receivers;
  receivers.reserve(cooldowns.pairs.size());
  for (const PairCooldown& pair : cooldowns.pairs) {
    receivers.push_back(pair.receiver);
  }
  return receivers;
}

TEST_CASE("Sorted lists are sorted on read; repeated keys are errors",
          "[rampage][serialization]") {
  const ComponentRegistry registry = Registry();
  // Stats by id, which FindStat's binary search needs: HP's id is larger
  // than Speed's.
  const auto stats =
      ReadJsonHook<Stats>(registry, "Stats",
                          R"({"values":[{"id":1894470373,"value":100},)"
                          R"({"id":174817312,"value":500}]})");
  REQUIRE(stats.has_value());
  CHECK(Ids(*stats) == std::vector<StatId>{kStatSpeed, kStatHp});
  const float* hp = FindStat(*stats, kStatHp);
  REQUIRE(hp != nullptr);
  REQUIRE(*hp == 100.0F);
  CHECK_FALSE(ReadJsonHook<Stats>(
      registry, "Stats",
      R"({"values":[{"id":5,"value":1},{"id":5,"value":2}]})"));
  const auto beve =
      ReadBeveHook(registry, "Stats", Stats{{{9, 1.0F}, {2, 2.0F}}});
  REQUIRE(beve.has_value());
  CHECK(Ids(*beve) == std::vector<StatId>{2, 9});
  CHECK_FALSE(ReadBeveHook(registry, "Stats", Stats{{{9, 1.0F}, {9, 2.0F}}}));

  // PairCooldowns by receiver Guid, which R2's lookup needs.
  const auto pairs = ReadJsonHook<PairCooldowns>(
      registry, "PairCooldowns",
      R"({"pairs":[{"receiver":{"hi":2,"lo":0},"last":1},)"
      R"({"receiver":{"hi":1,"lo":9},"last":2}]})");
  REQUIRE(pairs.has_value());
  CHECK(Receivers(*pairs) == std::vector<Guid>{{1, 9}, {2, 0}});
  REQUIRE(pairs->pairs[0].last == 2);
  const PairCooldowns twice{{{Guid{1, 1}, 1}, {Guid{1, 1}, 2}}};
  CHECK_FALSE(ReadBeveHook(registry, "PairCooldowns", twice));
  const PairCooldowns unsorted{{{Guid{3, 0}, 1}, {Guid{1, 1}, 2}}};
  const auto beve_pairs = ReadBeveHook(registry, "PairCooldowns", unsorted);
  REQUIRE(beve_pairs.has_value());
  CHECK(Receivers(*beve_pairs) == std::vector<Guid>{{1, 1}, {3, 0}});

  // ActiveEffects by seq (R8); equal seqs keep their order.
  const auto active = ReadJsonHook<ActiveEffects>(
      registry, "ActiveEffects",
      R"({"effects":[{"applied":3,"seq":7},{"applied":1,"seq":2},)"
      R"({"applied":2,"seq":2}]})");
  REQUIRE(active.has_value());
  std::vector<std::uint32_t> applied;
  applied.reserve(active->effects.size());
  for (const ActiveEffect& effect : active->effects) {
    applied.push_back(effect.applied);
  }
  CHECK(applied == std::vector<std::uint32_t>{1, 2, 3});
}

TEST_CASE("Enums serialize by name and reject unknown names",
          "[rampage][serialization]") {
  const ComponentRegistry registry = Registry();
  const ComponentInfo* team = registry.FindByName("Team");
  REQUIRE(team != nullptr);
  entt::registry reg;
  team->precreate(reg);
  const entt::entity e = reg.create();
  REQUIRE(team->read_json(reg, e, R"({"id":"friendly"})").has_value());
  CHECK(reg.get<Team>(e).id == TeamId::kFriendly);
  CHECK_FALSE(team->read_json(reg, e, R"({"id":"pirates"})").has_value());
}

}  // namespace
}  // namespace se::rampage
