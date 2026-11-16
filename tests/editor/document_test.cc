// Copyright 2026 The simple-engine-2 Authors.
//
// EditDocument (M10a): commands apply under the document's gate (closed
// otherwise), undo and redo walk the stack (undo then redo is the
// identity on the document hash), a drag coalesces into one step, the
// dirty flag follows the revision, and Serialize round-trips.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "editor/support/document_world.h"
#include "editor/support/rampage_editor_game.h"
#include "game_rampage/module.h"
#include "se_core/thread_role.h"
#include "se_editor/commands.h"
#include "se_editor/edit_document.h"
#include "se_sim/game_module.h"
#include "support/test_roles.h"

namespace se::editor {
namespace {

using testing::kEnemy1;
using testing::kEnemy2;
using testing::kPlayer;

std::string ComponentOf(const World& world, Guid guid, std::string_view name) {
  auto bundle = SerializeEntity(world, guid);
  REQUIRE(bundle.has_value());
  for (const ComponentJson& component : *bundle) {
    if (component.component == name) return component.json;
  }
  return {};
}

std::string TransformOf(const EditDocument& doc, Guid guid) {
  return ComponentOf(doc.Get(), guid, "Transform");
}

TEST_CASE("A document changes only through its commands", "[document]") {
  const testing::MainThreadScope main_thread;
  const ModuleInstance module(rampage::RampageModule());
  const EditorGame game = testing::RampageEditorGame();
  std::unique_ptr<EditDocument> doc =
      testing::OpenSandbox(module.Registry(), game);
  REQUIRE(doc->Get().Desc().kind == WorldKind::kEditDocument);
  REQUIRE_FALSE(doc->Get().GateOpen());
  REQUIRE_FALSE(doc->Dirty());
  const std::uint64_t opened = doc->Hash();
  {
    const InlineOwnerScope owner(OnMainThread());
    REQUIRE(doc->Apply(std::make_unique<SetComponentCommand>(
                           kPlayer, "Transform", TransformOf(*doc, kPlayer),
                           TransformOf(*doc, kEnemy1)))
                .has_value());
  }
  // The gate was open only while the command applied.
  REQUIRE_FALSE(doc->Get().GateOpen());
  REQUIRE(doc->Dirty());
  REQUIRE(doc->Hash() != opened);
  REQUIRE(doc->UndoCount() == 1);
  REQUIRE(doc->UndoLabel() == "Set Transform");
}

TEST_CASE("Undo then redo is the identity on the document hash", "[document]") {
  const testing::MainThreadScope main_thread;
  const ModuleInstance module(rampage::RampageModule());
  const EditorGame game = testing::RampageEditorGame();
  std::unique_ptr<EditDocument> doc =
      testing::OpenSandbox(module.Registry(), game);
  const InlineOwnerScope owner(OnMainThread());
  const std::uint64_t h0 = doc->Hash();
  REQUIRE(
      doc->Apply(std::make_unique<DestroyEntityCommand>(kEnemy1)).has_value());
  const std::uint64_t h1 = doc->Hash();
  auto insert = PathPointCommand::Insert(doc->Get(), kEnemy2, 0, {5.0F, 6.0F});
  REQUIRE(insert.has_value());
  REQUIRE(doc->Apply(std::move(*insert)).has_value());
  const std::uint64_t h2 = doc->Hash();
  REQUIRE(*doc->Undo());
  REQUIRE(doc->Hash() == h1);
  REQUIRE(*doc->Undo());
  REQUIRE(doc->Hash() == h0);
  REQUIRE_FALSE(*doc->Undo());
  REQUIRE(*doc->Redo());
  REQUIRE(doc->Hash() == h1);
  REQUIRE(*doc->Redo());
  REQUIRE(doc->Hash() == h2);
  REQUIRE_FALSE(*doc->Redo());
  // A new command clears the redo side.
  REQUIRE(*doc->Undo());
  REQUIRE(
      doc->Apply(std::make_unique<DestroyEntityCommand>(kPlayer)).has_value());
  REQUIRE(doc->RedoCount() == 0);
}

TEST_CASE("A drag is one undo step", "[document]") {
  const testing::MainThreadScope main_thread;
  const ModuleInstance module(rampage::RampageModule());
  const EditorGame game = testing::RampageEditorGame();
  std::unique_ptr<EditDocument> doc =
      testing::OpenSandbox(module.Registry(), game);
  const InlineOwnerScope owner(OnMainThread());
  const std::uint64_t before = doc->Hash();
  doc->BeginCoalesce("Drag point");
  for (int i = 0; i < 10; ++i) {
    const auto step = static_cast<float>(i);
    auto move = PathPointCommand::Move(
        doc->Get(), kEnemy1, 2, Vec2{100.0F + step, 200.0F + 2.0F * step});
    REQUIRE(move.has_value());
    REQUIRE(doc->Apply(std::move(*move)).has_value());
  }
  doc->EndCoalesce();
  REQUIRE(doc->UndoCount() == 1);
  REQUIRE(doc->UndoLabel() == "Move path point");
  auto points =
      PathPointCommand::PointsOf(ComponentOf(doc->Get(), kEnemy1, "Path"));
  REQUIRE(points.has_value());
  REQUIRE((*points)[2] == Vec2{109.0F, 218.0F});
  REQUIRE(*doc->Undo());
  REQUIRE(doc->Hash() == before);
}

TEST_CASE("Dirty follows the saved revision; Serialize round-trips",
          "[document]") {
  const testing::MainThreadScope main_thread;
  const ModuleInstance module(rampage::RampageModule());
  const EditorGame game = testing::RampageEditorGame();
  std::unique_ptr<EditDocument> doc =
      testing::OpenSandbox(module.Registry(), game);
  // An unedited document loads back to the same hash (Play's round trip).
  {
    auto text = doc->Serialize();
    REQUIRE(text.has_value());
    const InlineOwnerScope owner(OnMainThread());
    auto reopened =
        EditDocument::Open(module.Registry(), game, doc->Desc(), *text);
    REQUIRE(reopened.has_value());
    REQUIRE((*reopened)->Hash() == doc->Hash());
  }
  {
    const InlineOwnerScope owner(OnMainThread());
    REQUIRE(doc->Apply(std::make_unique<DestroyEntityCommand>(kEnemy2))
                .has_value());
  }
  REQUIRE(doc->Dirty());
  doc->MarkSaved(doc->Revision());
  REQUIRE_FALSE(doc->Dirty());
  auto text = doc->Serialize();
  REQUIRE(text.has_value());
  DocumentDesc desc = doc->Desc();
  std::unique_ptr<EditDocument> again;
  {
    const InlineOwnerScope owner(OnMainThread());
    auto opened = EditDocument::Open(module.Registry(), game, desc, *text);
    REQUIRE(opened.has_value());
    again = std::move(*opened);
  }
  // Spawn serials are dense again after the load, so only the contents
  // compare.
  REQUIRE(again->Get().EntityCount() == doc->Get().EntityCount());
  REQUIRE_FALSE(again->Get().Find(kEnemy2).has_value());
}

}  // namespace
}  // namespace se::editor
