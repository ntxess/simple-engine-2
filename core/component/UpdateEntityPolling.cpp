#include "UpdateEntityPolling.hpp"

UpdateEntityPolling::UpdateEntityPolling()
    : startTick{0}
    , cooldownTicks{0}
{}

UpdateEntityPolling::UpdateEntityPolling(std::uint64_t cooldown, bool readyOnStart, std::uint64_t nowTick)
    : startTick{nowTick}
    , cooldownTicks{cooldown}
{}

void UpdateEntityPolling::accept(IComponentVisitor* visitor, entt::entity entityID)
{
    visitor->visit(this, entityID);
}

bool UpdateEntityPolling::isReady(std::uint64_t nowTick)
{
    const std::uint64_t elapsed = nowTick > startTick ? nowTick - startTick : 0;
    if (elapsed <= cooldownTicks) return false;
    startTick = nowTick;
    return true;
}
