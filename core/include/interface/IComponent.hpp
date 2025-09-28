#pragma once

#include "entt/entity/entity.hpp"

class IComponentVisitor;

class IComponent
{
public:
    virtual ~IComponent() = default;
    virtual void accept(IComponentVisitor* visitor, entt::entity entityID) = 0;
};