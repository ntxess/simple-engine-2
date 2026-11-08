// Copyright 2026 The simple-engine-2 Authors.

#include "se_sim/session_log.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/component_registry.h"
#include "se_core/error.h"
#include "se_core/hash.h"
#include "se_core/log.h"
#include "se_core/world.h"
#include "se_sim/bytes.h"

namespace se {
namespace {

constexpr std::uint64_t kPayloadSeed = 0x5E5510A1C0DE0001ULL;
constexpr std::uint32_t kMagic = 0x4C534553;  // "SESL"
constexpr std::uint32_t kVersion = 1;

bool EntryLess(const SessionEntry& a, const SessionEntry& b) {
  return a.apply_before != b.apply_before ? a.apply_before < b.apply_before
                                          : a.seq < b.seq;
}

SessionEntry Finish(SessionEntry entry) {
  entry.content_hash = ContentHash(entry.payload);
  return entry;
}

// A component's value as BEVE, converted from JSON in a scratch registry.
std::expected<std::vector<std::byte>, Error> ComponentBeve(
    const ComponentInfo& info, std::string_view json) {
  entt::registry scratch;
  info.precreate(scratch);
  const entt::entity e = scratch.create();
  if (auto read = info.read_json(scratch, e, json); !read) {
    return std::unexpected(read.error());
  }
  return info.write_beve(scratch, e);
}

// [u32 count] then [u32 name_id][u32 size][BEVE] per component.
std::expected<void, Error> EncodeBundle(
    const ComponentRegistry& registry,
    std::span<const ComponentValue> components, ByteWriter& out) {
  out.U32(static_cast<std::uint32_t>(components.size()));
  for (const ComponentValue& value : components) {
    const ComponentInfo* info = registry.FindByName(value.component);
    if (info == nullptr) {
      return Fail(ErrorCode::kNotFound,
                  "unknown component '" + std::string(value.component) + "'");
    }
    auto beve = ComponentBeve(*info, value.json);
    if (!beve) return std::unexpected(beve.error());
    out.U32(info->name_id);
    out.SizedBytes(*beve);
  }
  return {};
}

bool ApplyBundle(World& world, entt::entity entity, ByteReader& in)
    SE_REQUIRES(g_sim_owner) {
  std::uint32_t count = 0;
  if (!in.U32(&count)) return false;
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint32_t name_id = 0;
    std::vector<std::byte> beve;
    if (!in.U32(&name_id) || !in.SizedBytes(&beve)) return false;
    const ComponentInfo* info = world.Registry().FindByNameId(name_id);
    if (info == nullptr) return false;
    if (!info->read_beve(world.Reg(), entity, beve)) return false;
  }
  return true;
}

std::optional<std::pair<World*, entt::entity>> FindTarget(
    const std::vector<World*>& worlds, Guid target) {
  for (World* world : worlds) {
    if (const auto entity = world->Find(target)) {
      return std::make_pair(world, *entity);
    }
  }
  return std::nullopt;
}

}  // namespace

std::uint64_t ContentHash(std::span<const std::byte> payload) {
  Hasher hasher(kPayloadSeed);
  hasher.AddBytes(payload);
  return hasher.Finish();
}

std::expected<SessionEntry, Error> MakeComponentEntry(
    const ComponentRegistry& registry, SessionOp op, Tick apply_before,
    Guid target, std::string_view component, std::string_view json) {
  const ComponentInfo* info = registry.FindByName(component);
  if (info == nullptr) {
    return Fail(ErrorCode::kNotFound,
                "unknown component '" + std::string(component) + "'");
  }
  auto beve = ComponentBeve(*info, json);
  if (!beve) return std::unexpected(beve.error());
  SessionEntry entry;
  entry.apply_before = apply_before;
  entry.op = op;
  entry.target = target;
  entry.type = info->name_id;
  entry.payload = std::move(*beve);
  return Finish(std::move(entry));
}

std::expected<SessionEntry, Error> MakeRemoveComponent(
    const ComponentRegistry& registry, Tick apply_before, Guid target,
    std::string_view component) {
  const ComponentInfo* info = registry.FindByName(component);
  if (info == nullptr) {
    return Fail(ErrorCode::kNotFound,
                "unknown component '" + std::string(component) + "'");
  }
  SessionEntry entry;
  entry.apply_before = apply_before;
  entry.op = SessionOp::kRemoveComponent;
  entry.target = target;
  entry.type = info->name_id;
  return Finish(std::move(entry));
}

std::expected<SessionEntry, Error> MakeCreateEntity(
    const ComponentRegistry& registry, Tick apply_before, Guid target,
    std::span<const ComponentValue> components) {
  ByteWriter out;
  if (auto encoded = EncodeBundle(registry, components, out); !encoded) {
    return std::unexpected(encoded.error());
  }
  SessionEntry entry;
  entry.apply_before = apply_before;
  entry.op = SessionOp::kCreateEntity;
  entry.target = target;
  entry.payload = out.Take();
  return Finish(std::move(entry));
}

