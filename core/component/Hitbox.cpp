#include "Hitbox.hpp"

void Hitbox::accept(IComponentVisitor* visitor, entt::entity entityID)
{
    visitor->visit(this, entityID);
}