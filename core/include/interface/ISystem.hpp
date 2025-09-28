#pragma once

#include "entt/entity/entity.hpp"
#include "entt/entity/registry.hpp"
#include <string_view>

class ISystem
{
public:
    virtual ~ISystem() = default;
    virtual constexpr std::string_view name() const = 0;
    virtual void update(entt::registry& reg, const float& dt = 0.f) = 0;
};