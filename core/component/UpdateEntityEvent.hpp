#pragma once

#include <cstdint>

#include <entt/entity/entity.hpp>

#include "interface/IComponent.hpp"
#include "interface/IComponentVisitor.hpp"

/**
 * @brief Dirty flag of an entity that moved this tick (set by WayPointSystem); event-driven
 * collision sources lose it once processed. Carries a cooldown in ticks like UpdateEntityPolling.
*/
class UpdateEntityEvent : public IComponent
{
public:
    UpdateEntityEvent();
    UpdateEntityEvent(std::uint64_t cooldown, std::uint64_t nowTick = 0);

    void accept(IComponentVisitor* visitor, entt::entity entityID) override;

    // True once more than cooldownTicks ticks have passed since startTick; restarts the cooldown.
    bool isReady(std::uint64_t nowTick);

    std::uint64_t startTick;
    std::uint64_t cooldownTicks;
};
