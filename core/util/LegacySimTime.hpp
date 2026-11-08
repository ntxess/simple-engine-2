#pragma once

#include <cstdint>

/**
 * @brief Fixed-rate simulation clock of the legacy reference (docs/milestone-specs.md, M1a).
 * The main loop advances `tick` once per simulation step; nothing in the simulation reads a
 * wall clock. Tick 0 is the state after scene initialisation; the first step is tick 1.
*/
struct LegacySimTime
{
    std::uint64_t tick = 0;
    std::uint32_t rate = 120;
    double dt = 1.0 / 120.0;

    void setRate(std::uint32_t newRate)
    {
        rate = newRate;
        dt = 1.0 / static_cast<double>(newRate);
    }
};

/**
 * @brief Converts a duration in milliseconds to whole ticks, rounding up:
 * msToTicks(ms, rate) = ceil(ms * rate / 1000). Non-positive durations give 0.
*/
constexpr std::uint64_t msToTicks(std::int64_t ms, std::uint32_t rate)
{
    if (ms <= 0)
        return 0;
    const std::uint64_t scaled = static_cast<std::uint64_t>(ms) * rate;
    return (scaled + 999) / 1000;
}

static_assert(msToTicks(1000, 120) == 120);
static_assert(msToTicks(5000, 120) == 600);
static_assert(msToTicks(1, 120) == 1);
static_assert(msToTicks(0, 120) == 0);
