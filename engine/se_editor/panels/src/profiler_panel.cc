// Copyright 2026 The simple-engine-2 Authors.
//
// The Profiler and Schedule panels (docs/design/mt-design.md "Editor",
// panels). The per-system graphs port the legacy performance panel
// (app/editor/EditorSceneSystemVisitor.cpp:5-118 at master): a plot against
// the axis rule, the axis's maximum and 0 at its right edge, average, peak
// and latest lines and a tooltip on hover, and the three values stacked
// below.

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "se_core/jobs.h"
#include "se_core/schedule.h"
#include "se_core/time.h"
#include "se_editor/editor_stats.h"
#include "se_editor/panel.h"
#include "se_editor_panels/panels.h"
#include "se_editor_panels/profiler_series.h"
#include "se_render/renderer_2d.h"
#include "se_runtime/frame_pacer.h"
#include "se_runtime/pacer_policy.h"

namespace se::editor {
namespace {

constexpr ImVec4 kRed{1.0F, 0.3F, 0.3F, 1.0F};

double Ms(std::uint64_t ns) { return static_cast<double>(ns) * 1e-6; }

const char* CadenceName(Cadence cadence) {
  return cadence == Cadence::kLocked ? "Locked" : "Free";
}

void Cell(std::uint64_t value) {
  ImGui::TextUnformatted(std::format("{}", value).c_str());
}

void Row(const char* label, const std::string& value) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextUnformatted(label);
  ImGui::TableNextColumn();
  ImGui::TextUnformatted(value.c_str());
}

void DrawForkSites(std::span<const ForkSiteStats> sites, int workers) {
  ImGui::Text("Fork-join pool: %d worker(s)", workers);
  if (sites.empty()) {
    ImGui::TextDisabled("No fork sites (they arrive with M14's pool)");
    return;
  }
  constexpr ImGuiTableFlags kFlags =
      ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders;
  if (!ImGui::BeginTable("##forks", 8, kFlags)) return;
  for (const char* title : {"Site", "State", "Forks", "Inline", "Join p99.9",
                            "Late wakes", "Missed wakes", "Fallbacks"}) {
    ImGui::TableSetupColumn(title);
  }
  ImGui::TableHeadersRow();
  for (const ForkSiteStats& site : sites) {
    const jobs::ForkStats& s = site.stats;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(std::string(site.name).c_str());
    ImGui::TableNextColumn();
    if (!site.enabled) {
      ImGui::TextDisabled("disabled");
    } else if (site.inline_until_ns != 0) {
      ImGui::TextColored(kRed, "inline (fallback)");
    } else {
      ImGui::TextUnformatted("threaded");
    }
    ImGui::TableNextColumn();
    Cell(s.forks);
    ImGui::TableNextColumn();
    Cell(s.inline_forks);
    ImGui::TableNextColumn();
    ImGui::Text("%.1f us", s.join_p999_us);
    ImGui::TableNextColumn();
    Cell(s.late_wakes);
    ImGui::TableNextColumn();
    Cell(s.missed_wakes);
    ImGui::TableNextColumn();
    Cell(s.fallbacks);
  }
  ImGui::EndTable();
}

}  // namespace

