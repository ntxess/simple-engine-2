// Copyright 2026 The simple-engine-2 Authors.
//
// The editor and the runtime load identical Worlds (docs/milestone-specs.md
// "Integration (M11a + M11b + M13a + M13b)"): for every scene fixture of
// tests/fixtures/scenes and the sandbox, EditDocument::Open (the editor's
// loader) against the "gameplay" state's World of a headless App boot of
// rampage on the same file, whose scene and prefabs load as texts through
// the background lane and the content gate (M13a). Equal Hash, Digests and
// flattened text, equal StatNames and TextureNames. The App runs a
// synthetic clock so slow that no tick runs before the World is compared.
// The reverse direction (a scene the editor saved plays in rampage) is
// rampage.plays_an_editor_saved_scene (tests/rampage/editor_saved_test.py).

#include <SDL3/SDL_timer.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "editor/support/rampage_editor_game.h"
#include "game_rampage/components.h"
#include "game_rampage/content.h"
#include "game_rampage/flow.h"
#include "game_rampage/module.h"
#include "game_rampage/scene.h"
#include "platform/support/test_app.h"
#include "se_core/error.h"
#include "se_core/hash.h"
#include "se_core/thread_role.h"
#include "se_core/world.h"
#include "se_editor/edit_document.h"
#include "se_editor/editor_game.h"
#include "se_platform/fs.h"
#include "se_runtime/app.h"
#include "se_runtime/asset_runtime.h"
#include "se_sim/game_module.h"
#include "se_sim/game_state.h"
#include "support/test_roles.h"

namespace se::editor {
namespace {

// What the comparison reads of a World.
struct WorldView {
  WorldHash hash;
  std::vector<EntityDigest> digests;
  std::string flattened;
  std::vector<std::pair<std::uint32_t, std::string>> stat_names;
  std::vector<std::pair<std::uint32_t, std::string>> texture_names;
  std::size_t entities = 0;
};

WorldView ViewOf(const World& world) {
  WorldView view;
  view.hash = world.Hash();
  view.digests = world.Digests();
  auto text = rampage::SaveScene(world, "compared");
  REQUIRE(text.has_value());
  view.flattened = std::move(*text);
  const auto& ctx = world.CReg().ctx();
  if (const auto* names = ctx.find<rampage::StatNames>()) {
    view.stat_names = names->All();
  }
  if (const auto* names = ctx.find<rampage::TextureNames>()) {
    view.texture_names = names->All();
  }
  view.entities = world.EntityCount();
  return view;
}

// The fixtures, content-relative (the content root is the repository), the
// sandbox last.
std::vector<std::string> ScenePaths() {
  std::vector<std::string> paths;
  for (const char* name :
       {"removed_component", "added_component", "nested_member",
        "stats_by_name", "no_prefab", "camera", "sprite_tint"}) {
    paths.push_back(std::string("tests/fixtures/scenes/") + name +
                    ".scene.json");
  }
  paths.emplace_back(rampage::kDefaultGameplayScene);
  return paths;
}

// rampage's runtime setup (games/rampage/app/main.cc) on the headless test
// App: the flow states' texts through the lane, the scene's prefabs as
// their dependencies, starting in "gameplay" on |scene|.
GameSetup RampageSetup(const std::string& scene) {
  GameSetup setup = testing::TestGameSetup();
  setup.module = &rampage::RampageModule();
  setup.title = "editor and runtime Worlds";
  setup.seed = 1;
  setup.tick_rate = 120;
  setup.start_state = std::string(rampage::kGameplayState);
  setup.state_assets = [](std::string_view state_id) {
    StateAssets assets;
    const std::string text = rampage::FlowSceneOf(state_id);
    if (!text.empty()) assets.texts.push_back(text);
    return assets;
  };
  setup.text_dependencies = [](std::string_view path, std::string_view text) {
    return rampage::SceneDependencies(path, text, rampage::kContentAssetDir);
  };
  // The Gameplay scene, set before the App pushes the state (the content
  // root is the repository, --content-root below).
  rampage::FlowConfig flow;
  flow.gameplay_scene = scene;
  flow.content_root = SE_SOURCE_DIR;
  rampage::SetFlowConfig(std::move(flow));
  return setup;
}

// The "gameplay" World of a headless rampage boot on |scene|, before its
// first tick.
WorldView RuntimeWorld(const std::string& scene) {
  std::optional<WorldView> view;
  bool ticked_first = false;
  const std::uint64_t start_ns = SDL_GetTicksNS();
  GameSetup setup = RampageSetup(scene);
  setup.on_frame = [&](App& app, const FrameReport& report) {
    AssertMainThread();
    const GameStateStack* states = app.GameStates();
    if (!view && states != nullptr && states->Depth() == 1 &&
        states->TopId() == rampage::kGameplayState) {
      ticked_first = report.next_tick != 1;
      const InlineOwnerScope owner(OnMainThread());
      view = ViewOf(*states->TopWorlds().front());
      app.RequestQuit();
    }
    if (SDL_GetTicksNS() - start_ns > 30'000'000'000ULL) app.RequestQuit();
  };
  // 1 us frames: thousands of frames before the first 120 Hz tick.
  const int code = testing::RunTestApp(
      std::move(setup),
      {"--headless", "--frames", "100000000", "--fixed-frame-dt", "0.001",
       "--audio", "none", "--content-root", SE_SOURCE_DIR});
  REQUIRE(code == 0);
  REQUIRE_FALSE(ticked_first);
  if (!view) {
    FAIL("the gameplay state was never created");
    return {};
  }
  return *view;
}

WorldView EditorWorld(const std::string& scene) SE_REQUIRES(g_main) {
  const ModuleInstance module(rampage::RampageModule());
  const EditorGame game = testing::RampageEditorGame();
  DocumentDesc desc;
  desc.path = fs::Join(SE_SOURCE_DIR, scene);
  desc.name = "compared";
  desc.asset_root = testing::AssetRoot();
  desc.seed = 1;
  auto text = fs::ReadTextFile(desc.path);
  REQUIRE(text.has_value());
  const InlineOwnerScope owner(OnMainThread());
  auto opened =
      EditDocument::Open(module.Registry(), game, std::move(desc), *text);
  REQUIRE(opened.has_value());
  return ViewOf((*opened)->Get());
}

TEST_CASE("The editor and the runtime load identical Worlds",
          "[integration][content]") {
  for (const std::string& scene : ScenePaths()) {
    INFO(scene);
    const WorldView runtime = RuntimeWorld(scene);
    const testing::MainThreadScope main_thread;
    const WorldView editor = EditorWorld(scene);
    const bool loaded_any = runtime.entities != 0;
    CHECK(loaded_any);
    CHECK(editor.entities == runtime.entities);
    CHECK(editor.hash == runtime.hash);
    CHECK(editor.digests == runtime.digests);
    CHECK(editor.flattened == runtime.flattened);
    CHECK(editor.stat_names == runtime.stat_names);
    CHECK(editor.texture_names == runtime.texture_names);
  }
}

}  // namespace
}  // namespace se::editor
