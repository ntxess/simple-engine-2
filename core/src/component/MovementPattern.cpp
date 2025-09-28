#include "MovementPattern.hpp"

MovementPattern::MovementPattern()
    : movePattern(nullptr), currentPath(nullptr), distance(0.0f), repeat(false) 
{}

MovementPattern::MovementPattern(std::unique_ptr<WayPoint> wp, bool repeat)
    : movePattern(std::move(wp)), currentPath(movePattern.get()), distance(0.0f), repeat(repeat)
{}

void MovementPattern::accept(IComponentVisitor* visitor, entt::entity entityID)
{
    visitor->visit(this, entityID);
}