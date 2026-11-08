#include "UpdateEntityEvent.hpp"

UpdateEntityEvent::UpdateEntityEvent()
    : startTick{0}
    , cooldownTicks{0}
{}

UpdateEntityEvent::UpdateEntityEvent(std::uint64_t cooldown, std::uint64_t nowTick)
    : startTick{nowTick}
    , cooldownTicks{cooldown}
{}

void UpdateEntityEvent::accept(IComponentVisitor* visitor, entt::entity entityID)
{
    visitor->visit(this, entityID);
}

bool UpdateEntityEvent::isReady(std::uint64_t nowTick)
{
    const std::uint64_t elapsed = nowTick > startTick ? nowTick - startTick : 0;
    if (elapsed <= cooldownTicks) return false;
    startTick = nowTick;
    return true;
}
