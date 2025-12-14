#pragma once

#include <memory>

#include <entt/entity/entity.hpp>

#include "util/WayPoint.hpp"
#include "interface/IComponent.hpp"
#include "interface/IComponentVisitor.hpp"

class MovementPattern : public IComponent
{
public:
    MovementPattern();
    MovementPattern(std::unique_ptr<WayPoint> wp, bool repeat = false);

    void accept(IComponentVisitor* visitor, entt::entity entityID) override;

    std::unique_ptr<WayPoint> movePattern;
    WayPoint* currentPath;
    float distance;
    bool repeat;
};