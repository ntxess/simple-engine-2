// Copyright 2026 The simple-engine-2 Authors.
//
// se-fault-boundary: a background lane job that throws ends with an Error
// (ADR-079). Each step and each result runs inside one try; the catch
// turns the exception into ErrorCode::kIo with the job's kind and the
// exception's text, so the job completes failed and the lane carries on.
// Nothing else here catches.

#include "src/lane_fault_boundary.h"

#include <exception>
#include <expected>
#include <format>
#include <string>
#include <utility>

#include "se_assets/background_lane.h"
#include "se_core/error.h"
#include "se_core/log.h"

namespace se::lane_internal {
namespace {

std::unexpected<Error> Faulted(const LaneJob& job, const char* what) {
  SE_LOG_ERROR("lane job '{}' threw: {}", job.Kind(), what);
  return std::unexpected(
      Error{ErrorCode::kIo,
            std::format("lane job '{}' threw: {}", job.Kind(), what),
            {},
            0,
            0});
}

}  // namespace

std::expected<StepResult, Error> ContainedStep(LaneJob& job,
                                               LaneContext& lane) noexcept {
  try {
    return job.Step(lane);
  } catch (const std::exception& error) {
    return Faulted(job, error.what());
  } catch (...) {
    return Faulted(job, "an exception that is not a std::exception");
  }
}

std::expected<LanePayload, Error> ContainedResult(LaneJob& job) noexcept {
  try {
    return job.TakeResult();
  } catch (const std::exception& error) {
    return Faulted(job, error.what());
  } catch (...) {
    return Faulted(job, "an exception that is not a std::exception");
  }
}

}  // namespace se::lane_internal
