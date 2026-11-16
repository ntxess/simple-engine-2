// Copyright 2026 The simple-engine-2 Authors.
//
// The editor's panels (docs/milestone-specs.md M10b; docs/design/
// costfree-end-state.md "Editor"). Each is a Panel (se_editor/panel.h):
// the editor draws it inside the window titled Name() and passes the
// frame's PanelContext; the panel reads the const World and the frame's
// stats and changes the document only through EditorCommands (Edit mode)
// or PlayEdits (Play), via EditSink. In Play.Faulted every panel draws the
// frozen World read-only.
//
//   Hierarchy        every entity (name, Guid, parent), in spawn order, with
//                    a filter; a click selects (EditorCommands::Select,
//                    ctrl-click toggles); create and destroy commands.
//   Inspector        the selection's components: the reflection drawer and
//                    the ported custom drawers (component_drawers.h), the
//                    Path's points and its waypoint canvas window (the
//                    legacy "WayPoint Editor", waypoint_canvas.h,
//                    path_commands.h); add and remove a component.
//   Viewport         the World through an editor camera into an M7
//                    ViewportTarget shown with ImGui::Image through
//                    TextureIds; right or middle drag pans, the wheel zooms
//                    about the cursor; a left click picks (picking.h); the
//                    move gizmo (gizmo.h) and the ported overlays
//                    (overlays.h) as ImGui overlays.
//   Log              the editor's LogView: severity colours and layout of
//                    the legacy log panel (Editor.cpp:380-427), a level
//                    filter, auto-scroll, and the drop counts.
//   Profiler         per-system graphs with the legacy axis rule
//                    (profiler_series.h), pacing, the overload and dilation
//                    badge, render stats, ForkStats, a flight-recorder dump
//                    button.
//   Schedule         serial and parallel time per phase, each system's
//                    timing, each fork site's state.
//   Stress generator the ported enemy generator (stress_generator.h).
//   Scene settings   the primary camera, the tick rate, the viewport's
//                    background colour.

#ifndef SE_EDITOR_PANELS_PANELS_H_
#define SE_EDITOR_PANELS_PANELS_H_

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/guid.h"
#include "se_core/log.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_editor/panel.h"
#include "se_editor_panels/component_drawers.h"
#include "se_editor_panels/gizmo.h"
#include "se_editor_panels/overlays.h"
#include "se_editor_panels/panel_support.h"
#include "se_editor_panels/picking.h"
#include "se_editor_panels/profiler_series.h"
#include "se_editor_panels/stress_generator.h"
#include "se_editor_panels/viewport_math.h"
#include "se_editor_panels/waypoint_canvas.h"
#include "se_render_core/render_components.h"

namespace se::editor {

// What the Viewport and Scene settings panels share: the editor's view of
// the scene, not document state.
struct ViewSettings {
  // The viewport's clear colour (straight RGBA in [0, 1]).
  float background[4] = {0.08F, 0.08F, 0.10F, 1.0F};
  OverlayOptions overlays;
};

class HierarchyPanel final : public Panel, public Probed {
 public:
  [[nodiscard]] std::string_view Name() const override { return "Hierarchy"; }
  void Draw(const PanelContext& context) override;

  // The rows the last Draw listed (the ones the filter keeps), in order;
  // only those scrolled into view are submitted to ImGui.
  [[nodiscard]] const std::vector<Guid>& Rows() const noexcept { return rows_; }
  // How many of them the last Draw submitted.
  [[nodiscard]] std::size_t RowsDrawn() const noexcept { return drawn_; }

 private:
  void DrawRow(const PanelContext& context, EditSink& sink, const Guid& row,
               entt::entity entity);

  std::string filter_;
  std::vector<Guid> rows_;
  std::size_t drawn_ = 0;
};

class InspectorPanel final : public Panel, public Probed {
 public:
  [[nodiscard]] std::string_view Name() const override { return "Inspector"; }
  void Draw(const PanelContext& context) override;

  // Opens the waypoint canvas of |guid| (the "Set New Path" button).
  void OpenCanvas(const Guid& guid) { canvases_[guid].open = true; }
  [[nodiscard]] std::optional<CanvasView> CanvasViewOf(const Guid& guid) const;

 private:
  struct Canvas {
    bool open = false;
    CanvasView view;
  };

