// Copyright 2026 The simple-engine-2 Authors.
//
// The engine's own components. Every World entity has an IdComponent and a
// Spawned record; Transform and PrevTransform carry the 2D pose and its
// previous-tick copy for interpolation; DestroyRequest marks an entity for
// destruction at the end of the tick.

#ifndef SE_CORE_CORE_COMPONENTS_H_
#define SE_CORE_CORE_COMPONENTS_H_

#include <cstdint>
#include <string>

#include "se_core/component_registry.h"
#include "se_core/guid.h"
#include "se_core/types.h"

namespace se {

struct IdComponent {
  Guid guid;
};

struct Name {
  std::string value;
};

struct Transform {
  Vec2 pos;
  // Radians, clockwise on screen (y grows downwards); ADR-023.
  float rot = 0.0F;
  Vec2 scale{1.0F, 1.0F};
  // The tick of the last teleport; the renderer does not interpolate across
  // it (teleport_tick == current tick draws prev = current).
  Tick teleport_tick = 0;
};

// Transform as it was at the end of the previous tick.
struct PrevTransform {
  Vec2 pos;
  float rot = 0.0F;
  Vec2 scale{1.0F, 1.0F};
  Tick tick = 0;
};

struct Spawned {
  Tick spawn_tick = 0;
  // Creation order within the World.
  std::uint32_t spawn_serial = 0;
};

struct DestroyRequest {};

// Registers the components above (core_components.cc).
void RegisterCoreComponents(ComponentRegistry& registry);

}  // namespace se

#endif  // SE_CORE_CORE_COMPONENTS_H_
