// Copyright 2026 The simple-engine-2 Authors.
//
// World: the only container of simulation state (docs/design/
// costfree-end-state.md "World and state domain"): an EnTT registry, a
// SimClock and the Guid index. Systems keep no state of their own, so
// cloning, hashing or destroying a World covers everything.
//
// Contracts checked at run time (SE_ASSERTS) and by the analysis:
//   - structural calls and Reg() require g_sim_owner (ADR-007) and assert the
//     World's owner thread and not-in-fork;
//   - a Play World may only be mutated while a MutationGate is open (the
//     Schedule opens one per tick, the SessionLog one per applied entry,
//     the GameStateStack one when it takes the World's queued commands);
//     reading it, Clone() included, needs no gate;
//   - every registered storage is created in the constructor, so nothing
//     creates a storage lazily later (EnTT's non-const access would);
//   - destroys are requested and drained in Cleanup, followed by
//     FinalizeSpawns, which sets PrevTransform = Transform and
//     teleport_tick = spawn_tick for every entity created in the tick.
//
// The hash is order-independent (ADR-011): per-entity digests of the Guid
// and every kHashed component, folded by sum and xor.

#ifndef SE_CORE_WORLD_H_
#define SE_CORE_WORLD_H_

#include <cstddef>
#include <cstdint>
#include <entt/core/type_info.hpp>
#include <entt/entity/registry.hpp>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "se_core/component_registry.h"
#include "se_core/guid.h"
#include "se_core/hash.h"
#include "se_core/jobs.h"
#include "se_core/parallel_view.h"
#include "se_core/sim_clock.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/types.h"

namespace se {

enum class WorldKind : std::uint8_t {
  kRuntime,
  kEditDocument,
  // An editor Play session: mutations need an open MutationGate.
  kPlay,
};

enum class GateReason : std::uint8_t {
  kTick,
  kSessionLog,
  kLoad,
  // GameStateStack taking the GameState commands systems queued in the World
  // (a tick boundary).
  kStateChange,
  kEditorCommand,
  kTest,
};

struct WorldDesc {
  std::string name{};
  WorldKind kind = WorldKind::kRuntime;
  std::uint64_t seed = 0;
  std::uint32_t tick_rate = kDefaultTickRate;
};

// One entity's digest, for --det-crosscheck.
struct EntityDigest {
  Guid guid;
  std::uint64_t digest = 0;

  friend bool operator==(const EntityDigest&, const EntityDigest&) = default;
};

class World;

// Allows mutation of a Play World while it lives (World::OpenGate).
class MutationGate {
 public:
  MutationGate(MutationGate&& other) noexcept;
  MutationGate& operator=(MutationGate&&) = delete;
  MutationGate(const MutationGate&) = delete;
  MutationGate& operator=(const MutationGate&) = delete;
  ~MutationGate();

  [[nodiscard]] GateReason Reason() const noexcept { return reason_; }

 private:
  friend class World;
  MutationGate(World* world, GateReason reason) noexcept;

  World* world_;
  GateReason reason_;
};

class World {
 public:
  // The registry must be sealed and contain the core components. The
  // constructing thread becomes the owner.
  World(const ComponentRegistry& registry, WorldDesc desc);
  ~World();
  World(const World&) = delete;
  World& operator=(const World&) = delete;

  // Creates an entity with IdComponent{guid} and Spawned, and marks it for
  // FinalizeSpawns. The Guid must be new and not nil.
  entt::entity Create(Guid guid) SE_REQUIRES(g_sim_owner);
  // Creates an entity whose Guid derives from (parent, current tick, index).
  entt::entity Spawn(Guid parent, std::uint32_t index) SE_REQUIRES(g_sim_owner);
  // The runtime Guid rule (design change 19), with fixed constants:
  //   h = Hasher(kDeriveGuidSeed); h.Add(parent.hi, parent.lo, tick, index)
  //   hi = h.Finish(); h.Add(kDeriveGuidLo); lo = h.Finish()
  [[nodiscard]] static Guid DeriveGuid(Guid parent, Tick tick,
                                       std::uint32_t index) noexcept;

