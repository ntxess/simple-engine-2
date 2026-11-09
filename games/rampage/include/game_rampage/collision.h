// Copyright 2026 The simple-engine-2 Authors.
//
// The CollisionSystem's per-tick scratch, kept in the World's ctx and
// cleared and reused every tick, so steady-state ticks do not allocate. It
// holds no state between ticks: everything in it is rebuilt from the
// components at the start of each CollisionSystem run.

#ifndef GAME_RAMPAGE_COLLISION_H_
#define GAME_RAMPAGE_COLLISION_H_

#include <cstdint>
#include <entt/entity/entity.hpp>
#include <vector>

#include "game_rampage/broadphase.h"
#include "game_rampage/components.h"
#include "se_core/types.h"

namespace se::rampage {

struct CollisionCandidate {
  std::uint32_t serial = 0;
  entt::entity entity = entt::null;
};

struct Contact {
  std::uint32_t source_serial = 0;
  std::uint32_t receiver_serial = 0;
  std::uint32_t source = 0;
  std::uint32_t receiver = 0;
};

struct CollisionScratch {
  // Every entity with a Collider and a Team, by spawn serial (R3).
  std::vector<CollisionCandidate> candidates;
  std::vector<Aabb> boxes;
  SpatialHash broadphase;
  std::vector<std::uint32_t> query;
  std::vector<Contact> contacts;
  // ActiveEffect::seq of the effects created in |seq_tick|.
  Tick seq_tick = 0;
  std::uint32_t seq_next = 0;
};

}  // namespace se::rampage

#endif  // GAME_RAMPAGE_COLLISION_H_
