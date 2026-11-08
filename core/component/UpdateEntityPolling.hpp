#pragma once

#include <cstdint>

#include <entt/entity/entity.hpp>

#include "interface/IComponent.hpp"
#include "interface/IComponentVisitor.hpp"

/**
 * @brief Collision source that is polled every tick, with a cooldown in ticks between
 * contacts (the player's invincibility frames).
*/
class UpdateEntityPolling : public IComponent
{
public:
    UpdateEntityPolling();
    UpdateEntityPolling(std::uint64_t cooldown, bool readyOnStart = true, std::uint64_t nowTick = 0);

    void accept(IComponentVisitor* visitor, entt::entity entityID) override;

    // True once more than cooldownTicks ticks have passed since startTick; restarts the cooldown.
    bool isReady(std::uint64_t nowTick);

    std::uint64_t startTick;
    std::uint64_t cooldownTicks;
};
