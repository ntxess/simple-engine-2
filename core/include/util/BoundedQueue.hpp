#pragma once

#include <condition_variable>
#include <mutex>
#include <vector>

template <typename T>
class BoundedQueue
{
public:
    explicit BoundedQueue(size_t capacity)
        : m_capacity{capacity}
        , m_buffer{capacity}
    {
        if (m_capacity == 0)
            throw std::invalid_argument("Capacity must be > 0");

        static_assert(std::is_move_constructible_v<T> || std::is_copy_constructible_v<T>, "T must be copy/move constructible");
    }

    bool push(const T& item)
    {
        return pushImpl(item);
    }

    bool push(T&& item)
    {
        return pushImpl(std::move(item));
    }

    bool tryPush(const T& item, std::chrono::milliseconds timeout)
    {
        return tryPushImpl(item, timeout);
    }

    bool tryPush(T&& item, std::chrono::milliseconds timeout)
    {
        return tryPushImpl(std::move(item), timeout);
    }

    bool pop(T& out)
    {
        std::unique_lock<std::mutex> lock(m_mtx);

        m_notEmpty.wait(lock, [this] { return m_size > 0 || m_closed; });
        if (m_size == 0 && m_closed) return false;

        out = std::move(m_buffer[m_head]);
        advanceHeadUnlocked();
        lock.unlock();
        m_notFull.notify_one();
        return true;
    }

    bool tryPop(T& out, std::chrono::milliseconds timeout)
    {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock<std::mutex> lock(m_mtx);

        bool ready = m_notEmpty.wait_until(lock, deadline, [this] { return m_size > 0 || m_closed; });
        if (m_size == 0 || !ready) return false;

        out = std::move(m_buffer[m_head]);
        advanceHeadUnlocked();
        lock.unlock();
        m_notFull.notify_one();
        return true;
    }

    void close()
    {
        std::scoped_lock<std::mutex> lock(m_mtx);
        m_closed = true;
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

    bool isClosed() const
    {
        std::scoped_lock<std::mutex> lock(m_mtx);
        return m_closed;
    }

    size_t size() const
    {
        std::scoped_lock<std::mutex> lock(m_mtx);
        return m_size;
    }
    size_t capacity() const noexcept
    {
        return m_capacity;
    }

private:
    template <typename U>
    bool pushImpl(U&& item)
    {
        std::unique_lock<std::mutex> lock(m_mtx);
        m_notFull.wait(lock, [this] { return m_size < m_capacity || m_closed; });
        if (m_closed) return false;

        m_buffer[m_tail] = std::forward<U>(item);
        advanceTailUnlocked();
        lock.unlock();
        m_notEmpty.notify_one();
        return true;
    }

    template <typename U>
    bool tryPushImpl(U&& item, std::chrono::milliseconds timeout)
    {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock<std::mutex> lock(m_mtx);
        bool ready = m_notFull.wait_until(lock, deadline, [this] { return m_size < m_capacity || m_closed; });
        if (!ready || m_closed) return false;

        m_buffer[m_tail] = std::forward<U>(item);
        advanceTailUnlocked();
        lock.unlock();
        m_notEmpty.notify_one();
        return true;
    }

    inline void advanceHeadUnlocked()
    {
        m_head = (m_head + 1) % m_capacity;
        --m_size;
    }

    inline void advanceTailUnlocked()
    {
        m_tail = (m_tail + 1) % m_capacity;
        ++m_size;
    }

private:
    const size_t m_capacity;
    std::vector<T> m_buffer;
    size_t m_head{ 0 };
    size_t m_tail{ 0 };
    size_t m_size{ 0 };
    bool m_closed{ false };

    mutable std::mutex m_mtx;
    std::condition_variable m_notFull;
    std::condition_variable m_notEmpty;
};