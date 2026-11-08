#pragma once

#include <cstddef>
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

    // Starts the path over at its first point: segment 0, nothing travelled, not finished.
    void restart();

    std::unique_ptr<WayPoint> movePattern;
    // Progress (rule R12): the current segment, from waypoint `segment` to the next one, and the
    // distance travelled along it; `finished` once a non-repeating path has reached its end.
    std::size_t segment;
    float segmentDistance;
    bool finished;
    bool repeat;
};