#pragma once

#include <SFML/System/Vector2.hpp>
#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>

#include "interface/ICommand.hpp"
#include "component/Sprite.hpp"
#include "util/LegacySimTime.hpp"

/**
 * @brief Moves an entity along `direction` at `speed` pixels per second for one tick of the
 * simulation clock (rule R5, docs/rules/gameplay-rules.md).
*/
class Movement : public ICommand
{
public:
    Movement(const entt::entity entityID, sf::Vector2f direction, float speed, const LegacySimTime& simTime);
    void execute(entt::registry& reg);

private:
    const entt::entity entityId;
    sf::Vector2f direction;
    float speed;
    const LegacySimTime& simTime;
};