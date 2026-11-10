// Copyright 2026 The simple-engine-2 Authors.
//
// InputService: the InputSource of the runtime (docs/milestone-specs.md M8;
// docs/design/mt-design.md "Input And Latency"; ADR-073). It replaces M5's
// BasicInputService. SDL_AppEvent hands it every key, mouse button, gamepad
// button and axis event with its SDL timestamp (HandleEvent); it keeps the
// physical inputs that are down, the ordered, stamped changes not yet
// latched, the latched held state and the axes. No system polls the
// keyboard.
//
// Latch(tick, window_end, last_tick) builds the tick's InputFrame with the
// placement rule (se_core/edge_placement.h): it takes the changes stamped
// before the tick's window end (every change on the frame's last tick),
// applies them in (stamp, arrival) order to the held state, and records
// each action that went down or up as an InputEdge {action, down, stamp,
// order among edges of the same stamp, source}. Changes that arrive after
// their tick was latched go to the next latch. Up to kMaxEdgesPerTick edges
// are kept per frame; the pressed and released counts and the held bits
// count every edge.
//
// Sources: keys carry the configured key source (kRawKeyboard when SDL's
// raw keyboard stamps them, else kWinMessage), mouse buttons kMouse,
// gamepads kGamepadSampler. ClearHeld (focus loss, the modal loop) releases
// every physical input that is down with a change stamped at the clear,
// source kCleared, and zeroes the axes. Gamepad half axes press when the
// stick passes axis_press and release below axis_release.
//
// While nothing latches (a paused game), the oldest changes fold into the
// held state without edges once more than kMaxPending wait.
//
// Main thread only.

#ifndef SE_RUNTIME_INPUT_SERVICE_H_
#define SE_RUNTIME_INPUT_SERVICE_H_

#include <SDL3/SDL_events.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "se_core/input_frame.h"
#include "se_core/types.h"
#include "se_runtime/action_map.h"

namespace se {

struct InputServiceConfig {
  // The source of key stamps.
  EdgeSource key_source = EdgeSource::kRawKeyboard;
  // Gamepad half axes: pressed at this deflection, released below the
  // lower one.
  float axis_press = 0.5F;
  float axis_release = 0.35F;
};

struct InputServiceStats {
  std::uint64_t events = 0;
  std::uint64_t edges = 0;
  // Edges beyond kMaxEdgesPerTick in one frame (counted, not stored).
  std::uint64_t edges_not_stored = 0;
  // Changes folded into the held state without edges (nothing latched).
  std::uint64_t changes_folded = 0;
  // Changes latched after the tick their stamp belonged to.
  std::uint64_t late_changes = 0;
  std::uint64_t clears = 0;
};

class InputService final : public InputSource {
 public:
  static constexpr std::size_t kMaxPending = 1024;

  InputService(ActionMap map, InputServiceConfig config);

  // An SDL event on main: keys (repeats ignored), mouse buttons, gamepad
  // buttons, axes and removal. Returns true when it was one of those.
  bool HandleEvent(const SDL_Event& event);

  // The same, from values (tests, tools).
  void OnKey(std::uint32_t keycode, bool down, bool repeat,
             std::uint64_t stamp_ns);
  void OnMouseButton(std::uint8_t button, bool down, std::uint64_t stamp_ns);
  void OnPadButton(std::uint32_t pad, std::uint8_t button, bool down,
                   std::uint64_t stamp_ns);
  // |value| in [-1, 1] (triggers [0, 1]).
  void OnPadAxis(std::uint32_t pad, std::uint8_t axis, float value,
                 std::uint64_t stamp_ns);
  // Releases everything |pad| holds.
  void OnPadRemoved(std::uint32_t pad, std::uint64_t stamp_ns);
  // Releases every held input (focus loss, the modal loop).
  void ClearHeld(std::uint64_t stamp_ns);

  InputFrame Latch(Tick tick, std::uint64_t tick_window_end_ns,
                   bool last_tick_of_frame) override;

  // The held bits as of the last latch.
  [[nodiscard]] std::uint64_t Held() const noexcept { return held_; }
  // Changes waiting for a latch.
  [[nodiscard]] std::size_t Pending() const noexcept { return pending_.size(); }
  [[nodiscard]] const InputServiceStats& Stats() const noexcept {
    return stats_;
  }
  [[nodiscard]] const ActionMap& Map() const noexcept { return map_; }

 private:
  enum class Device : std::uint8_t { kKey, kMouse, kPadButton, kPadAxis };
  struct Physical {
    Device device = Device::kKey;
    std::uint32_t pad = 0;
    // A keycode, a mouse or pad button, or axis * 2 + positive.
    std::uint32_t code = 0;
    friend bool operator==(const Physical&, const Physical&) = default;
  };
  struct Down {
    Physical input;
    std::uint64_t actions = 0;
    EdgeSource source = EdgeSource::kReplay;
  };
  struct Change {
    std::uint64_t stamp_ns = 0;
    std::uint64_t seq = 0;
    std::uint64_t actions = 0;
    bool down = false;
    EdgeSource source = EdgeSource::kReplay;
  };
  struct AxisSample {
    std::uint64_t stamp_ns = 0;
    std::uint64_t seq = 0;
    std::uint8_t axis = 0;
    float value = 0.0F;
  };

  void Press(const Physical& input, std::uint64_t actions, EdgeSource source,
             std::uint64_t stamp_ns);
  void Release(const Physical& input, std::uint64_t stamp_ns,
               EdgeSource source);
  void Queue(const Change& change);
  // Applies |change| to the held state; adds its edges to |frame| when one
  // is given.
  void Apply(const Change& change, InputFrame* frame) noexcept;
  void FoldOldest();

  ActionMap map_;
  InputServiceConfig config_;
  std::vector<Down> down_;
  std::vector<Change> pending_;
  std::vector<Change> taken_;
  std::vector<AxisSample> axis_pending_;
  std::uint64_t seq_ = 0;
  // Latched state.
  std::uint64_t held_ = 0;
  std::array<std::uint16_t, kMaxActions> holders_{};
  std::array<float, kMaxAxes> axes_{};
  // The latest stamp latched (late changes are counted against it).
  std::uint64_t last_window_end_ns_ = 0;
  InputServiceStats stats_;
};

}  // namespace se

#endif  // SE_RUNTIME_INPUT_SERVICE_H_
