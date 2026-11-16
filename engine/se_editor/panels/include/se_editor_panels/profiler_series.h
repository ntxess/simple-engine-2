// Copyright 2026 The simple-engine-2 Authors.
//
// The data behind the Profiler panel's per-system graphs, ported from the
// legacy performance panel (app/editor/EditorSceneSystemVisitor.cpp:5-29):
// the last kProfilerHistory runs of a system in milliseconds, oldest first,
// their average, peak and latest value, and the graph's axis rule
//
//   axis max = max(avg * 1.5, peak * 1.1, 1.0)   (milliseconds)
//
// (EditorSceneSystemVisitor.cpp:29). A system with no runs plots ten zeros
// against an axis of 1 ms, as the legacy panel did (:11). The samples come
// from the Schedule's per-system rings (se_core/schedule.h), recorded in
// nanoseconds.

#ifndef SE_EDITOR_PANELS_PROFILER_SERIES_H_
#define SE_EDITOR_PANELS_PROFILER_SERIES_H_

#include <cstddef>
#include <span>
#include <vector>

#include "se_core/schedule.h"

namespace se::editor {

// The legacy SystemProfiler's history length (core/util/SystemProfiler.hpp).
inline constexpr std::size_t kProfilerHistory = 120;
// What an empty history plots (EditorSceneSystemVisitor.cpp:11).
inline constexpr std::size_t kProfilerEmptySamples = 10;

// The legacy axis rule.
[[nodiscard]] float ProfilerAxisMax(float avg_ms, float peak_ms) noexcept;

struct SeriesSummary {
  float avg_ms = 0.0F;
  float peak_ms = 0.0F;
  float current_ms = 0.0F;
  float axis_max_ms = 1.0F;
};

// Summarises |values_ms| (oldest first); an empty span is all zeros.
[[nodiscard]] SeriesSummary SummarizeSeries(
    std::span<const float> values_ms) noexcept;

// Replaces |values_ms| with the last |history| runs of |stats| in
// milliseconds, oldest first (kProfilerEmptySamples zeros when it never
// ran), and summarises them.
SeriesSummary SystemSeries(const SystemStats& stats,
                           std::vector<float>& values_ms,
                           std::size_t history = kProfilerHistory);

}  // namespace se::editor

#endif  // SE_EDITOR_PANELS_PROFILER_SERIES_H_