void ProfilerPanel::DrawSystem(const SystemStats& stats, std::size_t index) {
  const std::string name(stats.name);
  const SeriesSummary summary = SystemSeries(stats, values_);
  series_.emplace_back(name, summary);
  ImGui::PushID(static_cast<int>(index));
  const std::string title = name + " Profiler";
  if (!ImGui::TreeNode(title.c_str())) {
    ImGui::PopID();
    return;
  }
  const float axis = summary.axis_max_ms;
  const ImVec2 size{ImGui::GetContentRegionAvail().x - 40.0F, 120.0F};
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImGui::PlotLines("##plot", values_.data(), static_cast<int>(values_.size()),
                   0, nullptr, 0.0F, axis, size);
  ProbeItem("profiler.plot." + name);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const auto y_of = [&](float v) { return pos.y + size.y * (1.0F - v / axis); };
  const std::string max_label = std::format("{:.3f}", axis);
  draw->AddText({pos.x + size.x + 2.0F, pos.y}, IM_COL32(255, 255, 255, 255),
                max_label.c_str());
  draw->AddText({pos.x + size.x + 2.0F, pos.y + size.y - 12.0F},
                IM_COL32(255, 255, 255, 255), "0.000");
  if (ImGui::IsItemHovered()) {
    ImGui::BeginTooltip();
    ImGui::Text("Avg:  %.3f ms", summary.avg_ms);
    ImGui::Text("Peak: %.3f ms", summary.peak_ms);
    ImGui::Text("Curr: %.3f ms", summary.current_ms);
    ImGui::EndTooltip();
    draw->AddLine({pos.x, y_of(summary.avg_ms)},
                  {pos.x + size.x, y_of(summary.avg_ms)},
                  IM_COL32(255, 255, 0, 255));
    draw->AddLine({pos.x, y_of(summary.peak_ms)},
                  {pos.x + size.x, y_of(summary.peak_ms)},
                  IM_COL32(255, 0, 0, 255));
    draw->AddLine({pos.x, y_of(summary.current_ms)},
                  {pos.x + size.x, y_of(summary.current_ms)},
                  IM_COL32(0, 255, 0, 255));
  }
  ImGui::Dummy({0.0F, 55.0F});
  const float x = pos.x + 5.0F;
  float y = pos.y + size.y + 5.0F;
  const std::string avg = std::format("Avg:  {:.6f} ms", summary.avg_ms);
  const std::string peak = std::format("Peak: {:.6f} ms", summary.peak_ms);
  const std::string curr = std::format("Curr: {:.6f} ms", summary.current_ms);
  draw->AddText({x, y}, IM_COL32(255, 255, 0, 255), avg.c_str());
  y += 15.0F;
  draw->AddText({x, y}, IM_COL32(255, 0, 0, 255), peak.c_str());
  y += 15.0F;
  draw->AddText({x, y}, IM_COL32(0, 255, 0, 255), curr.c_str());
  ImGui::TreePop();
  ImGui::PopID();
}

void ProfilerPanel::Draw(const PanelContext& context) {
  series_.clear();
  slowdown_ = false;
  const EditorStats* stats = context.stats;
  ImGui::BeginDisabled(!dump_);
  if (ImGui::Button("Dump flight recorder") && dump_) dump_();
  ProbeItem("profiler.dump");
  ImGui::EndDisabled();
  if (stats == nullptr) {
    ImGui::TextDisabled("No stats this frame");
    return;
  }
  const StepPlan& plan = stats->plan;

  // The overload badge: the stepper dropped ticks it could not run in
  // time (Overloaded), so the simulation runs slower than real time.
  slowdown_ = plan.overloaded || plan.dilation_sec > 0.0;
  if (slowdown_) {
    ImGui::SameLine();
    ImGui::TextColored(kRed, "SLOWDOWN");
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Overloaded: %s, dilation %.3f s this frame, debt %u",
                        plan.overloaded ? "yes" : "no", plan.dilation_sec,
                        plan.debt_ticks);
    }
  }
  ProbeItem("profiler.badge");

  if (ImGui::CollapsingHeader("Pacing", ImGuiTreeNodeFlags_DefaultOpen) &&
      ImGui::BeginTable("##pacing", 2, ImGuiTableFlags_RowBg)) {
    Row("Mode", std::format("{} ({})", PacingModeName(stats->pacing_mode),
                            stats->pacing_reason));
    Row("Cadence", std::format("{} (k {}, {} ticks this frame, alpha {:.2f})",
                               CadenceName(plan.cadence), plan.locked_k,
                               plan.steps, plan.alpha));
    Row("Budget B", std::format("{:.3f} ms", plan.budget_b * 1e3));
    Row("Frame latency", std::format("{}", stats->frame_latency));
    Row("Present interval", std::format("{}{}", stats->present_interval,
                                        stats->half_rate ? " (HalfRate)" : ""));
    Row("Display", std::format("{:.2f} Hz", stats->display_hz));
    if (stats->pacer_totals != nullptr) {
      Row("Misses",
          std::format("{} of {} presents", stats->pacer_totals->missed_presents,
                      stats->pacer_totals->resolved_presents));
    }
    if (stats->stepper != nullptr) {
      Row("Dilation", std::format("{:.3f} s ({} ticks), {} overloads",
                                  stats->stepper->debt_dilation_sec,
                                  stats->stepper->dilated_ticks,
                                  stats->stepper->overload_entries));
    }
    Row("Editor Play floor", stats->editor_play ? "0 ticks" : "-");
    Row("Preview 60 Hz", stats->preview_60hz ? "on" : "off");
    Row("Idle", stats->idle ? "yes" : "no");
    ImGui::EndTable();
  }

  if (ImGui::CollapsingHeader("Render") &&
      ImGui::BeginTable("##render", 2, ImGuiTableFlags_RowBg)) {
    if (const Renderer2DStats* r = stats->render) {
      Row("Quads, batches, draws",
          std::format("{}, {}, {}", r->quads, r->batches, r->draws));
      Row("Layers, targets", std::format("{}, {}", r->layers, r->targets));
      Row("Uploads", std::format("{} bytes", r->upload_bytes));
      Row("Build, record", std::format("{:.3f} ms, {:.3f} ms", Ms(r->build_ns),
                                       Ms(r->record_ns)));
      Row("GPU", r->gpu_timed ? std::format("{:.3f} ms", Ms(r->gpu_ns))
                              : std::string("unknown"));
    }
    if (const Renderer2DTotals* t = stats->render_totals) {
      Row("Frames", std::format("{} ({} presented)", t->frames, t->presented));
      Row("Faults",
          std::format("{} build, {} skipped, {} pass", t->build_faults,
                      t->skipped_frames, t->pass_failures));
      Row("Vertex growths", std::format("{}", t->vertex_growths));
    }
    ImGui::EndTable();
  }

  if (ImGui::CollapsingHeader("Fork-join")) {
    DrawForkSites(stats->fork_sites, stats->fork_workers);
  }

  if (ImGui::CollapsingHeader("Systems", ImGuiTreeNodeFlags_DefaultOpen)) {
    for (std::size_t i = 0; i < stats->systems.size(); ++i) {
      DrawSystem(stats->systems[i], i);
    }
  }
}

