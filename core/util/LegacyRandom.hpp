#pragma once

#include <cstdint>
#include <random>

/**
 * @brief Seeded random streams of the legacy reference. Every generator is a std::mt19937 seeded
 * with the low 32 bits of (ApplicationContext::seed ^ stream); std::random_device is never used,
 * so a seed reproduces a session. The seed comes from config/config.json ("seed") or --seed.
*/
namespace RngStream
{
    constexpr std::uint64_t GameOfLife = 0x474F4C00;      // Initial Game of Life grid
    constexpr std::uint64_t StressGenerator = 0x53545200; // Editor stress generator; + the call index
}

inline std::mt19937 makeLegacyRng(std::uint64_t seed, std::uint64_t stream)
{
    return std::mt19937(static_cast<std::mt19937::result_type>((seed ^ stream) & 0xFFFFFFFFu));
}
