// Copyright 2026 The simple-engine-2 Authors.
//
// CollisionSystem (R2, R3, R4, R8): every overlapping (source, receiver)
// pair of the tick, found through the spatial hash, sorted by spawn serial,
// then the self, same-team and cooldown checks and one ActiveEffect per
// effect of the source, in list order (core/system/CollisionSystem.cpp).
// The boxes and cells (rampage.Broadphase) and the pair tests
// (rampage.Narrowphase) are fork sites; the contacts are concatenated in
// chunk order, so they come out as the serial loop made them.

#include "game_rampage/collision.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <vector>

#include "game_rampage/broadphase.h"
#include "game_rampage/components.h"
#include "game_rampage/fork_sites.h"
#include "game_rampage/systems.h"
#include "se_core/core_components.h"
#include "se_core/guid.h"
#include "se_core/jobs.h"
#include "se_core/schedule.h"
#include "se_core/world.h"

namespace se::rampage {
namespace {

// R2: whether the polled source may affect |receiver| now. A positive
// decision records the contact, so the pair is decided once per tick.
bool ReadyFor(PairCooldowns& cooldowns, const EffectSource& source,
              Tick spawn_tick, const Guid& receiver, Tick now,
              std::uint32_t rate) {
  const Tick cooldown = TicksFromMs(source.cooldown_ms, rate);
  auto& pairs = cooldowns.pairs;
  const auto it =
      std::lower_bound(pairs.begin(), pairs.end(), receiver,
                       [](const PairCooldown& pair, const Guid& key) {
                         return pair.receiver < key;
                       });
  if (it != pairs.end() && it->receiver == receiver) {
    if (now - it->last <= cooldown) return false;
    it->last = now;
    return true;
  }
  if (!source.ready_on_start) {
    const Tick since = now > spawn_tick ? now - spawn_tick : 0;
    if (since <= cooldown) return false;
  }
  pairs.insert(it, PairCooldown{receiver, now});
  return true;
}

Tick FirstDue(const EffectDef& def, Tick now, std::uint32_t rate) {
  switch (def.kind) {
    case EffectKind::kInstant:
      return now;
    case EffectKind::kOverTime:
      return now + std::max<Tick>(TicksFromMs(def.interval_ms, rate), 1);
    case EffectKind::kTempTimed:
      return now + TicksFromMs(def.duration_ms, rate);
  }
  return now;
}

}  // namespace

void CollisionSystem::Run(World& world, const TickContext& context) {
  entt::registry& reg = world.Reg();
  auto& ctx = reg.ctx();
  CollisionScratch* scratch = ctx.find<CollisionScratch>();
  if (scratch == nullptr) scratch = &ctx.emplace<CollisionScratch>();
  const Tick now = context.clock.tick;
  const std::uint32_t rate = context.clock.rate;
  if (scratch->seq_tick != now) {
    scratch->seq_tick = now;
    scratch->seq_next = 0;
  }

  // Contacts whose cooldown ran out no longer change any decision.
  for (const auto [e, source, cooldowns] :
       reg.view<const EffectSource, PairCooldowns>().each()) {
    const Tick cooldown = TicksFromMs(source.cooldown_ms, rate);
    std::erase_if(cooldowns.pairs, [now, cooldown](const PairCooldown& pair) {
      return now - pair.last > cooldown;
    });
  }

  // R3: every entity with a collider and a team is a candidate receiver, in
  // spawn order; R4: its collider is the unrotated box at its position.
  auto& candidates = scratch->candidates;
  candidates.clear();
  for (const auto [e, spawned, collider, team, transform] :
       reg.view<const Spawned, const Collider, const Team, const Transform>()
           .each()) {
    candidates.push_back(CollisionCandidate{spawned.spawn_serial, e});
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const CollisionCandidate& a, const CollisionCandidate& b) {
              return a.serial < b.serial;
            });
  const auto count = static_cast<std::uint32_t>(candidates.size());
  jobs::ForkJoinPool& jobs = context.jobs;

