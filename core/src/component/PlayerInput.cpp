#include "PlayerInput.hpp"

void PlayerInput::accept(IComponentVisitor* visitor, entt::entity entityID)
{
    visitor->visit(this, entityID);
}

void PlayerInput::processInput(entt::registry& reg)
{
    for (const auto& [key, action] : input)
    {
        if (sf::Keyboard::isKeyPressed(key))
        {
            // Entity registry is passes in for convinience when creating more complex actions
            action->execute(reg);
        }
    }
}