  // Marks an entity for destruction in Cleanup.
  void RequestDestroy(entt::entity entity) SE_REQUIRES(g_sim_owner);
  // Destroys every requested entity, in Guid order.
  void DrainDestroyRequests() SE_REQUIRES(g_sim_owner);
  // prev = current and teleport_tick = spawn_tick for entities created since
  // the last call. Cleanup runs it every tick; loaders run it after a load.
  void FinalizeSpawns() SE_REQUIRES(g_sim_owner);

  // The registry, for systems and loaders.
  [[nodiscard]] entt::registry& Reg() SE_REQUIRES(g_sim_owner);
  // Read-only access; no checks.
  [[nodiscard]] const entt::registry& CReg() const noexcept { return reg_; }

  [[nodiscard]] std::optional<entt::entity> Find(Guid guid) const;
  [[nodiscard]] std::size_t EntityCount() const noexcept;

  // Captures the storages of C... for fork chunks (owner thread). Debug
  // builds assert at each use that none of them changed structurally since.
  template <class... C>
  [[nodiscard]] ParallelView<C...> Parallel(const jobs::ForkSite& site)
      SE_REQUIRES(g_sim_owner) {
    entt::registry& reg = Reg();
    return ParallelView<C...>(
        reg, site, {StructureGeneration(entt::type_hash<C>::value())...});
  }
  // The number of constructions and destructions so far in the storage
  // |id| (debug builds, from EnTT's storage signals); nullptr when nothing
  // counts them (release builds, or no watch_structure hook).
  [[nodiscard]] const std::uint64_t* StructureGeneration(
      entt::id_type id) const noexcept;

  // The hash of the current state.
  [[nodiscard]] WorldHash Hash() const;
  // Per-entity digests sorted by Guid.
  [[nodiscard]] std::vector<EntityDigest> Digests() const;
  // The hash recorded at the end of the last Schedule step, and whether the
  // World was mutated since (Reg() marks it).
  [[nodiscard]] const WorldHash& LastHash() const noexcept {
    return last_hash_;
  }
  [[nodiscard]] bool HashDirty() const noexcept { return hash_dirty_; }
  // Records Hash() as LastHash() and clears the dirty mark.
  void RecordHash() SE_REQUIRES(g_sim_owner);

  // A deep copy: same entity ids, packed orders, Guids and clock. The
  // registry's context variables are not copied.
  [[nodiscard]] std::unique_ptr<World> Clone() const SE_REQUIRES(g_sim_owner);

  [[nodiscard]] MutationGate OpenGate(GateReason reason);
  [[nodiscard]] bool GateOpen() const noexcept { return open_gates_ > 0; }

  [[nodiscard]] std::thread::id Owner() const noexcept { return owner_; }
  void SetOwner(std::thread::id owner) noexcept { owner_ = owner; }

  [[nodiscard]] const SimClock& Clock() const noexcept { return clock_; }
  [[nodiscard]] SimClock& MutableClock() SE_REQUIRES(g_sim_owner);

  [[nodiscard]] const WorldDesc& Desc() const noexcept { return desc_; }
  [[nodiscard]] const ComponentRegistry& Registry() const noexcept {
    return *registry_;
  }

 private:
  friend class MutationGate;

  // The owner-thread and not-in-fork checks of every registry access.
  void CheckAccess() const;
  // CheckAccess() plus the Play-World gate check of Reg().
  void CheckMutation() const;
  [[nodiscard]] std::uint64_t EntityDigestOf(entt::entity entity,
                                             const Guid& guid) const;

  const ComponentRegistry* registry_;
  WorldDesc desc_;
  // Per registered component, in registration order: StructureGeneration().
  // Sized once, so the signal listeners' addresses stay valid, and declared
  // before reg_ so it outlives the registry.
  std::vector<std::uint64_t> structure_generations_;
  entt::registry reg_;
  SimClock clock_;
  std::unordered_map<Guid, entt::entity, GuidHash> guids_;
  std::vector<entt::entity> pending_spawns_;
  std::uint32_t spawn_serial_ = 0;
  std::thread::id owner_;
  int open_gates_ = 0;
  WorldHash last_hash_;
  bool hash_dirty_ = true;
  // Hashed components and their storages, in registration order.
  std::vector<std::pair<const ComponentInfo*, const entt::sparse_set*>> hashed_;
};

}  // namespace se

#endif  // SE_CORE_WORLD_H_
