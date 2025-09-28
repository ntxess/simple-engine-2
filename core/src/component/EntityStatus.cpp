#include "EntityStatus.hpp"

void EntityStatus::accept(IComponentVisitor* visitor, entt::entity entityID)
{
    visitor->visit(this, entityID);
}