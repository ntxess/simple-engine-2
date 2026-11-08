#include "UpdateEntityPolling.hpp"

UpdateEntityPolling::UpdateEntityPolling()
    : startTick{0}
    , cooldownTicks{0}
    , readyOnStart{true}
{}

UpdateEntityPolling::UpdateEntityPolling(std::uint64_t cooldown, bool readyOnStart, std::uint64_t nowTick)
    : startTick{nowTick}
    , cooldownTicks{cooldown}
    , readyOnStart{readyOnStart}
{}

void UpdateEntityPolling::accept(IComponentVisitor* visitor, entt::entity entityID)
{
    visitor->visit(this, entityID);
}

bool UpdateEntityPolling::isReadyFor(entt::entity receiver, std::uint64_t nowTick)
{
    const auto it = lastContactTick.find(receiver);
    if (it != lastContactTick.end())
    {
        // Covers a second check in the same tick (0 ticks since the contact).
        if (nowTick - it->second <= cooldownTicks) return false;
    }
    else if (!readyOnStart)
    {
        const std::uint64_t sinceStart = nowTick > startTick ? nowTick - startTick : 0;
        if (sinceStart <= cooldownTicks) return false;
    }

    lastContactTick[receiver] = nowTick;
    return true;
}

void UpdateEntityPolling::forgetExpired(std::uint64_t nowTick)
{
    for (auto it = lastContactTick.begin(); it != lastContactTick.end();)
    {
        if (nowTick - it->second > cooldownTicks)
            it = lastContactTick.erase(it);
        else
            ++it;
    }
}
