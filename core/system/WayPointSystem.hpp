#pragma once

#include <string>
#include <string_view>

#include <SFML/System/Angle.hpp>
#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>

#include "interface/ISystem.hpp"

class TaskScheduler;

class WayPointSystem : public ISystem
{
public:
    WayPointSystem() = delete;
    WayPointSystem(std::string bindingStatID, TaskScheduler* jobScheduler = nullptr);

    constexpr std::string_view name() const override final { return "WayPointSystem"; }
    void update(entt::registry& reg, const float& dt = 0.f) override final;

private:
    const std::string m_bindingStatID;
    TaskScheduler* m_jobScheduler;
};