#pragma once

#include <cstdint>
#include <unordered_map>

#include <entt/entity/entity.hpp>

#include "interface/IComponent.hpp"
#include "interface/IComponentVisitor.hpp"

/**
 * @brief Collision source that is polled every tick, with a cooldown in ticks per receiver
 * (rule R2, docs/rules/gameplay-rules.md).
*/
class UpdateEntityPolling : public IComponent
{
public:
    UpdateEntityPolling();
    UpdateEntityPolling(std::uint64_t cooldown, bool readyOnStart = true, std::uint64_t nowTick = 0);

    void accept(IComponentVisitor* visitor, entt::entity entityID) override;

    // Whether this source may affect `receiver` at `nowTick`: not yet this tick, and more than
    // cooldownTicks ticks since their last contact (or, before any contact, readyOnStart or more
    // than cooldownTicks ticks since startTick). A true answer records the contact.
    bool isReadyFor(entt::entity receiver, std::uint64_t nowTick);

    // Forgets contacts whose cooldown has run out; they no longer change any decision.
    void forgetExpired(std::uint64_t nowTick);

    std::uint64_t startTick;
    std::uint64_t cooldownTicks;
    bool readyOnStart;
    std::unordered_map<entt::entity, std::uint64_t> lastContactTick;
};
