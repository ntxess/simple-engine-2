// Copyright 2026 The simple-engine-2 Authors.
//
// The kernel test module: a small game module that exercises every kernel
// path the determinism tests rely on.
//
//   gameplay   48 circular movers (dm::SinCos through a ParallelView fork),
//              a player moved by the InputFrame, a spawner that spawns a
//              child every 30 ticks with World::Spawn (Guids from
//              DeriveGuid) and destroys it 90 ticks later, a follower with
//              an EntityRef, and a rule that requests Replace("game_over")
//              at tick 300.
//   game_over  a banner and one mover.
//
// tests/fixtures/kernel/edits.txt adds logged SetComponent edits at ticks
// 100 and 250. ScriptedInput produces the same InputFrame for a tick
// whatever the frame rate.

#ifndef TESTS_SUPPORT_KERNEL_TEST_MODULE_H_
#define TESTS_SUPPORT_KERNEL_TEST_MODULE_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/component_registry.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/input_frame.h"
#include "se_core/types.h"
#include "se_sim/game_module.h"
#include "se_sim/session_log.h"
#include "support/kernel_test_components.h"

namespace se::testing {

inline constexpr std::uint64_t kSceneHi = 0x00005E5E00000000ULL;
inline constexpr Tick kGameOverTick = 300;
inline constexpr int kMoverCount = 48;
inline constexpr std::string_view kGameplayState = "gameplay";
inline constexpr std::string_view kGameOverState = "game_over";

enum KernelAction : ActionId {
  kActionUp = 0,
  kActionDown = 1,
  kActionLeft = 2,
  kActionRight = 3,
  kActionFire = 4,
};

[[nodiscard]] const GameModule& KernelTestModule();

[[nodiscard]] constexpr Guid MoverGuid(int index) noexcept {
  return Guid{kSceneHi, static_cast<std::uint64_t>(index) + 1};
}
inline constexpr Guid kPlayerGuid{kSceneHi, 1000};
inline constexpr Guid kSpawnerGuid{kSceneHi, 2000};
inline constexpr Guid kFollowerGuid{kSceneHi, 3000};
inline constexpr Guid kBannerGuid{kSceneHi, 4000};

// The scene description recorded as a replay's start bytes.
[[nodiscard]] std::vector<std::byte> SceneBytes(std::uint64_t seed);

// Input as a pure function of the tick and a seed.
class ScriptedInput final : public InputSource {
 public:
  explicit ScriptedInput(std::uint64_t seed) noexcept : seed_(seed) {}
  InputFrame Latch(Tick tick, std::uint64_t tick_window_end_ns,
                   bool last_tick_of_frame) override;
  [[nodiscard]] static InputFrame FrameFor(Tick tick, std::uint64_t seed);

 private:
  std::uint64_t seed_;
};

// tests/fixtures/kernel/edits.txt.
[[nodiscard]] std::string EditsFixturePath();

// Reads an edits file: "<tick> <guid> <component> <json>" per line, '#'
// comments. Each line becomes a kSetComponent entry for its tick.
[[nodiscard]] std::expected<std::vector<SessionEntry>, Error> LoadEdits(
    const ComponentRegistry& registry, const std::string& path);

}  // namespace se::testing

#endif  // TESTS_SUPPORT_KERNEL_TEST_MODULE_H_
