// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/world.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "se_core/assert.h"
#include "se_core/component_registry.h"
#include "se_core/core_components.h"
#include "se_core/guid.h"
#include "se_core/hash.h"
#include "se_core/jobs.h"
#include "se_core/thread_role.h"

// EnTT's type ids must be generated with atomics once fork workers exist,
// and every translation unit must agree (docs/design/mt-design.md, EnTT).
#ifndef ENTT_USE_ATOMIC
#error "se_core requires ENTT_USE_ATOMIC in every translation unit"
#endif

namespace se {
namespace {

constexpr std::uint64_t kDeriveGuidSeed = 0x0D3121E5EED61D00ULL;
constexpr std::uint64_t kDeriveGuidLo = 0x10ADED61D5EC0DEULL;
constexpr std::uint64_t kEntityDigestSeed = 0xE7171D1635EED000ULL;

}  // namespace

MutationGate::MutationGate(World* world, GateReason reason) noexcept
    : world_(world), reason_(reason) {
  ++world_->open_gates_;
}

MutationGate::MutationGate(MutationGate&& other) noexcept
    : world_(std::exchange(other.world_, nullptr)), reason_(other.reason_) {}

MutationGate::~MutationGate() {
  if (world_ != nullptr) --world_->open_gates_;
}

World::World(const ComponentRegistry& registry, WorldDesc desc)
    : registry_(&registry),
      desc_(std::move(desc)),
      structure_generations_(registry.All().size(), 0),
      owner_(std::this_thread::get_id()) {
  SE_ASSERT_MSG(registry.Sealed(), "seal the ComponentRegistry first");
  SE_ASSERT_MSG(registry.Find(entt::type_hash<IdComponent>::value()) != nullptr,
                "register the core components (RegisterCoreComponents)");
  clock_.SetRate(desc_.tick_rate);
  static_cast<void>(reg_.storage<entt::entity>());
  for (const ComponentInfo& info : registry.All()) {
    info.precreate(reg_);
  }
#if SE_ASSERTS
  // The ParallelView structural-change check: a listener per storage. A
  // remove and an emplace (or a destroy and a create) keep the sizes but
  // still move entities in the packed arrays.
  const std::span<const ComponentInfo> infos = registry.All();
  for (std::size_t i = 0; i < infos.size(); ++i) {
    if (infos[i].watch_structure != nullptr) {
      infos[i].watch_structure(reg_, structure_generations_[i]);
    }
  }
#endif
  for (const ComponentInfo& info : registry.All()) {
    if (HasFlag(info.flags, ComponentFlags::kHashed)) {
      hashed_.emplace_back(&info, std::as_const(reg_).storage(info.id));
    }
  }
}

World::~World() = default;

void World::CheckAccess() const {
  SE_ASSERT_MSG(std::this_thread::get_id() == owner_,
                "World accessed from a thread that does not own it");
  SE_ASSERT_NOT_IN_FORK();
}

void World::CheckMutation() const {
  CheckAccess();
  SE_ASSERT_MSG(desc_.kind != WorldKind::kPlay || open_gates_ > 0,
                "Play World mutated while its MutationGate is closed");
}

entt::registry& World::Reg() {
  CheckMutation();
  hash_dirty_ = true;
  return reg_;
}

SimClock& World::MutableClock() {
  CheckMutation();
  return clock_;
}

entt::entity World::Create(Guid guid) {
  entt::registry& reg = Reg();
  SE_ASSERT_MSG(!guid.IsNil(), "an entity needs a Guid");
  SE_ASSERT_MSG(!guids_.contains(guid), "Guid already in use in this World");
  const entt::entity entity = reg.create();
  reg.emplace<IdComponent>(entity, guid);
  reg.emplace<Spawned>(entity, clock_.tick, spawn_serial_++);
  guids_.emplace(guid, entity);
  pending_spawns_.push_back(entity);
  return entity;
}

entt::entity World::Spawn(Guid parent, std::uint32_t index) {
  return Create(DeriveGuid(parent, clock_.tick, index));
}

Guid World::DeriveGuid(Guid parent, Tick tick, std::uint32_t index) noexcept {
  Hasher hasher(kDeriveGuidSeed);
  HashGuid(hasher, parent);
  hasher.AddU64(tick);
  hasher.AddU64(index);
  Guid guid;
  guid.hi = hasher.Finish();
  hasher.AddU64(kDeriveGuidLo);
  guid.lo = hasher.Finish();
  if (guid.IsNil()) guid.lo = 1;
  return guid;
}

void World::RequestDestroy(entt::entity entity) {
  entt::registry& reg = Reg();
  SE_ASSERT_MSG(reg.valid(entity), "RequestDestroy of an invalid entity");
  reg.emplace_or_replace<DestroyRequest>(entity);
}

void World::DrainDestroyRequests() {
  entt::registry& reg = Reg();
  auto& requests = reg.storage<DestroyRequest>();
  if (requests.empty()) return;
  std::vector<std::pair<Guid, entt::entity>> doomed;
  doomed.reserve(requests.size());
  for (const entt::entity entity : requests) {
    doomed.emplace_back(reg.get<IdComponent>(entity).guid, entity);
  }
  std::sort(doomed.begin(), doomed.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& [guid, entity] : doomed) {
    guids_.erase(guid);
    reg.destroy(entity);
  }
}

void World::FinalizeSpawns() {
  entt::registry& reg = Reg();
  for (const entt::entity entity : pending_spawns_) {
    if (!reg.valid(entity)) continue;
    if (Transform* transform = reg.try_get<Transform>(entity)) {
      transform->teleport_tick = reg.get<Spawned>(entity).spawn_tick;
      reg.emplace_or_replace<PrevTransform>(entity, transform->pos,
                                            transform->rot, transform->scale,
                                            clock_.tick);
    }
  }
  pending_spawns_.clear();
}

std::optional<entt::entity> World::Find(Guid guid) const {
  const auto it = guids_.find(guid);
  if (it == guids_.end()) return std::nullopt;
  return it->second;
}

std::size_t World::EntityCount() const noexcept { return guids_.size(); }

const std::uint64_t* World::StructureGeneration(
    entt::id_type id) const noexcept {
#if SE_ASSERTS
  const ComponentInfo* info = registry_->Find(id);
  if (info == nullptr || info->watch_structure == nullptr) return nullptr;
  const auto index = static_cast<std::size_t>(info - registry_->All().data());
  return &structure_generations_[index];
#else
  static_cast<void>(id);
  return nullptr;
#endif
}

std::uint64_t World::EntityDigestOf(entt::entity entity,
                                    const Guid& guid) const {
  Hasher hasher(kEntityDigestSeed);
  HashGuid(hasher, guid);
  for (const auto& [info, storage] : hashed_) {
    if (!storage->contains(entity)) continue;
    hasher.AddU64(info->name_id);
    info->hash(storage->value(entity), hasher);
  }
  return hasher.Finish();
}

WorldHash World::Hash() const {
  WorldHash hash;
  hash.tick = clock_.tick;
  for (const auto [entity, id] : reg_.storage<IdComponent>()->each()) {
    hash.Fold(EntityDigestOf(entity, id.guid));
  }
  return hash;
}

std::vector<EntityDigest> World::Digests() const {
  std::vector<EntityDigest> digests;
  const auto* ids = reg_.storage<IdComponent>();
  digests.reserve(ids->size());
  for (const auto [entity, id] : ids->each()) {
    digests.push_back({id.guid, EntityDigestOf(entity, id.guid)});
  }
  std::sort(digests.begin(), digests.end(),
            [](const EntityDigest& a, const EntityDigest& b) {
              return a.guid < b.guid;
            });
  return digests;
}

void World::RecordHash() {
  CheckMutation();
  last_hash_ = Hash();
  hash_dirty_ = false;
}

std::unique_ptr<World> World::Clone() const {
  // Reading the source needs ownership, not an open gate: a Play World can
  // be cloned between ticks (--det-crosscheck).
  CheckAccess();
  auto copy = std::make_unique<World>(*registry_, desc_);
  // The entity storage: same ids and versions, same packed order, same free
  // list, so later creates hand out the same ids as in the original.
  const auto& from_entities = *reg_.storage<entt::entity>();
  auto& to_entities = copy->reg_.storage<entt::entity>();
  const entt::sparse_set& packed = from_entities;
  to_entities.push(packed.data(), packed.data() + packed.size());
  to_entities.free_list(from_entities.free_list());
  for (const ComponentInfo& info : registry_->All()) {
    info.clone(reg_, copy->reg_);
  }
  copy->clock_ = clock_;
  copy->guids_ = guids_;
  copy->pending_spawns_ = pending_spawns_;
  copy->spawn_serial_ = spawn_serial_;
  copy->owner_ = owner_;
  copy->last_hash_ = last_hash_;
  copy->hash_dirty_ = hash_dirty_;
  return copy;
}

MutationGate World::OpenGate(GateReason reason) {
  SE_ASSERT_MSG(
      std::this_thread::get_id() == owner_,
      "MutationGate opened from a thread that does not own the World");
  return MutationGate(this, reason);
}

entt::entity EntityRef::Resolve(const World& world) const {
  const entt::registry& reg = world.CReg();
  if (cached != entt::null && reg.valid(cached)) {
    const auto* id = reg.try_get<IdComponent>(cached);
    if (id != nullptr && id->guid == guid) return cached;
  }
  const std::optional<entt::entity> found = world.Find(guid);
  cached = found.value_or(entt::null);
  return cached;
}

}  // namespace se
