#include "BoundarySystem.hpp"

#include <algorithm>

BoundarySystem::BoundarySystem(const sf::Vector2u& bounds)
    : m_bounds{bounds}
{}

constexpr std::string_view BoundarySystem::name() const
{
    return "BoundarySystem";
}

void BoundarySystem::update(entt::registry& reg, const float& dt)
{
    const auto& spriteView = reg.view<Sprite>();
    for (const auto& entity : spriteView)
    {
        checkBoundary(m_bounds, spriteView.get<Sprite>(entity));
    }
}

void BoundarySystem::checkBoundary(const sf::Vector2u& boundary, sf::Sprite& obj)
{
    // Rule R11: the collider, the unrotated texture-size box centred on the position (rule R4),
    // stays inside the bounds, and a sprite larger than the bounds is centred on that axis. Both
    // axes are computed from the position before the clamp and written once. The float
    // expressions are evaluated as written, in this order, as the engine does.
    const sf::Vector2f position = obj.getPosition();
    const float hw = static_cast<float>(obj.getTextureRect().size.x) / 2.f;
    const float hh = static_cast<float>(obj.getTextureRect().size.y) / 2.f;
    const float w = static_cast<float>(boundary.x);
    const float h = static_cast<float>(boundary.y);

    const float x = w < 2.f * hw ? w * 0.5f : std::clamp(position.x, hw, w - hw);
    const float y = h < 2.f * hh ? h * 0.5f : std::clamp(position.y, hh, h - hh);
    obj.setPosition({ x, y });
}
