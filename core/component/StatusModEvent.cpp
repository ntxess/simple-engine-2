#include "StatusModEvent.hpp"

StatusModEvent::StatusModEvent()
    : sourceID{entt::null}
    , receiverID{entt::null}
    , effectType{EffectType::NULLTYPE}
    , effect{}
    , startTick{0}
    , lastApplyTick{0}
    , elapsedTicks{0}
{}

StatusModEvent::StatusModEvent(const entt::entity source, const entt::entity receiver, const EffectType type, const Effects& effect, std::uint64_t nowTick)
    : sourceID{source}
    , receiverID{receiver}
    , effectType{type}
    , effect{effect}
    , startTick{nowTick}
    , lastApplyTick{nowTick}
    , elapsedTicks{0}
{}

void StatusModEvent::accept(IComponentVisitor* visitor, entt::entity entityID)
{
    visitor->visit(this, entityID);
}
