// Copyright 2026 The simple-engine-2 Authors.
//
// Live input for the dedicated sim thread (docs/design/dedicated-driver.md
// "Data Flow", InputEvent, and "Input And Latency", FOLDING; ADR-084).
//
// The platform routes each input event to an action, stamps it on the
// engine's monotonic clock at pump time and pushes it, by value, on the
// InputEvent ring (4096 entries; when it is full the platform coalesces
// axis events and counts the drop, never blocking). The sim's InputFolder is
// then the only holder of game input state: it builds InputFrame k from
// every event stamped at or before the tick's latch time L_k, which is the
// inline driver's placement rule (an edge goes to the first tick whose
// window ends at or after its stamp, M8) with the sim's own deadlines as the
// window ends. It peeks the ring's front and pops only those events: the
// first later one, and everything behind it, waits for the next tick. A
// burst of catch-up ticks latches each tick at its own deadline. Since the
// windows end at the deadlines, there is no frame remainder for a last tick
// to take, so InputSource's last_tick_of_frame has no counterpart here.
//
// Folding, per event in ring order:
//   kPress, kRelease  the held bit, the press or release count (saturating
//                     at 255, so a tap shorter than a tick is never lost) and
//                     an InputEdge (stamp, source, order among same-stamp
//                     edges); edges beyond kMaxEdgesPerTick are counted
//   kAxis             axes[action] = value (the latest wins)
//   kReleaseAll       every held action is released (counted, with edges)
//   kHeartbeat        nothing but liveness (counted)
// Every live frame has platform_live set.

#ifndef SE_SIM_INPUT_FOLDER_H_
#define SE_SIM_INPUT_FOLDER_H_

#include <array>
#include <cstdint>
#include <type_traits>

#include "se_core/input_frame.h"
#include "se_core/spsc_queue.h"
#include "se_core/types.h"

namespace se {

enum class InputKind : std::uint8_t {
  kPress,
  kRelease,
  kAxis,
  kReleaseAll,
  kHeartbeat,
};

struct InputEvent {
  // The engine's monotonic clock at pump time: decides the tick.
  std::uint64_t stamp_ns = 0;
  // The device's own stamp (SDL's), for diagnostics only.
  std::uint64_t device_ns = 0;
  // The action (kPress, kRelease) or the axis index (kAxis).
  ActionId action = 0;
  InputKind kind = InputKind::kPress;
  EdgeSource source = EdgeSource::kWinMessage;
  // The axis value (kAxis).
  float value = 0.0F;
};

static_assert(std::is_trivially_copyable_v<InputEvent>);

inline constexpr std::uint32_t kInputEventRingCapacity = 4096;
using InputEventRing = SpscQueue<InputEvent>;

struct InputFolderStats {
  std::uint64_t events = 0;
  std::uint64_t heartbeats = 0;
  // Edges that did not fit a frame's kMaxEdgesPerTick (still counted in
  // pressed and released).
  std::uint64_t edges_dropped = 0;
  // Axis events naming an axis past kMaxAxes, or an action past kMaxActions.
  std::uint64_t ignored = 0;
  // The newest stamp folded so far.
  std::uint64_t last_stamp_ns = 0;
};

class InputFolder {
 public:
  // InputFrame |tick|: every event at the ring's front stamped at or before
  // |latch_ns|, folded onto the held state the folder keeps.
  [[nodiscard]] InputFrame Latch(Tick tick, std::uint64_t latch_ns,
                                 InputEventRing& ring) noexcept;
  // Forgets the held state and axes (a new session).
  void Reset() noexcept;

  [[nodiscard]] std::uint64_t Held() const noexcept { return held_; }
  [[nodiscard]] const InputFolderStats& Stats() const noexcept {
    return stats_;
  }

 private:
  void Edge(InputFrame& frame, ActionId action, bool down,
            const InputEvent& event) noexcept;

  std::uint64_t held_ = 0;
  std::array<float, kMaxAxes> axes_{};
  InputFolderStats stats_;
};

}  // namespace se

#endif  // SE_SIM_INPUT_FOLDER_H_