  // Broadphase: each candidate's box (R4), whether it is a source this tick
  // (polled ones every tick, event-driven ones on ticks they moved) and its
  // cells.
  jobs::ForkSite& broad = jobs.Site(BroadphaseSite());
  auto& boxes = scratch->boxes;
  auto& sources = scratch->sources;
  boxes.resize(count);
  sources.resize(count);
  const std::uint32_t broad_chunks = jobs.ChunkCountFor(broad, count);
  scratch->cells.Reset(broad_chunks);
  scratch->oversized.Reset(broad_chunks);
  {
    const auto view =
        world.Parallel<Transform, Collider, EffectSource, Moved>(broad);
    const SpatialHash& hash = scratch->broadphase;
    jobs.ParallelFor(broad, count, [&](jobs::ChunkRange range) {
      std::vector<SpatialHash::Entry>& cells = scratch->cells.At(range);
      std::vector<std::uint32_t>& oversized = scratch->oversized.At(range);
      for (std::uint32_t i = range.begin; i < range.end; ++i) {
        const entt::entity e = candidates[i].entity;
        boxes[i] = ColliderBox(view.Read<Transform>(e).pos,
                               view.Read<Collider>(e).half_extents);
        const bool source =
            view.Has<EffectSource>(e) &&
            (view.Read<EffectSource>(e).polled || view.Has<Moved>(e));
        sources[i] = source ? 1 : 0;
        hash.AppendCells(boxes[i], i, cells, oversized);
      }
    });
  }
  scratch->broadphase.Assemble(count, scratch->cells, scratch->oversized);

  // Narrowphase: each source's candidates, tested for overlap (R3).
  jobs::ForkSite& narrow = jobs.Site(NarrowphaseSite());
  const std::uint32_t narrow_chunks = jobs.ChunkCountFor(narrow, count);
  scratch->queries.Reset(narrow_chunks);
  scratch->found.Reset(narrow_chunks);
  {
    const SpatialHash& hash = scratch->broadphase;
    jobs.ParallelFor(narrow, count, [&](jobs::ChunkRange range) {
      std::vector<std::uint32_t>& query = scratch->queries.At(range);
      std::vector<Contact>& found = scratch->found.At(range);
      for (std::uint32_t i = range.begin; i < range.end; ++i) {
        if (sources[i] == 0) continue;
        hash.Query(boxes[i], query);
        for (const std::uint32_t j : query) {
          if (Overlaps(boxes[i], boxes[j])) {
            found.push_back(
                Contact{candidates[i].serial, candidates[j].serial, i, j});
          }
        }
      }
    });
  }
  auto& contacts = scratch->contacts;
  contacts.clear();
  scratch->found.AppendTo(contacts);
  // R8: by (source, receiver) spawn serial.
  std::sort(contacts.begin(), contacts.end(),
            [](const Contact& a, const Contact& b) {
              return a.source_serial != b.source_serial
                         ? a.source_serial < b.source_serial
                         : a.receiver_serial < b.receiver_serial;
            });

  for (const Contact& contact : contacts) {
    if (contact.source == contact.receiver) continue;
    const entt::entity src = candidates[contact.source].entity;
    const entt::entity rcv = candidates[contact.receiver].entity;
    if (reg.get<Team>(src).id == reg.get<Team>(rcv).id) continue;
    const EffectSource& source = reg.get<EffectSource>(src);
    const Guid receiver_guid = reg.get<IdComponent>(rcv).guid;
    if (source.polled &&
        !ReadyFor(reg.get_or_emplace<PairCooldowns>(src), source,
                  reg.get<Spawned>(src).spawn_tick, receiver_guid, now, rate)) {
      continue;
    }
    if (!reg.all_of<Stats>(rcv) || source.effects.empty()) continue;
    auto& active = reg.get_or_emplace<ActiveEffects>(rcv).effects;
    const Guid source_guid = reg.get<IdComponent>(src).guid;
    for (const EffectDef& def : source.effects) {
      ActiveEffect effect;
      effect.def = def;
      effect.source = EntityRef{source_guid, src};
      effect.start = now;
      effect.next = FirstDue(def, now, rate);
      effect.seq = (now << 32) | scratch->seq_next++;
      active.push_back(effect);
    }
  }
}

}  // namespace se::rampage
