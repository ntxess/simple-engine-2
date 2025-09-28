#include "Effects.hpp"

Effects::Effects()
    : targetStat()
    , modificationVal(0.f)
    , maxDuration(std::chrono::milliseconds(0))
    , tickRate(std::chrono::milliseconds(0))
{}

Effects::Effects(const std::string& targStat, float modVal, std::chrono::milliseconds maxDur, std::chrono::milliseconds tickR)
    : targetStat(targStat)
    , modificationVal(modVal)
    , maxDuration(maxDur)
    , tickRate(tickR)
{}

Effects::Effects(const Effects& other)
    : targetStat(other.targetStat)
    , modificationVal(other.modificationVal)
    , maxDuration(other.maxDuration)
    , tickRate(other.tickRate)
{}

Effects::Effects(Effects&& other) noexcept
    : targetStat(std::move(other.targetStat))
    , modificationVal(std::move(other.modificationVal))
    , maxDuration(std::move(other.maxDuration))
    , tickRate(std::move(other.tickRate))
{}

Effects& Effects::operator=(const Effects& other)
{
    if (this != &other)
    {
        targetStat = other.targetStat;
        modificationVal = other.modificationVal;
        maxDuration = other.maxDuration;
        tickRate = other.tickRate;
    }

    return *this;
}

void Effects::accept(IComponentVisitor* visitor, entt::entity entityID)
{
    visitor->visit(this, entityID);
}