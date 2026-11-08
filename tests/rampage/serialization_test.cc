// Copyright 2026 The simple-engine-2 Authors.
//
// JSON and BEVE round trips for every component game_rampage registers
// (docs/milestone-specs.md, M3b "Components"): each registered name needs a
// sample here, so a new component cannot skip the test.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <entt/entity/registry.hpp>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "game_rampage/components.h"
#include "se_core/component_registry.h"
#include "se_core/core_components.h"
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
        "Collider", "KinematicBody", "MoveIntent", "PlayerControl", "Path"}) {
    INFO(name);
    const ComponentInfo* info = registry.FindByName(name);
    REQUIRE(info != nullptr);
    CHECK(HasFlag(info->flags, ComponentFlags::kHashed));
    CHECK(HasFlag(info->flags, ComponentFlags::kSerialized));
  }
  // The derived path cache is neither; Moved is not saved.
  const ComponentInfo* cache = registry.FindByName("PathCache");
  REQUIRE(cache != nullptr);
  CHECK_FALSE(HasFlag(cache->flags, ComponentFlags::kHashed));
  CHECK_FALSE(HasFlag(cache->flags, ComponentFlags::kSerialized));
  const ComponentInfo* moved = registry.FindByName("Moved");
  REQUIRE(moved != nullptr);
  CHECK_FALSE(HasFlag(moved->flags, ComponentFlags::kSerialized));
  CHECK(moved->empty);
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
