#include "PlayerInput.hpp"

void PlayerInput::accept(IComponentVisitor* visitor, entt::entity entityID)
{
    visitor->visit(this, entityID);
}

void PlayerInput::processInput(entt::registry& reg, const LegacyInputFrame& frame)
{
    for (const auto& [action, command] : input)
    {
        if (frame.isHeld(action))
        {
            // Entity registry is passes in for convinience when creating more complex actions
            command->execute(reg);
        }
    }
}
