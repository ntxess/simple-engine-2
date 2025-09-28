#include "WayPointSystem.hpp"

WayPointSystem::WayPointSystem(std::string bindingStatID)
    : bindingStatID(bindingStatID)
{}

constexpr std::string_view WayPointSystem::name() const
{
    return "WayPointSystem";
}

void WayPointSystem::update(entt::registry& reg, const float& dt)
{
    const auto& group = reg.group<MovementPattern, EntityStatus>(entt::get<Sprite>);
    for (const auto& entityID : group)
    {
        auto [wpc, es, sp] = group.get<MovementPattern, EntityStatus, Sprite>(entityID);

        if (!es.values.count(bindingStatID)) return;
        
        const float speed = es.values.at(bindingStatID);

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

        float theta = (std::atan2(velocity.y, velocity.x)) * (180.f / float(std::numbers::pi));
        sp.sprite.setRotation(theta + 90);
        sp.sprite.move(velocity);
        reg.emplace_or_replace<UpdateEntityEvent>(entityID);
    }
}