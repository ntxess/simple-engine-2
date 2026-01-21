#include "PerformanceTracker.hpp"

#include <algorithm>
#include <chrono>

namespace
{
    struct FrameAccum
    {
        std::string threadName;
        bool inFrame = false;
        std::unordered_map<std::string, std::uint64_t> scopeNs;
        std::unordered_map<std::string, std::int64_t> counters;
    };

    thread_local FrameAccum g_frame;
}

PerformanceTracker::PerformanceTracker(std::size_t historyLen)
    : m_historyLen{historyLen}
    , m_enabled{true}
{}

void PerformanceTracker::setEnabled(bool enabled)
{
    m_enabled = enabled;
}

bool PerformanceTracker::isEnabled() const
{
    return m_enabled;
}

std::uint64_t PerformanceTracker::nowNs() const
{
    using clock = std::chrono::steady_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count()
    );
}

void PerformanceTracker::beginFrame(std::string_view threadName)
{
    if (!m_enabled) return;

    g_frame.threadName = std::string(threadName);
    g_frame.inFrame = true;
    g_frame.scopeNs.clear();
    g_frame.counters.clear();
}

void PerformanceTracker::endFrame()
{
    if (!m_enabled) return;
    if (!g_frame.inFrame) return;

    const std::string threadName = g_frame.threadName;

    std::scoped_lock lk{m_mutex};
    ThreadStore& store = m_threads[threadName];

    // Ensure all known scopes/counters get a value every frame (0 for missing),
    // and newly discovered ones are backfilled for previous frames.
    for (const auto& [scopeName, ns] : g_frame.scopeNs)
    {
        auto& series = store.scopeHistoryMs[scopeName];
        if (series.empty() && store.frameCount > 0)
            series.assign(store.frameCount, 0.0);
    }

    for (const auto& [counterName, value] : g_frame.counters)
    {
        auto& series = store.counterHistory[counterName];
        if (series.empty() && store.frameCount > 0)
            series.assign(store.frameCount, 0);
    }

    for (auto& [scopeName, series] : store.scopeHistoryMs)
    {
        const std::uint64_t ns = g_frame.scopeNs.count(scopeName) ? g_frame.scopeNs.at(scopeName) : 0ULL;
        series.push_back(static_cast<double>(ns) * 1e-6); // ns -> ms
        if (series.size() > m_historyLen) series.pop_front();
    }

    for (auto& [counterName, series] : store.counterHistory)
    {
        const std::int64_t v = g_frame.counters.count(counterName) ? g_frame.counters.at(counterName) : 0;
        series.push_back(v);
        if (series.size() > m_historyLen) series.pop_front();
    }

    store.frameCount++;

    g_frame.inFrame = false;
    g_frame.threadName.clear();
    g_frame.scopeNs.clear();
    g_frame.counters.clear();
}

void PerformanceTracker::addScopeDurationNs(std::string_view scopeName, std::uint64_t ns)
{
    if (!m_enabled) return;
    if (!g_frame.inFrame) return;

    g_frame.scopeNs[std::string(scopeName)] += ns;
}

void PerformanceTracker::counterAdd(std::string_view counterName, std::int64_t delta)
{
    if (!m_enabled) return;
    if (!g_frame.inFrame) return;

    g_frame.counters[std::string(counterName)] += delta;
}

void PerformanceTracker::counterSet(std::string_view counterName, std::int64_t value)
{
    if (!m_enabled) return;
    if (!g_frame.inFrame) return;

    g_frame.counters[std::string(counterName)] = value;
}

PerformanceTracker::ScopedTimer::ScopedTimer(PerformanceTracker& tracker, std::string_view scopeName)
    : m_tracker{&tracker}
    , m_scopeName{scopeName}
    , m_startNs{tracker.nowNs()}
{}

PerformanceTracker::ScopedTimer::~ScopedTimer()
{
    if (!m_tracker) return;
    const std::uint64_t endNs = m_tracker->nowNs();
    m_tracker->addScopeDurationNs(m_scopeName, endNs - m_startNs);
}

std::vector<std::string> PerformanceTracker::listThreads() const
{
    std::scoped_lock lk{m_mutex};
    std::vector<std::string> out;
    out.reserve(m_threads.size());
    for (const auto& [name, _] : m_threads)
        out.push_back(name);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> PerformanceTracker::listScopes(std::string_view threadName) const
{
    std::scoped_lock lk{m_mutex};
    std::vector<std::string> out;
    const auto it = m_threads.find(std::string(threadName));
    if (it == m_threads.end()) return out;

    out.reserve(it->second.scopeHistoryMs.size());
    for (const auto& [name, _] : it->second.scopeHistoryMs)
        out.push_back(name);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> PerformanceTracker::listCounters(std::string_view threadName) const
{
    std::scoped_lock lk{m_mutex};
    std::vector<std::string> out;
    const auto it = m_threads.find(std::string(threadName));
    if (it == m_threads.end()) return out;

    out.reserve(it->second.counterHistory.size());
    for (const auto& [name, _] : it->second.counterHistory)
        out.push_back(name);
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<double> PerformanceTracker::getLastScopeMs(std::string_view threadName, std::string_view scopeName) const
{
    std::scoped_lock lk{m_mutex};
    const auto tIt = m_threads.find(std::string(threadName));
    if (tIt == m_threads.end()) return std::nullopt;

    const auto sIt = tIt->second.scopeHistoryMs.find(std::string(scopeName));
    if (sIt == tIt->second.scopeHistoryMs.end() || sIt->second.empty()) return std::nullopt;
    return sIt->second.back();
}

std::optional<std::int64_t> PerformanceTracker::getLastCounter(std::string_view threadName, std::string_view counterName) const
{
    std::scoped_lock lk{m_mutex};
    const auto tIt = m_threads.find(std::string(threadName));
    if (tIt == m_threads.end()) return std::nullopt;

    const auto cIt = tIt->second.counterHistory.find(std::string(counterName));
    if (cIt == tIt->second.counterHistory.end() || cIt->second.empty()) return std::nullopt;
    return cIt->second.back();
}

std::deque<double> PerformanceTracker::getScopeHistoryMs(std::string_view threadName, std::string_view scopeName) const
{
    std::scoped_lock lk{m_mutex};
    const auto tIt = m_threads.find(std::string(threadName));
    if (tIt == m_threads.end()) return {};

    const auto sIt = tIt->second.scopeHistoryMs.find(std::string(scopeName));
    if (sIt == tIt->second.scopeHistoryMs.end()) return {};
    return sIt->second;
}

std::deque<std::int64_t> PerformanceTracker::getCounterHistory(std::string_view threadName, std::string_view counterName) const
{
    std::scoped_lock lk{m_mutex};
    const auto tIt = m_threads.find(std::string(threadName));
    if (tIt == m_threads.end()) return {};

    const auto cIt = tIt->second.counterHistory.find(std::string(counterName));
    if (cIt == tIt->second.counterHistory.end()) return {};
    return cIt->second;
}


