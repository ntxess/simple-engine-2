#include "EventSystem.hpp"

#include <algorithm>

#include "util/Logger.hpp"

EventSystem::EventSystem(entt::registry& piplineReg, const LegacySimTime& simTime, std::uint64_t watchdogTicks)
    : m_piplineReg{piplineReg}
    , m_simTime{simTime}
    , m_eventWatchdogTicks{watchdogTicks}
{}

constexpr std::string_view EventSystem::name() const
{
    return "EventSystem";
}

void EventSystem::setWatchdogTicks(std::uint64_t watchdogTicks)
{
    m_eventWatchdogTicks = watchdogTicks;
}

void EventSystem::update(entt::registry& entityReg, const float& dt)
{
    LOG_TRACE(Logger::get()) << "Entering EventSystem::update()";

    // Rule R8: events are processed in creation order, which follows the sorted contacts.
    m_order.clear();
    const auto& statusModView = m_piplineReg.view<StatusModEvent>();
    for (const auto& eventID : statusModView)
    {
        m_order.emplace_back(statusModView.get<StatusModEvent>(eventID).sequence, eventID);
    }
    std::sort(m_order.begin(), m_order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& [sequence, eventID] : m_order)
    {
        const entt::entity receiverID = m_piplineReg.get<StatusModEvent>(eventID).receiverID;

        if (entityReg.valid(receiverID) && entityReg.all_of<EntityStatus>(receiverID))
        {
            const EffectType& effectType = m_piplineReg.get<StatusModEvent>(eventID).effectType;
            StatusModEvent& statusModEvent = m_piplineReg.get<StatusModEvent>(eventID);
            EntityStatus& receiverStatus = entityReg.get<EntityStatus>(statusModEvent.receiverID);

            LOG_TRACE(Logger::get()) << "Processing Event ID [" << static_cast<unsigned int>(eventID) << "]";

            if (apply(effectType, statusModEvent, receiverStatus) == EventStatus::COMPLETE)
            {
                // Event completed processesing
                LOG_INFO(Logger::get()) << "Completed processing of Event ID [" << static_cast<unsigned int>(eventID) << "]";

                m_piplineReg.destroy(eventID);
            }
        }
        else
        {
            // Destroy event if it receiver is invalid
            LOG_WARNING(Logger::get())
                << "Failed processing of Event ID [" << static_cast<unsigned int>(eventID)
                << "]. Entity [" << static_cast<unsigned int>(receiverID) << "] no longer valid";

            m_piplineReg.destroy(eventID);
        }
    }

    LOG_TRACE(Logger::get()) << "Leaving EventSystem::update()";
}

EventSystem::EventStatus EventSystem::apply(const EffectType effectType, StatusModEvent& statusModEvent, EntityStatus& receiverStatus) const
{
    switch (effectType)
    {
    case EffectType::NULLTYPE:
        break;

    case EffectType::INSTANT:
        return instantEvent(statusModEvent, receiverStatus);

    case EffectType::OVERTIME:
        return overTimeEvent(statusModEvent, receiverStatus);

    case EffectType::TEMPTIMED:
        return tempTimedEvent(statusModEvent, receiverStatus);

    default:
        break;
    }

    return EventStatus::COMPLETE;
}

EventSystem::EventStatus EventSystem::instantEvent(StatusModEvent& statusModEvent, EntityStatus& receiverStatus) const
{
    const auto it = receiverStatus.values.find(statusModEvent.effect.targetStat);
    if (it != receiverStatus.values.end())
    {
        it->second += statusModEvent.effect.modificationVal;
        LOG_DEBUG(Logger::get())
            << "Applying instant event. Modifying \"" << statusModEvent.effect.targetStat
            << "\" of entity [" << static_cast<unsigned int>(statusModEvent.receiverID)
            << "] by " << statusModEvent.effect.modificationVal
            << ". Current val: " << it->second;
    }

    return EventStatus::COMPLETE;
}

