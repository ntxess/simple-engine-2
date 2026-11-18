// Copyright 2026 The simple-engine-2 Authors.
//
// SpriteRenderer and Camera2D: registration flags, JSON and BEVE round
// trips, staying out of the world hash, and PrimaryCamera().

#include "se_render_core/render_components.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "render/render_fixture.h"
#include "se_core/component_registry.h"
#include "se_core/guid.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render_core/render_list.h"
#include "support/test_roles.h"

namespace se {
namespace {

const ComponentInfo& Info(std::string_view name) {
  const ComponentInfo* info = testing::RenderRegistry().FindByName(name);
  REQUIRE(info != nullptr);
  return *info;
}

TEST_CASE("Render components are serialized but not hashed",
          "[render][components]") {
  for (const std::string_view name : {"SpriteRenderer", "Camera2D"}) {
    INFO(name);
    const ComponentInfo& info = Info(name);
    CHECK(HasFlag(info.flags, ComponentFlags::kSerialized));
    CHECK_FALSE(HasFlag(info.flags, ComponentFlags::kHashed));
  }
}

TEST_CASE("Render components round-trip through JSON and BEVE",
          "[render][components]") {
  // Non-default values in glaze's canonical JSON form.
  const std::string_view samples[][2] = {
      {"SpriteRenderer",
       R"({"texture":4000000000,"region":{"x":16,"y":32,"w":8.5,"h":4},)"
       R"("origin":{"x":2,"y":-1.25},"tint":{"r":1,"g":2,"b":3,"a":200},)"
       R"("layer":-7,"z":0.75,"flip_x":true,"flip_y":false})"},
      {"Camera2D", R"({"center":{"x":960,"y":-540.5},"zoom":2.5,)"
                   R"("virtual_size":{"x":640,"y":360},"integer_scale":true,)"
                   R"("primary":true})"},
  };
  entt::registry reg;
  for (const auto& [name, json] : samples) {
    INFO(name);
    const ComponentInfo& info = Info(name);
    const entt::entity a = reg.create();
    REQUIRE(info.read_json(reg, a, json).has_value());
    const auto written = info.write_json(reg, a);
    REQUIRE(written.has_value());
    CHECK(*written == json);

    const auto beve = info.write_beve(reg, a);
    REQUIRE(beve.has_value());
    const entt::entity b = reg.create();
    REQUIRE(
        info.read_beve(reg, b, std::span<const std::byte>(*beve)).has_value());
    const auto again = info.write_json(reg, b);
    REQUIRE(again.has_value());
    CHECK(*again == json);
  }
}

TEST_CASE("Changing a sprite does not change the world hash",
          "[render][components]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(testing::RenderRegistry(), {});
  const entt::entity e = testing::AddSprite(
      world, 1, {10.0F, 10.0F}, testing::CenteredSprite(1, 4.0F, 4.0F));
  const std::uint64_t before = world.Hash().Folded();
  SpriteRenderer& sprite = world.Reg().get<SpriteRenderer>(e);
  sprite.texture = 99;
  sprite.layer = 3;
  sprite.flip_x = true;
  world.Reg().emplace<Camera2D>(e);
  CHECK(world.Hash().Folded() == before);
}

TEST_CASE("PrimaryCamera picks the primary camera with the lowest serial",
          "[render][components]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  World world(testing::RenderRegistry(), {});
  CHECK_FALSE(PrimaryCamera(world).has_value());

  Camera2D side;
  side.center = {1.0F, 1.0F};
  world.Reg().emplace<Camera2D>(world.Create(Guid{0, 1}), side);
  CHECK_FALSE(PrimaryCamera(world).has_value());

  Camera2D first;
  first.center = {2.0F, 2.0F};
  first.primary = true;
  const entt::entity first_entity = world.Create(Guid{0, 2});
  Camera2D second = first;
  second.center = {3.0F, 3.0F};
  // Emplaced in the opposite order of creation, so storage order differs.
  world.Reg().emplace<Camera2D>(world.Create(Guid{0, 3}), second);
  world.Reg().emplace<Camera2D>(first_entity, first);

  const std::optional<Camera2D> primary = PrimaryCamera(world);
  REQUIRE(primary.has_value());
  CHECK(primary.value_or(Camera2D{}).center == Vec2{2.0F, 2.0F});
}

}  // namespace
}  // namespace se
