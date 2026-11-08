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

/**
 * @brief Moves entities along their MovementPattern at their bound speed stat, in px/s; dt is the
 * tick length in seconds (rule R5). A tick's step follows the polyline across as many waypoints as
 * it reaches, and the position is taken from the progress along the current segment (rule R12). A
 * repeating path closes the loop from its last point back to its first (rule R13).
*/
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