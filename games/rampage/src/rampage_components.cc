// Copyright 2026 The simple-engine-2 Authors.
//
// Registration of game_rampage's components (the only translation unit of
// the module that includes glaze, ADR-010). Enums serialize by name.

#include "game_rampage/components.h"
#include "se_core/component_registry.h"
#include "se_core/reflect_glaze.h"

template <>
struct glz::meta<se::rampage::TeamId> {
  using enum se::rampage::TeamId;
  // glaze's customization point is named value.
  static constexpr auto value =  // NOLINT(readability-identifier-naming)
      glz::enumerate("unaffiliated", kUnaffiliated, "friendly", kFriendly,
                     "enemy", kEnemy);
};

template <>
struct glz::meta<se::rampage::EffectKind> {
  using enum se::rampage::EffectKind;
  static constexpr auto value =  // NOLINT(readability-identifier-naming)
      glz::enumerate("instant", kInstant, "over_time", kOverTime, "temp_timed",
                     kTempTimed);
};

template <>
struct glz::meta<se::rampage::ColliderRole> {
  using enum se::rampage::ColliderRole;
  static constexpr auto value =  // NOLINT(readability-identifier-naming)
      glz::enumerate("body", kBody, "hitbox", kHitbox, "hurtbox", kHurtbox,
                     "trigger", kTrigger);
};

namespace se::rampage {

void RegisterRampageComponents(ComponentRegistry& registry) {
  using enum ComponentFlags;
  constexpr ComponentFlags kState = kHashed | kSerialized | kEditorVisible;
  registry.Add<Team>("Team", 1, kState);
  registry.Add<Stats>("Stats", 1, kState);
  registry.Add<EffectSource>("EffectSource", 1, kState);
  registry.Add<PairCooldowns>("PairCooldowns", 1, kHashed | kSerialized);
  registry.Add<ActiveEffects>("ActiveEffects", 1, kHashed | kSerialized);
  registry.Add<Collider>("Collider", 1, kState);
  registry.Add<KinematicBody>("KinematicBody", 1, kState);
  registry.Add<MoveIntent>("MoveIntent", 1, kHashed | kSerialized);
  registry.Add<PlayerControl>("PlayerControl", 1, kState);
  registry.Add<Path>("Path", 1, kState);
  // Derived from Path; rebuilt on load.
  registry.Add<PathCache>("PathCache", 1, kNone);
  // Set and cleared within a tick (only a load leaves it between ticks).
  registry.Add<Moved>("Moved", 1, kHashed);
}

}  // namespace se::rampage
