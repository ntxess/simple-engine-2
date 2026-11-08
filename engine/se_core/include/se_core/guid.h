// Copyright 2026 The simple-engine-2 Authors.
//
// Stable entity identity. Every World entity carries an IdComponent{Guid};
// the world hash, the SessionLog and replays refer to entities by Guid, never
// by entt::entity (whose ids and versions depend on storage history).
// Runtime Guids derive from (parent Guid, tick, spawn index):
// World::DeriveGuid.

#ifndef SE_CORE_GUID_H_
#define SE_CORE_GUID_H_

#include <compare>
#include <cstddef>
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <expected>
#include <string>
#include <string_view>

#include "se_core/error.h"
#include "se_core/hash.h"

namespace se {

class World;

struct Guid {
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;

  [[nodiscard]] constexpr bool IsNil() const noexcept {
    return hi == 0 && lo == 0;
  }

  auto operator<=>(const Guid&) const = default;
};

// Formats as 8-4-4-4-12 lower-case hex digits of hi then lo, for example
// "0123abcd-0000-0000-0000-000000000001".
[[nodiscard]] std::string ToString(const Guid& guid);

// Parses the ToString() form.
[[nodiscard]] std::expected<Guid, Error> ParseGuid(std::string_view text);

// Adds a Guid to a running hash.
constexpr void HashGuid(Hasher& hasher, const Guid& guid) noexcept {
  hasher.AddU64(guid.hi);
  hasher.AddU64(guid.lo);
}

// For unordered containers. Lookups only: iteration order of such a
// container must never reach the simulation.
struct GuidHash {
  [[nodiscard]] std::size_t operator()(const Guid& guid) const noexcept {
    return static_cast<std::size_t>(Mix64(guid.hi ^ Mix64(guid.lo)));
  }
};

// A cross-entity link: the Guid is the identity, the cached entity a lookup
// hint that Resolve() validates on every use.
struct EntityRef {
  Guid guid;
  mutable entt::entity cached = entt::null;

  // The entity with |guid| in |world|, or entt::null.
  [[nodiscard]] entt::entity Resolve(const World& world) const;
};

}  // namespace se

#endif  // SE_CORE_GUID_H_
