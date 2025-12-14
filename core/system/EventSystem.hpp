#pragma once

#include <chrono>
#include <string_view>

#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>

#include "Components.hpp"
#include "interface/ISystem.hpp"

class EventSystem : public ISystem
{
private:
    enum class EventStatus
    {
        INCOMPLETE,
        COMPLETE,
    };

public:
    EventSystem() = delete;
    EventSystem(entt::registry& piplineReg, std::chrono::milliseconds watchdogTime);

    constexpr std::string_view name() const override final;
    void update(entt::registry& entityReg, const float& dt = 0.f) override final;

    EventStatus apply(const EffectType effectType, StatusModEvent& statusModEvent, EntityStatus& receiverStatus) const;
    EventStatus instantEvent(StatusModEvent& statusModEvent, EntityStatus& receiverStatus) const;
    EventStatus overTimeEvent(StatusModEvent& statusModEvent, EntityStatus& receiverStatus) const;
    EventStatus tempTimedEvent(StatusModEvent& statusModEvent, EntityStatus& receiverStatus) const;

private:
	entt::registry& m_piplineReg;
    const std::chrono::milliseconds m_eventWatchdogTime;
};