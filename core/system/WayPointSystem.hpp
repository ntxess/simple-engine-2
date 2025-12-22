#pragma once

#include <cmath>
#include <numbers>
#include <string>
#include <string_view>

#include <SFML/System/Angle.hpp>
#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>

#include "Components.hpp"
#include "interface/ISystem.hpp"

class WayPointSystem : public ISystem
{
public:
    WayPointSystem() = delete;
    WayPointSystem(std::string bindingStatID);

    constexpr std::string_view name() const override final;
    void update(entt::registry& reg, const float& dt = 0.f) override final;

private:
    const std::string m_bindingStatID;
};