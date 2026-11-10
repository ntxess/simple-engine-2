// Copyright 2026 The simple-engine-2 Authors.
//
// The stress generator (the legacy Debug panel's Generate, Replace and
// Remove buttons, app/editor/Editor.cpp:933-984 at master), kept as a stress
// tool (docs/design/costfree-end-state.md "Kept From Todays Code").
//
// The spawn spec is the ported enemy archetype (Editor.cpp:966-980, also
// assets/prefabs/enemy.prefab.json since M3): a list of components as the
// ComponentRegistry's JSON, prefab-like, into which each spawn writes its
// Transform position and its Path's points. Each spawn also carries the
// derived components a loader adds (PathCache, ActiveEffects; rampage's
// FinishLoad), so the PathSystem moves it from its first tick.
//
// Distributions (decided here, ADR-102; the legacy ones are written down in
// docs/ledger.md "Stress generator distributions"): spawn i of press p draws
// from rng::Stream(seed, p, StressGuid(seed, p, i), kStressRngStream):
//   - its point count N = 1 + Below(max_points) (uniform on 1..max_points;
//     the legacy loop redrew its bound and gave a mean of 3.66);
//   - each point x = Below(width), then y = Below(height), whole pixels,
//     uniform (the legacy folded y and doubled x = 0);
//   - the entity starts at its first point.
// Guids are StressGuid(seed, press, index): a fixed hash, so a press makes
// the same entities on every machine, and presses never collide.
//
// Spreading: a press of n spawns becomes ceil(n / per_tick) batches of at
// most per_tick spawns; batch k applies k ticks after the first (Play: the
// SessionLog's kSpawnBatch entries against next tick + k) or k frames after
// the press (Edit: create-entity commands, one coalesced undo step).

#ifndef SE_EDITOR_PANELS_STRESS_GENERATOR_H_
#define SE_EDITOR_PANELS_STRESS_GENERATOR_H_

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "se_core/guid.h"
#include "se_core/types.h"

namespace se::editor {

inline constexpr std::uint32_t kStressRngStream = 0x53545200U;  // "STR\0"
inline constexpr std::uint32_t kStressDefaultPerTick = 64;

// One component of a spawn: registered name and JSON value.
struct SpawnComponent {
  std::string component;
  std::string json;
  friend bool operator==(const SpawnComponent&,
                         const SpawnComponent&) = default;
};

struct StressSpec {
  // The archetype; Transform and Path are written per spawn.
  std::vector<SpawnComponent> components;
  float width = 1920.0F;
  float height = 1080.0F;
  std::uint32_t max_points = 10;
};

// The ported enemy archetype in the arena |width| x |height|.
[[nodiscard]] StressSpec DefaultStressSpec(float width = 1920.0F,
                                           float height = 1080.0F);

struct StressSpawn {
  Guid guid;
  std::vector<Vec2> points;
  // The spec's components with Transform and Path filled in, in the spec's
  // order (Transform first).
  std::vector<SpawnComponent> components;
};

// The Guid of spawn |index| of press |press|.
[[nodiscard]] Guid StressGuid(std::uint64_t seed, std::uint32_t press,
                              std::uint32_t index) noexcept;

// The paths of press |press| (the distributions above).
[[nodiscard]] std::vector<std::vector<Vec2>> StressPaths(const StressSpec& spec,
                                                         std::uint64_t seed,
                                                         std::uint32_t press,
                                                         std::uint32_t count);

// The spawns of press |press|: Guids, paths and component JSON.
[[nodiscard]] std::vector<StressSpawn> PlanStress(const StressSpec& spec,
                                                  std::uint64_t seed,
                                                  std::uint32_t press,
                                                  std::uint32_t count);

// One batch: spawns [first, first + count), applied |offset| ticks (or
// frames) after the first batch.
struct SpawnBatch {
  std::uint32_t first = 0;
  std::uint32_t count = 0;
  std::uint32_t offset = 0;
  friend constexpr bool operator==(const SpawnBatch&,
                                   const SpawnBatch&) = default;
};

// Splits |count| spawns into batches of at most |per_tick| (0 counts as 1).
[[nodiscard]] std::vector<SpawnBatch> SpreadSpawns(std::uint32_t count,
                                                   std::uint32_t per_tick);

}  // namespace se::editor

#endif  // SE_EDITOR_PANELS_STRESS_GENERATOR_H_
