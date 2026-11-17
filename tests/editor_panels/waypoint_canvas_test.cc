// Copyright 2026 The simple-engine-2 Authors.
//
// The waypoint canvas's controls and path-editing rules as pure functions
// (app/editor/Editor.cpp:828-885 and :903-930 at master): each control and
// each rule against what the legacy canvas did.

#include "se_editor_panels/waypoint_canvas.h"

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <utility>
#include <vector>

#include "game_rampage/components.h"
#include "se_core/types.h"

namespace se::editor {
namespace {

using Kind = PathPointOp::Kind;

rampage::Path PathOf(std::vector<Vec2> points, bool loop = false) {
  rampage::Path path;
  path.points = std::move(points);
  path.loop = loop;
  path.segment = 1;
  path.u = 12.5F;
  path.done = true;
  return path;
}

void RequireRestarted(const rampage::Path& path) {
  REQUIRE(path.segment == 0);
  REQUIRE(path.u == 0.0F);
  REQUIRE_FALSE(path.done);
}

TEST_CASE("The canvas maps world pixels through scrolling, scale and zoom",
          "[editor][canvas]") {
  const CanvasView view{{10.0F, -20.0F}, 2.0F};
  const CanvasMapping mapping = MapCanvas({100.0F, 50.0F}, {0.5F, 0.5F}, view);
  REQUIRE(mapping.origin == Vec2{110.0F, 30.0F});
  const Vec2 screen = WorldToCanvas(mapping, {40.0F, 60.0F});
  REQUIRE(screen == Vec2{150.0F, 90.0F});
  REQUIRE(CanvasToWorld(mapping, screen) == Vec2{40.0F, 60.0F});
}

TEST_CASE("A left click on the hovered canvas adds a point at the cursor",
          "[editor][canvas]") {
  const CanvasView view{{8.0F, 4.0F}, 1.0F};
  const CanvasMapping mapping = MapCanvas({0.0F, 0.0F}, {1.0F, 1.0F}, view);
  CanvasInput input;
  input.mouse = {108.0F, 54.0F};
  input.hovered = true;
  input.left_clicked = true;
  const CanvasFrame frame = ProcessCanvasInput(view, mapping, input);
  REQUIRE(frame.add_point == Vec2{100.0F, 50.0F});
  REQUIRE_FALSE(frame.open_menu);
  REQUIRE(frame.view == view);

  input.hovered = false;
  REQUIRE_FALSE(ProcessCanvasInput(view, mapping, input).add_point);
}

TEST_CASE("A right drag pans by the mouse delta and opens no menu",
          "[editor][canvas]") {
  const CanvasView view{{0.0F, 0.0F}, 1.0F};
  const CanvasMapping mapping = MapCanvas({}, {1.0F, 1.0F}, view);
  CanvasInput input;
  input.hovered = true;
  input.active = true;
  input.right_dragging = true;
  input.mouse_delta = {5.0F, -3.0F};
  input.right_drag_delta = {12.0F, -7.0F};
  CanvasFrame frame = ProcessCanvasInput(view, mapping, input);
  REQUIRE(frame.view.scrolling == Vec2{5.0F, -3.0F});
  // Releasing after the drag opens no menu.
  input.right_dragging = false;
  input.right_released = true;
  frame = ProcessCanvasInput(frame.view, mapping, input);
  REQUIRE_FALSE(frame.open_menu);
  // A drag needs the item active (the press started on the canvas).
  input.active = false;
  input.right_dragging = true;
  REQUIRE(ProcessCanvasInput(view, mapping, input).view == view);
}

TEST_CASE("A right click without a drag opens the context menu",
          "[editor][canvas]") {
  const CanvasView view;
  const CanvasMapping mapping = MapCanvas({}, {1.0F, 1.0F}, view);
  CanvasInput input;
  input.hovered = true;
  input.right_released = true;
  REQUIRE(ProcessCanvasInput(view, mapping, input).open_menu);
  input.hovered = false;
  REQUIRE_FALSE(ProcessCanvasInput(view, mapping, input).open_menu);
}

TEST_CASE("The wheel zooms in 0.1 steps within 0.1 to 10", "[editor][canvas]") {
  REQUIRE(ZoomAfterWheel(1.0F, 1.0F) == 1.0F + kCanvasZoomStep);
  REQUIRE(ZoomAfterWheel(1.0F, -3.0F) == 1.0F - 3.0F * kCanvasZoomStep);
  REQUIRE(ZoomAfterWheel(0.15F, -1.0F) == kCanvasZoomMin);
  REQUIRE(ZoomAfterWheel(9.95F, 1.0F) == kCanvasZoomMax);
  REQUIRE(ZoomAfterWheel(3.0F, 100.0F) == kCanvasZoomMax);

  const CanvasView view;
  const CanvasMapping mapping = MapCanvas({}, {1.0F, 1.0F}, view);
  CanvasInput input;
  input.wheel = 2.0F;
  REQUIRE(ProcessCanvasInput(view, mapping, input).view.zoom == 1.0F);
  input.hovered = true;
  REQUIRE(ProcessCanvasInput(view, mapping, input).view.zoom ==
          ZoomAfterWheel(1.0F, 2.0F));
}

TEST_CASE("The cursor's point is taken before the frame's pan",
          "[editor][canvas]") {
  const CanvasView view;
  const CanvasMapping mapping = MapCanvas({}, {1.0F, 1.0F}, view);
  CanvasInput input;
  input.mouse = {30.0F, 40.0F};
  input.hovered = true;
  input.active = true;
  input.left_clicked = true;
  input.right_dragging = true;
  input.mouse_delta = {100.0F, 100.0F};
  const CanvasFrame frame = ProcessCanvasInput(view, mapping, input);
  REQUIRE(frame.add_point == Vec2{30.0F, 40.0F});
  REQUIRE(frame.view.scrolling == Vec2{100.0F, 100.0F});
}

TEST_CASE("Reset Camera restores scrolling 0 and zoom 1", "[editor][canvas]") {
  REQUIRE(ResetCanvasView() == CanvasView{{0.0F, 0.0F}, 1.0F});
}

TEST_CASE("The grid starts at fmod(scrolling * zoom) and steps by 32 * zoom",
          "[editor][canvas]") {
  std::vector<float> lines;
  CanvasGridLines(40.0F, 1.0F, 100.0F, lines);
  REQUIRE(lines == std::vector<float>{8.0F, 40.0F, 72.0F});
  CanvasGridLines(0.0F, 2.0F, 130.0F, lines);
  REQUIRE(lines == std::vector<float>{0.0F, 64.0F, 128.0F});
  CanvasGridLines(0.0F, 0.0F, 100.0F, lines);
  REQUIRE(lines.empty());
}

TEST_CASE("Append adds the point, restarts and moves the entity to point 0",
          "[editor][canvas][rules]") {
  const rampage::Path before = PathOf({{10.0F, 10.0F}, {50.0F, 10.0F}});
  const Vec2 entity{30.0F, 10.0F};
  const PathEdit edit = AppendPointEdit(before, entity, {70.0F, 90.0F});
  REQUIRE(edit.kind == PathEditKind::kAppend);
  REQUIRE(edit.ops ==
          std::vector<PathPointOp>{{Kind::kInsert, 2, {70.0F, 90.0F}}});
  REQUIRE(edit.restart);
  REQUIRE_FALSE(edit.loop);
  REQUIRE(edit.entity_pos == Vec2{10.0F, 10.0F});

  rampage::Path path = before;
  Vec2 pos = entity;
  ApplyPathEdit(edit, path, pos);
  REQUIRE(path.points ==
          std::vector<Vec2>{{10.0F, 10.0F}, {50.0F, 10.0F}, {70.0F, 90.0F}});
  RequireRestarted(path);
  REQUIRE_FALSE(path.loop);
  REQUIRE(pos == Vec2{10.0F, 10.0F});
}

TEST_CASE("Append to a path without points starts it at the entity",
          "[editor][canvas][rules]") {
  const rampage::Path before = PathOf({}, true);
  const PathEdit edit = AppendPointEdit(before, {5.0F, 6.0F}, {70.0F, 90.0F});
  REQUIRE(edit.ops ==
          std::vector<PathPointOp>{{Kind::kInsert, 0, {5.0F, 6.0F}},
                                   {Kind::kInsert, 1, {70.0F, 90.0F}}});
  REQUIRE_FALSE(edit.entity_pos);
  rampage::Path path = before;
  Vec2 pos{5.0F, 6.0F};
  ApplyPathEdit(edit, path, pos);
  REQUIRE(path.points == std::vector<Vec2>{{5.0F, 6.0F}, {70.0F, 90.0F}});
  REQUIRE(path.loop);
  RequireRestarted(path);
}

TEST_CASE("Append leaves an entity already at point 0 where it is",
          "[editor][canvas][rules]") {
  const rampage::Path before = PathOf({{1.0F, 2.0F}});
  const PathEdit edit = AppendPointEdit(before, {1.0F, 2.0F}, {3.0F, 4.0F});
  REQUIRE_FALSE(edit.entity_pos);
}

TEST_CASE("Remove one erases the last point and moves the entity to point 0",
          "[editor][canvas][rules]") {
  const rampage::Path before =
      PathOf({{10.0F, 10.0F}, {50.0F, 10.0F}, {50.0F, 50.0F}}, true);
  const std::optional<PathEdit> maybe =
      RemoveLastPointEdit(before, {50.0F, 30.0F});
  REQUIRE(maybe.has_value());
  const PathEdit edit = maybe.value_or(PathEdit{});
  REQUIRE(edit.kind == PathEditKind::kRemoveLast);
  REQUIRE(edit.ops == std::vector<PathPointOp>{{Kind::kErase, 2, {}}});
  REQUIRE(edit.entity_pos == Vec2{10.0F, 10.0F});
  rampage::Path path = before;
  Vec2 pos{50.0F, 30.0F};
  ApplyPathEdit(edit, path, pos);
  REQUIRE(path.points == std::vector<Vec2>{{10.0F, 10.0F}, {50.0F, 10.0F}});
  REQUIRE(path.loop);
  RequireRestarted(path);
  REQUIRE(pos == Vec2{10.0F, 10.0F});
}

TEST_CASE("Remove one keeps a path's last point and only restarts it",
          "[editor][canvas][rules]") {
  const rampage::Path before = PathOf({{10.0F, 10.0F}});
  const std::optional<PathEdit> maybe =
      RemoveLastPointEdit(before, {12.0F, 10.0F});
  REQUIRE(maybe.has_value());
  const PathEdit edit = maybe.value_or(PathEdit{});
  REQUIRE(edit.ops.empty());
  REQUIRE(edit.entity_pos == Vec2{10.0F, 10.0F});
  rampage::Path path = before;
  Vec2 pos{12.0F, 10.0F};
  ApplyPathEdit(edit, path, pos);
  REQUIRE(path.points == std::vector<Vec2>{{10.0F, 10.0F}});
  RequireRestarted(path);
}

TEST_CASE("Remove one and Remove all do nothing on a path without points",
          "[editor][canvas][rules]") {
  const rampage::Path empty = PathOf({});
  REQUIRE_FALSE(RemoveLastPointEdit(empty, {1.0F, 1.0F}).has_value());
  REQUIRE_FALSE(ClearPathEdit(empty, {1.0F, 1.0F}).has_value());
}

TEST_CASE("Remove all leaves the entity's position alone, looping",
          "[editor][canvas][rules]") {
  const rampage::Path before =
      PathOf({{10.0F, 10.0F}, {50.0F, 10.0F}, {50.0F, 50.0F}}, false);
  const Vec2 entity{42.0F, 17.0F};
  const std::optional<PathEdit> maybe = ClearPathEdit(before, entity);
  REQUIRE(maybe.has_value());
  const PathEdit edit = maybe.value_or(PathEdit{});
  REQUIRE(edit.kind == PathEditKind::kClear);
  REQUIRE(edit.ops == std::vector<PathPointOp>{{Kind::kErase, 2, {}},
                                               {Kind::kErase, 1, {}},
                                               {Kind::kMove, 0, entity}});
  REQUIRE(edit.loop == true);
  REQUIRE_FALSE(edit.entity_pos);
  rampage::Path path = before;
  Vec2 pos = entity;
  ApplyPathEdit(edit, path, pos);
  REQUIRE(path.points == std::vector<Vec2>{entity});
  REQUIRE(path.loop);
  RequireRestarted(path);
  REQUIRE(pos == entity);
}

TEST_CASE("Remove all on a path already at the entity only restarts it",
          "[editor][canvas][rules]") {
  const rampage::Path before = PathOf({{3.0F, 3.0F}});
  const std::optional<PathEdit> maybe = ClearPathEdit(before, {3.0F, 3.0F});
  REQUIRE(maybe.has_value());
  const PathEdit edit = maybe.value_or(PathEdit{});
  REQUIRE(edit.ops.empty());
  REQUIRE(edit.loop == true);
}

}  // namespace
}  // namespace se::editor