  void DrawEntity(const PanelContext& context, EditSink& sink,
                  entt::entity entity, Guid guid);
  void DrawComponent(const DrawTarget& target, const ComponentInfo& info);
  void DrawPath(const DrawTarget& target);
  void DrawCanvasWindow(const DrawTarget& target, const PanelContext& context,
                        Canvas& canvas);

  ReflectionDrawer reflection_;
  StatsDrawer stats_;
  EffectSourceDrawer effects_{&stats_};
  TextEdits text_;
  std::map<Guid, Canvas> canvases_;
};

class ViewportPanel final : public Panel, public Probed {
 public:
  // |settings| outlives the panel.
  explicit ViewportPanel(ViewSettings* settings) noexcept
      : settings_(settings) {}
  // The panel shares the settings' ownership (MakeDefaultPanels).
  explicit ViewportPanel(std::shared_ptr<ViewSettings> settings) noexcept
      : owned_settings_(std::move(settings)),
        settings_(owned_settings_.get()) {}

  [[nodiscard]] std::string_view Name() const override { return "Viewport"; }
  void Draw(const PanelContext& context) override;

  // The editor camera (set on the first Draw from the World's view).
  [[nodiscard]] const std::optional<Camera2D>& EditorCamera() const noexcept {
    return camera_;
  }
  void SetEditorCamera(const Camera2D& camera) noexcept { camera_ = camera; }
  // The last Draw's layout.
  [[nodiscard]] const ViewportMapping& Mapping() const noexcept {
    return mapping_;
  }
  [[nodiscard]] const PickResult& LastPick() const noexcept {
    return last_pick_;
  }

 private:
  struct Drag {
    GizmoAxis axis = GizmoAxis::kNone;
    Guid guid;
    Vec2 start_pos;
    Vec2 start_mouse;
  };

  void HandleInput(const PanelContext& context, EditSink& sink);
  void DrawOverlays(const PanelContext& context);
  [[nodiscard]] std::optional<Vec2> GizmoOrigin(const PanelContext& context,
                                                Guid* guid) const;

  std::shared_ptr<ViewSettings> owned_settings_;
  ViewSettings* settings_;
  std::optional<Camera2D> camera_;
  ViewportMapping mapping_;
  PickScratch scratch_;
  PickResult last_pick_;
  Overlays overlays_;
  std::optional<Drag> drag_;
};

// The legacy log panel's colours for a level (Editor.cpp:401-410).
[[nodiscard]] ImVec4 LogLevelColor(LogLevel level) noexcept;

class LogPanel final : public Panel, public Probed {
 public:
  [[nodiscard]] std::string_view Name() const override { return "Log"; }
  void Draw(const PanelContext& context) override;

  struct Shown {
    std::size_t lines = 0;
    // The indices (LogView::At) of the first and last line drawn.
    std::size_t first = 0;
    std::size_t last = 0;
    std::string counts;
  };
  [[nodiscard]] const Shown& LastShown() const noexcept { return shown_; }

 private:
  bool auto_scroll_ = true;
  int min_level_ = 0;
  Shown shown_;
};

class ProfilerPanel final : public Panel, public Probed {
 public:
  explicit ProfilerPanel(std::function<void()> dump_flight_recorder = {})
      : dump_(std::move(dump_flight_recorder)) {}

  [[nodiscard]] std::string_view Name() const override { return "Profiler"; }
  void Draw(const PanelContext& context) override;

  // The summary of each system's graph in the last Draw (tests).
  [[nodiscard]] const std::vector<std::pair<std::string, SeriesSummary>>&
  Series() const noexcept {
    return series_;
  }
  [[nodiscard]] bool SlowdownShown() const noexcept { return slowdown_; }

 private:
  void DrawSystem(const SystemStats& stats, std::size_t index);

  std::function<void()> dump_;
  std::vector<float> values_;
  std::vector<std::pair<std::string, SeriesSummary>> series_;
  bool slowdown_ = false;
};

class SchedulePanel final : public Panel, public Probed {
 public:
  [[nodiscard]] std::string_view Name() const override { return "Schedule"; }
  void Draw(const PanelContext& context) override;
};

class StressPanel final : public Panel, public Probed {
 public:
  [[nodiscard]] std::string_view Name() const override {
    return "Stress generator";
  }
  void Draw(const PanelContext& context) override;