std::expected<SessionEntry, Error> MakeSpawnBatch(
    const ComponentRegistry& registry, Tick apply_before,
    std::span<const std::pair<Guid, std::vector<ComponentValue>>> entities) {
  ByteWriter out;
  out.U32(static_cast<std::uint32_t>(entities.size()));
  for (const auto& [guid, components] : entities) {
    out.U64(guid.hi);
    out.U64(guid.lo);
    if (auto encoded = EncodeBundle(registry, components, out); !encoded) {
      return std::unexpected(encoded.error());
    }
  }
  SessionEntry entry;
  entry.apply_before = apply_before;
  entry.op = SessionOp::kSpawnBatch;
  entry.payload = out.Take();
  return Finish(std::move(entry));
}

SessionEntry MakeDestroyEntity(Tick apply_before, Guid target) {
  SessionEntry entry;
  entry.apply_before = apply_before;
  entry.op = SessionOp::kDestroyEntity;
  entry.target = target;
  return Finish(std::move(entry));
}

SessionEntry MakeGameStateCommand(Tick apply_before, StackOp op,
                                  std::string_view state_id) {
  ByteWriter out;
  out.U8(static_cast<std::uint8_t>(op));
  out.String(state_id);
  SessionEntry entry;
  entry.apply_before = apply_before;
  entry.op = SessionOp::kGameStateCommand;
  entry.payload = out.Take();
  return Finish(std::move(entry));
}

SessionEntry MakeAssetSwap(Tick apply_before, Guid asset,
                           std::uint64_t content_hash) {
  ByteWriter out;
  out.U64(content_hash);
  SessionEntry entry;
  entry.apply_before = apply_before;
  entry.op = SessionOp::kAssetSwap;
  entry.target = asset;
  entry.payload = out.Take();
  return Finish(std::move(entry));
}

SessionEntry MakeMarker(Tick apply_before, SessionOp op) {
  SessionEntry entry;
  entry.apply_before = apply_before;
  entry.op = op;
  return Finish(std::move(entry));
}

void SessionLog::Insert(SessionEntry entry) {
  const auto at =
      std::upper_bound(pending_.begin(), pending_.end(), entry, EntryLess);
  pending_.insert(at, std::move(entry));
}

void SessionLog::Enqueue(SessionEntry entry, Tick next_tick,
                         GameStateStack& states) {
  entry.seq = next_seq_++;
  if (!states.SimRunning()) {
    // Paused at the boundary before next_tick. Finish that boundary's
    // pending work in RunTick's order, which is also a replay's order, then
    // apply the entry there.
    states.ApplyPending();
    ApplyDue(next_tick, states);
    entry.apply_before = next_tick;
    if (!Apply(entry, states)) ++failures_;
    applied_.push_back(std::move(entry));
    return;
  }
  entry.apply_before = std::max(entry.apply_before, next_tick);
  Insert(std::move(entry));
}

void SessionLog::AddScheduled(SessionEntry entry) {
  entry.seq = next_seq_++;
  Insert(std::move(entry));
}

void SessionLog::ApplyDue(Tick tick, GameStateStack& states) {
  std::size_t due = 0;
  while (due < pending_.size() && pending_[due].apply_before <= tick) {
    if (!Apply(pending_[due], states)) ++failures_;
    applied_.push_back(std::move(pending_[due]));
    ++due;
  }
  pending_.erase(pending_.begin(),
                 pending_.begin() + static_cast<std::ptrdiff_t>(due));
}

