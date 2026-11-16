// Copyright 2026 The simple-engine-2 Authors.
//
// The move gizmo's handles and drag geometry.

#include "se_editor_panels/gizmo.h"

#include <catch2/catch_test_macros.hpp>

#include "se_core/types.h"

namespace se::editor {
namespace {

TEST_CASE("A press takes the handle under it", "[editor][gizmo]") {
  const Vec2 o{100.0F, 100.0F};
  REQUIRE(HitGizmo(o, o) == GizmoAxis::kBoth);
  REQUIRE(HitGizmo(o, {105.0F, 95.0F}) == GizmoAxis::kBoth);
  REQUIRE(HitGizmo(o, {140.0F, 103.0F}) == GizmoAxis::kX);
  REQUIRE(HitGizmo(o, {100.0F + kGizmoLength, 100.0F}) == GizmoAxis::kX);
  REQUIRE(HitGizmo(o, {98.0F, 150.0F}) == GizmoAxis::kY);
  REQUIRE(HitGizmo(o, {140.0F, 120.0F}) == GizmoAxis::kNone);
  REQUIRE(HitGizmo(o, {60.0F, 100.0F}) == GizmoAxis::kNone);
  REQUIRE(HitGizmo(o, {100.0F, 100.0F + kGizmoLength + 1.0F}) ==
          GizmoAxis::kNone);
}

TEST_CASE("A drag moves along its handle's axis in world pixels",
          "[editor][gizmo]") {
  const Vec2 start{10.0F, 20.0F};
  const Vec2 from{100.0F, 100.0F};
  const Vec2 to{140.0F, 60.0F};
  REQUIRE(GizmoDragPosition(GizmoAxis::kBoth, start, from, to, 2.0F) ==
          Vec2{30.0F, 0.0F});
  REQUIRE(GizmoDragPosition(GizmoAxis::kX, start, from, to, 2.0F) ==
          Vec2{30.0F, 20.0F});
  REQUIRE(GizmoDragPosition(GizmoAxis::kY, start, from, to, 2.0F) ==
          Vec2{10.0F, 0.0F});
  REQUIRE(GizmoDragPosition(GizmoAxis::kNone, start, from, to, 2.0F) == start);
  REQUIRE(GizmoDragPosition(GizmoAxis::kX, start, from, to, 0.0F) == start);
}

}  // namespace
}  // namespace se::editor
