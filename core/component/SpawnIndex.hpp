#pragma once

#include <cstdint>

#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>

/**
 * @brief Spawn order of an entity within its scene registry: the key of the semantic projection
 * (util/SemanticProjection.hpp). Indices are handed out once, in creation order, and never reused,
 * so they do not depend on entt's id recycling.
*/
struct SpawnIndex
{
    std::uint32_t value = 0;
};

/**
 * @brief Next SpawnIndex of a registry, kept in the registry context.
*/
struct SpawnCounter
{
    std::uint32_t next = 0;
};

inline SpawnIndex assignSpawnIndex(entt::registry& reg, entt::entity entity)
{
    auto& counter = reg.ctx().emplace<SpawnCounter>();
    const SpawnIndex index{ counter.next++ };
    reg.emplace_or_replace<SpawnIndex>(entity, index);
    return index;
}
