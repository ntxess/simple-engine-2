#pragma once

#include <mutex>
#include <shared_mutex>

struct NoSync
{
    void lock() noexcept {}
    void unlock() noexcept {}
};

struct MutexSync
{
    mutable std::mutex mtx;
    void lock() noexcept { mtx.lock(); }
    void unlock() noexcept { mtx.unlock();}
};

struct SharedMutexSync
{
    mutable std::shared_mutex mtx;
    void lock() noexcept { mtx.lock(); }
    void unlock() noexcept { mtx.unlock(); }
};