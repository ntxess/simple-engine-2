#include "WayPointSystem.hpp"

WayPointSystem::WayPointSystem(std::string bindingStatID)
    : m_bindingStatID{bindingStatID}
{}


void WayPointSystem::update(entt::registry& reg, const float& dt)
{
    const auto& group = reg.group<MovementPattern, EntityStatus>(entt::get<Sprite>);
    for (const auto& entityID : group)
    {
        auto [wpc, es, sp] = group.get<MovementPattern, EntityStatus, Sprite>(entityID);

        const auto speedIt = es.values.find(m_bindingStatID);
        if (speedIt == es.values.end()) continue;

        const float speed = speedIt->second;

        WayPoint* headPtr = wpc.currentPath;
        WayPoint* nextPtr = headPtr->next();

        if (nextPtr == nullptr)
        {
            if (wpc.repeat)
            {
                wpc.currentPath = wpc.movePattern.get();
                wpc.distance = 0.f;
            }
            continue;
        }

        wpc.distance += speed * dt;
        if (wpc.distance > nextPtr->distanceTotal)
            wpc.currentPath = nextPtr;

        sf::Vector2f unitDist;
        unitDist.x = (nextPtr->coordinate.x - headPtr->coordinate.x) / headPtr->distanceToNext;
        unitDist.y = (nextPtr->coordinate.y - headPtr->coordinate.y) / headPtr->distanceToNext;

        sf::Vector2f velocity;
        velocity.x = unitDist.x * speed * dt;
        velocity.y = unitDist.y * speed * dt;

        float heading = (std::atan2(velocity.y, velocity.x) * (180.f / float(std::numbers::pi))) + 90.f;
        sp.setRotation(sf::degrees(heading));
        sp.move(velocity);
        reg.emplace_or_replace<UpdateEntityEvent>(entityID);
    }
}