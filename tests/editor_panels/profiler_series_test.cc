// Copyright 2026 The simple-engine-2 Authors.
//
// The profiler graphs' data: the ported axis rule max(avg * 1.5,
// peak * 1.1, 1.0) (app/editor/EditorSceneSystemVisitor.cpp:29) and the
// series read from a Schedule system's ring, oldest first, in milliseconds.

#include "se_editor_panels/profiler_series.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "se_core/schedule.h"

namespace se::editor {
namespace {

using Catch::Approx;

TEST_CASE("The axis rule takes the largest of 1.5 avg, 1.1 peak and 1 ms",
          "[editor][profiler]") {
  // Quiet systems plot against 1 ms.
  REQUIRE(ProfilerAxisMax(0.0F, 0.0F) == 1.0F);
  REQUIRE(ProfilerAxisMax(0.5F, 0.8F) == 1.0F);
  // A steady system: the average decides.
  REQUIRE(ProfilerAxisMax(2.0F, 2.5F) == 2.0F * 1.5F);
  // A spike: the peak decides.
  REQUIRE(ProfilerAxisMax(2.0F, 10.0F) == 10.0F * 1.1F);
}

TEST_CASE("A series summary has the average, peak, last value and axis",
          "[editor][profiler]") {
  const std::vector<float> values{1.0F, 3.0F, 2.0F, 6.0F, 3.0F};
  const SeriesSummary summary = SummarizeSeries(values);
  REQUIRE(summary.avg_ms == Approx(3.0));
  REQUIRE(summary.peak_ms == 6.0F);
  REQUIRE(summary.current_ms == 3.0F);
  REQUIRE(summary.axis_max_ms == Approx(6.6));
  const SeriesSummary empty = SummarizeSeries({});
  REQUIRE(empty.avg_ms == 0.0F);
  REQUIRE(empty.axis_max_ms == 1.0F);
}

TEST_CASE("A system that never ran plots ten zeros against 1 ms",
          "[editor][profiler]") {
  const SystemStats stats;
  std::vector<float> values{4.0F};
  const SeriesSummary summary = SystemSeries(stats, values);
  REQUIRE(values == std::vector<float>(kProfilerEmptySamples, 0.0F));
  REQUIRE(summary.axis_max_ms == 1.0F);
}

TEST_CASE("The series is the newest runs of the ring in ms, oldest first",
          "[editor][profiler]") {
  SystemStats stats;
  // Wrap the 1024-entry ring: runs 0..1099 take 1000 ns * (i % 7 + 1).
  for (std::uint64_t i = 0; i < 1100; ++i) stats.Record(1000 * (i % 7 + 1));
  std::vector<float> values;
  const SeriesSummary summary = SystemSeries(stats, values, 120);
  REQUIRE(values.size() == 120);
  for (std::size_t k = 0; k < values.size(); ++k) {
    const std::uint64_t run = 1100 - 120 + k;
    CHECK(values[k] == Approx(1e-3 * static_cast<double>(run % 7 + 1)));
  }
  REQUIRE(summary.current_ms == Approx(1e-3 * (1099 % 7 + 1)));
  REQUIRE(summary.peak_ms == Approx(7e-3));
  REQUIRE(summary.axis_max_ms == 1.0F);
  // A history longer than the runs shows every run.
  SystemStats few;
  few.Record(2'000'000);
  few.Record(4'000'000);
  REQUIRE(SystemSeries(few, values).axis_max_ms == Approx(4.5));
  REQUIRE(values.size() == 2);
  REQUIRE(values[0] == Approx(2.0));
  REQUIRE(values[1] == Approx(4.0));
}

}  // namespace
}  // namespace se::editor
