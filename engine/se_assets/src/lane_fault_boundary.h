// Copyright 2026 The simple-engine-2 Authors.
//
// The background lane's fault boundary (ADR-079): a lane job that throws
// (a standard-library throw such as bad_alloc, or glaze's) ends with an
// Error instead of taking the lane thread down (docs/design/mt-design.md
// "Failure Handling": background job exceptions are converted to the
// same Error{code, message, path, line, column}).

#ifndef ENGINE_SE_ASSETS_SRC_LANE_FAULT_BOUNDARY_H_
#define ENGINE_SE_ASSETS_SRC_LANE_FAULT_BOUNDARY_H_

#include <expected>

#include "se_assets/background_lane.h"
#include "se_core/error.h"

namespace se::lane_internal {

[[nodiscard]] std::expected<StepResult, Error> ContainedStep(
    LaneJob& job, LaneContext& lane) noexcept;

[[nodiscard]] std::expected<LanePayload, Error> ContainedResult(
    LaneJob& job) noexcept;

}  // namespace se::lane_internal

#endif  // ENGINE_SE_ASSETS_SRC_LANE_FAULT_BOUNDARY_H_
