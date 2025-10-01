#include "EventBus.hpp"

EventBus::EventBus(size_t numThreads)
    : m_taskScheduler{numThreads}
    , m_nextId{0}
{}

EventBus::EventBus(size_t numThreads, size_t taskQueueCapacity)
    : m_taskScheduler{numThreads, taskQueueCapacity}
    , m_nextId{0}
{}

EventBus::~EventBus()
{
    m_taskScheduler.shutdownNow();
}

void EventBus::dispatch(std::type_index type, std::shared_ptr<void> eventPtr)
{
    std::vector<Handler> handlers;
    {
        std::scoped_lock<std::mutex> lock(m_mtx);
        auto it = m_subscribers.find(type);
        if (it == m_subscribers.end()) return;
        
        for (const auto& [_, callback] : it->second)
            handlers.push_back(callback);
    }


    for (auto& callback : handlers)
    {
        LOG_INFO(Logger::get()) << "Event [" << type.name() << "] triggered. Executing callback <" << typeid(callback).name() << ">";
        callback(eventPtr);
    }
}
