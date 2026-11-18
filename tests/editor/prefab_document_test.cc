// Copyright 2026 The simple-engine-2 Authors.
//
// The editor's prefab work on rampage's scene contract (M11b; ADR-117 to
// ADR-121): undo then redo is the identity on the document hash and the
// content hash for every command kind (StatNameCommand, the removal
// groups, Revert to prefab and a prefab rebase included, each alone and in
// a mixed sequence of 50); a removal takes what derives from it; a typed
// stat reaches StatNames and the file by name; a saved or changed prefab
// is one undo step per open scene; prefab documents; the Play-start bytes
// stay flattened; recovery files keep the prefab references.
//
// The documents load a copy of the repository's sandbox and prefabs under
// the test's output directory, so the tests may write prefab files.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "editor/support/document_world.h"
#include "editor/support/rampage_editor_game.h"
#include "game_rampage/components.h"
#include "game_rampage/module.h"
#include "game_rampage/scene.h"
#include "se_core/guid.h"
#include "se_core/thread_role.h"
#include "se_editor/commands.h"
#include "se_editor/documents.h"
#include "se_editor/edit_document.h"
#include "se_editor/panel.h"
#include "se_editor/prefab_commands.h"
#include "se_editor/recovery.h"
#include "se_editor_panels/panel_support.h"
#include "se_editor_panels/stat_name_command.h"
#include "se_editor_panels/stress_generator.h"
#include "se_platform/fs.h"
#include "se_sim/game_module.h"
#include "support/test_roles.h"

namespace se::editor {
namespace {

using testing::kEnemy1;
using testing::kEnemy2;
using testing::kPlayer;

// More of the sandbox's enemies.
constexpr Guid kEnemy3{0x5a4db0c500004000ULL, 0x8000000000000003ULL};
constexpr Guid kEnemy4{0x5a4db0c500004000ULL, 0x8000000000000004ULL};
constexpr Guid kEnemy5{0x5a4db0c500004000ULL, 0x8000000000000005ULL};

constexpr std::string_view kEnemyRef = "prefabs/enemy.prefab.json";

// |text| without its layout (the writer indents; no tested key or value
// holds a space).
std::string Compact(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (c != ' ' && c != '\n' && c != '\r') out.push_back(c);
  }
  return out;
}

std::span<const std::byte> Bytes(std::string_view text) {
  return std::as_bytes(std::span<const char>(text));
}

// A copy of the sandbox and both prefabs under the test's output
// directory, named |name|: <out>/<name>/assets/...
struct Content {
  explicit Content(std::string_view name) {
    root = std::string(SE_TEST_OUT_DIR) + "/" + std::string(name);
    assets = root + "/assets";
    for (const char* file :
         {"scenes/sandbox.scene.json", "prefabs/enemy.prefab.json",
          "prefabs/player.prefab.json"}) {
      auto text =
          fs::ReadTextFile(std::string(SE_SOURCE_DIR) + "/assets/" + file);
      REQUIRE(text.has_value());
      const std::string to = assets + "/" + file;
      REQUIRE(fs::CreateDirectories(fs::Parent(to)).has_value());
      REQUIRE(fs::WriteFileAtomic(to, Bytes(*text)).has_value());
    }
    scene = assets + "/scenes/sandbox.scene.json";
  }
  [[nodiscard]] std::string Read(std::string_view rel) const {
    auto text = fs::ReadTextFile(assets + "/" + std::string(rel));
    REQUIRE(text.has_value());
    return *text;
  }
  void Write(std::string_view rel, std::string_view text) const {
    REQUIRE(fs::WriteFileAtomic(assets + "/" + std::string(rel), Bytes(text))
                .has_value());
  }

