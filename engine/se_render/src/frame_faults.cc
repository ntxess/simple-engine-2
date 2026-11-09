// Copyright 2026 The simple-engine-2 Authors.

#include "se_render/frame_faults.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "se_core/error.h"
#include "se_core/log.h"

namespace se {
namespace {

constexpr std::array<std::string_view, kFrameSteps> kStepNames = {
    "none",      "build",         "vertices", "upload",         "prepare",
    "copy_pass", "viewport_pass", "acquire",  "swapchain_pass", "submit",
};

std::optional<std::uint64_t> ParseCount(std::string_view text) {
  std::uint64_t value = 0;
  const char* first = text.data();
  const char* last = first + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec != std::errc() || ptr != last) return std::nullopt;
  return value;
}

}  // namespace

std::string_view FrameStepName(FrameStep step) noexcept {
  return kStepNames[static_cast<std::size_t>(step)];
}

std::optional<FrameStep> ParseFrameStep(std::string_view name) noexcept {
  for (std::size_t i = 1; i < kStepNames.size(); ++i) {
    if (kStepNames[i] == name) return static_cast<FrameStep>(i);
  }
  return std::nullopt;
}

std::expected<FrameFaultConfig, Error> ParseFrameFault(std::string_view text) {
  FrameFaultConfig config;
  std::string_view rest = text;
  const std::size_t first = rest.find(':');
  const std::string_view step = rest.substr(0, first);
  const std::optional<FrameStep> parsed = ParseFrameStep(step);
  if (!parsed) {
    return Fail(ErrorCode::kInvalidArgument,
                std::format("--inject-frame-fault: unknown step '{}' (build, "
                            "vertices, upload, prepare, copy_pass, "
                            "viewport_pass, acquire, swapchain_pass, submit)",
                            step));
  }
  config.step = *parsed;
  if (first == std::string_view::npos) return config;
  rest = rest.substr(first + 1);
  const std::size_t second = rest.find(':');
  const std::optional<std::uint64_t> every = ParseCount(rest.substr(0, second));
  if (!every || *every == 0) {
    return Fail(ErrorCode::kInvalidArgument,
                std::format("--inject-frame-fault: bad EVERY in '{}'", text));
  }
  config.every = *every;
  if (second == std::string_view::npos) return config;
  const std::optional<std::uint64_t> until =
      ParseCount(rest.substr(second + 1));
  if (!until || *until == 0) {
    return Fail(ErrorCode::kInvalidArgument,
                std::format("--inject-frame-fault: bad UNTIL in '{}'", text));
  }
  config.until = *until;
  return config;
}

bool FrameFaults::Fires(FrameStep step) noexcept {
  if (config_.step == FrameStep::kNone || step != config_.step) return false;
  if (frame_ == 0 || frame_ % config_.every != 0) return false;
  if (config_.until != 0 && frame_ > config_.until) return false;
  ++injected_[static_cast<std::size_t>(step)];
  return true;
}

std::uint64_t FrameFaults::InjectedTotal() const noexcept {
  std::uint64_t total = 0;
  for (const std::uint64_t count : injected_) total += count;
  return total;
}

void ThrowInjectedFault(FrameStep step) {
  // A standard-library throw, the kind the transaction's boundary contains.
  const std::vector<int> empty;
  const int never = empty.at(static_cast<std::size_t>(step));
  SE_LOG_ERROR("the injected {} fault did not throw ({})", FrameStepName(step),
               never);
}

}  // namespace se
