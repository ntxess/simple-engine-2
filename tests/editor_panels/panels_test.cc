// Copyright 2026 The simple-engine-2 Authors.
//
// The other panels in a headless ImGui context: the Viewport's picking,
// gizmo and camera, the Hierarchy's rows and selection, the Log panel's
// lines and drop counts, the Profiler's axis rule and badge, the Stress
// generator's spreading, Scene settings, and every panel drawing a Play
// World whose gate is closed (and a faulted one) without mutating it.

#include "se_editor_panels/panels.h"

#include <imgui.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "editor_panels/support/panel_harness.h"
#include "editor_panels/support/panel_world.h"
#include "game_rampage/components.h"
#include "se_core/core_components.h"
#include "se_core/guid.h"
#include "se_core/log.h"
#include "se_core/schedule.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_editor/commands.h"
#include "se_editor/editor_stats.h"
#include "se_editor/log_view.h"
#include "se_editor/panel.h"
#include "se_editor_panels/panel_support.h"
#include "se_editor_panels/picking.h"
#include "se_editor_panels/stress_generator.h"
#include "se_render_core/render_list.h"
#include "support/test_roles.h"

namespace se::editor {
namespace {

using Catch::Approx;
using testing::FakeTextureIds;
using testing::PanelHarness;
using testing::PanelWorld;
using testing::RecordingCommands;
using testing::RecordingPlayEdits;

PanelContext EditContext(const World& world, RecordingCommands& commands) {
  PanelContext context;
  context.world = &world;
  context.selection = &commands.selection;
  context.commands = &commands;
  return context;
}

ImVec2 Floor(Vec2 v) { return {std::floor(v.x), std::floor(v.y)}; }

ImVec2 Center(const WidgetProbe& probe, std::string_view key) {
  const std::optional<ImVec2> center = probe.Center(key);
  REQUIRE(center.has_value());
  return center.value_or(ImVec2{});
}

TEST_CASE("A viewport click selects the sprite on top; empty space clears",
          "[editor][viewport]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  PanelWorld w;
  const entt::entity back = w.AddSprite({400.0F, 300.0F}, 64, 64, 0, 0.0F);
  const entt::entity front = w.AddSprite({420.0F, 300.0F}, 64, 64, 1, 0.0F);
  w.Finish();
  PanelHarness ui;
  RecordingCommands commands;
  ViewSettings view;
  ViewportPanel viewport(&view);
  const PanelContext context = EditContext(w.CGet(), commands);
  ui.Frame(viewport, context);
  REQUIRE(viewport.EditorCamera().has_value());

  ui.Click(viewport, context,
           Floor(viewport.Mapping().WorldToScreen({410.0F, 300.0F})));
  REQUIRE(commands.selections.size() == 1);
  REQUIRE(commands.selections.back() == std::vector<Guid>{w.GuidOf(front)});
  REQUIRE(viewport.LastPick().path == PickPath::kExact);
  REQUIRE(viewport.LastPick().hits.size() == 2);
  REQUIRE(viewport.LastPick().hits.back().entity == back);

  ui.Click(viewport, context,
           Floor(viewport.Mapping().WorldToScreen({1500.0F, 900.0F})));
  REQUIRE(commands.selections.back().empty());
  REQUIRE(commands.commands.empty());
}

TEST_CASE("The gizmo drag moves the selection in one coalesced step",
          "[editor][viewport]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  PanelWorld w;
  const entt::entity e = w.AddSprite({400.0F, 300.0F});
  {
    const MutationGate gate = w.Get().OpenGate(GateReason::kTest);
    w.Get().Reg().emplace<Name>(e, "box");
  }
  w.Finish();
  const Guid guid = w.GuidOf(e);
  PanelHarness ui;
  RecordingCommands commands;
  commands.selection.Set(std::span<const Guid>(&guid, 1));
  ViewSettings view;
  ViewportPanel viewport(&view);
  const PanelContext context = EditContext(w.CGet(), commands);
  ui.Frame(viewport, context);
  const float scale = viewport.Mapping().Scale();
  // The X handle, halfway along.
  const ImVec2 handle =
      Floor(viewport.Mapping().WorldToScreen({400.0F, 300.0F}));
  const ImVec2 press{handle.x + 30.0F, handle.y};
  ui.MoveMouse(press);
  ui.Button(0, true);
  ui.Frame(viewport, context);
  for (int i = 1; i <= 2; ++i) {
    ui.MoveMouse({press.x + 20.0F * static_cast<float>(i), press.y + 15.0F});
    ui.Frame(viewport, context);
    REQUIRE(commands.ApplyPending(w.Get()));
  }
  ui.Button(0, false);
  ui.Frame(viewport, context);
  REQUIRE(commands.events.front() == "begin:Move box");
  REQUIRE(commands.events.back() == "end");
  REQUIRE(commands.commands.size() == 2);
  const Vec2 pos = w.CGet().CReg().get<Transform>(e).pos;
  REQUIRE(pos.x == Approx(400.0F + 40.0F / scale));
  // The X handle keeps y.
  REQUIRE(pos.y == 300.0F);
  // No pick happened: the press was on the gizmo.
  REQUIRE(commands.selections.empty());
}

TEST_CASE("The wheel zooms the editor camera about the cursor",
          "[editor][viewport]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  PanelWorld w;
  w.Finish();
  PanelHarness ui;
  RecordingCommands commands;
  ViewSettings view;
  ViewportPanel viewport(&view);
  const PanelContext context = EditContext(w.CGet(), commands);
  ui.Frame(viewport, context);
  const ImVec2 cursor =
      Floor(viewport.Mapping().WorldToScreen({300.0F, 200.0F}));
  const Vec2 anchor = viewport.Mapping().ScreenToWorld({cursor.x, cursor.y});
  ui.MoveMouse({cursor.x + 1.0F, cursor.y});
  ui.Frame(viewport, context);
  ui.MoveMouse(cursor);
  ui.Frame(viewport, context);
  ui.Wheel(1.0F);
  ui.Frame(viewport, context);
  ui.Frame(viewport, context);
  REQUIRE(viewport.EditorCamera().value_or(Camera2D{}).zoom == Approx(1.1F));
  const Vec2 under = viewport.Mapping().ScreenToWorld({cursor.x, cursor.y});
  REQUIRE(under.x == Approx(anchor.x).margin(0.01));
  REQUIRE(under.y == Approx(anchor.y).margin(0.01));
  // The document's own cameras are untouched.
  REQUIRE(commands.commands.empty());
}

TEST_CASE("A faulted World picks by brute force and takes no edits",
          "[editor][viewport]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  PanelWorld w(WorldKind::kPlay);
  w.AddSprite({400.0F, 300.0F});
  w.Finish();
  w.Step();
  PanelHarness ui;
  Selection selection;
  ViewSettings view;
  ViewportPanel viewport(&view);
  PanelContext context;
  context.world = &w.CGet();
  context.playing = true;
  context.faulted = true;
  context.selection = &selection;
  ui.Frame(viewport, context);
  ui.Click(viewport, context,
           Floor(viewport.Mapping().WorldToScreen({400.0F, 300.0F})));
  REQUIRE(viewport.LastPick().path == PickPath::kBruteForce);
  REQUIRE(viewport.LastPick().hits.size() == 1);
  REQUIRE(selection.Empty());
}

TEST_CASE("The hierarchy lists entities in spawn order and selects them",
          "[editor][hierarchy]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  PanelWorld w;
  const entt::entity a = w.AddSprite({1.0F, 1.0F});
  const entt::entity b = w.AddSprite({2.0F, 2.0F});
  const entt::entity c = w.AddSprite({3.0F, 3.0F});
  {
    const MutationGate gate = w.Get().OpenGate(GateReason::kTest);
    w.Get().Reg().emplace<Name>(a, "alpha");
    w.Get().Reg().emplace<Name>(b, "beta");
    w.Get().Reg().emplace<Name>(c, "gamma");
  }
  w.Finish();
  PanelHarness ui;
  RecordingCommands commands;
  HierarchyPanel hierarchy;
  WidgetProbe probe;
  hierarchy.SetProbe(&probe);
  const PanelContext context = EditContext(w.CGet(), commands);
  ui.Frame(hierarchy, context);
  REQUIRE(hierarchy.Rows() ==
          std::vector<Guid>{w.GuidOf(a), w.GuidOf(b), w.GuidOf(c)});

  ui.Click(hierarchy, context,
           Center(probe, "hierarchy.row." + ToString(w.GuidOf(b))));
  REQUIRE(commands.selections.back() == std::vector<Guid>{w.GuidOf(b)});
  // Ctrl-click adds to the selection.
  ui.Key(ImGuiMod_Ctrl, true);
  ui.Click(hierarchy, context,
           Center(probe, "hierarchy.row." + ToString(w.GuidOf(c))));
  ui.Key(ImGuiMod_Ctrl, false);
  REQUIRE(commands.selections.back() ==
          std::vector<Guid>{w.GuidOf(b), w.GuidOf(c)});

  // Destroy selected: one destroy command per selected entity, one step.
  ui.Click(hierarchy, context, Center(probe, "hierarchy.destroy"));
  REQUIRE(commands.commands.size() == 2);
  REQUIRE(commands.commands[0]->Kind() == CommandKind::kDestroyEntity);
  REQUIRE(commands.events.front() == "begin:Destroy entities");

  // The filter keeps matching names.
  ui.Click(hierarchy, context, Center(probe, "hierarchy.filter"));
  ui.Type("amm");
  ui.Frame(hierarchy, context);
  ui.Frame(hierarchy, context);
  REQUIRE(hierarchy.Rows() == std::vector<Guid>{w.GuidOf(c)});
}

TEST_CASE("The log panel shows the view's lines, colours and drop counts",
          "[editor][log]") {
  const testing::MainThreadScope main_thread;
  PanelHarness ui;
  LogView log(16);
  for (int i = 0; i < 20; ++i) {
    log.Append(std::format("0.{:06} {} main t={} a.cc:1 line {}", i,
                           i % 2 == 0 ? 'I' : 'E', i, i));
  }
  log.SetCounts(3, 2);
  PanelContext context;
  context.log = &log;
  LogPanel panel;
  ui.Frame(panel, context);
  REQUIRE(panel.LastShown().lines == 16);
  REQUIRE(panel.LastShown().first == 0);
  REQUIRE(panel.LastShown().last == 15);
  REQUIRE(panel.LastShown().counts ==
          "20 lines, 3 dropped, 2 cut by the "
          "rate limit");
  REQUIRE(LogLevelColor(LogLevel::kError).x == 1.0F);
  REQUIRE(LogLevelColor(LogLevel::kError).y == 0.0F);
  REQUIRE(LogLevelColor(LogLevel::kWarn).z == 0.0F);
  REQUIRE(LogLevelColor(LogLevel::kInfo).z == 1.0F);

  PanelContext no_log;
  ui.Frame(panel, no_log);
  REQUIRE(panel.LastShown().lines == 0);
}

TEST_CASE("The profiler graphs each system against the legacy axis rule",
          "[editor][profiler]") {
  const testing::MainThreadScope main_thread;
  PanelHarness ui;
  std::vector<SystemStats> systems(2);
  systems[0].name = "PathSystem";
  systems[0].Record(2'000'000);
  systems[0].Record(4'000'000);
  systems[1].name = "Idle";
  EditorStats stats;
  stats.systems = systems;
  PanelContext context;
  context.stats = &stats;
  bool dumped = false;
  ProfilerPanel panel([&dumped] { dumped = true; });
  WidgetProbe probe;
  panel.SetProbe(&probe);
  ui.Frame(panel, context);
  REQUIRE(panel.Series().size() == 2);
  REQUIRE(panel.Series()[0].first == "PathSystem");
  REQUIRE(panel.Series()[0].second.axis_max_ms == Approx(4.5F));
  REQUIRE(panel.Series()[0].second.peak_ms == Approx(4.0F));
  REQUIRE(panel.Series()[1].second.axis_max_ms == 1.0F);
  REQUIRE_FALSE(panel.SlowdownShown());

  stats.plan.overloaded = true;
  ui.Click(panel, context, Center(probe, "profiler.dump"));
  REQUIRE(panel.SlowdownShown());
  REQUIRE(dumped);
}

TEST_CASE("The stress generator spreads a press over frames and ticks",
          "[editor][stress]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  PanelWorld w;
  w.Finish();
  PanelHarness ui;
  StressPanel stress;
  stress.SetAmount(130);
  stress.SetPerTick(64);
  WidgetProbe probe;
  stress.SetProbe(&probe);

  SECTION("Edit mode: one batch of create commands per frame, one step") {
    RecordingCommands commands;
    const PanelContext context = EditContext(w.CGet(), commands);
    ui.Frame(stress, context);
    ui.Click(stress, context, Center(probe, "stress.generate"));
    REQUIRE(commands.commands.size() == 64);
    REQUIRE(commands.events.front() == "begin:Generate 130 enemies");
    REQUIRE(stress.Pending() == 66);
    ui.Frame(stress, context);
    REQUIRE(commands.commands.size() == 128);
    ui.Frame(stress, context);
    REQUIRE(commands.commands.size() == 130);
    REQUIRE(stress.Pending() == 0);
    REQUIRE(commands.events.back() == "end");
    REQUIRE(commands.commands[0]->Kind() == CommandKind::kCreateEntity);
    REQUIRE(commands.commands[0]->Target() ==
            StressGuid(w.CGet().Desc().seed, 0, 0));
    REQUIRE(commands.ApplyPending(w.Get()));
    REQUIRE(w.CGet().EntityCount() == 130);
    // Remove destroys every enemy, in one step.
    ui.Click(stress, context, Center(probe, "stress.remove"));
    REQUIRE(commands.commands.size() == 260);
    REQUIRE(commands.ApplyPending(w.Get()));
    REQUIRE(w.CGet().EntityCount() == 0);
    // The next press takes new Guids.
    ui.Click(stress, context, Center(probe, "stress.generate"));
    REQUIRE(commands.commands.back()->Kind() == CommandKind::kCreateEntity);
    REQUIRE(stress.Presses() == 2);
  }

  SECTION("Play: SessionLog edits against consecutive ticks") {
    RecordingPlayEdits play;
    play.next_tick = 50;
    PanelContext context;
    context.world = &w.CGet();
    context.playing = true;
    context.selection = &play.selection;
    context.play_edits = &play;
    ui.Frame(stress, context);
    ui.Click(stress, context, Center(probe, "stress.generate"));
    REQUIRE(play.queued.size() == 130);
    REQUIRE(play.queued[0].first == 50);
    REQUIRE(play.queued[63].first == 50);
    REQUIRE(play.queued[64].first == 51);
    REQUIRE(play.queued[129].first == 52);
    REQUIRE(stress.Pending() == 0);
  }
}

TEST_CASE("Scene settings adds a primary camera, then edits it",
          "[editor][scene]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  PanelWorld w;
  w.Finish();
  PanelHarness ui;
  RecordingCommands commands;
  ViewSettings view;
  SceneSettingsPanel panel(&view);
  WidgetProbe probe;
  panel.SetProbe(&probe);
  const PanelContext context = EditContext(w.CGet(), commands);
  ui.Frame(panel, context);
  ui.Click(panel, context, Center(probe, "scene.camera.add"));
  REQUIRE(commands.commands.size() == 1);
  REQUIRE(commands.commands[0]->Kind() == CommandKind::kCreateEntity);
  REQUIRE(commands.ApplyPending(w.Get()));
  const auto camera = PrimaryCamera(w.CGet());
  REQUIRE(camera.has_value());
  REQUIRE(camera.value_or(Camera2D{}).virtual_size == Vec2{1920.0F, 1080.0F});
  probe.Clear();
  ui.Frame(panel, context);
  REQUIRE(probe.Find("scene.camera.zoom").has_value());
}

TEST_CASE("No panel mutates a Play World or a faulted one",
          "[editor][panels]") {
  const testing::MainThreadScope main_thread;
  const InlineOwnerScope owner(OnMainThread());
  PanelWorld w(WorldKind::kPlay);
  const entt::entity e = w.AddSprite({400.0F, 300.0F});
  {
    const MutationGate gate = w.Get().OpenGate(GateReason::kTest);
    entt::registry& reg = w.Get().Reg();
    reg.emplace<Name>(e, "walker");
    rampage::Path path;
    path.points = {{400.0F, 300.0F}, {600.0F, 300.0F}};
    reg.emplace<rampage::Path>(e, path);
    rampage::Stats stats;
    rampage::SetStat(stats, rampage::kStatHp, 100.0F);
    reg.emplace<rampage::Stats>(e, stats);
    reg.emplace<rampage::Team>(e, rampage::TeamId::kEnemy);
    reg.emplace<rampage::EffectSource>(e);
  }
  w.Finish();
  w.Step(3);
  const WorldHash hash = w.CGet().Hash();
  const std::size_t entities = w.CGet().EntityCount();
  const Guid guid = w.GuidOf(e);

  PanelHarness ui;
  RecordingPlayEdits play;
  play.selection.Set(std::span<const Guid>(&guid, 1));
  LogView log;
  log.Append("0.000001 I main t=0 a.cc:1 hello");
  std::vector<SystemStats> systems(1);
  systems[0].name = "PathSystem";
  EditorStats stats;
  stats.systems = systems;
  FakeTextureIds textures;
  DefaultPanels panels;
  panels.Inspector().OpenCanvas(guid);
  std::vector<Panel*> all;
  for (const auto& panel : panels.All()) all.push_back(panel.get());

  PanelContext context;
  context.world = &w.CGet();
  context.playing = true;
  context.selection = &play.selection;
  context.play_edits = &play;
  context.textures = &textures;
  context.stats = &stats;
  context.log = &log;
  for (int frame = 0; frame < 4; ++frame) {
    ui.MoveMouse({200.0F + 50.0F * static_cast<float>(frame), 300.0F});
    ui.Button(0, frame % 2 == 0);
    ui.Frame(all, context);
  }
  ui.Button(0, false);
  context.faulted = true;
  context.play_edits = nullptr;
  for (int frame = 0; frame < 4; ++frame) {
    ui.MoveMouse({300.0F + 40.0F * static_cast<float>(frame), 400.0F});
    ui.Button(0, frame % 2 == 0);
    ui.Frame(all, context);
  }
  // The gate stayed closed (any Reg() call would have tripped its assert),
  // and nothing changed.
  REQUIRE_FALSE(w.CGet().GateOpen());
  REQUIRE(w.CGet().Hash().Folded() == hash.Folded());
  REQUIRE(w.CGet().EntityCount() == entities);
  REQUIRE_FALSE(w.CGet().HashDirty());
}

}  // namespace
}  // namespace se::editor