EventSystem::EventStatus EventSystem::overTimeEvent(StatusModEvent& statusModEvent, EntityStatus& receiverStatus) const
{
    // Durations are authored in milliseconds and compared in ticks (msToTicks rounds up).
    // Rule R7: the first application comes exactly one interval after the contact, the next ones one
    // interval apart, floor(duration / interval) times in all. An interval of 0 applies once per tick,
    // for ticks(duration) ticks.
    const std::uint64_t nowTick = m_simTime.tick;
    const std::uint64_t currTicks = nowTick - statusModEvent.lastApplyTick;
    const std::int64_t intervalMs = statusModEvent.effect.tickRate.count();
    const std::int64_t durationMs = statusModEvent.effect.maxDuration.count();
    const std::uint64_t intervalTicks = std::max<std::uint64_t>(msToTicks(intervalMs, m_simTime.rate), 1);
    const std::uint64_t totalApplications = intervalMs > 0
        ? static_cast<std::uint64_t>(std::max<std::int64_t>(durationMs, 0) / intervalMs)
        : msToTicks(durationMs, m_simTime.rate);

    // TickRate controls the rate at which the effects applies its modification to the receivers stats
    if (statusModEvent.applications < totalApplications && currTicks >= intervalTicks)
    {
        statusModEvent.lastApplyTick = nowTick;
        statusModEvent.elapsedTicks += currTicks;
        ++statusModEvent.applications;

        const auto it = receiverStatus.values.find(statusModEvent.effect.targetStat);
        if (it != receiverStatus.values.end())
        {
            it->second += statusModEvent.effect.modificationVal;
            LOG_DEBUG(Logger::get())
                << "Applying over-time event. Modifying \"" << statusModEvent.effect.targetStat
                << "\" of entity [" << static_cast<unsigned int>(statusModEvent.receiverID)
                << "] by " << statusModEvent.effect.modificationVal
                << ". Current val: " << it->second;
        }
    }

    if (m_eventWatchdogTicks < currTicks || statusModEvent.applications >= totalApplications)
    {
        const auto it = receiverStatus.values.find(statusModEvent.effect.targetStat);
        if (it != receiverStatus.values.end())
        {
            LOG_DEBUG(Logger::get())
                << "Completed over-time event. \"" << statusModEvent.effect.targetStat
                << "\" stat of entity [" << static_cast<unsigned int>(statusModEvent.receiverID)
                << "] is " << it->second
                << ". Applications: " << statusModEvent.applications << " of " << totalApplications;
        }

        return EventStatus::COMPLETE;
    }

    return EventStatus::INCOMPLETE;
}

EventSystem::EventStatus EventSystem::tempTimedEvent(StatusModEvent& statusModEvent, EntityStatus& receiverStatus) const
{
    const std::uint64_t nowTick = m_simTime.tick;
    const std::uint64_t currTicks = nowTick - statusModEvent.lastApplyTick;
    const std::uint64_t durationTicks = msToTicks(statusModEvent.effect.maxDuration.count(), m_simTime.rate);

    // If event some how lasts longer than it should, consider it complete and prep for destroy.
    // Rule R1 (docs/rules/gameplay-rules.md): the modification is reverted on this path too.
    if (m_eventWatchdogTicks < currTicks)
    {
        if (statusModEvent.elapsedTicks != 0)
            revertTempTimed(statusModEvent, receiverStatus);
        return EventStatus::COMPLETE;
    }

    // Only modify the receiverStatus once in this type of event. We can use the elapsedTicks variable as a bool to prevent reaccessing this logic
    if (statusModEvent.elapsedTicks == 0)
    {
        statusModEvent.lastApplyTick = nowTick;
        statusModEvent.elapsedTicks += 1; // Disable this logic block with this variable

        const auto it = receiverStatus.values.find(statusModEvent.effect.targetStat);
        if (it != receiverStatus.values.end())
        {
            it->second += statusModEvent.effect.modificationVal;
            LOG_DEBUG(Logger::get())
                << "Applying temporary-timed event. Modifying \"" << statusModEvent.effect.targetStat
                << "\" of entity [" << static_cast<unsigned int>(statusModEvent.receiverID)
                << "] by " << statusModEvent.effect.modificationVal
                << ". Current val: " << it->second;
        }
    }

    if (currTicks >= durationTicks)
    {
        revertTempTimed(statusModEvent, receiverStatus);
        return EventStatus::COMPLETE;
    }

    return EventStatus::INCOMPLETE;
}

void EventSystem::revertTempTimed(StatusModEvent& statusModEvent, EntityStatus& receiverStatus) const
{
    const auto it = receiverStatus.values.find(statusModEvent.effect.targetStat);
    if (it != receiverStatus.values.end())
    {
        it->second += statusModEvent.effect.modificationVal * -1.f;
        LOG_DEBUG(Logger::get())
            << "Completed temporary-timed event. Restoring \"" << statusModEvent.effect.targetStat
            << "\" of entity [" << static_cast<unsigned int>(statusModEvent.receiverID)
            << "] by " << statusModEvent.effect.modificationVal
            << ". Current val: " << it->second;
    }
}
