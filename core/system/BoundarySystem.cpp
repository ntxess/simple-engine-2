#include "BoundarySystem.hpp"

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
    sf::Vector2f position = obj.getPosition();
    sf::FloatRect rect = obj.getGlobalBounds();

    if (position.x < 0)
        obj.setPosition(sf::Vector2f(0.f, position.y));

    if (position.x + (rect.size.x) > boundary.x)
        obj.setPosition(sf::Vector2f(boundary.x - (rect.size.x), position.y));

    if (position.y < 0)
        obj.setPosition(sf::Vector2f(position.x, 0.f));

    if (position.y + rect.size.y > boundary.y)
        obj.setPosition(sf::Vector2f(position.x, boundary.y - (rect.size.y)));
}
