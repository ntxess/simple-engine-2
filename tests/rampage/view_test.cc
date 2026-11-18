// Copyright 2026 The simple-engine-2 Authors.
//
// game_rampage's sprites and view: the sandbox scene and the legacy
// scenarios carry SpriteRenderers (texture names from config/texture.json,
// the legacy Sprite's whole texture and centred origin), the sandbox draws
// through the arena camera unless a scene places one, texture names
// survive SaveScene, and sprites never change the world hash.

#include "game_rampage/view.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <string>
#include <vector>

#include "game_rampage/components.h"
#include "game_rampage/legacy_scenario.h"
#include "game_rampage/scene.h"
#include "rampage/support/rampage_session.h"
#include "rampage/support/test_world.h"
#include "se_core/core_components.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"
#include "se_render_core/render_list.h"
#include "support/test_roles.h"

namespace se::rampage {
namespace {

constexpr std::uint32_t kPlayerTexture = TextureKeyOf("player");

SceneLoadOptions AssetOptions() {
  SceneLoadOptions options;
  options.asset_root = testing::RampageAssetRoot();
  return options;
}

void LoadSandbox(testing::RampageTestWorld& w) SE_REQUIRES(g_sim_owner) {
  REQUIRE(LoadSceneFile(
      w.Get(), testing::RampageAssetRoot() + "/scenes/sandbox.scene.json",
      AssetOptions()));
}

TEST_CASE("The sandbox scene draws every entity with the player texture",
          "[rampage][render]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::RampageTestWorld w;
  LoadSandbox(w);
  const entt::registry& reg = w.Get().CReg();
  std::size_t sprites = 0;
  for (const auto [e, sprite] : reg.view<const SpriteRenderer>().each()) {
    CHECK(sprite.texture == kPlayerTexture);
    CHECK(sprite.region == Rect{0.0F, 0.0F, 48.0F, 48.0F});
    CHECK(sprite.origin == Vec2{24.0F, 24.0F});
    CHECK(sprite.tint == Rgba8{});
    ++sprites;
  }
  REQUIRE(sprites == 17);
  CHECK(reg.ctx().get<TextureNames>().Name(kPlayerTexture) == "player");

  // No scene camera: the whole arena, as the legacy scene view.
  const Camera2D camera = SandboxCamera(w.Get());
  CHECK(camera.center == Vec2{960.0F, 540.0F});
  CHECK(camera.virtual_size == Vec2{1920.0F, 1080.0F});
  CHECK(camera.zoom == 1.0F);

  RenderList list;
  BuildRenderList(w.Get(), camera, 1.0F, list);
  REQUIRE(list.quads.size() == 17);
  REQUIRE(list.batches.size() == 1);
  CHECK(list.batches[0].texture == kPlayerTexture);
  REQUIRE(list.batches[0].count == 17);
  // Spawn serial 0, the player at (200, 200): the 48 x 48 texture centred.
  const SpriteQuad& player = list.quads[0];
  CHECK(player.pos[0] == Vec2{176.0F, 176.0F});
  CHECK(player.pos[2] == Vec2{224.0F, 224.0F});
  CHECK(player.uv[0] == Vec2{0.0F, 0.0F});
  CHECK(player.uv[2] == Vec2{48.0F, 48.0F});

  // Ticks keep drawing every entity (the clamp keeps them in the arena).
  w.Step(240, 0x8);
  BuildRenderList(w.Get(), SandboxCamera(w.Get()), 0.5F, list);
  REQUIRE(list.quads.size() == 17);
  REQUIRE(list.stats.interpolated == 17);
}

TEST_CASE("Sprites do not change the world hash", "[rampage][render]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::RampageTestWorld drawn;
  LoadSandbox(drawn);
  testing::RampageTestWorld bare;
  LoadSandbox(bare);
  std::vector<entt::entity> with_sprites;
  for (const entt::entity e : bare.Reg().view<SpriteRenderer>()) {
    with_sprites.push_back(e);
  }
  REQUIRE(with_sprites.size() == 17);
  bare.Reg().remove<SpriteRenderer>(with_sprites.begin(), with_sprites.end());
  CHECK(bare.Get().Hash() == drawn.Get().Hash());
  CHECK(bare.Get().Digests() == drawn.Get().Digests());
  for (int i = 0; i < 4; ++i) {
    drawn.Step(150, 0x1 | 0x8);
    bare.Step(150, 0x1 | 0x8);
    CHECK(bare.Get().Hash() == drawn.Get().Hash());
  }
  // Building a RenderList reads the World only.
  const WorldHash before = drawn.Get().Hash();
  RenderList list;
  BuildRenderList(drawn.Get(), SandboxCamera(drawn.Get()), 0.25F, list);
  CHECK(drawn.Get().Hash() == before);
}

TEST_CASE("Texture names survive saving and reloading", "[rampage][render]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::RampageTestWorld original;
  LoadSandbox(original);
  const auto saved = SaveScene(original.Get(), "sandbox");
  REQUIRE(saved.has_value());
  CHECK(saved->find("\"texture\": \"player\"") != std::string::npos);
  testing::RampageTestWorld reloaded;
  REQUIRE(LoadScene(reloaded.Get(), *saved, SceneLoadOptions{}));
  std::size_t sprites = 0;
  for (const auto [e, sprite] :
       reloaded.Get().CReg().view<const SpriteRenderer>().each()) {
    CHECK(sprite.texture == kPlayerTexture);
    ++sprites;
  }
  REQUIRE(sprites == 17);
}

std::string OneEntity(const std::string& components) {
  return R"({"format": "se-scene-v1", "entities": [{"guid": )"
         R"("00000000-0000-0001-0000-000000000001", "components": )" +
         components + "}]}";
}

TEST_CASE("A scene's primary camera replaces the arena camera",
          "[rampage][render]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  testing::RampageTestWorld w;
  REQUIRE(LoadScene(w.Get(),
                    OneEntity(R"({"Camera2D": {)"
                              R"("center": {"x": 100, "y": 50},)"
                              R"( "zoom": 2, "primary": true}})"),
                    SceneLoadOptions{}));
  const Camera2D camera = SandboxCamera(w.Get());
  CHECK(camera.center == Vec2{100.0F, 50.0F});
  CHECK(camera.zoom == 2.0F);

  // Texture names must not be empty; a key is accepted as it is.
  testing::RampageTestWorld empty;
  CHECK_FALSE(LoadScene(empty.Get(),
                        OneEntity(R"({"SpriteRenderer": {"texture": ""}})"),
                        SceneLoadOptions{}));
  testing::RampageTestWorld keyed;
  REQUIRE(LoadScene(keyed.Get(),
                    OneEntity(R"({"SpriteRenderer": {"texture": 7}})"),
                    SceneLoadOptions{}));
  for (const auto [e, sprite] :
       keyed.Get().CReg().view<const SpriteRenderer>().each()) {
    REQUIRE(sprite.texture == 7);
  }
}

TEST_CASE("Legacy scenarios draw like the legacy Sprite", "[rampage][render]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  for (const std::string name : {"stress", "effects", "patrol"}) {
    INFO(name);
    testing::RampageTestWorld w;
    LegacyScenarioOptions options;
    options.root = testing::RampageRepoRoot();
    REQUIRE(LoadLegacyScenario(
        w.Get(),
        testing::RampageFixtureDir() + "/reference/" + name + ".scenario.json",
        options));
    const entt::registry& reg = w.Get().CReg();
    std::size_t sprites = 0;
    for (const auto [e, sprite, collider] :
         reg.view<const SpriteRenderer, const Collider>().each()) {
      // The whole texture with its origin at the centre; the collider is
      // the same box (R4).
      CHECK(sprite.texture == kPlayerTexture);
      CHECK(sprite.region == Rect{0.0F, 0.0F, 2.0F * collider.half_extents.x,
                                  2.0F * collider.half_extents.y});
      CHECK(sprite.origin == collider.half_extents);
      ++sprites;
    }
    CHECK(sprites == w.Get().EntityCount());
    RenderList list;
    BuildRenderList(w.Get(), SandboxCamera(w.Get()), 1.0F, list);
    CHECK(list.stats.sprites == sprites);
  }
}

}  // namespace
}  // namespace se::rampage
