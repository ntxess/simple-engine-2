#pragma once

#include <algorithm>
#include <any>
#include <functional>
#include <memory>
#include <mutex>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "util/Logger.hpp"
#include "util/TaskScheduler.hpp"

class EventBus
{
public:
    using Handler = std::function<void(std::shared_ptr<void>)>;
    using SubscriberId = std::size_t;

    explicit EventBus(size_t numThreads = 4);
    explicit EventBus(size_t numThreads, size_t taskQueueCapacity);
    ~EventBus();

    template<typename T>
    SubscriberId subscribe(std::function<void(const T&)> handler);

    template<typename T>
    void unsubscribe(SubscriberId id);

    template<typename T>
    void publish(const T& event);

private:
    void dispatch(std::type_index type, std::shared_ptr<void> eventPtr);

private:
    TaskScheduler m_taskScheduler;
    SubscriberId m_nextId;
    std::unordered_map<std::type_index, std::vector<std::pair<SubscriberId, Handler>>> m_subscribers;
    std::mutex m_mtx;
};

template <typename T>
inline EventBus::SubscriberId EventBus::subscribe(std::function<void(const T&)> handler)
{
    std::scoped_lock<std::mutex> lock(m_mtx);
    SubscriberId id = ++m_nextId;

    auto& vec = m_subscribers[std::type_index(typeid(T))];
    vec.emplace_back(id, [handler](std::shared_ptr<void> e) {
        handler(*static_pointer_cast<T>(e));
        });

    return id;
}

template <typename T>
inline void EventBus::unsubscribe(SubscriberId id)
{
    std::scoped_lock<std::mutex> lock(m_mtx);
    auto subscriberIt = m_subscribers.find(std::type_index(typeid(T)));

    if (subscriberIt == m_subscribers.end())
        return;

    auto& vec = subscriberIt->second;
    vec.erase(
        std::remove_if(
            vec.begin(),
            vec.end(),
            [id](auto& pair) { return pair.first == id; }
        ),
        vec.end()
    );

    // Remove the vector entirely if empty
    if (vec.empty())
        m_subscribers.erase(subscriberIt);
}

template <typename T>
inline void EventBus::publish(const T& event)
{
    // Heap allocate a copy of the event
    auto eventPtr = std::make_shared<T>(event);

    // Schedule the subscriber's callback function
    // Capture 'this' in order to invoke dispatch()
    m_taskScheduler.schedule([this, eventPtr]() {
        dispatch(std::type_index(typeid(T)), eventPtr); // Type-erased dispatch to uniformly store into scheduler queue
    });
}
