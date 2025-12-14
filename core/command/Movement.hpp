#pragma once

#include <SFML/System/Vector2.hpp>
#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>

#include "interface/ICommand.hpp"
#include "component/Sprite.hpp"

class Movement : public ICommand
{
public:
    Movement(const entt::entity entityID, sf::Vector2f direction);
    void execute(entt::registry& reg);

private:
    const entt::entity entityId;
    sf::Vector2f direction;
};