// Copyright 2026 The simple-engine-2 Authors.
//
// The panels on prefab instances (M11b; ADR-119): the Hierarchy's prefab
// column and its override marker (none in a prefab document), the
// Inspector's override markers and "Revert to prefab", and its removal,
// which takes what derives from a component and offers no close button
// for what the game keeps. The documents are the repository's sandbox
// and enemy prefab, opened with rampage's scene hooks; the panels draw in
// a headless ImGui context and hand their commands to a recording sink.

#include <imgui.h>

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "editor/support/document_world.h"
#include "editor/support/rampage_editor_game.h"
#include "editor_panels/support/panel_harness.h"
#include "game_rampage/module.h"
#include "game_rampage/scene.h"
#include "se_core/guid.h"
#include "se_core/thread_role.h"
#include "se_editor/commands.h"
#include "se_editor/edit_document.h"
#include "se_editor/panel.h"
#include "se_editor/prefab_commands.h"
#include "se_editor_panels/panel_support.h"
#include "se_editor_panels/panels.h"
#include "se_sim/game_module.h"
#include "support/test_roles.h"

namespace se::editor {
namespace {

using testing::kEnemy1;
using testing::kPlayer;
using testing::PanelHarness;
using testing::RecordingCommands;

PanelContext ContextFor(const EditDocument& doc, const EditorGame& game,
                        RecordingCommands& commands) {
  PanelContext context;
  context.world = &doc.Get();
  context.selection = &commands.selection;
  context.commands = &commands;
  context.game = &game;
  context.asset_root = doc.Desc().asset_root;
  context.prefab_document = doc.Desc().kind == DocumentKind::kPrefab;
  context.document = 1;
  context.revision = doc.Revision() + 1;
  return context;
}

ImVec2 Center(const WidgetProbe& probe, std::string_view key) {
  const std::optional<ImVec2> center = probe.Center(key);
  REQUIRE(center.has_value());
  return center.value_or(ImVec2{});
}

// A collapsing header's close button: at its right end.
ImVec2 CloseButton(const WidgetProbe& probe, std::string_view header) {
  const std::optional<Rect> found = probe.Find(header);
  REQUIRE(found.has_value());
  const Rect rect = found.value_or(Rect{});
  const ImGuiStyle& style = ImGui::GetStyle();
  const float size = ImGui::GetFontSize();
  return {rect.x + rect.w - style.FramePadding.x - size * 0.5F,
          rect.y + rect.h * 0.5F};
}

bool Has(const std::vector<std::string>& names, std::string_view name) {
  for (const std::string& n : names) {
    if (n == name) return true;
  }
  return false;
}

TEST_CASE("The Hierarchy shows each entity's prefab and marks overrides",
          "[editor][hierarchy][prefab]") {
  const testing::MainThreadScope main_thread;
  const ModuleInstance module(rampage::RampageModule());
  const EditorGame game = testing::RampageEditorGame();
  std::unique_ptr<EditDocument> doc =
      testing::OpenSandbox(module.Registry(), game);
  const InlineOwnerScope owner(OnMainThread());
  // An entity without a prefab.
  const Guid plain{0xE0, 0x1};
  REQUIRE(
      doc
          ->Apply(std::make_unique<CreateEntityCommand>(
              plain, std::vector<ComponentJson>{{"Name", R"({"value":"a"})"},
                                                {"Transform", "{}"}}))
          .has_value());
  PanelHarness ui;
  RecordingCommands commands;
  HierarchyPanel hierarchy;
  WidgetProbe probe;
  hierarchy.SetProbe(&probe);
  PanelContext context = ContextFor(*doc, game, commands);
  ui.Frame(hierarchy, context);
  // The player has no override; enemy 1 overrides its Path.
  REQUIRE(hierarchy.PrefabCell(kPlayer) == "player.prefab.json");
  REQUIRE(hierarchy.PrefabCell(kEnemy1) == "enemy.prefab.json *");
  REQUIRE(hierarchy.PrefabCell(plain) == "-");
  // An override of the player marks it (after the document changed).
  REQUIRE(doc
              ->Apply(std::make_unique<SetComponentCommand>(
                  kPlayer, "Collider", "", R"({"half_extents":{"x":5,"y":5}})"))
              .has_value());
  context = ContextFor(*doc, game, commands);
  ui.Frame(hierarchy, context);
  REQUIRE(hierarchy.PrefabCell(kPlayer) == "player.prefab.json *");
  // Play shows the prefab without markers.
  context.playing = true;
  context.commands = nullptr;
  ui.Frame(hierarchy, context);
  REQUIRE(hierarchy.PrefabCell(kEnemy1) == "enemy.prefab.json");
}

TEST_CASE("A prefab document's root has no prefab column",
          "[editor][hierarchy][prefab]") {
  const testing::MainThreadScope main_thread;
  const ModuleInstance module(rampage::RampageModule());
  const EditorGame game = testing::RampageEditorGame();
  DocumentDesc desc;
  desc.kind = DocumentKind::kPrefab;
  desc.path = testing::AssetRoot() + "/prefabs/enemy.prefab.json";
  desc.name = "enemy";
  desc.ref = "prefabs/enemy.prefab.json";
  desc.asset_root = testing::AssetRoot();
  auto text = rampage::ReadTextFile(desc.path);
  REQUIRE(text.has_value());
  std::unique_ptr<EditDocument> doc;
  {
    const InlineOwnerScope owner(OnMainThread());
    auto opened = EditDocument::Open(module.Registry(), game, desc, *text);
    REQUIRE(opened.has_value());
    doc = std::move(*opened);
  }
  PanelHarness ui;
  RecordingCommands commands;
  HierarchyPanel hierarchy;
  const PanelContext context = ContextFor(*doc, game, commands);
  ui.Frame(hierarchy, context);
  REQUIRE(hierarchy.Rows().size() == 1);
  REQUIRE(hierarchy.PrefabCell(hierarchy.Rows().front()).empty());
}

TEST_CASE("The Inspector marks overrides and reverts them to the prefab",
          "[editor][inspector][prefab]") {
  const testing::MainThreadScope main_thread;
  const ModuleInstance module(rampage::RampageModule());
  const EditorGame game = testing::RampageEditorGame();
  std::unique_ptr<EditDocument> doc =
      testing::OpenSandbox(module.Registry(), game);
  const InlineOwnerScope owner(OnMainThread());
  PanelHarness ui;
  RecordingCommands commands;
  commands.selection.Set(std::span<const Guid>(&kEnemy1, 1));
  InspectorPanel inspector;
  WidgetProbe probe;
  inspector.SetProbe(&probe);
  PanelContext context = ContextFor(*doc, game, commands);
  ui.Frame(inspector, context);
  REQUIRE(probe.Find("inspector.prefab").has_value());
  REQUIRE(Has(inspector.Overridden(), "Path"));
  REQUIRE_FALSE(Has(inspector.Overridden(), "Stats"));
  REQUIRE(probe.Find("inspector.override.Path").has_value());
  REQUIRE_FALSE(probe.Find("inspector.override.Stats").has_value());

  ui.Click(inspector, context, Center(probe, "inspector.revert.Path"));
  REQUIRE(commands.commands.size() == 1);
  REQUIRE(commands.commands[0]->Kind() == CommandKind::kSetComponent);
  REQUIRE(commands.ApplyPending(*doc->StateWorld()));
  context = ContextFor(*doc, game, commands);
  context.revision += 1;
  probe.Clear();
  ui.Frame(inspector, context);
  REQUIRE_FALSE(Has(inspector.Overridden(), "Path"));

  // A removed component is an override with its own revert.
  commands.Clear();
  REQUIRE(
      doc->Apply(std::make_unique<RemoveComponentCommand>(kEnemy1, "Collider"))
          .has_value());
  context = ContextFor(*doc, game, commands);
  context.revision += 2;
  probe.Clear();
  ui.Frame(inspector, context);
  REQUIRE(Has(inspector.Overridden(), "Collider"));
  ui.Click(inspector, context, Center(probe, "inspector.revert.Collider"));
  REQUIRE(commands.commands.size() == 1);
  REQUIRE(commands.ApplyPending(*doc->StateWorld()));
  REQUIRE(HoldsComponent(doc->Get(), kEnemy1, "Collider"));
}

TEST_CASE("The Inspector's removal takes what derives from a component",
          "[editor][inspector][prefab]") {
  const testing::MainThreadScope main_thread;
  const ModuleInstance module(rampage::RampageModule());
  const EditorGame game = testing::RampageEditorGame();
  std::unique_ptr<EditDocument> doc =
      testing::OpenSandbox(module.Registry(), game);
  const InlineOwnerScope owner(OnMainThread());
  PanelHarness ui;
  RecordingCommands commands;
  commands.selection.Set(std::span<const Guid>(&kEnemy1, 1));
  InspectorPanel inspector;
  WidgetProbe probe;
  inspector.SetProbe(&probe);
  const PanelContext context = ContextFor(*doc, game, commands);
  ui.Frame(inspector, context);
  // Transform is the game's to keep: its header has no close button.
  ui.Click(inspector, context,
           CloseButton(probe, "inspector.header.Transform"));
  REQUIRE(commands.commands.empty());
  // Path goes with its PathCache, in one step.
  ui.Click(inspector, context, CloseButton(probe, "inspector.header.Path"));
  REQUIRE(commands.commands.size() == 1);
  REQUIRE(commands.commands[0]->Kind() == CommandKind::kGroup);
  REQUIRE(commands.ApplyPending(*doc->StateWorld()));
  REQUIRE_FALSE(HoldsComponent(doc->Get(), kEnemy1, "Path"));
  REQUIRE_FALSE(HoldsComponent(doc->Get(), kEnemy1, "PathCache"));
}

}  // namespace
}  // namespace se::editor
