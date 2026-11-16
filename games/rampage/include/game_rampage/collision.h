// Copyright 2026 The simple-engine-2 Authors.
//
// The CollisionSystem's per-tick scratch, kept in the World's ctx and
// cleared and reused every tick, so steady-state ticks do not allocate. It
// holds no state between ticks: everything in it is rebuilt from the
// components at the start of each CollisionSystem run. The broadphase and
// narrowphase fork sites (game_rampage/fork_sites.h) write its per-index
// arrays by chunk range and their variable outputs per chunk.

#ifndef GAME_RAMPAGE_COLLISION_H_
#define GAME_RAMPAGE_COLLISION_H_

#include <cstdint>
#include <entt/entity/entity.hpp>
#include <vector>

#include "game_rampage/broadphase.h"
#include "game_rampage/components.h"
#include "se_core/jobs.h"
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
  // 1 when candidate i acts as a source this tick (polled, or event-driven
  // and Moved); bytes, not vector<bool>, since chunks write their ranges.
  std::vector<std::uint8_t> sources;
  SpatialHash broadphase;
  // Per chunk: the broadphase's cells and oversized boxes, the
  // narrowphase's query buffer and contacts.
  jobs::ChunkOutputs<SpatialHash::Entry> cells;
  jobs::ChunkOutputs<std::uint32_t> oversized;
  jobs::ChunkOutputs<std::uint32_t> queries;
  jobs::ChunkOutputs<Contact> found;
  std::vector<Contact> contacts;
  // ActiveEffect::seq of the effects created in |seq_tick|.
  Tick seq_tick = 0;
  std::uint32_t seq_next = 0;
};

}  // namespace se::rampage

#endif  // GAME_RAMPAGE_COLLISION_H_
