// Copyright 2026 The simple-engine-2 Authors.
//
// The frame transaction's fault hook (docs/milestone-specs.md M7, test
// hooks; ADR-062): `--inject-frame-fault STEP[:EVERY[:UNTIL]]` makes STEP
// fail on every EVERY-th frame (default 1), up to frame UNTIL (default: no
// end), counting frames as the App's iterates. Build-phase steps throw a
// standard-library exception (engine code never writes `throw`); recording
// steps make the SDL call look failed: a NULL command buffer or pass, a
// NULL swapchain texture, a failed submit. The recorder consults Fires() at
// each step; nothing fires unless a fault is configured, which only test
// builds' AppConfig does.

#ifndef SE_RENDER_FRAME_FAULTS_H_
#define SE_RENDER_FRAME_FAULTS_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

#include "se_core/error.h"

namespace se {

// The steps of the frame transaction, in order.
enum class FrameStep : std::uint8_t {
  kNone,
  // Build phase (may throw): the RenderLists, then the vertices.
  kBuild,
  kVertices,
  // The upload command buffer.
  kUpload,
  // The frame command buffer.
  kPrepare,
  // The vertex copy pass.
  kCopyPass,
  // Offscreen viewport passes and the composite.
  kViewportPass,
  // The swapchain acquire, then its pass (the blit).
  kAcquire,
  kSwapchainPass,
  kSubmit,
};
inline constexpr std::size_t kFrameSteps = 10;

[[nodiscard]] std::string_view FrameStepName(FrameStep step) noexcept;
[[nodiscard]] std::optional<FrameStep> ParseFrameStep(
    std::string_view name) noexcept;

struct FrameFaultConfig {
  FrameStep step = FrameStep::kNone;
  std::uint64_t every = 1;
  // The last frame that faults; 0: no end.
  std::uint64_t until = 0;
};

// Parses STEP[:EVERY[:UNTIL]].
[[nodiscard]] std::expected<FrameFaultConfig, Error> ParseFrameFault(
    std::string_view text);

class FrameFaults {
 public:
  void Configure(const FrameFaultConfig& config) noexcept { config_ = config; }
  [[nodiscard]] const FrameFaultConfig& Config() const noexcept {
    return config_;
  }
  // The frame (the App's iterate) the next steps belong to.
  void SetFrame(std::uint64_t frame) noexcept { frame_ = frame; }
  // True when |step| fails in this frame; counts it.
  [[nodiscard]] bool Fires(FrameStep step) noexcept;
  [[nodiscard]] std::uint64_t Injected(FrameStep step) const noexcept {
    return injected_[static_cast<std::size_t>(step)];
  }
  [[nodiscard]] std::uint64_t InjectedTotal() const noexcept;

 private:
  FrameFaultConfig config_;
  std::uint64_t frame_ = 0;
  std::array<std::uint64_t, kFrameSteps> injected_{};
};

// Throws the standard library's std::out_of_range (the build-phase fault).
void ThrowInjectedFault(FrameStep step);

}  // namespace se

#endif  // SE_RENDER_FRAME_FAULTS_H_