  // Spawns still to submit in Edit mode (one batch per frame).
  [[nodiscard]] std::size_t Pending() const noexcept;
  [[nodiscard]] std::uint32_t Presses() const noexcept { return press_; }
  void SetAmount(int amount) noexcept { amount_ = amount; }
  void SetPerTick(int per_tick) noexcept { per_tick_ = per_tick; }

 private:
  struct Job {
    std::vector<StressSpawn> spawns;
    std::vector<SpawnBatch> batches;
    std::size_t next = 0;
    std::string label;
  };

  void Generate(const PanelContext& context, EditSink& sink);
  void RemoveEnemies(const PanelContext& context, EditSink& sink);
  void SubmitBatch(EditSink& sink, Job& job, std::size_t batch,
                   Tick apply_before);

  int amount_ = 60;
  int per_tick_ = static_cast<int>(kStressDefaultPerTick);
  std::uint32_t press_ = 0;
  std::optional<Job> job_;
};

class SceneSettingsPanel final : public Panel, public Probed {
 public:
  // |settings| outlives the panel.
  explicit SceneSettingsPanel(ViewSettings* settings) noexcept
      : settings_(settings) {}
  // The panel shares the settings' ownership (MakeDefaultPanels).
  explicit SceneSettingsPanel(std::shared_ptr<ViewSettings> settings) noexcept
      : owned_settings_(std::move(settings)),
        settings_(owned_settings_.get()) {}

  [[nodiscard]] std::string_view Name() const override {
    return "Scene settings";
  }
  void Draw(const PanelContext& context) override;

 private:
  std::shared_ptr<ViewSettings> owned_settings_;
  ViewSettings* settings_;
};

// A new entity Guid for |world| (one it does not hold), from a fixed hash
// of the World's name, |purpose| and a counter.
[[nodiscard]] Guid NewEntityGuid(const World& world, std::string_view purpose);

struct PanelOptions {
  // The Profiler's flight-recorder button (null: the button is disabled).
  std::function<void()> dump_flight_recorder;
};

// The panels the editor docks, in their dock order, with the settings the
// Viewport and Scene settings panels share.
class DefaultPanels {
 public:
  explicit DefaultPanels(PanelOptions options = {});
  DefaultPanels(const DefaultPanels&) = delete;
  DefaultPanels& operator=(const DefaultPanels&) = delete;

  [[nodiscard]] std::span<const std::unique_ptr<Panel>> All() const noexcept {
    return panels_;
  }
  [[nodiscard]] ViewSettings& View() noexcept { return view_; }
  // Attaches |probe| to every panel (tests).
  void SetProbe(WidgetProbe* probe) noexcept;

  [[nodiscard]] HierarchyPanel& Hierarchy() noexcept { return *hierarchy_; }
  [[nodiscard]] InspectorPanel& Inspector() noexcept { return *inspector_; }
  [[nodiscard]] ViewportPanel& Viewport() noexcept { return *viewport_; }
  [[nodiscard]] LogPanel& Log() noexcept { return *log_; }
  [[nodiscard]] ProfilerPanel& Profiler() noexcept { return *profiler_; }
  [[nodiscard]] SchedulePanel& Schedule() noexcept { return *schedule_; }
  [[nodiscard]] StressPanel& Stress() noexcept { return *stress_; }
  [[nodiscard]] SceneSettingsPanel& SceneSettings() noexcept {
    return *scene_settings_;
  }

 private:
  ViewSettings view_;
  std::vector<std::unique_ptr<Panel>> panels_;
  HierarchyPanel* hierarchy_ = nullptr;
  InspectorPanel* inspector_ = nullptr;
  ViewportPanel* viewport_ = nullptr;
  LogPanel* log_ = nullptr;
  ProfilerPanel* profiler_ = nullptr;
  SchedulePanel* schedule_ = nullptr;
  StressPanel* stress_ = nullptr;
  SceneSettingsPanel* scene_settings_ = nullptr;
};

// The same panels, in the same order, as Panels the caller owns: what the
// editor executable hands to the editor shell, one by one
// (Editor::AddPanel). The Viewport and Scene settings panels share the
// ViewSettings between them and keep it alive.
[[nodiscard]] std::vector<std::unique_ptr<Panel>> MakeDefaultPanels(
    PanelOptions options = {});

}  // namespace se::editor

#endif  // SE_EDITOR_PANELS_PANELS_H_