bool SessionLog::Apply(const SessionEntry& entry, GameStateStack& states) {
  if (ContentHash(entry.payload) != entry.content_hash) {
    SE_LOG_ERROR("session entry {} (tick {}) fails its content hash", entry.seq,
                 entry.apply_before);
    return false;
  }
  if (entry.op == SessionOp::kGameStateCommand) {
    ByteReader in(entry.payload);
    std::uint8_t op = 0;
    std::string state_id;
    if (!in.U8(&op) || !in.String(&state_id) ||
        op > static_cast<std::uint8_t>(StackOp::kPop)) {
      return false;
    }
    states.Request(static_cast<StackOp>(op), state_id);
    states.ApplyPending();
    return true;
  }
  if (entry.op == SessionOp::kAssetSwap || entry.op == SessionOp::kPause ||
      entry.op == SessionOp::kResume || entry.op == SessionOp::kStep ||
      entry.op == SessionOp::kStop) {
    // Markers for the editor's Play session and asset swaps (M9) carry no
    // World change in the kernel.
    return true;
  }

  const std::vector<World*> worlds = states.TopWorlds();
  if (worlds.empty()) return false;

  if (entry.op == SessionOp::kCreateEntity) {
    World& world = *worlds.front();
    if (world.Find(entry.target)) return false;
    const MutationGate gate = world.OpenGate(GateReason::kSessionLog);
    const entt::entity entity = world.Create(entry.target);
    ByteReader in(entry.payload);
    const bool ok = ApplyBundle(world, entity, in);
    world.FinalizeSpawns();
    return ok;
  }
  if (entry.op == SessionOp::kSpawnBatch) {
    World& world = *worlds.front();
    const MutationGate gate = world.OpenGate(GateReason::kSessionLog);
    ByteReader in(entry.payload);
    std::uint32_t count = 0;
    if (!in.U32(&count)) return false;
    bool ok = true;
    for (std::uint32_t i = 0; i < count && ok; ++i) {
      Guid guid;
      if (!in.U64(&guid.hi) || !in.U64(&guid.lo) || world.Find(guid)) {
        ok = false;
        break;
      }
      ok = ApplyBundle(world, world.Create(guid), in);
    }
    world.FinalizeSpawns();
    return ok;
  }

  const auto found = FindTarget(worlds, entry.target);
  if (!found) return false;
  auto [world, entity] = *found;
  const MutationGate gate = world->OpenGate(GateReason::kSessionLog);
  if (entry.op == SessionOp::kDestroyEntity) {
    world->RequestDestroy(entity);
    world->DrainDestroyRequests();
    return true;
  }
  const ComponentInfo* info = world->Registry().FindByNameId(entry.type);
  if (info == nullptr) return false;
  const bool present = world->CReg().storage(info->id)->contains(entity);
  switch (entry.op) {
    case SessionOp::kSetComponent:
      return info->read_beve(world->Reg(), entity, entry.payload).has_value();
    case SessionOp::kAddComponent:
      if (present) return false;
      return info->read_beve(world->Reg(), entity, entry.payload).has_value();
    case SessionOp::kRemoveComponent:
      if (!present || info->name == "IdComponent") return false;
      return world->Reg().storage(info->id)->remove(entity);
    default:
      return false;
  }
}

std::vector<std::byte> SessionLog::Write() const {
  std::vector<const SessionEntry*> all;
  all.reserve(applied_.size() + pending_.size());
  for (const SessionEntry& e : applied_) all.push_back(&e);
  for (const SessionEntry& e : pending_) all.push_back(&e);
  std::stable_sort(all.begin(), all.end(),
                   [](const SessionEntry* a, const SessionEntry* b) {
                     return EntryLess(*a, *b);
                   });
  ByteWriter out;
  out.U32(kMagic);
  out.U32(kVersion);
  out.U32(static_cast<std::uint32_t>(all.size()));
  for (const SessionEntry* e : all) {
    out.U64(e->apply_before);
    out.U32(e->seq);
    out.U8(static_cast<std::uint8_t>(e->op));
    out.U64(e->target.hi);
    out.U64(e->target.lo);
    out.U32(e->type);
    out.U64(e->content_hash);
    out.SizedBytes(e->payload);
  }
  return out.Take();
}

std::expected<SessionLog, Error> SessionLog::Read(
    std::span<const std::byte> bytes) {
  ByteReader in(bytes);
  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  std::uint32_t count = 0;
  if (!in.U32(&magic) || magic != kMagic) {
    return Fail(ErrorCode::kCorrupt, "not a session log");
  }
  if (!in.U32(&version) || version != kVersion) {
    return Fail(ErrorCode::kVersion, "unsupported session log version");
  }
  if (!in.U32(&count)) return Fail(ErrorCode::kCorrupt, "truncated log");
  SessionLog log;
  for (std::uint32_t i = 0; i < count; ++i) {
    SessionEntry e;
    std::uint8_t op = 0;
    std::uint32_t type = 0;
    if (!in.U64(&e.apply_before) || !in.U32(&e.seq) || !in.U8(&op) ||
        !in.U64(&e.target.hi) || !in.U64(&e.target.lo) || !in.U32(&type) ||
        !in.U64(&e.content_hash) || !in.SizedBytes(&e.payload) ||
        op > static_cast<std::uint8_t>(SessionOp::kStop)) {
      return Fail(ErrorCode::kCorrupt, "truncated or invalid session entry");
    }
    e.op = static_cast<SessionOp>(op);
    e.type = type;
    log.next_seq_ = std::max(log.next_seq_, e.seq + 1);
    log.Insert(std::move(e));
  }
  if (!in.AtEnd()) return Fail(ErrorCode::kCorrupt, "trailing bytes in log");
  return log;
}

}  // namespace se
