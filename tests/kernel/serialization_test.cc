// Copyright 2026 The simple-engine-2 Authors.
//
// JSON and BEVE round trips for every registered component: the core
// components and the kernel test module's. Every registered name needs a
// sample here, so a new component cannot skip the test.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <entt/entity/registry.hpp>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/component_registry.h"
#include "se_core/core_components.h"
#include "se_core/hash.h"
#include "support/kernel_test_components.h"

namespace se {
namespace {

// A non-default value for each component, in its canonical JSON form (the
// way glaze writes it back).
const std::map<std::string_view, std::string_view>& Samples() {
  static const std::map<std::string_view, std::string_view> kSamples = {
      {"IdComponent", R"({"guid":{"hi":1311768467463790320,"lo":42}})"},
      {"Name", R"({"value":"mover \"seven\""})"},
      {"Transform",
       R"({"pos":{"x":1.5,"y":-2.25},"rot":0.125,"scale":{"x":2,"y":0.5},)"
       R"("teleport_tick":77})"},
      {"PrevTransform", R"({"pos":{"x":-1,"y":3.0517578E-5},"rot":3.1415927,)"
                        R"("scale":{"x":1,"y":1},"tick":76})"},
      {"Spawned", R"({"spawn_tick":12,"spawn_serial":4000000000})"},
      {"DestroyRequest", R"({})"},
      {"PrefabInstance", R"({"prefab":"prefabs/enemy.prefab.json"})"},
      {"Mover",
       R"({"center":{"x":100,"y":-50},"radius":32.5,"angular_speed":-1.25,)"
       R"("phase":0.75})"},
      {"Spawner",
       R"({"every_ticks":30,"spawned":3,"max_alive":8,"child_ttl":90})"},
      {"Lifetime", R"({"expire_tick":1234})"},
      {"Player", R"({"speed":180,"inputs":17})"},
      {"Follow", R"({"target":{"guid":{"hi":0,"lo":9}},"distance":12.5})"},
      {"Banner", R"({"text":"GAME OVER","shown_since":300})"},
  };
  return kSamples;
}

ComponentRegistry FullRegistry() {
  ComponentRegistry registry;
  RegisterCoreComponents(registry);
  testing::RegisterKernelTestComponents(registry);
  registry.Seal();
  return registry;
}

TEST_CASE("Every registered component has a round-trip sample",
          "[serialization]") {
  const ComponentRegistry registry = FullRegistry();
  for (const ComponentInfo& info : registry.All()) {
    INFO(info.name);
    CHECK(Samples().contains(info.name));
  }
}

TEST_CASE("JSON and BEVE round trips for every registered component",
          "[serialization]") {
  const ComponentRegistry registry = FullRegistry();
  for (const ComponentInfo& info : registry.All()) {
    INFO(info.name);
    const auto sample = Samples().find(info.name);
    if (sample == Samples().end()) continue;  // reported above
    entt::registry reg;
    for (const ComponentInfo& each : registry.All()) each.precreate(reg);
    const entt::entity a = reg.create();
    const entt::entity b = reg.create();
    const entt::entity c = reg.create();

    // JSON -> component -> JSON gives the canonical text back.
    REQUIRE(info.read_json(reg, a, sample->second).has_value());
    const auto json = info.write_json(reg, a);
    REQUIRE(json.has_value());
    CHECK(*json == sample->second);

    // JSON text round trip into another entity.
    REQUIRE(info.read_json(reg, b, *json).has_value());
    CHECK(info.write_json(reg, b).value() == *json);

    // BEVE round trip.
    const auto beve = info.write_beve(reg, a);
    REQUIRE(beve.has_value());
    CHECK_FALSE(beve->empty());
    REQUIRE(info.read_beve(reg, c, *beve).has_value());
    CHECK(info.write_json(reg, c).value() == *json);
    CHECK(info.write_beve(reg, c).value() == *beve);

    // The hash survives both round trips.
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

TEST_CASE("Malformed input is an error, not a throw", "[serialization]") {
  const ComponentRegistry registry = FullRegistry();
  const ComponentInfo* transform = registry.FindByName("Transform");
  REQUIRE(transform != nullptr);
  entt::registry reg;
  transform->precreate(reg);
  const entt::entity e = reg.create();
  const auto bad = transform->read_json(reg, e, R"({"pos":{"x":)");
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().code == ErrorCode::kParse);
  const auto unknown = transform->read_json(reg, e, R"({"position":{}})");
  REQUIRE_FALSE(unknown.has_value());
  CHECK_FALSE(reg.all_of<Transform>(e));
  const std::vector<std::byte> junk(3, std::byte{0xFF});
  CHECK_FALSE(transform->read_beve(reg, e, junk).has_value());
  CHECK_FALSE(transform->write_json(reg, e).has_value());
}

TEST_CASE("The registry finds components by storage id and name",
          "[serialization]") {
  const ComponentRegistry registry = FullRegistry();
  const ComponentInfo* by_name = registry.FindByName("Transform");
  REQUIRE(by_name != nullptr);
  CHECK(registry.Find(by_name->id) == by_name);
  CHECK(registry.FindByNameId(Fnv1a32("Transform")) == by_name);
  CHECK(registry.FindByName("Nope") == nullptr);
  CHECK(HasFlag(by_name->flags, ComponentFlags::kHashed));
  CHECK(HasFlag(by_name->flags, ComponentFlags::kInterpolated));
  CHECK_FALSE(HasFlag(registry.FindByName("IdComponent")->flags,
                      ComponentFlags::kHashed));
  CHECK(registry.FindByName("DestroyRequest")->empty);
  CHECK(registry.Sealed());
}

}  // namespace
}  // namespace se
