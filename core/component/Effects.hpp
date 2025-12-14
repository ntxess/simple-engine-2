#pragma once

#include <chrono>
#include <string>

#include <entt/entity/entity.hpp>

#include "interface/IComponent.hpp"
#include "interface/IComponentVisitor.hpp"

class Effects : public IComponent
{
public:
    Effects();
    Effects(const std::string& targStat, float modVal, std::chrono::milliseconds maxDur = std::chrono::milliseconds(0), std::chrono::milliseconds tickR = std::chrono::milliseconds(0));
    Effects(const Effects& other);
    Effects(Effects&& other) noexcept;
    Effects& operator=(const Effects& other);

    void accept(IComponentVisitor* visitor, entt::entity entityID) override;

    std::string targetStat;
    float modificationVal;
    std::chrono::milliseconds maxDuration;
    std::chrono::milliseconds tickRate;
};