#include "MovementPattern.hpp"

MovementPattern::MovementPattern()
    : movePattern{nullptr}
    , segment{0}
    , segmentDistance{0.0f}
    , finished{false}
    , repeat{false} 
{}

MovementPattern::MovementPattern(std::unique_ptr<WayPoint> wp, bool repeat)
    : movePattern{std::move(wp)}
    , segment{0}
    , segmentDistance{0.0f}
    , finished{false}
    , repeat{repeat}
{}

void MovementPattern::restart()
{
    segment = 0;
    segmentDistance = 0.0f;
    finished = false;
}

void MovementPattern::accept(IComponentVisitor* visitor, entt::entity entityID)
{
    visitor->visit(this, entityID);
}