void SchedulePanel::Draw(const PanelContext& context) {
  const EditorStats* stats = context.stats;
  if (stats == nullptr) {
    ImGui::TextDisabled("No stats this frame");
    return;
  }
  // Serial time per phase: the editor's phase totals of the last tick, or
  // the sum of the phase's systems' last runs. Parallel time is what fork
  // chunks ran on workers; at W = 0 (sites disabled) it is 0.
  bool any_enabled = false;
  for (const ForkSiteStats& site : stats->fork_sites) {
    any_enabled = any_enabled || site.enabled;
  }
  constexpr ImGuiTableFlags kFlags =
      ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders;
  if (ImGui::BeginTable("##phases", 4, kFlags)) {
    ImGui::TableSetupColumn("Phase");
    ImGui::TableSetupColumn("Serial (ms)");
    ImGui::TableSetupColumn("Parallel (ms)");
    ImGui::TableSetupColumn("Systems");
    ImGui::TableHeadersRow();
    for (int p = 0; p < kPhaseCount; ++p) {
      const auto phase = static_cast<Phase>(p);
      std::uint64_t serial = 0;
      int systems = 0;
      for (const SystemStats& s : stats->systems) {
        if (s.phase != phase) continue;
        serial += s.last_ns;
        ++systems;
      }
      if (static_cast<std::size_t>(p) < stats->phase_ns.size()) {
        serial = stats->phase_ns[static_cast<std::size_t>(p)];
      }
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(std::string(PhaseName(phase)).c_str());
      ImGui::TableNextColumn();
      ImGui::Text("%.3f", Ms(serial));
      ImGui::TableNextColumn();
      if (any_enabled) {
        ImGui::TextDisabled("n/a");
      } else {
        ImGui::Text("%.3f", 0.0);
      }
      ImGui::TableNextColumn();
      ImGui::Text("%d", systems);
    }
    ImGui::EndTable();
  }
  ProbeItem("schedule.phases");

  if (ImGui::CollapsingHeader("Systems", ImGuiTreeNodeFlags_DefaultOpen) &&
      ImGui::BeginTable("##systems", 6, kFlags)) {
    for (const char* title :
         {"System", "Phase", "Last (ms)", "p50", "p99", "Max"}) {
      ImGui::TableSetupColumn(title);
    }
    ImGui::TableHeadersRow();
    for (const SystemStats& s : stats->systems) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(std::string(s.name).c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(std::string(PhaseName(s.phase)).c_str());
      ImGui::TableNextColumn();
      ImGui::Text("%.3f", Ms(s.last_ns));
      ImGui::TableNextColumn();
      ImGui::Text("%.3f", Ms(s.PercentileNs(0.5)));
      ImGui::TableNextColumn();
      ImGui::Text("%.3f", Ms(s.PercentileNs(0.99)));
      ImGui::TableNextColumn();
      ImGui::Text("%.3f", Ms(s.max_ns));
    }
    ImGui::EndTable();
  }
  if (ImGui::CollapsingHeader("Fork sites", ImGuiTreeNodeFlags_DefaultOpen)) {
    DrawForkSites(stats->fork_sites, stats->fork_workers);
  }
}

}  // namespace se::editor
