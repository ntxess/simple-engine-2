#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "util/BoundedQueue.hpp"
#include "util/Logger.hpp"

struct ScheduledTask
{
public:
    std::function<void()> func;
    std::chrono::steady_clock::time_point nextRun{};
    std::chrono::milliseconds interval{ 0 };
    std::chrono::steady_clock::time_point endTime{};

    bool operator>(const ScheduledTask& other) const noexcept
    {
        return nextRun > other.nextRun;
    }
};

class TaskScheduler
{
public:
    explicit TaskScheduler(size_t numThreads);
    explicit TaskScheduler(size_t numThreads, size_t immTaskCapacity);
    ~TaskScheduler();
    bool schedule(std::function<void()> taskFunc);
    bool scheduleAt(std::function<void()> taskFunc, std::chrono::steady_clock::time_point time);
    bool scheduleFor(std::function<void()> taskFunc, std::chrono::milliseconds interval, std::chrono::milliseconds duration);
    bool scheduleAfter(std::function<void()> taskFunc, std::chrono::milliseconds delay);
    void shutdown();
    void shutdownNow();

private:
    void init();
    void workerLoop();

private:
    const size_t m_numThreads;
    BoundedQueue<std::function<void()>> m_immTaskQueue;
    bool m_stopped;
    std::priority_queue<ScheduledTask, std::vector<ScheduledTask>, std::greater<>> m_delayTaskQueue;
    std::vector<std::thread> m_threads;
    std::condition_variable m_taskAvail;
    std::mutex m_mtx;
};