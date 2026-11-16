// Copyright 2026 The simple-engine-2 Authors.
//
// EditorStats: what the profiler and schedule panels show, gathered by the
// editor once per frame after the frame's ticks (docs/milestone-specs.md
// M10, the panel interface and the Profiler and Schedule panels; ADR-095).
// The pointers and spans are valid for the frame's Draw calls only; any
// may be null or empty (a headless editor has no pacer or renderer, and
// headless panel tests fill only what they show).

#ifndef SE_EDITOR_EDITOR_STATS_H_
#define SE_EDITOR_EDITOR_STATS_H_

#include <cstdint>
#include <span>
#include <string_view>

#include "se_core/jobs.h"
#include "se_core/schedule.h"
#include "se_core/time.h"
#include "se_core/types.h"
#include "se_render/renderer_2d.h"
#include "se_runtime/frame_pacer.h"
#include "se_runtime/pacer_policy.h"

namespace se::editor {

// One fork site's state (the pool's record: ForkJoinPool::Sites(); sites
// ship disabled).
struct ForkSiteStats {
  std::string_view name;
  bool enabled = false;
  // The site runs inline after a slow join until this time (ns on the
  // pool's clock; 0: no fallback window is open).
  std::uint64_t inline_until_ns = 0;
  jobs::ForkStats stats;
};

struct EditorStats {
  // The editor's frame (the App's iterate).
  std::uint64_t frame = 0;
  // The World the panels see: its last tick, and the play session's next.
  Tick tick = 0;
  Tick next_tick = 0;

  // Pacing: the FramePacer's mode and why, cadence and budget from the
  // frame's StepPlan, presentation interval (2 in HalfRate), frame latency,
  // the display's refresh, misses.
  PacingMode pacing_mode = PacingMode::kStandard;
  std::string_view pacing_reason;
  int present_interval = 1;
  int frame_latency = 1;
  bool half_rate = false;
  double display_hz = 0.0;
  const PacerFrame* pacer = nullptr;
  const PacerTotals* pacer_totals = nullptr;
  // This frame's plan: steps, cadence, alpha, B, overload, debt, dilation.
  StepPlan plan;
  const StepperStats* stepper = nullptr;
  // Editor Play's Overloaded floor of 0 ticks is in force (the simulation
  // dilates, counted, while the UI keeps its rate).
  bool editor_play = false;
  // The 60 Hz preview toggle.
  bool preview_60hz = false;
  // Idle: the editor waits for events (Edit mode, no input for 1 s).
  bool idle = false;

  // The module's systems: per-system timing rings (built-ins first).
  std::span<const SystemStats> systems;
  // Phase totals of the last tick (ns), by Phase: the systems' time, of
  // which phase_parallel_ns was spent inside threaded forks (publish to
  // join; 0 while every fork site runs inline). The serial time of a phase
  // is the difference. phase_parallel_ns is empty when it is not measured.
  std::span<const std::uint64_t> phase_ns;
  std::span<const std::uint64_t> phase_parallel_ns;

  // The renderer's last frame and totals.
  const Renderer2DStats* render = nullptr;
  const Renderer2DTotals* render_totals = nullptr;

  // The pool's fork sites, and its workers (0 until a site is enabled).
  std::span<const ForkSiteStats> fork_sites;
  int fork_workers = 0;
};

}  // namespace se::editor

#endif  // SE_EDITOR_EDITOR_STATS_H_
