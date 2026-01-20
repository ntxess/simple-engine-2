#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/**
 * Lightweight per-frame CPU profiler + named counters.
 *
 * Design goals:
 * - Very cheap "hot path": timers/counters only touch thread-local data.
 * - Frame commit (endFrame) is the only place that locks and touches history.
 * - Histories are kept per-thread-name and per-scope/counter-name.
 */
class PerformanceTracker
{
public:
    explicit PerformanceTracker(std::size_t historyLen = 240);

    void setEnabled(bool enabled);
    bool isEnabled() const;

    // Frame boundaries (call once per loop iteration / tick).
    void beginFrame(std::string_view threadName);
    void endFrame();

    // Scoped CPU timer (RAII)
    class ScopedTimer
    {
    public:
        ScopedTimer(PerformanceTracker& tracker, std::string_view scopeName);
        ~ScopedTimer();

        ScopedTimer(const ScopedTimer&) = delete;
        ScopedTimer& operator=(const ScopedTimer&) = delete;
        ScopedTimer(ScopedTimer&&) = delete;
        ScopedTimer& operator=(ScopedTimer&&) = delete;

    private:
        PerformanceTracker* m_tracker;
        std::string_view m_scopeName;
        std::uint64_t m_startNs;
    };

    // Counters (accumulate within the active frame on the current thread).
    void counterAdd(std::string_view counterName, std::int64_t delta);
    void counterSet(std::string_view counterName, std::int64_t value);

    // Read-only accessors (thread-safe snapshots).
    std::vector<std::string> listThreads() const;
    std::vector<std::string> listScopes(std::string_view threadName) const;
    std::vector<std::string> listCounters(std::string_view threadName) const;

    std::optional<double> getLastScopeMs(std::string_view threadName, std::string_view scopeName) const;
    std::optional<std::int64_t> getLastCounter(std::string_view threadName, std::string_view counterName) const;

    std::deque<double> getScopeHistoryMs(std::string_view threadName, std::string_view scopeName) const;
    std::deque<std::int64_t> getCounterHistory(std::string_view threadName, std::string_view counterName) const;

private:
    friend class ScopedTimer;

    void addScopeDurationNs(std::string_view scopeName, std::uint64_t ns);

    struct ThreadStore
    {
        std::size_t frameCount = 0;
        std::unordered_map<std::string, std::deque<double>> scopeHistoryMs;
        std::unordered_map<std::string, std::deque<std::int64_t>> counterHistory;
    };

    std::uint64_t nowNs() const;

private:
    std::size_t m_historyLen;
    bool m_enabled;

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, ThreadStore> m_threads;
};

// Convenience macros (compile-time removable).
#if defined(SIMPLE_ENGINE_ENABLE_PROFILER) && SIMPLE_ENGINE_ENABLE_PROFILER
#define SE_FRAME_BEGIN(tracker, threadNameLiteral) (tracker).beginFrame((threadNameLiteral))
#define SE_FRAME_END(tracker) (tracker).endFrame()
#define SE_PROFILE_SCOPE(tracker, nameLiteral) ::PerformanceTracker::ScopedTimer _seTimer##__LINE__{ (tracker), (nameLiteral) }
#define SE_COUNTER_ADD(tracker, nameLiteral, delta) (tracker).counterAdd((nameLiteral), (delta))
#define SE_COUNTER_SET(tracker, nameLiteral, value) (tracker).counterSet((nameLiteral), (value))
#else
#define SE_FRAME_BEGIN(tracker, threadNameLiteral) ((void)0)
#define SE_FRAME_END(tracker) ((void)0)
#define SE_PROFILE_SCOPE(tracker, nameLiteral) ((void)0)
#define SE_COUNTER_ADD(tracker, nameLiteral, delta) ((void)0)
#define SE_COUNTER_SET(tracker, nameLiteral, value) ((void)0)
#endif


