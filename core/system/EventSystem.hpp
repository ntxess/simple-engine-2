#pragma once

#include <cstdint>
#include <string_view>

#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>

#include "Components.hpp"
#include "interface/ISystem.hpp"
#include "util/LegacySimTime.hpp"

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
    EventSystem(entt::registry& piplineReg, const LegacySimTime& simTime, std::uint64_t watchdogTicks);

    constexpr std::string_view name() const override final;
    void setWatchdogTicks(std::uint64_t watchdogTicks);
    void update(entt::registry& entityReg, const float& dt = 0.f) override final;

    EventStatus apply(const EffectType effectType, StatusModEvent& statusModEvent, EntityStatus& receiverStatus) const;
    EventStatus instantEvent(StatusModEvent& statusModEvent, EntityStatus& receiverStatus) const;
    EventStatus overTimeEvent(StatusModEvent& statusModEvent, EntityStatus& receiverStatus) const;
    EventStatus tempTimedEvent(StatusModEvent& statusModEvent, EntityStatus& receiverStatus) const;

private:
	entt::registry& m_piplineReg;
    const LegacySimTime& m_simTime;
    std::uint64_t m_eventWatchdogTicks;
};