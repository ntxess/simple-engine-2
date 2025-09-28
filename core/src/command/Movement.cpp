#include "command/Movement.hpp"

Movement::Movement(const entt::entity entityID, sf::Vector2f direction)
    : entityId(entityID)
    , direction(direction)
{}

void Movement::execute(entt::registry& reg)
{
    reg.get<Sprite>(entityId).sprite.move(direction.x, direction.y);
    // reg.emplace_or_replace<UpdateEntityEvent>(entityId);
}