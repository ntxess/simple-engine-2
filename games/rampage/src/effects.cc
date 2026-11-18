// Copyright 2026 The simple-engine-2 Authors.
//
// EffectSystem (R1, R7, R8), DeathRule (R10) and ClearMovedSystem. The
// effect timing is the legacy EventSystem's, comparison for comparison
// (core/system/EventSystem.cpp), with the event's lastApplyTick kept as
// ActiveEffect::next minus the interval.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <vector>

#include "game_rampage/components.h"
#include "game_rampage/systems.h"
#include "se_core/core_components.h"
#include "se_core/schedule.h"
#include "se_core/world.h"
#include "se_sim/audio_events.h"

namespace se::rampage {
namespace {

void Add(float* stat, float value) {
  if (stat != nullptr) *stat += value;
}

// An effect's own application (not a revert); |damaged| is set when it
// lowers HP.
void Apply(const EffectDef& def, float* stat, bool& damaged) {
  Add(stat, def.value);
  if (stat != nullptr && def.stat == kStatHp && def.value < 0.0F) {
    damaged = true;
  }
}

// One tick of one effect; true when it is complete.
bool Process(ActiveEffect& effect, Stats& stats, Tick now, Tick watchdog,
             std::uint32_t rate, bool& damaged) {
  const EffectDef& def = effect.def;
  float* stat = FindStat(stats, def.stat);
  switch (def.kind) {
    case EffectKind::kInstant:
      Apply(def, stat, damaged);
      return true;
    case EffectKind::kOverTime: {
      // R7: floor(d / i) applications, the first one interval after the
      // contact, compared with >= in ticks; an interval of 0 applies once
      // per tick for ticks(d) ticks.
      const Tick interval =
          std::max<Tick>(TicksFromMs(def.interval_ms, rate), 1);
      const std::uint64_t total =
          def.interval_ms > 0 ? static_cast<std::uint64_t>(
                                    std::max<std::int64_t>(def.duration_ms, 0) /
                                    def.interval_ms)
                              : TicksFromMs(def.duration_ms, rate);
      const Tick last = effect.next - interval;
      const Tick since = now - last;
      if (effect.applied < total && since >= interval) {
        effect.next = now + interval;
        ++effect.applied;
        Apply(def, stat, damaged);
      }
      return watchdog < since || effect.applied >= total;
    }
    case EffectKind::kTempTimed: {
      // R1: applied once at contact, reverted exactly once, at its end or
      // by the watchdog.
      const Tick duration = TicksFromMs(def.duration_ms, rate);
      const Tick since = now - effect.start;
      if (watchdog < since) {
        if (effect.applied != 0) Add(stat, def.value * -1.0F);
        return true;
      }
      if (effect.applied == 0) {
        effect.applied = 1;
        effect.next = effect.start + duration;
        Apply(def, stat, damaged);
      }
      if (since >= duration) {
        Add(stat, def.value * -1.0F);
        return true;
      }
      return false;
    }
  }
  return true;
}

}  // namespace

void EffectSystem::Run(World& world, const TickContext& context) {
  entt::registry& reg = world.Reg();
  const auto* settings = reg.ctx().find<EffectSettings>();
  const std::int64_t watchdog_ms = settings != nullptr
                                       ? settings->event_watchdog_ms
                                       : EffectSettings{}.event_watchdog_ms;
  const std::uint32_t rate = context.clock.rate;
  const Tick watchdog = TicksFromMs(watchdog_ms, rate);
  const Tick now = context.clock.tick;
  AudioEvents* audio = FindAudioEvents(world);
  for (const auto [e, active] : reg.view<ActiveEffects>().each()) {
    auto& effects = active.effects;
    if (effects.empty()) continue;
    Stats* stats = reg.try_get<Stats>(e);
    if (stats == nullptr) {
      // Without a receiver status the effects are dropped.
      effects.clear();
      continue;
    }
    // R8: in creation order.
    bool damaged = false;
    std::size_t kept = 0;
    for (std::size_t i = 0; i < effects.size(); ++i) {
      if (Process(effects[i], *stats, now, watchdog, rate, damaged)) continue;
      if (kept != i) effects[kept] = effects[i];
      ++kept;
    }
    effects.resize(kept);
    // The sound of the damage (M8): asked for, never read back.
    if (damaged && audio != nullptr) {
      const auto* transform = reg.try_get<Transform>(e);
      audio->Push({.sound = kSoundHit,
                   .positional = transform != nullptr,
                   .position = transform != nullptr ? transform->pos : Vec2{}});
    }
  }
}

void DeathRule::Run(World& world, const TickContext& /*context*/) {
  entt::registry& reg = world.Reg();
  AudioEvents* audio = FindAudioEvents(world);
  for (const auto [e, stats] : reg.view<const Stats>().each()) {
    // R10: a missing HP counts as 0.
    const float* hp = FindStat(stats, kStatHp);
    const float value = hp != nullptr ? *hp : 0.0F;
    if (value > 0.0F) continue;
    world.RequestDestroy(e);
    // A cosmetic sound: dropped when it would start too late (M8).
    if (audio != nullptr) {
      const auto* transform = reg.try_get<Transform>(e);
      audio->Push({.sound = kSoundDeath,
                   .cosmetic = true,
                   .positional = transform != nullptr,
                   .position = transform != nullptr ? transform->pos : Vec2{}});
    }
  }
}

void ClearMovedSystem::Run(World& world, const TickContext& /*context*/) {
  world.Reg().clear<Moved>();
}

void RegisterRampageSystems(Schedule& schedule) {
  schedule.Add<PlayerInputSystem>(Phase::kInput);
  schedule.Add<IntentSystem>(Phase::kIntent);
  schedule.Add<KinematicMoveSystem>(Phase::kMovement);
  schedule.Add<PathSystem>(Phase::kMovement);
  schedule.Add<BoundarySystem>(Phase::kMovement);
  schedule.Add<CollisionSystem>(Phase::kCollision);
  schedule.Add<EffectSystem>(Phase::kEffects);
  schedule.Add<DeathRule>(Phase::kGameRules);
  schedule.Add<ClearMovedSystem>(Phase::kCleanup);
}

}  // namespace se::rampage
