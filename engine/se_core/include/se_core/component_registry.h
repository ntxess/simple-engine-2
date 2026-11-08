// Copyright 2026 The simple-engine-2 Authors.
//
// The component registry: one entry per component type with a stable name,
// a version, flags and type-erased hooks for storage pre-creation, JSON and
// BEVE serialization, cloning and hashing (docs/design/costfree-end-state.md
// "Serialization, reflection and config").
//
// Add<T>() is declared here and defined in se_core/reflect_glaze.h, which
// generates the hooks with glaze. glaze is heavy to compile (ADR-010), so
// only translation units named *_components.cc include that header (checked
// by tools/checks/check_glaze.py); everything else uses the hooks.
//
// Two ids per component: ComponentInfo::id is EnTT's storage id
// (entt::type_hash<T>, which differs between compilers), name_id the 32-bit
// FNV-1a hash of the registered name, stable everywhere and used in files,
// session logs and world hashes.

#ifndef SE_CORE_COMPONENT_REGISTRY_H_
#define SE_CORE_COMPONENT_REGISTRY_H_

#include <cstddef>
#include <cstdint>
#include <entt/core/fwd.hpp>
#include <entt/entity/fwd.hpp>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/error.h"
#include "se_core/hash.h"

namespace se {

enum class ComponentFlags : std::uint32_t {
  kNone = 0,
  // Part of the world hash.
  kHashed = 1U << 0,
  // Saved in scenes and snapshots.
  kSerialized = 1U << 1,
  // Interpolated by the renderer in Free cadence.
  kInterpolated = 1U << 2,
  // Shown in the editor's inspector.
  kEditorVisible = 1U << 3,
};

[[nodiscard]] constexpr ComponentFlags operator|(ComponentFlags a,
                                                 ComponentFlags b) noexcept {
  return static_cast<ComponentFlags>(static_cast<std::uint32_t>(a) |
                                     static_cast<std::uint32_t>(b));
}

[[nodiscard]] constexpr bool HasFlag(ComponentFlags flags,
                                     ComponentFlags flag) noexcept {
  return (static_cast<std::uint32_t>(flags) &
          static_cast<std::uint32_t>(flag)) != 0;
}

struct ComponentInfo {
  // The registered name; a string literal (the registry keeps the view).
  std::string_view name;
  std::uint32_t version = 1;
  // EnTT's storage id for the type.
  entt::id_type id = 0;
  // Fnv1a32(name): the stable id in files, session logs and hashes.
  std::uint32_t name_id = 0;
  ComponentFlags flags = ComponentFlags::kNone;
  // A tag type without data.
  bool empty = false;

  // Creates the type's storage, so later access never creates one lazily.
  void (*precreate)(entt::registry& registry) = nullptr;
  // Adds or replaces the component on |entity| from JSON.
  std::expected<void, Error> (*read_json)(entt::registry& registry,
                                          entt::entity entity,
                                          std::string_view json) = nullptr;
  std::expected<std::string, Error> (*write_json)(
      const entt::registry& registry, entt::entity entity) = nullptr;
  // The same with glaze's BEVE binary format.
  std::expected<void, Error> (*read_beve)(
      entt::registry& registry, entt::entity entity,
      std::span<const std::byte> bytes) = nullptr;
  std::expected<std::vector<std::byte>, Error> (*write_beve)(
      const entt::registry& registry, entt::entity entity) = nullptr;
  // Copies the whole storage from one registry to another, keeping entity
  // ids and the packed order. The entities must already exist in |to|.
  void (*clone)(const entt::registry& from, entt::registry& to) = nullptr;
  // Adds the component's fields to a hash (floats canonicalised). Null
  // |component| for tag types.
  void (*hash)(const void* component, Hasher& hasher) = nullptr;
  // Upgrades JSON written by an older version (none yet).
  std::expected<void, Error> (*migrate)(std::uint32_t from_version,
                                        std::string& json) = nullptr;
  // Draws the component in the editor (M10).
  void (*inspect)(void* component, void* ui_context) = nullptr;
};

class ComponentRegistry {
 public:
  // Registers T. Defined in se_core/reflect_glaze.h; call it only from a
  // *_components.cc file.
  template <class T>
  void Add(std::string_view name, std::uint32_t version, ComponentFlags flags);

  // Registers a hand-written entry. Names, storage ids and name ids must be
  // unique, and the registry must not be sealed.
  void Register(const ComponentInfo& info);

  [[nodiscard]] const ComponentInfo* Find(entt::id_type id) const noexcept;
  [[nodiscard]] const ComponentInfo* FindByName(
      std::string_view name) const noexcept;
  [[nodiscard]] const ComponentInfo* FindByNameId(
      std::uint32_t name_id) const noexcept;
  // In registration order, which is also the world-hash order.
  [[nodiscard]] std::span<const ComponentInfo> All() const noexcept {
    return infos_;
  }

  // No more registrations; Worlds require a sealed registry.
  void Seal() noexcept { sealed_ = true; }
  [[nodiscard]] bool Sealed() const noexcept { return sealed_; }

 private:
  std::vector<ComponentInfo> infos_;
  bool sealed_ = false;
};

}  // namespace se

#endif  // SE_CORE_COMPONENT_REGISTRY_H_
