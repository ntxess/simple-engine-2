// Copyright 2026 The simple-engine-2 Authors.
//
// SimDriver's TickGate (M9: a World never steps before its AssetScope is
// resident). The kernel test module replaces gameplay with game_over at
// tick 300; a gate that holds game_over for a few frames stops the frame
// at the replace, and the held ticks run later with the same results: the
// per-tick hashes equal an ungated run's, so asset timing cannot change
// what a tick computes.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>

#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_sim/game_state.h"
#include "se_sim/sim_driver.h"
#include "support/kernel_session.h"
#include "support/kernel_test_module.h"
#include "support/test_roles.h"

namespace se {
namespace {

using testing::KernelSessionOptions;
using testing::KernelSessionResult;
using testing::MainThreadScope;
using testing::RunKernelSession;

// Holds every tick of game_over until it has been asked |holds| times.
class GameOverGate final : public TickGate {
 public:
  explicit GameOverGate(int holds) : holds_(holds) {}

  bool HoldTicks(const GameStateStack& states, Tick next_tick) override {
    ++calls_;
    if (states.TopId() != testing::kGameOverState) return false;
    if (held_ >= holds_) return false;
    if (held_ == 0) first_held_tick_ = next_tick;
    ++held_;
    return true;
  }

  [[nodiscard]] int Held() const noexcept { return held_; }
  [[nodiscard]] int Calls() const noexcept { return calls_; }
  [[nodiscard]] Tick FirstHeldTick() const noexcept { return first_held_tick_; }

 private:
  int holds_;
  int held_ = 0;
  int calls_ = 0;
  Tick first_held_tick_ = 0;
};

TEST_CASE("A held state change delays ticks without changing them",
          "[tick_gate]") {
  const MainThreadScope main;
  KernelSessionOptions options;
  // 60 Hz: two ticks per frame, so the replace at tick 300 lands inside a
  // frame.
  options.hz = 60.0;
  options.ticks = 420;
  const KernelSessionResult ungated = RunKernelSession(options);
  REQUIRE(ungated.error.empty());

  GameOverGate gate(5);
  options.gate = &gate;
  const KernelSessionResult gated = RunKernelSession(options);
  REQUIRE(gated.error.empty());
  // The first game_over tick was held, five times.
  REQUIRE(gate.Held() == 5);
  REQUIRE(gate.FirstHeldTick() == testing::kGameOverTick + 1);
  // Asked before every tick, plus once per held frame.
  REQUIRE(gate.Calls() == 420 + 5);
  // Every tick ran, in the same order, with the same results.
  REQUIRE(gated.hashes.size() == ungated.hashes.size());
  CHECK(gated.hashes == ungated.hashes);
  CHECK(gated.top_states == ungated.top_states);
}

}  // namespace
}  // namespace se
