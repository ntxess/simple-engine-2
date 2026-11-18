#pragma once

#include <string_view>

#include <SFML/Graphics/Sprite.hpp>
#include <SFML/System/Vector2.hpp>
#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>

#include "Components.hpp"
#include "interface/ISystem.hpp"

/**
 * @brief Keeps every sprite's collider inside the scene bounds, after movement and before
 * collision (rules R9 and R11, docs/rules/gameplay-rules.md).
*/
class BoundarySystem : public ISystem
{
public:
    BoundarySystem() = delete;
    BoundarySystem(const sf::Vector2u& bounds);

    constexpr std::string_view name() const override final;
    void update(entt::registry& reg, const float& dt = 0.f) override final;

private:
    static void checkBoundary(const sf::Vector2u& boundary, sf::Sprite& obj);

    const sf::Vector2u m_bounds;
};
