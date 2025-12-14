#pragma once

#include <string>
#include <unordered_map>

#include <entt/entity/entity.hpp>

#include "interface/IComponent.hpp"
#include "interface/IComponentVisitor.hpp"

class EntityStatus : public IComponent
{
public:
    void accept(IComponentVisitor* visitor, entt::entity entityID) override;

    std::unordered_map<std::string, float> values;
};