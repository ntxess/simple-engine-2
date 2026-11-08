#pragma once

#include <cstdint>

#include <entt/entity/entity.hpp>

#include "component/EffectType.hpp"
#include "component/Effects.hpp"
#include "interface/IComponent.hpp"
#include "interface/IComponentVisitor.hpp"

class StatusModEvent : public IComponent
{
public:
    StatusModEvent();
    StatusModEvent(const entt::entity source, const entt::entity receiver, const EffectType type, const Effects& effect, std::uint64_t nowTick = 0);
    StatusModEvent(const StatusModEvent& other) = default;
    StatusModEvent(StatusModEvent&& other) noexcept = default;
    StatusModEvent& operator=(const StatusModEvent& other) = default;
    StatusModEvent& operator=(StatusModEvent&& other) noexcept = default;

    void accept(IComponentVisitor* visitor, entt::entity entityID) override;

    entt::entity sourceID;
    entt::entity receiverID;
    EffectType effectType;
    Effects effect;              // Copied from the source's EffectsList at contact, so later edits cannot dangle
    std::uint64_t startTick;     // Tick of the contact that created the event
    std::uint64_t lastApplyTick; // Tick the timing of the next application counts from (starts at startTick)
    std::uint64_t elapsedTicks;  // Ticks accumulated by the applications so far
    std::uint64_t applications;  // OVERTIME applications so far (rule R7)
    std::uint64_t sequence;      // Creation order; events are processed in this order (rule R8)
};
