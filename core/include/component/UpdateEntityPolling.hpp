#pragma once

#include "interface/IComponent.hpp"
#include "interface/IComponentVisitor.hpp"
#include "entt/entity/entity.hpp"
#include <chrono>

class UpdateEntityPolling : public IComponent
{
public:
    UpdateEntityPolling();
    UpdateEntityPolling(std::chrono::milliseconds duration, bool readyOnStart = true);

    void accept(IComponentVisitor* visitor, entt::entity entityID) override;

    bool isReady();

    std::chrono::steady_clock::time_point timeStart;
    std::chrono::milliseconds maxDuration;
};