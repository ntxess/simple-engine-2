#include "EffectsList.hpp"

void EffectsList::accept(IComponentVisitor* visitor, entt::entity entityID)
{
    visitor->visit(this, entityID);
}