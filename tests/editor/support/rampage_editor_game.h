// Copyright 2026 The simple-engine-2 Authors.
//
// rampage as the editor's tests see it: the EditorGame simple_editor gives
// the editor (apps/editor/main.cc), the sandbox scene's text, and an
// EditDocument opened from it. Use inside a MainThreadScope.

#ifndef TESTS_EDITOR_SUPPORT_RAMPAGE_EDITOR_GAME_H_
#define TESTS_EDITOR_SUPPORT_RAMPAGE_EDITOR_GAME_H_

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
#include "se_editor_panels/stat_name_command.h"
#include "se_sim/game_module.h"

namespace se::testing {

[[nodiscard]] inline std::string AssetRoot() {
  return std::string(SE_SOURCE_DIR) + "/assets";
}

[[nodiscard]] inline editor::EntityJson ToEntityJson(
    rampage::EntityComponentsJson entity) {
  editor::EntityJson out;
  out.guid = entity.guid;
  out.components.reserve(entity.components.size());
  for (auto& [name, json] : entity.components) {
    out.components.push_back(
        editor::ComponentJson{std::move(name), std::move(json)});
  }
  return out;
}

// What simple_editor gives the editor (apps/editor/main.cc), the M11
// hooks included.
[[nodiscard]] inline editor::EditorGame RampageEditorGame() {
  editor::EditorGame game;
  game.module = &rampage::RampageModule();
  game.load_scene =
      [](World& world, std::string_view text, const std::string& asset_root,
         const std::function<void(std::string_view, std::string_view)>&
             on_prefab) -> std::expected<void, Error> {
    AssertMainThread();
    const InlineOwnerScope owner(OnMainThread());
    rampage::SceneLoadOptions options;
    options.asset_root = asset_root;
    options.on_prefab = on_prefab;
    auto loaded = rampage::LoadScene(world, text, options);
    if (!loaded) return std::unexpected(std::move(loaded.error()));
    return {};
  };
  game.save_scene = [](const World& world, std::string_view name) {
    // Flattened (kFlatten): the Play-start bytes, faults, the content hash.
    return rampage::SaveScene(world, name);
  };
  game.save_scene_file = [](const World& world, std::string_view name,
                            const std::string& asset_root,
                            const editor::PrefabTextMap* prefab_texts) {
    // With prefab references (kPrefabs): Save, Save As, autosave,
    // recovery.
    rampage::SceneWriteOptions options;
    options.mode = rampage::SceneWriteMode::kPrefabs;
    options.asset_root = asset_root;
    if (prefab_texts != nullptr) {
      options.resolver = rampage::TextsFirst(*prefab_texts, asset_root);
    }
    return rampage::SaveScene(world, name, options);
  };
  game.load_prefab =
      [](World& world, std::string_view text, const std::string& ref,
         const std::string& asset_root) -> std::expected<void, Error> {
    AssertMainThread();
    const InlineOwnerScope owner(OnMainThread());
    rampage::SceneLoadOptions options;
    options.asset_root = asset_root;
    auto loaded = rampage::LoadPrefabDocument(world, text, ref, options);
    if (!loaded) return std::unexpected(std::move(loaded.error()));
    return {};
  };
  game.save_prefab = [](const World& world, const std::string& ref,
                        std::string_view name) {
    return rampage::SavePrefabDocument(world, ref, name);
  };
  game.prefab_baseline = [](const World& world, Guid guid,
                            const std::string& asset_root,
                            const editor::PrefabTextMap* prefab_texts)
      -> std::expected<editor::EntityJson, Error> {
    rampage::SceneLoadOptions options;
    options.asset_root = asset_root;
    if (prefab_texts != nullptr) {
      options.resolver = rampage::TextsFirst(*prefab_texts, asset_root);
    }
    auto baseline = rampage::PrefabBaseline(world, guid, options);
    if (!baseline) return std::unexpected(std::move(baseline.error()));
    return ToEntityJson(std::move(*baseline));
  };
  game.rebase_instances =
      [](const World& world, std::string_view ref, std::string_view old_text,
         std::string_view new_text, const std::string& asset_root)
      -> std::expected<std::vector<editor::EntityJson>, Error> {
    rampage::SceneLoadOptions options;
    options.asset_root = asset_root;
    auto rebased =
        rampage::RebasePrefabInstances(world, ref, old_text, new_text, options);
    if (!rebased) return std::unexpected(std::move(rebased.error()));
    std::vector<editor::EntityJson> out;
    out.reserve(rebased->size());
    for (rampage::EntityComponentsJson& entity : *rebased) {
      out.push_back(ToEntityJson(std::move(entity)));
    }
    return out;
  };
  game.prefab_names = &editor::MakePrefabNames;
  game.removed_with = [](std::string_view component) {
    std::vector<std::string> out;
    for (const std::string_view derived : rampage::RemovedWith(component)) {
      out.emplace_back(derived);
    }
    return out;
  };
  game.removable = [](std::string_view component) {
    return rampage::Removable(component);
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
