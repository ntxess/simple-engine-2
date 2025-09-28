#include "UpdateEntityPolling.hpp"

UpdateEntityPolling::UpdateEntityPolling()
    : timeStart(std::chrono::steady_clock::now()), maxDuration(0)
{}

UpdateEntityPolling::UpdateEntityPolling(std::chrono::milliseconds duration, bool readyOnStart)
    : timeStart(std::chrono::steady_clock::now()), maxDuration(duration)
{}

void UpdateEntityPolling::accept(IComponentVisitor* visitor, entt::entity entityID)
{
    visitor->visit(this, entityID);
}

bool UpdateEntityPolling::isReady()
{
    const auto currDuration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - timeStart);
    if (currDuration <= maxDuration) return false;
    timeStart = std::chrono::steady_clock::now();
    return true;
}