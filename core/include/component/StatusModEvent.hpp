#pragma once

#include "component/EffectType.hpp"
#include "component/Effects.hpp"
#include "interface/IComponent.hpp"
#include "interface/IComponentVisitor.hpp"
#include "entt/entity/entity.hpp"
#include <chrono>

class StatusModEvent : public IComponent
{
public:
    StatusModEvent();
    StatusModEvent(const entt::entity source, const entt::entity receiver, const EffectType type, Effects* effect);
    StatusModEvent(const StatusModEvent& other);
    StatusModEvent(StatusModEvent&& other) noexcept;
    StatusModEvent& operator=(const StatusModEvent& other);

    void accept(IComponentVisitor* visitor, entt::entity entityID) override;

    entt::entity sourceID;
    entt::entity receiverID;
    EffectType effectType;
    Effects* effect;
    std::chrono::steady_clock::time_point timeStart;
    std::chrono::milliseconds timeElapsed;
};