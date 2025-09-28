#include "TeamTag.hpp"

TeamTag::TeamTag()
    : tag(Team::UNAFFILIATED)
{}

TeamTag::TeamTag(Team tag)
    : tag(tag)
{}

void TeamTag::accept(IComponentVisitor* visitor, entt::entity entityID)
{
    visitor->visit(this, entityID);
}