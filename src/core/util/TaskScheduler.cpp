#include "TaskScheduler.hpp"

TaskScheduler::TaskScheduler(size_t numThreads)
    : m_numThreads(numThreads)
    , m_immTaskQueue(100)
    , m_stopped(false)
{
    init();
}

TaskScheduler::TaskScheduler(size_t numThreads, size_t immTaskCapacity)
    : m_numThreads(numThreads)
    , m_immTaskQueue(immTaskCapacity)
    , m_stopped(false)
{
    init();
}

TaskScheduler::~TaskScheduler()
{
    LOG_INFO(Logger::get()) << "Shutting down task scheduler";
    shutdownNow();
}

bool TaskScheduler::schedule(std::function<void()> taskFunc)
{
    {
        std::scoped_lock<std::mutex> lock(m_mtx);
        if (m_stopped) return false;
    }

    if (m_immTaskQueue.push(std::move(taskFunc)))
    {
        m_taskAvail.notify_one();
        return true;
    }

    return false;
}

bool TaskScheduler::scheduleAt(std::function<void()> taskFunc, std::chrono::steady_clock::time_point time)
{
    // "scheduleAt" tasks are defined as 'nextRun == time && interval == 0 && duration == 0'
    std::unique_lock<std::mutex> lock(m_mtx);
    if (m_stopped) return false;
    m_delayTaskQueue.push(ScheduledTask{
        std::move(taskFunc),
        time,
        std::chrono::milliseconds{0},
        std::chrono::steady_clock::time_point::min()
    });
    lock.unlock();
    m_taskAvail.notify_one();
    return true;
}

bool TaskScheduler::scheduleFor(std::function<void()> taskFunc, std::chrono::milliseconds interval, std::chrono::milliseconds duration)
{
    // "scheduleFor" tasks are defined as 'nextRun == nanosec(0) && interval > 0 && duration > 0'
    auto now = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(m_mtx);
    if (m_stopped) return false;
    m_delayTaskQueue.push(ScheduledTask{ std::move(taskFunc), now + interval, interval, now + duration });
    lock.unlock();
    m_taskAvail.notify_one();
    return true;
}

bool TaskScheduler::scheduleAfter(std::function<void()> taskFunc, std::chrono::milliseconds delay)
{
    // "scheduleAfter" tasks are defined as 'nextRun == now() + interval && interval > 0 && duration == 0' 
    return scheduleAt(std::move(taskFunc), std::chrono::steady_clock::now() + delay);
}

void TaskScheduler::shutdown()
{
    {
        std::scoped_lock<std::mutex> lock(m_mtx);
        m_stopped = true;
    }

    m_immTaskQueue.close();
    m_taskAvail.notify_all();
    for (auto& thread : m_threads)
        if (thread.joinable())
            thread.join();
}

void TaskScheduler::shutdownNow()
{
    {
        std::scoped_lock<std::mutex> lock(m_mtx);
        m_stopped = true;

        while (m_delayTaskQueue.size() > 0)
            m_delayTaskQueue.pop();
    }

    while (m_immTaskQueue.size() > 0)
    {
        std::function<void()> temp;
        m_immTaskQueue.pop(temp);
    }

    m_immTaskQueue.close();
    m_taskAvail.notify_all();
    for (auto& thread : m_threads)
        if (thread.joinable())
            thread.join();
}

void TaskScheduler::init()
{
    if (m_numThreads == 0)
    {
        LOG_FATAL(Logger::get()) << "TaskScheduler failed init. NumThreads must be > 0";
        throw std::invalid_argument("NumThreads must be > 0");
    }

    for (size_t i = 0; i < m_numThreads; ++i)
    {
        LOG_INFO(Logger::get()) << "TaskScheduler worker thread [" << i << "] started";
        m_threads.emplace_back(&TaskScheduler::workerLoop, this);
    }
}

void TaskScheduler::workerLoop()
{
    while (true)
    {
        // Immediate task processing
        std::function<void()> task;
        if (m_immTaskQueue.tryPop(task, std::chrono::milliseconds(50)))
        {
            task();
            LOG_INFO(Logger::get()) << "TaskScheduler finished immediate task [" << typeid(task).name() << "]";

            continue;
        }

        // Delayed task processing
        std::unique_lock<std::mutex> lock(m_mtx);

        // Exit only when stopped and there is nothing to do
        if (m_stopped && m_delayTaskQueue.empty()) break;

        if (!m_delayTaskQueue.empty())
        {
            const auto& topTask = m_delayTaskQueue.top();
            auto now = std::chrono::steady_clock::now();

            // If the task is overdue, run immediately
            if (topTask.nextRun <= now)
            {
                ScheduledTask scheduledTask = m_delayTaskQueue.top();
                m_delayTaskQueue.pop();
                lock.unlock();

                // Run the task function
                scheduledTask.func();
                LOG_INFO(Logger::get()) << "TaskScheduler finished delayed task [" << typeid(topTask.func).name() << "]";


                if (scheduledTask.interval > std::chrono::milliseconds{ 0 } &&
                    (scheduledTask.endTime == std::chrono::steady_clock::time_point::min() ||
                        scheduledTask.nextRun + scheduledTask.interval <= scheduledTask.endTime))
                {
                    scheduledTask.nextRun += scheduledTask.interval;
                    std::scoped_lock lock2(m_mtx);
                    if (!m_stopped)
                    {
                        m_delayTaskQueue.push(std::move(scheduledTask));
                        m_taskAvail.notify_one();
                    }
                }

                continue;
            }

            m_taskAvail.wait_until(lock, topTask.nextRun, [&] { return !m_delayTaskQueue.empty() || m_stopped; });
            continue;
        }
        else
        {
            m_taskAvail.wait(lock, [&] { return !m_delayTaskQueue.empty() || m_stopped; });
        }
    }
}