  std::string root;
  std::string assets;
  std::string scene;
};

std::unique_ptr<EditDocument> OpenScene(const ComponentRegistry& registry,
                                        const EditorGame& game,
                                        const Content& content,
                                        std::string_view text = {})
    SE_REQUIRES(g_main) {
  DocumentDesc desc;
  desc.path = content.scene;
  desc.name = "sandbox";
  desc.asset_root = content.assets;
  desc.seed = 1;
  const std::string file = text.empty()
                               ? content.Read("scenes/sandbox.scene.json")
                               : std::string(text);
  const InlineOwnerScope owner(OnMainThread());
  auto opened = EditDocument::Open(registry, game, std::move(desc), file);
  REQUIRE(opened.has_value());
  return std::move(*opened);
}

std::unique_ptr<EditDocument> OpenPrefab(const ComponentRegistry& registry,
                                         const EditorGame& game,
                                         const Content& content)
    SE_REQUIRES(g_main) {
  DocumentDesc desc;
  desc.kind = DocumentKind::kPrefab;
  desc.path = content.assets + "/" + std::string(kEnemyRef);
  desc.name = "enemy";
  desc.ref = std::string(kEnemyRef);
  desc.asset_root = content.assets;
  const InlineOwnerScope owner(OnMainThread());
  auto opened = EditDocument::Open(registry, game, std::move(desc),
                                   content.Read(kEnemyRef));
  REQUIRE(opened.has_value());
  return std::move(*opened);
}

std::string JsonOf(const World& world, Guid guid, std::string_view name) {
  auto bundle = SerializeEntity(world, guid);
  REQUIRE(bundle.has_value());
  for (const ComponentJson& component : *bundle) {
    if (component.component == name) return component.json;
  }
  return {};
}

float StatOf(const World& world, Guid guid, std::string_view stat) {
  const auto entity = world.Find(guid);
  REQUIRE(entity.has_value());
  const auto* stats = world.CReg().try_get<rampage::Stats>(*entity);
  REQUIRE(stats != nullptr);
  const float* value = rampage::FindStat(*stats, rampage::StatIdOf(stat));
  REQUIRE(value != nullptr);
  return *value;
}

// The enemy prefab with HP |hp| (and, when given, Speed |speed|).
std::string EnemyWith(const Content& content, int hp,
                      std::optional<int> speed = std::nullopt) {
  std::string text = content.Read(kEnemyRef);
  const std::string from = R"("Stats": {"HP": 100, "Speed": 500})";
  const std::size_t at = text.find(from);
  REQUIRE(at != std::string::npos);
  text.replace(at, from.size(),
               "\"Stats\": {\"HP\": " + std::to_string(hp) +
                   ", \"Speed\": " + std::to_string(speed.value_or(500)) + "}");
  return text;
}

struct Hashes {
  std::uint64_t world = 0;
  std::uint64_t content = 0;
  friend bool operator==(const Hashes&, const Hashes&) = default;
};

Hashes HashesOf(const EditDocument& doc) {
  return Hashes{doc.Hash(), doc.ContentHash()};
}

// Applies |command|, then undo and redo: the identity on both hashes.
void CheckUndoRedo(EditDocument& doc, std::unique_ptr<EditorCommand> command)
    SE_REQUIRES(g_sim_owner) {
  REQUIRE(command != nullptr);
  const std::string label(command->Label());
  INFO(label);
  const Hashes before = HashesOf(doc);
  REQUIRE(doc.Apply(std::move(command)).has_value());
  const Hashes after = HashesOf(doc);
  REQUIRE(after.content != before.content);
  REQUIRE(doc.Undo().value_or(false));
  REQUIRE(HashesOf(doc) == before);
  REQUIRE(doc.Redo().value_or(false));
  REQUIRE(HashesOf(doc) == after);
}

TEST_CASE(
    "Undo then redo is the identity on the document hash and content "
    "hash",
    "[prefab][undo]") {
  const testing::MainThreadScope main_thread;
  const ModuleInstance module(rampage::RampageModule());
  const EditorGame game = testing::RampageEditorGame();
  const Content content("undo_redo");
  std::unique_ptr<EditDocument> doc =
      OpenScene(module.Registry(), game, content);
  const InlineOwnerScope owner(OnMainThread());
  const World& world = doc->Get();

  SECTION("every command kind") {
    CheckUndoRedo(*doc,
                  std::make_unique<SetComponentCommand>(
                      kPlayer, "Transform", JsonOf(world, kPlayer, "Transform"),
                      R"({"pos":{"x":333,"y":250}})"));
    CheckUndoRedo(*doc, std::make_unique<AddComponentCommand>(
                            kPlayer, "Camera2D", R"({"primary":true})"));
    // (Each command stays applied: the later ones take other entities.)
    auto removal = MakeRemoval(world, kEnemy3, "Path", game);
    REQUIRE(removal.has_value());
    CheckUndoRedo(*doc, std::move(*removal));
    auto copy = SerializeEntity(world, kEnemy2);
    REQUIRE(copy.has_value());
    CheckUndoRedo(*doc, std::make_unique<CreateEntityCommand>(
                            Guid{0xA, 0xB}, std::move(*copy)));
    // A create without a Spawned record: redo gives it the same one.
    CheckUndoRedo(*doc, std::make_unique<CreateEntityCommand>(
                            Guid{0xA, 0xC}, std::vector<ComponentJson>{
                                                {"Name", R"({"value":"made"})"},
                                                {"Transform", "{}"}}));
    CheckUndoRedo(*doc, std::make_unique<DestroyEntityCommand>(kEnemy4));
    auto insert = PathPointCommand::Insert(world, kEnemy1, 0, {11, 22});
    REQUIRE(insert.has_value());
    CheckUndoRedo(*doc, std::move(*insert));
    auto move = PathPointCommand::Move(world, kEnemy1, 1, {33, 44});
    REQUIRE(move.has_value());
    CheckUndoRedo(*doc, std::move(*move));
    auto erase = PathPointCommand::Delete(world, kEnemy1, 1);
    REQUIRE(erase.has_value());
    CheckUndoRedo(*doc, std::move(*erase));
    // A StatNameCommand group: the name is in the content hash only.
    auto stat = MakeStatNameEdit(world, kEnemy1, "Armor", 3.0F);
    REQUIRE(stat.has_value());
    CheckUndoRedo(*doc, std::move(*stat));
    REQUIRE(HasStatName(world, "Armor"));
    // A name no stat uses is in neither hash (the file writes names where
    // stats use them); undo removes it, redo puts it back.
    {
      const Hashes before = HashesOf(*doc);
      REQUIRE(
          doc->Apply(std::make_unique<StatNameCommand>("Mana")).has_value());
      REQUIRE(HashesOf(*doc) == before);
      REQUIRE(HasStatName(world, "Mana"));
      REQUIRE(doc->Undo().value_or(false));
      REQUIRE(HashesOf(*doc) == before);
      REQUIRE_FALSE(HasStatName(world, "Mana"));
      REQUIRE(doc->Redo().value_or(false));
      REQUIRE(HasStatName(world, "Mana"));
    }
    // Revert to prefab: enemy 1's Path back to the prefab's.
    auto baseline =
        game.prefab_baseline(world, kEnemy1, content.assets, nullptr);
    REQUIRE(baseline.has_value());
    REQUIRE(DiffersFromBaseline(world, kEnemy1, "Path", *baseline));
    auto revert = MakeRevertToPrefab(world, kEnemy1, "Path", *baseline);
    REQUIRE(revert.has_value());
    CheckUndoRedo(*doc, std::move(*revert));
    REQUIRE_FALSE(DiffersFromBaseline(world, kEnemy1, "Path", *baseline));
    // A prefab rebase (HP 100 to 250): one undo step.
    const Hashes before = HashesOf(*doc);
    const std::string hp250 = EnemyWith(content, 250);
    auto rebased = RebaseDocument(*doc, game, kEnemyRef, hp250);
    REQUIRE(rebased.has_value());
    REQUIRE(*rebased);
    REQUIRE(doc->UndoLabel() == "Prefab changed: prefabs/enemy.prefab.json");
    const Hashes after = HashesOf(*doc);
    REQUIRE(after.world != before.world);
    REQUIRE(StatOf(world, kEnemy5, "HP") == 250.0F);
    REQUIRE(doc->Undo().value_or(false));
    REQUIRE(HashesOf(*doc) == before);
    REQUIRE(doc->Redo().value_or(false));
    REQUIRE(HashesOf(*doc) == after);
  }

  SECTION("a mixed sequence of 50: undo x 50, then redo x 50") {
    const Hashes start = HashesOf(*doc);
    for (int i = 0; i < 50; ++i) {
      std::unique_ptr<EditorCommand> command;
      switch (i % 7) {
        case 0:
          command = std::make_unique<SetComponentCommand>(
              kPlayer, "Transform", JsonOf(world, kPlayer, "Transform"),
              "{\"pos\":{\"x\":" + std::to_string(100 + i) + ",\"y\":250}}");
          break;
        case 1: {
          auto made = PathPointCommand::Insert(world, kEnemy1, 0,
                                               {static_cast<float>(i), 7.0F});
          REQUIRE(made.has_value());
          command = std::move(*made);
          break;
        }
        case 2: {
          auto made = MakeStatNameEdit(world, kEnemy2,
                                       "Stat" + std::to_string(i), 1.0F);
          REQUIRE(made.has_value());
          command = std::move(*made);
          break;
        }
        case 3:
          command = std::make_unique<CreateEntityCommand>(
              Guid{0xC0, static_cast<std::uint64_t>(i + 1)},
              std::vector<ComponentJson>{{"Name", R"({"value":"x"})"},
                                         {"Transform", "{}"}});
          break;
        case 4: {
          auto made =
              HoldsComponent(world, kPlayer, "Camera2D")
                  ? MakeRemoval(world, kPlayer, "Camera2D", game)
                  : std::expected<std::unique_ptr<EditorCommand>, Error>(
                        std::make_unique<AddComponentCommand>(
                            kPlayer, "Camera2D", "{}"));
          REQUIRE(made.has_value());
          command = std::move(*made);
          break;
        }
        case 5: {
          auto made = PathPointCommand::Move(world, kEnemy1, 0,
                                             {static_cast<float>(i), 9.0F});
          REQUIRE(made.has_value());
          command = std::move(*made);
          break;
        }
        default: {
          auto made =
              HoldsComponent(world, kEnemy2, "Path")
                  ? MakeRemoval(world, kEnemy2, "Path", game)
                  : std::expected<std::unique_ptr<EditorCommand>, Error>(
                        std::make_unique<AddComponentCommand>(
                            kEnemy2, "Path", R"({"points":[{"x":1,"y":2}]})"));
          REQUIRE(made.has_value());
          command = std::move(*made);
          break;
        }
      }
      REQUIRE(doc->Apply(std::move(command)).has_value());
    }
    // And a prefab change on top.
    REQUIRE(RebaseDocument(*doc, game, kEnemyRef, EnemyWith(content, 175))
                .value_or(false));
    const Hashes end = HashesOf(*doc);
    REQUIRE(doc->UndoCount() == 51);
    for (int i = 0; i < 51; ++i) REQUIRE(doc->Undo().value_or(false));
    REQUIRE(HashesOf(*doc) == start);
    for (int i = 0; i < 51; ++i) REQUIRE(doc->Redo().value_or(false));
    REQUIRE(HashesOf(*doc) == end);
  }
}

TEST_CASE("Removing a component removes what derives from it",
          "[prefab][removal]") {
  const testing::MainThreadScope main_thread;
  const ModuleInstance module(rampage::RampageModule());
  const EditorGame game = testing::RampageEditorGame();
  const Content content("removal");
  std::unique_ptr<EditDocument> doc =
      OpenScene(module.Registry(), game, content);
  const InlineOwnerScope owner(OnMainThread());
  const World& world = doc->Get();
  REQUIRE(HoldsComponent(world, kEnemy1, "PathCache"));
  REQUIRE(HoldsComponent(world, kEnemy1, "ActiveEffects"));
  auto path = MakeRemoval(world, kEnemy1, "Path", game);
  REQUIRE(path.has_value());
  REQUIRE((*path)->Kind() == CommandKind::kGroup);
  REQUIRE(static_cast<const CommandGroup&>(**path).size() == 2);
  REQUIRE(doc->Apply(std::move(*path)).has_value());
  REQUIRE_FALSE(HoldsComponent(world, kEnemy1, "Path"));
  REQUIRE_FALSE(HoldsComponent(world, kEnemy1, "PathCache"));
  auto stats = MakeRemoval(world, kEnemy1, "Stats", game);
  REQUIRE(stats.has_value());
  REQUIRE(doc->Apply(std::move(*stats)).has_value());
  REQUIRE_FALSE(HoldsComponent(world, kEnemy1, "ActiveEffects"));
  // One undo step each restores all of them.
  REQUIRE(doc->Undo().value_or(false));
  REQUIRE(HoldsComponent(world, kEnemy1, "Stats"));
  REQUIRE(HoldsComponent(world, kEnemy1, "ActiveEffects"));
  REQUIRE(doc->Undo().value_or(false));
  REQUIRE(HoldsComponent(world, kEnemy1, "Path"));
  REQUIRE(HoldsComponent(world, kEnemy1, "PathCache"));
  // What the game keeps cannot be removed.
  for (std::string_view kept :
       {"IdComponent", "Name", "Spawned", "PrefabInstance", "Transform"}) {
    INFO(kept);
    REQUIRE_FALSE(RemovableBy(game, kept));
    REQUIRE_FALSE(MakeRemoval(world, kEnemy1, kept, game).has_value());
  }
  // On a prefab instance the saved file holds the removal as null.
  auto again = MakeRemoval(world, kEnemy1, "Collider", game);
  REQUIRE(again.has_value());
  REQUIRE(doc->Apply(std::move(*again)).has_value());
  auto file = doc->SerializeFile();
  REQUIRE(file.has_value());
  REQUIRE(Compact(*file).find("\"Collider\":null") != std::string::npos);
}

TEST_CASE("A stat typed in the Stats drawer is saved by name",
          "[prefab][stats]") {
  const testing::MainThreadScope main_thread;
  const ModuleInstance module(rampage::RampageModule());
  const EditorGame game = testing::RampageEditorGame();
  const Content content("stat_names");
  std::unique_ptr<EditDocument> doc =
      OpenScene(module.Registry(), game, content);
  std::string saved;
  {
    const InlineOwnerScope owner(OnMainThread());
    auto edit = MakeStatNameEdit(doc->Get(), kEnemy1, "Armor", 7.0F);
    REQUIRE(edit.has_value());
    REQUIRE(doc->Apply(std::move(*edit)).has_value());
    auto file = doc->SerializeFile();
    REQUIRE(file.has_value());
    saved = std::move(*file);
  }
  // The file has the named form.
  REQUIRE(saved.find("\"Armor\"") != std::string::npos);
  // Reopened, the name is in StatNames and the value by it.
  std::unique_ptr<EditDocument> reopened =
      OpenScene(module.Registry(), game, content, saved);
  REQUIRE(HasStatName(reopened->Get(), "Armor"));
  REQUIRE(StatOf(reopened->Get(), kEnemy1, "Armor") == 7.0F);
  REQUIRE(reopened->Hash() == doc->Hash());
  REQUIRE(reopened->ContentHash() == doc->ContentHash());
}

TEST_CASE("A saved prefab updates its instances in an open scene",
          "[prefab][rebase]") {
  const testing::MainThreadScope main_thread;
  const ModuleInstance module(rampage::RampageModule());
  const EditorGame game = testing::RampageEditorGame();
  const Content content("saved_prefab");
  std::unique_ptr<EditDocument> scene =
      OpenScene(module.Registry(), game, content);
  std::unique_ptr<EditDocument> prefab =
      OpenPrefab(module.Registry(), game, content);
  REQUIRE(prefab->Get().EntityCount() == 1);
  REQUIRE(scene->PrefabTexts().contains(kEnemyRef));
  const std::string path1 = JsonOf(scene->Get(), kEnemy1, "Path");
  const InlineOwnerScope owner(OnMainThread());
  // The prefab document's HP becomes 250 and it is saved.
  const Guid root = rampage::PrefabRootGuid(kEnemyRef);
  auto edit = MakeStatNameEdit(prefab->Get(), root, "HP", 250.0F);
  REQUIRE(edit.has_value());
  REQUIRE(prefab->Apply(std::move(*edit)).has_value());
  auto text = prefab->SerializeFile();
  REQUIRE(text.has_value());
  content.Write(kEnemyRef, *text);
  prefab->MarkSaved(prefab->Revision());
  prefab->SetFileText(*text);
  // The save's completion rebases the open scene at once.
  REQUIRE_FALSE(scene->Dirty());
  auto rebased = RebaseDocument(*scene, game, kEnemyRef, *text);
  REQUIRE(rebased.has_value());
  REQUIRE(*rebased);
  REQUIRE(StatOf(scene->Get(), kEnemy1, "HP") == 250.0F);
  REQUIRE(StatOf(scene->Get(), kEnemy2, "HP") == 250.0F);
  // The instance's override survives.
  REQUIRE(JsonOf(scene->Get(), kEnemy1, "Path") == path1);
  REQUIRE(scene->UndoCount() == 1);
  // Decision 2: its file would not change (the instances load as it
  // says), so it stays clean.
  REQUIRE_FALSE(scene->Dirty());
  // The same content again (the scan's later report) changes nothing.
  REQUIRE_FALSE(RebaseDocument(*scene, game, kEnemyRef, *text).value_or(true));
  REQUIRE(scene->UndoCount() == 1);
  // After an undo the instances differ from the file's prefab: dirty.
  REQUIRE(scene->Undo().value_or(false));
  REQUIRE(StatOf(scene->Get(), kEnemy1, "HP") == 100.0F);
  REQUIRE(scene->Dirty());
}

// Review fix F1: a stat the prefab gains (a name the scene's World did not
// know) comes with the rebase's components, and the scene saves without an
// override of it; the saved scene follows a later change of the prefab.
TEST_CASE("A stat a prefab gains is not written as an override",
          "[prefab][rebase]") {
  const testing::MainThreadScope main_thread;
  const ModuleInstance module(rampage::RampageModule());
  const EditorGame game = testing::RampageEditorGame();
  const Content content("prefab_gains_stat");
  std::unique_ptr<EditDocument> scene =
      OpenScene(module.Registry(), game, content);
  REQUIRE_FALSE(HasStatName(scene->Get(), "Armor"));
  std::string armored = content.Read(kEnemyRef);
  const std::string from = R"("Stats": {"HP": 100, "Speed": 500})";
  const std::size_t at = armored.find(from);
  REQUIRE(at != std::string::npos);
  armored.replace(at, from.size(),
                  R"("Stats": {"HP": 100, "Speed": 500, "Armor": 5})");
  content.Write(kEnemyRef, armored);
  {
    const InlineOwnerScope owner(OnMainThread());
    auto rebased = RebaseDocument(*scene, game, kEnemyRef, armored);
    REQUIRE(rebased.has_value());
    REQUIRE(*rebased);
  }
  REQUIRE(StatOf(scene->Get(), kEnemy1, "Armor") == 5.0F);
  REQUIRE(HasStatName(scene->Get(), "Armor"));
  REQUIRE(scene->UndoCount() == 1);
  auto saved = scene->SerializeFile();
  REQUIRE(saved.has_value());
  INFO(*saved);
  // Every enemy equals its prefab: no Stats override, no registry form.
  REQUIRE(Compact(*saved).find("\"Stats\":") == std::string::npos);
  REQUIRE(saved->find("\"values\"") == std::string::npos);
  // Undo takes the name back with the components.
  {
    const InlineOwnerScope owner(OnMainThread());
    REQUIRE(scene->Undo().value_or(false));
    REQUIRE_FALSE(HasStatName(scene->Get(), "Armor"));
    REQUIRE(scene->Redo().value_or(false));
    REQUIRE(HasStatName(scene->Get(), "Armor"));
  }
  // The saved scene follows the prefab's next change.
  content.Write("scenes/sandbox.scene.json", *saved);
  std::string hp999 = armored;
  hp999.replace(hp999.find(R"("HP": 100)"), 9, R"("HP": 999)");
  content.Write(kEnemyRef, hp999);
  std::unique_ptr<EditDocument> reopened =
      OpenScene(module.Registry(), game, content);
  REQUIRE(StatOf(reopened->Get(), kEnemy1, "HP") == 999.0F);
  REQUIRE(StatOf(reopened->Get(), kEnemy2, "Armor") == 5.0F);
}

TEST_CASE("A prefab changed on disk is one undo step per document",
          "[prefab][rebase]") {
  const testing::MainThreadScope main_thread;
  const ModuleInstance module(rampage::RampageModule());
  const EditorGame game = testing::RampageEditorGame();
  const Content content("changed_prefab");
  DocumentSet documents(kAutosaveQuietNs);
  const DocumentId a =
      documents.Add(OpenScene(module.Registry(), game, content));
  const DocumentId b =
      documents.Add(OpenScene(module.Registry(), game, content));
  // b has an override of enemy 2's Speed.
  {
    const InlineOwnerScope owner(OnMainThread());
    EditDocument& doc = *documents.Find(b)->document;
    auto edit = MakeStatNameEdit(doc.Get(), kEnemy2, "Speed", 42.0F);
    REQUIRE(edit.has_value());
    REQUIRE(doc.Apply(std::move(*edit)).has_value());
  }
  // Another program writes the prefab: HP and Speed change.
  const std::string text = EnemyWith(content, 300, 900);
  content.Write(kEnemyRef, text);
  const InlineOwnerScope owner(OnMainThread());
  for (const auto& entry : documents.All()) {
    auto rebased = RebaseDocument(*entry->document, game, kEnemyRef, text);
    REQUIRE(rebased.has_value());
    REQUIRE(*rebased);
  }
  const EditDocument& da = *documents.Find(a)->document;
  const EditDocument& db = *documents.Find(b)->document;
  REQUIRE(da.UndoCount() == 1);
  REQUIRE(db.UndoCount() == 2);
  REQUIRE(da.UndoLabel() == "Prefab changed: prefabs/enemy.prefab.json");
  REQUIRE(db.UndoLabel() == "Prefab changed: prefabs/enemy.prefab.json");
  REQUIRE(StatOf(da.Get(), kEnemy2, "HP") == 300.0F);
  REQUIRE(StatOf(da.Get(), kEnemy2, "Speed") == 900.0F);
  REQUIRE(StatOf(db.Get(), kEnemy2, "HP") == 300.0F);
  // b's override is kept across the change.
  REQUIRE(StatOf(db.Get(), kEnemy2, "Speed") == 42.0F);
  REQUIRE(da.PrefabTexts().at(std::string(kEnemyRef)) == text);
}

TEST_CASE("A prefab document saves its file and edits its root",
          "[prefab][documents]") {
  const testing::MainThreadScope main_thread;
  const ModuleInstance module(rampage::RampageModule());
  const EditorGame game = testing::RampageEditorGame();
  const Content content("prefab_document");
  std::unique_ptr<EditDocument> prefab =
      OpenPrefab(module.Registry(), game, content);
  const Guid root = rampage::PrefabRootGuid(kEnemyRef);
  REQUIRE(prefab->Get().Find(root).has_value());
  auto file = prefab->SerializeFile();
  REQUIRE(file.has_value());
  REQUIRE(file->find("se-prefab-v1") != std::string::npos);
  // Reopened from its own file: the same document.
  DocumentDesc desc = prefab->Desc();
  {
    const InlineOwnerScope owner(OnMainThread());
    auto again = EditDocument::Open(module.Registry(), game, desc, *file);
    REQUIRE(again.has_value());
    REQUIRE((*again)->Hash() == prefab->Hash());
    REQUIRE((*again)->ContentHash() == prefab->ContentHash());
    CheckUndoRedo(*prefab,
                  std::make_unique<SetComponentCommand>(
                      root, "Collider", JsonOf(prefab->Get(), root, "Collider"),
                      R"({"half_extents":{"x":30,"y":30}})"));
  }
}

TEST_CASE("Play-start bytes stay flattened", "[prefab][play]") {
  const testing::MainThreadScope main_thread;
  const ModuleInstance module(rampage::RampageModule());
  const EditorGame game = testing::RampageEditorGame();
  const Content content("play_start");
  std::unique_ptr<EditDocument> doc =
      OpenScene(module.Registry(), game, content);
  auto flat = doc->Serialize();
  auto file = doc->SerializeFile();
  REQUIRE(flat.has_value());
  REQUIRE(file.has_value());
  REQUIRE(flat->find("\"prefab\"") == std::string::npos);
  REQUIRE(file->find("\"prefab\"") != std::string::npos);
  // The flattened text loads without any prefab file.
  World world(module.Registry(),
              WorldDesc{.name = "play", .kind = WorldKind::kPlay});
  {
    const InlineOwnerScope owner(OnMainThread());
    const MutationGate gate = world.OpenGate(GateReason::kLoad);
    rampage::SceneLoadOptions options;
    options.resolver =
        [](std::string_view ref) -> std::expected<std::string, Error> {
      return Fail(ErrorCode::kNotFound,
                  "no prefab may be read: " + std::string(ref));
    };
    REQUIRE(rampage::LoadScene(world, *flat, options).has_value());
  }
  REQUIRE(world.Hash().Folded() == doc->Hash());
}

TEST_CASE("Recovery files with prefab references round-trip",
          "[prefab][recovery]") {
  const testing::MainThreadScope main_thread;
  const ModuleInstance module(rampage::RampageModule());
  const EditorGame game = testing::RampageEditorGame();
  const Content content("recovery_prefabs");
  std::unique_ptr<EditDocument> doc =
      OpenScene(module.Registry(), game, content);
  {
    const InlineOwnerScope owner(OnMainThread());
    // A Collider, not the Transform: an Edit-mode move leaves the loaded
    // PrevTransform (hashed) behind, which a reload sets to the new pose.
    REQUIRE(doc->Apply(std::make_unique<SetComponentCommand>(
                           kPlayer, "Collider",
                           JsonOf(doc->Get(), kPlayer, "Collider"),
                           R"({"half_extents":{"x":30,"y":30}})"))
                .has_value());
  }
  auto text = doc->SerializeFile();
  REQUIRE(text.has_value());
  RecoveryFile file;
  file.document = doc->Desc().path;
  file.name = doc->Desc().name;
  file.revision = doc->Revision();
  file.scene = *text;
  auto decoded = DecodeRecovery(EncodeRecovery(file), "r.serecovery");
  REQUIRE(decoded.has_value());
  REQUIRE(decoded->kind == DocumentKind::kScene);
  REQUIRE(decoded->scene.find("\"prefab\"") != std::string::npos);
  std::unique_ptr<EditDocument> recovered =
      OpenScene(module.Registry(), game, content, decoded->scene);
  REQUIRE(recovered->Hash() == doc->Hash());
  REQUIRE(recovered->ContentHash() == doc->ContentHash());
}

// Review fix F3: a prefab file that cannot be read does not cost the unsaved
// edits. The recovery text takes the overrides against the prefab texts the
// document recorded, and loads back to the same document.
TEST_CASE("A recovery file survives a broken prefab file",
          "[prefab][recovery]") {
  const testing::MainThreadScope main_thread;
  const ModuleInstance module(rampage::RampageModule());
  const EditorGame game = testing::RampageEditorGame();
  const Content content("recovery_broken_prefab");
  std::unique_ptr<EditDocument> doc =
      OpenScene(module.Registry(), game, content);
  {
    const InlineOwnerScope owner(OnMainThread());
    REQUIRE(doc->Apply(std::make_unique<SetComponentCommand>(
                           kEnemy1, "Collider",
                           JsonOf(doc->Get(), kEnemy1, "Collider"),
                           R"({"half_extents":{"x":30,"y":30}})"))
                .has_value());
  }
  const std::string enemy = content.Read(kEnemyRef);
  content.Write(kEnemyRef, "oops");
  REQUIRE_FALSE(doc->SerializeFile().has_value());
  auto text = doc->SerializeRecovery();
  REQUIRE(text.has_value());
  REQUIRE(text->find("\"prefab\"") != std::string::npos);
  // Loaded once the prefab is back: the same document.
  content.Write(kEnemyRef, enemy);
  std::unique_ptr<EditDocument> recovered =
      OpenScene(module.Registry(), game, content, *text);
  REQUIRE(recovered->Hash() == doc->Hash());
  REQUIRE(recovered->ContentHash() == doc->ContentHash());
}

// Review fix F8: the panels' prefab baselines come from the document's
// recorded prefab texts, one scratch load per prefab and text, however many
// instances are asked about and however often the document changes.
TEST_CASE("Prefab baselines are kept per prefab, not per row",
          "[prefab][panels]") {
  const testing::MainThreadScope main_thread;
  const ModuleInstance module(rampage::RampageModule());
  const EditorGame game = testing::RampageEditorGame();
  const Content content("baselines_per_prefab");
  std::unique_ptr<EditDocument> doc =
      OpenScene(module.Registry(), game, content);
  PanelContext context;
  context.world = &doc->Get();
  context.game = &game;
  context.asset_root = doc->Desc().asset_root;
  context.document = 1;
  context.prefab_texts = &doc->PrefabTexts();
  PrefabBaselines baselines;
  for (std::uint64_t revision = 1; revision <= 5; ++revision) {
    context.revision = revision;
    for (const Guid guid : {kEnemy1, kEnemy2, kEnemy3, kEnemy4, kEnemy5}) {
      REQUIRE(baselines.Get(context, guid) != nullptr);
    }
    REQUIRE(baselines.Get(context, kPlayer) != nullptr);
  }
  REQUIRE(baselines.Loads() == 2);
  // No prefab file is read: the recorded texts serve them.
  const std::string hp250 = EnemyWith(content, 250);
  content.Write(kEnemyRef, "oops");
  PrefabBaselines fresh;
  REQUIRE(fresh.Get(context, kEnemy1) != nullptr);
  REQUIRE(DiffersFromBaseline(doc->Get(), kEnemy1, "Path",
                              *fresh.Get(context, kEnemy1)));
  REQUIRE_FALSE(DiffersFromBaseline(doc->Get(), kEnemy1, "Stats",
                                    *fresh.Get(context, kEnemy1)));
  // A rebase records a new text: the baseline follows it.
  {
    const InlineOwnerScope owner(OnMainThread());
    REQUIRE(RebaseDocument(*doc, game, kEnemyRef, hp250).value_or(false));
  }
  const EntityJson* rebased = fresh.Get(context, kEnemy1);
  REQUIRE(rebased != nullptr);
  REQUIRE(fresh.Loads() == 2);
  REQUIRE_FALSE(DiffersFromBaseline(doc->Get(), kEnemy1, "Stats", *rebased));
}

TEST_CASE("A stress press saves as prefab instances", "[prefab][stress]") {
  const testing::MainThreadScope main_thread;
  const ModuleInstance module(rampage::RampageModule());
  const EditorGame game = testing::RampageEditorGame();
  const Content content("stress_prefabs");
  std::unique_ptr<EditDocument> doc =
      OpenScene(module.Registry(), game, content);
  const std::vector<StressSpawn> spawns =
      PlanStress(DefaultStressSpec(), doc->Get().Desc().seed, 0, 20);
  {
    const InlineOwnerScope owner(OnMainThread());
    auto group = std::make_unique<CommandGroup>("Generate 20 enemies");
    for (const StressSpawn& spawn : spawns) {
      std::vector<ComponentJson> components;
      components.reserve(spawn.components.size());
      for (const SpawnComponent& c : spawn.components) {
        components.push_back({c.component, c.json});
      }
      group->Add(std::make_unique<CreateEntityCommand>(spawn.guid,
                                                       std::move(components)));
    }
    REQUIRE(doc->Apply(std::move(group)).has_value());
  }
  REQUIRE(PrefabOf(doc->Get(), spawns[0].guid) == std::string(kEnemyRef));
  auto saved = doc->SerializeFile();
  REQUIRE(saved.has_value());
  const std::string file = Compact(*saved);
  // Each spawn is written as the prefab and its overrides.
  const std::string guid = ToString(spawns[3].guid);
  const std::size_t at = file.find(guid);
  REQUIRE(at != std::string::npos);
  const std::size_t next = file.find("\"guid\"", at);
  const std::string entity = file.substr(at, next - at);
  REQUIRE(entity.find("\"prefab\":\"prefabs/enemy.prefab.json\"") !=
          std::string::npos);
  REQUIRE(entity.find("\"components\"") == std::string::npos);
  // It loads back to the same World.
  std::unique_ptr<EditDocument> reopened =
      OpenScene(module.Registry(), game, content, *saved);
  // It loads back to the same content. (Not the same World hash: a spawn
  // made by a command has no PrevTransform until its first tick, which a
  // load's FinalizeSpawns gives it; the flattened text has neither.)
  REQUIRE(reopened->ContentHash() == doc->ContentHash());
  REQUIRE(reopened->Get().EntityCount() == doc->Get().EntityCount());
}

}  // namespace
}  // namespace se::editor
