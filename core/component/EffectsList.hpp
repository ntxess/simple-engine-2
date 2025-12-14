#pragma once

#include <utility>
#include <vector>

#include <entt/entity/entity.hpp>

#include "component/EffectType.hpp"
#include "component/Effects.hpp"
#include "interface/IComponent.hpp"
#include "interface/IComponentVisitor.hpp"

class EffectsList : public IComponent
{
public:
    void accept(IComponentVisitor* visitor, entt::entity entityID) override;

    std::vector<std::pair<EffectType, Effects>> effectsList;
};
