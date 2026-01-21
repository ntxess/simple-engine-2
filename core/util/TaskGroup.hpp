#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>

#include "util/TaskScheduler.hpp"

/**
 * A "wait group" for fire-and-wait style jobs on top of TaskScheduler.
 *
 * Typical usage:
 *   TaskGroup g;
 *   g.submit(scheduler, []{ ... });
 *   g.submit(scheduler, []{ ... });
 *   g.wait();
 */
class TaskGroup
{
public:
    TaskGroup() = default;

    // Submit a task; if the scheduler is saturated, it runs inline.
    void submit(TaskScheduler& scheduler, std::function<void()> fn)
    {
        m_pending.fetch_add(1, std::memory_order_relaxed);

        const bool scheduled = scheduler.schedule([this, fn = std::move(fn)]() mutable {
            fn();
            this->completeOne();
        });

        if (!scheduled)
        {
            // Fall back to running in caller thread if the queue is full.
            fn();
            completeOne();
        }
    }

    void wait()
    {
        std::unique_lock lk(m_mtx);
        m_cv.wait(lk, [&] { return m_pending.load(std::memory_order_acquire) == 0; });
    }

private:
    void completeOne()
    {
        if (m_pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            std::scoped_lock lk(m_mtx);
            m_cv.notify_all();
        }
    }

private:
    std::atomic<int> m_pending{0};
    std::mutex m_mtx;
    std::condition_variable m_cv;
};

template <typename Fn>
inline void parallel_for(TaskScheduler& scheduler, std::size_t count, std::size_t grain, Fn&& fn)
{
    if (count == 0) return;
    if (grain == 0) grain = 1;

    TaskGroup g;
    for (std::size_t start = 0; start < count; start += grain)
    {
        const std::size_t end = std::min(count, start + grain);
        g.submit(scheduler, [start, end, &fn] {
            fn(start, end);
        });
    }
    g.wait();
}

