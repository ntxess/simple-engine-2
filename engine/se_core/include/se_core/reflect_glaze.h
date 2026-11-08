// Copyright 2026 The simple-engine-2 Authors.
//
// glaze reflection for the component registry: the definition of
// ComponentRegistry::Add<T>() and the hooks it generates.
//
// Include this header only from translation units named *_components.cc
// (tools/checks/check_glaze.py enforces it): glaze costs seconds per
// translation unit to compile (ADR-010).
//
// Components are plain aggregates. Field types supported by the hash: bool,
// integers, enums, float, double, Guid, EntityRef, std::string, std::array,
// std::vector and nested aggregates. EntityRef serializes as its Guid only.

#ifndef SE_CORE_REFLECT_GLAZE_H_
#define SE_CORE_REFLECT_GLAZE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <entt/core/type_info.hpp>
#include <entt/entity/registry.hpp>
#include <expected>
#include <glaze/beve.hpp>
#include <glaze/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "se_core/component_registry.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/hash.h"

template <>
struct glz::meta<se::EntityRef> {
  using T = se::EntityRef;
  // glaze's customization point is named value.
  static constexpr auto value =  // NOLINT(readability-identifier-naming)
      glz::object("guid", &T::guid);
};

namespace se::reflect {
namespace detail {

template <class T>
struct IsStdArray : std::false_type {};
template <class U, std::size_t Size>
struct IsStdArray<std::array<U, Size>> : std::true_type {};

template <class T>
struct IsStdVector : std::false_type {};
template <class U, class A>
struct IsStdVector<std::vector<U, A>> : std::true_type {};

}  // namespace detail

// Adds a value to a hash field by field, in declaration order.
template <class T>
void HashValue(Hasher& hasher, const T& value) {
  using U = std::remove_cvref_t<T>;
  constexpr bool kIsSequence =
      detail::IsStdArray<U>::value || detail::IsStdVector<U>::value;
  if constexpr (std::is_same_v<U, bool>) {
    hasher.AddBool(value);
  } else if constexpr (std::is_same_v<U, float>) {
    hasher.AddFloat(value);
  } else if constexpr (std::is_same_v<U, double>) {
    hasher.AddDouble(value);
  } else if constexpr (std::is_enum_v<U>) {
    hasher.AddI64(static_cast<std::int64_t>(value));
  } else if constexpr (std::is_integral_v<U> && std::is_signed_v<U>) {
    hasher.AddI64(value);
  } else if constexpr (std::is_integral_v<U>) {
    hasher.AddU64(value);
  } else if constexpr (std::is_same_v<U, Guid>) {
    HashGuid(hasher, value);
  } else if constexpr (std::is_same_v<U, EntityRef>) {
    HashGuid(hasher, value.guid);
  } else if constexpr (std::is_same_v<U, std::string>) {
    hasher.AddString(value);
  } else if constexpr (kIsSequence) {
    hasher.AddU64(value.size());
    for (const auto& element : value) HashValue(hasher, element);
  } else if constexpr (std::is_empty_v<U>) {
    // Tags carry no data.
  } else {
    static_assert(std::is_aggregate_v<U>,
                  "component fields must be aggregates or supported types");
    glz::for_each_field(
        value, [&hasher](const auto& field) { HashValue(hasher, field); });
  }
}

template <class T>
void Precreate(entt::registry& registry) {
  static_cast<void>(registry.storage<T>());
}

template <class T>
void Emplace(entt::registry& registry, entt::entity entity, T&& value) {
  if constexpr (std::is_empty_v<std::remove_cvref_t<T>>) {
    static_cast<void>(value);
    registry.emplace_or_replace<std::remove_cvref_t<T>>(entity);
  } else {
    registry.emplace_or_replace<std::remove_cvref_t<T>>(entity,
                                                        std::forward<T>(value));
  }
}

template <class T>
std::expected<void, Error> ReadJson(entt::registry& registry,
                                    entt::entity entity,
                                    std::string_view json) {
  T value{};
  if (const glz::error_ctx ec = glz::read_json(value, json); ec) {
    return Fail(ErrorCode::kParse, glz::format_error(ec, json));
  }
  Emplace(registry, entity, std::move(value));
  return {};
}

template <class T>
std::expected<std::string, Error> WriteJson(const entt::registry& registry,
                                            entt::entity entity) {
  if (!registry.all_of<T>(entity)) {
    return Fail(ErrorCode::kNotFound, "the entity has no such component");
  }
  std::string out;
  glz::error_ctx ec;
  if constexpr (std::is_empty_v<T>) {
    const T value{};
    ec = glz::write_json(value, out);
  } else {
    ec = glz::write_json(registry.get<T>(entity), out);
  }
  if (ec) return Fail(ErrorCode::kInvalidArgument, glz::format_error(ec));
  return out;
}

template <class T>
std::expected<void, Error> ReadBeve(entt::registry& registry,
                                    entt::entity entity,
                                    std::span<const std::byte> bytes) {
  T value{};
  if (const glz::error_ctx ec = glz::read_beve(value, bytes); ec) {
    return Fail(ErrorCode::kParse, glz::format_error(ec));
  }
  Emplace(registry, entity, std::move(value));
  return {};
}

template <class T>
std::expected<std::vector<std::byte>, Error> WriteBeve(
    const entt::registry& registry, entt::entity entity) {
  if (!registry.all_of<T>(entity)) {
    return Fail(ErrorCode::kNotFound, "the entity has no such component");
  }
  std::vector<std::byte> out;
  glz::error_ctx ec;
  if constexpr (std::is_empty_v<T>) {
    const T value{};
    ec = glz::write_beve(value, out);
  } else {
    ec = glz::write_beve(registry.get<T>(entity), out);
  }
  if (ec) return Fail(ErrorCode::kInvalidArgument, glz::format_error(ec));
  return out;
}

template <class T>
void Clone(const entt::registry& from, entt::registry& to) {
  const auto* source = from.storage<T>();
  if (source == nullptr) return;
  auto& target = to.storage<T>();
  const entt::sparse_set& packed = *source;
  for (std::size_t i = 0; i < packed.size(); ++i) {
    const entt::entity entity = packed.data()[i];
    if constexpr (std::is_empty_v<T>) {
      target.emplace(entity);
    } else {
      target.emplace(entity, source->get(entity));
    }
  }
}

template <class T>
void HashComponent(const void* component, Hasher& hasher) {
  if constexpr (std::is_empty_v<T>) {
    static_cast<void>(component);
    static_cast<void>(hasher);
  } else {
    HashValue(hasher, *static_cast<const T*>(component));
  }
}

}  // namespace se::reflect

namespace se {

template <class T>
void ComponentRegistry::Add(std::string_view name, std::uint32_t version,
                            ComponentFlags flags) {
  static_assert(std::is_aggregate_v<T> || std::is_empty_v<T>,
                "components are plain aggregates");
  ComponentInfo info;
  info.name = name;
  info.version = version;
  info.id = entt::type_hash<T>::value();
  info.name_id = Fnv1a32(name);
  info.flags = flags;
  info.empty = std::is_empty_v<T>;
  info.precreate = &reflect::Precreate<T>;
  info.read_json = &reflect::ReadJson<T>;
  info.write_json = &reflect::WriteJson<T>;
  info.read_beve = &reflect::ReadBeve<T>;
  info.write_beve = &reflect::WriteBeve<T>;
  info.clone = &reflect::Clone<T>;
  info.hash = &reflect::HashComponent<T>;
  Register(info);
}

}  // namespace se

#endif  // SE_CORE_REFLECT_GLAZE_H_
