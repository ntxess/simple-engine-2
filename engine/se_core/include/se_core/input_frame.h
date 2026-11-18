// Copyright 2026 The simple-engine-2 Authors.
//
// The input of one simulation tick. Systems see input only through an
// InputFrame passed by value: held action bits, per-tick press and release
// counts, axes, and up to kMaxEdgesPerTick ordered, timestamped edges. A
// replay is the sequence of these frames.

#ifndef SE_CORE_INPUT_FRAME_H_
#define SE_CORE_INPUT_FRAME_H_

#include <array>
#include <cstdint>
#include <type_traits>

#include "se_core/types.h"

namespace se {

using ActionId = std::uint8_t;

inline constexpr int kMaxActions = 64;
inline constexpr int kMaxAxes = 8;
inline constexpr int kMaxEdgesPerTick = 32;

// Where an edge's stamp came from (docs/design/mt-design.md "Timestamp
// sources"); replays keep it. Values are stored in replay files: append
// only.
enum class EdgeSource : std::uint8_t {
  // SDL's raw-input thread (SDL_HINT_WINDOWS_RAW_KEYBOARD=1), SDL_GetTicksNS.
  kRawKeyboard,
  // Win32 message times (the editor, raw keyboard off).
  kWinMessage,
  // The input sampler's SDL_UpdateGamepads (about 1 ms).
  kGamepadSampler,
  kMouse,
  // Synthesised without a platform (tests, legacy inputs).
  kReplay,
  // A release the engine made when it cleared held input (focus loss, the
  // Windows modal loop), stamped when it cleared (M8).
  kCleared,
};
inline constexpr EdgeSource kLastEdgeSource = EdgeSource::kCleared;

struct InputEdge {
  ActionId action = 0;
  bool down = false;
  // When the edge happened, on the platform's monotonic clock.
  std::uint64_t stamp_ns = 0;
  // Order among edges with the same stamp.
  std::uint8_t order = 0;
  EdgeSource source = EdgeSource::kReplay;

  friend bool operator==(const InputEdge&, const InputEdge&) = default;
};

struct InputFrame {
  Tick tick = 0;
  // Bit i: action i is held at the end of the tick's window.
  std::uint64_t held = 0;
  // Presses and releases of each action within the window.
  std::array<std::uint8_t, kMaxActions> pressed{};
  std::array<std::uint8_t, kMaxActions> released{};
  std::array<float, kMaxAxes> axes{};
  std::uint8_t edge_count = 0;
  std::array<InputEdge, kMaxEdgesPerTick> edges{};
  // False for frames synthesised without a live platform (replays, tests).
  bool platform_live = false;

  [[nodiscard]] constexpr bool Held(ActionId action) const noexcept {
    return action < kMaxActions && ((held >> action) & 1U) != 0;
  }

  friend bool operator==(const InputFrame&, const InputFrame&) = default;
};

static_assert(std::is_trivially_copyable_v<InputFrame>,
              "InputFrame is copied by value into rings and replays");

// Produces the InputFrame for a tick. tick_window_end_ns is the end of the
// tick's input window (FixedStepper's StepPlan::tick_window_end_ns);
// last_tick_of_frame says edges stamped later belong to this tick too.
class InputSource {
 public:
  virtual ~InputSource() = default;
  virtual InputFrame Latch(Tick tick, std::uint64_t tick_window_end_ns,
                           bool last_tick_of_frame) = 0;
};

}  // namespace se

#endif  // SE_CORE_INPUT_FRAME_H_
