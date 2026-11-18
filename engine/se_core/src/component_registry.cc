// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/component_registry.h"

#include <cstdint>
#include <string_view>

#include "se_core/assert.h"
#include "se_core/hash.h"

namespace se {

void ComponentRegistry::Register(const ComponentInfo& info) {
  SE_ASSERT_MSG(!sealed_, "the component registry is sealed");
  SE_ASSERT_MSG(!info.name.empty(), "a component needs a name");
  SE_ASSERT_MSG(info.name_id == Fnv1a32(info.name),
                "name_id must be Fnv1a32(name)");
  SE_ASSERT_MSG(Find(info.id) == nullptr, "component type registered twice");
  SE_ASSERT_MSG(FindByNameId(info.name_id) == nullptr,
                "component name (or its hash) registered twice");
  infos_.push_back(info);
}

const ComponentInfo* ComponentRegistry::Find(entt::id_type id) const noexcept {
  for (const ComponentInfo& info : infos_) {
    if (info.id == id) return &info;
  }
  return nullptr;
}

const ComponentInfo* ComponentRegistry::FindByName(
    std::string_view name) const noexcept {
  for (const ComponentInfo& info : infos_) {
    if (info.name == name) return &info;
  }
  return nullptr;
}

const ComponentInfo* ComponentRegistry::FindByNameId(
    std::uint32_t name_id) const noexcept {
  for (const ComponentInfo& info : infos_) {
    if (info.name_id == name_id) return &info;
  }
  return nullptr;
}

}  // namespace se
