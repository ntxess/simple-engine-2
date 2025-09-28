#include "UpdateEntityEvent.hpp"

UpdateEntityEvent::UpdateEntityEvent()
    : timeStart(std::chrono::steady_clock::now()), maxDuration(0)
{}

UpdateEntityEvent::UpdateEntityEvent(std::chrono::milliseconds duration)
    : timeStart(std::chrono::steady_clock::now()), maxDuration(duration)
{}

void UpdateEntityEvent::accept(IComponentVisitor* visitor, entt::entity entityID)
{
    visitor->visit(this, entityID);
}

bool UpdateEntityEvent::isReady()
{
    const auto currDuration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - timeStart);
    if (currDuration <= maxDuration) return false;
    timeStart = std::chrono::steady_clock::now();
    return true;
}