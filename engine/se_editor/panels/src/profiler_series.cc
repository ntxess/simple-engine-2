// Copyright 2026 The simple-engine-2 Authors.

#include "se_editor_panels/profiler_series.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "se_core/schedule.h"

namespace se::editor {

float ProfilerAxisMax(float avg_ms, float peak_ms) noexcept {
  return std::max({avg_ms * 1.5F, peak_ms * 1.1F, 1.0F});
}

SeriesSummary SummarizeSeries(std::span<const float> values_ms) noexcept {
  SeriesSummary summary;
  if (values_ms.empty()) return summary;
  float sum = 0.0F;
  for (const float value : values_ms) {
    sum += value;
    summary.peak_ms = std::max(summary.peak_ms, value);
  }
  summary.avg_ms = sum / static_cast<float>(values_ms.size());
  summary.current_ms = values_ms.back();
  summary.axis_max_ms = ProfilerAxisMax(summary.avg_ms, summary.peak_ms);
  return summary;
}

SeriesSummary SystemSeries(const SystemStats& stats,
                           std::vector<float>& values_ms, std::size_t history) {
  values_ms.clear();
  const std::size_t count =
      std::min<std::size_t>({stats.count, history, kSystemRingSize});
  if (count == 0) {
    values_ms.assign(kProfilerEmptySamples, 0.0F);
    return SummarizeSeries({});
  }
  // head is the next slot Record writes, so the newest run is head - 1.
  const std::size_t first =
      (stats.head + kSystemRingSize - count) % kSystemRingSize;
  for (std::size_t i = 0; i < count; ++i) {
    const std::uint32_t ns = stats.ring_ns[(first + i) % kSystemRingSize];
    values_ms.push_back(static_cast<float>(ns) * 1e-6F);
  }
  return SummarizeSeries(values_ms);
}

}  // namespace se::editor
