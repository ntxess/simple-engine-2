// Copyright 2026 The simple-engine-2 Authors.
//
// The Inspector in a headless ImGui context, driven by synthetic ImGui IO
// events: the waypoint canvas's rules each produce the expected path
// commands; reflection and custom drawers produce set-component commands
// with the right before and after JSON; a drag is one coalesced step.

#include <imgui.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor_panels/support/panel_harness.h"
#include "editor_panels/support/panel_world.h"
#include "game_rampage/components.h"
#include "game_rampage/json.h"
#include "se_core/core_components.h"
#include "se_core/guid.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_editor/commands.h"
#include "se_editor/panel.h"
#include "se_editor_panels/panel_support.h"
#include "se_editor_panels/panels.h"
#include "se_editor_panels/path_commands.h"
#include "se_editor_panels/stat_name_command.h"
#include "se_editor_panels/waypoint_canvas.h"
#include "support/test_roles.h"

namespace se::editor {
namespace {

using Catch::Approx;
using testing::PanelHarness;
using testing::PanelWorld;
using testing::RecordingCommands;

PanelContext EditContext(const World& world, RecordingCommands& commands) {
  PanelContext context;
  context.world = &world;
  context.selection = &commands.selection;
  context.commands = &commands;
  return context;
}

ImVec2 Center(const WidgetProbe& probe, std::string_view key) {
  const std::optional<ImVec2> center = probe.Center(key);
  REQUIRE(center.has_value());
  return center.value_or(ImVec2{});
}

// A walker: a Path of two points, standing on the first.
entt::entity AddWalker(PanelWorld& w) SE_REQUIRES(g_sim_owner) {
  const entt::entity e = w.AddSprite({100.0F, 100.0F});
  const MutationGate gate = w.Get().OpenGate(GateReason::kTest);
  entt::registry& reg = w.Get().Reg();
  reg.emplace<Name>(e, "walker");
  rampage::Path path;
  path.points = {{100.0F, 100.0F}, {300.0F, 100.0F}};
  path.loop = true;
  reg.emplace<rampage::Path>(e, path);
  return e;
}

std::vector<CommandKind> Kinds(const RecordingCommands& commands) {
  std::vector<CommandKind> kinds;
  kinds.reserve(commands.commands.size());
  for (const auto& command : commands.commands) {
    kinds.push_back(command->Kind());
  }
  return kinds;
}

const rampage::Path& PathOf(const PanelWorld& w, entt::entity e) {
  return w.CGet().CReg().get<rampage::Path>(e);
}

TEST_CASE("Canvas rules become path commands: add, remove one, remove all",
          "[editor][inspector][canvas]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  PanelWorld w;
  const entt::entity e = AddWalker(w);
  w.Finish();
  const Guid guid = w.GuidOf(e);
  PanelHarness ui;
  RecordingCommands commands;
  commands.selection.Set(std::span<const Guid>(&guid, 1));
  InspectorPanel inspector;
  WidgetProbe probe;
  inspector.SetProbe(&probe);
  inspector.OpenCanvas(guid);
  const PanelContext context = EditContext(w.CGet(), commands);
  ui.Frame(inspector, context);
  ui.Frame(inspector, context);
  const std::optional<Rect> canvas = probe.Find("canvas");
  REQUIRE(canvas.has_value());
  const Rect rect = canvas.value_or(Rect{});
  // The arena (1920 x 1080) letterboxed into the canvas, unpanned.
  const float scale = rect.w / 1920.0F;
  REQUIRE(rect.h == Approx(1080.0F * scale).margin(1.0));

  SECTION("a left click appends one point: InsertPathPoint") {
    // ImGui floors the mouse position: the point is where the floored
    // cursor is.
    const ImVec2 click{std::floor(rect.x + 500.0F * scale),
                       std::floor(rect.y + 400.0F * scale)};
    const float expected_x = (click.x - rect.x) / scale;
    const float expected_y = (click.y - rect.y) / scale;
    ui.Click(inspector, context, click);
    REQUIRE(Kinds(commands) ==
            std::vector<CommandKind>{CommandKind::kInsertPathPoint});
    REQUIRE(commands.events ==
            std::vector<std::string>{
                "begin:Add path point",
                "submit:" + std::string(commands.commands[0]->Label()), "end"});
    const auto& insert =
        static_cast<const PathPointCommand&>(*commands.commands[0]);
    REQUIRE(insert.Index() == 2);
    REQUIRE(insert.Point().x == Approx(expected_x).margin(0.01));
    REQUIRE(insert.Point().y == Approx(expected_y).margin(0.01));
    REQUIRE(commands.ApplyPending(w.Get()));
    REQUIRE(PathOf(w, e).points.size() == 3);
  }

  SECTION("a right click opens the menu; Remove one deletes the last point") {
    ui.Click(inspector, context, {rect.x + 50.0F, rect.y + 50.0F}, 1);
    ui.Frame(inspector, context);
    const ImVec2 remove = Center(probe, "canvas.menu.remove_one");
    ui.Click(inspector, context, remove);
    REQUIRE(Kinds(commands) ==
            std::vector<CommandKind>{CommandKind::kDeletePathPoint});
    REQUIRE(
        static_cast<const PathPointCommand&>(*commands.commands[0]).Index() ==
        1);
    REQUIRE(commands.ApplyPending(w.Get()));
    REQUIRE(PathOf(w, e).points == std::vector<Vec2>{{100.0F, 100.0F}});
  }

  SECTION("Remove all on a looping path is one delete here") {
    // Point 0 is the entity's position and the path loops already: only
    // point 1 goes.
    ui.Click(inspector, context, {rect.x + 50.0F, rect.y + 50.0F}, 1);
    ui.Frame(inspector, context);
    ui.Click(inspector, context, Center(probe, "canvas.menu.remove_all"));
    REQUIRE(Kinds(commands) ==
            std::vector<CommandKind>{CommandKind::kDeletePathPoint});
    REQUIRE(commands.ApplyPending(w.Get()));
    REQUIRE(PathOf(w, e).points == std::vector<Vec2>{{100.0F, 100.0F}});
  }

  SECTION("Remove all leaves the entity's position alone, looping") {
    {
      const MutationGate gate = w.Get().OpenGate(GateReason::kTest);
      rampage::Path& path = w.Get().Reg().get<rampage::Path>(e);
      path.points.push_back({300.0F, 300.0F});
      path.loop = false;
    }
    ui.Click(inspector, context, {rect.x + 50.0F, rect.y + 50.0F}, 1);
    ui.Frame(inspector, context);
    ui.Click(inspector, context, Center(probe, "canvas.menu.remove_all"));
    REQUIRE(Kinds(commands) ==
            std::vector<CommandKind>{CommandKind::kSetComponent,
                                     CommandKind::kSetComponent});
    const auto& set =
        static_cast<const SetComponentCommand&>(*commands.commands[0]);
    REQUIRE(set.Component() == "Path");
    REQUIRE(commands.ApplyPending(w.Get()));
    REQUIRE(PathOf(w, e).points == std::vector<Vec2>{{100.0F, 100.0F}});
    REQUIRE(PathOf(w, e).loop);
    // The second resets the derived PathCache.
    REQUIRE(static_cast<const SetComponentCommand&>(*commands.commands[1])
                .Component() == "PathCache");
  }

  SECTION("the wheel zooms in 0.1 steps and a right drag pans, no menu") {
    ui.MoveMouse({rect.x + 100.0F, rect.y + 100.0F});
    ui.Wheel(1.0F);
    ui.Frame(inspector, context);
    REQUIRE(inspector.CanvasViewOf(guid).value_or(CanvasView{}).zoom ==
            Approx(1.1F));
    ui.Button(1, true);
    ui.Frame(inspector, context);
    for (int i = 1; i <= 4; ++i) {
      ui.MoveMouse(
          {rect.x + 100.0F + 10.0F * static_cast<float>(i), rect.y + 100.0F});
      ui.Frame(inspector, context);
    }
    ui.Button(1, false);
    probe.Clear();
    ui.Frame(inspector, context);
    ui.Frame(inspector, context);
    REQUIRE(inspector.CanvasViewOf(guid).value_or(CanvasView{}).scrolling.x >
            20.0F);
    REQUIRE_FALSE(probe.Find("canvas.menu.remove_one").has_value());
    REQUIRE(commands.commands.empty());
  }
}

TEST_CASE("A canvas rule on a running path restarts it and moves the entity",
          "[editor][inspector][canvas]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  PanelWorld w;
  const entt::entity e = AddWalker(w);
  w.Finish();
  {
    const MutationGate gate = w.Get().OpenGate(GateReason::kTest);
    rampage::Path& path = w.Get().Reg().get<rampage::Path>(e);
    path.segment = 1;
    path.u = 30.0F;
    w.Get().Reg().get<Transform>(e).pos = {130.0F, 100.0F};
  }
  const Guid guid = w.GuidOf(e);
  const PathEdit edit =
      AppendPointEdit(PathOf(w, e), {130.0F, 100.0F}, {500.0F, 500.0F});
  auto made = PathEditCommands(w.CGet(), guid, edit);
  REQUIRE(made.has_value());
  std::vector<std::string> components;
  for (const auto& command : *made) {
    REQUIRE(command->Kind() == CommandKind::kSetComponent);
    components.push_back(
        static_cast<const SetComponentCommand&>(*command).Component());
  }
  REQUIRE(components ==
          std::vector<std::string>{"Path", "Transform", "PathCache"});
  {
    const MutationGate gate = w.Get().OpenGate(GateReason::kEditorCommand);
    for (auto& command : *made) REQUIRE(command->Apply(w.Get()).has_value());
  }
  const rampage::Path& path = PathOf(w, e);
  REQUIRE(path.points.size() == 3);
  REQUIRE(path.segment == 0);
  REQUIRE(path.u == 0.0F);
  REQUIRE(w.CGet().CReg().get<Transform>(e).pos == Vec2{100.0F, 100.0F});
  REQUIRE(w.CGet().CReg().get<rampage::PathCache>(e).totals.empty());
}

TEST_CASE("A reflection drawer drag is one set-component step",
          "[editor][inspector]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  PanelWorld w;
  const entt::entity e = w.AddSprite({10.0F, 20.0F});
  w.Finish();
  const Guid guid = w.GuidOf(e);
  auto original = ReadComponentJson(w.CGet(), e, "Transform");
  REQUIRE(original.has_value());
  PanelHarness ui;
  RecordingCommands commands;
  commands.selection.Set(std::span<const Guid>(&guid, 1));
  InspectorPanel inspector;
  WidgetProbe probe;
  inspector.SetProbe(&probe);
  const PanelContext context = EditContext(w.CGet(), commands);
  ui.Frame(inspector, context);
  const std::optional<Rect> pos = probe.Find("Transform.pos");
  REQUIRE(pos.has_value());
  const Rect field = pos.value_or(Rect{});
  // Press on the x component and drag 30 px right over three frames.
  const ImVec2 start{field.x + 12.0F, field.y + field.h / 2.0F};
  ui.MoveMouse(start);
  ui.Frame(inspector, context);
  ui.Button(0, true);
  ui.Frame(inspector, context);
  for (int i = 1; i <= 3; ++i) {
    ui.MoveMouse({start.x + 10.0F * static_cast<float>(i), start.y});
    ui.Frame(inspector, context);
    REQUIRE(commands.ApplyPending(w.Get()));
  }
  ui.Button(0, false);
  ui.Frame(inspector, context);

  REQUIRE(commands.events.front() == "begin:Edit Transform");
  REQUIRE(commands.events.back() == "end");
  REQUIRE(commands.commands.size() == 3);
  for (const auto& command : commands.commands) {
    REQUIRE(command->Kind() == CommandKind::kSetComponent);
    REQUIRE(command->Target() == guid);
  }
  const auto& first =
      static_cast<const SetComponentCommand&>(*commands.commands.front());
  REQUIRE(first.Component() == "Transform");
  REQUIRE(first.Before() == *original);
  // The first step's after is the registry's own JSON of x + 5 (speed 0.5).
  Transform moved = w.CGet().CReg().get<Transform>(e);
  const float final_x = moved.pos.x;
  REQUIRE(final_x == Approx(25.0F));
  REQUIRE(moved.pos.y == 20.0F);
  moved.pos.x = 15.0F;
  REQUIRE(first.After() == JsonOfValue(w.CGet(), "Transform", moved).value());
  // Each later step starts where the one before ended.
  for (std::size_t i = 1; i < commands.commands.size(); ++i) {
    const auto& prev =
        static_cast<const SetComponentCommand&>(*commands.commands[i - 1]);
    const auto& next =
        static_cast<const SetComponentCommand&>(*commands.commands[i]);
    REQUIRE(next.Before() == prev.After());
  }
}

TEST_CASE("The Stats drawer edits a stat by name, adds and drops stats",
          "[editor][inspector]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  PanelWorld w;
  const entt::entity e = w.AddSprite({10.0F, 20.0F});
  {
    const MutationGate gate = w.Get().OpenGate(GateReason::kTest);
    entt::registry& reg = w.Get().Reg();
    auto& names = reg.ctx().get<rampage::StatNames>();
    rampage::Stats stats;
    rampage::SetStat(stats, names.Add("HP"), 100.0F);
    rampage::SetStat(stats, names.Add("Speed"), 500.0F);
    reg.emplace<rampage::Stats>(e, stats);
  }
  w.Finish();
  const Guid guid = w.GuidOf(e);
  PanelHarness ui;
  RecordingCommands commands;
  commands.selection.Set(std::span<const Guid>(&guid, 1));
  InspectorPanel inspector;
  WidgetProbe probe;
  inspector.SetProbe(&probe);
  const PanelContext context = EditContext(w.CGet(), commands);
  ui.Frame(inspector, context);

  SECTION("a drag on HP") {
    const ImVec2 hp = Center(probe, "Stats.value.HP");
    ui.MoveMouse(hp);
    ui.Frame(inspector, context);
    ui.Button(0, true);
    ui.Frame(inspector, context);
    ui.MoveMouse({hp.x + 20.0F, hp.y});
    ui.Frame(inspector, context);
    ui.Button(0, false);
    ui.Frame(inspector, context);
    REQUIRE(commands.events.front() == "begin:Edit Stats");
    REQUIRE(commands.commands.size() == 1);
    REQUIRE(commands.ApplyPending(w.Get()));
    const auto& stats = w.CGet().CReg().get<rampage::Stats>(e);
    REQUIRE(*rampage::FindStat(stats, rampage::kStatHp) == Approx(110.0F));
    REQUIRE(*rampage::FindStat(stats, rampage::kStatSpeed) == 500.0F);
  }

  SECTION("the - button drops Speed") {
    ui.Click(inspector, context, Center(probe, "Stats.remove.Speed"));
    REQUIRE(commands.commands.size() == 1);
    REQUIRE(commands.ApplyPending(w.Get()));
    const auto& stats = w.CGet().CReg().get<rampage::Stats>(e);
    REQUIRE(stats.values.size() == 1);
    REQUIRE(rampage::FindStat(stats, rampage::kStatSpeed) == nullptr);
  }

  SECTION("a new stat is typed, then added with +: one group with its name") {
    ui.Click(inspector, context, Center(probe, "Stats.new.name"));
    ui.Type("Armor");
    ui.Frame(inspector, context);
    ui.Click(inspector, context, Center(probe, "Stats.new.add"));
    // {StatNameCommand, SetComponentCommand}: one undo step (M11b).
    REQUIRE(commands.commands.size() == 1);
    REQUIRE(commands.commands[0]->Kind() == CommandKind::kGroup);
    const auto& group = static_cast<const CommandGroup&>(*commands.commands[0]);
    REQUIRE(group.size() == 2);
    REQUIRE(group.At(0).Kind() == CommandKind::kGame);
    REQUIRE(static_cast<const StatNameCommand&>(group.At(0)).StatName() ==
            "Armor");
    REQUIRE(group.At(1).Kind() == CommandKind::kSetComponent);
    REQUIRE_FALSE(HasStatName(w.CGet(), "Armor"));
    REQUIRE(commands.ApplyPending(w.Get()));
    const auto& stats = w.CGet().CReg().get<rampage::Stats>(e);
    REQUIRE(rampage::FindStat(stats, rampage::StatIdOf("Armor")) != nullptr);
    // The name is the World's now: the drawer labels the stat with it.
    REQUIRE(HasStatName(w.CGet(), "Armor"));
    probe.Clear();
    ui.Frame(inspector, context);
    REQUIRE(probe.Find("Stats.value.Armor").has_value());
    // Undo takes the name away with the stat.
    {
      const MutationGate gate = w.Get().OpenGate(GateReason::kEditorCommand);
      REQUIRE(commands.commands[0]->Revert(w.Get()).has_value());
    }
    REQUIRE_FALSE(HasStatName(w.CGet(), "Armor"));
    REQUIRE(HasStatName(w.CGet(), "HP"));
    REQUIRE(rampage::FindStat(w.CGet().CReg().get<rampage::Stats>(e),
                              rampage::StatIdOf("Armor")) == nullptr);
  }
}

TEST_CASE("The effect drawer's stat is typed as text and set on Enter",
          "[editor][inspector]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  PanelWorld w;
  const entt::entity e = w.AddSprite({10.0F, 20.0F});
  {
    const MutationGate gate = w.Get().OpenGate(GateReason::kTest);
    entt::registry& reg = w.Get().Reg();
    auto& names = reg.ctx().get<rampage::StatNames>();
    rampage::EffectSource source;
    source.effects.push_back(
        {rampage::EffectKind::kInstant, names.Add("HP"), -10.0F, 0, 0});
    reg.emplace<rampage::EffectSource>(e, source);
  }
  w.Finish();
  const Guid guid = w.GuidOf(e);
  PanelHarness ui;
  RecordingCommands commands;
  commands.selection.Set(std::span<const Guid>(&guid, 1));
  InspectorPanel inspector;
  WidgetProbe probe;
  inspector.SetProbe(&probe);
  const PanelContext context = EditContext(w.CGet(), commands);
  ui.Frame(inspector, context);
  auto before = ReadComponentJson(w.CGet(), e, "EffectSource");
  REQUIRE(before.has_value());

  ui.Click(inspector, context, Center(probe, "EffectSource.0.stat"));
  ui.Key(ImGuiMod_Ctrl, true);
  ui.Key(ImGuiKey_A, true);
  ui.Frame(inspector, context);
  ui.Key(ImGuiKey_A, false);
  ui.Key(ImGuiMod_Ctrl, false);
  ui.Type("Speed");
  ui.Frame(inspector, context);
  // Nothing is submitted while typing.
  REQUIRE(commands.commands.empty());
  ui.Key(ImGuiKey_Enter, true);
  ui.Frame(inspector, context);
  ui.Key(ImGuiKey_Enter, false);
  ui.Frame(inspector, context);
  REQUIRE(commands.commands.size() == 1);
  // The typed name goes with the edit (M11b): {StatNameCommand, set}.
  REQUIRE(commands.commands.front()->Kind() == CommandKind::kGroup);
  const auto& group =
      static_cast<const CommandGroup&>(*commands.commands.front());
  REQUIRE(group.size() == 2);
  REQUIRE(group.At(0).Kind() == CommandKind::kGame);
  const auto& set = static_cast<const SetComponentCommand&>(group.At(1));
  REQUIRE(set.Component() == "EffectSource");
  REQUIRE(set.Before() == *before);
  REQUIRE(commands.ApplyPending(w.Get()));
  const auto& source = w.CGet().CReg().get<rampage::EffectSource>(e);
  REQUIRE(source.effects.front().stat == rampage::StatIdOf("Speed"));
  REQUIRE(source.effects.front().value == -10.0F);
  REQUIRE(HasStatName(w.CGet(), "Speed"));
}

// R9 of the M10-M14 review: in the docked layout the Inspector is narrow,
// and the effect table's headers ran into each other. Each header is cut
// to its column, the column of the row widget under it.
TEST_CASE("The effect drawer's headers keep to their columns when narrow",
          "[editor][inspector]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  PanelWorld w;
  const entt::entity e = w.AddSprite({10.0F, 20.0F});
  {
    const MutationGate gate = w.Get().OpenGate(GateReason::kTest);
    entt::registry& reg = w.Get().Reg();
    auto& names = reg.ctx().get<rampage::StatNames>();
    rampage::EffectSource source;
    source.effects.push_back(
        {rampage::EffectKind::kInstant, names.Add("HP"), -10.0F, 0, 0});
    reg.emplace<rampage::EffectSource>(e, source);
  }
  w.Finish();
  const Guid guid = w.GuidOf(e);
  PanelHarness ui;
  ui.SetFirstWidth(360.0F);
  RecordingCommands commands;
  commands.selection.Set(std::span<const Guid>(&guid, 1));
  InspectorPanel inspector;
  WidgetProbe probe;
  inspector.SetProbe(&probe);
  const PanelContext context = EditContext(w.CGet(), commands);
  ui.Frame(inspector, context);
  ui.Frame(inspector, context);
  static constexpr const char* kRow[] = {"kind", "stat", "value", "duration",
                                         "interval"};
  static constexpr std::string_view kPrefixes[] = {"EffectSource",
                                                   "EffectSource.new"};
  for (const std::string_view prefix : kPrefixes) {
    std::optional<float> previous_end;
    for (int i = 0; i < 5; ++i) {
      INFO(prefix << " header " << i);
      const std::optional<Rect> found =
          probe.Find(std::format("{}.header.{}", prefix, i));
      REQUIRE(found.has_value());
      const Rect cell = found.value_or(Rect{});
      // "Max Duration" is wider than a column of a 360 px Inspector.
      if (previous_end) REQUIRE(previous_end.value_or(0.0F) <= cell.x);
      if (prefix == "EffectSource") {
        const std::optional<Rect> under =
            probe.Find(std::format("EffectSource.0.{}", kRow[i]));
        REQUIRE(under.has_value());
        const Rect widget = under.value_or(Rect{});
        REQUIRE(cell.x == Approx(widget.x));
        REQUIRE(cell.w <= widget.w + 0.5F);
      }
      previous_end = cell.x + cell.w;
    }
  }
  REQUIRE(ImGui::CalcTextSize("Max Duration").x >
          probe.Find("EffectSource.header.3").value_or(Rect{}).w);
}

TEST_CASE("Number fields are probed as floats or integers once",
          "[editor][inspector]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  PanelWorld w;
  ReflectionDrawer drawer;
  const ComponentInfo* transform = w.Registry().FindByName("Transform");
  const ComponentInfo* collider = w.Registry().FindByName("Collider");
  REQUIRE(transform != nullptr);
  REQUIRE(collider != nullptr);
  auto root = rampage::json::Parse(
      R"({"pos":{"x":1,"y":2},"rot":0,"scale":{"x":1,"y":1},"teleport_tick":0})");
  REQUIRE(root.has_value());
  REQUIRE(drawer.IsFloatField(*transform, *root, {"pos", "x"}));
  REQUIRE(drawer.IsFloatField(*transform, *root, {"rot"}));
  REQUIRE_FALSE(drawer.IsFloatField(*transform, *root, {"teleport_tick"}));
  auto box = rampage::json::Parse(
      R"({"half_extents":{"x":24,"y":24},"role":"body","layer":1,"mask":7})");
  REQUIRE(box.has_value());
  REQUIRE_FALSE(drawer.IsFloatField(*collider, *box, {"layer"}));
  REQUIRE(drawer.IsFloatField(*collider, *box, {"half_extents", "y"}));
}

}  // namespace
}  // namespace se::editor
