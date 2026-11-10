// Copyright 2026 The simple-engine-2 Authors.
//
// game_rampage's systems: stateless structs registered by the GameModule in
// the phases of rule R9 (docs/milestone-specs.md, M3b "Systems"):
//
//   Input      PlayerInputSystem    held bits -> MoveIntent
//   Intent     IntentSystem         MoveIntent * speed -> velocity; the
//                                   discrete presses into ActionBuffer
//   Movement   (PrevTransformCopy)  built in
//              KinematicMoveSystem  the player: pos += velocity * dt
//              PathSystem           path following (R6, R12, R13)
//              BoundarySystem       the arena clamp (R11)
//   Collision  CollisionSystem      contacts, cooldowns, new effects
//                                   (R2, R3, R4, R8)
//   Effects    EffectSystem         INSTANT, OVERTIME, TEMPTIMED (R1, R7);
//                                   the "hit" sound when HP is damaged
//   GameRules  DeathRule            HP <= 0 -> DestroyRequest (R10); the
//                                   cosmetic "death" sound
//   Cleanup    ClearMovedSystem     drops the Moved flags
//              (built-in Cleanup)   destroys, FinalizeSpawns, hash
//
// With RulesConfig::version == kV1 (tests only), PathSystem and
// BoundarySystem follow the legacy-ref-v1 reference instead of R11 to R13.

#ifndef GAME_RAMPAGE_SYSTEMS_H_
#define GAME_RAMPAGE_SYSTEMS_H_

#include <cstdint>
#include <string_view>

#include "game_rampage/components.h"
#include "se_core/core_components.h"
#include "se_core/hash.h"
#include "se_core/input_frame.h"
#include "se_core/schedule.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/types.h"
#include "se_core/world.h"

namespace se::rampage {

struct PlayerInputSystem {
  static constexpr std::string_view kName = "PlayerInput";
  void Run(World& world, const TickContext& context) SE_REQUIRES(g_sim_owner);
};

struct IntentSystem {
  static constexpr std::string_view kName = "Intent";
  void Run(World& world, const TickContext& context) SE_REQUIRES(g_sim_owner);
};

struct KinematicMoveSystem {
  static constexpr std::string_view kName = "KinematicMove";
  void Run(World& world, const TickContext& context) SE_REQUIRES(g_sim_owner);
};

struct PathSystem {
  static constexpr std::string_view kName = "Path";
  void Run(World& world, const TickContext& context) SE_REQUIRES(g_sim_owner);
};

struct BoundarySystem {
  static constexpr std::string_view kName = "Boundary";
  void Run(World& world, const TickContext& context) SE_REQUIRES(g_sim_owner);
};

struct CollisionSystem {
  static constexpr std::string_view kName = "Collision";
  void Run(World& world, const TickContext& context) SE_REQUIRES(g_sim_owner);
};

struct EffectSystem {
  static constexpr std::string_view kName = "Effects";
  void Run(World& world, const TickContext& context) SE_REQUIRES(g_sim_owner);
};

struct DeathRule {
  static constexpr std::string_view kName = "DeathRule";
  void Run(World& world, const TickContext& context) SE_REQUIRES(g_sim_owner);
};

struct ClearMovedSystem {
  static constexpr std::string_view kName = "ClearMoved";
  void Run(World& world, const TickContext& context) SE_REQUIRES(g_sim_owner);
};

// Registers the systems above in their phases.
void RegisterRampageSystems(Schedule& schedule);

// The sounds the rules ask for (config/sound.json names them; M8). Audio
// events never change simulation state.
inline constexpr std::uint32_t kSoundHit = Fnv1a32("hit");
inline constexpr std::uint32_t kSoundDeath = Fnv1a32("death");

// The buffer windows of the discrete actions, in ticks (6 to 12): jump 8,
// exit, enter and back 6, a1-a6 10.
[[nodiscard]] std::uint8_t ActionBufferTicks(ActionId action) noexcept;
// Whether |action| is a discrete action IntentSystem buffers.
[[nodiscard]] bool IsBufferedAction(ActionId action) noexcept;

// The pieces of the systems, for tests.

// The legacy sf::Angle conversions: a heading in degrees becomes radians
// (degrees * (pi / 180)) wrapped to [0, 2 pi) as sf::Angle::wrapUnsigned
// does; RotationDegrees is sf::Angle::asDegrees.
[[nodiscard]] float RotationFromHeading(float degrees) noexcept;
[[nodiscard]] float RotationDegrees(float radians) noexcept;

// R11: the position that keeps a box of half extents |half| inside
// |bounds|; each axis is computed from |pos|.
[[nodiscard]] Vec2 ClampToArena(Vec2 pos, Vec2 half,
                                const ArenaBounds& bounds) noexcept;
// legacy-ref-v1: the clamp against the rotated sprite bounds, where the last
// correction's setPosition wins (BoundarySystem.cpp before R11).
[[nodiscard]] Vec2 ClampToArenaV1(const Transform& transform, Vec2 half,
                                  const ArenaBounds& bounds) noexcept;

// Rebuilds the lengths and running totals of |path|'s segments.
void BuildPathCache(const Path& path, PathCache& cache);

// The number of segments of |path| (the closing one included).
[[nodiscard]] std::uint32_t SegmentCount(const Path& path) noexcept;

// One tick of R12 and R13: |rem| pixels along |path|.
struct PathStep {
  // False when the path does not move the entity (it is done, or all its
  // points coincide on a loop).
  bool active = false;
  // The point reached.
  Vec2 pos;
  // The heading of the segment the step ends on, or of the last segment
  // travelled when it reaches the end (degrees, unnormalised).
  bool has_heading = false;
  float heading = 0.0F;
};
[[nodiscard]] PathStep AdvancePath(Path& path, const PathCache& cache,
                                   float rem) noexcept;

}  // namespace se::rampage

#endif  // GAME_RAMPAGE_SYSTEMS_H_
