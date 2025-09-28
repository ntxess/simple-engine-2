#pragma once

#include "interface/IComponent.hpp"
#include "interface/IComponentVisitor.hpp"
#include "entt/entity/entity.hpp"
#include <string>
#include <unordered_map>

class EntityStatus : public IComponent
{
public:
    void accept(IComponentVisitor* visitor, entt::entity entityID) override;

    std::unordered_map<std::string, float> values;
};