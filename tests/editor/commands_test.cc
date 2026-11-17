// Copyright 2026 The simple-engine-2 Authors.
//
// The editor's command kinds (M10a "Commands and undo"): each applies to a
// document World through the ComponentRegistry's JSON hooks, Revert after
// Apply restores the document hash and Apply after Revert gives the hash
// after the first Apply (undo then redo is the identity), path-point
// commands edit the points of rampage's Path, and coalescing keeps the
// first before and the last after.

#include "se_editor/commands.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "editor/support/document_world.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_sim/session_log.h"
#include "support/test_roles.h"

namespace se::editor {
namespace {

using testing::DocumentWorld;
using testing::kEnemy1;
using testing::kEnemy2;
using testing::kPlayer;

// Apply, Revert, Apply again: the hashes before, after, and after the
// redo.
struct RoundTrip {
  std::uint64_t before = 0;
  std::uint64_t applied = 0;
  std::uint64_t reverted = 0;
  std::uint64_t reapplied = 0;
};

RoundTrip CheckRoundTrip(DocumentWorld& doc, EditorCommand& command)
    SE_REQUIRES(g_sim_owner) {
  RoundTrip trip;
  trip.before = doc.Hash();
  REQUIRE(doc.Apply(command).has_value());
  trip.applied = doc.Hash();
  REQUIRE(doc.Revert(command).has_value());
  trip.reverted = doc.Hash();
  REQUIRE(doc.Apply(command).has_value());
  trip.reapplied = doc.Hash();
  CHECK(trip.applied != trip.before);
  CHECK(trip.reverted == trip.before);
  CHECK(trip.reapplied == trip.applied);
  return trip;
}

TEST_CASE("Set component applies, reverts and redoes", "[commands]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  DocumentWorld doc;
  const std::string enemy_transform = doc.ComponentJson(kEnemy1, "Transform");
  const std::string player_transform = doc.ComponentJson(kPlayer, "Transform");
  auto set = SetComponentCommand::Make(doc.Get(), kPlayer, "Transform",
                                       enemy_transform);
  REQUIRE(set.has_value());
  REQUIRE((*set)->Before() == player_transform);
  REQUIRE((*set)->After() == enemy_transform);
  REQUIRE((*set)->Label() == "Set Transform");
  CheckRoundTrip(doc, **set);
  REQUIRE(doc.ComponentJson(kPlayer, "Transform") == enemy_transform);
  REQUIRE(doc.Revert(**set).has_value());
  REQUIRE(doc.ComponentJson(kPlayer, "Transform") == player_transform);
}

TEST_CASE("Set component rejects unknown entities and components",
          "[commands]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  DocumentWorld doc;
  REQUIRE_FALSE(
      SetComponentCommand::Make(doc.Get(), Guid{9, 9}, "Transform", "{}"));
  REQUIRE_FALSE(
      SetComponentCommand::Make(doc.Get(), kPlayer, "NoSuchThing", "{}"));
  // A malformed value fails to apply and leaves the document alone.
  SetComponentCommand bad(kPlayer, "Transform",
                          doc.ComponentJson(kPlayer, "Transform"),
                          "{\"pos\": [oops");
  const std::uint64_t before = doc.Hash();
  REQUIRE_FALSE(doc.Apply(bad).has_value());
  REQUIRE(doc.Hash() == before);
}

TEST_CASE("Add and remove component round-trip", "[commands]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  DocumentWorld doc;
  const std::string name = doc.ComponentJson(kPlayer, "Name");
  RemoveComponentCommand remove(kPlayer, "Name");
  CheckRoundTrip(doc, remove);
  REQUIRE(remove.Before() == name);
  REQUIRE_FALSE(doc.Has(kPlayer, "Name"));

  AddComponentCommand add(kPlayer, "Name", name);
  CheckRoundTrip(doc, add);
  REQUIRE(doc.ComponentJson(kPlayer, "Name") == name);
  // Adding what the entity has is an error.
  REQUIRE_FALSE(doc.Apply(add).has_value());
}

TEST_CASE("Destroy and create restore the entity exactly", "[commands]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  DocumentWorld doc;
  const std::size_t entities = doc.Get().EntityCount();
  DestroyEntityCommand destroy(kEnemy1);
  const RoundTrip trip = CheckRoundTrip(doc, destroy);
  REQUIRE(doc.Get().EntityCount() == entities - 1);
  REQUIRE_FALSE(doc.Get().Find(kEnemy1).has_value());
  REQUIRE_FALSE(destroy.Components().empty());
  // Spawned is part of the bundle, so the undone destroy hashes the same.
  bool has_spawned = false;
  for (const ComponentJson& component : destroy.Components()) {
    REQUIRE(component.component != "IdComponent");
    has_spawned = has_spawned || component.component == "Spawned";
  }
  REQUIRE(has_spawned);
  REQUIRE(doc.Revert(destroy).has_value());
  REQUIRE(doc.Hash() == trip.before);

  auto bundle = SerializeEntity(doc.Get(), kEnemy2);
  REQUIRE(bundle.has_value());
  const Guid fresh{0xED17, 1};
  CreateEntityCommand create(fresh, *bundle);
  CheckRoundTrip(doc, create);
  REQUIRE(doc.Get().Find(fresh).has_value());
  REQUIRE(doc.Get().EntityCount() == entities + 1);
  // A Guid in use cannot be created again.
  CreateEntityCommand twice(fresh, *bundle);
  REQUIRE_FALSE(doc.Apply(twice).has_value());
}

TEST_CASE("Path-point commands insert, move and delete points", "[commands]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  DocumentWorld doc;
  auto points = PathPointCommand::PointsOf(doc.ComponentJson(kEnemy1, "Path"));
  REQUIRE(points.has_value());
  REQUIRE(points->size() == 5);
  REQUIRE((*points)[0] == Vec2{1499.0F, 345.0F});

  auto insert =
      PathPointCommand::Insert(doc.Get(), kEnemy1, 1, Vec2{10.5F, 20.25F});
  REQUIRE(insert.has_value());
  REQUIRE((*insert)->Kind() == CommandKind::kInsertPathPoint);
  CheckRoundTrip(doc, **insert);
  auto after = PathPointCommand::PointsOf(doc.ComponentJson(kEnemy1, "Path"));
  REQUIRE(after.has_value());
  REQUIRE(after->size() == 6);
  REQUIRE((*after)[1] == Vec2{10.5F, 20.25F});
  REQUIRE((*after)[2] == (*points)[1]);
  REQUIRE(doc.Revert(**insert).has_value());

  auto move = PathPointCommand::Move(doc.Get(), kEnemy1, 4, Vec2{1.0F, 2.0F});
  REQUIRE(move.has_value());
  CheckRoundTrip(doc, **move);
  after = PathPointCommand::PointsOf(doc.ComponentJson(kEnemy1, "Path"));
  REQUIRE((*after)[4] == Vec2{1.0F, 2.0F});
  REQUIRE(after->size() == 5);
  REQUIRE(doc.Revert(**move).has_value());

  auto erase = PathPointCommand::Delete(doc.Get(), kEnemy1, 0);
  REQUIRE(erase.has_value());
  REQUIRE((*erase)->Point() == (*points)[0]);
  CheckRoundTrip(doc, **erase);
  after = PathPointCommand::PointsOf(doc.ComponentJson(kEnemy1, "Path"));
  REQUIRE(after->size() == 4);
  REQUIRE((*after)[0] == (*points)[1]);

  // Out of range: no command.
  REQUIRE_FALSE(PathPointCommand::Insert(doc.Get(), kEnemy1, 9, Vec2{}));
  REQUIRE_FALSE(PathPointCommand::Move(doc.Get(), kEnemy1, 4, Vec2{}));
  REQUIRE_FALSE(PathPointCommand::Delete(doc.Get(), kPlayer, 0));
}

TEST_CASE("A drag coalesces into one command", "[commands]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  DocumentWorld doc;
  const std::string start = doc.ComponentJson(kPlayer, "Transform");
  const std::string middle = doc.ComponentJson(kEnemy1, "Transform");
  const std::string end = doc.ComponentJson(kEnemy2, "Transform");
  CommandGroup group("Drag");
  group.Add(std::make_unique<SetComponentCommand>(kPlayer, "Transform", start,
                                                  middle));
  group.Add(
      std::make_unique<SetComponentCommand>(kPlayer, "Transform", middle, end));
  REQUIRE(group.size() == 1);
  const auto& merged = static_cast<const SetComponentCommand&>(group.At(0));
  REQUIRE(merged.Before() == start);
  REQUIRE(merged.After() == end);
  // Another component is a second command of the same step.
  group.Add(std::make_unique<SetComponentCommand>(
      kPlayer, "Name", doc.ComponentJson(kPlayer, "Name"),
      "{\"value\":\"renamed\"}"));
  REQUIRE(group.size() == 2);
  REQUIRE(group.Target() == kPlayer);
  CheckRoundTrip(doc, group);

  // Moves of one point merge; moves of two points do not.
  auto first = PathPointCommand::Move(doc.Get(), kEnemy1, 0, Vec2{1.0F, 1.0F});
  auto second = PathPointCommand::Move(doc.Get(), kEnemy1, 0, Vec2{2.0F, 2.0F});
  auto other = PathPointCommand::Move(doc.Get(), kEnemy1, 1, Vec2{3.0F, 3.0F});
  REQUIRE((first && second && other));
  REQUIRE((*first)->Absorb(**second));
  REQUIRE((*first)->Point() == Vec2{2.0F, 2.0F});
  REQUIRE_FALSE((*first)->Absorb(**other));
}

// R7 of the M10-M14 review: a panel's commands are made during its Draw and
// applied after ImGui::Render, so two commands of one frame are made from
// the same World. Each takes its start when it first applies: undoing the
// second gives the first's result, and both path inserts land.
TEST_CASE("Commands of one frame undo to their own start", "[commands]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  DocumentWorld doc;
  const std::string start = doc.ComponentJson(kPlayer, "Transform");
  const std::string middle = doc.ComponentJson(kEnemy1, "Transform");
  const std::string end = doc.ComponentJson(kEnemy2, "Transform");
  auto first =
      SetComponentCommand::Make(doc.Get(), kPlayer, "Transform", middle);
  auto second = SetComponentCommand::Make(doc.Get(), kPlayer, "Transform", end);
  REQUIRE((first && second));
  REQUIRE(doc.Apply(**first).has_value());
  REQUIRE(doc.Apply(**second).has_value());
  REQUIRE((*second)->Before() == middle);
  REQUIRE(doc.Revert(**second).has_value());
  REQUIRE(doc.ComponentJson(kPlayer, "Transform") == middle);
  REQUIRE(doc.Revert(**first).has_value());
  REQUIRE(doc.ComponentJson(kPlayer, "Transform") == start);
  // Redo keeps each command's start.
  REQUIRE(doc.Apply(**first).has_value());
  REQUIRE(doc.Apply(**second).has_value());
  REQUIRE(doc.ComponentJson(kPlayer, "Transform") == end);

  const std::string path = doc.ComponentJson(kEnemy1, "Path");
  auto points = PathPointCommand::PointsOf(path);
  REQUIRE(points.has_value());
  auto insert_a =
      PathPointCommand::Insert(doc.Get(), kEnemy1, 0, Vec2{1.0F, 2.0F});
  auto insert_b =
      PathPointCommand::Insert(doc.Get(), kEnemy1, 0, Vec2{3.0F, 4.0F});
  REQUIRE((insert_a && insert_b));
  REQUIRE(doc.Apply(**insert_a).has_value());
  REQUIRE(doc.Apply(**insert_b).has_value());
  auto both = PathPointCommand::PointsOf(doc.ComponentJson(kEnemy1, "Path"));
  REQUIRE(both.has_value());
  REQUIRE(both->size() == points->size() + 2);
  REQUIRE((*both)[0] == Vec2{3.0F, 4.0F});
  REQUIRE((*both)[1] == Vec2{1.0F, 2.0F});
  REQUIRE(doc.Revert(**insert_b).has_value());
  auto one = PathPointCommand::PointsOf(doc.ComponentJson(kEnemy1, "Path"));
  REQUIRE(one.has_value());
  REQUIRE(one->size() == points->size() + 1);
  REQUIRE((*one)[0] == Vec2{1.0F, 2.0F});
  REQUIRE(doc.Revert(**insert_a).has_value());
  REQUIRE(doc.ComponentJson(kEnemy1, "Path") == path);
}

TEST_CASE("Commands become SessionLog entries for Play", "[commands]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  DocumentWorld doc;
  SetComponentCommand set(kPlayer, "Transform",
                          doc.ComponentJson(kPlayer, "Transform"),
                          doc.ComponentJson(kEnemy1, "Transform"));
  auto entries = set.ToSession(doc.Registry(), 42);
  REQUIRE(entries.has_value());
  REQUIRE(entries->size() == 1);
  REQUIRE((*entries)[0].op == SessionOp::kSetComponent);
  REQUIRE((*entries)[0].apply_before == 42);
  REQUIRE((*entries)[0].target == kPlayer);

  DestroyEntityCommand destroy(kEnemy1);
  entries = destroy.ToSession(doc.Registry(), 7);
  REQUIRE(entries.has_value());
  REQUIRE((*entries)[0].op == SessionOp::kDestroyEntity);

  auto bundle = SerializeEntity(doc.Get(), kEnemy2);
  REQUIRE(bundle.has_value());
  CreateEntityCommand create(Guid{0xED17, 2}, *bundle);
  entries = create.ToSession(doc.Registry(), 7);
  REQUIRE(entries.has_value());
  REQUIRE((*entries)[0].op == SessionOp::kCreateEntity);
}

}  // namespace
}  // namespace se::editor
