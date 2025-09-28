#pragma once

#include "entt/entity/registry.hpp"

class ICommand
{
public:
    virtual ~ICommand() = default;
    virtual void execute(entt::registry& reg) = 0;
};