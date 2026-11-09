// Copyright 2026 The simple-engine-2 Authors.
//
// The components of game_rampage, the headless port of the legacy Sandbox's
// gameplay (docs/milestone-specs.md, M3b; docs/rules/gameplay-rules.md).
// All are plain aggregates registered in rampage_components.cc, hashed and
// serialized unless noted.
//
// Units: positions in pixels, Transform::rot in radians in [0, 2 pi) (the
// legacy sf::Angle's storage, see RotationFromHeading), speeds in pixels per
// second, authored durations in milliseconds (TicksFromMs converts them).
//
// The spawn serial (Spawned::spawn_serial) plays the legacy SpawnIndex: the
// player first, then the entities in file order. Systems order contacts and
// the projection by it.

#ifndef GAME_RAMPAGE_COMPONENTS_H_
#define GAME_RAMPAGE_COMPONENTS_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/component_registry.h"
#include "se_core/guid.h"
#include "se_core/hash.h"
#include "se_core/sim_clock.h"
#include "se_core/types.h"

namespace se::rampage {

// Teams. Sources never affect receivers of their own team.
enum class TeamId : std::uint8_t { kUnaffiliated, kFriendly, kEnemy };

struct Team {
  TeamId id = TeamId::kUnaffiliated;
};

// A stat's id: Fnv1a32 of its name. StatNames maps ids back to names.
using StatId = std::uint32_t;

[[nodiscard]] constexpr StatId StatIdOf(std::string_view name) noexcept {
  return Fnv1a32(name);
}

inline constexpr StatId kStatHp = StatIdOf("HP");
inline constexpr StatId kStatSpeed = StatIdOf("Speed");

struct StatValue {
  StatId id = 0;
  float value = 0.0F;
};

// Named float stats, sorted by id. A missing stat reads as absent, not 0,
// except where a rule says otherwise (DeathRule, R10).
struct Stats {
  std::vector<StatValue> values;
};

// The stat |id| of |stats|, or nullptr.
[[nodiscard]] const float* FindStat(const Stats& stats, StatId id) noexcept;
[[nodiscard]] float* FindStat(Stats& stats, StatId id) noexcept;
// Adds or replaces the stat |id|, keeping the values sorted.
void SetStat(Stats& stats, StatId id, float value);

enum class EffectKind : std::uint8_t { kInstant, kOverTime, kTempTimed };

// One effect of a source's list (the legacy Effects with its EffectType).
struct EffectDef {
  EffectKind kind = EffectKind::kInstant;
  StatId stat = 0;
  float value = 0.0F;
  std::int64_t duration_ms = 0;
  std::int64_t interval_ms = 0;
};

// ceil(ms * 120 / 1000) ticks at the default rate, in integer arithmetic;
// non-positive durations are 0 ticks (the legacy msToTicks).
[[nodiscard]] constexpr Tick TicksFromMs(
    std::int64_t ms, std::uint32_t rate = kDefaultTickRate) noexcept {
  return ms <= 0 ? 0
                 : TicksFromMilliseconds(static_cast<std::uint64_t>(ms), rate);
}

// An entity whose collider applies effects to the receivers it overlaps.
// A polled source (the legacy UpdateEntityPolling) acts every tick, with a
// cooldown per receiver (PairCooldowns, R2). Otherwise the source is
// event-driven and acts on the ticks it moved (Moved).
struct EffectSource {
  std::vector<EffectDef> effects;
  bool polled = false;
  std::int64_t cooldown_ms = 0;
  // Before the first contact with a receiver: act at once, or only once
  // more than the cooldown has passed since the source spawned.
  bool ready_on_start = true;
};

// The last contact of a polled source with one receiver.
struct PairCooldown {
  Guid receiver;
  Tick last = 0;
};

// On polled sources: one entry per receiver, sorted by receiver Guid.
struct PairCooldowns {
  std::vector<PairCooldown> pairs;
};

// One effect running on its receiver (the legacy StatusModEvent).
//   start    the contact tick;
//   next     OVERTIME: the tick of the next application (one interval
//            after the previous one, or after the contact);
//            TEMPTIMED: the tick of the revert;
//   applied  applications so far (TEMPTIMED: 1 once applied);
//   seq      creation order: (contact tick << 32) + index within the tick.
struct ActiveEffect {
  EffectDef def;
  EntityRef source;
  Tick start = 0;
  Tick next = 0;
  std::uint32_t applied = 0;
  std::uint64_t seq = 0;
};

// On receivers (entities with Stats), in seq order.
struct ActiveEffects {
  std::vector<ActiveEffect> effects;
};

enum class ColliderRole : std::uint8_t { kBody, kHitbox, kHurtbox, kTrigger };

// One axis-aligned box centred on Transform::pos, unrotated (R4).
struct Collider {
  Vec2 half_extents;
  ColliderRole role = ColliderRole::kBody;
  std::uint32_t layer = 1;
  std::uint32_t mask = 0xFFFFFFFFU;
};

struct KinematicBody {
  Vec2 velocity;
};

// The direction the controls ask for this tick (components -1, 0 or 1).
struct MoveIntent {
  Vec2 dir;
};

// The player's controls: 120 px/s (R5).
struct PlayerControl {
  std::uint8_t player = 0;
  float speed = 120.0F;
};

// A polyline the entity follows at its Speed stat (R6, R12, R13): segment i
// runs from points[i] to points[i + 1]; a looping path adds a closing
// segment from the last point to the first. u is the distance travelled
// along the current segment. done: the end of a non-looping path was
// reached, and the entity no longer moves.
struct Path {
  std::vector<Vec2> points;
  bool loop = false;
  std::uint32_t segment = 0;
  float u = 0.0F;
  bool done = false;
};

// Derived from Path::points (not hashed, not serialized; rebuilt on load and
// whenever the point count changes): lengths[i] of segment i (the closing
// segment last) and totals[i], the running sum of the lengths before point
// i (the legacy WayPoint::distanceTotal).
struct PathCache {
  std::vector<float> lengths;
  std::vector<float> totals;
};

// The entity moved this tick (set by Movement, removed by Cleanup; not
// serialized): what makes an event-driven source act (R2, R8).
struct Moved {};

// Registers the components above.
void RegisterRampageComponents(ComponentRegistry& registry);

// World context (entt::registry::ctx). Scene settings:

struct ArenaBounds {
  float w = 1920.0F;
  float h = 1080.0F;
};

struct EffectSettings {
  // The event watchdog: an effect older than this ends early (R1, R7).
  std::int64_t event_watchdog_ms = 36000;
};

struct SceneSeed {
  std::uint64_t value = 0;
};

// Which reference rules the systems follow. kV1 drops R11 to R13 (the
// legacy-ref-v1 boundary clamp and path steps); only the comparison against
// the legacy-ref-v1 goldens uses it.
enum class RulesVersion : std::uint8_t { kV1, kV2 };

struct RulesConfig {
  RulesVersion version = RulesVersion::kV2;
};

// Names by their 32-bit FNV-1a id (Fnv1a32: StatIdOf, TextureKeyOf).
class NameTable {
 public:
  // Records |name| and returns its id.
  std::uint32_t Add(std::string_view name);
  // The name of |id|, or empty.
  [[nodiscard]] std::string_view Name(std::uint32_t id) const noexcept;
  [[nodiscard]] const std::vector<std::pair<std::uint32_t, std::string>>& All()
      const noexcept {
    return names_;
  }

 private:
  // Sorted by id.
  std::vector<std::pair<std::uint32_t, std::string>> names_;
};

// Stat names by id, for the projection and, later, the editor.
class StatNames : public NameTable {};

// Texture names by key (SpriteRenderer::texture), recorded by the loaders so
// SaveScene writes names back.
class TextureNames : public NameTable {};

}  // namespace se::rampage

#endif  // GAME_RAMPAGE_COMPONENTS_H_
