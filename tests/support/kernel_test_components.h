// Copyright 2026 The simple-engine-2 Authors.
//
// Components of the kernel test module (support/kernel_test_module.h): a
// game module small enough to read, built to exercise every kernel path the
// determinism tests rely on.

#ifndef TESTS_SUPPORT_KERNEL_TEST_COMPONENTS_H_
#define TESTS_SUPPORT_KERNEL_TEST_COMPONENTS_H_

#include <cstdint>
#include <string>

#include "se_core/component_registry.h"
#include "se_core/guid.h"
#include "se_core/types.h"

namespace se::testing {

// Moves its entity on a circle: pos = center + radius * (cos a, sin a) with
// a = phase + angular_speed * time, using se::dm.
struct Mover {
  Vec2 center;
  float radius = 0.0F;
  float angular_speed = 0.0F;
  float phase = 0.0F;
};

// Spawns a child every every_ticks ticks with Guids from World::Spawn,
// keeping at most max_alive children; children live child_ttl ticks.
struct Spawner {
  std::uint32_t every_ticks = 30;
  std::uint32_t spawned = 0;
  std::uint32_t max_alive = 8;
  std::uint32_t child_ttl = 90;
};

// Destroyed at expire_tick.
struct Lifetime {
  Tick expire_tick = 0;
};

// Moved by the held directions of the InputFrame.
struct Player {
  float speed = 120.0F;
  // How many ticks had any action held.
  std::uint32_t inputs = 0;
};

// Trails another entity (an EntityRef, resolved by Guid every tick).
struct Follow {
  EntityRef target;
  float distance = 0.0F;
};

// The GameOver state's banner.
struct Banner {
  std::string text;
  Tick shown_since = 0;
};

// Registers the components above (kernel_test_components.cc).
void RegisterKernelTestComponents(ComponentRegistry& registry);

}  // namespace se::testing

#endif  // TESTS_SUPPORT_KERNEL_TEST_COMPONENTS_H_
