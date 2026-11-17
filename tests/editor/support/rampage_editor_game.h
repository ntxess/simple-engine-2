// Copyright 2026 The simple-engine-2 Authors.
//
// rampage as the editor's tests see it: the EditorGame simple_editor gives
// the editor (apps/editor/main.cc), the sandbox scene's text, and an
// EditDocument opened from it. Use inside a MainThreadScope.

#ifndef TESTS_EDITOR_SUPPORT_RAMPAGE_EDITOR_GAME_H_
#define TESTS_EDITOR_SUPPORT_RAMPAGE_EDITOR_GAME_H_

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "editor/support/document_world.h"
#include "game_rampage/module.h"
#include "game_rampage/scene.h"
#include "game_rampage/view.h"
#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/world.h"
#include "se_editor/edit_document.h"
#include "se_editor/editor_game.h"
#include "se_sim/game_module.h"

namespace se::testing {

[[nodiscard]] inline std::string AssetRoot() {
  return std::string(SE_SOURCE_DIR) + "/assets";
}

[[nodiscard]] inline editor::EditorGame RampageEditorGame() {
  editor::EditorGame game;
  game.module = &rampage::RampageModule();
  game.load_scene =
      [](World& world, std::string_view text,
         const std::string& asset_root) -> std::expected<void, Error> {
    AssertMainThread();
    const InlineOwnerScope owner(OnMainThread());
    rampage::SceneLoadOptions options;
    options.asset_root = asset_root;
    auto loaded = rampage::LoadScene(world, text, options);
    if (!loaded) return std::unexpected(std::move(loaded.error()));
    return {};
  };
  game.save_scene = [](const World& world, std::string_view name) {
    return rampage::SaveScene(world, name);
  };
  game.camera = [](const World& world) {
    return rampage::SandboxCamera(world);
  };
  return game;
}

[[nodiscard]] inline std::string SandboxText() {
  auto text = rampage::ReadTextFile(SandboxScenePath());
  REQUIRE(text.has_value());
  return *text;
}

// The sandbox as an EditDocument.
[[nodiscard]] inline std::unique_ptr<editor::EditDocument> OpenSandbox(
    const ComponentRegistry& registry, const editor::EditorGame& game)
    SE_REQUIRES(g_main) {
  editor::DocumentDesc desc;
  desc.path = SandboxScenePath();
  desc.name = "sandbox";
  desc.asset_root = AssetRoot();
  desc.seed = 1;
  const InlineOwnerScope owner(OnMainThread());
  auto opened = editor::EditDocument::Open(registry, game, std::move(desc),
                                           SandboxText());
  REQUIRE(opened.has_value());
  return std::move(*opened);
}

}  // namespace se::testing

#endif  // TESTS_EDITOR_SUPPORT_RAMPAGE_EDITOR_GAME_H_
