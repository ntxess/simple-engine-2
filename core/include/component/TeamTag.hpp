#pragma once

#include "component/Team.hpp"
#include "interface/IComponent.hpp"
#include "interface/IComponentVisitor.hpp"
#include "entt/entity/entity.hpp"

class TeamTag : public IComponent
{
public:
    TeamTag();
    TeamTag(Team tag);

    void accept(IComponentVisitor* visitor, entt::entity entityID) override;

    Team tag;
};