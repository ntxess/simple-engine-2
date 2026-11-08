#include "command/Movement.hpp"
#include "component/UpdateEntityEvent.hpp"

Movement::Movement(const entt::entity entityID, sf::Vector2f direction, float speed, const LegacySimTime& simTime)
    : entityId{entityID}
    , direction{direction}
    , speed{speed}
    , simTime{simTime}
{}

void Movement::execute(entt::registry& reg)
{
    const float step = speed * static_cast<float>(simTime.dt);
    reg.get<Sprite>(entityId).move({direction.x * step, direction.y * step});
    // reg.emplace_or_replace<UpdateEntityEvent>(entityId);
}