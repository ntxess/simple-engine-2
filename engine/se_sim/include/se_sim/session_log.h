// Copyright 2026 The simple-engine-2 Authors.
//
// The SessionLog (design change 19): every play-World mutation the Schedule
// did not produce (editor edits, spawns, asset swaps, GameState commands,
// Pause/Step/Stop), each applied at a recorded tick boundary. A replay of the
// InputFrames plus the SessionLog reproduces a session exactly.
//
// Entries queue against a tick (apply_before) and apply in (apply_before,
// seq) order at the boundary before that tick (SimDriver::RunTick calls
// ApplyDue). While the session is paused, Enqueue applies an entry at once
// and logs it against the next tick, which is where a replay applies it.
//
// SessionEntry::type is the component's stable name id
// (ComponentInfo::name_id), so logs read the same on every compiler.
// Payloads: a component's BEVE (kSetComponent, kAddComponent); a component
// bundle per entity (kCreateEntity, kSpawnBatch); the op and state id
// (kGameStateCommand); nothing otherwise.

#ifndef SE_SIM_SESSION_LOG_H_
#define SE_SIM_SESSION_LOG_H_

#include <cstddef>
#include <cstdint>
#include <entt/core/fwd.hpp>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/types.h"
#include "se_sim/game_state.h"

namespace se {

class ComponentRegistry;

enum class SessionOp : std::uint8_t {
  kSetComponent,
  kAddComponent,
  kRemoveComponent,
  kCreateEntity,
  kDestroyEntity,
  kSpawnBatch,
  kAssetSwap,
  kGameStateCommand,
  kPause,
  kResume,
  kStep,
  kStop,
};

struct SessionEntry {
  // Applies at the boundary before this tick.
  Tick apply_before = 0;
  // Order among entries with the same tick; assigned by the log.
  std::uint32_t seq = 0;
  SessionOp op = SessionOp::kSetComponent;
  // The entity (or asset) the entry is about.
  Guid target{};
  // Component name id (ComponentInfo::name_id), when the op has one.
  entt::id_type type = 0;
  std::vector<std::byte> payload{};
  // ContentHash(payload); checked before applying.
  std::uint64_t content_hash = 0;

  friend bool operator==(const SessionEntry&, const SessionEntry&) = default;
};

// The hash every entry carries of its payload.
[[nodiscard]] std::uint64_t ContentHash(std::span<const std::byte> payload);

// One component of an entity bundle: registered name and JSON value.
struct ComponentValue {
  std::string_view component;
  std::string json;
};

// Builders. Component values are given as JSON and stored as BEVE.
[[nodiscard]] std::expected<SessionEntry, Error> MakeComponentEntry(
    const ComponentRegistry& registry, SessionOp op, Tick apply_before,
    Guid target, std::string_view component, std::string_view json);
[[nodiscard]] std::expected<SessionEntry, Error> MakeRemoveComponent(
    const ComponentRegistry& registry, Tick apply_before, Guid target,
    std::string_view component);
[[nodiscard]] std::expected<SessionEntry, Error> MakeCreateEntity(
    const ComponentRegistry& registry, Tick apply_before, Guid target,
    std::span<const ComponentValue> components);
[[nodiscard]] std::expected<SessionEntry, Error> MakeSpawnBatch(
    const ComponentRegistry& registry, Tick apply_before,
    std::span<const std::pair<Guid, std::vector<ComponentValue>>> entities);
[[nodiscard]] SessionEntry MakeDestroyEntity(Tick apply_before, Guid target);
[[nodiscard]] SessionEntry MakeGameStateCommand(Tick apply_before, StackOp op,
                                                std::string_view state_id);
[[nodiscard]] SessionEntry MakeAssetSwap(Tick apply_before, Guid asset,
                                         std::uint64_t content_hash);
// kPause, kResume, kStep and kStop markers.
[[nodiscard]] SessionEntry MakeMarker(Tick apply_before, SessionOp op);

class SessionLog {
 public:
  // Queues a live entry against entry.apply_before; while the session is
  // paused (no running World) it applies at once instead.
  void Enqueue(SessionEntry entry, GameStateStack& states)
      SE_REQUIRES(g_sim_owner);
  // Queues an entry for its tick without applying it (scripted edits,
  // fixtures).
  void AddScheduled(SessionEntry entry);
  // Applies every queued entry with apply_before <= tick, in order.
  void ApplyDue(Tick tick, GameStateStack& states) SE_REQUIRES(g_sim_owner);

  [[nodiscard]] std::span<const SessionEntry> Applied() const noexcept {
    return applied_;
  }
  [[nodiscard]] std::size_t PendingCount() const noexcept {
    return pending_.size();
  }
  // Entries that could not apply (missing entity, bad payload).
  [[nodiscard]] std::uint32_t Failures() const noexcept { return failures_; }

  // Every entry, applied and pending, in (apply_before, seq) order.
  [[nodiscard]] std::vector<std::byte> Write() const;
  // Entries become pending; Applied() starts empty.
  [[nodiscard]] static std::expected<SessionLog, Error> Read(
      std::span<const std::byte> bytes);

 private:
  // Applies to the top state's Worlds; false when the entry could not apply.
  bool Apply(const SessionEntry& entry, GameStateStack& states)
      SE_REQUIRES(g_sim_owner);
  void Insert(SessionEntry entry);

  // Sorted by (apply_before, seq).
  std::vector<SessionEntry> pending_;
  std::vector<SessionEntry> applied_;
  std::uint32_t next_seq_ = 0;
  std::uint32_t failures_ = 0;
};

}  // namespace se

#endif  // SE_SIM_SESSION_LOG_H_
