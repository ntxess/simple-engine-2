// Copyright 2026 The simple-engine-2 Authors.
//
// The editor's Idle (M10a): allowed in Edit mode only, switched per frame
// with Lifecycle::SetIdleAllowed; after 1 s without input the callbacks
// wait for events, and any event (input or a wake) runs the next frame.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>

#include "se_runtime/lifecycle.h"

namespace se {
namespace {

constexpr std::uint64_t kMs = 1'000'000;

TEST_CASE("Idle follows the editor's mode", "[idle]") {
  LifecycleConfig config;
  config.idle_allowed = true;
  Lifecycle lifecycle(config);
  lifecycle.BootComplete(0, true, true);
  lifecycle.Update(500 * kMs);
  REQUIRE_FALSE(lifecycle.Idle());
  lifecycle.Update(1100 * kMs);
  REQUIRE(lifecycle.Idle());
  REQUIRE(lifecycle.Rate() == CallbackRate::kWaitEvent);
  // Play starts: no Idle while it runs.
  lifecycle.SetIdleAllowed(false);
  lifecycle.Update(1200 * kMs);
  REQUIRE_FALSE(lifecycle.Idle());
  REQUIRE(lifecycle.Rate() == CallbackRate::kFull);
  // Back in Edit mode, idle again after the quiet second.
  lifecycle.SetIdleAllowed(true);
  lifecycle.OnInputActivity(1300 * kMs);
  lifecycle.Update(1400 * kMs);
  REQUIRE_FALSE(lifecycle.Idle());
  lifecycle.Update(2400 * kMs);
  REQUIRE(lifecycle.Idle());
  // A wake (any event) runs the next frame.
  lifecycle.OnAnyEvent();
  REQUIRE_FALSE(lifecycle.Idle());
  REQUIRE(lifecycle.Rate() == CallbackRate::kFull);
}

}  // namespace
}  // namespace se
